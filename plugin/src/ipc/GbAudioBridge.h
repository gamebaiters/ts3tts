#pragma once
// Shared-memory bridge between the GameBaiters Soundboard and TTS
// plugins. Both are separate DLLs that can be loaded into the SAME
// ts3client_win64.exe process; TS3 chains each loaded plugin's own
// ts3plugin_onEditCapturedVoiceDataEvent hook through the SAME
// `short *samples` buffer, in whatever order it happened to load the
// plugins. Without coordination, each plugin's own "replace the mic
// while I'm talking" logic blindly zeroes/overwrites whatever the
// OTHER plugin already wrote there - "soundboard playback overwrites
// the TTS, and the TTS likely overwrites the soundboard" (both true,
// depending on load order).
//
// Contract: the Soundboard is authoritative when BOTH plugins are
// present. TTS publishes its per-block audio + desired mic policy here
// on every capture callback and does NOT touch the host capture buffer
// itself once it sees the Soundboard's heartbeat is fresh (see
// TtsAudio::onCapture). The Soundboard's own capture callback, after
// its normal mic/VAD/slot-mix logic, applies TTS's published mic
// policy and adds TTS's published (already fully DSP-processed) audio
// as the FINAL step before returning - i.e. the TTS chain, effects
// included, is inserted at the very end of the soundboard's own audio
// pipeline, right before it hands the buffer back to TeamSpeak.
//
// This works regardless of which plugin's callback TS3 invokes first
// for a given block: whichever runs second just finds the buffer
// already correctly combined (or, for TTS, finds Soundboard is present
// and skips its own writes). Either plugin instantly reverts to its
// original solo behaviour the moment the other's heartbeat goes stale
// (not installed, not loaded yet, or just unloaded) - the single-
// plugin case is completely unaffected.
//
// KEEP THIS FILE BYTE-FOR-BYTE IDENTICAL IN BOTH REPOS
// (SOUNDBOARD_4.0/upstream-clone/src/ipc/ and "TTS TS3"/plugin/src/ipc/).
// Bump kAbiVersion (and validate it on open) if the layout ever changes.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GbBridge {

constexpr uint32_t kAbiVersion = 1;
#ifdef _WIN32
constexpr wchar_t kMappingName[] = L"Local\\GameBaiters_TS3_AudioBridge_v1";
#endif
// >= the largest capture callback either plugin ever services
// (TS3 delivers 960 frames/20 ms in practice; both plugins cap their own
// scratch buffers at 8192 as a safety margin - matched here).
constexpr int kMaxFrames = 8192;
constexpr int64_t kStaleMs = 250;   // heartbeat older than this = "not present"

enum class MicPolicy : uint8_t { None = 0, Duck = 1, Silence = 2 };

inline int64_t nowMsSteady()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#pragma pack(push, 1)
struct Shared {
    uint32_t abiVersion;
    uint32_t structSize;

    std::atomic<int64_t> soundboardHeartbeatMs;
    std::atomic<int64_t> ttsHeartbeatMs;

    // TTS -> Soundboard, republished every TTS capture callback (so a
    // stale read by the Soundboard is at most one ~20 ms tick behind -
    // self-correcting every subsequent tick, never a real-world issue
    // for audio glue like this). Seqlock-style: odd = writer mid-
    // update, even = a complete, consistent snapshot is in place.
    std::atomic<uint32_t> ttsSeq;
    int32_t ttsFrames;
    int32_t ttsChannels;
    uint8_t ttsMicPolicy;   // MicPolicy
    uint8_t ttsActive;      // 0/1 - true only while TTS has something to contribute
    int16_t ttsPcm[kMaxFrames * 2];   // interleaved, up to stereo

    // Publish this block's audio + desired mic policy. Called every TTS
    // capture callback, active or not, so the heartbeat alone already
    // doubles as presence detection even while TTS is silent.
    void publishTts(int32_t frames, int32_t channels, MicPolicy policy,
                     bool active, const int16_t *pcm)
    {
        const uint32_t seq = ttsSeq.load(std::memory_order_relaxed);
        ttsSeq.store(seq + 1, std::memory_order_release);   // odd: writing
        ttsFrames = frames;
        ttsChannels = channels;
        ttsMicPolicy = static_cast<uint8_t>(policy);
        ttsActive = active ? 1 : 0;
        if (pcm && frames > 0 && channels > 0) {
            const size_t n = std::min<size_t>(
                static_cast<size_t>(frames) * static_cast<size_t>(channels),
                static_cast<size_t>(kMaxFrames) * 2);
            std::memcpy(ttsPcm, pcm, n * sizeof(int16_t));
        }
        ttsHeartbeatMs.store(nowMsSteady(), std::memory_order_relaxed);
        ttsSeq.store(seq + 2, std::memory_order_release);   // even: done
    }

