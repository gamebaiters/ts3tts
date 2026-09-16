#include "ui/EngineSetupDialog.h"

#include "core/Controller.h"
#include "gbtts.h"
#include "net/BackendProcess.h"
#include "ui/ProgressBar.h"
#include "ui/TtsIcons.h"

#include "modules/theme.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace gbtts {

namespace {

enum Page { ChoosePage, ProgressPage, ResultPage };

constexpr qint64 kGb = 1024LL * 1024 * 1024;
// Measured on clean installs (vault build-release/engine-installer.md), with a margin.
constexpr qint64 kNeedGpu = 12 * kGb;
constexpr qint64 kNeedCpu = 5 * kGb;

const QColor kGreen(0x57, 0xC4, 0x5E);
const QColor kRed(0xD9, 0x53, 0x4F);
const QColor kAmber(0xE6, 0xA8, 0x3C);

QString sizeText(qint64 bytes)
{
    if (bytes >= kGb) return QObject::tr("%1 GB").arg(bytes / double(kGb), 0, 'f', 1);
    return QObject::tr("%1 MB").arg(bytes / 1048576.0, 0, 'f', 0);
}

QLabel *muted(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setEnabled(false);
    return l;
}

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() + 4);
    f.setBold(true);
    l->setFont(f);
    l->setWordWrap(true);
    return l;
}

QString errorHint(const QString &code)
{
    if (code == QLatin1String("disk_space")) return EngineSetupDialog::tr("Free some space on the disk or choose another folder, then press Retry.");
    if (code == QLatin1String("network") || code == QLatin1String("checksum") || code == QLatin1String("models"))
        return EngineSetupDialog::tr("Check the internet connection and press Retry: what is already downloaded is kept.");
    if (code == QLatin1String("gpu_driver"))
        return EngineSetupDialog::tr("Update the NVIDIA driver (nvidia.com or the NVIDIA app), or install the light engine that runs on the processor.");
    if (code == QLatin1String("no_gpu")) return EngineSetupDialog::tr("No NVIDIA graphics card was found: install the light engine.");
    if (code == QLatin1String("busy")) return EngineSetupDialog::tr("Another installation is running: wait for it to finish.");
    if (code == QLatin1String("sources")) return EngineSetupDialog::tr("The plugin package is incomplete: install the plugin again.");
    if (code == QLatin1String("interrupted"))
        return EngineSetupDialog::tr("The installation was stopped from outside (antivirus, Task Manager, shutdown). Press Retry: it resumes.");
    if (code == QLatin1String("cancelled")) return EngineSetupDialog::tr("Press Retry whenever you want: it resumes where it stopped.");
    return EngineSetupDialog::tr("Press Retry: the installation resumes where it stopped. If it fails again, open the log.");
}

} // namespace

EngineSetupDialog::EngineSetupDialog(Controller *ctrl, QWidget *parent) : QDialog(parent), m_ctrl(ctrl)
{
    setWindowTitle(tr("GameBaiters TTS - voice engine installation"));
    setWindowIcon(TtsIcons::app());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setProperty("isGBSoundboard", true);
    setModal(false);
    m_gpu = EngineInstaller::detectNvidiaGpu();
    m_speedClock.start();

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("gbttsSetupPages"));   // object names: tests/host_harness.cpp --setup-*
    m_pages->addWidget(buildChoosePage());
    m_pages->addWidget(buildProgressPage());
    m_pages->addWidget(buildResultPage());
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 16, 18, 14);
    lay->addWidget(m_pages);
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);
    resize(620, 560);

    EngineInstaller *inst = m_ctrl->installer();
    connect(inst, &EngineInstaller::progress, this, &EngineSetupDialog::refreshProgress);
    connect(inst, &EngineInstaller::logAppended, this, [this](const QString &text) {
        const bool atEnd = m_log->verticalScrollBar()->value() >= m_log->verticalScrollBar()->maximum() - 4;
        QString chunk = text;
        if (chunk.endsWith(QLatin1Char('\n'))) chunk.chop(1);
        if (chunk.endsWith(QLatin1Char('\r'))) chunk.chop(1);
        m_log->appendPlainText(chunk);   // maximumBlockCount keeps it bounded
        if (atEnd) m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    });
    connect(inst, &EngineInstaller::finished, this, [this](bool ok) {
        showResult(ok);
        // Hidden window: success speaks for itself (the engine starts, the TTS window says so);
        // a failure must not go unnoticed.
        if (!ok && !isVisible()) {
            show();
            raise();
            activateWindow();
        }
    });
    m_clock.setInterval(1000);
    connect(&m_clock, &QTimer::timeout, this, &EngineSetupDialog::refreshProgress);
    m_cancelArm.setSingleShot(true);
    m_cancelArm.setInterval(4000);
    connect(&m_cancelArm, &QTimer::timeout, this, [this] { m_cancel->setText(tr("Cancel")); });
    present();
}

void EngineSetupDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    if (m_pages->currentIndex() == ChoosePage) refreshChoose();
}

void EngineSetupDialog::present()
{
    if (m_ctrl->installer()->running()) {
        m_log->setPlainText(m_ctrl->installer()->logTail());
        m_pages->setCurrentIndex(ProgressPage);
        m_clock.start();
        refreshProgress();
    } else if (m_pages->currentIndex() != ResultPage) {
        m_pages->setCurrentIndex(ChoosePage);
        refreshChoose();
    }
}

// ---------------------------------------------------------------------------
QWidget *EngineSetupDialog::buildChoosePage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    v->addWidget(heading(tr("Install the voice engine"), page));
    auto *intro = new QLabel(tr("GameBaiters TTS speaks with a neural voice that runs on this PC. It is installed once and "
                                "everything is automatic: Python, the libraries and the voice models are downloaded into "
                                "one folder, nothing else on the PC is changed."), page);
    intro->setWordWrap(true);
    v->addWidget(intro);

    m_gpuLabel = new QLabel(page);
    m_gpuLabel->setWordWrap(true);
    v->addWidget(m_gpuLabel);

    auto *group = new QButtonGroup(page);
    m_full = new QRadioButton(tr("Complete — Qwen3-TTS on the NVIDIA graphics card, plus Kokoro"), page);
    m_fullDesc = muted(tr("The most natural voice, ten Italian voices ready to use, speaking styles, voice design and "
                          "cloning. About 8 GB to download, about 10 GB on disk."), page);
    m_light = new QRadioButton(tr("Light — Kokoro on the processor"), page);
    auto *lightDesc = muted(tr("Good Italian voices and no video memory used: the graphics card stays free for games. "
                               "About 1 GB to download, about 1.5 GB on disk. The complete engine can be added later."), page);
    m_full->setObjectName(QStringLiteral("gbttsSetupFull"));
    m_light->setObjectName(QStringLiteral("gbttsSetupLight"));
    for (QRadioButton *r : {m_full, m_light}) {
        QFont f = r->font();
        f.setBold(true);
        r->setFont(f);
        group->addButton(r);
    }
    lightDesc->setContentsMargins(22, 0, 0, 0);
    m_fullDesc->setContentsMargins(22, 0, 0, 0);
    v->addWidget(m_full);
    v->addWidget(m_fullDesc);
    v->addWidget(m_light);
    v->addWidget(lightDesc);

    v->addSpacing(4);
    v->addWidget(new QLabel(tr("Folder"), page));
    auto *folderRow = new QHBoxLayout();
    m_folder = new QLineEdit(page);
    m_folder->setObjectName(QStringLiteral("gbttsSetupFolder"));
    auto *browse = new QPushButton(tr("Change…"), page);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    v->addLayout(folderRow);
    m_space = new QLabel(page);
    v->addWidget(m_space);
    m_existing = muted(QString(), page);
    v->addWidget(m_existing);
    v->addWidget(muted(tr("The installation continues in the background if you close this window or TeamSpeak; "
                          "the TTS window shows how far it is."), page));
    m_startError = new QLabel(page);
    m_startError->setWordWrap(true);
    m_startError->setStyleSheet(QStringLiteral("color:%1;").arg(kRed.name()));
    m_startError->hide();
    v->addWidget(m_startError);
    v->addStretch(1);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto *later = new QPushButton(tr("Later"), page);
    m_install = new QPushButton(tr("Install"), page);
    m_install->setObjectName(QStringLiteral("gbttsSetupInstall"));
    m_install->setDefault(true);
    buttons->addWidget(later);
    buttons->addWidget(m_install);
    v->addLayout(buttons);

    const QString configured = m_ctrl->settings().backendHome.trimmed();
    m_folder->setText(configured.isEmpty() ? BackendProcess::defaultHome() : configured);
    (m_gpu.name.isEmpty() ? m_light : m_full)->setChecked(true);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Folder of the voice engine"), m_folder->text());
        if (d.isEmpty()) return;
        // An empty folder or an existing engine folder is used as is; otherwise a subfolder.
        QDir dir(d);
        const bool usable = dir.isEmpty() || QFileInfo::exists(dir.filePath(QStringLiteral("install"))) ||
                            QFileInfo::exists(dir.filePath(QStringLiteral("venv")));
        m_folder->setText(QDir::toNativeSeparators(usable ? d : dir.filePath(QStringLiteral("GameBaitersTTS"))));
        refreshChoose();
    });
    connect(m_folder, &QLineEdit::textChanged, this, &EngineSetupDialog::refreshChoose);
    connect(group, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked), this, [this] { refreshChoose(); });
    connect(later, &QPushButton::clicked, this, &QDialog::close);
    connect(m_install, &QPushButton::clicked, this, &EngineSetupDialog::startInstall);
    return page;
}

