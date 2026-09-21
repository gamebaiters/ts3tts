#include "audio/TtsAudio.h"
#include "audio/LocalPlayer.h"

#include "common.h"   // SPEAKER_* channel masks (TeamSpeak SDK)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace gbtts {

namespace {

constexpr uint8_t kFlagLocal = 0x01;
constexpr uint8_t kFlagEnd   = 0x02;

constexpr int kTargetBlocks   = 12;    // keep 120 ms processed ahead of playout
constexpr int kLowWaterBlocks = 3;     // below this a partial block is emitted
constexpr int kFadeFrames     = 240;   // 5 ms stop fade
constexpr int kRampFrames     = 96;    // 2 ms underrun ramps
constexpr int kMaxTailBlocks  = 300;   // 3 s reverb/delay ring-out cap
constexpr int kGapMs          = 220;   // pause between two messages
constexpr int64_t kStaleMs    = 150;   // a callback silent this long is "not running"

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline short clampS(int v)
{
    return static_cast<short>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

struct SpinGuard {
    explicit SpinGuard(SpinLock &l) : lock(l) { lock.lock(); }
    ~SpinGuard() { lock.unlock(); }
    SpinLock &lock;
};

} // namespace

TtsAudio::TtsAudio()
{
    m_dsp.setSampleRate(kRate);
    m_pitch.setSampleRate(kRate);
    m_pitch.setWindowMs(40.0f);
    m_tmpMono.resize(kBlock);
    m_tmpFloat.resize(kBlock);
    m_capStereo.resize(kMaxCallbackFrames * 2);
    m_capFlags.resize(kMaxCallbackFrames);
    m_capMonitor.resize(kMaxCallbackFrames * 2);
    m_pbStereo.resize(kMaxCallbackFrames * 2);
    m_pbFlags.resize(kMaxCallbackFrames);
    m_fbFlags.resize(kMaxCallbackFrames);
    m_bridgePub.resize(kMaxCallbackFrames * 2);
    m_vcScratch.resize(kMaxCallbackFrames);
    m_player = std::make_unique<LocalPlayer>(this);
}

TtsAudio::~TtsAudio()
{
    stop();
}

void TtsAudio::start()
{
    if (m_running.exchange(true)) return;
    m_pump = std::thread([this] { pumpLoop(); });
    m_player->start();
}

void TtsAudio::stop()
{
    if (m_player) m_player->stop();
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_pump.joinable()) m_pump.join();
}

// ===========================================================================
// Producer
// ===========================================================================
bool TtsAudio::pushPcm(uint32_t jobId, const int16_t *samples, int count, bool local)
{
    if (count <= 0) return true;
    if (jobId < m_minJob.load(std::memory_order_acquire)) return true;   // cancelled: drop
    if (m_raw.writeAvailable() < static_cast<size_t>(count) || m_segs.writeAvailable() < 1)
        return false;
    // Samples first, descriptor second: the pump never sees a descriptor whose
    // samples are not in the ring yet.
    m_raw.write(samples, static_cast<size_t>(count));
    Seg s{jobId, static_cast<uint32_t>(count), static_cast<uint8_t>(local ? kFlagLocal : 0)};
    m_segs.write(&s, 1);
    if (!local) m_rawRemote.fetch_add(count, std::memory_order_relaxed);
    m_wake.notify_one();
    return true;
}

bool TtsAudio::pushEnd(uint32_t jobId, bool local)
{
    if (jobId < m_minJob.load(std::memory_order_acquire)) return true;
    if (m_segs.writeAvailable() < 1) return false;
    Seg s{jobId, 0, static_cast<uint8_t>(kFlagEnd | (local ? kFlagLocal : 0))};
    m_segs.write(&s, 1);
    m_wake.notify_one();
    return true;
}

