#include "audio/LocalPlayer.h"
#include "audio/TtsAudio.h"

#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace gbtts {

LocalPlayer::LocalPlayer(TtsAudio *audio) : m_audio(audio) {}

LocalPlayer::~LocalPlayer()
{
    stop();
}

void LocalPlayer::start()
{
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void LocalPlayer::stop()
{
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

#ifdef _WIN32
void LocalPlayer::run()
{
    constexpr int kFrames = 960;          // 20 ms per buffer
    constexpr int kQueued = 4;            // ~80 ms device latency
    HWAVEOUT dev = nullptr;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<std::vector<int16_t>> bufs(kQueued, std::vector<int16_t>(kFrames * 2));
    std::vector<WAVEHDR> hdrs(kQueued);
    std::vector<bool> busy(kQueued, false);
    auto lastAudio = std::chrono::steady_clock::now();

    auto closeDevice = [&]() {
        if (!dev) return;
        waveOutReset(dev);
        for (int i = 0; i < kQueued; ++i) {
            if (hdrs[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(dev, &hdrs[i], sizeof(WAVEHDR));
            busy[i] = false;
        }
        waveOutClose(dev);
        dev = nullptr;
    };

    while (m_running.load(std::memory_order_relaxed)) {
        if (!dev) {
            if (!m_audio->fallbackWanted()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            WAVEFORMATEX fmt{};
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = 2;
            fmt.nSamplesPerSec = TtsAudio::kRate;
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = 4;
            fmt.nAvgBytesPerSec = TtsAudio::kRate * 4;
            if (waveOutOpen(&dev, WAVE_MAPPER, &fmt, static_cast<DWORD_PTR>(reinterpret_cast<intptr_t>(done)), 0,
                            CALLBACK_EVENT) != MMSYSERR_NOERROR) {
                dev = nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            lastAudio = std::chrono::steady_clock::now();
        }

        // Refill every finished buffer.
        bool any = false;
        for (int i = 0; i < kQueued; ++i) {
            if (busy[i] && (hdrs[i].dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(dev, &hdrs[i], sizeof(WAVEHDR));
                busy[i] = false;
            }
            if (busy[i]) continue;
            std::fill(bufs[i].begin(), bufs[i].end(), static_cast<int16_t>(0));
            if (m_audio->renderFallback(bufs[i].data(), kFrames) > 0) {
                any = true;
                lastAudio = std::chrono::steady_clock::now();
            }
            std::memset(&hdrs[i], 0, sizeof(WAVEHDR));
            hdrs[i].lpData = reinterpret_cast<LPSTR>(bufs[i].data());
            hdrs[i].dwBufferLength = kFrames * 4;
            waveOutPrepareHeader(dev, &hdrs[i], sizeof(WAVEHDR));
            waveOutWrite(dev, &hdrs[i], sizeof(WAVEHDR));
            busy[i] = true;
        }
        (void)any;

        if (std::chrono::steady_clock::now() - lastAudio > std::chrono::seconds(1)) {
            closeDevice();
            continue;
        }
        WaitForSingleObject(done, 30);
    }
    closeDevice();
    CloseHandle(done);
}
#else
void LocalPlayer::run()
{
    while (m_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
#endif

} // namespace gbtts
