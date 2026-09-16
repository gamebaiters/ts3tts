#include "ui/TtsWindow.h"

#include "audio/TtsAudio.h"
#include "core/Controller.h"
#include "core/Defer.h"
#include "gbtts.h"
#include "ui/InputBox.h"
#include "ui/TtsIcons.h"
#include "ui/VoiceGroups.h"

#include "modules/channel_meter.h"
#include "modules/channel_sandbox_dialog.h"
#include "modules/fine_slider.h"
#include "modules/icon_factory.h"
#include "modules/section_box.h"
#include "modules/theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace gbtts {

namespace {

const QColor kGreen(0x57, 0xC4, 0x5E);
const QColor kAmber(0xE6, 0xA8, 0x3C);
const QColor kRed(0xD9, 0x53, 0x4F);
const QColor kGrey(0x8A, 0x8F, 0x98);

QString dbText(int db, int muteBelow)
{
    if (db <= muteBelow) return QObject::tr("off");
    return QStringLiteral("%1%2 dB").arg(db > 0 ? QStringLiteral("+") : QString()).arg(db);
}

QLabel *caption(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setMinimumWidth(l->fontMetrics().horizontalAdvance(text) + 6);
    return l;
}

QLabel *valueLabel(QWidget *parent, const QString &widest)
{
    auto *l = new QLabel(parent);
    l->setMinimumWidth(l->fontMetrics().horizontalAdvance(widest) + 8);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return l;
}

} // namespace

// ===========================================================================
StatusDot::StatusDot(QWidget *parent) : QWidget(parent)
{
    setFixedSize(14, 14);
}

void StatusDot::setColor(const QColor &c)
{
    if (c == m_color) return;
    m_color = c;
    update();
}

void StatusDot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor halo = m_color;
    halo.setAlpha(70);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(QRectF(0.5, 0.5, 13, 13));
    p.setBrush(m_color);
    p.drawEllipse(QRectF(3, 3, 8, 8));
}

// ===========================================================================
TtsWindow::TtsWindow(Controller *ctrl, QWidget *parent) : QWidget(parent), m_ctrl(ctrl)
{
    setWindowTitle(tr("GameBaiters - TTS"));
    setWindowIcon(TtsIcons::app());
    setProperty("isGBSoundboard", true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    buildUi();
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);

    m_syncing = true;   // restoring geometry must not schedule a save
    const QByteArray geo = m_ctrl->settings().windowGeometry;
    if (geo.isEmpty() || !restoreGeometry(geo)) resize(640, 660);
    m_syncing = false;
    applyWindowFlags();

    connect(m_ctrl, &Controller::stateChanged, this, &TtsWindow::refreshState);
    connect(m_ctrl, &Controller::engineChanged, this, &TtsWindow::refreshEngine);
    connect(m_ctrl, &Controller::voicesChanged, this, &TtsWindow::refreshVoices);
    connect(m_ctrl, &Controller::jobsChanged, this, &TtsWindow::refreshJobs);
    connect(m_ctrl, &Controller::vcChanged, this, &TtsWindow::refreshVc);
    connect(m_ctrl, &Controller::vcTargetsChanged, this, &TtsWindow::refreshVc);
    connect(m_ctrl, &Controller::vcTargetAdded, this, [this](const QString &, bool ok, const QString &msg) {
        showNotice(ok ? tr("Voice “%1” added: the voice changer now uses it.").arg(msg) : msg, ok ? 0 : 2);
    });
    connect(m_ctrl, &Controller::notice, this, &TtsWindow::showNotice);
    connect(m_ctrl, &Controller::settingsApplied, this, [this] {
        syncFromSettings();
        applyWindowFlags();
    });
    connect(m_ctrl, &Controller::speakingChanged, this, [this](bool on) {
        m_stop->setEnabled(on);
        refreshJobs();
    });

    m_meterTimer.setInterval(33);
    connect(&m_meterTimer, &QTimer::timeout, this, &TtsWindow::onMeterTick);
    m_noticeTimer.setSingleShot(true);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] { m_notice->hide(); });

    refreshEngine();
    syncFromSettings();
    refreshState();
    refreshJobs();
    refreshVc();
}

TtsWindow::~TtsWindow() = default;

void TtsWindow::applyWindowFlags()
{
    const bool onTop = m_ctrl->settings().alwaysOnTop;
    if (bool(windowFlags() & Qt::WindowStaysOnTopHint) == onTop) return;
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, onTop);
    if (wasVisible) show();
}

void TtsWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 10);
    root->setSpacing(8);

    // ---- status bar -------------------------------------------------------
    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(8);
    m_dot = new StatusDot(this);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont bold = m_status->font();
    bold.setBold(true);
    m_status->setFont(bold);
    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("gbttsDetail"));
    m_detail->setEnabled(false);
    m_action = new QPushButton(this);
    m_action->hide();
    connect(m_action, &QPushButton::clicked, this, [this] {
        const Controller::Backend st = m_ctrl->state();
        if (st == Controller::Backend::NotInstalled || st == Controller::Backend::Installing) openEngineSetup();
        else m_ctrl->startBackend();
    });
    m_btnVoices = new QToolButton(this);
    m_btnVoices->setIcon(TtsIcons::voices());
    m_btnVoices->setText(tr("Voices"));
    m_btnVoices->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_btnVoices->setToolTip(tr("Voice library of the active engine"));
    connect(m_btnVoices, &QToolButton::clicked, this, [] { openVoices(); });
    m_btnSettings = new QToolButton(this);
    m_btnSettings->setIcon(TtsIcons::settings());
    m_btnSettings->setToolTip(tr("Settings"));
    connect(m_btnSettings, &QToolButton::clicked, this, [] { openSettings(); });

    auto *statusText = new QVBoxLayout();
    statusText->setSpacing(0);
    statusText->addWidget(m_status);
    statusText->addWidget(m_detail);
    statusRow->addWidget(m_dot, 0, Qt::AlignVCenter);
    statusRow->addLayout(statusText, 1);
    statusRow->addWidget(m_action);
    statusRow->addWidget(m_btnVoices);
    statusRow->addWidget(m_btnSettings);
    root->addLayout(statusRow);

    // ---- voice section --------------------------------------------------------
    auto *voiceBox = new SectionBox(tr("Voice"), this);
    voiceBox->setPersistenceKey(QStringLiteral("gbtts_window_voice"));
    QWidget *vb = voiceBox->body();
    auto *vg = new QGridLayout();
    vg->setHorizontalSpacing(8);
    vg->setVerticalSpacing(6);

    auto *engineRow = new QHBoxLayout();
    engineRow->setSpacing(6);
    m_engineBadge = new QLabel(vb);
    m_engineBadge->setTextFormat(Qt::RichText);
    m_engineBtn = new QToolButton(vb);
    m_engineBtn->setText(tr("Change engine…"));
    m_engineBtn->setToolTip(tr("The engine is chosen in Settings; only its voices are listed here"));
    connect(m_engineBtn, &QToolButton::clicked, this, [] { openSettings(); });
    engineRow->addWidget(caption(tr("Engine"), vb));
    engineRow->addWidget(m_engineBadge, 1);
    engineRow->addWidget(m_engineBtn);

    m_voice = new QComboBox(vb);
    m_voice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_voice->setMinimumContentsLength(16);
    m_voice->setMaxVisibleItems(24);
    m_testVoice = new QToolButton(vb);
    m_testVoice->setIcon(IconFactory::play());
    m_testVoice->setToolTip(tr("Listen to this voice (only you hear it)"));
    m_lang = new QComboBox(vb);
    m_lang->setToolTip(tr("Language of the text. Only the languages this engine speaks are listed."));

    m_speed = new FineSlider(Qt::Horizontal, vb);
    m_speed->setRange(50, 200);
    m_speed->setPageStep(10);
    m_speedLbl = valueLabel(vb, QStringLiteral("2.00×"));
    m_instruct = new QLineEdit(vb);
    m_instruct->setPlaceholderText(tr("Style (optional): cheerful, whispering, angry, excited…"));
    m_instruct->setToolTip(tr("How the sentence should be said. Short descriptions work best."));
    m_instruct->setClearButtonEnabled(true);

    vg->addLayout(engineRow, 0, 0, 1, 4);
    vg->addWidget(caption(tr("Voice"), vb), 1, 0);
    auto *voiceRow = new QHBoxLayout();
    voiceRow->setSpacing(4);
    voiceRow->addWidget(m_voice, 1);
    voiceRow->addWidget(m_testVoice);
    vg->addLayout(voiceRow, 1, 1);
    vg->addWidget(caption(tr("Language"), vb), 1, 2);
    vg->addWidget(m_lang, 1, 3);
    vg->addWidget(caption(tr("Speed"), vb), 2, 0);
    auto *speedRow = new QHBoxLayout();
    speedRow->addWidget(m_speed, 1);
    speedRow->addWidget(m_speedLbl);
    vg->addLayout(speedRow, 2, 1);
    vg->addWidget(m_instruct, 2, 2, 1, 2);
    vg->setColumnStretch(1, 3);
    vg->setColumnStretch(3, 2);
    voiceBox->setContentLayout(vg);
    root->addWidget(voiceBox);

    // ---- output section ----------------------------------------------------------
    auto *outBox = new SectionBox(tr("Output and effects"), this);
    outBox->setPersistenceKey(QStringLiteral("gbtts_window_output"));
    auto *og = new QGridLayout();
    og->setHorizontalSpacing(8);
    og->setVerticalSpacing(6);
    QWidget *ob = outBox->body();

    m_remote = new FineSlider(Qt::Horizontal, ob);
    m_remote->setRange(-30, 12);
    m_remoteLbl = valueLabel(ob, QStringLiteral("+12 dB"));
    m_local = new FineSlider(Qt::Horizontal, ob);
    m_local->setRange(-60, 12);
    m_localLbl = valueLabel(ob, QStringLiteral("+12 dB"));
    m_monitor = new QCheckBox(tr("Hear it"), ob);
    m_monitor->setToolTip(tr("Play the voice in your own headphones too"));
    m_pitch = new FineSlider(Qt::Horizontal, ob);
    m_pitch->setRange(-12, 12);
    m_pitch->setProperty("bipolarFill", true);
    m_pitchLbl = valueLabel(ob, QStringLiteral("+12 st"));
    m_preset = new QComboBox(ob);
    for (const Controller::Preset &p : Controller::presets()) m_preset->addItem(p.name);
    m_preset->addItem(tr("Custom"));
    m_sandboxBtn = new QPushButton(IconFactory::sandbox(), tr("Effects…"), ob);
    m_sandboxBtn->setToolTip(tr("Open the full GameBaiters Soundboard effects chain for the voice"));
    m_meter = new ChannelMeter(ob);

    og->addWidget(caption(tr("Channel"), ob), 0, 0);
    og->addWidget(m_remote, 0, 1);
    og->addWidget(m_remoteLbl, 0, 2);
    og->addWidget(caption(tr("Me"), ob), 0, 3);
    og->addWidget(m_local, 0, 4);
    og->addWidget(m_localLbl, 0, 5);
    og->addWidget(m_monitor, 0, 6);
    og->addWidget(caption(tr("Pitch"), ob), 1, 0);
    og->addWidget(m_pitch, 1, 1);
    og->addWidget(m_pitchLbl, 1, 2);
    og->addWidget(caption(tr("Effect"), ob), 1, 3);
    og->addWidget(m_preset, 1, 4, 1, 2);
    og->addWidget(m_sandboxBtn, 1, 6);
    og->addWidget(m_meter, 2, 0, 1, 7);
    og->setColumnStretch(1, 2);
    og->setColumnStretch(4, 2);

    auto *micRow = new QHBoxLayout();
    m_micMode = new QComboBox(ob);
    m_micMode->addItem(tr("Mute my microphone"));
    m_micMode->addItem(tr("Lower my microphone"));
    m_micMode->addItem(tr("Keep my microphone"));
    m_micMode->setToolTip(tr("What happens to your real microphone while the voice is speaking"));
    m_preview = new QCheckBox(tr("Only for me"), ob);
    m_preview->setToolTip(tr("Preview: the voice plays only in your headphones, the channel hears nothing"));
    m_speakMuted = new QCheckBox(tr("Speak even when muted"), ob);
    m_speakMuted->setToolTip(tr("If your microphone is muted in TeamSpeak, lift the mute just for the voice "
                                "(your real microphone stays silent) and put it back afterwards"));
    micRow->addWidget(caption(tr("While speaking"), ob));
    micRow->addWidget(m_micMode);
    micRow->addSpacing(8);
    micRow->addWidget(m_speakMuted);
    micRow->addStretch(1);
    micRow->addWidget(m_preview);
    og->addLayout(micRow, 3, 0, 1, 7);
    outBox->setContentLayout(og);
    root->addWidget(outBox);

    // ---- live voice changer ---------------------------------------------------------
    auto *vcBox = new SectionBox(tr("Live voice (AI)"), this);
    vcBox->setPersistenceKey(QStringLiteral("gbtts_window_vc"));
    QWidget *vb2 = vcBox->body();
    auto *vcg = new QGridLayout();
    vcg->setHorizontalSpacing(8);
    vcg->setVerticalSpacing(6);
    m_vcEnable = new QCheckBox(tr("Change my voice while I talk"), vb2);
    m_vcEnable->setToolTip(tr("When you talk (push-to-talk or voice activation) an AI model on this PC turns your "
                              "voice into the chosen one, in about a quarter of a second. While this is on, your "
                              "real voice is never sent."));
    m_vcVoice = new QComboBox(vb2);
    m_vcVoice->setToolTip(tr("The voice you will sound like: any voice of your library"));
    m_vcAdd = new QToolButton(vb2);
    m_vcAdd->setIcon(IconFactory::folderOpen());
    m_vcAdd->setToolTip(tr("Add a voice from a recording (5-15 s of clean speech)"));
    m_vcPreset = new QComboBox(vb2);
    m_vcPreset->addItem(tr("Reactive (~0.25 s, about 3 CPU cores)"), QStringLiteral("40ms"));
    m_vcPreset->addItem(tr("Light (~0.35 s, about 1 CPU core)"), QStringLiteral("120ms"));
    m_vcMonitor = new QCheckBox(tr("Hear myself"), vb2);
    m_vcMonitor->setToolTip(tr("Play your converted voice in your headphones (it is delayed: can be distracting)"));
    m_vcStatus = new QLabel(vb2);
    m_vcStatus->setWordWrap(true);
    m_vcMeter = new ChannelMeter(vb2);
    vcg->addWidget(m_vcEnable, 0, 0, 1, 2);
    vcg->addWidget(m_vcMonitor, 0, 3);
    vcg->addWidget(caption(tr("Sound like"), vb2), 1, 0);
    auto *vcVoiceRow = new QHBoxLayout();
    vcVoiceRow->setSpacing(4);
    vcVoiceRow->addWidget(m_vcVoice, 1);
    vcVoiceRow->addWidget(m_vcAdd);
    vcg->addLayout(vcVoiceRow, 1, 1);
    vcg->addWidget(caption(tr("Latency"), vb2), 1, 2);
    vcg->addWidget(m_vcPreset, 1, 3);
    vcg->addWidget(m_vcStatus, 2, 0, 1, 4);
    vcg->addWidget(m_vcMeter, 3, 0, 1, 4);
    vcg->setColumnStretch(1, 3);
    vcg->setColumnStretch(3, 2);
    vcBox->setContentLayout(vcg);
    root->addWidget(vcBox);

    connect(m_vcEnable, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setVcEnabled(on);
    });
    connect(m_vcVoice, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx >= 0) m_ctrl->setVcVoice(m_vcVoice->itemData(idx).toString());
    });
    connect(m_vcPreset, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx >= 0) m_ctrl->setVcPreset(m_vcPreset->itemData(idx).toString());
    });
    connect(m_vcMonitor, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setVcMonitor(on);
    });
    connect(m_vcAdd, &QToolButton::clicked, this, &TtsWindow::addVcTarget);

    // ---- history -------------------------------------------------------------------
    m_history = new QListWidget(this);
    m_history->setObjectName(QStringLiteral("gbttsHistory"));
    m_history->setSelectionMode(QAbstractItemView::SingleSelection);
    m_history->setWordWrap(true);
    m_history->setUniformItemSizes(false);
    m_history->setContextMenuPolicy(Qt::CustomContextMenu);
    m_history->setToolTip(tr("Double-click to say it again, right-click for more"));
    root->addWidget(m_history, 1);

    // ---- input ---------------------------------------------------------------------
    auto *inputRow = new QHBoxLayout();
    inputRow->setSpacing(6);
    m_input = new InputBox(this);
    m_input->setPlaceholderText(tr("Type here and press Enter to speak…"));
    m_send = new QPushButton(IconFactory::play(), tr("Speak"), this);
    m_send->setDefault(false);
    m_send->setAutoDefault(false);
    m_stop = new QPushButton(IconFactory::stop(), tr("Stop"), this);
    m_stop->setEnabled(false);
    auto *btnCol = new QVBoxLayout();
    btnCol->setSpacing(4);
    btnCol->addWidget(m_send);
    btnCol->addWidget(m_stop);
    btnCol->addStretch(1);
    inputRow->addWidget(m_input, 1);
    inputRow->addLayout(btnCol);
    root->addLayout(inputRow);

    // Directly under the text box: whether what I type in the TeamSpeak channel chat
    // is spoken too (v1.5: moved here from Settings, it is an everyday switch).
    auto *footer = new QHBoxLayout();
    footer->setSpacing(8);
    m_readChat = new QCheckBox(tr("Read aloud what I write in the channel chat"), this);
    m_readChat->setToolTip(tr("Everything you type in the channel chat is also spoken by the TTS voice, so whoever "
                              "cannot read the chat hears it.") +
                           QStringLiteral("\n") + tr("Private chats, the server chat and other people's messages are never read."));
    m_hint = new QLabel(this);
    m_hint->setEnabled(false);
    m_notice = new QLabel(this);
    m_notice->setWordWrap(true);
    m_notice->hide();
    footer->addWidget(m_readChat);
    footer->addStretch(1);
    footer->addWidget(m_hint);
    root->addLayout(footer);
    root->addWidget(m_notice);

    // ---- wiring (every control persists through the controller) -------------------
    connect(m_voice, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_syncing || idx < 0) return;
        const QString id = m_voice->itemData(idx).toString();
        if (!id.isEmpty()) m_ctrl->setVoice(id);
        m_testVoice->setEnabled(!id.isEmpty());
    });
    connect(m_testVoice, &QToolButton::clicked, this, [this] {
        const QString id = m_voice->currentData().toString();
        if (!id.isEmpty()) m_ctrl->testVoice(id);
    });
    connect(m_lang, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_syncing && idx >= 0) m_ctrl->setLang(m_lang->itemData(idx).toString());
    });
    connect(m_speed, &QSlider::valueChanged, this, [this](int v) {
        m_speedLbl->setText(QStringLiteral("%1×").arg(v / 100.0, 0, 'f', 2));
        if (!m_syncing) m_ctrl->setSpeed(v / 100.0);
    });
    connect(m_instruct, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (!m_syncing) m_ctrl->setInstruct(t);
    });
    connect(m_instruct, &QLineEdit::editingFinished, m_ctrl, &Controller::saveNow);
    connect(m_remote, &QSlider::valueChanged, this, [this](int v) {
        m_remoteLbl->setText(dbText(v, -31));
        if (!m_syncing) m_ctrl->setRemoteDb(v);
    });
    connect(m_local, &QSlider::valueChanged, this, [this](int v) {
        m_localLbl->setText(dbText(v, -60));
        if (!m_syncing) m_ctrl->setLocalDb(v);
    });
    connect(m_monitor, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setMonitor(on);
    });
    connect(m_pitch, &QSlider::valueChanged, this, [this](int v) {
        m_pitchLbl->setText(QStringLiteral("%1%2 st").arg(v > 0 ? QStringLiteral("+") : QString()).arg(v));
        if (!m_syncing) {
            m_ctrl->setPitch(v);
            refreshPresetLabel();
        }
    });
    // Sliders save 300 ms after the last movement; releasing one saves at once.
    for (QSlider *s : {static_cast<QSlider *>(m_speed), static_cast<QSlider *>(m_remote),
                       static_cast<QSlider *>(m_local), static_cast<QSlider *>(m_pitch)})
        connect(s, &QSlider::sliderReleased, m_ctrl, &Controller::saveNow);
    connect(m_preset, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx < Controller::presets().size()) {
            m_ctrl->applyPreset(idx);
            if (m_sandbox) m_sandbox->setState(m_ctrl->sandbox());
        }
    });
    connect(m_sandboxBtn, &QPushButton::clicked, this, &TtsWindow::openSandbox);
    connect(m_micMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_syncing && idx >= 0) m_ctrl->setMicMode(idx);
    });
    connect(m_preview, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setPreviewOnly(on);
    });
    connect(m_speakMuted, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setSpeakWhenMuted(on);
    });
    connect(m_readChat, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_syncing) m_ctrl->setReadChannelChat(on);
    });
    connect(m_input, &InputBox::submitted, this, &TtsWindow::submit);
    connect(m_input, &InputBox::stopRequested, m_ctrl, &Controller::stopAll);
    connect(m_send, &QPushButton::clicked, this, [this] { submit(m_input->toPlainText().trimmed()); });
    connect(m_stop, &QPushButton::clicked, m_ctrl, &Controller::stopAll);
    connect(m_history, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        m_ctrl->speak(item->data(Qt::UserRole).toString());
    });
    connect(m_history, &QListWidget::customContextMenuRequested, this, &TtsWindow::jobMenu);
}

