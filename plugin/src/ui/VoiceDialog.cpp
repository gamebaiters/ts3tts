#include "ui/VoiceDialog.h"

#include "core/Controller.h"
#include "gbtts.h"
#include "ui/TtsIcons.h"
#include "ui/VoiceGroups.h"

#include "modules/icon_factory.h"
#include "modules/theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

namespace gbtts {

namespace {

QString newReq()
{
    return QString::number(QRandomGenerator::global()->generate64(), 16);
}

void fillCreationLanguages(QComboBox *c)
{
    for (const QString &code : {QStringLiteral("it"), QStringLiteral("en"), QStringLiteral("es"),
                                QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("pt")})
        c->addItem(VoiceText::languageName(code), code);
}

} // namespace

VoiceDialog::VoiceDialog(Controller *ctrl, QWidget *parent) : QDialog(parent), m_ctrl(ctrl)
{
    setWindowTitle(tr("GameBaiters TTS - voices"));
    setWindowIcon(TtsIcons::voices());
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setProperty("isGBSoundboard", true);
    setModal(false);
    buildUi();
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);
    resize(900, 580);

    connect(m_ctrl, &Controller::voicesChanged, this, &VoiceDialog::refreshList);
    connect(m_ctrl, &Controller::engineChanged, this, [this] {
        m_filter->clear();
        refreshEnginePanel();
        refreshList();
    });
    connect(m_ctrl, &Controller::designReady, this, [this](const QString &req, double seconds) {
        if (req != m_dReq) return;
        clearBusy();
        m_dReady = true;
        m_dSave->setEnabled(true);
        m_busyText->setText(tr("Preview ready (%1 s). Like it? Give it a name and save it; otherwise change the "
                               "description and generate again.").arg(seconds, 0, 'f', 1));
    });
    connect(m_ctrl, &Controller::voiceOpResult, this, [this](const QString &req, bool ok, const QString &msg) {
        if (req != m_dReq && req != m_cReq) return;
        clearBusy();
        if (ok) {
            m_busyText->setText(tr("Voice “%1” created and selected.").arg(msg));
            if (req == m_dReq) {
                m_dReady = false;
                m_dSave->setEnabled(false);
            }
        } else {
            m_busyText->setText(tr("Failed: %1").arg(msg));
        }
    });
    refreshEnginePanel();
    refreshList();
}