QString EngineSetupDialog::selectedHome() const
{
    return QDir::toNativeSeparators(QDir::cleanPath(m_folder->text().trimmed()));
}

QString EngineSetupDialog::selectedMode() const
{
    return m_full->isChecked() ? QStringLiteral("gpu") : QStringLiteral("cpu");
}

qint64 EngineSetupDialog::requiredBytes() const
{
    return m_full->isChecked() ? kNeedGpu : kNeedCpu;
}

void EngineSetupDialog::refreshChoose()
{
    if (m_gpu.name.isEmpty()) {
        m_gpuLabel->setText(tr("No NVIDIA graphics card found: the light engine is the one for this PC."));
        m_full->setEnabled(false);
        m_fullDesc->setEnabled(false);
        m_light->setChecked(true);
    } else {
        m_gpuLabel->setText(tr("Graphics card: %1 (%2 of video memory).").arg(m_gpu.name, sizeText(m_gpu.memoryBytes)));
        m_full->setEnabled(true);
    }

    const QString home = selectedHome();
    const bool pathOk = !home.isEmpty() && QDir::isAbsolutePath(home);
    const qint64 free = pathOk ? EngineInstaller::freeBytes(home) : -1;
    const bool enough = free < 0 || free >= requiredBytes();
    if (!pathOk) {
        m_space->setText(tr("Choose a folder."));
        m_space->setStyleSheet(QStringLiteral("color:%1;").arg(kRed.name()));
    } else if (free >= 0) {
        m_space->setText(enough ? tr("Free space: %1 (needed about %2).").arg(sizeText(free), sizeText(requiredBytes()))
                                : tr("Not enough space: %1 free, about %2 needed.").arg(sizeText(free), sizeText(requiredBytes())));
        m_space->setStyleSheet(enough ? QString() : QStringLiteral("color:%1;").arg(kRed.name()));
    } else {
        m_space->clear();
    }

    const BackendProcess::Layout layout = BackendProcess::inspect(home);
    const bool existing = pathOk && QFileInfo::exists(QDir(home).filePath(QStringLiteral("venv")));
    if (existing && !layout.valid) {
        m_existing->setText(tr("An incomplete or broken installation is in this folder (%1): it will be repaired, the "
                               "models already downloaded are kept.").arg(layout.problem));
        m_install->setText(tr("Repair"));
    } else if (existing) {
        m_existing->setText(tr("The engine is already installed here: installing again updates and repairs it."));
        m_install->setText(tr("Repair"));
    } else {
        m_existing->clear();
        m_install->setText(tr("Install"));
    }
    m_existing->setVisible(!m_existing->text().isEmpty());
    m_install->setEnabled(pathOk && enough);
}

void EngineSetupDialog::startInstall()
{
    m_startError->hide();
    QString error;
    if (!m_ctrl->installEngine(selectedHome(), selectedMode(), &error)) {
        m_startError->setText(tr("The installation could not start: %1").arg(error));
        m_startError->show();
        return;
    }
    m_log->clear();
    m_bar->snapTo(0.0);
    m_lastBytes = -1;
    m_bytesPerSecond = 0.0;
    present();
}

