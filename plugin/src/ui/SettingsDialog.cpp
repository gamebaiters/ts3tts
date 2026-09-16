#include "ui/SettingsDialog.h"

#include "core/Controller.h"
#include "net/BackendProcess.h"
#include "ui/ToolbarButton.h"
#include "ui/TtsIcons.h"
#include "ui/VoiceGroups.h"
#include "gbtts.h"

#include "modules/fine_slider.h"
#include "modules/section_box.h"
#include "modules/theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

extern "C" const char *getTs3ConfigPath();

namespace gbtts {

namespace {

const QStringList kEngineOrder = {QStringLiteral("qwen"), QStringLiteral("kokoro"), QStringLiteral("supertonic")};

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setEnabled(false);
    return l;
}

} // namespace

// ===========================================================================
EngineCard::EngineCard(const QString &k, QWidget *parent) : QFrame(parent), key(k)
{
    setObjectName(QStringLiteral("gbttsEngineCard"));
    setFrameShape(QFrame::StyledPanel);
    setCursor(Qt::PointingHandCursor);
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(3);

    auto *top = new QHBoxLayout();
    radio = new QRadioButton(Controller::engineLabel(k), this);
    QFont f = radio->font();
    f.setBold(true);
    radio->setFont(f);
    auto *tag = mutedLabel(QStringLiteral("— ") + VoiceText::engineTagline(k), this);
    tag->setWordWrap(false);
    top->addWidget(radio);
    top->addWidget(tag);
    top->addStretch(1);
    v->addLayout(top);

    auto *desc = new QLabel(VoiceText::engineDescription(k), this);
    desc->setWordWrap(true);
    v->addWidget(desc);
    resources = mutedLabel(VoiceText::engineResources(k), this);
    v->addWidget(resources);
    status = new QLabel(this);
    status->setWordWrap(true);
    status->hide();
    v->addWidget(status);
}

void EngineCard::setSelected(bool on)
{
    const Theme::Derived &d = Theme::derivedCached();
    setStyleSheet(on ? QStringLiteral("#gbttsEngineCard{border:2px solid %1;border-radius:6px;}").arg(d.accent.name())
                     : QStringLiteral("#gbttsEngineCard{border:1px solid %1;border-radius:6px;}").arg(d.border.name()));
}

void EngineCard::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && radio->isEnabled() && rect().contains(e->pos())) radio->click();
    QFrame::mouseReleaseEvent(e);
}

// ===========================================================================
SettingsDialog::SettingsDialog(Controller *ctrl, QWidget *parent) : QDialog(parent), m_ctrl(ctrl)
{
    setWindowTitle(tr("GameBaiters TTS - settings"));
    setWindowIcon(TtsIcons::settings());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setProperty("isGBSoundboard", true);
    setModal(false);
    buildUi();
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);
    resize(680, 780);
    connect(m_ctrl, &Controller::stateChanged, this, &SettingsDialog::refreshBackendStatus);
    connect(m_ctrl, &Controller::enginesChanged, this, &SettingsDialog::refreshEngines);
    connect(m_ctrl, &Controller::engineChanged, this, &SettingsDialog::refreshEngines);
    load();
}

void SettingsDialog::showEvent(QShowEvent *e)
{
    load();
    QDialog::showEvent(e);
}

