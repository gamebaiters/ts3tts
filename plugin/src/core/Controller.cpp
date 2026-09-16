#include "core/Controller.h"

#include "audio/TtsAudio.h"
#include "net/BackendLink.h"
#include "net/BackendProcess.h"
#include "net/EngineInstaller.h"
#include "ts3/TalkState.h"

#include "common.h"
#include "ts3log.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <functional>

namespace gbtts {

namespace {

constexpr int kMaxText = 2000;
constexpr int kMaxJobs = 80;
constexpr int kSaveDebounceMs = 300;

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// TeamSpeak chat text is BBCode ([b], [URL=...], [color=...]): speak the words, not the tags.
QString stripBbcode(const QString &message)
{
    static const QRegularExpression tag(QStringLiteral("\\[/?[A-Za-z*]+(=[^\\]]*)?\\]"));
    QString text = message;
    text.remove(tag);
    return text.simplified();
}

QString sampleSentence(const QString &lang)
{
    if (lang == QLatin1String("en")) return QStringLiteral("Hi everyone, this is my voice. Can you hear me clearly?");
    if (lang == QLatin1String("es")) return QStringLiteral("Hola a todos, esta es mi voz. ¿Me oís bien?");
    if (lang == QLatin1String("fr")) return QStringLiteral("Salut tout le monde, voici ma voix. Vous m'entendez bien ?");
    if (lang == QLatin1String("de")) return QStringLiteral("Hallo zusammen, das ist meine Stimme. Hört ihr mich gut?");
    if (lang == QLatin1String("pt")) return QStringLiteral("Olá a todos, esta é a minha voz. Vocês me ouvem bem?");
    return QStringLiteral("Ciao a tutti, questa è la mia voce. Mi sentite bene?");
}

} // namespace

// ===========================================================================
Controller::Controller(QObject *parent) : QObject(parent)
{
    m_settings.load();
    m_audio = std::make_unique<TtsAudio>();
    m_link = std::make_unique<BackendLink>(m_audio.get());
    m_process = std::make_unique<BackendProcess>();
    m_talk = std::make_unique<TalkState>(m_audio.get());
    m_installer = std::make_unique<EngineInstaller>();
    connect(m_installer.get(), &EngineInstaller::progress, this, &Controller::onInstallerProgress);
    connect(m_installer.get(), &EngineInstaller::finished, this, &Controller::onInstallerFinished);

    connect(m_link.get(), &BackendLink::connected, this, &Controller::onLinkConnected);
    connect(m_link.get(), &BackendLink::disconnected, this, &Controller::onLinkDisconnected);
    connect(m_link.get(), &BackendLink::message, this, &Controller::onMessage);
    connect(m_process.get(), &BackendProcess::exited, this, &Controller::onProcessExited);

    m_tick.setInterval(20);
    connect(&m_tick, &QTimer::timeout, this, &Controller::onTick);

    m_restartTimer.setSingleShot(true);
    connect(&m_restartTimer, &QTimer::timeout, this, &Controller::startBackend);

    m_memoryRestartTimer.setSingleShot(true);
    m_memoryRestartTimer.setInterval(1500);
    connect(&m_memoryRestartTimer, &QTimer::timeout, this, [this] {
        if (m_tearingDown || m_settings.engine == QLatin1String("qwen") || !m_link->isConnected()) return;
        if (pendingJobs() > 0 || m_audio->anyActive()) {
            m_memoryRestartTimer.start();   // never cut a message: retry when idle
            return;
        }
        logInfo("GBTTS: restarting the engine to release PyTorch memory (engine %s)",
                m_settings.engine.toUtf8().constData());
        emit notice(tr("Restarting the voice engine to free the memory used by Qwen…"), 0);
        restartBackend();
    });

    // Engine options are debounced: dragging a slider in the settings dialog
    // must not reconfigure the backend ten times.
    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(500);
    connect(&m_applyTimer, &QTimer::timeout, this, [this] {
        if (m_link->isConnected()) m_link->send(m_settings.backendConfig());
    });

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &Controller::saveNow);
}

Controller::~Controller()
{
    teardown();
}

void Controller::startup()
{
    if (!m_settings.sandboxJson.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(m_settings.sandboxJson);
        if (doc.isObject()) m_sandbox = SandboxState::fromJson(doc.object());
    }
    pushAudioSettings();
    m_audio->start();

    if (ts3Functions.getCurrentServerConnectionHandlerID) m_server = ts3Functions.getCurrentServerConnectionHandlerID();
    m_audio->setActiveServer(m_server);
    m_tick.start();

    // First run after an update: persist the migrated layout (per-engine voices).
    saveNow();

    // An installation started in an earlier TeamSpeak session may still be running.
    for (const QString &home : {m_settings.backendHome, BackendProcess::defaultHome()}) {
        if (m_installer->attach(home)) {
            onInstallerProgress();
            return;
        }
    }

    const BackendProcess::Layout layout = BackendProcess::resolve(m_settings.backendHome);
    if (!layout.valid) {
        setState(Backend::NotInstalled, tr("The voice engine is not installed yet."));
    } else if (m_settings.autostart) {
        startBackend();
    } else {
        setState(Backend::Stopped, tr("Voice engine stopped."));
    }
}

void Controller::teardown()
{
    if (m_tearingDown) return;
    m_tearingDown = true;
    m_tick.stop();
    m_restartTimer.stop();
    m_applyTimer.stop();
    m_memoryRestartTimer.stop();
    m_saveTimer.stop();

    if (m_talk && m_talk->active()) {
        int status = STATUS_DISCONNECTED;
        const uint64_t sch = m_talk->server();
        if (ts3Functions.getConnectionStatus && ts3Functions.getConnectionStatus(sch, &status) == ERROR_ok &&
            status == STATUS_CONNECTION_ESTABLISHED)
            m_talk->end();
        else
            m_talk->onConnectionLost(0);
    }
    if (m_link && m_link->isConnected()) m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("shutdown")}});
    if (m_process) m_process->kill();
    if (m_link) m_link->shutdown();
    if (m_audio) m_audio->stop();
    if (!m_settings.save()) logWarning("GBTTS: could not write the settings on shutdown");
}

void Controller::saveNow()
{
    m_saveTimer.stop();
    if (!m_settings.save()) logWarning("GBTTS: could not write the settings to the registry");
}

void Controller::scheduleSave()
{
    if (!m_tearingDown) m_saveTimer.start();
}

// ===========================================================================
// Engines
// ===========================================================================
QString Controller::engineLabel(const QString &key)
{
    if (key == QLatin1String("kokoro")) return QStringLiteral("Kokoro");
    if (key == QLatin1String("supertonic")) return QStringLiteral("Supertonic 3");
    return QStringLiteral("Qwen3-TTS");
}

QStringList Controller::engineLanguages(const QString &key)
{
    // Languages each engine actually pronounces well ("auto" = Italian/English detection).
    if (key == QLatin1String("kokoro"))
        return {QStringLiteral("it"), QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("es"),
                QStringLiteral("fr"), QStringLiteral("pt")};
    if (key == QLatin1String("supertonic"))
        return {QStringLiteral("it"), QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("es"),
                QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("pt"), QStringLiteral("ru"),
                QStringLiteral("ja"), QStringLiteral("ko")};
    return {QStringLiteral("it"), QStringLiteral("auto"), QStringLiteral("en"), QStringLiteral("es"),
            QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("pt"), QStringLiteral("ru"),
            QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("zh")};
}

