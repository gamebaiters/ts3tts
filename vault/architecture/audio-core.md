# Audio core — `TtsAudio` (plugin/src/audio/TtsAudio.*)

## Rings
| ring | type | producer | consumer |
|---|---|---|---|
| `m_raw` | `SpscRing<int16_t>` 2^21 (~43 s mono) | link thread | pump |
| `m_segs` | `SpscRing<Seg{job,count,flags}>` | link thread | pump |
| `m_blocks` | `SpscRing<Block>` 64 × 10 ms stereo | pump | active reader |
| `m_monitorRing` | stereo int16 | capture reader | playback |

Producer writes samples **before** the descriptor → pump never sees a descriptor without samples. `pushPcm` accepts all or nothing; `false` = rings full → `LinkWorker` parks the frame, stops reading the socket (Qt read buffer capped 512 KB) → TCP window fills → backend `sendall` blocks. Memory bounded end to end.

Flags: `kFlagLocal` (preview job: never injected), `kFlagEnd` (end-of-utterance marker, pushed in-band by `LinkWorker` when `job_end` arrives).

## Pump (`pumpOnce`)
- keeps `kTargetBlocks = 12` (120 ms) processed ahead: effect slider changes are audible within 120 ms;
- start of utterance waits for the jitter buffer (`setJitterMs`, default 200 ms) or an END marker or 350 ms, and for `m_notBeforeMs` = 220 ms pause after the previous message;
- mid-utterance waits for a full block unless playout is at ≤ 3 blocks → then emits a partial block with a 2 ms ramp-down (`kRampFrames`), re-arms the jitter buffer, next audio ramps up: **underruns are gaps, never clicks**;
- blocks are homogeneous (one job, one routing);
- after END with DSP or pitch active: up to 3 s of ring-out blocks through the chain, cut after 200 ms of silence (`m_quietBlocks >= 20`);
- DSP state push (`setSandboxState`) does Leia/conv-IR bring-up outside `m_dspMutex` (Soundboard contract), pump locks only for the block;
- `setSandboxState` sanitises: Paulstretch, Binaural, sidechains, duck source forced off.

## Readers and the clock hand-over (`readAs`)
Capture is the preferred clock (what the channel hears). Playback reads blocks itself only when capture has been silent ≥ 150 ms (PTT idle, no mic device); `LocalPlayer` (waveOut, opened lazily, closed after 1 s idle) only when both are silent. Reads are serialised by a `SpinLock` (critical section = one memcpy).

## Stop (`flush(firstValidJob)`)
Raises `m_minJob`, bumps `m_flushEpoch`. Producer drops stale jobs, pump discards stale raw + resets chain (reverb cut), reader drops queued stale blocks and fades the current one over 5 ms (`kFadeFrames = 240`).

## Microphone handling (capture callback)
- `MicReplace` (default): mic zeroed while TTS audio flowed in the last 600 ms (no mic popping between sentences);
- `MicDuck`: mic /4; `MicMix`: untouched;
- `m_forceMicSilence`: set by [[ts3-integration]] while the TeamSpeak mute is lifted for the voice.

## Reader starvation guard (v1.3)
With a producer slower than real time the raw ring can be EMPTY when the queued blocks run out, so the pump has nothing to ramp down and the reader zero-filled after a full-amplitude sample (selftest §4, intermittent 8 302-LSB step). `readLocked` now decays the last sample to zero over `kRampFrames` (keeping the last block's routing, so the channel gets the decay), ramps the next block in, and sums a still-running decay into returning audio.

## Real-time voice changer (v1.3)
Capture callback, before the mic handling above: while enabled the raw mic is replaced by converted voice (`m_vcDown`, 60 ms pre-buffer, 2 ms ramps, 2 ms reserve) or silence; the mic goes to `m_vcUp` only while `(*edited & 2)` and no override of ours (+200 ms). TTS mixing and mic replace/duck then act on the converted voice. Details: [[voice-changer]].

## Activity (`remoteActive`)
raw remote samples > 0 ‖ remote blocks queued ‖ pump inside a remote utterance/tail ‖ last remote frame read < 250 ms ago. Drives the talk-state override.

Tests: `plugin/tests/audio_selftest.cpp` — see [[testing]].
