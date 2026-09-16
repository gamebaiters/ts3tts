#pragma once

#include <QDialog>
#include <QFrame>
#include <QMap>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class FineSlider;

namespace gbtts {

class Controller;

// A card that forwards clicks anywhere on it to its radio button.
class EngineCard : public QFrame
{
    Q_OBJECT
public:
    EngineCard(const QString &key, QWidget *parent = nullptr);

    QString       key;
    QRadioButton *radio = nullptr;
    QLabel       *status = nullptr;
    QLabel       *resources = nullptr;

    void setSelected(bool on);

protected:
    void mouseReleaseEvent(QMouseEvent *e) override;
};

// All preferences. Every control applies AND saves immediately (continuous
// controls debounced inside the controller); nothing waits for an OK button.
//
// The engine is chosen HERE and only here: one card per engine with what it
// costs, then the options of the selected engine only.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Controller *ctrl, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private:
    void buildUi();
    QWidget *buildEngineSection(QWidget *page);
    QWidget *buildQwenPage();
    QWidget *buildKokoroPage();
    QWidget *buildSupertonicPage();
    void load();
    void refreshEngines();
    void refreshBackendStatus();
    void pushEngineOptions();
    void saveDictionary();
    void installBackend();

    Controller *m_ctrl;
    bool        m_loading = false;

    QButtonGroup               *m_engineGroup = nullptr;
    QMap<QString, EngineCard *> m_cards;
    QStackedWidget             *m_enginePages = nullptr;
    QLabel                     *m_engineHint = nullptr;

    QComboBox  *m_size = nullptr;
    QSpinBox   *m_chunk = nullptr;
    QLabel     *m_chunkHint = nullptr;
    QCheckBox  *m_prefill = nullptr;
    FineSlider *m_temp = nullptr;
    QLabel     *m_tempLbl = nullptr;
    QSpinBox   *m_idle = nullptr;
    QComboBox  *m_kokoroVariant = nullptr;
    QSpinBox   *m_steps = nullptr;

    QLineEdit *m_home = nullptr;
    QLabel    *m_homeStatus = nullptr;
    QCheckBox *m_autostart = nullptr;

    FineSlider *m_voiceGain = nullptr;
    QLabel     *m_voiceGainLbl = nullptr;
    QSpinBox   *m_jitter = nullptr;
    QCheckBox  *m_leveler = nullptr;

    QCheckBox    *m_numbers = nullptr;
    QCheckBox    *m_slang = nullptr;
    QCheckBox    *m_echo = nullptr;
    QLineEdit    *m_prefix = nullptr;
    QTableWidget *m_dict = nullptr;

    QCheckBox *m_toolbar = nullptr;
    QCheckBox *m_clear = nullptr;
    QCheckBox *m_onTop = nullptr;
    QComboBox *m_uiLang = nullptr;
    QCheckBox *m_autoUpdate = nullptr;
};

} // namespace gbtts
