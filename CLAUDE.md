# TTS TS3 — GameBaiters TTS (Claude reference)

TeamSpeak 3 plugin: type in a box, press Enter, a **local** neural voice speaks in the channel. Built on SOUNDBOARD_4.0's architecture and crash rules; its DSP chain + effects editor are compiled **in place** from `../SOUNDBOARD_4.0/upstream-clone/src` (never copied).
C++17 / Qt 5.15.2 plugin + Python 3.12 backend: Qwen3-TTS (GPU, faster-qwen3-tts CUDA graphs), Kokoro-82M (light CPU, kokoro-onnx fp32), Supertonic 3 (CPU). Current: **v1.1.0** (engine off by default + power on/off button, DSP-tail ring-out no longer overholds for non-tail effects, cross-plugin shared-memory audio bridge with the GameBaiters Soundboard — see `vault/history/v1.1.0.md`, `vault/architecture/audio-bridge.md`). Prior: v1.0.0 first public release (internal development line v1.0-v1.5, history in `vault/history/`; engine installation window with progress + private Python via uv, auto-update, GitHub release pipeline; read own channel chat, queued messages, real-time AI voice changer) — see `vault/history/v1.5.0.md`, `vault/build-release/engine-installer.md`, `vault/build-release/release-and-update.md`.
Repository: `github.com/gamebaiters/ts3tts` (Windows only). Releases = push a `vX.Y.Z` tag (README "Releasing").

**Session start:** read this file, then `vault/HOME.md`. Grep the vault before reading source.

## Layout
```
TTS TS3/
├── plugin/            C++ plugin (CMakeLists.txt, src/, i18n/, tests/)
│   └── src/  audio/ (TtsAudio, SpscRing, LocalPlayer) net/ (BackendLink, BackendProcess)
│             core/ (Controller, Settings) ts3/ (TalkState) ui/ (TtsWindow, VoiceDialog, SettingsDialog, ToolbarButton)
│             gbtts.cpp (lifecycle + callback bridge)  plugin_exports.cpp (SDK C exports)  compat.cpp (host facade)
├── backend/           Python engine: gbtts/ (server, protocol, textproc, audio, voices, engines/), tools/ (bench, download_models)
│   ├── .venv/  models/  voices/   (dev install lives here; NOT packaged)
├── installer/install_backend.ps1  engine installer: uv + private Python 3.12 + torch + models; run HIDDEN by the plugin (status.json)
├── tools/             package_plugin.ps1 (shared by build_local + CI), release_check.ps1
├── .github/workflows/release.yml  tag vX.Y.Z → build, tests, package, version.xml, GitHub release
├── build_local.ps1    build + package → dist\gb_tts_<ver>_win64.ts3_plugin
└── vault/             knowledge base (Obsidian, wikilinked) — HOME.md = index
```

## Build / run
```powershell
.\build_local.ps1 -Local            # VS2022/Build Tools (vswhere) + Qt 5.15.2 msvc2019_64 (D:\QT\Qt64)
.\build_local.ps1 -Local -Tests     # + build\tests\gbtts_{audio,settings,update}_selftest.exe, gbtts_host_harness.exe
.\build_local.ps1 -Local -Install   # also copies into %APPDATA%\TS3Client\plugins (TS3 closed)
build\tests\gbtts_host_harness.exe --dll build\release\gb_tts_win64.dll --cycles 15 --unload   # + --crash segv --backend-home backend --engine qwen --vram
build\tests\gbtts_host_harness.exe --setup-fake --dll ... --installer plugin\tests\fake_install_backend.ps1   # installation window
build\tests\gbtts_host_harness.exe --update --dll ...                                                      # auto-update flow
backend\.venv\Scripts\python.exe backend\tools\bench.py --home backend [--asr-only]
```
Details: `vault/build-release/build-and-install.md`.

