#pragma once

#include "audio/SpscRing.h"

#include "dsp/PitchShiftGrain.h"
#include "dsp/SandboxState.h"
#include "dsp/SlotDsp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gbtts {

class LocalPlayer;

// ---------------------------------------------------------------------------
// The audio core. Four kinds of threads touch it:
//
//   link thread     (producer)  pushPcm / pushEnd: 48 k mono int16 from the
//                               backend, tagged with the job id
//   pump thread     (owned)     raw ring -> gain -> pitch -> SlotDsp -> 10 ms
//                               stereo blocks, kept ~120 ms ahead of playout
//   TS3 capture     (reader)    injects blocks into the outgoing voice, feeds
//                               the local monitor ring
//   TS3 playback /  (reader)    plays the monitor ring locally; takes over the
//   LocalPlayer                 block reader itself when capture is not running
//                               (preview while PTT is up / not connected)
//
// No audio thread ever waits on a mutex the GUI can hold: DSP state pushes lock
// only against the pump, readers hand over under a spinlock, everything the
// callbacks read is atomic.
// ---------------------------------------------------------------------------
class TtsAudio
{
public:
    static constexpr int kRate  = 48000;
    static constexpr int kBlock = 480;              // 10 ms

    enum MicMode { MicReplace = 0, MicDuck = 1, MicMix = 2 };

    TtsAudio();
    ~TtsAudio();

    void start();
    void stop();

    // ---- producer (link thread) ------------------------------------------
    // Accepts the whole chunk or nothing (false = rings full, retry later).
    bool pushPcm(uint32_t jobId, const int16_t *samples, int count, bool local);
    bool pushEnd(uint32_t jobId, bool local);
    uint32_t minJob() const { return m_minJob.load(std::memory_order_acquire); }

    // ---- GUI thread --------------------------------------------------------
    // Drop everything older than `firstValidJob`, fading the sound out in 5 ms.
    void flush(uint32_t firstValidJob);
    void setSandboxState(const SandboxState &s);
    void setPitchSemitones(float st);
    void setVoiceGainDb(float db);
    void setRemoteGainDb(float db);
    void setLocalGainDb(float db);
    void setMonitor(bool on)            { m_monitor.store(on, std::memory_order_relaxed); }
    void setMicMode(int mode)           { m_micMode.store(mode, std::memory_order_relaxed); }
    void setPreviewOnly(bool on)        { m_previewOnly.store(on, std::memory_order_relaxed); }
    void setForceMicSilence(bool on)    { m_forceMicSilence.store(on, std::memory_order_relaxed); }
    void setJitterMs(int ms);
    void setActiveServer(uint64_t sch)  { m_activeServer.store(sch, std::memory_order_relaxed); }
    uint64_t activeServer() const       { return m_activeServer.load(std::memory_order_relaxed); }

    // True while audio meant for the channel is queued, playing or ringing out.
    bool remoteActive() const;
    bool anyActive() const;
    bool captureRunning() const;
    float outputPeak() const;        // decaying peak for the meter (0..1)
    float dspCpuPercent();

    // ---- real-time voice changer -------------------------------------------
    // GUI thread
    void setVcEnabled(bool on)          { m_vcEnabled.store(on, std::memory_order_relaxed); }
    bool vcEnabled() const              { return m_vcEnabled.load(std::memory_order_relaxed); }
    void setVcMonitor(bool on)          { m_vcMonitor.store(on, std::memory_order_relaxed); }
    // True while OUR talk-state override forces transmission: the client's
    // "will send" flag then says nothing about the user, so no new mic is sent.
    void setVcHoldOverride(bool on)     { m_vcHoldOverride.store(on, std::memory_order_relaxed); }
    int64_t vcLastOutputMs() const      { return m_vcLastOutputMs.load(std::memory_order_relaxed); }
    int64_t vcLastIntentMs() const      { return m_vcLastIntentMs.load(std::memory_order_relaxed); }
    float vcInputPeak() const           { return m_vcInPeak.load(std::memory_order_relaxed); }
    float vcOutputPeak() const          { return m_vcOutPeak.load(std::memory_order_relaxed); }
    uint32_t vcDropped() const          { return m_vcDropped.load(std::memory_order_relaxed); }
    // link thread
    size_t readMicUplink(int16_t *dst, size_t max) { return m_vcUp.read(dst, max); }
    size_t micUplinkAvailable() const   { return m_vcUp.readAvailable(); }
    bool pushVc(const int16_t *samples, int count);   // false = dropped (voice changer off or ring full)