    // Copies out a fresh, consistent snapshot. Returns false only when TTS
    // is absent/stale (nothing to read at all) - the caller must still
    // check the returned `active` before mixing `outPcm` in: the mic
    // POLICY stays valid through TTS's whole trailing "just finished
    // talking" hold even on ticks where `active` is false (no new audio
    // that exact tick), so a caller that bailed out on `active` alone
    // would drop the mic policy up to ~600 ms early and let the real mic
    // pop back in before TTS intended.
    bool readTts(int16_t *outPcm, int32_t maxSamples, int32_t &frames,
                 int32_t &channels, MicPolicy &policy, bool &active) const
    {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t s1 = ttsSeq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;   // writer mid-update, retry
            if (nowMsSteady() - ttsHeartbeatMs.load(std::memory_order_relaxed) > kStaleMs)
                return false;
            frames = ttsFrames;
            channels = ttsChannels;
            policy = static_cast<MicPolicy>(ttsMicPolicy);
            active = ttsActive != 0;
            const size_t n = std::min<size_t>(
                static_cast<size_t>(std::max(0, frames)) * static_cast<size_t>(std::max(0, channels)),
                static_cast<size_t>(std::max(0, maxSamples)));
            if (outPcm && n > 0) std::memcpy(outPcm, ttsPcm, n * sizeof(int16_t));
            const uint32_t s2 = ttsSeq.load(std::memory_order_acquire);
            if (s1 != s2) continue;   // torn read, retry
            return true;
        }
        return false;
    }

    bool soundboardFresh() const
    {
        return nowMsSteady() - soundboardHeartbeatMs.load(std::memory_order_relaxed) < kStaleMs;
    }
    bool ttsFresh() const
    {
        return nowMsSteady() - ttsHeartbeatMs.load(std::memory_order_relaxed) < kStaleMs;
    }
};
#pragma pack(pop)

// Opens (creating on first use) the process-wide shared mapping. Each
// plugin keeps exactly one Handle alive for its own lifetime (e.g. a
// function-local static in a free accessor - see soundboardBridge() /
// ttsBridge() callers). Never throws; get() returns nullptr on any
// failure and every caller treats that exactly like "the other plugin
// is not installed" (falls back to solo behaviour).
class Handle {
public:
    Handle() = default;
    ~Handle() { close(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    Shared *get()
    {
        if (m_view) return m_view;
#ifdef _WIN32
        if (!m_map) {
            m_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(Shared), kMappingName);
            if (!m_map) return nullptr;
        }
        const bool firstOpen = (GetLastError() != ERROR_ALREADY_EXISTS);
        void *view = MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
        if (!view) { CloseHandle(m_map); m_map = nullptr; return nullptr; }
        auto *s = static_cast<Shared *>(view);
        if (firstOpen) {
            std::memset(s, 0, sizeof(Shared));
            s->abiVersion = kAbiVersion;
            s->structSize = sizeof(Shared);
        } else if (s->abiVersion != kAbiVersion || s->structSize != sizeof(Shared)) {
            // A mismatched build somehow got here first - never trust a
            // layout we do not recognise. Both plugins just fall back
            // to solo behaviour, same as if the mapping did not exist.
            UnmapViewOfFile(view);
            CloseHandle(m_map);
            m_map = nullptr;
            return nullptr;
        }
        m_view = s;
        return m_view;
#else
        return nullptr;
#endif
    }

    void close()
    {
#ifdef _WIN32
        if (m_view) { UnmapViewOfFile(m_view); m_view = nullptr; }
        if (m_map)  { CloseHandle(m_map); m_map = nullptr; }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_map = nullptr;
#endif
    Shared *m_view = nullptr;
};

} // namespace GbBridge
