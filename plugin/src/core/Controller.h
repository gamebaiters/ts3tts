#pragma once

#include "core/Settings.h"
#include "dsp/SandboxState.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <memory>

namespace gbtts {

class TtsAudio;
class BackendLink;
class EngineInstaller;
class BackendProcess;
class TalkState;

// The brain. Lives on the TeamSpeak GUI thread; owns the audio core, the
// backend process + link and the talk-state override, and exposes a small
// model the widgets bind to. Widgets never talk to the backend directly.
//
// Engine model (v1.2): exactly ONE engine runs, chosen in Settings
// (setEngine). The backend loads only that engine and lists only its voices;
// every voice list in the UI shows only those.
class Controller : public QObject
{
    Q_OBJECT
public:
    enum class Backend { NotInstalled, Installing, Stopped, Starting, Loading, Ready, Busy, Error };

    struct Voice {
        QString id, name, kind, lang, gender, engine, description;
        QString source;   // Qwen library voices: stock (shipped Italian) | design | file
        double  seconds = 0.0;
    };

    enum JobState { Queued, Speaking, Done, Cancelled, Failed };
    struct Job {
        uint32_t  id = 0;
        QString   text;
        QString   voiceName;
        bool      local = false;
        JobState  state = Queued;
        double    ttfaMs = 0.0;
        int       audioMs = 0;
        QDateTime when;
        QString   error;
        // Waiting for the engine: created at Enter, sent when the engine is ready.
        bool      sent = false;
        QString   voiceOverride;
        uint64_t  server = 0;
    };

    struct Preset {
        QString      name;
        SandboxState state;
        float        pitch = 0.0f;
    };

    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    void startup();
    void teardown();

    Settings &settings() { return m_settings; }
    // Persistence: saveNow() for discrete choices, scheduleSave() for continuous
    // controls (flushed 300 ms after the last change, and by saveNow()).
    void saveNow();
    void scheduleSave();
    void saveSettings() { saveNow(); }
    TtsAudio *audio() const { return m_audio.get(); }

    // ---- engines -------------------------------------------------------------
    static QString engineLabel(const QString &key);
    QString engine() const { return m_settings.engine; }
    void setEngine(const QString &key);
    bool enginesKnown() const { return !m_engines.isEmpty(); }
    bool engineAvailable(const QString &key) const;
    QString engineUnavailableReason(const QString &key) const;
    static QStringList engineLanguages(const QString &key);

    // ---- backend -------------------------------------------------------------
    Backend state() const { return m_state; }
    QString stateMessage() const { return m_stateMessage; }
    QJsonObject backendInfo() const { return m_info; }
    QString resolvedHome() const;
    QString backendProblem() const;
    QString logFile() const;
    void startBackend();
    void restartBackend();
    void stopBackend();
    void applyBackendSettings();
    void unloadModel();

    // ---- engine installation (GUI in EngineSetupDialog) ---------------------------
    EngineInstaller *installer() const { return m_installer.get(); }
    bool installEngine(const QString &home, const QString &mode, QString *error);
    // Step names for the UI: check uv python venv torch libs code verify models finish.
    static QString installStageLabel(const QString &stage, const QString &mode);

    // ---- speaking ----------------------------------------------------------------
    uint32_t speak(const QString &text, bool local = false, const QString &voiceOverride = QString());
    void stopAll();
    void repeatLast();
    int  pendingJobs() const;
    const QList<Job> &jobs() const { return m_jobs; }
    bool speaking() const { return m_speaking; }
    double lastTtfaMs() const { return m_lastTtfa; }

    // ---- voices (active engine only) ---------------------------------------------------
    const QList<Voice> &voices() const { return m_voices; }
    bool voicesLoaded() const { return m_voicesEngine == m_settings.engine; }
    QString currentVoice() const { return m_settings.currentVoice(); }
    QString voiceName(const QString &id) const;
    static QString engineOfVoice(const QString &voiceId) { return Settings::engineOfVoice(voiceId); }
    void setVoice(const QString &id);
    void testVoice(const QString &id);
    void voiceFromFile(const QString &req, const QString &name, const QString &path,
                       const QString &refText, const QString &lang);
    uint32_t voiceDesignPreview(const QString &req, const QString &instruct, const QString &text,
                                const QString &lang);
    void voiceDesignSave(const QString &req, const QString &name);
    void deleteVoice(const QString &id);
    void renameVoice(const QString &id, const QString &name);