    // ---- TeamSpeak audio threads ------------------------------------------
    // clientWillSend = (*edited & 2): TeamSpeak will transmit this block (PTT down / voice detected).
    bool onCapture(uint64_t sch, short *samples, int frames, int channels, bool clientWillSend = true);
    void onPlayback(uint64_t sch, short *samples, int frames, int channels,
                    const unsigned int *speakers, unsigned int *fillMask);

    // ---- LocalPlayer thread --------------------------------------------------
    int  renderFallback(int16_t *stereo, int frames);
    bool fallbackWanted() const;

private:
    struct Seg   { uint32_t job; uint32_t count; uint8_t flags; };
    struct Block { int16_t pcm[kBlock * 2]; uint32_t job; uint8_t flags; };
    enum Reader : int { ReaderNone = 0, ReaderCapture = 1, ReaderPlayback = 2, ReaderFallback = 3 };

    void pumpLoop();
    bool pumpOnce();
    bool endPendingInRaw() const;
    void resetChain();

    // Reader side, caller holds m_readLock.
    int  readLocked(int16_t *stereo, uint8_t *flags, int frames, bool &anyRemote);
    int  readAs(Reader who, int16_t *stereo, uint8_t *flags, int frames, bool &anyRemote);

    // ---- rings --------------------------------------------------------------
    SpscRing<int16_t> m_raw{1u << 21};          // ~43 s of mono audio
    SpscRing<Seg>     m_segs{1u << 14};
    SpscRing<Block>   m_blocks{64};
    SpscRing<int16_t> m_monitorRing{1u << 16};  // stereo interleaved

    // ---- job / control state -------------------------------------------------
    std::atomic<uint32_t> m_minJob{1};
    std::atomic<uint32_t> m_flushEpoch{0};
    std::atomic<int64_t>  m_rawRemote{0};       // remote samples not yet pumped
    std::atomic<int>      m_blocksRemote{0};    // remote blocks waiting for a reader
    std::atomic<bool>     m_pumpRemoteBusy{false};
    std::atomic<int64_t>  m_lastRemoteMs{0};
    std::atomic<int64_t>  m_lastAnyMs{0};
    std::atomic<int64_t>  m_lastCaptureMs{0};
    std::atomic<int64_t>  m_lastPlaybackMs{0};
    std::atomic<uint64_t> m_activeServer{0};

    std::atomic<float> m_voiceGain{1.0f};
    std::atomic<float> m_remoteGain{1.0f};
    std::atomic<float> m_localGain{1.0f};
    std::atomic<float> m_pitchRatio{1.0f};
    std::atomic<bool>  m_monitor{true};
    std::atomic<int>   m_micMode{MicReplace};
    std::atomic<bool>  m_previewOnly{false};
    std::atomic<bool>  m_forceMicSilence{false};
    std::atomic<int>   m_jitterFrames{kRate / 5};
    std::atomic<float> m_peak{0.0f};