// ---------------------------------------------------------------------------
QWidget *EngineSetupDialog::buildProgressPage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    v->addWidget(heading(tr("Installing the voice engine"), page));

    auto *barRow = new QHBoxLayout();
    m_bar = new ProgressBar(page);
    m_bar->setMonotonic(true);
    m_percent = new QLabel(page);
    m_percent->setObjectName(QStringLiteral("gbttsSetupPercent"));
    m_percent->setMinimumWidth(m_percent->fontMetrics().horizontalAdvance(QStringLiteral("100 %")) + 8);
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont bold = m_percent->font();
    bold.setBold(true);
    m_percent->setFont(bold);
    barRow->addWidget(m_bar, 1);
    barRow->addWidget(m_percent);
    v->addLayout(barRow);

    m_step = new QLabel(page);
    m_step->setFont(bold);
    m_step->setWordWrap(true);
    v->addWidget(m_step);
    m_detail = new QLabel(page);
    m_detail->setWordWrap(true);
    v->addWidget(m_detail);

    m_steps = new QListWidget(page);
    m_steps->setSelectionMode(QAbstractItemView::NoSelection);
    m_steps->setFocusPolicy(Qt::NoFocus);
    m_steps->setFrameShape(QFrame::NoFrame);
    m_steps->setMinimumHeight(200);
    v->addWidget(m_steps, 1);

    auto *infoRow = new QHBoxLayout();
    m_elapsed = muted(QString(), page);
    m_logToggle = new QToolButton(page);
    m_logToggle->setText(tr("Show details"));
    m_logToggle->setCheckable(true);
    infoRow->addWidget(m_elapsed, 1);
    infoRow->addWidget(m_logToggle);
    v->addLayout(infoRow);

    m_log = new QPlainTextEdit(page);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setPointSizeF(m_log->font().pointSizeF() - 0.5);
    m_log->setFont(mono);
    m_log->setMinimumHeight(140);
    m_log->hide();
    v->addWidget(m_log, 1);

    auto *buttons = new QHBoxLayout();
    buttons->addWidget(muted(tr("You can close this window: the installation continues."), page), 1);
    auto *hide = new QPushButton(tr("Hide"), page);
    m_cancel = new QPushButton(tr("Cancel"), page);
    m_cancel->setObjectName(QStringLiteral("gbttsSetupCancel"));
    buttons->addWidget(hide);
    buttons->addWidget(m_cancel);
    v->addLayout(buttons);

    connect(m_logToggle, &QToolButton::toggled, this, [this](bool on) {
        m_log->setVisible(on);
        m_logToggle->setText(on ? tr("Hide details") : tr("Show details"));
        if (on) m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    });
    connect(hide, &QPushButton::clicked, this, &QDialog::close);
    // Two clicks, no modal question box: a nested event loop inside a plugin is a crash
    // waiting for TeamSpeak to unload us while it runs.
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (!m_cancelArm.isActive()) {
            m_cancel->setText(tr("Click again to cancel"));
            m_cancelArm.start();
            return;
        }
        m_cancelArm.stop();
        m_cancel->setEnabled(false);
        m_ctrl->installer()->cancel();
    });
    return page;
}

