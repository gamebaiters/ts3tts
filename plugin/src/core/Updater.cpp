#include "core/Updater.h"

#include "core/Controller.h"
#include "core/Defer.h"
#include "ui/UpdaterWindow.h"

#include "modules/theme.h"
#include "ts3log.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QPushButton>

namespace gbtts {

namespace {

constexpr qint64 kDay = 24 * 3600;
constexpr int    kFeedTimeoutMs = 20000;

QByteArray utf8(const QString &s) { return s.toUtf8(); }

} // namespace

bool updateDryRun()
{
    // Tests: download, verify and write the helper, but never start it nor close the client.
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_UPDATE_NO_LAUNCH")) == QLatin1String("1");
}

Updater::Updater(Controller *ctrl, QObject *parent) : QObject(parent), m_ctrl(ctrl)
{
    // No QNetworkAccessManager until a check really runs: in Qt 5.15 creating one starts the
    // bearer/network-configuration machinery, which is not free at every plugin load.
}

QNetworkAccessManager *Updater::net()
{
    if (!m_net) m_net = new QNetworkAccessManager(this);
    return m_net;
}

Updater::~Updater()
{
    if (m_reply) {
        m_reply->disconnect(this);   // abort() emits finished() synchronously
        m_reply->abort();
    }
    delete m_box.data();
    delete m_window.data();
}

QNetworkRequest Updater::request(const QUrl &url) const
{
    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", QByteArray("GameBaiters TTS Update Checker, ") + GBTTS_VERSION);
    r.setRawHeader("Cache-Control", "no-cache");
    // releases/latest/download/... answers with 302 to the asset storage.
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    r.setMaximumRedirectsAllowed(10);
    r.setTransferTimeout(kFeedTimeoutMs);
    return r;
}

void Updater::checkAutomatically()
{
    const Settings &s = m_ctrl->settings();
    if (!s.updateAutoCheck) {
        logInfo("GBTTS: automatic update check is off");
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now < s.updateNextCheck) {
        logInfo("GBTTS: next update check %s",
                utf8(QDateTime::fromSecsSinceEpoch(s.updateNextCheck).toString(Qt::ISODate)).constData());
        return;
    }
    startCheck(false);
}

void Updater::checkNow()
{
    if (m_window) {
        m_window->showNormal();
        m_window->raise();
        m_window->activateWindow();
        return;
    }
    if (m_box && m_box->isVisible()) {
        m_box->raise();
        m_box->activateWindow();
        return;
    }
    startCheck(true);
}

void Updater::startCheck(bool explicitCheck)
{
    if (m_reply) {   // already running: just make sure an explicit request gets an answer
        m_explicit = m_explicit || explicitCheck;
        return;
    }
    m_explicit = explicitCheck;
    m_info = update::Info();
    m_notes.clear();
    const QUrl url = update::feedUrl();
    logInfo("GBTTS: checking for updates (%s)", utf8(url.toString()).constData());
    QNetworkReply *reply = net()->get(request(url));
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { onFeedFinished(reply); });
}

void Updater::onFeedFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_reply = nullptr;
    if (reply->error() != QNetworkReply::NoError) {
        logWarning("GBTTS: update check failed: %s", utf8(reply->errorString()).constData());
        if (m_explicit)
            message(true, tr("Check for updates"), tr("Could not check for updates: %1").arg(reply->errorString()));
        return;
    }
    m_info = update::parseFeed(reply->readAll());
    if (!m_info.valid()) {
        logWarning("GBTTS: update information rejected: %s", utf8(m_info.error).constData());
        if (m_explicit)
            message(true, tr("Check for updates"), tr("The update information is not valid: %1").arg(m_info.error));
        return;
    }
    const int current = update::versionNumber(QStringLiteral(GBTTS_VERSION));
    if (m_info.build <= current) {
        logInfo("GBTTS: up to date (installed %s, latest %s)", GBTTS_VERSION, utf8(m_info.version).constData());
        postpone(kDay);
        if (m_explicit)
            message(false, tr("Check for updates"),
                    tr("GameBaiters TTS is up to date (version %1).").arg(QStringLiteral(GBTTS_VERSION)));
        return;
    }
    logInfo("GBTTS: version %s is available (installed %s)", utf8(m_info.version).constData(), GBTTS_VERSION);
    if (m_info.notesUrl.isEmpty()) {
        offer();
        return;
    }
    QNetworkReply *notes = net()->get(request(QUrl(m_info.notesUrl)));
    m_reply = notes;
    connect(notes, &QNetworkReply::finished, this, [this, notes] { onNotesFinished(notes); });
}