void TtsWindow::syncFromSettings()
{
    m_syncing = true;
    const Settings &s = m_ctrl->settings();
    const int li = m_lang->findData(s.lang);
    m_lang->setCurrentIndex(li < 0 ? 0 : li);
    m_speed->setValue(int(std::lround(s.speed * 100)));
    m_speedLbl->setText(QStringLiteral("%1×").arg(s.speed, 0, 'f', 2));
    if (m_instruct->text() != s.instruct) m_instruct->setText(s.instruct);
    m_remote->setValue(int(std::lround(s.remoteDb)));
    m_remoteLbl->setText(dbText(m_remote->value(), -31));
    m_local->setValue(int(std::lround(s.localDb)));
    m_localLbl->setText(dbText(m_local->value(), -60));
    m_monitor->setChecked(s.monitor);
    m_pitch->setValue(int(std::lround(s.pitchSt)));
    m_pitchLbl->setText(QStringLiteral("%1%2 st").arg(m_pitch->value() > 0 ? QStringLiteral("+") : QString()).arg(m_pitch->value()));
    m_micMode->setCurrentIndex(s.micMode);
    m_preview->setChecked(s.previewOnly);
    m_speakMuted->setChecked(s.speakWhenMuted);
    m_readChat->setChecked(s.readChannelChat);
    m_input->setHistory(s.history);
    m_syncing = false;
    refreshPresetLabel();
}

