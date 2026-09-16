// ---------------------------------------------------------------------------
// Offline self-test of the TtsAudio core, driven by fake TeamSpeak callbacks
// on a real-time clock (the core's staleness logic is time based).
//
//   build: cmake -DGBTTS_BUILD_TESTS=ON ...  -> gbtts_audio_selftest.exe
//
// What it proves without a TeamSpeak client:
//   1 continuity    injected audio == pushed audio, no discontinuities, mic replaced
//   2 routing       a local job never reaches the channel, reaches the monitor
//   3 stop          flush fades out within 5 ms and nothing stale plays afterwards
//   4 underrun      a producer slower than real time never produces clicks
//   5 activity      remoteActive() follows the utterance and releases afterwards
//   6 effects tail  with reverb the channel keeps transmitting the ring-out
//   7 clock handoff with no capture callbacks the playback callback plays it
// ---------------------------------------------------------------------------
#include "audio/TtsAudio.h"

#include "common.h"

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace gbtts;
using clk = std::chrono::steady_clock;

namespace {

int g_pass = 0, g_fail = 0;

void check(const char *name, bool ok, const char *fmt = "", double a = 0, double b = 0)
{
    std::printf("  %s %-46s ", ok ? "PASS" : "FAIL", name);
    std::printf(fmt, a, b);
    std::printf("\n");
    (ok ? g_pass : g_fail)++;
}

std::vector<int16_t> sine(double hz, double seconds, double amp = 0.4)
{
    std::vector<int16_t> v(static_cast<size_t>(seconds * 48000));
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<int16_t>(std::lrint(std::sin(2.0 * M_PI * hz * i / 48000.0) * amp * 32767.0));
    return v;
}

// Pushes `pcm` as job `id` at `pace` x real time from a producer thread.
std::thread producer(TtsAudio &a, uint32_t id, const std::vector<int16_t> &pcm, double pace, bool local)
{
    return std::thread([&a, id, pcm, pace, local] {
        const int chunk = 2400;
        auto start = clk::now();
        size_t sent = 0;
        while (sent < pcm.size()) {
            const int n = static_cast<int>(std::min<size_t>(chunk, pcm.size() - sent));
            while (!a.pushPcm(id, pcm.data() + sent, n, local)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            sent += n;
            const double due = (sent / 48000.0) / pace;
            std::this_thread::sleep_until(start + std::chrono::duration<double>(due));
        }
        while (!a.pushEnd(id, local)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
}

struct Capture {
    std::vector<int16_t> out;       // what the channel would receive (mono)
    std::vector<int16_t> monitor;   // what the local playback callback produced (L)
};

// Runs capture (+ optional playback) callbacks every 20 ms for `seconds`.
Capture runCallbacks(TtsAudio &a, double seconds, bool withCapture, int16_t micLevel,
                     const std::function<void(double)> &onTick = nullptr)
{
    Capture c;
    constexpr int frames = 960;
    std::vector<short> mic(frames), play(frames * 2);
    unsigned int speakers[2] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT};
    auto start = clk::now();
    int blocks = static_cast<int>(seconds * 50);
    for (int b = 0; b < blocks; ++b) {
        if (withCapture) {
            std::fill(mic.begin(), mic.end(), micLevel);
            a.onCapture(1, mic.data(), frames, 1);
            c.out.insert(c.out.end(), mic.begin(), mic.end());
        }
        std::fill(play.begin(), play.end(), 0);
        unsigned int mask = 0;
        a.onPlayback(1, play.data(), frames, 2, speakers, &mask);
        for (int i = 0; i < frames; ++i) c.monitor.push_back(play[i * 2]);
        if (onTick) onTick(b * 0.02);
        std::this_thread::sleep_until(start + std::chrono::milliseconds(20 * (b + 1)));
    }
    return c;
}

size_t firstNonZero(const std::vector<int16_t> &v, int16_t ignore = 0)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] != ignore) return i;
    return v.size();
}

int maxStep(const std::vector<int16_t> &v, size_t from, size_t to)
{
    int m = 0;
    for (size_t i = from + 1; i < std::min(to, v.size()); ++i) m = std::max(m, std::abs(v[i] - v[i - 1]));
    return m;
}

} // namespace