bool Controller::engineAvailable(const QString &key) const
{
    if (m_engines.isEmpty()) return true;   // not reported yet: assume yes
    const QJsonValue v = m_engines.value(key);
    return v.isObject() && v.toObject().value(QStringLiteral("available")).toBool();
}

QString Controller::engineUnavailableReason(const QString &key) const
{
    return m_engines.value(key).toObject().value(QStringLiteral("reason")).toString();
}

void Controller::setEngine(const QString &key)
{
    if (m_tearingDown || !Settings::engineKeys().contains(key) || key == m_settings.engine) return;
    const bool torchLoaded = m_info.value(QStringLiteral("torch_loaded")).toBool();
    logInfo("GBTTS: engine %s -> %s", m_settings.engine.toUtf8().constData(), key.toUtf8().constData());

    // A message of the old engine must not keep the GPU busy after the switch.
    stopAll();
    m_settings.engine = key;
    if (!engineLanguages(key).contains(m_settings.lang)) m_settings.lang = QStringLiteral("it");
    m_voices.clear();
    m_voicesEngine.clear();
    m_memoryRestartTimer.stop();
    m_applyTimer.stop();
    saveNow();

    emit engineChanged(key);
    emit voicesChanged();
    emit settingsApplied();

    if (!m_link->isConnected()) return;   // the next hello configures the new engine
    if (torchLoaded && key != QLatin1String("qwen")) {
        // PyTorch never returns its ~2 GB of RAM and the CUDA context: a fresh
        // process is the only way the light engine really runs light.
        restartBackend();
    } else {
        setState(Backend::Loading, tr("Loading %1…").arg(engineLabel(key)));
        m_link->send(m_settings.backendConfig());
    }
}

// ===========================================================================
// Backend lifecycle
// ===========================================================================
void Controller::setState(Backend s, const QString &message)
{
    m_state = s;
    m_stateMessage = message;
    emit stateChanged();
}

QString Controller::resolvedHome() const
{
    return BackendProcess::resolve(m_settings.backendHome).home;
}

QString Controller::backendProblem() const
{
    return BackendProcess::resolve(m_settings.backendHome).problem;
}

QString Controller::logFile() const
{
    return BackendProcess::resolve(m_settings.backendHome).logFile;
}

bool Controller::installEngine(const QString &home, const QString &mode, QString *error)
{
    if (m_tearingDown) return false;
    // The installer replaces app\ and may recreate venv\: nothing of ours may run from there.
    if (m_process->running() || m_link->isConnected()) stopBackend();
    QStringList models;   // default set chosen by the installer from the mode
    if (!m_installer->start(home, mode, models, error)) return false;
    m_settings.backendHome = QDir::toNativeSeparators(home);
    saveNow();
    onInstallerProgress();
    return true;
}

QString Controller::installStageLabel(const QString &stage, const QString &mode)
{
    if (stage == QLatin1String("check")) return tr("Checking the system");
    if (stage == QLatin1String("uv")) return tr("Installation tools");
    if (stage == QLatin1String("python")) return tr("Python 3.12");
    if (stage == QLatin1String("venv")) return tr("Engine environment");
    if (stage == QLatin1String("torch"))
        return mode == QLatin1String("gpu") ? tr("PyTorch for NVIDIA graphics cards") : tr("PyTorch for the processor");
    if (stage == QLatin1String("libs")) return tr("Voice engine libraries");
    if (stage == QLatin1String("code")) return tr("Voice engine code");
    if (stage == QLatin1String("verify")) return tr("Verification");
    if (stage == QLatin1String("models")) return tr("Voice models");
    if (stage == QLatin1String("finish")) return tr("Finishing");
    return stage;
}

void Controller::onInstallerProgress()
{
    if (m_tearingDown || !m_installer->running()) return;
    const EngineInstaller::Status &s = m_installer->status();
    QString msg = tr("%1% — %2").arg(int(s.overall * 100)).arg(installStageLabel(s.stage.isEmpty() ? QStringLiteral("check") : s.stage, s.mode));
    if (m_installer->cancelRequested()) msg = tr("cancelling…");
    if (m_state != Backend::Installing || m_stateMessage != msg) setState(Backend::Installing, msg);
}

void Controller::onInstallerFinished(bool ok)
{
    if (m_tearingDown) return;
    const EngineInstaller::Status &s = m_installer->status();
    if (!ok) {
        const bool cancelled = s.errorCode == QLatin1String("cancelled");
        setState(Backend::NotInstalled, cancelled ? tr("Installation cancelled.") : tr("Installation failed: %1").arg(s.error));
        return;
    }
    m_settings.backendHome = QDir::toNativeSeparators(m_installer->home());
    // Without the Qwen models (light installation) the Kokoro engine is the one that works.
    if (s.mode == QLatin1String("cpu") && m_settings.engine == QLatin1String("qwen")) {
        m_settings.engine = QStringLiteral("kokoro");
        emit engineChanged(m_settings.engine);
        emit voicesChanged();
    }
    saveNow();
    emit settingsApplied();
    emit notice(tr("Voice engine installed: starting it…"), 0);
    m_restartCount = 0;
    m_wantBackend = true;
    startBackend();
}

void Controller::startBackend()
{
    if (m_tearingDown) return;
    if (m_installer->running()) {
        onInstallerProgress();   // started automatically when the installation ends
        return;
    }
    m_wantBackend = true;
    const BackendProcess::Layout layout = BackendProcess::resolve(m_settings.backendHome);
    if (!layout.valid) {
        setState(Backend::NotInstalled, tr("The voice engine is not installed yet."));
        return;
    }
    if (m_process->running() && m_link->isConnected()) return;
    // A plugin update ships new backend code in plugins\gb_tts\backend: install it into
    // <home>\app before the process starts (start() would kill a leftover one anyway).
    m_process->kill();
    const BackendProcess::CodeSync sync = BackendProcess::syncPackagedCode(layout);
    if (sync.result == BackendProcess::CodeSync::Updated) {
        logInfo("GBTTS: voice engine code updated %s -> %s", sync.installed.toUtf8().constData(),
                sync.packaged.toUtf8().constData());
        emit notice(tr("Voice engine updated to version %1 together with the plugin.").arg(sync.packaged), 0);
    } else if (sync.result == BackendProcess::CodeSync::Failed) {
        logWarning("GBTTS: could not update the voice engine code: %s", sync.detail.toUtf8().constData());
        emit notice(tr("Could not update the voice engine (%1): open Settings and press “Install / repair the engine…”.")
                        .arg(sync.detail), 1);
    }
    if (sync.requirementsChanged && !m_requirementsNoticeShown) {
        m_requirementsNoticeShown = true;
        logWarning("GBTTS: the packaged backend needs different Python libraries than the installed ones");
        emit notice(tr("This version needs updated voice engine libraries: open Settings and press “Install / repair "
                       "the engine…”."), 1);
    }
    const quint16 port = m_link->start();
    if (port == 0) {
        setState(Backend::Error, tr("Cannot open the local connection for the voice engine."));
        return;
    }
    QString err;
    if (!m_process->start(layout, port, m_link->token(), &err)) {
        setState(Backend::Error, tr("Cannot start the voice engine: %1").arg(err));
        return;
    }
    setState(Backend::Starting, tr("Starting the voice engine…"));
}

