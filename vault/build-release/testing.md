# Testing

Three layers, none needs a TeamSpeak client.

## 1. Engine bench — backend/tools/bench.py
```
backend\.venv\Scripts\python.exe backend\tools\bench.py --home backend --size 1.7B --chunks 2 4 8 --prefill 0 1 --out <dir>
backend\.venv\Scripts\python.exe backend\tools\bench.py --home backend --asr-only --out <dir> --asr-cache <scratch HF dir>
```
TTFA / RTF / VRAM per config, WAVs to listen to, then Whisper large-v3-turbo CER in a **separate process** (HF_HOME is read at import; `--asr-cache` keeps Whisper out of `models/`). Results 2026-09-14: [[engines-and-models]] — CER 0.000 on all 45 renders.

## 2. Backend end-to-end (the plugin's protocol, real models)
Scratch script `e2e_backend.py` (2026-09-14) played BackendLink: loopback listener, token hello, fresh home with a junction to `models/`. **15/15 PASS**:
configure→ready 57 s including first-run Giulia + Marco design · job PCM length == `audio_ms` exactly (6080/6080) · warm TTFA 478 ms end-to-end (normalize + prefill + resample) · `cancel_all` ends a job in 226 ms · speed 1.3 · Supertonic job · 3 s reader stall (backpressure) without loss · voice design preview + save + speak (TTFA 457 ms) · shutdown exits.
Re-create it from [[backend-protocol]] if needed; it is not in the repo (dev-machine paths).