void TtsWindow::refreshPresetLabel()
{
    const auto list = Controller::presets();
    const int idx = m_ctrl->settings().presetIndex;
    bool matches = idx >= 0 && idx < list.size() &&
                   QJsonDocument(list[idx].state.toJson()) == QJsonDocument(m_ctrl->sandbox().toJson()) &&
                   std::fabs(list[idx].pitch - m_ctrl->settings().pitchSt) < 0.01;
    m_preset->setCurrentIndex(matches ? idx : m_preset->count() - 1);
}

void TtsWindow::refreshState()
{
    using B = Controller::Backend;
    const B st = m_ctrl->state();
    QColor c = kGrey;
    QString title;
    switch (st) {
    case B::NotInstalled: title = tr("Voice engine not installed"); break;
    case B::Installing: c = kAmber; title = tr("Installing the voice engine"); break;
    case B::Stopped: title = tr("Voice engine stopped"); break;
    case B::Starting: c = kAmber; title = tr("Starting…"); break;
    case B::Loading: c = kAmber; title = tr("Loading…"); break;
    case B::Ready: c = kGreen; title = tr("Ready"); break;
    case B::Busy: c = kAmber; title = tr("Working…"); break;
    case B::Error: c = kRed; title = tr("Error"); break;
    }
    m_dot->setColor(c);
    const QString msg = m_ctrl->stateMessage();
    m_status->setText(msg.isEmpty() || st == B::Ready ? title : QStringLiteral("%1 — %2").arg(title, msg));
    m_dot->setToolTip(msg);

    const QJsonObject info = m_ctrl->backendInfo();
    QStringList parts;
    const QString model = info.value(QStringLiteral("model")).toString();
    if (!model.isEmpty()) parts << model;
    const QString device = info.value(QStringLiteral("device")).toString();
    if (!device.isEmpty()) parts << device;
    const int vramUsed = info.value(QStringLiteral("vram_used_mb")).toInt();
    const int vramTotal = info.value(QStringLiteral("vram_total_mb")).toInt();
    if (vramTotal > 0) parts << tr("VRAM %1/%2 GB").arg(vramUsed / 1024.0, 0, 'f', 1).arg(vramTotal / 1024.0, 0, 'f', 0);
    if (m_ctrl->lastTtfaMs() > 0) parts << tr("replies in %1 ms").arg(int(m_ctrl->lastTtfaMs()));
    parts << (m_ctrl->connected() ? tr("server connected") : tr("not connected: only you hear it"));
    m_detail->setText(parts.join(QStringLiteral(" · ")));

    if (st == B::NotInstalled) {
        m_action->setText(tr("Install…"));
        m_action->show();
    } else if (st == B::Installing) {
        m_action->setText(tr("Show progress"));
        m_action->show();
    } else if (st == B::Stopped || st == B::Error) {
        m_action->setText(tr("Start"));
        m_action->show();
    } else {
        m_action->hide();
    }
}