void Controller::restartBackend()
{
    m_restartCount = 0;
    stopBackend();
    m_wantBackend = true;
    startBackend();
}

void Controller::stopBackend()
{
    m_wantBackend = false;
    m_restartTimer.stop();
    m_memoryRestartTimer.stop();
    if (m_link->isConnected()) m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("shutdown")}});
    // Forget the old connection BEFORE killing its process: its late
    // "disconnected" must not be mistaken for a crash of the next backend.
    m_link->dropPeer();
    m_process->kill();
    m_audio->setVcEnabled(false);
    setVcState(VcState::Off, QString());
    for (Job &j : m_jobs) {
        if (j.state == Queued || j.state == Speaking) j.state = Cancelled;
    }
    m_audio->flush(m_nextJob);
    emit jobsChanged();
    if (m_installer->running()) {
        onInstallerProgress();
        return;
    }
    const bool installed = BackendProcess::resolve(m_settings.backendHome).valid;
    setState(installed ? Backend::Stopped : Backend::NotInstalled,
             installed ? tr("Voice engine stopped.") : tr("The voice engine is not installed yet."));
}

void Controller::applyBackendSettings()
{
    // Options of the running engine (model size, steps, text rules...). The
    // engine itself changes only through setEngine().
    saveNow();
    m_applyTimer.start();
    emit settingsApplied();
}

void Controller::unloadModel()
{
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("unload")}});
}

void Controller::onLinkConnected()
{
    // Authenticated; the hello message itself arrives right after.
}

void Controller::onLinkDisconnected()
{
    if (m_tearingDown) return;
    m_audio->setVcEnabled(false);          // never keep the mic silenced without a converter
    setVcState(VcState::Off, QString());
    for (Job &j : m_jobs) {
        if (j.state == Queued || j.state == Speaking) {
            j.state = Failed;
            j.error = tr("voice engine disconnected");
        }
    }
    emit jobsChanged();
    if (!m_wantBackend) return;
    if (m_process->running()) {
        m_process->kill();
        return;    // onProcessExited schedules the restart
    }
    onProcessExited(-1);
}

void Controller::onProcessExited(int code)
{
    if (m_tearingDown || !m_wantBackend || m_restartTimer.isActive()) return;
    const qint64 now = nowMs();
    if (now - m_restartWindowStart > 120000) {
        m_restartWindowStart = now;
        m_restartCount = 0;
    }
    ++m_restartCount;
    if (m_restartCount > 3) {
        for (Job &j : m_jobs) {
            if (!j.sent && j.state == Queued) {
                j.state = Failed;
                j.error = tr("voice engine disconnected");
            }
        }
        emit jobsChanged();
        setState(Backend::Error, tr("The voice engine stopped repeatedly (exit code %1). Open the log from Settings.").arg(code));
        return;
    }
    setState(Backend::Starting, tr("Voice engine stopped (exit code %1), restarting…").arg(code));
    m_restartTimer.start(1500);
}

