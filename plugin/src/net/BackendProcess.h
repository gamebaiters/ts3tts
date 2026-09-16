#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace gbtts {

// Launches and supervises the Python backend.
//
// Windows specifics that matter inside a TeamSpeak plugin:
//  - the process is created SUSPENDED and put into a Job Object with
//    KILL_ON_JOB_CLOSE before it runs a single instruction: if TeamSpeak
//    exits or crashes, Windows kills the backend (no orphan holding 5 GB of
//    VRAM);
//  - kill() only terminates the job and closes handles - it never waits,
//    because it runs inside ts3plugin_shutdown on the client's GUI/STA thread
//    (a blocking wait there is the Soundboard's documented crash-on-close);
//  - only the log file handle is inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST),
//    never TeamSpeak's own handles.
class BackendProcess : public QObject
{
    Q_OBJECT
public:
    struct Layout {
        QString home;
        QString python;
        QString appDir;
        QString logFile;
        bool    valid = false;
        QString problem;
    };

    explicit BackendProcess(QObject *parent = nullptr);
    ~BackendProcess() override;

    // Resolution order: configured home, then %LOCALAPPDATA%\GameBaitersTTS,
    // then the plugin's own backend folder.
    static Layout resolve(const QString &configuredHome);
    static Layout inspect(const QString &home);   // that folder only, no fallbacks
    static QString defaultHome();

    // The installer COPIES the backend code into <home>\app; a plugin update only
    // replaces plugins\gb_tts\backend. Before every start the packaged code is
    // compared with the installed one (gbtts/__init__.py __version__) and, if it
    // differs, <home>\app\gbtts is replaced (copy to gbtts.new, then two renames).
    // Never touches a developer layout (<home> holding gbtts\ directly).
    struct CodeSync {
        enum Result { NotApplicable, UpToDate, Updated, Failed };
        Result  result = NotApplicable;
        QString installed;             // version found in <home>\app before
        QString packaged;              // version shipped with the plugin
        QString detail;                // why it failed
        bool    requirementsChanged = false;   // pip requirements differ: "Install / repair" needed
    };
    // packagedDir: empty = <TS3 config>\plugins\gb_tts\backend.
    static CodeSync syncPackagedCode(const Layout &layout, const QString &packagedDir = QString());
    static QString readPythonVersion(const QString &initPy);

    bool start(const Layout &layout, quint16 port, const QString &token, QString *error);
    void kill();
    bool running() const;

signals:
    void exited(int exitCode);

private:
    void poll();

    void   *m_job = nullptr;
    void   *m_proc = nullptr;
    QTimer  m_poll;
};

} // namespace gbtts
