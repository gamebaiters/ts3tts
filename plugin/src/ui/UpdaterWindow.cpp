#include "ui/UpdaterWindow.h"

#include "ui/ProgressBar.h"
#include "ui/TtsIcons.h"

#include "modules/theme.h"
#include "ts3log.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

extern "C" const char *getTs3ConfigPath();

namespace gbtts {

bool updateDryRun();   // core/Updater.cpp

namespace {

QString megabytes(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

} // namespace

UpdaterWindow::UpdaterWindow(const update::Info &info, QWidget *parent) : QDialog(parent), m_info(info)
{
    setWindowTitle(tr("GameBaiters TTS update"));
    setWindowIcon(TtsIcons::app());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setProperty("isGBSoundboard", true);
    setMinimumWidth(480);

    auto *title = new QLabel(tr("Updating GameBaiters TTS to %1").arg(m_info.version), this);
    QFont f = title->font();
    f.setPointSize(f.pointSize() + 3);
    f.setBold(true);
    title->setFont(f);
    m_status = new QLabel(tr("Preparing…"), this);
    m_bar = new ProgressBar(this);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(120);
    m_cancel = new QPushButton(tr("Cancel"), this);
    connect(m_cancel, &QPushButton::clicked, this, &UpdaterWindow::reject);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    buttons->addWidget(m_cancel);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 14, 16, 12);
    lay->setSpacing(10);
    lay->addWidget(title);
    lay->addWidget(m_status);
    lay->addWidget(m_bar);
    lay->addWidget(m_log);
    lay->addLayout(buttons);
    setStyleSheet(Theme::compositeStyleSheet());

    m_net = new QNetworkAccessManager(this);
}

UpdaterWindow::~UpdaterWindow()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_file.isOpen()) {
        m_file.close();
        m_file.remove();
    }
}

void UpdaterWindow::addLog(const QString &line)
{
    m_log->appendPlainText(line);
    logInfo("GBTTS: [updater] %s", line.toUtf8().constData());
}

void UpdaterWindow::start()
{
    const QUrl url(m_info.url);
    QString name = QFileInfo(url.path()).fileName();
    if (!name.endsWith(QLatin1String(".ts3_plugin"), Qt::CaseInsensitive))
        name = QStringLiteral("gb_tts_%1_win64.ts3_plugin").arg(m_info.version);
    m_dir = QDir::temp().filePath(QStringLiteral("GameBaitersTTS-update"));
    QDir().mkpath(m_dir);
    m_target = QDir(m_dir).filePath(name);

    addLog(tr("Installed %1, installing %2").arg(QStringLiteral(GBTTS_VERSION), m_info.version));
    addLog(tr("Downloading %1 from %2").arg(name, url.host().isEmpty() ? url.scheme() : url.host()));
    m_file.setFileName(m_target + QStringLiteral(".part"));
    m_file.remove();
    if (!m_file.open(QIODevice::WriteOnly)) {
        finish(false, tr("cannot write %1").arg(QDir::toNativeSeparators(m_file.fileName())));
        return;
    }

    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", QByteArray("GameBaiters TTS Updater, ") + GBTTS_VERSION);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setMaximumRedirectsAllowed(10);
    r.setTransferTimeout(60000);   // no byte for a minute = stalled
    m_status->setText(tr("Connecting…"));
    m_speedTimer.start();
    QNetworkReply *reply = m_net->get(r);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_file.isOpen()) m_file.write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &UpdaterWindow::onProgress);
    connect(reply, &QNetworkReply::finished, this, &UpdaterWindow::onReplyFinished);
}

