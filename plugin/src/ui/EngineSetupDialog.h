#pragma once

#include "net/EngineInstaller.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QToolButton;

namespace gbtts {

class Controller;
class ProgressBar;

// "Install the voice engine": one window, no console, nothing to prepare.
//   choose   -> what to install (complete on an NVIDIA GPU / light on the CPU) and where
//   progress -> overall bar, the ten steps, bytes and speed, optional log; the window can
//               be hidden (the installation continues, the TTS window shows the percentage)
//   result   -> done (the engine starts by itself) or what went wrong + Retry
// The installation itself is Controller::installer() - it outlives this window.
class EngineSetupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit EngineSetupDialog(Controller *ctrl, QWidget *parent = nullptr);

    // Shows the page that matches the installer: progress if one is running.
    void present();

protected:
    void showEvent(QShowEvent *e) override;

private:
    QWidget *buildChoosePage();
    QWidget *buildProgressPage();
    QWidget *buildResultPage();
    void refreshChoose();
    void refreshProgress();
    void showResult(bool ok);
    void startInstall();
    QString selectedHome() const;
    QString selectedMode() const;
    qint64 requiredBytes() const;

    Controller *m_ctrl;
    EngineInstaller::Gpu m_gpu;
    QStackedWidget *m_pages = nullptr;

    // choose
    QLabel       *m_gpuLabel = nullptr;
    QRadioButton *m_full = nullptr;
    QRadioButton *m_light = nullptr;
    QLabel       *m_fullDesc = nullptr;
    QLineEdit    *m_folder = nullptr;
    QLabel       *m_space = nullptr;
    QLabel       *m_existing = nullptr;
    QLabel       *m_startError = nullptr;
    QPushButton  *m_install = nullptr;

    // progress
    ProgressBar    *m_bar = nullptr;
    QLabel         *m_percent = nullptr;
    QLabel         *m_step = nullptr;
    QLabel         *m_detail = nullptr;
    QListWidget    *m_steps = nullptr;
    QLabel         *m_elapsed = nullptr;
    QToolButton    *m_logToggle = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton    *m_cancel = nullptr;
    QTimer          m_cancelArm;
    QTimer          m_clock;
    qint64          m_lastBytes = -1;
    qint64          m_lastBytesMs = 0;
    double          m_bytesPerSecond = 0.0;
    QString         m_lastItem;
    QElapsedTimer   m_speedClock;

    // result
    QLabel      *m_resultTitle = nullptr;
    QLabel      *m_resultText = nullptr;
    QPushButton *m_retry = nullptr;
    QPushButton *m_openLog = nullptr;
    QPushButton *m_openTts = nullptr;
};

} // namespace gbtts