// TtsAudio embeds a full SlotDsp (two PathStates of inline DSP buffers): it is
// far too large for a thread stack. The plugin heap-allocates it (Controller);
// so must the test.
#define NEW_AUDIO(name) auto name##_owner = std::make_unique<TtsAudio>(); TtsAudio &name = *name##_owner

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    std::printf("GameBaiters TTS - audio core self-test (sizeof(TtsAudio) = %.1f KB)\n", sizeof(TtsAudio) / 1024.0);

    // ---- 1 continuity + mic replace ----------------------------------------------
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        a.setMicMode(TtsAudio::MicReplace);
        a.start();
        const auto pcm = sine(440.0, 2.0);
        auto prod = producer(a, 1, pcm, 3.0, false);
        const Capture c = runCallbacks(a, 3.2, true, 1000);
        prod.join();
        a.stop();

        // The mic level 1000 must be gone wherever TTS is active.
        const size_t start = firstNonZero(c.out, 1000);
        // Only the speech window counts: after the utterance the mic stays
        // replaced by silence for the 600 ms hold, which is not "audio".
        size_t nonMic = 0;
        for (size_t i = start; i < std::min(c.out.size(), start + pcm.size()); ++i) nonMic += (c.out[i] != 1000);
        const int inStep = maxStep(pcm, 0, pcm.size());
        const int outStep = maxStep(c.out, start + 200, start + pcm.size() - 400);
        // Sample-exact comparison of the steady part.
        int maxErr = 0;
        for (size_t i = 200; i + 400 < pcm.size() && start + i < c.out.size(); ++i)
            maxErr = std::max(maxErr, std::abs(c.out[start + i] - pcm[i]));
        check("continuity: all audio delivered", nonMic >= pcm.size() - 400, "%.0f of %.0f samples", double(nonMic), double(pcm.size()));
        check("continuity: sample-exact passthrough", maxErr <= 2, "max error %.0f LSB", maxErr);
        check("continuity: no discontinuities", outStep <= inStep + 2, "max step %.0f (input %.0f)", outStep, inStep);
        check("latency: first audio within 400 ms", start / 48.0 < 400.0, "%.0f ms", start / 48.0);
    }

    // ---- 2 local routing -------------------------------------------------------------
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        a.setLocalGainDb(0.0f);
        a.start();
        const auto pcm = sine(330.0, 1.0);
        auto prod = producer(a, 1, pcm, 4.0, true);
        const Capture c = runCallbacks(a, 1.8, true, 0);
        prod.join();
        a.stop();
        size_t injected = 0, heard = 0;
        for (int16_t s : c.out) injected += (s != 0);
        for (int16_t s : c.monitor) heard += (s != 0);
        check("routing: local job not sent to channel", injected == 0, "%.0f samples leaked", double(injected));
        check("routing: local job heard in monitor", heard > pcm.size() * 9 / 10, "%.0f samples", double(heard));
    }

    // ---- 3 stop ---------------------------------------------------------------------------
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        a.start();
        const auto pcm = sine(220.0, 4.0);
        auto prod = producer(a, 1, pcm, 2.0, false);
        size_t flushAt = 0;
        const Capture c = runCallbacks(a, 2.5, true, 0, [&](double t) {
            if (flushAt == 0 && t >= 1.0) {
                flushAt = static_cast<size_t>((t + 0.02) * 48000);
                a.flush(2);
            }
        });
        prod.join();
        a.stop();
        size_t lastNonZero = 0;
        for (size_t i = 0; i < c.out.size(); ++i)
            if (c.out[i] != 0) lastNonZero = i;
        const double tailMs = (double(lastNonZero) - double(flushAt)) / 48.0;
        // A 5 ms linear fade adds at most amplitude/240 per sample to the
        // natural slope (~14 LSB here); a click is a step of the full amplitude
        // (~13 000 LSB). 25 % headroom separates the two by two orders of magnitude.
        const int inStep = maxStep(pcm, 0, 48000);
        const int fadeStep = maxStep(c.out, flushAt - 960, lastNonZero + 2);
        check("stop: fades out (not an instant cut)", tailMs > 2.0 && tailMs < 25.0, "%.1f ms after flush", tailMs);
        check("stop: fade-out has no click", fadeStep <= inStep * 5 / 4, "max step %.0f (input %.0f)", fadeStep, inStep);
    }

    // ---- 4 underrun ---------------------------------------------------------------------------
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        a.setJitterMs(100);
        a.start();
        const auto pcm = sine(440.0, 2.0);
        auto prod = producer(a, 1, pcm, 0.8, false);   // slower than real time
        const Capture c = runCallbacks(a, 3.6, true, 0);
        prod.join();
        a.stop();
        size_t delivered = 0;
        for (int16_t s : c.out) delivered += (s != 0);
        const int inStep = maxStep(pcm, 0, pcm.size());
        check("underrun: audio preserved", delivered >= pcm.size() * 95 / 100, "%.0f of %.0f", double(delivered), double(pcm.size()));
        check("underrun: gaps are ramped, no clicks", maxStep(c.out, 0, c.out.size()) <= inStep + 2,
              "max step %.0f (input %.0f)", maxStep(c.out, 0, c.out.size()), inStep);
    }

    // ---- 5 activity + 6 effects tail ---------------------------------------------------------------
    for (int withReverb = 0; withReverb <= 1; ++withReverb) {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        if (withReverb) {
            SandboxState s;
            s.enabled = true;
            s.reverbWet = 0.6f;
            a.setSandboxState(s);
        }
        a.start();
        const auto pcm = sine(500.0, 0.6);
        auto prod = producer(a, 1, pcm, 5.0, false);
        std::vector<std::pair<double, bool>> activity;
        const Capture c = runCallbacks(a, 3.0, true, 0, [&](double t) { activity.push_back({t, a.remoteActive()}); });
        prod.join();
        a.stop();
        double releaseAt = -1;
        for (size_t i = 1; i < activity.size(); ++i)
            if (activity[i - 1].second && !activity[i].second) releaseAt = activity[i].first;
        size_t lastNonZero = 0;
        for (size_t i = 0; i < c.out.size(); ++i)
            if (std::abs(c.out[i]) > 3) lastNonZero = i;
        const double audioEnd = lastNonZero / 48000.0;
        if (!withReverb) {
            check("activity: active while speaking, released after", releaseAt > 0 && releaseAt - audioEnd < 0.5,
                  "released %.2f s, audio ended %.2f s", releaseAt, audioEnd);
        } else {
            const double firstAudio = firstNonZero(c.out) / 48000.0;
            check("effects: reverb rings out past the dry audio", audioEnd - firstAudio > 0.6 + 0.3,
                  "audible %.2f s for 0.60 s of speech", audioEnd - firstAudio);
            check("effects: transmission held through the tail", releaseAt >= audioEnd, "released %.2f, tail ends %.2f",
                  releaseAt, audioEnd);
        }
    }

    // ---- 7 clock handoff: no capture callbacks ------------------------------------------------------
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setLocalGainDb(0.0f);
        a.start();
        const auto pcm = sine(600.0, 1.0);
        auto prod = producer(a, 1, pcm, 4.0, false);
        const Capture c = runCallbacks(a, 1.8, false, 0);
        prod.join();
        a.stop();
        size_t heard = 0;
        for (int16_t s : c.monitor) heard += (s != 0);
        check("handoff: playback plays when capture is idle", heard > pcm.size() * 9 / 10, "%.0f samples", double(heard));
        check("handoff: no clicks", maxStep(c.monitor, 0, c.monitor.size()) <= maxStep(pcm, 0, pcm.size()) + 2);
    }

    // ---- 8 real-time voice changer ------------------------------------------------------
    // A fake backend echoes the uplinked mic NEGATED after 240 ms (like the converter's
    // delay), so every output sample tells where it came from: positive = raw mic leaked,
    // negative = converted voice.
    {
        NEW_AUDIO(a);
        a.setActiveServer(1);
        a.setMonitor(false);
        a.start();
        const int16_t probe[1] = {1};
        check("voice changer: backend audio refused while off", !a.pushVc(probe, 1));
        a.setVcEnabled(true);
        std::atomic<bool> run{true};
        std::vector<int16_t> uplinked;
        std::thread fake([&] {
            std::deque<std::pair<clk::time_point, std::vector<int16_t>>> q;
            int16_t buf[960];
            while (run.load()) {
                size_t n;
                while ((n = a.readMicUplink(buf, 960)) > 0) {
                    std::vector<int16_t> v(buf, buf + n);
                    uplinked.insert(uplinked.end(), v.begin(), v.end());
                    for (auto &s : v) s = static_cast<int16_t>(-s);
                    q.push_back({clk::now() + std::chrono::milliseconds(240), std::move(v)});
                }
                while (!q.empty() && q.front().first <= clk::now()) {
                    a.pushVc(q.front().second.data(), static_cast<int>(q.front().second.size()));
                    q.pop_front();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        constexpr int frames = 960;
        constexpr int talkBlocks = 50;          // 1 s with PTT down, then 1.2 s released
        std::vector<short> mic(frames);
        std::vector<int16_t> out;
        int64_t lastOutputAtRelease = 0, lastOutputAfter = 0;
        auto start = clk::now();
        for (int b = 0; b < 110; ++b) {
            std::fill(mic.begin(), mic.end(), static_cast<short>(1000 + b));
            a.onCapture(1, mic.data(), frames, 1, b < talkBlocks);
            out.insert(out.end(), mic.begin(), mic.end());
            if (b == talkBlocks) lastOutputAtRelease = a.vcLastOutputMs();
            if (b == talkBlocks + 20) lastOutputAfter = a.vcLastOutputMs();
            std::this_thread::sleep_until(start + std::chrono::milliseconds(20 * (b + 1)));
        }
        run = false;
        fake.join();
        a.stop();
        size_t leaked = 0, converted = 0, firstConv = out.size(), lastConv = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i] > 0) ++leaked;
            if (out[i] < 0) {
                ++converted;
                firstConv = std::min(firstConv, i);
                lastConv = i;
            }
        }
        const double latencyMs = firstConv / 48.0;
        const double tailMs = (double(lastConv) - talkBlocks * frames) / 48.0;
        check("voice changer: raw microphone never reaches the channel", leaked == 0, "%.0f samples leaked", double(leaked));
        check("voice changer: converted voice delivered", converted >= size_t(talkBlocks * frames * 85 / 100),
              "%.0f of %.0f samples", double(converted), double(talkBlocks * frames));
        check("voice changer: delay = model + pre-buffer", latencyMs >= 240.0 && latencyMs <= 360.0, "%.0f ms", latencyMs);
        check("voice changer: uplink stops after PTT up (+200 ms)",
              uplinked.size() <= size_t((talkBlocks + 11) * frames) && uplinked.size() >= size_t(talkBlocks * frames),
              "%.0f ms uplinked for 1000 ms of talk", uplinked.size() / 48.0);
        check("voice changer: converted tail plays after release", tailMs >= 200.0, "%.0f ms after release", tailMs);
        check("voice changer: output timestamp follows the tail", lastOutputAfter > lastOutputAtRelease);
        check("voice changer: no clicks", maxStep(out, 0, out.size()) <= 60, "max step %.0f", double(maxStep(out, 0, out.size())));
    }

    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