void Updater::onNotesFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    m_reply = nullptr;
    if (reply->error() == QNetworkReply::NoError)
        m_notes = update::notesSince(QString::fromUtf8(reply->readAll()), QStringLiteral(GBTTS_VERSION));
    else
        logWarning("GBTTS: release notes not available: %s", utf8(reply->errorString()).constData());
    offer();
}

void Updater::offer()
{
    delete m_box.data();
    auto *box = new QMessageBox();
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setProperty("isGBSoundboard", true);
    box->setStyleSheet(Theme::compositeStyleSheet());
    box->setIcon(QMessageBox::Information);
    box->setWindowTitle(tr("New version of GameBaiters TTS"));
    box->setText(tr("GameBaiters TTS %1 is available (you have %2).").arg(m_info.version, QStringLiteral(GBTTS_VERSION)));
    box->setInformativeText(tr("Download and install it now? TeamSpeak closes to install the update; your settings, "
                               "your voices and the voice engine are kept."));
    if (!m_notes.isEmpty()) box->setDetailedText(m_notes);
    QPushButton *now = box->addButton(tr("Update now"), QMessageBox::AcceptRole);
    QPushButton *later = box->addButton(tr("Later"), QMessageBox::RejectRole);
    box->setDefaultButton(now);
    box->setEscapeButton(later);
    connect(box, &QMessageBox::buttonClicked, this, [this, now](QAbstractButton *b) {
        if (b == now) {
            startDownload();
        } else {
            logInfo("GBTTS: update %s postponed by the user", utf8(m_info.version).constData());
            postpone(3 * kDay);
        }
    });
    m_box = box;
    box->show();
    box->raise();
    box->activateWindow();
}

void Updater::startDownload()
{
    m_ctrl->saveNow();
    delete m_window.data();
    auto *w = new UpdaterWindow(m_info);
    m_window = w;
    connect(w, &UpdaterWindow::downloadFinished, this, &Updater::onDownloadFinished);
    w->show();
    w->raise();
    w->start();
}

void Updater::onDownloadFinished(bool ok, const QString &error)
{
    if (ok) {
        if (updateDryRun()) {
            logInfo("GBTTS: update helper written, not launched (GBTTS_UPDATE_NO_LAUNCH=1)");
            return;
        }
        logInfo("GBTTS: update %s downloaded, helper started - asking TeamSpeak to close",
                utf8(m_info.version).constData());
        m_ctrl->saveNow();
        // The helper waits for a GRACEFUL exit; closing every window is how a plugin asks
        // TeamSpeak to quit. From a plugin-owned timer, never from inside this slot.
        deferCall(this, 1500, [] { QApplication::closeAllWindows(); });
        return;
    }
    if (m_window) {
        m_window->hide();
        m_window->deleteLater();
    }
    if (error.isEmpty()) {
        logInfo("GBTTS: update download cancelled");
        return;
    }
    logWarning("GBTTS: update failed: %s", utf8(error).constData());
    message(true, tr("Update failed"),
            tr("The update to %1 failed: %2\n\nYou can download it manually from:\n%3")
                .arg(m_info.version, error, QString::fromLatin1(update::kReleasesPage)));
}

void Updater::postpone(qint64 seconds)
{
    m_ctrl->settings().updateNextCheck = QDateTime::currentSecsSinceEpoch() + seconds;
    m_ctrl->saveNow();
}

void Updater::message(bool warning, const QString &title, const QString &text)
{
    delete m_box.data();
    auto *box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text, QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setProperty("isGBSoundboard", true);
    box->setStyleSheet(Theme::compositeStyleSheet());
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_box = box;
    box->show();
    box->raise();
    box->activateWindow();
}

} // namespace gbtts