    // ---- reader state (m_readLock) ---------------------------------------------
    SpinLock m_readLock;
    Block    m_cur{};
    bool     m_haveCur = false;
    int      m_curOff = 0;
    uint32_t m_readerEpoch = 0;
    int      m_fadeLeft = 0;
    std::atomic<int> m_owner{ReaderNone};
    // Starvation guard: the blocks can run dry mid-waveform (raw ring empty when the
    // producer is slower than real time, so the pump had nothing to ramp down). The
    // reader then decays the last sample to zero over kRampFrames and ramps the next
    // block in; a decay still running when audio returns is summed into it.
    int16_t  m_lastL = 0, m_lastR = 0;
    uint8_t  m_lastReadFlags = 0;
    bool     m_starved = false;
    int      m_rampInLeft = 0;
    int      m_decayLeft = 0;
    int16_t  m_decayL = 0, m_decayR = 0;

    // ---- pump state (pump thread only) ------------------------------------------
    std::thread             m_pump;
    std::atomic<bool>       m_running{false};
    std::mutex              m_wakeMutex;
    std::condition_variable m_wake;
    uint32_t m_pumpEpoch = 0;
    Seg      m_seg{};
    bool     m_haveSeg = false;
    uint32_t m_segLeft = 0;
    bool     m_inUtterance = false;
    int64_t  m_waitStartMs = 0;
    int64_t  m_notBeforeMs = 0;
    int      m_tailBlocks = 0;
    int      m_quietBlocks = 0;
    uint32_t m_lastJob = 0;
    uint8_t  m_lastFlags = 0;
    float    m_ramp = 0.0f;
    std::vector<int16_t> m_tmpMono;
    std::vector<float>   m_tmpFloat;

    // ---- reader scratch buffers (each used by exactly one callback thread) --------
    // Plain members allocated once, not thread_local: TeamSpeak's audio threads
    // outlive the plugin DLL, and thread_local storage created by an unloaded DLL
    // on a foreign thread is never freed (and its destructor would run into
    // unmapped code when that thread exits). Callbacks larger than this are
    // served for their first kMaxCallbackFrames (TS3 delivers 960).
    static constexpr int kMaxCallbackFrames = 8192;
    std::vector<int16_t> m_capStereo;
    std::vector<uint8_t> m_capFlags;
    std::vector<int16_t> m_capMonitor;
    std::vector<int16_t> m_pbStereo;
    std::vector<uint8_t> m_pbFlags;
    std::vector<uint8_t> m_fbFlags;

    // ---- voice changer ------------------------------------------------------------
    // m_vcUp:   capture thread writes the user's mic, link thread reads it (frames 0x03).
    // m_vcDown: link thread writes converted voice (frames 0x04), capture thread reads it.
    static constexpr size_t kVcPrebufferFrames = kRate * 60 / 1000;   // the model emits 40-120 ms bursts
    static constexpr int    kVcRampFrames = 96;                      // 2 ms gap edges
    static constexpr int    kVcIntentTailMs = 200;                   // keep sending briefly after PTT up
    SpscRing<int16_t>     m_vcUp{1u << 16};     // ~1.4 s mono
    SpscRing<int16_t>     m_vcDown{1u << 17};   // ~2.7 s mono
    std::atomic<bool>     m_vcEnabled{false};
    std::atomic<bool>     m_vcMonitor{false};
    std::atomic<bool>     m_vcHoldOverride{false};
    std::atomic<int64_t>  m_vcLastOutputMs{0};
    std::atomic<int64_t>  m_vcLastIntentMs{0};
    std::atomic<float>    m_vcInPeak{0.0f};
    std::atomic<float>    m_vcOutPeak{0.0f};
    std::atomic<uint32_t> m_vcDropped{0};
    // capture thread only
    bool    m_vcWasEnabled = false;
    bool    m_vcPlaying = false;
    float   m_vcGain = 0.0f;
    int64_t m_vcIntentUntilMs = 0;
    std::vector<int16_t> m_vcScratch;

    // ---- DSP (pump thread, state pushes under m_dspMutex) ------------------------
    std::mutex      m_dspMutex;
    SlotDsp         m_dsp;
    PitchShiftGrain m_pitch;
    bool            m_resetChainPending = false;

    std::unique_ptr<LocalPlayer> m_player;
};

} // namespace gbtts