    // ---- audio / voice parameters (apply + persist) -------------------------------------
    void setRemoteDb(double db);
    void setLocalDb(double db);
    void setVoiceDb(double db);
    void setPitch(double st);
    void setMonitor(bool on);
    void setMicMode(int mode);
    void setPreviewOnly(bool on);
    void setSpeakWhenMuted(bool on);
    void setReadChannelChat(bool on);
    void setSpeed(double speed);
    void setLang(const QString &lang);
    void setInstruct(const QString &instruct);
    void setJitterMs(int ms);
    SandboxState sandbox() const { return m_sandbox; }
    void setSandbox(const SandboxState &s);
    static QVector<Preset> presets();
    void applyPreset(int index);

    // ---- real-time voice changer ---------------------------------------------------------
    struct VcTarget {
        QString id, name, source;
        double  seconds = 0.0;
    };
    enum class VcState { Off, Downloading, Loading, Ready, Error };
    VcState vcState() const { return m_vcState; }
    QString vcMessage() const { return m_vcMessage; }
    int vcLatencyMs() const { return m_vcLatencyMs; }
    const QList<VcTarget> &vcTargets() const { return m_vcTargets; }
    void setVcEnabled(bool on);
    void setVcVoice(const QString &id);
    void setVcPreset(const QString &preset);
    void setVcMonitor(bool on);
    void requestVcTargets();
    void addVcTargetFromFile(const QString &req, const QString &name, const QString &path);

    // ---- TeamSpeak events --------------------------------------------------------------
    void onConnectStatus(uint64_t sch, int status);
    void onCurrentServer(uint64_t sch);
    void onClientStopsTalking(uint64_t sch);
    // The user wrote `message` in the channel chat of `sch` (already filtered: own, channel).
    void onOwnChannelMessage(uint64_t sch, const QString &message);
    uint64_t targetServer() const;
    bool connected() const;

signals:
    void stateChanged();
    void engineChanged(const QString &key);
    void enginesChanged();
    void voicesChanged();
    void jobsChanged();
    void speakingChanged(bool speaking);
    void voiceOpResult(const QString &req, bool ok, const QString &message);
    void designReady(const QString &req, double seconds);
    void notice(const QString &text, int level);   // 0 info, 1 warning, 2 error
    void settingsApplied();
    void vcChanged();
    void vcTargetsChanged();
    void vcTargetAdded(const QString &req, bool ok, const QString &message);

private:
    void setState(Backend s, const QString &message);
    void onInstallerProgress();
    void onInstallerFinished(bool ok);
    void onLinkConnected();
    void onLinkDisconnected();
    void onMessage(const QJsonObject &m);
    void onProcessExited(int code);
    void onTick();
    void sendConfigure();
    void pickDefaultVoice();
    QString defaultVoiceFor(const QString &engine) const;
    Job *findJob(uint32_t id);
    void pruneJobs();
    void sendChatEcho(uint64_t sch, const QString &text);
    uint32_t speakImpl(const QString &text, bool local, const QString &voiceOverride, uint64_t server, bool fromChat);
    bool sendJob(Job &job);
    void flushPendingJobs();
    void pushAudioSettings();
    void sendVcConfigure();
    void setVcState(VcState s, const QString &message);

    Settings m_settings;
    std::unique_ptr<TtsAudio>       m_audio;
    std::unique_ptr<BackendLink>    m_link;
    std::unique_ptr<BackendProcess> m_process;
    std::unique_ptr<TalkState>      m_talk;
    std::unique_ptr<EngineInstaller> m_installer;

    Backend      m_state = Backend::Stopped;
    QString      m_stateMessage;
    QJsonObject  m_info;
    QJsonObject  m_engines;
    QList<Voice> m_voices;
    QString      m_voicesEngine;
    QList<Job>   m_jobs;
    SandboxState m_sandbox;

    uint32_t m_nextJob = 1;
    bool     m_speaking = false;
    double   m_lastTtfa = 0.0;
    qint64   m_lastRemoteActivityMs = 0;
    uint64_t m_server = 0;
    int      m_restartCount = 0;
    qint64   m_restartWindowStart = 0;
    bool     m_tearingDown = false;
    bool     m_wantBackend = false;
    bool     m_requirementsNoticeShown = false;
    QTimer   m_tick;
    QTimer   m_restartTimer;
    QTimer   m_applyTimer;
    QTimer   m_memoryRestartTimer;
    QTimer   m_saveTimer;

    VcState         m_vcState = VcState::Off;
    QString         m_vcMessage;
    int             m_vcLatencyMs = 0;
    QList<VcTarget> m_vcTargets;
    // "Also write in the channel chat" echoes we sent (time, text without BBCode): when the
    // server delivers them back as our own channel message they must not be read twice.
    QList<QPair<qint64, QString>> m_recentEchoes;
};

} // namespace gbtts
