#include "ts3/TalkState.h"
#include "audio/TtsAudio.h"

#include "common.h"
#include "ts3log.h"

#include <cstring>

namespace gbtts {

namespace {
const char *name(TalkState::Mode m)
{
    switch (m) {
    case TalkState::PttWithoutVa: return "PTT";
    case TalkState::PttWithVa: return "PTT+VA";
    case TalkState::VoiceActivation: return "VAD";
    case TalkState::ContTrans: return "CONT";
    default: return "INVALID";
    }
}
} // namespace

TalkState::TalkState(TtsAudio *audio, QObject *parent) : QObject(parent), m_audio(audio)
{
    m_watchdog.setInterval(150);
    connect(&m_watchdog, &QTimer::timeout, this, &TalkState::onWatchdog);
    m_restoreTimer.setSingleShot(true);
    connect(&m_restoreTimer, &QTimer::timeout, this, &TalkState::onRestoreVerify);
    // The real microphone stays replaced by silence a little after the mute is
    // put back, so TeamSpeak never transmits a live-mic fragment in between.
    m_micSilenceRelease.setSingleShot(true);
    m_micSilenceRelease.setInterval(300);
    connect(&m_micSilenceRelease, &QTimer::timeout, this, [this] {
        if (!m_unmutedByUs) m_audio->setForceMicSilence(false);
    });
}

TalkState::Mode TalkState::read(uint64_t sch)
{
    if (sch == 0 || !ts3Functions.getPreProcessorConfigValue) return Invalid;
    char *vadStr = nullptr;
    if (ts3Functions.getPreProcessorConfigValue(sch, "vad", &vadStr) != ERROR_ok || !vadStr) return Invalid;
    const bool vad = std::strcmp(vadStr, "true") == 0;
    ts3Functions.freeMemory(vadStr);
    int input = 0;
    if (ts3Functions.getClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, &input) != ERROR_ok) return Invalid;
    const bool ptt = input == INPUT_DEACTIVATED;
    if (ptt) return vad ? PttWithVa : PttWithoutVa;
    return vad ? VoiceActivation : ContTrans;
}

bool TalkState::write(uint64_t sch, Mode m)
{
    if (sch == 0 || m == Invalid) return false;
    const bool va = m == PttWithVa || m == VoiceActivation;
    const bool in = m == ContTrans || m == VoiceActivation;
    if (checkError(ts3Functions.setPreProcessorConfigValue(sch, "vad", va ? "true" : "false"), "GBTTS: set vad"))
        return false;
    if (checkError(ts3Functions.setClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED,
                                                           in ? INPUT_ACTIVE : INPUT_DEACTIVATED),
                   "GBTTS: set input"))
        return false;
    ts3Functions.flushClientSelfUpdates(sch, nullptr);
    m_current = m;
    if (m == ContTrans) {
        if (!m_watchdog.isActive()) m_watchdog.start();
    } else {
        m_watchdog.stop();
    }
    return true;
}

void TalkState::forceVadReinit(uint64_t sch, Mode target)
{
    if (sch == 0) return;
    ts3Functions.setPreProcessorConfigValue(sch, "vad", "false");
    ts3Functions.flushClientSelfUpdates(sch, nullptr);
    ts3Functions.setPreProcessorConfigValue(sch, "vad", "true");
    ts3Functions.flushClientSelfUpdates(sch, nullptr);
    const int inVal = (target == ContTrans || target == VoiceActivation) ? INPUT_ACTIVE : INPUT_DEACTIVATED;
    ts3Functions.setClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, inVal);
    ts3Functions.flushClientSelfUpdates(sch, nullptr);
}

void TalkState::verifyInput(uint64_t sch, Mode target)
{
    if (sch == 0) return;
    const bool wantActive = target == ContTrans || target == VoiceActivation;
    int actual = 0;
    if (ts3Functions.getClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED, &actual) != ERROR_ok) return;
    if (wantActive != (actual == INPUT_ACTIVE)) {
        ts3Functions.setClientSelfVariableAsInt(sch, CLIENT_INPUT_DEACTIVATED,
                                                wantActive ? INPUT_ACTIVE : INPUT_DEACTIVATED);
        ts3Functions.flushClientSelfUpdates(sch, nullptr);
    }
}