void TtsWindow::refreshEngine()
{
    const QString engine = m_ctrl->engine();
    m_engineBadge->setText(QStringLiteral("<b>%1</b> &nbsp;<span style='opacity:0.7'>%2</span>")
                               .arg(Controller::engineLabel(engine).toHtmlEscaped(),
                                    VoiceText::engineTagline(engine).toHtmlEscaped()));
    m_engineBadge->setToolTip(VoiceText::engineResources(engine));
    m_syncing = true;
    VoiceText::fillLanguages(m_lang, engine, m_ctrl->settings().lang);
    m_syncing = false;
    // Speaking style is a Qwen3-TTS feature: other engines do not get a dead field.
    m_instruct->setVisible(engine == QLatin1String("qwen"));
    refreshVoices();
}

void TtsWindow::refreshVoices()
{
    m_syncing = true;
    VoiceText::fillVoices(m_voice, m_ctrl->engine(), m_ctrl->voices(), m_ctrl->currentVoice(), m_ctrl->voicesLoaded());
    m_syncing = false;
    const bool haveVoice = !m_voice->currentData().toString().isEmpty();
    m_voice->setEnabled(!m_ctrl->voices().isEmpty());
    m_testVoice->setEnabled(haveVoice);
    m_voice->setToolTip(m_voice->currentData(Qt::ToolTipRole).toString());
}