## Critical gotchas (details in vault)
- **transformers MUST stay 5.15.1**: 5.17 breaks every Qwen load (`'MimiConfig' object has no attribute 'rope_theta'`). [engines-and-models]
- **Kaspersky/AV HTTPS scanning** re-signs TLS → certifi fails (`CERTIFICATE_VERIFY_FAILED self-signed`). Backend + downloader call `truststore.inject_into_ssl()`; never disable verification. [engines-and-models]
- **Never block in TS3 callbacks / sb_kill-equivalent**: `BackendProcess::kill()` terminates a Job Object and returns; shutdown pumps events instead of sleeping. [crash-safety]
- **QTranslator maps .qm in place** → our catalogues use `:/gbtts/...` paths, never the Soundboard's `:/i18n/...` (a shared path could point the Soundboard's translator into our unloaded DLL). [crash-safety]
- **Batch files: `|| exit /b %errorlevel%` exits 0** (expanded at parse time) — use `if errorlevel 1 exit /b 1`. [build-and-install]
- **One Qwen model in VRAM at a time** (1.7B + graphs ≈ 4.7 GB). Voice design swaps models; daily use = Base only. [engines-and-models]
- **Kokoro int8 ONNX is ~7× slower than fp32 on CPU** (dynamic-quant kernels ≈ one core): fp32 is the default. [engines-and-models]
- **`phonemizer.phonemize()` costs ~250 ms per call** (new espeak backend each time): keep one `EspeakBackend` per language. [engines-and-models]
- **Never import torch just to list engines**: availability checks use `find_spec`; a Kokoro-only backend stays ~600 MB.
- **ONE engine at a time, chosen in Settings** (`Controller::setEngine`). Backend lists/speaks only its voices (`voices` event carries `engine`); a voice of another engine is refused, never silently loaded. Voices remembered per engine (`voice/per_engine`).
- **Never `QTimer::singleShot(ms, ctx, lambda)`** — even with a plugin context the queued timer's dtor jumps into the unloaded DLL. Use `deferCall(ctx, ms, fn)` (`core/Defer.h`). [crash-safety]
- **No `thread_local` in code run by TS3 threads** (they outlive the DLL). [crash-safety]
- **Every setting change is saved** (`saveNow` / `scheduleSave` 300 ms); TS3 callbacks only start the timer.
- **Never refuse a message because the engine is starting**: `speakImpl` queues it (`Job::sent=false`, `flushPendingJobs` on voices/ready). Refusing made the chat echo look "30 s late". [ts3-integration]
- **Read channel chat** = own `TextMessageTarget_CHANNEL` messages only (`fromID == getClientID`), BBCode stripped, own echoes skipped (`m_recentEchoes`). [ts3-integration]
- **Voice changer = MeanVC2 on the CPU** (`backend/gbtts/vc`, vendored code, verified equal to upstream). Upstream `run_rt.py` stretches the 40 ms model 1.95× — our `engine.py` has the fix; the Fast-U2++ JIT is CPU-traced (no CUDA); `FirResampler3`, not soxr streams (−37 ms). [voice-changer]
- **While the voice changer is on the raw mic never reaches the channel**; mic is uplinked only when `(*edited & 2)` and no override of ours; audio-core enables it only on backend `ready`. [voice-changer]
- **Reader starvation guard** in `TtsAudio::readLocked` (decay + ramp-in): selftest §4 was intermittent without it. Run the audio selftest several times after touching the pump/reader. [audio-core]
- **The engine installer never needs a system Python**: uv installs a private 3.12 in `<home>\python` (`UV_PYTHON_PREFERENCE=only-managed`). A venv based on a vanished Python (Windows reinstall) is detected via `pyvenv.cfg` and offered for repair. [engine-installer]
- **Installer runs detached + hidden**; progress only through `<home>\install\status.json`. PowerShell writes it with `File.Replace(..., [NullString]::Value)` + retries (Qt holds the file without FILE_SHARE_DELETE; a plain `$null` becomes `""`). [engine-installer]
- **Harness never sees a real engine install**: it sets `GBTTS_DEFAULT_HOME`; harness modes share the `TTS-harness` registry key — never run two at once.
- **Update feed = release asset** `releases/latest/download/version.xml` (draft → publish): build number must equal `versionNumber(latestVersionString)` or the plugin rejects it (update loop). Bump version in CMake + `backend/gbtts/__init__.py` + release-notes (`tools/release_check.ps1`). [release-and-update]
- **Load Qwen from the local snapshot directory**, not the repo id (repo id ⇒ ~20 HTTP requests per start). [engines-and-models]
- UI language default is **Italian**; stored v1.0 `auto` is migrated to `it` (`Settings::load`).
- **`autostart` defaults to `false`** (v1.1.0): the engine never starts itself on plugin load unless the user turns it on (power button next to the status text, or the Settings checkbox). Existing users who already had `autostart=true` saved keep that choice — only fresh installs are affected.
- **If the GameBaiters Soundboard plugin is also installed**, this plugin stops writing the shared TS3 capture buffer itself and instead publishes to `ipc/GbAudioBridge.h` — the Soundboard becomes the single writer. [audio-bridge]
- Audio callbacks read `g_audio` atomically; shutdown nulls it + 50 ms pumped drain before delete. [audio-core]
- Talk-state override is the Soundboard's TalkStateManager port — keep every guard. [ts3-integration]
- SDK constants (`SPEAKER_*`, `CLIENT_INPUT_*`) need `common.h`.

## UI rules
Same as Soundboard (`../SOUNDBOARD_4.0/vault/ui/ui-rules.md`): theme via `Theme::compositeStyleSheet()` + `isGBSoundboard` property, no Qt5::Svg, painted icons, captions on status LEDs, `tr()` everything then `lupdate` → `python plugin/i18n/translate_it.py`.