void TalkState::begin(uint64_t sch, bool unmuteIfMuted)
{
    if (sch == 0) return;
    if (m_server != 0 && m_server != sch) end();
    m_restoreTimer.stop();
    m_restoreTarget = Invalid;
    m_restoreServer = 0;

    if (m_server == sch && m_current == ContTrans) return;

    if (unmuteIfMuted && !m_unmutedByUs) {
        int muted = 0;
        if (ts3Functions.getClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, &muted) == ERROR_ok &&
            muted == MUTEINPUT_MUTED) {
            // Silence the real microphone BEFORE lifting the mute.
            m_micSilenceRelease.stop();
            m_audio->setForceMicSilence(true);
            if (!checkError(ts3Functions.setClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, MUTEINPUT_NONE),
                            "GBTTS: unmute")) {
                ts3Functions.flushClientSelfUpdates(sch, nullptr);
                m_unmutedByUs = true;
            } else {
                m_audio->setForceMicSilence(false);
            }
        }
        int hardware = 1;
        if (ts3Functions.getClientSelfVariableAsInt(sch, CLIENT_INPUT_HARDWARE, &hardware) == ERROR_ok &&
            hardware == 0 && ts3Functions.activateCaptureDevice) {
            logInfo("GBTTS: capture device closed, activating it so the voice can be sent");
            ts3Functions.activateCaptureDevice(sch);
        }
    }

    if (m_previous == Invalid) {
        Mode s = read(sch);
        if (s == Invalid) return;
        if (s == ContTrans) {
            // Almost always our own (or the Soundboard's) override leaking back.
            s = (m_lastUserMode != Invalid && m_lastUserServer == sch) ? m_lastUserMode : PttWithoutVa;
        }
        m_previous = s;
        m_lastUserMode = s;
        m_lastUserServer = sch;
    }
    m_server = sch;
    write(sch, ContTrans);
}

void TalkState::end()
{
    if (m_server == 0) return;
    const uint64_t sch = m_server;
    m_server = 0;
    m_watchdog.stop();

    const Mode target = m_previous;
    m_previous = Invalid;
    if (target != Invalid) {
        write(sch, target);
        if (target == VoiceActivation || target == PttWithVa) forceVadReinit(sch, target);
        verifyInput(sch, target);
        m_restoreTarget = target;
        m_restoreServer = sch;
        m_restoreAttempts = 0;
        m_restoreTimer.start(80);
    }

    if (m_unmutedByUs) {
        m_unmutedByUs = false;
        int muted = 0;
        // Put the mute back only if nobody changed it while we were speaking.
        if (ts3Functions.getClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, &muted) == ERROR_ok &&
            muted == MUTEINPUT_NONE) {
            ts3Functions.setClientSelfVariableAsInt(sch, CLIENT_INPUT_MUTED, MUTEINPUT_MUTED);
            ts3Functions.flushClientSelfUpdates(sch, nullptr);
        }
        m_micSilenceRelease.start();
    }
}

void TalkState::onClientStopsTalking(uint64_t sch)
{
    if (m_server == 0 || sch != m_server) return;
    if (m_current == ContTrans && (m_previous == PttWithoutVa || m_previous == PttWithVa))
        write(m_server, ContTrans);
}

void TalkState::onWatchdog()
{
    if (m_current != ContTrans || m_server == 0) {
        m_watchdog.stop();
        return;
    }
    int input = 0;
    if (ts3Functions.getClientSelfVariableAsInt(m_server, CLIENT_INPUT_DEACTIVATED, &input) != ERROR_ok) return;
    if (input != INPUT_ACTIVE) {
        ts3Functions.setClientSelfVariableAsInt(m_server, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE);
        ts3Functions.flushClientSelfUpdates(m_server, nullptr);
    }
}

void TalkState::onRestoreVerify()
{
    if (m_restoreTarget == Invalid || m_restoreServer == 0) return;
    if (m_current == ContTrans && m_server != 0) return;   // speaking again
    const Mode observed = read(m_restoreServer);
    if (observed != Invalid && observed != m_restoreTarget) {
        logDebug("GBTTS: restore drift %s -> %s, re-applying", name(observed), name(m_restoreTarget));
        write(m_restoreServer, m_restoreTarget);
        if (m_restoreTarget == VoiceActivation || m_restoreTarget == PttWithVa)
            forceVadReinit(m_restoreServer, m_restoreTarget);
    }
    ++m_restoreAttempts;
    if (m_restoreAttempts == 1) {
        m_restoreTimer.start(140);
    } else if (m_restoreAttempts == 2) {
        m_restoreTimer.start(280);
    } else {
        m_restoreTarget = Invalid;
        m_restoreServer = 0;
    }
}

void TalkState::onConnectionLost(uint64_t sch)
{
    if (sch != 0 && sch != m_server && sch != m_restoreServer) return;
    // No ts3Functions here: TeamSpeak is tearing the connection's audio down.
    m_watchdog.stop();
    m_restoreTimer.stop();
    m_micSilenceRelease.stop();
    m_restoreTarget = Invalid;
    m_restoreServer = 0;
    m_previous = Invalid;
    m_current = Invalid;
    m_server = 0;
    m_unmutedByUs = false;
    m_audio->setForceMicSilence(false);
}

} // namespace gbtts
