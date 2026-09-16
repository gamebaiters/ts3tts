#pragma once

#include <QColor>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class ChannelMeter;
class ChannelSandboxDialog;
class FineSlider;

namespace gbtts {

class Controller;
class InputBox;

// Status LED with a fixed box, painted from the theme-independent state colour.
class StatusDot : public QWidget
{
    Q_OBJECT
public:
    explicit StatusDot(QWidget *parent = nullptr);
    void setColor(const QColor &c);
    QSize sizeHint() const override { return {14, 14}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QColor m_color{0x8A, 0x8F, 0x98};
};

class TtsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TtsWindow(Controller *ctrl, QWidget *parent = nullptr);
    ~TtsWindow() override;

    void showAndRaise(bool focusInput);

protected:
    void closeEvent(QCloseEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void buildUi();
    void syncFromSettings();
    void refreshState();
    // The voice section shows ONLY the active engine: its badge, its voices
    // (grouped), its languages and, for Qwen, the style field.
    void refreshEngine();
    void refreshVoices();
    void refreshJobs();
    void refreshPresetLabel();
    void refreshVc();
    void addVcTarget();
    void submit(const QString &text);
    void showNotice(const QString &text, int level);
    void openSandbox();
    void onMeterTick();
    void jobMenu(const QPoint &pos);
    void applyWindowFlags();
    void rememberGeometry(bool now);

    Controller *m_ctrl;
    bool        m_syncing = false;

    StatusDot   *m_dot = nullptr;
    QLabel      *m_status = nullptr;
    QLabel      *m_detail = nullptr;
    QPushButton *m_action = nullptr;
    QToolButton *m_btnVoices = nullptr;
    QToolButton *m_btnSettings = nullptr;

    QLabel      *m_engineBadge = nullptr;
    QToolButton *m_engineBtn = nullptr;
    QComboBox   *m_voice = nullptr;
    QToolButton *m_testVoice = nullptr;
    QComboBox   *m_lang = nullptr;
    FineSlider  *m_speed = nullptr;
    QLabel      *m_speedLbl = nullptr;
    QLineEdit   *m_instruct = nullptr;

    FineSlider   *m_remote = nullptr;
    QLabel       *m_remoteLbl = nullptr;
    FineSlider   *m_local = nullptr;
    QLabel       *m_localLbl = nullptr;
    QCheckBox    *m_monitor = nullptr;
    FineSlider   *m_pitch = nullptr;
    QLabel       *m_pitchLbl = nullptr;
    QComboBox    *m_preset = nullptr;
    QPushButton  *m_sandboxBtn = nullptr;
    ChannelMeter *m_meter = nullptr;

    QComboBox *m_micMode = nullptr;
    QCheckBox *m_preview = nullptr;
    QCheckBox *m_speakMuted = nullptr;

    QCheckBox    *m_vcEnable = nullptr;
    QComboBox    *m_vcVoice = nullptr;
    QToolButton  *m_vcAdd = nullptr;
    QComboBox    *m_vcPreset = nullptr;
    QCheckBox    *m_vcMonitor = nullptr;
    QLabel       *m_vcStatus = nullptr;
    ChannelMeter *m_vcMeter = nullptr;

    QListWidget *m_history = nullptr;
    InputBox    *m_input = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_stop = nullptr;
    QCheckBox   *m_readChat = nullptr;
    QLabel      *m_hint = nullptr;
    QLabel      *m_notice = nullptr;

    QTimer m_meterTimer;
    QTimer m_noticeTimer;
    QPointer<ChannelSandboxDialog> m_sandbox;
};

} // namespace gbtts
