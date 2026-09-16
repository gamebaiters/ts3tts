#pragma once

#include <QObject>
#include <QTimer>

#include <cstdint>

namespace gbtts {

class TtsAudio;

// Makes TeamSpeak transmit while the TTS voice speaks and restores exactly what
// the user had afterwards. A port of the GameBaiters Soundboard's
// TalkStateManager, keeping every guard that took a release to learn:
//  - 150 ms watchdog re-asserting INPUT_ACTIVE (TeamSpeak's PTT-up handler steals it)
//  - never trust a TS_CONT_TRANS snapshot as the "user mode" (our own write leaking back)
//  - VAD module re-init after restoring a VAD mode (else voice activation stays dead)
//  - restore-verify at 80 / 220 / 500 ms (late TeamSpeak flips on PTT-only servers)
//  - onConnectionLost() never calls ts3Functions (DirectSound teardown crash)
// Plus what a TTS needs on top: talking while the microphone is MUTED. The mute
// is lifted only for the utterance, the real mic is replaced by silence first,
// and the mute is put back afterwards unless the user changed it meanwhile.
class TalkState : public QObject
{
    Q_OBJECT
public:
    enum Mode { Invalid, PttWithoutVa, PttWithVa, VoiceActivation, ContTrans };

    explicit TalkState(TtsAudio *audio, QObject *parent = nullptr);

    void begin(uint64_t sch, bool unmuteIfMuted);
    void end();
    void onClientStopsTalking(uint64_t sch);
    void onConnectionLost(uint64_t sch);     // 0 = every server
    bool active() const { return m_server != 0; }
    uint64_t server() const { return m_server; }
    bool micWasMuted() const { return m_unmutedByUs; }

private:
    Mode read(uint64_t sch);
    bool write(uint64_t sch, Mode m);
    void forceVadReinit(uint64_t sch, Mode target);
    void verifyInput(uint64_t sch, Mode target);
    void onWatchdog();
    void onRestoreVerify();

    TtsAudio *m_audio;
    uint64_t  m_server = 0;
    Mode      m_previous = Invalid;
    Mode      m_current = Invalid;
    Mode      m_lastUserMode = Invalid;
    uint64_t  m_lastUserServer = 0;
    bool      m_unmutedByUs = false;
    QTimer    m_watchdog;
    QTimer    m_restoreTimer;
    QTimer    m_micSilenceRelease;
    Mode      m_restoreTarget = Invalid;
    uint64_t  m_restoreServer = 0;
    int       m_restoreAttempts = 0;
};

} // namespace gbtts
