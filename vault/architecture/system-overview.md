# System overview

```
TtsWindow (Enter) ──> Controller::speak()  [TS3 GUI thread]
   │ job id, local flag, history, optional chat echo
   ▼
BackendLink ──JSON op "speak"──> gbtts.server (Python, separate process, GPU)
                                   textproc.normalize → split_segments
                                   engine.stream(segment)  (Qwen3-TTS 24 k / Supertonic 44.1 k)
                                   StreamResampler(48 k) → WsolaStretch(speed) → Leveler → to_int16
   ◄──PCM frames (u32 job + int16 mono 48 k)── in order, then JSON job_end
LinkWorker [link thread] ── pushPcm / pushEnd ──> TtsAudio raw ring (SPSC)
TtsAudio pump [pump thread]: gain → PitchShiftGrain → SlotDsp (Soundboard chain) → 10 ms stereo blocks
TS3 capture callback  [audio thread]: replace/duck mic, inject remote blocks, feed monitor ring
TS3 playback callback [audio thread]: play monitor ring (or be the clock when capture is idle)
LocalPlayer [waveOut thread]: last-resort clock when TeamSpeak delivers no callbacks at all
Controller::onTick (20 ms) → TalkState::begin/end (continuous transmission, muted-mic lift)
```

Why a separate Python process and not in-DLL inference:
- a crash/OOM in PyTorch can never take the TeamSpeak client down (the plugin just restarts it);
- CUDA/PyTorch DLLs (~3 GB) never load into ts3client_win64.exe;
- model stays warm independent of the TS3 GUI thread; the Job Object kills it with TeamSpeak.

Why streaming end to end: time-to-first-audio ≈ 0.3 s on an RTX 3080 instead of waiting for the whole sentence; nothing is ever written to disk (no mp3/wav cache — replay re-synthesises).

Related: [[audio-core]], [[backend-protocol]], [[engines-and-models]], [[ts3-integration]].