void EngineSetupDialog::refreshProgress()
{
    EngineInstaller *inst = m_ctrl->installer();
    if (!inst->running()) {
        m_clock.stop();
        return;
    }
    if (m_pages->currentIndex() != ProgressPage) {
        m_pages->setCurrentIndex(ProgressPage);
        m_clock.start();
    }
    const EngineInstaller::Status &s = inst->status();
    m_bar->setValue(s.overall);
    m_bar->setBusy(true);
    m_percent->setText(QStringLiteral("%1 %").arg(int(m_bar->value() * 100)));   // same monotonic value as the bar

    const int count = s.stages.size();
    const QString stage = s.stage.isEmpty() ? QStringLiteral("check") : s.stage;
    if (inst->cancelRequested()) {
        m_step->setText(tr("Cancelling…"));
    } else if (count > 0) {
        m_step->setText(tr("Step %1 of %2 — %3").arg(s.stageIndex + 1).arg(count).arg(Controller::installStageLabel(stage, s.mode)));
    } else {
        m_step->setText(tr("Starting…"));
    }
    m_cancel->setEnabled(!inst->cancelRequested());

    // Speed from the byte counter of the current step (reset when the item changes).
    const qint64 now = m_speedClock.elapsed();
    if (s.item != m_lastItem || s.bytesDone < m_lastBytes) {
        m_lastItem = s.item;
        m_lastBytes = s.bytesDone;
        m_lastBytesMs = now;
        m_bytesPerSecond = 0.0;
    } else if (now - m_lastBytesMs >= 1500 && s.bytesDone > m_lastBytes) {
        const double inst0 = double(s.bytesDone - m_lastBytes) * 1000.0 / double(now - m_lastBytesMs);
        m_bytesPerSecond = m_bytesPerSecond > 0 ? m_bytesPerSecond * 0.6 + inst0 * 0.4 : inst0;
        m_lastBytes = s.bytesDone;
        m_lastBytesMs = now;
    } else if (now - m_lastBytesMs > 8000) {
        m_bytesPerSecond = 0.0;
    }
    QStringList detail;
    if (s.bytesTotal > 0) detail << tr("%1 of %2").arg(sizeText(s.bytesDone), sizeText(s.bytesTotal));
    else if (s.bytesDone > 0) detail << sizeText(s.bytesDone);
    if (m_bytesPerSecond > 256 * 1024) detail << tr("%1/s").arg(sizeText(qint64(m_bytesPerSecond)));
    if (stage == QLatin1String("verify")) detail << tr("checking that the libraries load (up to a minute)");
    m_detail->setText(detail.join(QStringLiteral(" · ")));

    if (m_steps->count() != count) {
        m_steps->clear();
        for (int i = 0; i < count; ++i) m_steps->addItem(QString());
    }
    const Theme::Derived &d = Theme::derivedCached();
    for (int i = 0; i < count; ++i) {
        QListWidgetItem *it = m_steps->item(i);
        const QString label = Controller::installStageLabel(s.stages.at(i), s.mode);
        if (i < s.stageIndex || s.state == QLatin1String("done")) {
            it->setText(QStringLiteral("✓  ") + label);
            it->setForeground(kGreen);
            it->setFont(m_steps->font());
        } else if (i == s.stageIndex) {
            it->setText(QStringLiteral("›  ") + label);
            it->setForeground(d.text);
            QFont f = m_steps->font();
            f.setBold(true);
            it->setFont(f);
        } else {
            it->setText(QStringLiteral("•  ") + label);
            QColor c = d.text;
            c.setAlpha(120);
            it->setForeground(c);
            it->setFont(m_steps->font());
        }
    }

    if (s.started.isValid()) {
        const qint64 secs = qMax<qint64>(0, s.started.secsTo(QDateTime::currentDateTime()));
        m_elapsed->setText(tr("Elapsed %1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')));
    }
}

// ---------------------------------------------------------------------------
QWidget *EngineSetupDialog::buildResultPage()
{
    auto *page = new QWidget(this);
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    m_resultTitle = heading(QString(), page);
    m_resultTitle->setObjectName(QStringLiteral("gbttsSetupResult"));
    m_resultText = new QLabel(page);
    m_resultText->setWordWrap(true);
    m_resultText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_resultTitle);
    v->addWidget(m_resultText);
    v->addStretch(1);
    auto *buttons = new QHBoxLayout();
    m_openLog = new QPushButton(tr("Open log"), page);
    buttons->addWidget(m_openLog);
    buttons->addStretch(1);
    m_retry = new QPushButton(tr("Retry"), page);
    m_retry->setObjectName(QStringLiteral("gbttsSetupRetry"));
    m_openTts = new QPushButton(tr("Open GameBaiters TTS"), page);
    auto *closeButton = new QPushButton(tr("Close"), page);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_openTts);
    buttons->addWidget(closeButton);
    v->addLayout(buttons);

    connect(m_openLog, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_ctrl->installer()->logFile()));
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        m_pages->setCurrentIndex(ChoosePage);
        refreshChoose();
    });
    connect(m_openTts, &QPushButton::clicked, this, [this] {
        close();
        openWindow(true);
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    return page;
}

void EngineSetupDialog::showResult(bool ok)
{
    m_clock.stop();
    m_cancelArm.stop();
    m_cancel->setText(tr("Cancel"));
    const EngineInstaller::Status &s = m_ctrl->installer()->status();
    m_bar->setBusy(false);
    if (ok) {
        m_resultTitle->setText(tr("The voice engine is installed"));
        m_resultTitle->setStyleSheet(QStringLiteral("color:%1;").arg(kGreen.name()));
        QString text = tr("Installed in %1 (%2 on disk).").arg(QDir::toNativeSeparators(m_ctrl->installer()->home()), sizeText(s.sizeBytes));
        text += QStringLiteral("\n\n");
        text += s.mode == QLatin1String("gpu")
                    ? tr("The engine is starting now: the first start prepares the graphics card for about a minute, "
                         "then the Italian voices are ready.")
                    : tr("The engine is starting now with the Kokoro voices: it is ready in a few seconds.");
        m_resultText->setText(text);
    } else {
        const bool cancelled = s.errorCode == QLatin1String("cancelled");
        m_resultTitle->setText(cancelled ? tr("Installation cancelled") : tr("The installation did not finish"));
        m_resultTitle->setStyleSheet(QStringLiteral("color:%1;").arg((cancelled ? kAmber : kRed).name()));
        QString text;
        if (!cancelled) {
            const QString step = Controller::installStageLabel(s.stage, s.mode);
            text = tr("Step “%1”: %2").arg(step, s.error) + QStringLiteral("\n\n");
        }
        text += errorHint(s.errorCode);
        m_resultText->setText(text);
    }
    m_retry->setVisible(!ok);
    m_openLog->setVisible(!ok);
    m_openTts->setVisible(ok);
    m_pages->setCurrentIndex(ResultPage);
}

} // namespace gbtts
