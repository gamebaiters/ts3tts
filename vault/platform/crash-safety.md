# Crash safety — Soundboard lessons applied (plugin/src/gbtts.cpp)

| Soundboard lesson (SOUNDBOARD vault) | Applied here |
|---|---|
| Blocking inside TS3 callbacks → DirectSound NULL-deref ("ghost user") | callbacks only post work; `BackendProcess::kill` = `TerminateJobObject` + close handles, no wait; settings saved on GUI events |
| Context-less `QTimer::singleShot` outlives the DLL | **stronger than the Soundboard rule (v1.2)**: a singleShot functor WITH a plugin context also crashes after unload (the queued `QSingleShotTimer` is owned by the host dispatcher; its dtor calls the lambda impl in our DLL). Every deferred call uses `deferCall(ctx, ms, fn)` (`core/Defer.h`): a `QTimer` CHILD of `g_ctx` / `s_ctx`, killed with it in `shutdown()`. Never write `QTimer::singleShot(…, lambda)` in this plugin. Reproduced + verified by `gbtts_host_harness` ([[v1.2.0]]) |
| `thread_local` on TS3 audio threads | none: reader scratch buffers are `TtsAudio` members allocated once (TLS of an unloaded DLL on host threads is never freed) |
| Late signals of a dropped backend connection | `BackendLink` generations: `connected/disconnected/message` carry the generation, GUI drops stale ones; `dropPeer()` before killing a backend on restart |
| Audio callback racing object deletion | `g_audio` atomic nulled, 50 ms **pumped** drain (`processEvents(ExcludeUserInputEvents)`), then delete |
| Parentless top-level widgets leak a vtable | window + dialogs hidden, unparented, deleted in shutdown |
| Plugin widget left in the host toolbar | `ToolbarButton::remove()` is the first statement of `shutdown()` |
| `QFontCache` destroys DLL-owned strings after unload | same exported-symbol purge (`?instance@QFontCache` / `?clear@QFontCache`) |
| Deferred deletes firing after FreeLibrary | `sendPostedEvents(DeferredDelete)` + `processEvents(200 ms)` + again |
| `QStringLiteral` in host-lifetime structures | font families never set from literals; theme uses system font |

New for a process-owning plugin:
- **Job Object with KILL_ON_JOB_CLOSE** (process created suspended, assigned, resumed): TS3 crash/exit kills the backend; backend also watches `--parent-pid` with `WaitForSingleObject`. Measured with a crashing child client: backend processes dead 21–182 ms after the client, Qwen VRAM back to baseline immediately ([[v1.2.0]]).
- **Settings are written on every change** (300 ms debounce for sliders, immediate otherwise, `sync()` each time, 1.78 ms): a crash loses at most a slider drag in progress.
- **Handle inheritance restricted** to the log file (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`).
- Link thread joined with `wait(3000)` after `closeAll` (no pending I/O: sockets aborted in-thread).
- **Translations**: QTranslator references the .qm data in place → catalogues at `:/gbtts/i18n/*.qm`. `:/style/dark_style.qss` and `:/leia/default.sofa` are shared paths on purpose: their readers copy the data (`QFile::readAll` / copy to cache dir), so either DLL unloading is harmless.
- **Updater / installer windows (v1.5)**: every message box non-modal (no nested event loop a plugin unload could interrupt), two-click cancel instead of a question box; `Updater` owns them through `QPointer` and deletes them (plus its parentless progress window and pending replies) in `gbtts::shutdown`, before the unload — harness `--update` checks "0 windows left". `QApplication::closeAllWindows()` for the update runs from a `deferCall` timer, never inside the slot.
- **No `QNetworkAccessManager` until a request runs**: creating one at each init cost ~0.5 MB per plugin load (A/B vs v1.4, [[v1.5.0]]).
- **Engine installer is detached, never owned**: `EngineInstaller` only polls status.json and closes its process handle in the destructor — no `QProcess` (its destructor waits up to 30 s), no kill on unload.
- **No FFmpeg in the DLL**: `CreateInputFileFFmpeg` stub returns null (only the convolution reverb's "load IR file" used it; presets still work).

Related: [[ts3-integration]], Soundboard `platform/ghost-crash-forensics.md`.
