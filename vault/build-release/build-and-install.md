# Build and install

## Prereqs (same machine as SOUNDBOARD)
VS2022 `F:\VisualStudio 2022`, Qt 5.15.2 msvc2019_64 `D:\QT\Qt64\5.15.2\msvc2019_64` (TS3 ships Qt 5.15.2 — verified `Qt5Core.dll` 5.15.2.0, client 3.6.2), CMake ≥ 3.16, 7-Zip, `SOUNDBOARD_4.0` next to this folder (DSP sources + populated `build_local/_deps` for zlib/libmysofa, no network at configure).

## build_local.ps1 -Local
1. vcvars64 → `cmake -S plugin -B build -G "Visual Studio 17 2022" -A x64 -DSOUNDBOARD_ROOT=…` → `cmake --build build --config Release`
2. stage `build\stage\`: `package.ini` (from `plugin/package.ini.in`), `plugins\gb_tts_win64.dll`, `plugins\gb_tts\gbtts_16.png` (System.Drawing), `plugins\gb_tts\backend\{gbtts, tools\download_models.py, requirements.txt, install_backend.ps1}`
3. zip → `dist\gb_tts_<ver>_win64.ts3_plugin`
4. `-Install`: refuses while `ts3client_win64` runs; copies DLL + assets.

Batch trap: `cmd || exit /b %errorlevel%` exits **0** (expanded at parse) — the first build printed "DLL not produced" instead of failing. Use `if errorlevel 1 exit /b 1`.
Resources: the qrc listing the generated `.qm` is compiled with `qt5_add_resources` (SKIP_AUTORCC) so lrelease runs first.

## Engine install (end users) — installer/install_backend.ps1
Default `%LOCALAPPDATA%\GameBaitersTTS`: `venv\` (Python 3.12, torch 2.11 cu128 or cpu), `app\` (gbtts code), `models\hf` + `models\supertonic3`, `voices\`, `logs\backend.log` (+ `.prev.log`). Writes `HKCU\Software\GameBaiters\TTS\backend\home`. GPU → models base-1.7b + design-1.7b + supertonic (~9 GB); no GPU → supertonic only and `engine=supertonic`.
Launched from Settings › Install / repair (explicit consent dialog listing downloads), visible PowerShell window, idempotent.

## Resolution order (BackendProcess::resolve)
configured `backend/home` → `%LOCALAPPDATA%\GameBaitersTTS` → `<TS3 config>\plugins\gb_tts\backend`. Python at `venv\` or `.venv\`; code at `app\gbtts` or `gbtts`.
Dev layout: `TTS TS3\backend` (`.venv`, code in place).

## Uninstall
TeamSpeak removes the DLL; the engine folder (models) is user data and is not deleted automatically — delete `%LOCALAPPDATA%\GameBaitersTTS` and `HKCU\Software\GameBaiters\TTS`.

Related: [[testing]].