void Controller::onMessage(const QJsonObject &m)
{
    if (m_tearingDown) return;
    const QString ev = m.value(QStringLiteral("ev")).toString();

    if (ev == QLatin1String("hello")) {
        const QString backendVersion = m.value(QStringLiteral("version")).toString();
        logInfo("GBTTS: backend %s connected", backendVersion.toUtf8().constData());
        if (backendVersion != QLatin1String(GBTTS_VERSION)) {
            logWarning("GBTTS: backend %s differs from plugin %s", backendVersion.toUtf8().constData(), GBTTS_VERSION);
            emit notice(tr("The voice engine (%1) and the plugin (%2) have different versions: if something does not "
                           "work, open Settings and press “Install / repair the engine…”.")
                            .arg(backendVersion, QStringLiteral(GBTTS_VERSION)), 1);
        }
        setState(Backend::Loading, tr("Loading %1…").arg(engineLabel(m_settings.engine)));
        m_link->send(m_settings.backendConfig());
        requestVcTargets();
        sendVcConfigure();
        return;
    }
    if (ev == QLatin1String("status")) {
        m_info = m;
        const QString st = m.value(QStringLiteral("state")).toString();
        const QString msg = m.value(QStringLiteral("message")).toString();
        Backend s = m_state;
        if (st == QLatin1String("starting") || st == QLatin1String("loading")) s = Backend::Loading;
        else if (st == QLatin1String("ready") || st == QLatin1String("idle")) s = Backend::Ready;
        else if (st == QLatin1String("busy")) s = Backend::Busy;
        else if (st == QLatin1String("error")) s = Backend::Error;
        else if (st == QLatin1String("warning")) emit notice(msg, 1);
        if (s == Backend::Ready) m_restartCount = 0;
        setState(s, msg);
        if (s == Backend::Ready) flushPendingJobs();
        // A backend that ever loaded Qwen keeps PyTorch (~2 GB RAM + CUDA
        // context) even after switching to Kokoro/Supertonic. Restart it once,
        // when idle, so choosing a light engine really frees the memory.
        if (s == Backend::Ready && m.value(QStringLiteral("torch_loaded")).toBool() &&
            m_settings.engine != QLatin1String("qwen") &&
            m.value(QStringLiteral("selected_engine")).toString() == m_settings.engine)
            m_memoryRestartTimer.start();
        return;
    }
    if (ev == QLatin1String("engines")) {
        m_engines = QJsonObject();
        for (const QJsonValue &v : m.value(QStringLiteral("engines")).toArray()) {
            const QJsonObject o = v.toObject();
            m_engines.insert(o.value(QStringLiteral("key")).toString(), o);
        }
        const QString selected = m_settings.engine;
        if (!engineAvailable(selected)) {
            // Same order the backend falls back in: lightest first.
            QString fallback;
            for (const QString &k : {QStringLiteral("kokoro"), QStringLiteral("supertonic"), QStringLiteral("qwen")}) {
                if (k != selected && engineAvailable(k)) {
                    fallback = k;
                    break;
                }
            }
            if (!fallback.isEmpty()) {
                emit notice(tr("%1 cannot run on this PC (%2): using %3. You can change it in Settings.")
                                .arg(engineLabel(selected), engineUnavailableReason(selected), engineLabel(fallback)), 1);
                m_settings.engine = fallback;
                if (!engineLanguages(fallback).contains(m_settings.lang)) m_settings.lang = QStringLiteral("it");
                m_voices.clear();
                m_voicesEngine.clear();
                saveNow();
                emit engineChanged(fallback);
                emit voicesChanged();
                emit settingsApplied();
                m_link->send(m_settings.backendConfig());
            }
        }
        emit enginesChanged();
        emit stateChanged();
        return;
    }
    if (ev == QLatin1String("voices")) {
        // Voices of an engine we already left (a reply still in flight) are ignored.
        const QString engine = m.value(QStringLiteral("engine")).toString();
        if (!engine.isEmpty() && engine != m_settings.engine) return;
        m_voices.clear();
        for (const QJsonValue &v : m.value(QStringLiteral("voices")).toArray()) {
            const QJsonObject o = v.toObject();
            Voice voice;
            voice.id = o.value(QStringLiteral("id")).toString();
            voice.name = o.value(QStringLiteral("name")).toString();
            voice.kind = o.value(QStringLiteral("kind")).toString();
            voice.lang = o.value(QStringLiteral("lang")).toString();
            voice.gender = o.value(QStringLiteral("gender")).toString();
            voice.engine = o.value(QStringLiteral("engine")).toString();
            voice.description = o.value(QStringLiteral("description")).toString();
            voice.source = o.value(QStringLiteral("source")).toString();
            voice.seconds = o.value(QStringLiteral("seconds")).toDouble();
            if (!voice.id.isEmpty() && engineOfVoice(voice.id) == m_settings.engine) m_voices.append(voice);
        }
        m_voicesEngine = m_settings.engine;
        pickDefaultVoice();
        emit voicesChanged();
        flushPendingJobs();          // messages typed while the engine was loading
        return;
    }

    const uint32_t id = static_cast<uint32_t>(m.value(QStringLiteral("id")).toDouble());
    if (ev == QLatin1String("job_start")) {
        if (Job *j = findJob(id)) {
            j->state = Speaking;
            emit jobsChanged();
        }
    } else if (ev == QLatin1String("job_first_audio")) {
        if (Job *j = findJob(id)) {
            j->ttfaMs = m.value(QStringLiteral("ttfa_ms")).toDouble();
            m_lastTtfa = j->ttfaMs;
            emit jobsChanged();
            emit stateChanged();
        }
    } else if (ev == QLatin1String("job_end")) {
        if (Job *j = findJob(id)) {
            if (j->state != Cancelled && j->state != Failed)
                j->state = m.value(QStringLiteral("cancelled")).toBool() ? Cancelled : Done;
            j->audioMs = m.value(QStringLiteral("audio_ms")).toInt();
            emit jobsChanged();
        }
        m_link->forgetJob(id);
    } else if (ev == QLatin1String("job_error")) {
        const QString err = m.value(QStringLiteral("message")).toString();
        if (Job *j = findJob(id)) {
            j->state = Failed;
            j->error = err;
            emit jobsChanged();
        }
        m_audio->pushEnd(id, true);
        m_link->forgetJob(id);
        emit notice(tr("Could not speak the message: %1").arg(err), 2);
    } else if (ev == QLatin1String("design_ready")) {
        emit designReady(m.value(QStringLiteral("req")).toString(), m.value(QStringLiteral("seconds")).toDouble());
    } else if (ev == QLatin1String("voice_created")) {
        const QJsonObject v = m.value(QStringLiteral("voice")).toObject();
        emit voiceOpResult(m.value(QStringLiteral("req")).toString(), true, v.value(QStringLiteral("name")).toString());
        const QString vid = v.value(QStringLiteral("id")).toString();
        if (!vid.isEmpty() && engineOfVoice(vid) == m_settings.engine) {
            m_settings.voices.insert(m_settings.engine, vid);
            saveNow();
            emit voicesChanged();
        }
    } else if (ev == QLatin1String("vc_status")) {
        const QString st = m.value(QStringLiteral("state")).toString();
        const QString msg = m.value(QStringLiteral("message")).toString();
        VcState s = VcState::Off;
        if (st == QLatin1String("downloading")) s = VcState::Downloading;
        else if (st == QLatin1String("loading")) s = VcState::Loading;
        else if (st == QLatin1String("ready")) s = VcState::Ready;
        else if (st == QLatin1String("error")) s = VcState::Error;
        if (s == VcState::Ready) m_vcLatencyMs = m.value(QStringLiteral("latency_ms")).toInt();
        // The microphone is replaced only once the converter really runs: while it
        // loads the user keeps a normal microphone instead of silence.
        m_audio->setVcEnabled(s == VcState::Ready && m_settings.vcEnabled);
        setVcState(s, msg);
        if (s == VcState::Error) emit notice(msg, 2);
    } else if (ev == QLatin1String("vc_targets")) {
        m_vcTargets.clear();
        for (const QJsonValue &v : m.value(QStringLiteral("voices")).toArray()) {
            const QJsonObject o = v.toObject();
            VcTarget t;
            t.id = o.value(QStringLiteral("id")).toString();
            t.name = o.value(QStringLiteral("name")).toString();
            t.source = o.value(QStringLiteral("source")).toString();
            t.seconds = o.value(QStringLiteral("seconds")).toDouble();
            if (!t.id.isEmpty()) m_vcTargets.append(t);
        }
        bool known = false;
        for (const VcTarget &t : m_vcTargets) known |= t.id == m_settings.vcVoice;
        if (!known && !m_vcTargets.isEmpty()) {
            m_settings.vcVoice = m_vcTargets.first().id;
            saveNow();
            if (m_settings.vcEnabled) sendVcConfigure();
        }
        emit vcTargetsChanged();
    } else if (ev == QLatin1String("vc_target_added")) {
        const QJsonObject v = m.value(QStringLiteral("voice")).toObject();
        emit vcTargetAdded(m.value(QStringLiteral("req")).toString(), true, v.value(QStringLiteral("name")).toString());
        setVcVoice(v.value(QStringLiteral("id")).toString());
    } else if (ev == QLatin1String("vc_stats")) {
        // diagnostics only (phrases, dropped blocks); nothing to show
    } else if (ev == QLatin1String("op_error")) {
        const QString err = m.value(QStringLiteral("message")).toString();
        const QString req = m.value(QStringLiteral("req")).toString();
        if (m.value(QStringLiteral("op")).toString() == QLatin1String("vc_add_target"))
            emit vcTargetAdded(req, false, err);
        else
            emit voiceOpResult(req, false, err);
        emit notice(err, 2);
    }
}

QString Controller::defaultVoiceFor(const QString &engine) const
{
    // Preferred defaults: native Italian voices of that engine.
    const QStringList preferred = engine == QLatin1String("kokoro")
        ? QStringList{QStringLiteral("ko:if_sara"), QStringLiteral("ko:im_nicola")}
        : engine == QLatin1String("supertonic") ? QStringList{QStringLiteral("st:F1")} : QStringList{};
    for (const QString &p : preferred)
        for (const Voice &v : m_voices)
            if (v.id == p) return p;
    if (engine == QLatin1String("qwen")) {
        // The shipped Italian voices come first in the library (Giulia).
        for (const Voice &v : m_voices)
            if (v.kind == QLatin1String("clone") && v.source == QLatin1String("stock")) return v.id;
        for (const Voice &v : m_voices)
            if (v.kind == QLatin1String("clone")) return v.id;
    }
    for (const Voice &v : m_voices)
        if (engineOfVoice(v.id) == engine && v.kind != QLatin1String("builtin_download")) return v.id;
    return QString();
}

