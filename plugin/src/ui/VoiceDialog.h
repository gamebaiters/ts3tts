#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;

namespace gbtts {

class Controller;

// Voice library of the ACTIVE engine: pick, preview, rename, delete. With
// Qwen3-TTS also design a new voice from a text description (listen first,
// save if you like it) or clone one from a recording you have the right to
// use; other engines have a fixed set of voices and show what they offer.
class VoiceDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VoiceDialog(Controller *ctrl, QWidget *parent = nullptr);

private:
    void buildUi();
    QWidget *buildCreatePages();
    void refreshList();
    void refreshButtons();
    void refreshEnginePanel();
    QString selectedId() const;
    void setBusy(const QString &text);
    void clearBusy();

    void onDesignPreview();
    void onDesignSave();
    void onCloneCreate();

    Controller *m_ctrl;

    QLabel      *m_title = nullptr;
    QLineEdit   *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel      *m_details = nullptr;
    QPushButton *m_use = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_rename = nullptr;
    QPushButton *m_delete = nullptr;

    QStackedWidget *m_right = nullptr;
    QLabel         *m_engineInfo = nullptr;

    QLineEdit      *m_dName = nullptr;
    QComboBox      *m_dLang = nullptr;
    QComboBox      *m_dExamples = nullptr;
    QPlainTextEdit *m_dDesc = nullptr;
    QLineEdit      *m_dText = nullptr;
    QPushButton    *m_dPreview = nullptr;
    QPushButton    *m_dSave = nullptr;
    QString         m_dReq;
    bool            m_dReady = false;

    QLineEdit      *m_cName = nullptr;
    QLineEdit      *m_cPath = nullptr;
    QPlainTextEdit *m_cText = nullptr;
    QComboBox      *m_cLang = nullptr;
    QCheckBox      *m_cConsent = nullptr;
    QPushButton    *m_cCreate = nullptr;
    QString         m_cReq;

    QProgressBar *m_busy = nullptr;
    QLabel       *m_busyText = nullptr;
};

} // namespace gbtts