void UpdaterWindow::onProgress(qint64 bytes, qint64 total)
{
    if (m_cancelled) return;
    const qint64 ms = m_speedTimer.elapsed();
    if (ms >= 500) {
        m_bytesPerSecond = double(bytes - m_lastBytes) * 1000.0 / double(ms);
        m_lastBytes = bytes;
        m_speedTimer.restart();
    }
    const QString speed = m_bytesPerSecond > 1024.0 ? QStringLiteral(" — %1/s").arg(megabytes(qint64(m_bytesPerSecond)))
                                                    : QString();
    if (total > 0) {
        m_bar->setValue(double(bytes) / double(total));
        m_status->setText(tr("Downloading… %1% (%2 of %3)").arg(int(100.0 * bytes / total)).arg(megabytes(bytes), megabytes(total)) + speed);
    } else {
        m_status->setText(tr("Downloading… %1").arg(megabytes(bytes)) + speed);
    }
}

void UpdaterWindow::onReplyFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply) return;
    reply->deleteLater();
    if (m_file.isOpen()) {
        m_file.write(reply->readAll());
        m_file.close();
    }
    const QString part = m_file.fileName();
    if (m_cancelled) {
        QFile::remove(part);
        addLog(tr("Download cancelled."));
        finish(false, QString());
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(part);
        finish(false, reply->errorString());
        return;
    }
    const qint64 size = QFileInfo(part).size();
    addLog(tr("Downloaded %1.").arg(megabytes(size)));
    if (size < 1024 || !update::looksLikeZip(part)) {
        QFile::remove(part);
        finish(false, tr("the downloaded file is not a TeamSpeak plugin package"));
        return;
    }
    if (!m_info.sha256.isEmpty()) {
        const QString sha = update::fileSha256(part);
        if (sha != m_info.sha256) {
            QFile::remove(part);
            finish(false, tr("checksum mismatch: the download is damaged (SHA-256 %1)").arg(sha.left(16)));
            return;
        }
        addLog(tr("Checksum verified (SHA-256)."));
    } else {
        addLog(tr("No checksum in the update information: only the archive format was verified."));
    }
    QFile::remove(m_target);
    if (!QFile::rename(part, m_target)) {
        finish(false, tr("cannot write %1").arg(QDir::toNativeSeparators(m_target)));
        return;
    }

    update::HelperSpec spec;
    const char *cfg = getTs3ConfigPath();
    spec.pluginsDir = QString::fromUtf8(cfg ? cfg : "") + QStringLiteral("plugins");
    spec.packagePath = m_target;
    const QString helperPath = QDir(m_dir).filePath(QStringLiteral("gbtts_update_helper.bat"));
    QFile helper(helperPath);
    if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate) || helper.write(update::helperScript(spec)) <= 0) {
        finish(false, tr("cannot write the update helper %1").arg(QDir::toNativeSeparators(helperPath)));
        return;
    }
    helper.close();
    addLog(tr("Update helper: %1").arg(QDir::toNativeSeparators(helperPath)));

    if (updateDryRun()) {
        m_status->setText(tr("Ready (test run: the helper is not started)."));
        finish(true, QString());
        return;
    }
    if (!QProcess::startDetached(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QDir::toNativeSeparators(helperPath)})) {
        finish(false, tr("could not start the update helper"));
        return;
    }
    m_status->setText(tr("Installing: TeamSpeak closes now, confirm the plugin installer, then start TeamSpeak again."));
    addLog(tr("Update helper started. TeamSpeak closes, the new version is installed, then start TeamSpeak again."));
    finish(true, QString());
}

void UpdaterWindow::finish(bool ok, const QString &error)
{
    if (m_finished) return;
    m_finished = true;
    m_bar->setFill(ok ? QColor(0x57, 0xC4, 0x5E) : QColor(0xD9, 0x53, 0x4F));
    m_bar->snapTo(1.0);
    m_cancel->setEnabled(false);
    if (!ok && !error.isEmpty()) {
        m_status->setText(tr("Update failed."));
        addLog(tr("ERROR: %1").arg(error));
    }
    emit downloadFinished(ok, error);
}

void UpdaterWindow::reject()
{
    if (m_finished) {
        QDialog::reject();
        return;
    }
    if (m_reply) {
        m_cancelled = true;
        m_status->setText(tr("Cancelling…"));
        m_reply->abort();   // onReplyFinished reports the cancellation
    } else {
        finish(false, QString());
    }
}

} // namespace gbtts