void Controller::pickDefaultVoice()
{
    // Keep the voice remembered for this engine if it still exists (a deleted
    // clone falls back to the engine default).
    const QString current = m_settings.currentVoice();
    for (const Voice &v : m_voices)
        if (v.id == current) return;
    QString pick = defaultVoiceFor(m_settings.engine);
    if (pick.isEmpty() && !m_voices.isEmpty()) pick = m_voices.first().id;
    if (pick.isEmpty() || pick == current) return;
    m_settings.voices.insert(m_settings.engine, pick);
    saveNow();
}

// ===========================================================================
// Speaking
// ===========================================================================
Controller::Job *Controller::findJob(uint32_t id)
{
    for (Job &j : m_jobs)
        if (j.id == id) return &j;
    return nullptr;
}

void Controller::pruneJobs()
{
    while (m_jobs.size() > kMaxJobs) {
        const Job &f = m_jobs.first();
        if (f.state == Queued || f.state == Speaking) break;
        m_jobs.removeFirst();
    }
}

uint64_t Controller::targetServer() const
{
    if (m_server) return m_server;
    return ts3Functions.getCurrentServerConnectionHandlerID ? ts3Functions.getCurrentServerConnectionHandlerID() : 0;
}

bool Controller::connected() const
{
    const uint64_t sch = targetServer();
    if (!sch || !ts3Functions.getConnectionStatus) return false;
    int status = 0;
    return ts3Functions.getConnectionStatus(sch, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED;
}

QString Controller::voiceName(const QString &id) const
{
    for (const Voice &v : m_voices)
        if (v.id == id) return v.name;
    return id;
}

uint32_t Controller::speak(const QString &rawText, bool local, const QString &voiceOverride)
{
    return speakImpl(rawText, local, voiceOverride, 0, false);
}

uint32_t Controller::speakImpl(const QString &rawText, bool local, const QString &voiceOverride, uint64_t server,
                               bool fromChat)
{
    if (m_tearingDown) return 0;
    QString text = rawText.trimmed();
    if (text.isEmpty()) return 0;
    if (text.size() > kMaxText) {
        text.truncate(kMaxText);
        emit notice(tr("Message shortened to %1 characters.").arg(kMaxText), 1);
    }
    // An engine that is starting or still loading its voices is not a reason to drop a
    // message: it is queued and spoken as soon as the engine is ready. (Up to v1.3 it
    // was refused: the text stayed in the box, the chat echo went out only when the user
    // pressed Enter again ~30 s later, and repeated presses sent a burst - chat log
    // 2026-09-15 10:01:19-24.)
    const bool ready = m_link->isConnected() && voicesLoaded();
    const bool starting = m_wantBackend && (m_state == Backend::Starting || m_state == Backend::Loading);
    if (!ready && !starting && !m_link->isConnected()) {
        if (m_state == Backend::Installing)
            emit notice(tr("The voice engine is still being installed: try again when it is ready."), 1);
        else if (m_state == Backend::NotInstalled)
            emit notice(tr("The voice engine is not installed: press “Install…” at the top of this window."), 2);
        else
            emit notice(tr("The voice engine is not running: press Start."), 2);
        return 0;
    }
    if (ready) {
        const QString voice = voiceOverride.isEmpty() ? m_settings.currentVoice() : voiceOverride;
        if (voice.isEmpty()) {
            emit notice(tr("Choose a voice first."), 1);
            return 0;
        }
        if (engineOfVoice(voice) != m_settings.engine) {
            emit notice(tr("That voice belongs to another engine: choose a %1 voice.").arg(engineLabel(m_settings.engine)), 1);
            return 0;
        }
    }

    // A chat message is spoken on the server tab it was written in.
    const uint64_t sch = server ? server : targetServer();
    int connStatus = 0;
    const bool tsConnected = sch && ts3Functions.getConnectionStatus &&
                             ts3Functions.getConnectionStatus(sch, &connStatus) == ERROR_ok &&
                             connStatus == STATUS_CONNECTION_ESTABLISHED;
    const bool effectiveLocal = local || m_settings.previewOnly || !tsConnected;

    Job job;
    job.id = m_nextJob++;
    job.text = text;
    job.voiceName = tr("waiting for the voice engine");
    job.local = effectiveLocal;
    job.when = QDateTime::currentDateTime();
    job.voiceOverride = voiceOverride;
    job.server = sch;
    m_jobs.append(job);
    const uint32_t id = job.id;
    if (ready) sendJob(m_jobs.last());
    else if (!fromChat) emit notice(tr("The voice engine is starting: the message will be spoken as soon as it is ready."), 0);
    pruneJobs();

    // Chat messages are already in the chat: no history entry, no echo back into it.
    // The echo goes out now, whether or not the engine is ready.
    if (!local && !fromChat) {
        m_settings.history.removeAll(text);
        m_settings.history.append(text);
        while (m_settings.history.size() > Settings::kHistoryMax) m_settings.history.removeFirst();
        scheduleSave();   // may run inside a TS3 command callback: only a timer start here
        if (!effectiveLocal && m_settings.echoToChat) sendChatEcho(sch, text);
        if (!tsConnected && !m_settings.previewOnly)
            emit notice(tr("Not connected to a server: only you will hear it."), 1);
    }
    emit jobsChanged();
    return id;
}

bool Controller::sendJob(Job &j)
{
    const QString voice = j.voiceOverride.isEmpty() ? m_settings.currentVoice() : j.voiceOverride;
    j.sent = true;
    if (voice.isEmpty() || engineOfVoice(voice) != m_settings.engine) {
        j.state = Failed;
        j.error = tr("no voice of the active engine");
        return false;
    }
    if (j.local) m_link->markLocalJob(j.id);
    if (!m_audio->anyActive()) m_audio->setActiveServer(j.server);
    j.voiceName = voiceName(voice);
    m_link->send(QJsonObject{
        {QStringLiteral("op"), QStringLiteral("speak")},
        {QStringLiteral("id"), static_cast<double>(j.id)},
        {QStringLiteral("text"), j.text},
        {QStringLiteral("lang"), m_settings.lang},
        {QStringLiteral("voice"), voice},
        {QStringLiteral("speed"), m_settings.speed},
        {QStringLiteral("instruct"), m_settings.engine == QLatin1String("qwen") ? m_settings.instruct : QString()},
        {QStringLiteral("local"), j.local},
    });
    return true;
}

void Controller::flushPendingJobs()
{
    if (!m_link->isConnected() || !voicesLoaded()) return;
    bool changed = false;
    for (Job &j : m_jobs) {
        if (!j.sent && j.state == Queued) {
            sendJob(j);
            changed = true;
        }
    }
    if (changed) emit jobsChanged();
}

void Controller::stopAll()
{
    m_audio->flush(m_nextJob);
    if (m_link->isConnected()) m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("cancel_all")}});
    bool changed = false;
    for (Job &j : m_jobs) {
        if (j.state == Queued || j.state == Speaking) {
            j.state = Cancelled;
            m_link->forgetJob(j.id);
            changed = true;
        }
    }
    if (changed) emit jobsChanged();
}