bool TtsAudio::pushVc(const int16_t *samples, int count)
{
    if (count <= 0) return true;
    if (!m_vcEnabled.load(std::memory_order_relaxed) || m_vcDown.writeAvailable() < static_cast<size_t>(count)) {
        // Never block the link thread for the voice changer: a late voice is worse than a gap.
        m_vcDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_vcDown.write(samples, static_cast<size_t>(count));
    return true;
}

// ===========================================================================
// GUI controls
// ===========================================================================
void TtsAudio::flush(uint32_t firstValidJob)
{
    uint32_t cur = m_minJob.load(std::memory_order_acquire);
    if (firstValidJob > cur) m_minJob.store(firstValidJob, std::memory_order_release);
    m_flushEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_wake.notify_one();
}

void TtsAudio::setSandboxState(const SandboxState &in)
{
    SandboxState s = in;
    // A TTS utterance is not a looping file: Paulstretch would accumulate
    // unbounded latency, binaural is a tone generator and sidechains point at
    // Soundboard slots that do not exist here.
    s.stretchEnabled = false;
    s.binauralEnabled = false;
    s.compSidechainSlot = -1;
    s.gateSidechainSlot = -1;
    s.deesserSidechainSlot = -1;
    s.duckSource = false;

    const bool wantsLeia = s.enabled && s.spatialEngine == SandboxState::Engine_Leia &&
                           (s.spatialMode == SandboxState::Spatial_3DManual ||
                            s.spatialMode == SandboxState::Spatial_3DRotate ||
                            s.spatialMode == SandboxState::Spatial_8DPreset);
    // Heavy one-time init (SOFA load, FFT plans, IR build) outside the lock:
    // the pump keeps running on the previous state meanwhile.
    if (wantsLeia) m_dsp.prepareLeia(kRate);
    m_dsp.prepareConvReverb(s);
    std::lock_guard<std::mutex> g(m_dspMutex);
    m_dsp.applyState(s);
}

void TtsAudio::setPitchSemitones(float st)
{
    st = std::max(-12.0f, std::min(12.0f, st));
    m_pitchRatio.store(std::pow(2.0f, st / 12.0f), std::memory_order_relaxed);
}

void TtsAudio::setVoiceGainDb(float db)  { m_voiceGain.store(dbToLin(std::max(-30.0f, std::min(12.0f, db))), std::memory_order_relaxed); }
void TtsAudio::setRemoteGainDb(float db) { m_remoteGain.store(db <= -60.0f ? 0.0f : dbToLin(std::min(12.0f, db)), std::memory_order_relaxed); }
void TtsAudio::setLocalGainDb(float db)  { m_localGain.store(db <= -60.0f ? 0.0f : dbToLin(std::min(12.0f, db)), std::memory_order_relaxed); }

void TtsAudio::setJitterMs(int ms)
{
    ms = std::max(40, std::min(1000, ms));
    m_jitterFrames.store(kRate * ms / 1000, std::memory_order_relaxed);
}

bool TtsAudio::remoteActive() const
{
    if (m_rawRemote.load(std::memory_order_relaxed) > 0) return true;
    if (m_blocksRemote.load(std::memory_order_relaxed) > 0) return true;
    if (m_pumpRemoteBusy.load(std::memory_order_relaxed)) return true;
    return nowMs() - m_lastRemoteMs.load(std::memory_order_relaxed) < 250;
}

bool TtsAudio::anyActive() const
{
    return m_raw.readAvailable() > 0 || m_blocks.readAvailable() > 0 || remoteActive() ||
           nowMs() - m_lastAnyMs.load(std::memory_order_relaxed) < 250;
}

bool TtsAudio::captureRunning() const
{
    return nowMs() - m_lastCaptureMs.load(std::memory_order_relaxed) < kStaleMs;
}

float TtsAudio::outputPeak() const
{
    return m_peak.load(std::memory_order_relaxed);
}

float TtsAudio::dspCpuPercent()
{
    return static_cast<float>(m_dsp.cpuPercent());
}

// ===========================================================================
// Pump
// ===========================================================================
void TtsAudio::pumpLoop()
{
    while (m_running.load(std::memory_order_relaxed)) {
        bool progressed = false;
        // Produce as many blocks as the target allows in one go.
        for (int i = 0; i < kTargetBlocks && pumpOnce(); ++i)
            progressed = true;
        if (!progressed) {
            std::unique_lock<std::mutex> lk(m_wakeMutex);
            m_wake.wait_for(lk, std::chrono::milliseconds(4));
        }
    }
}

bool TtsAudio::endPendingInRaw() const
{
    Seg s;
    for (size_t i = 0; i < 4096 && m_segs.peek(i, s); ++i)
        if (s.flags & kFlagEnd) return true;
    return false;
}

void TtsAudio::resetChain()
{
    std::lock_guard<std::mutex> g(m_dspMutex);
    m_dsp.reset();
    m_pitch.reset();
}

bool TtsAudio::pumpOnce()
{
    const uint32_t minJob = m_minJob.load(std::memory_order_acquire);
    const uint32_t epoch = m_flushEpoch.load(std::memory_order_acquire);
    const int64_t now = nowMs();

    // ---- flush: discard stale input, cut the chain's ring-out ----------------
    if (epoch != m_pumpEpoch) {
        m_pumpEpoch = epoch;
        Seg s;
        if (m_haveSeg && m_seg.job < minJob) {
            m_raw.discard(m_segLeft);
            if (!(m_seg.flags & kFlagLocal)) m_rawRemote.fetch_sub(m_segLeft, std::memory_order_relaxed);
            m_haveSeg = false;
        }
        while (m_segs.peek(0, s) && s.job < minJob) {
            m_segs.read(&s, 1);
            if (s.flags & kFlagEnd) continue;
            m_raw.discard(s.count);
            if (!(s.flags & kFlagLocal)) m_rawRemote.fetch_sub(s.count, std::memory_order_relaxed);
        }
        m_inUtterance = false;
        m_tailBlocks = 0;
        m_quietBlocks = 0;
        m_ramp = 0.0f;
        m_notBeforeMs = 0;
        m_waitStartMs = 0;
        m_pumpRemoteBusy.store(false, std::memory_order_relaxed);
        resetChain();
    }

    if (static_cast<int>(m_blocks.readAvailable()) >= kTargetBlocks || m_blocks.writeAvailable() == 0)
        return false;

    const size_t rawAvail = m_raw.readAvailable();
    const int fill = static_cast<int>(m_blocks.readAvailable());
    const bool endPending = endPendingInRaw();

    // ---- decide whether to take input now -------------------------------------
    bool takeInput = false;
    if (rawAvail > 0 || endPending) {
        if (m_inUtterance) {
            // Mid-utterance: wait for a full block unless playout is running dry.
            takeInput = rawAvail >= static_cast<size_t>(kBlock) || endPending || fill <= kLowWaterBlocks;
        } else {
            // Start of an utterance (or after an underrun): jitter buffer, and a
            // natural pause after the previous message.
            if (m_waitStartMs == 0) m_waitStartMs = now;
            const bool buffered = rawAvail >= static_cast<size_t>(m_jitterFrames.load(std::memory_order_relaxed));
            const bool waitedEnough = now - m_waitStartMs > 350;
            takeInput = (buffered || endPending || waitedEnough) && now >= m_notBeforeMs;
        }
    }

    Block blk;
    uint8_t flags = m_lastFlags;
    uint32_t job = m_lastJob;
    int got = 0;
    bool hitEnd = false;

    if (takeInput) {
        while (got < kBlock) {
            if (!m_haveSeg) {
                if (!m_segs.read(&m_seg, 1)) break;
                m_haveSeg = true;
                m_segLeft = m_seg.count;
            }
            if (m_seg.flags & kFlagEnd) {
                m_haveSeg = false;
                if (m_seg.job < minJob) continue;
                if (got > 0 || m_inUtterance) { hitEnd = true; break; }
                continue;
            }
            if (m_seg.job < minJob) {
                m_raw.discard(m_segLeft);
                if (!(m_seg.flags & kFlagLocal)) m_rawRemote.fetch_sub(m_segLeft, std::memory_order_relaxed);
                m_haveSeg = false;
                continue;
            }
            const uint8_t segLocal = m_seg.flags & kFlagLocal;
            if (got > 0 && (m_seg.job != job || segLocal != (flags & kFlagLocal)))
                break;   // blocks are homogeneous: one job, one routing
            job = m_seg.job;
            flags = segLocal;
            const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(kBlock - got), m_segLeft);
            const size_t r = m_raw.read(m_tmpMono.data() + got, n);
            got += static_cast<int>(r);
            m_segLeft -= static_cast<uint32_t>(r);
            if (!segLocal) m_rawRemote.fetch_sub(static_cast<int64_t>(r), std::memory_order_relaxed);
            if (m_segLeft == 0) m_haveSeg = false;
            if (r < n) break;   // cannot happen (samples precede descriptors), defensive
        }
    }

    const bool pitchOn = std::fabs(m_pitchRatio.load(std::memory_order_relaxed) - 1.0f) > 0.0005f;

    if (got == 0 && !hitEnd && m_tailBlocks <= 0) {
        // No input and nothing ringing out: idle.
        m_pumpRemoteBusy.store(false, std::memory_order_relaxed);
        return false;
    }

    if (got > 0) {
        if (!m_inUtterance) {
            m_inUtterance = true;
            m_waitStartMs = 0;
        }
        m_tailBlocks = 0;
        m_quietBlocks = 0;
        m_lastJob = job;
        m_lastFlags = flags;
        if (!(flags & kFlagLocal)) m_pumpRemoteBusy.store(true, std::memory_order_relaxed);
    }

    // ---- build the mono float block -----------------------------------------------
    const float vg = m_voiceGain.load(std::memory_order_relaxed);
    for (int i = 0; i < kBlock; ++i)
        m_tmpFloat[i] = (i < got) ? (m_tmpMono[i] / 32768.0f) * vg : 0.0f;

    if (got > 0 && m_ramp < 1.0f) {
        const int n = std::min(got, kRampFrames);
        for (int i = 0; i < n; ++i) m_tmpFloat[i] *= static_cast<float>(i) / n;
        m_ramp = 1.0f;
    }
    const bool underrun = got > 0 && got < kBlock && !hitEnd;
    if (underrun) {
        const int n = std::min(got, kRampFrames);
        for (int i = 0; i < n; ++i) m_tmpFloat[got - n + i] *= 1.0f - static_cast<float>(i + 1) / n;
        m_ramp = 0.0f;
        m_inUtterance = false;          // re-arm the jitter buffer
    }

    // ---- pitch + effects chain -------------------------------------------------------
    {
        std::lock_guard<std::mutex> g(m_dspMutex);
        if (pitchOn) {
            m_pitch.setRatio(m_pitchRatio.load(std::memory_order_relaxed));
            for (int i = 0; i < kBlock; ++i) m_tmpFloat[i] = m_pitch.process(m_tmpFloat[i]);
        }
        for (int i = 0; i < kBlock; ++i) {
            const short v = clampS(static_cast<int>(std::lrint(m_tmpFloat[i] * 32767.0f)));
            blk.pcm[i * 2] = v;
            blk.pcm[i * 2 + 1] = v;
        }
        if (m_dsp.isActive()) {
            float pl = 0.0f, pr = 0.0f;
            m_dsp.process(blk.pcm, kBlock, 2, pl, pr, /*isCapture=*/true);
        }
    }
    blk.job = job;
    blk.flags = flags;

    int peak = 0;
    for (int i = 0; i < kBlock * 2; ++i) peak = std::max(peak, std::abs(static_cast<int>(blk.pcm[i])));

    // m_dsp.isActive() just means "the sandbox is on", true for e.g. a
    // lone EQ/Compressor with no tail at all - that used to arm the
    // FULL kMaxTailBlocks (3 s) hold for ANY enabled stage, so the mic/
    // transmission stayed open for up to 3 s after every single phrase
    // whenever any effect was on, not only genuine tail effects. Only
    // stages that can actually still be producing sound from silent
    // input (reverb/delay tails, resonant modulation feedback, VoiceFx's
    // own pitch/formant grain windowing) now arm the ring-out; anything
    // else cuts immediately with the phrase, same as no DSP at all.
    const SandboxState &ds = m_dsp.state();
    const bool hasTailStage = m_dsp.isActive() && (
        ds.reverbWet > 0.001f ||
        (ds.delayEnabled   && ds.delayMix   > 0.001f) ||
        (ds.chorusEnabled  && ds.chorusMix  > 0.001f) ||
        (ds.flangerEnabled && ds.flangerMix > 0.001f) ||
        (ds.flangusEnabled && ds.flangusMix > 0.001f) ||
        (ds.phaserEnabled  && ds.phaserMix  > 0.001f) ||
        ds.vfxEnabled);
    const bool chainRings = hasTailStage || pitchOn;
    if (hitEnd) {
        m_inUtterance = false;
        m_ramp = 0.0f;
        m_tailBlocks = chainRings ? kMaxTailBlocks : 0;
        m_quietBlocks = 0;
        m_notBeforeMs = now + fill * 10 + kGapMs;
        m_waitStartMs = 0;
    } else if (got == 0) {
        // Ring-out block.
        --m_tailBlocks;
        m_quietBlocks = (peak < 8) ? m_quietBlocks + 1 : 0;
        if (m_quietBlocks >= 20) m_tailBlocks = 0;
        if (m_tailBlocks <= 0) m_pumpRemoteBusy.store(false, std::memory_order_relaxed);
    }

    m_blocks.write(&blk, 1);
    if (!(flags & kFlagLocal)) m_blocksRemote.fetch_add(1, std::memory_order_relaxed);
    if (hitEnd && !chainRings) m_pumpRemoteBusy.store(false, std::memory_order_relaxed);
    return true;
}