QWidget *SettingsDialog::buildQwenPage()
{
    auto *page = new QWidget();
    auto *f = new QFormLayout(page);
    f->setContentsMargins(0, 4, 0, 0);
    m_size = new QComboBox(page);
    m_size->addItem(tr("1.7B — best quality (~4.7 GB of VRAM)"), QStringLiteral("1.7B"));
    m_size->addItem(tr("0.6B — lighter and faster (~2.7 GB of VRAM)"), QStringLiteral("0.6B"));
    f->addRow(tr("Model"), m_size);

    auto *chunkRow = new QHBoxLayout();
    m_chunk = new QSpinBox(page);
    m_chunk->setRange(1, 12);
    m_chunkHint = mutedLabel(QString(), page);
    chunkRow->addWidget(m_chunk);
    chunkRow->addWidget(m_chunkHint, 1);
    f->addRow(tr("Streaming step"), chunkRow);

    m_prefill = new QCheckBox(tr("Read the whole sentence before speaking (more natural intonation)"), page);
    f->addRow(QString(), m_prefill);

    auto *tempRow = new QHBoxLayout();
    m_temp = new FineSlider(Qt::Horizontal, page);
    m_temp->setRange(30, 120);
    m_tempLbl = new QLabel(page);
    m_tempLbl->setMinimumWidth(40);
    tempRow->addWidget(m_temp, 1);
    tempRow->addWidget(m_tempLbl);
    f->addRow(tr("Expressiveness"), tempRow);

    m_idle = new QSpinBox(page);
    m_idle->setRange(0, 240);
    m_idle->setSpecialValueText(tr("never"));
    m_idle->setSuffix(tr(" min"));
    f->addRow(tr("Free the GPU after inactivity"), m_idle);

    auto *btns = new QHBoxLayout();
    auto *freeGpu = new QPushButton(tr("Free GPU memory now"), page);
    auto *voices = new QPushButton(TtsIcons::voices(), tr("Create and manage voices…"), page);
    btns->addWidget(freeGpu);
    btns->addWidget(voices);
    btns->addStretch(1);
    f->addRow(btns);

    connect(freeGpu, &QPushButton::clicked, m_ctrl, &Controller::unloadModel);
    connect(voices, &QPushButton::clicked, this, [] { openVoices(); });
    connect(m_size, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::pushEngineOptions);
    connect(m_chunk, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        m_chunkHint->setText(tr("%1 ms of audio per step: lower starts sooner, higher is steadier").arg(v * 1000 / 12));
        pushEngineOptions();
    });
    connect(m_prefill, &QCheckBox::toggled, this, &SettingsDialog::pushEngineOptions);
    connect(m_temp, &QSlider::valueChanged, this, [this](int v) {
        m_tempLbl->setText(QString::number(v / 100.0, 'f', 2));
        pushEngineOptions();
    });
    connect(m_idle, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::pushEngineOptions);
    return page;
}

QWidget *SettingsDialog::buildKokoroPage()
{
    auto *page = new QWidget();
    auto *f = new QFormLayout(page);
    f->setContentsMargins(0, 4, 0, 0);
    m_kokoroVariant = new QComboBox(page);
    m_kokoroVariant->addItem(tr("fp32 — fast on CPU (~325 MB, recommended)"), QStringLiteral("fp32"));
    m_kokoroVariant->addItem(tr("int8 — smaller download (~115 MB) but much slower"), QStringLiteral("int8"));
    f->addRow(tr("Model"), m_kokoroVariant);
    f->addRow(mutedLabel(tr("The first clause of every message starts right away, the rest follows. "
                            "Speaking style and voice creation are Qwen3-TTS features."), page));
    connect(m_kokoroVariant, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &SettingsDialog::pushEngineOptions);
    return page;
}

QWidget *SettingsDialog::buildSupertonicPage()
{
    auto *page = new QWidget();
    auto *f = new QFormLayout(page);
    f->setContentsMargins(0, 4, 0, 0);
    m_steps = new QSpinBox(page);
    m_steps->setRange(4, 16);
    m_steps->setToolTip(tr("More steps: cleaner sound, slower generation"));
    f->addRow(tr("Quality steps"), m_steps);
    connect(m_steps, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::pushEngineOptions);
    return page;
}

QWidget *SettingsDialog::buildEngineSection(QWidget *page)
{
    auto *engineBox = new SectionBox(tr("Voice engine"), page);
    engineBox->setPersistenceKey(QStringLiteral("gbtts_settings_engine"));
    auto *ev = new QVBoxLayout();
    ev->setSpacing(6);
    ev->addWidget(mutedLabel(tr("Only the chosen engine is loaded; the TTS window and the voice library show only "
                                "its voices. Switching frees the memory of the previous one."), engineBox->body()));

    m_engineGroup = new QButtonGroup(this);
    m_engineGroup->setExclusive(true);
    for (const QString &key : kEngineOrder) {
        auto *card = new EngineCard(key, engineBox->body());
        m_cards.insert(key, card);
        m_engineGroup->addButton(card->radio);
        ev->addWidget(card);
        connect(card->radio, &QRadioButton::toggled, this, [this, key](bool on) {
            if (m_loading || !on) return;
            m_ctrl->setEngine(key);
            refreshEngines();
        });
    }

    m_engineHint = new QLabel(engineBox->body());
    QFont bold = m_engineHint->font();
    bold.setBold(true);
    m_engineHint->setFont(bold);
    ev->addSpacing(4);
    ev->addWidget(m_engineHint);

    m_enginePages = new QStackedWidget(engineBox->body());
    m_enginePages->addWidget(buildQwenPage());
    m_enginePages->addWidget(buildKokoroPage());
    m_enginePages->addWidget(buildSupertonicPage());
    ev->addWidget(m_enginePages);
    engineBox->setContentLayout(ev);
    return engineBox;
}

void SettingsDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    auto *col = new QVBoxLayout(page);
    col->setSpacing(10);

    col->addWidget(buildEngineSection(page));

    // ---- backend ----------------------------------------------------------------
    auto *backBox = new SectionBox(tr("Engine installation"), page);
    backBox->setPersistenceKey(QStringLiteral("gbtts_settings_backend"));
    auto *bf = new QFormLayout();
    auto *homeRow = new QHBoxLayout();
    m_home = new QLineEdit(backBox->body());
    m_home->setPlaceholderText(BackendProcess::defaultHome());
    auto *browse = new QPushButton(tr("Browse…"), backBox->body());
    homeRow->addWidget(m_home, 1);
    homeRow->addWidget(browse);
    bf->addRow(tr("Folder"), homeRow);
    m_homeStatus = new QLabel(backBox->body());
    m_homeStatus->setWordWrap(true);
    bf->addRow(QString(), m_homeStatus);
    m_autostart = new QCheckBox(tr("Start the engine together with TeamSpeak"), backBox->body());
    bf->addRow(QString(), m_autostart);
    auto *btnRow = new QHBoxLayout();
    auto *start = new QPushButton(tr("Start / restart"), backBox->body());
    auto *stop = new QPushButton(tr("Stop"), backBox->body());
    auto *log = new QPushButton(tr("Open log"), backBox->body());
    auto *folder = new QPushButton(tr("Open folder"), backBox->body());
    btnRow->addWidget(start);
    btnRow->addWidget(stop);
    btnRow->addWidget(log);
    btnRow->addWidget(folder);
    btnRow->addStretch(1);
    bf->addRow(btnRow);
    auto *install = new QPushButton(tr("Install / repair the engine…"), backBox->body());
    bf->addRow(QString(), install);
    backBox->setContentLayout(bf);
    col->addWidget(backBox);

    // ---- audio -------------------------------------------------------------------
    auto *audioBox = new SectionBox(tr("Audio"), page);
    audioBox->setPersistenceKey(QStringLiteral("gbtts_settings_audio"));
    auto *af = new QFormLayout();
    auto *gainRow = new QHBoxLayout();
    m_voiceGain = new FineSlider(Qt::Horizontal, audioBox->body());
    m_voiceGain->setRange(-12, 12);
    m_voiceGain->setProperty("bipolarFill", true);
    m_voiceGainLbl = new QLabel(audioBox->body());
    m_voiceGainLbl->setMinimumWidth(52);
    gainRow->addWidget(m_voiceGain, 1);
    gainRow->addWidget(m_voiceGainLbl);
    af->addRow(tr("Voice level before effects"), gainRow);
    m_leveler = new QCheckBox(tr("Even out the loudness of every voice"), audioBox->body());
    af->addRow(QString(), m_leveler);
    m_jitter = new QSpinBox(audioBox->body());
    m_jitter->setRange(40, 1000);
    m_jitter->setSingleStep(20);
    m_jitter->setSuffix(tr(" ms"));
    m_jitter->setToolTip(tr("Audio buffered before a message starts. Raise it if the voice stutters while a game "
                            "uses the GPU; lower it for faster replies."));
    af->addRow(tr("Start buffer"), m_jitter);
    audioBox->setContentLayout(af);
    col->addWidget(audioBox);

    // ---- text ---------------------------------------------------------------------
    auto *textBox = new SectionBox(tr("Text"), page);
    textBox->setPersistenceKey(QStringLiteral("gbtts_settings_text"));
    auto *tf = new QFormLayout();
    m_numbers = new QCheckBox(tr("Read numbers, times, dates, money and units in words"), textBox->body());
    tf->addRow(QString(), m_numbers);
    m_slang = new QCheckBox(tr("Expand chat abbreviations (cmq, xké, nn, tvb…)"), textBox->body());
    tf->addRow(QString(), m_slang);
    auto *echoRow = new QHBoxLayout();
    m_echo = new QCheckBox(tr("Also write the message in the channel chat, prefix:"), textBox->body());
    m_prefix = new QLineEdit(textBox->body());
    m_prefix->setMaximumWidth(120);
    echoRow->addWidget(m_echo);
    echoRow->addWidget(m_prefix);
    echoRow->addStretch(1);
    // Empty label: the row lines up with the other checkboxes in the field column
    // (a label-less addRow(layout) spans both columns and starts at the left edge).
    tf->addRow(QString(), echoRow);
    // "Read my channel chat aloud" lives next to the text box in the TTS window.
    auto *dictLabel = new QLabel(tr("Pronunciation dictionary — how to say names, nicknames and words the voice "
                                    "gets wrong:"), textBox->body());
    dictLabel->setWordWrap(true);
    tf->addRow(dictLabel);
    m_dict = new QTableWidget(0, 2, textBox->body());
    m_dict->setHorizontalHeaderLabels({tr("Written"), tr("Say it as")});
    m_dict->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_dict->verticalHeader()->hide();
    m_dict->setMinimumHeight(140);
    tf->addRow(m_dict);
    auto *dictBtns = new QHBoxLayout();
    auto *addWord = new QPushButton(tr("Add"), textBox->body());
    auto *delWord = new QPushButton(tr("Remove"), textBox->body());
    dictBtns->addWidget(addWord);
    dictBtns->addWidget(delWord);
    dictBtns->addStretch(1);
    tf->addRow(dictBtns);
    textBox->setContentLayout(tf);
    col->addWidget(textBox);

    // ---- interface ------------------------------------------------------------------
    auto *uiBox = new SectionBox(tr("Interface"), page);
    uiBox->setPersistenceKey(QStringLiteral("gbtts_settings_ui"));
    auto *uf = new QFormLayout();
    m_toolbar = new QCheckBox(tr("Show the TTS button in the TeamSpeak toolbar"), uiBox->body());
    uf->addRow(QString(), m_toolbar);
    m_clear = new QCheckBox(tr("Clear the box after sending"), uiBox->body());
    uf->addRow(QString(), m_clear);
    m_onTop = new QCheckBox(tr("Keep the TTS window on top"), uiBox->body());
    uf->addRow(QString(), m_onTop);
    m_uiLang = new QComboBox(uiBox->body());
    m_uiLang->addItem(QStringLiteral("Italiano"), QStringLiteral("it"));
    m_uiLang->addItem(QStringLiteral("English"), QStringLiteral("en"));
    uf->addRow(tr("Language (after restarting TeamSpeak)"), m_uiLang);
    m_autoUpdate = new QCheckBox(tr("Check for updates automatically (at most once a day)"), uiBox->body());
    uf->addRow(QString(), m_autoUpdate);
    auto *updateRow = new QHBoxLayout();
    auto *checkUpdates = new QPushButton(tr("Check for updates now"), uiBox->body());
    updateRow->addWidget(checkUpdates);
    updateRow->addWidget(mutedLabel(tr("Installed version %1").arg(QStringLiteral(GBTTS_VERSION)), uiBox->body()), 1);
    uf->addRow(QString(), updateRow);
    uf->addRow(mutedLabel(tr("Hotkeys: TeamSpeak › Tools › Options › Hotkeys › Add › Plugins › GameBaiters - TTS.\n"
                             "Chat: /tts <text> speaks, /tts stop stops."), uiBox->body()));
    uf->addRow(mutedLabel(tr("Every change is saved immediately."), uiBox->body()));
    uiBox->setContentLayout(uf);
    col->addWidget(uiBox);

    col->addStretch(1);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);

    auto *bottom = new QHBoxLayout();
    bottom->addStretch(1);
    auto *close = new QPushButton(tr("Close"), this);
    bottom->addWidget(close);
    outer->addLayout(bottom);

    // ---- wiring -------------------------------------------------------------------------
    connect(m_home, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        m_ctrl->settings().backendHome = QDir::toNativeSeparators(m_home->text().trimmed());
        m_ctrl->saveNow();
        refreshBackendStatus();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Engine folder"), m_home->text());
        if (d.isEmpty()) return;
        m_home->setText(QDir::toNativeSeparators(d));
        m_ctrl->settings().backendHome = m_home->text();
        m_ctrl->saveNow();
        refreshBackendStatus();
    });
    connect(m_autostart, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        m_ctrl->settings().autostart = on;
        m_ctrl->saveNow();
    });
    connect(start, &QPushButton::clicked, m_ctrl, &Controller::restartBackend);
    connect(stop, &QPushButton::clicked, m_ctrl, &Controller::stopBackend);
    connect(log, &QPushButton::clicked, this, [this] {
        const QString f = m_ctrl->logFile();
        if (QFileInfo::exists(f)) QDesktopServices::openUrl(QUrl::fromLocalFile(f));
        else QMessageBox::information(this, windowTitle(), tr("No log yet: start the engine first."));
    });
    connect(folder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_ctrl->resolvedHome()));
    });
    connect(install, &QPushButton::clicked, this, &SettingsDialog::installBackend);

    connect(m_voiceGain, &QSlider::valueChanged, this, [this](int v) {
        m_voiceGainLbl->setText(QStringLiteral("%1%2 dB").arg(v > 0 ? QStringLiteral("+") : QString()).arg(v));
        if (!m_loading) m_ctrl->setVoiceDb(v);
    });
    connect(m_voiceGain, &QSlider::sliderReleased, m_ctrl, &Controller::saveNow);
    connect(m_jitter, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (!m_loading) m_ctrl->setJitterMs(v);
    });
    connect(m_leveler, &QCheckBox::toggled, this, &SettingsDialog::pushEngineOptions);

    auto textChanged = [this] {
        if (m_loading) return;
        Settings &s = m_ctrl->settings();
        s.numbers = m_numbers->isChecked();
        s.chatSlang = m_slang->isChecked();
        s.echoToChat = m_echo->isChecked();
        s.chatPrefix = m_prefix->text();
        m_ctrl->applyBackendSettings();
    };
    connect(m_numbers, &QCheckBox::toggled, this, textChanged);
    connect(m_slang, &QCheckBox::toggled, this, textChanged);
    connect(m_echo, &QCheckBox::toggled, this, textChanged);
    connect(m_prefix, &QLineEdit::editingFinished, this, textChanged);
    connect(m_dict, &QTableWidget::itemChanged, this, [this] {
        if (!m_loading) saveDictionary();
    });
    connect(addWord, &QPushButton::clicked, this, [this] {
        m_loading = true;
        const int r = m_dict->rowCount();
        m_dict->insertRow(r);
        m_dict->setItem(r, 0, new QTableWidgetItem());
        m_dict->setItem(r, 1, new QTableWidgetItem());
        m_loading = false;
        m_dict->editItem(m_dict->item(r, 0));
    });
    connect(delWord, &QPushButton::clicked, this, [this] {
        const int r = m_dict->currentRow();
        if (r >= 0) {
            m_dict->removeRow(r);
            saveDictionary();
        }
    });

    auto uiChanged = [this] {
        if (m_loading) return;
        Settings &s = m_ctrl->settings();
        const bool toolbarChanged = s.toolbarButton != m_toolbar->isChecked();
        s.toolbarButton = m_toolbar->isChecked();
        s.clearAfterSend = m_clear->isChecked();
        s.alwaysOnTop = m_onTop->isChecked();
        s.uiLanguage = m_uiLang->currentData().toString();
        m_ctrl->saveNow();
        if (toolbarChanged) ToolbarButton::setUserEnabled(s.toolbarButton);
        emit m_ctrl->settingsApplied();
    };
    connect(m_toolbar, &QCheckBox::toggled, this, uiChanged);
    connect(m_clear, &QCheckBox::toggled, this, uiChanged);
    connect(m_onTop, &QCheckBox::toggled, this, uiChanged);
    connect(m_uiLang, QOverload<int>::of(&QComboBox::currentIndexChanged), this, uiChanged);
    connect(m_autoUpdate, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        m_ctrl->settings().updateAutoCheck = on;
        m_ctrl->saveNow();
    });
    connect(checkUpdates, &QPushButton::clicked, this, [] { checkForUpdates(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
}

void SettingsDialog::pushEngineOptions()
{
    if (m_loading) return;
    Settings &s = m_ctrl->settings();
    s.qwenSize = m_size->currentData().toString();
    s.kokoroVariant = m_kokoroVariant->currentData().toString();
    s.chunkSize = m_chunk->value();
    s.fullTextPrefill = m_prefill->isChecked();
    s.temperature = m_temp->value() / 100.0;
    s.supertonicSteps = m_steps->value();
    s.idleUnloadMin = m_idle->value();
    s.leveler = m_leveler->isChecked();
    m_ctrl->applyBackendSettings();
}

void SettingsDialog::load()
{
    m_loading = true;
    const Settings &s = m_ctrl->settings();
    m_size->setCurrentIndex(qMax(0, m_size->findData(s.qwenSize)));
    m_kokoroVariant->setCurrentIndex(qMax(0, m_kokoroVariant->findData(s.kokoroVariant)));
    m_chunk->setValue(s.chunkSize);
    m_chunkHint->setText(tr("%1 ms of audio per step: lower starts sooner, higher is steadier").arg(s.chunkSize * 1000 / 12));
    m_prefill->setChecked(s.fullTextPrefill);
    m_temp->setValue(int(s.temperature * 100));
    m_tempLbl->setText(QString::number(s.temperature, 'f', 2));
    m_steps->setValue(s.supertonicSteps);
    m_idle->setValue(s.idleUnloadMin);
    m_leveler->setChecked(s.leveler);
    m_home->setText(s.backendHome);
    m_autostart->setChecked(s.autostart);
    m_voiceGain->setValue(int(s.voiceDb));
    m_voiceGainLbl->setText(QStringLiteral("%1%2 dB").arg(s.voiceDb > 0 ? QStringLiteral("+") : QString()).arg(int(s.voiceDb)));
    m_jitter->setValue(s.jitterMs);
    m_numbers->setChecked(s.numbers);
    m_slang->setChecked(s.chatSlang);
    m_echo->setChecked(s.echoToChat);
    m_prefix->setText(s.chatPrefix);
    m_dict->setRowCount(0);
    for (auto it = s.dictionary.cbegin(); it != s.dictionary.cend(); ++it) {
        const int r = m_dict->rowCount();
        m_dict->insertRow(r);
        m_dict->setItem(r, 0, new QTableWidgetItem(it.key()));
        m_dict->setItem(r, 1, new QTableWidgetItem(it.value()));
    }
    m_toolbar->setChecked(s.toolbarButton);
    m_clear->setChecked(s.clearAfterSend);
    m_onTop->setChecked(s.alwaysOnTop);
    m_uiLang->setCurrentIndex(qMax(0, m_uiLang->findData(s.uiLanguage)));
    m_autoUpdate->setChecked(s.updateAutoCheck);
    m_loading = false;
    refreshEngines();
    refreshBackendStatus();
}

void SettingsDialog::refreshEngines()
{
    const bool wasLoading = m_loading;
    m_loading = true;
    const QString current = m_ctrl->engine();
    for (EngineCard *card : m_cards) {
        const bool selected = card->key == current;
        const bool available = m_ctrl->engineAvailable(card->key);
        card->radio->setChecked(selected);
        card->radio->setEnabled(available || selected);
        card->setSelected(selected);
        QString status;
        if (!available) {
            const QString why = m_ctrl->engineUnavailableReason(card->key);
            status = why.isEmpty() ? tr("Not available on this PC.") : tr("Not available on this PC: %1").arg(why);
            card->status->setStyleSheet(QStringLiteral("color:#D9534F;"));
        } else if (selected) {
            status = m_ctrl->voicesLoaded() ? tr("Active — %1").arg(VoiceText::voiceCountText(current, m_ctrl->voices().size()))
                                            : tr("Active");
            card->status->setStyleSheet(QStringLiteral("color:#57C45E;"));
        }
        card->status->setText(status);
        card->status->setVisible(!status.isEmpty());
    }
    m_enginePages->setCurrentIndex(qMax(0, kEngineOrder.indexOf(current)));
    m_engineHint->setText(tr("%1 options").arg(Controller::engineLabel(current)));
    m_loading = wasLoading;
}

void SettingsDialog::refreshBackendStatus()
{
    const BackendProcess::Layout l = BackendProcess::resolve(m_ctrl->settings().backendHome);
    if (l.valid) {
        m_homeStatus->setText(tr("Found in %1 — %2").arg(l.home, m_ctrl->stateMessage()));
    } else {
        m_homeStatus->setText(tr("Not installed (%1). Press “Install / repair the engine…”.").arg(l.problem));
    }
    refreshEngines();
}

void SettingsDialog::saveDictionary()
{
    QMap<QString, QString> dict;
    for (int r = 0; r < m_dict->rowCount(); ++r) {
        const QTableWidgetItem *a = m_dict->item(r, 0);
        const QTableWidgetItem *b = m_dict->item(r, 1);
        const QString k = a ? a->text().trimmed() : QString();
        const QString v = b ? b->text().trimmed() : QString();
        if (!k.isEmpty() && !v.isEmpty()) dict.insert(k, v);
    }
    m_ctrl->settings().dictionary = dict;
    m_ctrl->applyBackendSettings();
}

void SettingsDialog::installBackend()
{
    openEngineSetup();
}

} // namespace gbtts