void TtsWindow::refreshVc()
{
    using S = Controller::VcState;
    const Settings &s = m_ctrl->settings();
    m_syncing = true;
    m_vcEnable->setChecked(s.vcEnabled);
    m_vcMonitor->setChecked(s.vcMonitor);
    m_vcPreset->setCurrentIndex(qMax(0, m_vcPreset->findData(s.vcPreset)));
    {
        const QSignalBlocker block(m_vcVoice);
        m_vcVoice->clear();
        for (const Controller::VcTarget &t : m_ctrl->vcTargets()) {
            m_vcVoice->addItem(t.name, t.id);
            m_vcVoice->setItemData(m_vcVoice->count() - 1,
                                   tr("Reference recording: %1 s").arg(t.seconds, 0, 'f', 1), Qt::ToolTipRole);
        }
        if (m_vcVoice->count() == 0) m_vcVoice->addItem(tr("(no voices yet: add a recording)"));
        const int idx = m_vcVoice->findData(s.vcVoice);
        if (idx >= 0) m_vcVoice->setCurrentIndex(idx);
    }
    m_syncing = false;

    QString text;
    QColor color = kGrey;
    switch (m_ctrl->vcState()) {
    case S::Off:
        text = s.vcEnabled ? tr("Waiting for the voice engine…") : tr("Off: your normal voice is sent.");
        break;
    case S::Downloading:
    case S::Loading:
        color = kAmber;
        text = m_ctrl->vcMessage() + QStringLiteral(" ") + tr("(your normal voice is sent meanwhile)");
        break;
    case S::Ready:
        color = kGreen;
        text = tr("On: you sound like “%1” · delay about %2 ms").arg(m_vcVoice->currentText()).arg(m_ctrl->vcLatencyMs() + 60);
        break;
    case S::Error:
        color = kRed;
        text = m_ctrl->vcMessage();
        break;
    }
    m_vcStatus->setStyleSheet(QStringLiteral("color:%1;").arg(color.name()));
    m_vcStatus->setText(text);
}