QWidget *VoiceDialog::buildCreatePages()
{
    m_right = new QStackedWidget();

    // ---- page 0: Qwen3-TTS voice creation ---------------------------------------
    auto *tabs = new QTabWidget(m_right);

    auto *design = new QWidget(tabs);
    auto *df = new QFormLayout(design);
    auto *dIntro = new QLabel(tr("Describe the voice you want: gender, age, timbre, tone, pace, accent. The model "
                                 "invents a matching voice; listen to it and save it if you like it."), design);
    dIntro->setWordWrap(true);
    df->addRow(dIntro);
    m_dName = new QLineEdit(design);
    m_dName->setPlaceholderText(tr("e.g. Narrator"));
    df->addRow(tr("Name"), m_dName);
    m_dLang = new QComboBox(design);
    fillCreationLanguages(m_dLang);
    df->addRow(tr("Language"), m_dLang);
    m_dExamples = new QComboBox(design);
    m_dExamples->addItem(tr("Examples…"));
    const QStringList examples = {
        QStringLiteral("Voce maschile italiana sui quarant'anni, profonda e calda, tono calmo e rassicurante, "
                       "ritmo lento da narratore di documentari."),
        QStringLiteral("Voce femminile italiana giovane, squillante ed energica, tono allegro e ironico, "
                       "parla velocemente come una streamer."),
        QStringLiteral("Voce maschile italiana di un uomo anziano, leggermente roca, saggia e gentile, "
                       "pause lunghe e scandite."),
        QStringLiteral("Voce femminile italiana matura, professionale e autorevole, dizione perfetta da "
                       "annunciatrice del telegiornale."),
        QStringLiteral("Voce maschile giovane, sussurrata e misteriosa, come chi racconta una storia di "
                       "fantasmi di notte."),
        QStringLiteral("Energetic male sports commentator, loud and excited, fast pace, big emotional peaks."),
    };
    for (const QString &e : examples) m_dExamples->addItem(e.left(70) + QStringLiteral("…"), e);
    df->addRow(QString(), m_dExamples);
    m_dDesc = new QPlainTextEdit(design);
    m_dDesc->setPlaceholderText(tr("Describe the voice…"));
    m_dDesc->setMinimumHeight(90);
    df->addRow(tr("Description"), m_dDesc);
    m_dText = new QLineEdit(design);
    m_dText->setText(QStringLiteral("Ciao a tutti, questa è la mia nuova voce. Vi sento benissimo, possiamo "
                                    "iniziare quando volete."));
    df->addRow(tr("Sample sentence"), m_dText);
    auto *db = new QHBoxLayout();
    m_dPreview = new QPushButton(IconFactory::play(), tr("Generate preview"), design);
    m_dSave = new QPushButton(IconFactory::save(), tr("Save voice"), design);
    m_dSave->setEnabled(false);
    db->addWidget(m_dPreview);
    db->addWidget(m_dSave);
    db->addStretch(1);
    df->addRow(db);
    auto *dNote = new QLabel(tr("The first preview loads the voice-design model (a few seconds, and ~4 GB the "
                                "first time if it is not downloaded yet)."), design);
    dNote->setWordWrap(true);
    dNote->setEnabled(false);
    df->addRow(dNote);
    tabs->addTab(design, tr("Design from a description"));

    auto *clone = new QWidget(tabs);
    auto *cf = new QFormLayout(clone);
    auto *cIntro = new QLabel(tr("Use 5-15 seconds of clean speech: one voice, no music, no background noise. "
                                 "Writing exactly what is said makes the copy much more faithful."), clone);
    cIntro->setWordWrap(true);
    cf->addRow(cIntro);
    m_cName = new QLineEdit(clone);
    m_cName->setPlaceholderText(tr("e.g. My voice"));
    cf->addRow(tr("Name"), m_cName);
    auto *pathRow = new QHBoxLayout();
    m_cPath = new QLineEdit(clone);
    m_cPath->setPlaceholderText(tr("WAV, FLAC, OGG or MP3 file"));
    auto *browse = new QPushButton(IconFactory::folderOpen(), QString(), clone);
    browse->setToolTip(tr("Choose file"));
    pathRow->addWidget(m_cPath, 1);
    pathRow->addWidget(browse);
    cf->addRow(tr("Recording"), pathRow);
    m_cText = new QPlainTextEdit(clone);
    m_cText->setPlaceholderText(tr("Exactly what is said in the recording (recommended)"));
    m_cText->setMinimumHeight(70);
    cf->addRow(tr("Transcript"), m_cText);
    m_cLang = new QComboBox(clone);
    fillCreationLanguages(m_cLang);
    cf->addRow(tr("Language"), m_cLang);
    m_cConsent = new QCheckBox(tr("This is my own voice, or I have explicit permission to use it"), clone);
    cf->addRow(m_cConsent);
    m_cCreate = new QPushButton(IconFactory::save(), tr("Create voice"), clone);
    m_cCreate->setEnabled(false);
    cf->addRow(m_cCreate);
    tabs->addTab(clone, tr("Clone from a recording"));
    m_right->addWidget(tabs);

    // ---- page 1: engines with a fixed voice set ----------------------------------
    auto *info = new QWidget(m_right);
    auto *iv = new QVBoxLayout(info);
    iv->setContentsMargins(16, 16, 16, 16);
    m_engineInfo = new QLabel(info);
    m_engineInfo->setWordWrap(true);
    m_engineInfo->setTextFormat(Qt::RichText);
    m_engineInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto *toSettings = new QPushButton(TtsIcons::settings(), tr("Choose the engine in Settings…"), info);
    iv->addWidget(m_engineInfo);
    iv->addSpacing(8);
    iv->addWidget(toSettings, 0, Qt::AlignLeft);
    iv->addStretch(1);
    m_right->addWidget(info);

    connect(toSettings, &QPushButton::clicked, this, [] { openSettings(); });
    connect(m_dExamples, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
        if (idx > 0) m_dDesc->setPlainText(m_dExamples->itemData(idx).toString());
        m_dExamples->setCurrentIndex(0);
    });
    connect(m_dDesc, &QPlainTextEdit::textChanged, this, [this] {
        m_dReady = false;
        m_dSave->setEnabled(false);
    });
    connect(m_dPreview, &QPushButton::clicked, this, &VoiceDialog::onDesignPreview);
    connect(m_dSave, &QPushButton::clicked, this, &VoiceDialog::onDesignSave);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Choose a recording"), QString(),
                                                       tr("Audio (*.wav *.flac *.ogg *.mp3)"));
        if (f.isEmpty()) return;
        m_cPath->setText(QDir::toNativeSeparators(f));
        if (m_cName->text().trimmed().isEmpty()) m_cName->setText(QFileInfo(f).completeBaseName());
    });
    auto updateClone = [this] {
        m_cCreate->setEnabled(m_cConsent->isChecked() && QFileInfo::exists(m_cPath->text().trimmed()));
    };
    connect(m_cConsent, &QCheckBox::toggled, this, updateClone);
    connect(m_cPath, &QLineEdit::textChanged, this, updateClone);
    connect(m_cCreate, &QPushButton::clicked, this, &VoiceDialog::onCloneCreate);
    return m_right;
}

void VoiceDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    auto *split = new QSplitter(Qt::Horizontal, this);

    // ---- library -------------------------------------------------------------
    auto *left = new QWidget(split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 6, 0);
    m_title = new QLabel(left);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    ll->addWidget(m_title);
    m_filter = new QLineEdit(left);
    m_filter->setPlaceholderText(tr("Search by name or language…"));
    m_filter->setClearButtonEnabled(true);
    ll->addWidget(m_filter);
    m_list = new QListWidget(left);
    ll->addWidget(m_list, 1);
    m_details = new QLabel(left);
    m_details->setWordWrap(true);
    m_details->setMinimumHeight(48);
    ll->addWidget(m_details);
    auto *lb = new QHBoxLayout();
    m_use = new QPushButton(tr("Use"), left);
    m_test = new QPushButton(IconFactory::play(), tr("Listen"), left);
    m_rename = new QPushButton(tr("Rename"), left);
    m_delete = new QPushButton(IconFactory::trash(), QString(), left);
    m_delete->setToolTip(tr("Delete voice"));
    lb->addWidget(m_use);
    lb->addWidget(m_test);
    lb->addWidget(m_rename);
    lb->addStretch(1);
    lb->addWidget(m_delete);
    ll->addLayout(lb);

    split->addWidget(left);
    split->addWidget(buildCreatePages());
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    root->addWidget(split, 1);

    auto *busyRow = new QHBoxLayout();
    m_busy = new QProgressBar(this);
    m_busy->setRange(0, 0);
    m_busy->setMaximumWidth(120);
    m_busy->setTextVisible(false);
    m_busy->hide();
    m_busyText = new QLabel(this);
    m_busyText->setWordWrap(true);
    busyRow->addWidget(m_busy);
    busyRow->addWidget(m_busyText, 1);
    auto *close = new QPushButton(tr("Close"), this);
    busyRow->addWidget(close);
    root->addLayout(busyRow);

    // ---- wiring ---------------------------------------------------------------
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_filter, &QLineEdit::textChanged, this, &VoiceDialog::refreshList);
    connect(m_list, &QListWidget::currentRowChanged, this, &VoiceDialog::refreshButtons);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (!selectedId().isEmpty()) m_ctrl->setVoice(selectedId());
    });
    connect(m_use, &QPushButton::clicked, this, [this] { m_ctrl->setVoice(selectedId()); });
    connect(m_test, &QPushButton::clicked, this, [this] { m_ctrl->testVoice(selectedId()); });
    connect(m_rename, &QPushButton::clicked, this, [this] {
        const QString id = selectedId();
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename voice"), tr("New name"), QLineEdit::Normal,
                                                   m_ctrl->voiceName(id), &ok);
        if (ok && !name.trimmed().isEmpty()) m_ctrl->renameVoice(id, name.trimmed());
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const QString id = selectedId();
        if (QMessageBox::question(this, tr("Delete voice"),
                                  tr("Delete “%1”? This cannot be undone.").arg(m_ctrl->voiceName(id))) ==
            QMessageBox::Yes)
            m_ctrl->deleteVoice(id);
    });
}

void VoiceDialog::refreshEnginePanel()
{
    const QString engine = m_ctrl->engine();
    const bool qwen = engine == QLatin1String("qwen");
    m_right->setCurrentIndex(qwen ? 0 : 1);
    if (!qwen) {
        m_engineInfo->setText(
            tr("<h3>%1</h3><p>%2</p><p>This engine has a fixed set of built-in voices: pick one on the left and "
               "press <b>Use</b>, or <b>Listen</b> to hear a sample.</p>"
               "<p>Designing a voice from a description or cloning one from a recording needs the "
               "<b>Qwen3-TTS</b> engine (NVIDIA GPU).</p>")
                .arg(Controller::engineLabel(engine).toHtmlEscaped(),
                     VoiceText::engineDescription(engine).toHtmlEscaped()));
    }
}