### v1.1 Kokoro e2e (scratch `e2e_kokoro.py`) — 13/13 PASS
configure kokoro 1.8 s (fp32) · 41 `ko:` voices incl. if_sara/im_nicola · first audio 292 ms · speed 1.25 · cancel · backend RSS 515 MB (read from the launcher's CHILD process) · no VRAM (whole-GPU delta, per-process VRAM is N/A under WDDM) · switch to Qwen 0.6B (+3 800 MB GPU) · back to Kokoro frees 2 995 MB · shutdown.
Engine micro-benchmarks: `kokoro_micro.py` (threads/opt/trim), `kokoro_micro2.py` (varying shapes × mem options), `kokoro_micro3.py` (spinning) — results in [[engines-and-models]].

## 3. Audio core selftest — plugin/tests/audio_selftest.cpp
`cmake -DGBTTS_BUILD_TESTS=ON` → `build\tests\gbtts_audio_selftest.exe` (needs Qt bin on PATH). Fake capture/playback callbacks on a real 20 ms clock. **15/15 PASS** (after the fix below):
continuity sample-exact (0 LSB error), no discontinuity · first audio 60 ms after data · local job: 0 samples leaked to channel · stop: 5.0 ms fade, max step = input · slower-than-real-time producer: 99.8 % delivered, no clicks · activity released 250 ms after audio · reverb tail keeps transmitting · playback becomes clock when capture idle.

### Bugs the selftest found
- **Stop clicked** when the flush landed between blocks (TS3 960-frame callbacks consume exactly two 480-frame blocks, so "no current block" is the common case): nothing to fade → hard cut mid-waveform. Fix in `TtsAudio::readLocked`: promote the next queued block and fade it.
- `TtsAudio` is **1.7 MB** (inline `SlotDsp`): on a thread stack it overflows (`0xC00000FD`). Plugin heap-allocates it; never put one on the stack.

## 4. Settings self-test — plugin/tests/settings_selftest.cpp (v1.2)
`build\tests\gbtts_settings_selftest.exe`, isolated key `HKCU\Software\GameBaiters\TTS-selftest` (`GBTTS_SETTINGS_APP`). 22 checks: v1.1 `voice/id` → per-engine map, foreign-prefix entries dropped, `auto` → it, clamps, round-trip of every field type (unicode history, binary geometry, dictionary, effects JSON), engine switch restores that engine's voice, **1.78 ms per save()** (200 saves).

## 5. Host harness — plugin/tests/host_harness.cpp (v1.2)
Loads the real `gb_tts_win64.dll` like the client (function pointers, plugin id, menus + hotkeys freed with `ts3plugin_freeMemory`, init, 20 ms audio thread calling capture + playback — kept running across `ts3plugin_shutdown`, menu events opening all windows, `/tts` command, shutdown, FreeLibrary) in a QApplication with fake `TS3Functions`; settings in `TTS-harness`. Unhandled-exception reporter prints module+offset+export symbol per frame and writes `%TEMP%\gbtts_harness_crash.dmp`.
```
set PATH=<Qt>\bin;%PATH%
gbtts_host_harness --dll build\release\gb_tts_win64.dll --cycles 15 --run-ms 1000 --unload
gbtts_host_harness ... --cycles 4 --run-ms 3000 --unload --backend-home backend --engine kokoro
gbtts_host_harness --crash terminate|segv --dll ... --backend-home backend --engine qwen --wait-ms 45000 --vram
```
Bisection flags: `--no-ui`, `--no-init`, `--no-final-unload`; `--trim` clears `QPixmapCache` and compacts every heap before sampling, and runs of ≥ 20 cycles add a least-squares slope over the second half (a leak keeps its slope, a cache flattens). Use ≥ 100 cycles before calling anything a leak: a raw 60-cycle run showed +17 MB that turned out to be a one-time plateau. Checks per mode: backend connected, voice reached the playback callback, no python descendant ≤ 5 s after shutdown, private bytes slope, handles, threads, GDI/USER; crash mode: backend dies ≤ 3 s after the client, GPU back to baseline, a setting toggled 50 ms before the crash is on disk. Results: [[v1.2.0]].

## 6. Backend e2e per engine (scratch `e2e_engines.py`, v1.2) — 23/23
Voices event tagged with the engine, only that engine's ids, gender/lang metadata, cross-engine speak and voice design refused, no PyTorch with Kokoro/Supertonic.

## 6b. Chat + queue in the host harness (v1.4)
With `--backend-home`, cycle 1 also checks: a `/tts` command issued right after the backend connects (voices not loaded) is spoken when ready; `ts3plugin_onTextMessageEvent` with an own private message and someone else's channel message produces no audio, an own channel message with BBCode is spoken and logged with the stripped length. The fake `getClientID` returns 42 and settings enable `text/read_channel_chat`.

## 7. Voice changer (v1.3)
- audio selftest §8 (fake backend: uplink negated + delayed 240 ms): raw mic never leaks, delay, uplink stops 200 ms after PTT up, tail plays, no clicks. Selftest is now 23 checks; run it several times — §4 was intermittent before the reader starvation guard.
- `backend\.venv\Scripts\python.exe backend\tests\test_vc_session.py` — streaming policy with a fake converter, 14 checks.
- scratch `e2e_vc.py` (server protocol), `vc_lab/test_engine.py` (engine vs lab runner at 1 thread, 48 kHz real-time), `vc_lab/verify_vendored.py`, `vc_lab/bench_meanvc2.py` / `engine_bench.py` + `eval_vc.py` (Whisper CER + ECAPA, lab venv with speechbrain). Numbers: [[voice-changer]].

## 8. Auto-update and release (v1.5)
- `build\tests\gbtts_update_selftest.exe` — 35 checks: version numbers, feed parsing and every rejection (other product, build ≠ version string, http, bad checksum, malformed XML), release notes since the installed version, SHA-256, zip magic, and the Windows helper **executed** twice against a fake client (renamed `ping.exe`): graceful exit → installer right after; never-exiting client killed after the grace time, locked DLL delete retried. `--validate-feed <xml> --expect-version X.Y.Z` is the CI check of the generated feed.
- `gbtts_host_harness --update` — 15 checks with a `file://` feed and `GBTTS_UPDATE_NO_LAUNCH=1`: offer at startup with only the newer notes, Later = +3 days, menu check ignores the postponement, unload with the offer / progress window open, verified download identical to the published file, helper targets this client's plugins folder, damaged download refused and removed, "up to date" = +1 day.
- `gbtts_host_harness --code-sync --backend-home backend` — older `app\gbtts` replaced before the engine starts, restart copies nothing, requirements drift reported.
- `tools\release_check.ps1 -Version X.Y.Z` — CMake / backend / release-notes agreement (also the first CI step).

## 9. Engine installation window (v1.5)
- `gbtts_host_harness --setup-fake --installer plugin\tests\fake_install_backend.ps1` — 18 checks, stable 4/4 runs: window opens by itself, default folder, progress, one click does not cancel, second click kills installer + child tree, Retry, unload mid-install (installer survives), reload re-attaches, completion saves `backend/home`, no window left.
- `gbtts_host_harness --setup-real --mode gpu|cpu [--home <dir>]` — the real `install_backend.ps1` driven through the window, then the engine starts by itself and speaks. Results: [[engine-installer]], [[v1.5.0]].
- Installer alone, hidden, CPU mode into a scratch folder: `powershell -File installer\install_backend.ps1 -InstallDir <dir> -Mode cpu -Models kokoro` (2.1 min, verify `OK 2.11.0+cpu`).

## Not yet covered
Live TeamSpeak session (talk-state restore on PTT/VAD servers, toolbar overlay next to the Soundboard button, muted-mic lift) — verify on host + VM like the Soundboard.