void TtsWindow::addVcTarget()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a recording"), QString(),
                                                      tr("Audio (*.wav *.flac *.ogg *.mp3)"));
    if (path.isEmpty()) return;
    if (QMessageBox::question(this, tr("Use this voice"),
                              tr("Use only your own voice, or a voice you have explicit permission to use. "
                                 "Do not impersonate real people. Continue?")) != QMessageBox::Yes)
        return;
    const QString req = QString::number(QRandomGenerator::global()->generate64(), 16);
    m_ctrl->addVcTargetFromFile(req, QFileInfo(path).completeBaseName(), QDir::toNativeSeparators(path));
    showNotice(tr("Analysing the recording…"), 0);
}

void TtsWindow::refreshJobs()
{
    const auto &jobs = m_ctrl->jobs();
    const bool atBottom = m_history->verticalScrollBar()->value() >= m_history->verticalScrollBar()->maximum() - 4;
    m_history->clear();
    for (const Controller::Job &j : jobs) {
        QString state;
        switch (j.state) {
        case Controller::Queued: state = tr("waiting"); break;
        case Controller::Speaking: state = tr("speaking"); break;
        case Controller::Done: state = j.audioMs > 0 ? tr("%1 s").arg(j.audioMs / 1000.0, 0, 'f', 1) : QStringLiteral("✓"); break;
        case Controller::Cancelled: state = tr("stopped"); break;
        case Controller::Failed: state = tr("error"); break;
        }
        QString line = QStringLiteral("%1   %2").arg(j.when.toString(QStringLiteral("HH:mm")), j.text);
        QString meta = state;
        if (j.local) meta += QStringLiteral(" · ") + tr("only me");
        auto *item = new QListWidgetItem(QStringLiteral("%1\n      %2 · %3").arg(line, j.voiceName, meta));
        item->setData(Qt::UserRole, j.text);
        item->setToolTip(j.error.isEmpty() ? j.text : j.error);
        if (j.state == Controller::Speaking) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        if (j.state == Controller::Failed) item->setForeground(kRed);
        m_history->addItem(item);
    }
    if (atBottom) m_history->scrollToBottom();

    const int pending = m_ctrl->pendingJobs();
    m_stop->setEnabled(pending > 0 || m_ctrl->speaking());
    QString hint = tr("Enter to speak · Shift+Enter new line · Esc stops · ↑ previous messages");
    if (pending > 1) hint = tr("%n in queue", nullptr, pending) + QStringLiteral(" · ") + hint;
    m_hint->setText(hint);
}

