#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace gbtts {

// Runs installer/install_backend.ps1 (shipped in plugins\gb_tts\backend) HIDDEN and
// follows it through <home>\install\status.json + install.log.
//
// The installer is a detached process on purpose: closing the window, closing
// TeamSpeak or unloading the plugin never interrupts a 13 GB installation, and a
// plugin started later re-attaches to it (attach()). Nothing here blocks: the
// process is polled with a timer, cancel() starts `taskkill /T /F` and returns.
class EngineInstaller : public QObject
{
    Q_OBJECT
public:
    struct Status {
        bool        valid = false;       // status.json read and parsed
        QString     state;               // running | done | error (cancelled/interrupted are set here)
        QString     mode;                // gpu | cpu
        QString     stage;               // check uv python venv torch libs code verify models finish
        QStringList stages;
        int         stageIndex = 0;
        double      overall = 0.0;       // 0..1
        double      stageFraction = -1;  // <0 = unknown
        qint64      bytesDone = 0;
        qint64      bytesTotal = 0;
        QString     item;
        QString     error;
        QString     errorCode;
        QString     gpu;
        qint64      sizeBytes = 0;
        QDateTime   started;
    };

    struct Gpu {
        QString name;                    // empty = no NVIDIA adapter
        qint64  memoryBytes = 0;
    };

    explicit EngineInstaller(QObject *parent = nullptr);
    ~EngineInstaller() override;

    // $GBTTS_INSTALLER_SCRIPT, the packaged plugins\gb_tts\backend\install_backend.ps1,
    // or installer\install_backend.ps1 next to a development backend.
    static QString scriptPath(const QString &configuredHome = QString());
    static Gpu detectNvidiaGpu();        // DXGI enumeration: no process, no driver call
    static qint64 freeBytes(const QString &path);
    static QString statusDir(const QString &home);

    bool start(const QString &home, const QString &mode, const QStringList &models, QString *error);
    // Follows an installation still running from an earlier TeamSpeak session.
    bool attach(const QString &home);
    void cancel();

    bool running() const { return m_process != nullptr; }
    bool cancelRequested() const { return m_cancelRequested; }
    QString home() const { return m_home; }
    const Status &status() const { return m_status; }
    QString logFile() const;
    QString logTail() const { return m_logTail; }

signals:
    void progress();
    void logAppended(const QString &text);
    void finished(bool ok);

private:
    void poll();
    bool readStatus();
    void readLog();
    void finish();

    QString m_home;
    void   *m_process = nullptr;
    QTimer  m_poll;
    Status  m_status;
    qint64  m_logOffset = 0;
    QString m_logTail;
    bool    m_cancelRequested = false;
};

} // namespace gbtts
