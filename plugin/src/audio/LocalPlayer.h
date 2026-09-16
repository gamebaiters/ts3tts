#pragma once

#include <atomic>
#include <thread>

namespace gbtts {

class TtsAudio;

// Last-resort local output. TeamSpeak only calls the plugin audio callbacks
// while a server connection has an open playback device; previewing a voice in
// the voice designer while disconnected would otherwise be silent. This thread
// idles on a 20 ms timer and opens the default waveOut device only while
// TtsAudio reports audio that no TeamSpeak callback is consuming, closing it
// again after one second of silence.
class LocalPlayer
{
public:
    explicit LocalPlayer(TtsAudio *audio);
    ~LocalPlayer();

    void start();
    void stop();

private:
    void run();

    TtsAudio         *m_audio;
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace gbtts