void VoiceDialog::refreshList()
{
    const QString engine = m_ctrl->engine();
    const auto &voices = m_ctrl->voices();
    m_title->setText(m_ctrl->voicesLoaded()
                         ? VoiceText::voiceCountText(engine, voices.size())
                         : tr("Loading the %1 voices…").arg(Controller::engineLabel(engine)));

    const QString keep = selectedId().isEmpty() ? m_ctrl->currentVoice() : selectedId();
    const QString filter = m_filter->text().trimmed();
    const QSignalBlocker block(m_list);
    m_list->clear();
    QListWidgetItem *toSelect = nullptr;
    const QList<VoiceText::Group> groups = VoiceText::groupVoices(engine, voices);
    for (const VoiceText::Group &g : groups) {
        QList<Controller::Voice> shown;
        for (const Controller::Voice &v : g.voices) {
            if (filter.isEmpty() || v.name.contains(filter, Qt::CaseInsensitive) ||
                g.title.contains(filter, Qt::CaseInsensitive) || v.description.contains(filter, Qt::CaseInsensitive))
                shown.append(v);
        }
        if (shown.isEmpty()) continue;
        if (groups.size() > 1) {
            auto *h = new QListWidgetItem(g.title, m_list);
            h->setFlags(Qt::NoItemFlags);
            QFont f = h->font();
            f.setBold(true);
            h->setFont(f);
        }
        for (const Controller::Voice &v : shown) {
            QString label = QStringLiteral("   ") + VoiceText::displayName(v);
            const bool current = v.id == m_ctrl->currentVoice();
            if (current) label += QStringLiteral("   ✓ ") + tr("in use");
            auto *item = new QListWidgetItem(label, m_list);
            item->setData(Qt::UserRole, v.id);
            item->setToolTip(v.description);
            if (current) {
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
            }
            if (v.id == keep) toSelect = item;
        }
    }
    if (toSelect) m_list->setCurrentItem(toSelect);
    refreshButtons();
}

QString VoiceDialog::selectedId() const
{
    const QListWidgetItem *it = m_list ? m_list->currentItem() : nullptr;
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void VoiceDialog::refreshButtons()
{
    const QString id = selectedId();
    const bool any = !id.isEmpty();
    const bool mine = id.startsWith(QLatin1String("clone:"));
    m_use->setEnabled(any && id != m_ctrl->currentVoice());
    m_test->setEnabled(any);
    m_rename->setEnabled(mine);
    m_delete->setEnabled(mine);
    QString details;
    for (const Controller::Voice &v : m_ctrl->voices()) {
        if (v.id != id) continue;
        details = v.description;
        if (v.seconds > 0) details += tr(" · reference %1 s").arg(v.seconds, 0, 'f', 1);
    }
    m_details->setText(details);
}

void VoiceDialog::setBusy(const QString &text)
{
    m_busy->show();
    m_busyText->setText(text);
    m_dPreview->setEnabled(false);
    m_cCreate->setEnabled(false);
}

void VoiceDialog::clearBusy()
{
    m_busy->hide();
    m_dPreview->setEnabled(true);
    m_cCreate->setEnabled(m_cConsent->isChecked() && QFileInfo::exists(m_cPath->text().trimmed()));
}

void VoiceDialog::onDesignPreview()
{
    const QString desc = m_dDesc->toPlainText().trimmed();
    const QString text = m_dText->text().trimmed();
    if (desc.isEmpty() || text.isEmpty()) {
        m_busyText->setText(tr("Write a description and a sample sentence first."));
        return;
    }
    m_dReq = newReq();
    m_dReady = false;
    m_dSave->setEnabled(false);
    setBusy(tr("Generating the preview… (you will hear it when it is ready)"));
    m_ctrl->voiceDesignPreview(m_dReq, desc, text, m_dLang->currentData().toString());
}

void VoiceDialog::onDesignSave()
{
    if (!m_dReady) return;
    QString name = m_dName->text().trimmed();
    if (name.isEmpty()) name = tr("Voice %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("dd-MM HH:mm")));
    setBusy(tr("Saving the voice…"));
    m_dSave->setEnabled(false);
    m_ctrl->voiceDesignSave(m_dReq, name);
}

void VoiceDialog::onCloneCreate()
{
    const QString path = m_cPath->text().trimmed();
    if (!QFileInfo::exists(path) || !m_cConsent->isChecked()) return;
    m_cReq = newReq();
    QString name = m_cName->text().trimmed();
    if (name.isEmpty()) name = QFileInfo(path).completeBaseName();
    setBusy(tr("Analysing the recording and creating the voice…"));
    m_ctrl->voiceFromFile(m_cReq, name, path, m_cText->toPlainText().trimmed(), m_cLang->currentData().toString());
}

} // namespace gbtts