// ===========================================================================
// Readers
// ===========================================================================
int TtsAudio::readLocked(int16_t *stereo, uint8_t *flags, int frames, bool &anyRemote)
{
    const uint32_t epoch = m_flushEpoch.load(std::memory_order_acquire);
    const uint32_t minJob = m_minJob.load(std::memory_order_acquire);
    if (epoch != m_readerEpoch) {
        m_readerEpoch = epoch;
        // TeamSpeak blocks (960) are whole multiples of ours (480), so a stop
        // usually lands exactly between blocks with no "current" block. Promote
        // the next queued block so there is always something to fade: cutting to
        // zero after a mid-waveform sample is an audible click (selftest 3).
        if (!m_haveCur && m_blocks.read(&m_cur, 1)) {
            if (!(m_cur.flags & kFlagLocal)) m_blocksRemote.fetch_sub(1, std::memory_order_relaxed);
            m_haveCur = true;
            m_curOff = 0;
        }
        m_fadeLeft = (m_haveCur && m_cur.job < minJob) ? kFadeFrames : 0;
        // Everything else already queued and stale goes now; the block being
        // played fades out over 5 ms instead of clicking.
        Block b;
        while (m_blocks.peek(0, b) && b.job < minJob) {
            m_blocks.read(&b, 1);
            if (!(b.flags & kFlagLocal)) m_blocksRemote.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    int out = 0;
    anyRemote = false;
    while (out < frames) {
        if (!m_haveCur) {
            if (!m_blocks.read(&m_cur, 1)) break;
            if (!(m_cur.flags & kFlagLocal)) m_blocksRemote.fetch_sub(1, std::memory_order_relaxed);
            if (m_cur.job < minJob) continue;
            m_haveCur = true;
            m_curOff = 0;
            if (m_starved) {                       // audio is back after running dry
                m_rampInLeft = kRampFrames;
                m_starved = false;
            }
        }
        const bool stale = m_cur.job < minJob;
        if (stale && m_fadeLeft <= 0) {
            m_haveCur = false;
            continue;
        }
        const int n = std::min(frames - out, kBlock - m_curOff);
        for (int i = 0; i < n; ++i) {
            int l = m_cur.pcm[(m_curOff + i) * 2];
            int r = m_cur.pcm[(m_curOff + i) * 2 + 1];
            if (stale) {
                const float g = m_fadeLeft > 0 ? static_cast<float>(m_fadeLeft) / kFadeFrames : 0.0f;
                l = static_cast<int>(l * g);
                r = static_cast<int>(r * g);
                if (m_fadeLeft > 0) --m_fadeLeft;
            }
            if (m_rampInLeft > 0) {
                const float g = 1.0f - static_cast<float>(m_rampInLeft) / kRampFrames;
                l = static_cast<int>(l * g);
                r = static_cast<int>(r * g);
                --m_rampInLeft;
            }
            if (m_decayLeft > 0) {
                const float g = static_cast<float>(m_decayLeft - 1) / kRampFrames;
                l += static_cast<int>(m_decayL * g);
                r += static_cast<int>(m_decayR * g);
                --m_decayLeft;
            }
            stereo[(out + i) * 2] = clampS(l);
            stereo[(out + i) * 2 + 1] = clampS(r);
            flags[out + i] = m_cur.flags;
            m_lastL = clampS(l);
            m_lastR = clampS(r);
        }
        m_lastReadFlags = m_cur.flags;
        if (!(m_cur.flags & kFlagLocal)) anyRemote = true;
        out += n;
        m_curOff += n;
        if (m_curOff >= kBlock || (stale && m_fadeLeft <= 0)) m_haveCur = false;
    }
    // Ran dry mid-waveform: decay the last sample instead of stepping to zero. The
    // decay keeps the last block's routing so the channel receives it too.
    if (out < frames && m_decayLeft == 0 && !m_starved && (m_lastL != 0 || m_lastR != 0)) {
        m_decayLeft = kRampFrames;
        m_decayL = m_lastL;
        m_decayR = m_lastR;
        m_lastL = m_lastR = 0;
        m_starved = true;
    }
    for (; out < frames && m_decayLeft > 0; ++out) {
        const float g = static_cast<float>(m_decayLeft - 1) / kRampFrames;
        stereo[out * 2] = static_cast<int16_t>(m_decayL * g);
        stereo[out * 2 + 1] = static_cast<int16_t>(m_decayR * g);
        flags[out] = m_lastReadFlags;
        if (!(m_lastReadFlags & kFlagLocal)) anyRemote = true;
        --m_decayLeft;
    }
    for (int i = out; i < frames; ++i) {
        stereo[i * 2] = 0;
        stereo[i * 2 + 1] = 0;
        flags[i] = kFlagLocal;
    }
    return out;
}

int TtsAudio::readAs(Reader who, int16_t *stereo, uint8_t *flags, int frames, bool &anyRemote)
{
    SpinGuard g(m_readLock);
    const int64_t now = nowMs();
    int owner = m_owner.load(std::memory_order_relaxed);
    const bool captureFresh = now - m_lastCaptureMs.load(std::memory_order_relaxed) < kStaleMs;
    const bool playbackFresh = now - m_lastPlaybackMs.load(std::memory_order_relaxed) < kStaleMs;

    // Clock hand-over: capture is the preferred clock (it is what the channel
    // hears), playback takes over when capture stops (PTT up / no mic), the
    // local waveOut player only when TeamSpeak delivers no audio at all.
    bool mine = false;
    switch (who) {
    case ReaderCapture:  mine = true; break;
    case ReaderPlayback: mine = !captureFresh; break;
    case ReaderFallback: mine = !captureFresh && !playbackFresh; break;
    default: break;
    }
    if (!mine) {
        anyRemote = false;
        return 0;
    }
    if (owner != who) m_owner.store(who, std::memory_order_relaxed);
    const int got = readLocked(stereo, flags, frames, anyRemote);
    if (got > 0) m_lastAnyMs.store(now, std::memory_order_relaxed);
    if (anyRemote) m_lastRemoteMs.store(now, std::memory_order_relaxed);
    return got;
}

bool TtsAudio::onCapture(uint64_t sch, short *samples, int frames, int channels, bool clientWillSend)
{
    if (sch != m_activeServer.load(std::memory_order_relaxed) || frames <= 0 || channels < 1)
        return false;
    const int64_t now = nowMs();
    m_lastCaptureMs.store(now, std::memory_order_relaxed);

    const int16_t *stereo = m_capStereo.data();
    const uint8_t *flags = m_capFlags.data();
    const int n = std::min(frames, kMaxCallbackFrames);

    bool anyRemote = false;
    const int got = readAs(ReaderCapture, m_capStereo.data(), m_capFlags.data(), n, anyRemote);
    const bool preview = m_previewOnly.load(std::memory_order_relaxed);
    const bool transmit = anyRemote && !preview;

    bool edited = false;

    // ---- real-time voice changer ------------------------------------------------------
    // While enabled the raw microphone NEVER reaches the channel. It is forwarded to the
    // backend only while TeamSpeak would transmit it (PTT down / voice detected, plus a
    // short tail), and the block is replaced by the converted voice (~0.25 s later) or
    // by silence. The TTS logic below then treats the converted voice as "the mic".
    const bool vcOn = m_vcEnabled.load(std::memory_order_relaxed);
    if (vcOn != m_vcWasEnabled) {
        m_vcWasEnabled = vcOn;
        m_vcDown.discard(m_vcDown.readAvailable());   // nothing stale from a previous session
        m_vcPlaying = false;
        m_vcGain = 0.0f;
    }
    if (vcOn) {
        int16_t *vc = m_vcScratch.data();
        const bool intent = clientWillSend && !m_vcHoldOverride.load(std::memory_order_relaxed);
        if (intent) {
            m_vcIntentUntilMs = now + kVcIntentTailMs;
            m_vcLastIntentMs.store(now, std::memory_order_relaxed);
        }
        float inPeak = 0.0f;
        if (now < m_vcIntentUntilMs) {
            for (int i = 0; i < n; ++i) {
                const int s = channels == 1 ? samples[i] : (samples[i * channels] + samples[i * channels + 1]) / 2;
                vc[i] = static_cast<int16_t>(s);
                inPeak = std::max(inPeak, std::abs(s) / 32768.0f);
            }
            if (m_vcUp.write(vc, static_cast<size_t>(n)) < static_cast<size_t>(n))
                m_vcDropped.fetch_add(1, std::memory_order_relaxed);
        }
        m_vcInPeak.store(inPeak, std::memory_order_relaxed);

        size_t have = 0;
        const size_t avail = m_vcDown.readAvailable();
        if (!m_vcPlaying && avail >= kVcPrebufferFrames) m_vcPlaying = true;
        if (m_vcPlaying) {
            // Keep a 2 ms reserve: if the ring would run dry exactly at the end of this
            // block, the NEXT block would start in silence with nothing left to fade
            // (selftest: 1058-LSB step). Deliver what is there and fade its tail now.
            const bool lastBlock = avail < static_cast<size_t>(n) + kVcRampFrames;
            have = m_vcDown.read(vc, static_cast<size_t>(n));
            if (lastBlock || have < static_cast<size_t>(n)) {
                // Underrun / end of phrase: fade the delivered tail, re-arm the pre-buffer.
                const size_t ramp = std::min<size_t>(kVcRampFrames, have);
                for (size_t k = 0; k < ramp; ++k)
                    vc[have - ramp + k] = static_cast<int16_t>(vc[have - ramp + k] * (float(ramp - k) / float(ramp)));
                m_vcPlaying = false;
            }
        }
        float outPeak = 0.0f;
        for (int i = 0; i < n; ++i) {
            int s = 0;
            if (static_cast<size_t>(i) < have) {
                if (m_vcGain < 1.0f) m_vcGain = std::min(1.0f, m_vcGain + 1.0f / kVcRampFrames);
                s = static_cast<int>(vc[i] * m_vcGain);
                outPeak = std::max(outPeak, std::abs(s) / 32768.0f);
            }
            for (int c = 0; c < channels; ++c) samples[i * channels + c] = static_cast<short>(s);
        }
        if (!m_vcPlaying) m_vcGain = 0.0f;       // a new start ramps in
        for (int i = n; i < frames; ++i)
            for (int c = 0; c < channels; ++c) samples[i * channels + c] = 0;
        m_vcOutPeak.store(outPeak, std::memory_order_relaxed);
        if (outPeak > 0.003f) m_vcLastOutputMs.store(now, std::memory_order_relaxed);
        if (m_vcMonitor.load(std::memory_order_relaxed) && have > 0 && got <= 0) {
            int16_t *mon = m_capMonitor.data();
            for (size_t i = 0; i < have; ++i) mon[i * 2] = mon[i * 2 + 1] = samples[i * channels];
            m_monitorRing.write(mon, have * 2);
        }
        edited = true;
    }

    // Microphone handling. A short hold after the last TTS sample keeps the
    // mic from popping in between sentences.
    const bool ttsRecently = now - m_lastRemoteMs.load(std::memory_order_relaxed) < 600;
    const int micMode = m_micMode.load(std::memory_order_relaxed);
    const GbBridge::MicPolicy wantMicPolicy =
        (m_forceMicSilence.load(std::memory_order_relaxed) || (!preview && ttsRecently && micMode == MicReplace))
            ? GbBridge::MicPolicy::Silence
        : (!preview && ttsRecently && micMode == MicDuck)
            ? GbBridge::MicPolicy::Duck
            : GbBridge::MicPolicy::None;

    // The Soundboard plugin, if also installed and loaded, is the single
    // writer of the shared TS3 capture buffer (see ipc/GbAudioBridge.h) -
    // two independent plugins each blindly zeroing/overwriting the SAME
    // host buffer is exactly the "soundboard playback overwrites the TTS
    // and vice versa" bug the bridge exists to fix. Falls straight back
    // to writing `samples` directly (unchanged original behaviour) the
    // instant the Soundboard is not present or its heartbeat goes stale.
    GbBridge::Shared *bridge = m_bridge.get();
    const bool soundboardPresent = bridge && bridge->soundboardFresh();

    if (!soundboardPresent) {
        if (wantMicPolicy == GbBridge::MicPolicy::Silence) {
            std::fill_n(samples, static_cast<size_t>(frames) * channels, static_cast<short>(0));
            edited = true;
        } else if (wantMicPolicy == GbBridge::MicPolicy::Duck) {
            for (int i = 0; i < frames * channels; ++i) samples[i] = static_cast<short>(samples[i] / 4);
            edited = true;
        }
    }

    float peak = m_peak.load(std::memory_order_relaxed) * 0.85f;
    bool anyAudible = false;
    if (got > 0) {
        const float rg = m_remoteGain.load(std::memory_order_relaxed);
        for (int i = 0; i < got; ++i) {
            const int l = stereo[i * 2], r = stereo[i * 2 + 1];
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)) / 32768.0f);
            if (!transmit || (flags[i] & kFlagLocal)) continue;
            anyAudible = true;
            if (soundboardPresent) {
                m_bridgePub[i * 2]     = clampS(static_cast<int>(l * rg));
                m_bridgePub[i * 2 + 1] = clampS(static_cast<int>(r * rg));
                continue;
            }
            if (channels == 1) {
                samples[i] = clampS(samples[i] + static_cast<int>(((l + r) * 0.5f) * rg));
            } else {
                samples[i * channels] = clampS(samples[i * channels] + static_cast<int>(l * rg));
                samples[i * channels + 1] = clampS(samples[i * channels + 1] + static_cast<int>(r * rg));
            }
        }
        if (!soundboardPresent && transmit) edited = true;

        // Local monitor: preview frames always, channel frames when enabled.
        const bool monitor = m_monitor.load(std::memory_order_relaxed);
        if (monitor || !anyRemote || preview) {
            int16_t *mon = m_capMonitor.data();
            for (int i = 0; i < got; ++i) {
                const bool hear = (flags[i] & kFlagLocal) || monitor || preview;
                mon[i * 2] = hear ? stereo[i * 2] : 0;
                mon[i * 2 + 1] = hear ? stereo[i * 2 + 1] : 0;
            }
            m_monitorRing.write(mon, static_cast<size_t>(got) * 2);
        }
    }
    if (soundboardPresent) {
        // Published every tick (not just while got > 0) so the mic
        // policy stays correctly applied through the whole 600 ms
        // "ttsRecently" trailing window, not just while new blocks are
        // actually being produced - otherwise the Soundboard's read
        // would go stale ~250 ms into that window and the mic would
        // pop back early even though TTS still wants it held.
        if (!anyAudible) std::fill_n(m_bridgePub.data(), static_cast<size_t>(n) * 2, static_cast<int16_t>(0));
        bridge->publishTts(n, 2, wantMicPolicy, anyAudible, m_bridgePub.data());
    }
    m_peak.store(peak, std::memory_order_relaxed);
    return edited;
}