void Controller::repeatLast()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (!m_jobs[i].local) {
            speak(m_jobs[i].text);
            return;
        }
    }
    if (!m_settings.history.isEmpty()) speak(m_settings.history.last());
}

int Controller::pendingJobs() const
{
    int n = 0;
    for (const Job &j : m_jobs)
        if (j.state == Queued || j.state == Speaking) ++n;
    return n;
}

void Controller::sendChatEcho(uint64_t sch, const QString &text)
{
    if (!sch || !ts3Functions.getClientID || !ts3Functions.getChannelOfClient || !ts3Functions.requestSendChannelTextMsg)
        return;
    anyID me = 0;
    uint64 channel = 0;
    if (ts3Functions.getClientID(sch, &me) != ERROR_ok) return;
    if (ts3Functions.getChannelOfClient(sch, me, &channel) != ERROR_ok) return;
    const QString full = m_settings.chatPrefix + text;
    const QByteArray msg = full.toUtf8();
    m_recentEchoes.append({nowMs(), stripBbcode(full)});
    while (m_recentEchoes.size() > 50) m_recentEchoes.removeFirst();
    ts3Functions.requestSendChannelTextMsg(sch, msg.constData(), channel, nullptr);
}

void Controller::onTick()
{
    const qint64 now = nowMs();
    bool remotePending = false;
    for (const Job &j : m_jobs) {
        if (!j.local && j.state == Speaking) {
            remotePending = true;
            break;
        }
    }
    if (remotePending || m_audio->remoteActive()) m_lastRemoteActivityMs = now;

    const bool ttsWant = !m_settings.previewOnly && now - m_lastRemoteActivityMs < 350;
    // Voice changer tail: the converted voice lags the microphone by ~0.25 s, so when the
    // user releases PTT (or stops talking) the channel must keep receiving it. The hold
    // starts only once the user's own transmission has stopped - forcing it while they
    // still talk would mask their PTT and stop the microphone uplink.
    const bool vcWant = m_audio->vcEnabled() && now - m_audio->vcLastOutputMs() < 250 &&
                        now - m_audio->vcLastIntentMs() > 60;
    const bool want = ttsWant || vcWant;
    if (want && !m_talk->active()) {
        uint64_t sch = m_audio->activeServer();
        if (!sch) sch = targetServer();
        int status = 0;
        if (sch && ts3Functions.getConnectionStatus &&
            ts3Functions.getConnectionStatus(sch, &status) == ERROR_ok && status == STATUS_CONNECTION_ESTABLISHED)
            m_talk->begin(sch, ttsWant && m_settings.speakWhenMuted);   // a muted mic stays muted for the voice changer
    } else if (!want && m_talk->active()) {
        m_talk->end();
    }
    m_audio->setVcHoldOverride(m_talk->active());

    const bool speakingNow = m_audio->anyActive() || pendingJobs() > 0;
    if (speakingNow != m_speaking) {
        m_speaking = speakingNow;
        emit speakingChanged(m_speaking);
    }
}

// ===========================================================================
// Voices
// ===========================================================================
void Controller::setVoice(const QString &id)
{
    if (id.isEmpty() || id == m_settings.currentVoice()) return;
    if (engineOfVoice(id) != m_settings.engine) {
        // Never load a second engine behind the user's back (RAM/VRAM).
        emit notice(tr("That voice belongs to another engine: choose a %1 voice.").arg(engineLabel(m_settings.engine)), 1);
        return;
    }
    m_settings.voices.insert(m_settings.engine, id);
    saveNow();
    emit voicesChanged();
}

void Controller::testVoice(const QString &id)
{
    QString lang = m_settings.lang;
    if (lang == QLatin1String("auto")) lang = QStringLiteral("it");
    speak(sampleSentence(lang), true, id);
}

namespace {
bool needsQwen(Controller *c, const QString &req, const std::function<void(const QString &)> &fail)
{
    Q_UNUSED(req);
    if (c->engine() != QLatin1String("qwen")) {
        fail(Controller::tr("Creating voices needs the Qwen3-TTS engine (Settings › Voice engine)."));
        return false;
    }
    if (c->state() == Controller::Backend::NotInstalled || c->state() == Controller::Backend::Installing ||
        c->state() == Controller::Backend::Stopped ||
        c->state() == Controller::Backend::Error) {
        fail(Controller::tr("The voice engine is not running: press Start."));
        return false;
    }
    return true;
}
} // namespace

void Controller::voiceFromFile(const QString &req, const QString &name, const QString &path, const QString &refText,
                               const QString &lang)
{
    if (!needsQwen(this, req, [this, req](const QString &e) { emit voiceOpResult(req, false, e); })) return;
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("voice_from_file")},
                             {QStringLiteral("req"), req},
                             {QStringLiteral("name"), name},
                             {QStringLiteral("path"), path},
                             {QStringLiteral("ref_text"), refText},
                             {QStringLiteral("lang"), lang}});
}

uint32_t Controller::voiceDesignPreview(const QString &req, const QString &instruct, const QString &text,
                                        const QString &lang)
{
    if (!needsQwen(this, req, [this, req](const QString &e) { emit voiceOpResult(req, false, e); })) return 0;
    const uint32_t id = m_nextJob++;
    m_link->markLocalJob(id);
    Job job;
    job.id = id;
    job.text = text;
    job.voiceName = tr("voice preview");
    job.local = true;
    job.when = QDateTime::currentDateTime();
    m_jobs.append(job);
    pruneJobs();
    emit jobsChanged();
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("voice_design_preview")},
                             {QStringLiteral("req"), req},
                             {QStringLiteral("id"), static_cast<double>(id)},
                             {QStringLiteral("instruct"), instruct},
                             {QStringLiteral("text"), text},
                             {QStringLiteral("lang"), lang}});
    return id;
}

void Controller::voiceDesignSave(const QString &req, const QString &name)
{
    if (!needsQwen(this, req, [this, req](const QString &e) { emit voiceOpResult(req, false, e); })) return;
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("voice_design_save")},
                             {QStringLiteral("req"), req},
                             {QStringLiteral("name"), name}});
}

void Controller::deleteVoice(const QString &id)
{
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("voice_delete")}, {QStringLiteral("voice"), id}});
}

void Controller::renameVoice(const QString &id, const QString &name)
{
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("voice_rename")},
                             {QStringLiteral("voice"), id},
                             {QStringLiteral("name"), name}});
}

// ===========================================================================
// Parameters: every setter applies AND persists (continuous ones debounced).
// ===========================================================================
void Controller::pushAudioSettings()
{
    m_audio->setRemoteGainDb(static_cast<float>(m_settings.remoteDb));
    m_audio->setLocalGainDb(static_cast<float>(m_settings.localDb));
    m_audio->setVoiceGainDb(static_cast<float>(m_settings.voiceDb));
    m_audio->setPitchSemitones(static_cast<float>(m_settings.pitchSt));
    m_audio->setMonitor(m_settings.monitor);
    m_audio->setMicMode(m_settings.micMode);
    m_audio->setPreviewOnly(m_settings.previewOnly);
    m_audio->setJitterMs(m_settings.jitterMs);
    m_audio->setSandboxState(m_sandbox);
    m_audio->setVcMonitor(m_settings.vcMonitor);
}