void TtsWindow::submit(const QString &text)
{
    if (text.isEmpty()) return;
    if (m_ctrl->speak(text) && m_ctrl->settings().clearAfterSend) m_input->clear();
    m_input->setHistory(m_ctrl->settings().history);
}

void TtsWindow::showNotice(const QString &text, int level)
{
    const QColor c = level >= 2 ? kRed : (level == 1 ? kAmber : Theme::derivedCached().text);
    m_notice->setStyleSheet(QStringLiteral("color:%1;").arg(c.name()));
    m_notice->setText(text);
    m_notice->show();
    m_noticeTimer.start(level >= 2 ? 9000 : 6000);
}

void TtsWindow::openSandbox()
{
    if (!m_sandbox) {
        m_sandbox = new ChannelSandboxDialog(0, this, /*micMode=*/true);
        Theme::trackThemedWidget(m_sandbox);
        connect(m_sandbox, &ChannelSandboxDialog::stateChanged, this, [this](const SandboxState &s) {
            m_ctrl->setSandbox(s);
            refreshPresetLabel();
        });
    }
    m_sandbox->setState(m_ctrl->sandbox());
    m_sandbox->show();
    m_sandbox->raise();
    m_sandbox->activateWindow();
    // The dialog titles itself for the microphone in micMode; name what it edits here.
    deferCall(m_sandbox.data(), 0, [d = m_sandbox] {
        if (d) d->setWindowTitle(QObject::tr("GameBaiters TTS - voice effects"));
    });
}

void TtsWindow::onMeterTick()
{
    const float p = m_ctrl->audio()->outputPeak();
    m_meter->setPeak(p, p);
    const float vc = m_ctrl->audio()->vcOutputPeak();
    m_vcMeter->setPeak(vc, vc);
    if (m_sandbox && m_sandbox->isVisible()) {
        m_sandbox->pushAudioLevel(p, p);
        m_sandbox->setCpuPercent(m_ctrl->audio()->dspCpuPercent());
    }
}

void TtsWindow::jobMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_history->itemAt(pos);
    if (!item) return;
    const QString text = item->data(Qt::UserRole).toString();
    QMenu menu(this);
    menu.addAction(IconFactory::play(), tr("Say it again"), this, [this, text] { m_ctrl->speak(text); });
    menu.addAction(tr("Listen only myself"), this, [this, text] { m_ctrl->speak(text, true); });
    menu.addAction(tr("Edit in the box"), this, [this, text] {
        m_input->setPlainText(text);
        m_input->setFocus();
        m_input->moveCursor(QTextCursor::End);
    });
    menu.addAction(IconFactory::copyDoc(), tr("Copy text"), this, [text] { QApplication::clipboard()->setText(text); });
    menu.exec(m_history->viewport()->mapToGlobal(pos));
}

void TtsWindow::showAndRaise(bool focusInput)
{
    show();
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
    m_meterTimer.start();
    refreshState();
    if (focusInput) m_input->setFocus(Qt::ShortcutFocusReason);
}

void TtsWindow::rememberGeometry(bool now)
{
    if (m_syncing || !isVisible()) return;
    m_ctrl->settings().windowGeometry = saveGeometry();
    if (now) m_ctrl->saveNow();
    else m_ctrl->scheduleSave();
}

void TtsWindow::moveEvent(QMoveEvent *e)
{
    QWidget::moveEvent(e);
    rememberGeometry(false);
}

void TtsWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    rememberGeometry(false);
}

void TtsWindow::closeEvent(QCloseEvent *e)
{
    rememberGeometry(true);
    QWidget::closeEvent(e);
}

void TtsWindow::hideEvent(QHideEvent *e)
{
    m_meterTimer.stop();
    if (!m_syncing) {
        m_ctrl->settings().windowGeometry = saveGeometry();
        m_ctrl->saveNow();
    }
    QWidget::hideEvent(e);
}

} // namespace gbtts