void TtsAudio::onPlayback(uint64_t sch, short *samples, int frames, int channels,
                          const unsigned int *speakers, unsigned int *fillMask)
{
    if (sch != m_activeServer.load(std::memory_order_relaxed) || frames <= 0 || channels < 1 || !fillMask)
        return;
    m_lastPlaybackMs.store(nowMs(), std::memory_order_relaxed);

    int16_t *stereo = m_pbStereo.data();
    const int n = std::min(frames, kMaxCallbackFrames);

    int got = 0;
    bool anyRemote = false;
    got = readAs(ReaderPlayback, stereo, m_pbFlags.data(), n, anyRemote);
    if (got > 0) {
        // Playback is the clock (capture idle): nothing reaches the channel, so
        // everything is played locally.
        float peak = m_peak.load(std::memory_order_relaxed) * 0.85f;
        for (int i = 0; i < got * 2; ++i) peak = std::max(peak, std::abs(stereo[i]) / 32768.0f);
        m_peak.store(peak, std::memory_order_relaxed);
    } else {
        // Capture is the clock: drain what it queued for the monitor, keeping the
        // backlog bounded so the monitor never drifts behind the channel.
        size_t avail = m_monitorRing.readAvailable() / 2;
        if (avail > static_cast<size_t>(kRate / 5)) {
            m_monitorRing.discard((avail - kRate / 10) * 2);
            avail = kRate / 10;
        }
        got = static_cast<int>(m_monitorRing.read(stereo, std::min<size_t>(avail, static_cast<size_t>(n)) * 2) / 2);
    }
    if (got <= 0) return;

    const unsigned int bmL = SPEAKER_FRONT_LEFT | SPEAKER_HEADPHONES_LEFT;
    const unsigned int bmR = SPEAKER_FRONT_RIGHT | SPEAKER_HEADPHONES_RIGHT;
    int ciL = 0, ciR = channels >= 2 ? 1 : 0;
    if (speakers) {
        for (int i = 0; i < channels; ++i) if (speakers[i] & bmL) { ciL = i; break; }
        for (int i = 0; i < channels; ++i) if (speakers[i] & bmR) { ciR = i; break; }
    }
    const bool overL = (*fillMask & bmL) == 0;
    const bool overR = (*fillMask & bmR) == 0;
    const float lg = m_localGain.load(std::memory_order_relaxed);

    for (int i = 0; i < frames; ++i) {
        const int l = i < got ? static_cast<int>(stereo[i * 2] * lg) : 0;
        const int r = i < got ? static_cast<int>(stereo[i * 2 + 1] * lg) : 0;
        short &dl = samples[i * channels + ciL];
        dl = overL ? clampS(l) : clampS(dl + l);
        if (ciR != ciL) {
            short &dr = samples[i * channels + ciR];
            dr = overR ? clampS(r) : clampS(dr + r);
        }
    }
    *fillMask |= (bmL | bmR);
}

bool TtsAudio::fallbackWanted() const
{
    const int64_t now = nowMs();
    const bool silentHost = now - m_lastCaptureMs.load(std::memory_order_relaxed) >= kStaleMs &&
                            now - m_lastPlaybackMs.load(std::memory_order_relaxed) >= kStaleMs;
    return silentHost && (m_blocks.readAvailable() > 0 || m_raw.readAvailable() > 0 || m_haveCur);
}

int TtsAudio::renderFallback(int16_t *stereo, int frames)
{
    frames = std::min(frames, kMaxCallbackFrames);
    bool anyRemote = false;
    const int got = readAs(ReaderFallback, stereo, m_fbFlags.data(), frames, anyRemote);
    if (got <= 0) return 0;
    const float lg = m_localGain.load(std::memory_order_relaxed);
    float peak = m_peak.load(std::memory_order_relaxed) * 0.85f;
    for (int i = 0; i < frames * 2; ++i) {
        peak = std::max(peak, std::abs(stereo[i]) / 32768.0f);
        stereo[i] = clampS(static_cast<int>(stereo[i] * lg));
    }
    m_peak.store(peak, std::memory_order_relaxed);
    return got;
}

} // namespace gbtts
