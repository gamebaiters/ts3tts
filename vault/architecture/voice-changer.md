# Real-time voice changer ("Voce live AI") — v1.3.0

User request (2026-09-14): "quando parlo la mia voce passa nel modello e viene modificata in tempo reale, facendo passare direttamente quella modificata, tipo clownfish però figo". Clownfish = DSP pitch/effects; this is **neural voice conversion**: you speak, the channel hears you with the timbre of another voice from your library.

## Model choice (researched 2026-09-15)
| Candidate | Why not / why |
|---|---|
| Seed-VC realtime fork (GPL-3.0, archived upstream) | 2 s look-ahead with the GUI defaults, ≥ 430–690 ms even tuned; XLSR + DiT 4-10 steps; needs transformers 4.46 + funasr |
| X-VC (MIT) | 539 M params, 298 ms latency, En/Zh only |
| RVC v2 | needs a trained model per target voice, no zero-shot |
| SynthVC | no code; fixed target speakers |
| Zero-VC | speaker anonymisation, not a chosen target |
| LLVC | one pretrained voice, 16 kHz |
| **MeanVC2** (Apache-2.0, ASLP-lab, Interspeech 2026) | 18 M params, streaming zero-shot, 40/120 ms chunk presets, weights on HF — **chosen** |

MeanVC2 is trained on Mandarin only (Emilia). Measured on **Italian** anyway (below): content survives.

## Measurements (vc_lab scratch scripts, RTX 3080 + i5-13600K)
Sources: 20 Italian sentences (Qwen Giulia, Kokoro Sara/Nicola, Supertonic M1) × targets Giulia / Marco; streamed in 20 ms blocks like the TS3 capture callback. Intelligibility = Whisper large-v3-turbo CER; timbre = SpeechBrain ECAPA cosine (WavLM-base-plus-sv saturated: 0.95 between a female and a male Qwen voice).

| system | CER (source 0.003) | ECAPA → target (baseline source→target) | lag p50 | RTF |
|---|---|---|---|---|
| upstream run_rt.py 40ms | 0.036–0.041 | — | negative (bug) | 1.56 |
| fixed 40ms, CPU 4 thr | 0.004 / 0.006 | 0.515 / 0.445 (0.154 / 0.243) | 200 ms | 0.75 |
| fixed 120ms, CPU 4 thr | 0.001 / 0.010 | 0.527 / 0.392 | 280 ms | 0.31 |
| fixed 40ms, DiT+vocoder on CUDA | 0.008 / 0.009 | 0.509 / 0.436 | 200 ms | 0.78 |

Converted-vs-source ECAPA stays at the source→target baseline: the original voice is gone, the target is approached (same-speaker pairs score ~0.6–0.8).

### Bugs / traps found
1. **Upstream `runtime/run_rt.py` stretches the 40 ms model 1.95×**: BN→mel upsampling hard-coded to 16 frames per call (right only for the 160 ms ASR fed 2 560-sample chunks). Fix: 4 mel frames per BN frame actually produced (`engine.py::_encode`), as the offline `infer_e2e.py` does.
2. **The Fast-U2++ ASR JIT was traced on CPU** (device constants in the graph): `.to(cuda)` fails inside the TorchScript interpreter. It dominates the cost, so CUDA for the rest gains nothing → the backend runs the voice changer on the **CPU only** (0 VRAM, no process-wide cuDNN switch that would slow Qwen).
3. **soxr streaming resamplers hold output back** (20 ms blocks): HQ ~31 ms, LQ ~21 ms, QQ ~10 ms per direction. QQ costs intelligibility (CER 0.012–0.013). `FirResampler3` (95-tap Kaiser, 6.8 kHz cutoff, −59 dB at 8 kHz, 0.98 ms group delay) gives QQ's latency with HQ's CER: **lag 237 → 200 ms**, default.
4. Numeric equivalence: vendored code == upstream with s3prl + x-transformers (710 state-dict keys, max |diff| 0 on speaker embedding and DiT velocity); engine == benchmarked runner at 1 thread (3.7e-8). 1-thread vs 4-thread outputs differ ≤ 1.6e-3 (−56 dB, kernel choice), two 4-thread runs are bit-identical.
5. The WavLM speaker checkpoint link is Google Drive; `yfyeung/wavlm-large-speaker-verification/wavlm-large.pt` on HF is byte-identical (SHA-256 `51f07e3b…`).