void Controller::setRemoteDb(double db)
{
    m_settings.remoteDb = db;
    m_audio->setRemoteGainDb(float(db));
    scheduleSave();
}

void Controller::setLocalDb(double db)
{
    m_settings.localDb = db;
    m_audio->setLocalGainDb(float(db));
    scheduleSave();
}

void Controller::setVoiceDb(double db)
{
    m_settings.voiceDb = db;
    m_audio->setVoiceGainDb(float(db));
    scheduleSave();
}

void Controller::setPitch(double st)
{
    m_settings.pitchSt = st;
    m_audio->setPitchSemitones(float(st));
    scheduleSave();
}

void Controller::setMonitor(bool on)
{
    m_settings.monitor = on;
    m_audio->setMonitor(on);
    saveNow();
}

void Controller::setMicMode(int mode)
{
    m_settings.micMode = qBound(0, mode, 2);
    m_audio->setMicMode(m_settings.micMode);
    saveNow();
}

void Controller::setSpeakWhenMuted(bool on)
{
    m_settings.speakWhenMuted = on;
    saveNow();
}

void Controller::setReadChannelChat(bool on)
{
    if (m_settings.readChannelChat == on) return;
    m_settings.readChannelChat = on;
    saveNow();
    logInfo("GBTTS: read my channel chat aloud %s", on ? "on" : "off");
}

void Controller::setSpeed(double speed)
{
    m_settings.speed = qBound(0.5, speed, 2.0);
    scheduleSave();
}

void Controller::setLang(const QString &lang)
{
    if (lang.isEmpty() || lang == m_settings.lang) return;
    m_settings.lang = lang;
    saveNow();
}

void Controller::setInstruct(const QString &instruct)
{
    m_settings.instruct = instruct.left(300);
    scheduleSave();
}

void Controller::setJitterMs(int ms)
{
    m_settings.jitterMs = qBound(40, ms, 1000);
    m_audio->setJitterMs(m_settings.jitterMs);
    scheduleSave();
}

void Controller::setPreviewOnly(bool on)
{
    if (m_settings.previewOnly == on) return;
    m_settings.previewOnly = on;
    m_audio->setPreviewOnly(on);
    saveNow();
    emit settingsApplied();
}

void Controller::setSandbox(const SandboxState &s)
{
    m_sandbox = s;
    m_settings.sandboxJson = QJsonDocument(s.toJson()).toJson(QJsonDocument::Compact);
    m_audio->setSandboxState(s);
    scheduleSave();
}

void Controller::applyPreset(int index)
{
    const QVector<Preset> list = presets();
    if (index < 0 || index >= list.size()) return;
    m_settings.presetIndex = index;
    setPitch(list[index].pitch);
    setSandbox(list[index].state);
    saveNow();
    emit settingsApplied();
}

