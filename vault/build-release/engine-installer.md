# Engine installer (v1.5)

Why it was rewritten: after a Windows reinstall (2026-09-16) the plugin could not get a working engine.
- The v1.4 installer needed a **Python 3.12 already installed**; the fresh PC had only 3.14 → "Python 3.12 non trovato".
- It ran in a visible PowerShell console with no progress, started by hand from Settings.
- An engine venv created from a system Python is **dead after reinstalling Windows**: `venv\Scripts\python.exe` is still there but every start prints `No Python at '...Python312\python.exe'`. The plugin considered it installed.

## Design
```
EngineSetupDialog (Qt)  ──installEngine──►  Controller  ──►  EngineInstaller ──CreateProcess hidden──► powershell -File install_backend.ps1
        ▲ progress / logAppended / finished            ▲                         │ polls every 400 ms
        └──────────── status.json + install.log ◄──────┴─────────────────────────┘ <home>\install\
```
- **One implementation**: `installer/install_backend.ps1` (packaged in `plugins\gb_tts\backend\`). The window only launches and reads it; run by hand it prints the same steps.
- **Nothing preinstalled**: the script downloads **uv** (pinned `UV_VERSION`, SHA-256 checked) into `<home>\tools`, then `uv python install 3.12` into `<home>\python` (`UV_PYTHON_PREFERENCE=only-managed`: never a system Python), `uv venv`, `uv pip install` torch from the PyTorch index (cu128 or cpu), then `requirements.txt`. `UV_NATIVE_TLS=1` = Windows certificate store (antivirus HTTPS scanning, same reason as `truststore`).
- **Package cache** `<home>\cache\uv` is hard-linked into `venv\` (verified with `fsutil hardlink list`): costs no extra disk, and a Repair re-downloads nothing. Size reports exclude it.
- **Detached + hidden**: `CREATE_NO_WINDOW`, breakaway from the host job when allowed. Closing the window, TeamSpeak or unloading the plugin never interrupts it. `Controller::startup` re-attaches (`EngineInstaller::attach`: status `running` + pid alive + image `powershell.exe`).
- **Environment**: `PSModulePath` removed before launching Windows PowerShell 5.1 (a PowerShell 7 value makes 5.1 load the wrong modules).
- **Cancel** = two clicks (no modal box: nested event loop in a plugin) → `taskkill /PID <pid> /T /F` (uv/python children too). A process gone without `done`/`error` becomes `cancelled` or `interrupted`.
- **Single instance**: named mutex `Local\GameBaitersTTS-engine-install`; the "busy" error never overwrites the running installation's status.json.
- On `done`: `backend/home` saved, engine switched to Kokoro for a light install, backend started automatically.
- `BackendProcess::isHome` reads `pyvenv.cfg` `home =`: missing base Python ⇒ layout invalid ("the Python environment is broken") ⇒ the window opens by itself offering **Repair**; the script detects a venv not based on its private Python and recreates it.
- Window opens by itself 2.5 s after start when the state is `NotInstalled`.

## status.json contract
`state` running|done|error · `pid` · `mode` gpu|cpu · `stages[]` (check uv python venv torch libs code verify models finish) · `stage`, `stageIndex` · `overall` 0..1 (stage weights) · `stageFraction` (<0 unknown) · `bytesDone`/`bytesTotal` · `item` · `errorCode` (disk_space network checksum no_gpu gpu_driver python venv verify models sources busy unexpected; plugin adds cancelled interrupted) · `error` · `sizeBytes`.
Written aside + `File.Replace` with **`[NullString]::Value`** (a plain `$null` becomes `""` → exception) and **20 retries**: Qt opens the file without `FILE_SHARE_DELETE`, so Replace fails while the plugin is reading it. Found by the fake-installer harness: without retries the script died at random (3 of 3 runs flaky → 4 of 4 green).

Progress sources: own downloads count bytes; uv steps measure `cache\uv` growth against measured sizes (`$ExpectedBytes`); models: `download_models.py --progress` prints `@@progress {"done","total","item"}` (HF API sizes, symlinks skipped).
PowerShell 5.1 pitfalls hit: `Register-ObjectEvent` handlers never run while the script loops → async `ReadLineAsync` on both pipes; `Invoke-WebRequest` progress bar slows downloads → `$ProgressPreference = SilentlyContinue`, own `HttpWebRequest` loop.

## Measured (i5-13600K, RTX 3080, 2026-09-16)
| Mode | Time | On disk | Notes |
|---|---|---|---|
| CPU (Kokoro) | 2.1 min | ~1.5 GB (+ cache hard links) | torch cpu 20 s, 88 packages prepared in 5 s, verify 36 s |
| GPU (Qwen 1.7B + design + Kokoro) | see [[v1.5.0]] | | real run through the window (`--setup-real --mode gpu`) |

## Tests
- `gbtts_host_harness --setup-fake --installer plugin\tests\fake_install_backend.ps1`: window opens by itself, default folder, progress, one click does not cancel, second click kills installer + child, Retry, unload mid-install → installer survives, reload re-attaches, completion saves `backend/home`, no window left. 18 checks.
- `--setup-real --mode gpu|cpu [--home]`: the real script through the window, then the engine starts by itself and speaks.
- Harness sets `GBTTS_DEFAULT_HOME` to a non-existent folder so a real `%LOCALAPPDATA%\GameBaitersTTS` never leaks into other modes.

Related: [[build-and-install]], [[testing]], [[crash-safety]].