## Architecture
```
TS3 capture cb ─(*edited & 2 = will send)─► TtsAudio: m_vcUp ring (mic, only while really transmitting +200 ms)
      ▲                                              │ LinkWorker 10 ms timer → frame 0x03 (u32 seq + int16 48 k)
      │ block replaced by converted voice / silence   ▼
TtsAudio: m_vcDown ring ◄─ frame 0x04 ─ backend gbtts-vc worker: VcSession (gate, pre-roll, flush, backlog) → VoiceConverter (MeanVC2)
```
- **Plugin** (`TtsAudio::onCapture`): while enabled the raw mic never reaches the channel; mic uplinked only when TeamSpeak would transmit it (PTT down / VAD) and our own override is not active; converted voice played after a 60 ms pre-buffer with 2 ms edge ramps and a 2 ms reserve (selftest found a 1 058-LSB step when the ring ran dry exactly at a block boundary). Enabled in the audio core only once the backend reports `ready`, so the user keeps a normal mic while models load.
- **Tail hold** (`Controller::onTick`): the converted voice lags ~0.25 s, so after PTT up TalkState keeps transmitting while converted audio flows (`vcLastOutputMs < 250`) — started only once the user's own transmission stopped (`vcLastIntentMs > 60 ms` ago), otherwise the override would mask PTT and stop the uplink. A muted TS mic stays muted (no unmute for the voice changer).
- **Backend** (`gbtts/vc/`): `service.py` own thread (never behind a TTS job; reader thread only enqueues), lazy download (`download(models, preset)`), target embedding computed once per voice (`<voice>/meanvc2_spk.npy`, WavLM-Large 1.2 GB loaded only for that, 5–9 s), `session.py` energy gate (−42/−50 dBFS, 350 ms hang, 200 ms pre-roll, 240 ms flush, 400 ms backlog cap), `engine.py` converter, `third_party/` vendored code + `NOTICE.md`.
- Targets = any voice of the library (`vc_targets`), independent of the active TTS engine; `vc_add_target` adds a recording without Qwen.

## End-to-end latency (40 ms preset)
TS capture block 20 ms + backend 200 ms (p50) + plugin pre-buffer 60 ms + loopback ≈ **0.28–0.30 s**; 120 ms preset ≈ 0.36 s. UI shows backend latency + 60 ms.

## Tests
- `plugin/tests/audio_selftest.cpp` §8 (fake backend negating + delaying 240 ms): 0 raw samples leaked, delay 300 ms, uplink 1 180 ms for 1 000 ms of talk, 480 ms tail after release, max step 12.
- `backend/tests/test_vc_session.py` 14/14 (gate, pre-roll order, hang, flush length, backlog drop).
- scratch `e2e_vc.py` through the real server 12/12: ready 3.0 s (cached), silence not converted, 6.14 s out for 5.86 s speech, off ignores mic, add target from file.
- scratch `vc_lab/test_engine.py` 12/12, `verify_vendored.py`, `engine_bench.py` + `eval_vc.py`.

## Limits / not covered
- Only clean TTS speech was measured as source; a real microphone (noise, room, AGC) is untested — MeanVC2's paper reports robustness to low-quality **references**, not sources.
- CPU: 40 ms preset ≈ 3 cores busy while talking (RTF 0.75 on 4 threads), 120 ms ≈ 1.3 cores. Silence costs nothing (gate).
- During the ~0.5 s tail hold a new PTT press is not seen (override masks it); speech resumes converting when the hold ends. Same while a TTS message holds transmission.
- Not yet tried in a live TeamSpeak session.

Related: [[audio-core]], [[backend-protocol]], [[ts3-integration]], [[v1.3.0]].