QVector<Controller::Preset> Controller::presets()
{
    // Voice presets. Most recipes are the GameBaiters Soundboard Mic FX ones
    // (MicFx::builtinPresets) - same DSP, same sound - plus a few that only make
    // sense for a narrated voice.
    QVector<Preset> out;
    auto base = [] {
        SandboxState s;
        s.enabled = true;
        return s;
    };
    auto add = [&out](const QString &name, const SandboxState &s, float pitch) {
        Preset p;
        p.name = name;
        p.state = s;
        p.pitch = pitch;
        out.push_back(p);
    };

    add(tr("No effect"), SandboxState(), 0.0f);

    {   // Radio host: gentle compression, presence, a touch of room.
        SandboxState s = base();
        s.compEnabled = true; s.compThresholdDb = -24.0f; s.compRatio = 3.0f;
        s.compAttackMs = 8.0f; s.compReleaseMs = 120.0f; s.compMakeupDb = 4.0f;
        static const float kEq[16] = {-6, -3, 0, 1, 2, 1, 0, 0, 1, 2, 3, 3, 2, 1, 0, -2};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kEq[i];
        s.deesserEnabled = true;
        s.reverbWet = 0.06f;
        add(tr("Radio host"), s, 0.0f);
    }
    {   // Small room.
        SandboxState s = base();
        s.reverbWet = 0.16f;
        add(tr("Small room"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxRingEnabled = true; s.vfxRingFreq = 42.0f; s.vfxRingMix = 0.85f;
        s.bitcrusherEnabled = true; s.bitcrusherBitDepth = 7; s.bitcrusherRate = 14000.0f;
        add(tr("Robot"), s, 0.0f);
    }
    add(tr("Chipmunk"), base(), 7.0f);
    {
        SandboxState s = base();
        s.reverbWet = 0.22f;
        s.bassEnhEnabled = true; s.bassEnhFreq = 150.0f; s.bassEnhDrive = 4.0f; s.bassEnhMix = 0.5f;
        add(tr("Demon"), s, -6.0f);
    }
    {
        SandboxState s = base();
        static const float kTel[16] = {-12, -12, -12, -12, -8, -3, 0, 2, 3, 2, -2, -8, -12, -12, -12, -12};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kTel[i];
        s.saturatorEnabled = true; s.saturatorDrive = 3.0f; s.saturatorMix = 0.45f; s.saturatorTone = 4000.0f;
        add(tr("Telephone"), s, 0.0f);
    }
    {
        SandboxState s = base();
        static const float kRad[16] = {-12, -12, -12, -12, -10, -5, 0, 3, 4, 3, -1, -6, -12, -12, -12, -12};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kRad[i];
        s.saturatorEnabled = true; s.saturatorDrive = 5.0f; s.saturatorMix = 0.65f; s.saturatorTone = 3200.0f;
        s.saturatorMode = 3;
        s.bitcrusherEnabled = true; s.bitcrusherBitDepth = 10; s.bitcrusherRate = 16000.0f;
        add(tr("Military radio"), s, 0.0f);
    }
    {
        SandboxState s = base();
        static const float kMeg[16] = {-10, -10, -8, -6, -3, 0, 2, 4, 5, 5, 4, 2, -2, -6, -10, -12};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kMeg[i];
        s.compEnabled = true; s.compThresholdDb = -28.0f; s.compRatio = 8.0f;
        s.compAttackMs = 2.0f; s.compReleaseMs = 80.0f; s.compMakeupDb = 6.0f;
        s.saturatorEnabled = true; s.saturatorDrive = 6.0f; s.saturatorMix = 0.7f; s.saturatorTone = 5500.0f;
        s.vfxEnabled = true;
        s.vfxExcEnabled = true; s.vfxExcFreq = 2500.0f; s.vfxExcDrive = 4.0f; s.vfxExcMix = 0.4f;
        add(tr("Megaphone"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.reverbWet = 0.5f;
        add(tr("Cave"), s, 0.0f);
    }
    {
        SandboxState s = base();
        static const float kSub[16] = {2, 2, 1, 0, 0, -2, -5, -8, -11, -12, -12, -12, -12, -12, -12, -12};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kSub[i];
        s.chorusEnabled = true; s.chorusRate = 0.35f; s.chorusDepth = 6.0f;
        s.chorusBaseDelay = 14.0f; s.chorusVoices = 2; s.chorusMix = 0.55f;
        add(tr("Underwater"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxTuneEnabled = true; s.vfxTuneStrength = 1.0f; s.vfxTuneSpeedMs = 1.0f; s.vfxTuneScale = 0;
        s.vfxFormEnabled = true; s.vfxFormShift = 4.0f; s.vfxFormMix = 1.0f;
        s.vfxVibEnabled = true; s.vfxVibRate = 6.5f; s.vfxVibDepth = 0.35f;
        add(tr("Alien"), s, 2.0f);
    }
    {
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxFormEnabled = true; s.vfxFormShift = 7.0f; s.vfxFormMix = 1.0f;
        add(tr("Helium"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxFormEnabled = true; s.vfxFormShift = -6.0f; s.vfxFormMix = 1.0f;
        add(tr("Giant"), s, -3.0f);
    }
    {
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxShimEnabled = true; s.vfxShimMix = 0.45f; s.vfxShimFeedback = 0.6f; s.vfxShimPitch = 12; s.vfxShimDamp = 0.35f;
        s.vfxVibEnabled = true; s.vfxVibRate = 0.8f; s.vfxVibDepth = 0.25f;
        s.reverbWet = 0.35f;
        add(tr("Ghost"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.genLossEnabled = true; s.genLossGenerations = 180;
        s.vfxEnabled = true;
        s.vfxTremEnabled = true; s.vfxTremRate = 5.5f; s.vfxTremDepth = 0.3f; s.vfxTremShape = 0;
        static const float kAm[16] = {-12, -12, -10, -6, -2, 0, 1, 1, 0, -2, -6, -10, -12, -12, -12, -12};
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kAm[i];
        add(tr("Old AM radio"), s, 0.0f);
    }
    {
        SandboxState s = base();
        s.spatialMode = SandboxState::Spatial_8DPreset;
        s.spatialEngine = SandboxState::Engine_Leia;
        s.rotateRpm = 10.0f; s.rotateRadiusM = 1.5f; s.rotateElev = 0.0f;
        s.stereoWidthDeg = 0.0f; s.spatialMix = 0.9f; s.headSway = true;
        s.leiaReflEnable = true; s.leiaReflLevel = -8.0f; s.leiaRoomSize = 10.0f; s.leiaRoomType = 1;
        s.leiaClarity = 100.0f; s.leiaWidth = 35.0f;
        s.reverbWet = 0.12f;
        add(tr("8D voice"), s, 0.0f);
    }
    return out;
}

// ===========================================================================
// Real-time voice changer
// ===========================================================================
void Controller::setVcState(VcState s, const QString &message)
{
    if (s == m_vcState && message == m_vcMessage) return;
    m_vcState = s;
    m_vcMessage = message;
    emit vcChanged();
}

void Controller::sendVcConfigure()
{
    if (!m_link->isConnected()) return;
    const bool on = m_settings.vcEnabled && !m_settings.vcVoice.isEmpty();
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("vc_configure")},
                             {QStringLiteral("enabled"), on},
                             {QStringLiteral("voice"), m_settings.vcVoice},
                             {QStringLiteral("preset"), m_settings.vcPreset},
                             {QStringLiteral("threads"), 4}});
}

void Controller::setVcEnabled(bool on)
{
    if (m_settings.vcEnabled == on) return;
    m_settings.vcEnabled = on;
    saveNow();
    if (!on) {
        m_audio->setVcEnabled(false);      // the real microphone is back immediately
        setVcState(VcState::Off, QString());
    } else if (m_settings.vcVoice.isEmpty()) {
        emit notice(tr("Choose the voice you want to sound like first."), 1);
    } else if (!m_link->isConnected()) {
        emit notice(tr("The voice engine is not running: the voice changer starts with it."), 1);
    }
    sendVcConfigure();
    emit vcChanged();
}

void Controller::setVcVoice(const QString &id)
{
    if (id.isEmpty() || id == m_settings.vcVoice) return;
    m_settings.vcVoice = id;
    saveNow();
    if (m_settings.vcEnabled) sendVcConfigure();
    emit vcChanged();
}

void Controller::setVcPreset(const QString &preset)
{
    if (preset == m_settings.vcPreset || (preset != QLatin1String("40ms") && preset != QLatin1String("120ms"))) return;
    m_settings.vcPreset = preset;
    saveNow();
    if (m_settings.vcEnabled) sendVcConfigure();
    emit vcChanged();
}

void Controller::setVcMonitor(bool on)
{
    m_settings.vcMonitor = on;
    m_audio->setVcMonitor(on);
    saveNow();
}

void Controller::requestVcTargets()
{
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("vc_list_targets")}});
}

void Controller::addVcTargetFromFile(const QString &req, const QString &name, const QString &path)
{
    if (!m_link->isConnected()) {
        emit vcTargetAdded(req, false, tr("The voice engine is not running: press Start."));
        return;
    }
    m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("vc_add_target")},
                             {QStringLiteral("req"), req},
                             {QStringLiteral("name"), name},
                             {QStringLiteral("path"), path}});
}

// ===========================================================================
// TeamSpeak events (called from TS3 callbacks: never block, never write the registry)
// ===========================================================================
void Controller::onConnectStatus(uint64_t sch, int status)
{
    if (status == STATUS_DISCONNECTED) {
        m_talk->onConnectionLost(sch);
        if (sch == m_audio->activeServer()) {
            // Anything queued for that server is meaningless now. Posting the
            // cancel is non-blocking; nothing here waits.
            m_audio->flush(m_nextJob);
            for (Job &j : m_jobs)
                if (j.state == Queued || j.state == Speaking) j.state = Cancelled;
            if (m_link->isConnected()) m_link->send(QJsonObject{{QStringLiteral("op"), QStringLiteral("cancel_all")}});
            emit jobsChanged();
        }
    } else if (status == STATUS_CONNECTION_ESTABLISHED) {
        if (!m_server) m_server = sch;
        if (!m_audio->anyActive()) m_audio->setActiveServer(targetServer());
    }
    emit stateChanged();
}

void Controller::onCurrentServer(uint64_t sch)
{
    m_server = sch;
    if (!m_audio->anyActive()) m_audio->setActiveServer(sch);
    emit stateChanged();
}

void Controller::onClientStopsTalking(uint64_t sch)
{
    m_talk->onClientStopsTalking(sch);
}

void Controller::onOwnChannelMessage(uint64_t sch, const QString &message)
{
    if (m_tearingDown || !m_settings.readChannelChat) return;
    const QString text = stripBbcode(message);
    if (text.isEmpty()) return;
    // Our own "also write it in the chat" echo coming back from the server: it was
    // already spoken when it was typed in the TTS window.
    const qint64 now = nowMs();
    for (int i = m_recentEchoes.size() - 1; i >= 0; --i) {
        if (now - m_recentEchoes[i].first > 120000) {
            m_recentEchoes.removeAt(i);
        } else if (m_recentEchoes[i].second == text) {
            m_recentEchoes.removeAt(i);
            return;
        }
    }
    logInfo("GBTTS: reading an own channel chat message aloud (%d characters)", int(text.size()));
    speakImpl(text, false, QString(), sch, true);
}

} // namespace gbtts
