# Release pipeline and auto-update (v1.5)

Modelled on the GameBaiters Soundboard (`../SOUNDBOARD_4.0/vault/reference/ci-pipeline.md`), **Windows only**.

## Repository
`github.com/gamebaiters/ts3tts` (public). Not in git: `backend/.venv`, `backend/models` (~19 GB), `backend/voices` (user recordings), logs, `build/`, `dist/` (`.gitignore`).
The DSP chain is compiled from `gamebaiters/RP-Soundboard` at a **pinned commit** (`SOUNDBOARD_REF` in the workflow): bump it deliberately.

## Cutting a release
1. Same `X.Y.Z` in `plugin/CMakeLists.txt` `GBTTS_VERSION` and `backend/gbtts/__init__.py` `__version__`.
2. New top section in `release-notes.txt`: `GameBaiters TTS vX.Y.Z` + `=====` line.
3. `tools\release_check.ps1 -Version X.Y.Z`, commit, push, `git tag vX.Y.Z`, `git push origin vX.Y.Z`.

`.github/workflows/release.yml` (tag `v*`; `workflow_dispatch` = snapshot artifact only):
checkout both repos → `release_check.ps1` → Qt 5.15.2 msvc2019_64 + MSVC → CMake with tests → settings + update self-tests → backend byte-compile + `test_vc_session.py` → `tools/package_plugin.ps1` (same script as `build_local.ps1`) → `version.xml` with the package SHA-256, validated by `gbtts_update_selftest --validate-feed` → **draft** release with package, `version.xml`, `SHA256SUMS.txt`, `release-notes.txt` → publish → download the feed and the package from `releases/latest/download/` and compare the checksum.

### CI pitfalls hit on the first runs (2026-09-16)
- `windows-latest` = `windows-2025-vs2026` (MSVC 14.51): its STL removed `stdext::make_checked_array_iterator`, still used by the Qt 5.15.2 headers (`qlist.h(915)`). The job is pinned to **`windows-2022`** (MSVC 14.4x, the toolset of the locally harness-tested builds; a newer toolset could also need a newer `msvcp140.dll` than TeamSpeak loads).
- The runner uses **Ninja**: `ExpandableSection.ui` listed in the sources gave "multiple rules generate ui_ExpandableSection.h" (the VS generator tolerates it). Nothing includes that header → removed. Reproduce CI locally with the Ninja bundled in Build Tools (`Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`).
- First green snapshot: run 35098615575, 3 min 53 s, settings 31 / update 35 / vc_session 14 / stock_voices 14, package identical in layout to the local one (69 entries).

## Update feed
`https://github.com/gamebaiters/ts3tts/releases/latest/download/version.xml` — a release **asset**, not a file on a branch: it goes live in the same instant as its package (draft → publish), so no client ever sees a version whose download 404s. Soundboard schema, product `gb_tts`, plus `<sha256>`.
Rejected by the plugin (`core/UpdateFeed.cpp`): other product, `latestVersion != versionNumber(latestVersionString)` (the update-loop trap), non-https URL, malformed checksum.

## Client flow (`core/Updater`, `ui/UpdaterWindow`)
4 s after start, if `update/auto_check` and `now >= update/next_check`: fetch feed → newer build → release notes of every newer version → non-modal box "Update now / Later" (Later = +3 days, up to date = +1 day). Download to `%TEMP%\GameBaitersTTS-update\*.part` → zip magic + SHA-256 → rename → write `gbtts_update_helper.bat` → start it → `QApplication::closeAllWindows()` 1.5 s later.
Helper: `chcp 65001`; waits up to 20 s for a graceful `ts3client_win64.exe` exit (`tasklist /FO CSV` — the table format truncates image names over 25 characters), then WM_CLOSE, then `/F`; deletes the DLL with 15 retries (lock outlives the process); removes `plugins\gb_tts\backend\gbtts` (no stale modules); `start "" package`. Absolute `%SystemRoot%\System32` tools (a GNU `find.exe` on PATH breaks the test); `ping` as sleep (`timeout` refuses redirected stdin).
After the update, `BackendProcess::syncPackagedCode` copies the packaged engine code into `<home>\app` when `__version__` differs (gbtts.new → rename), and warns when `requirements.txt` changed ("Install / repair").
Menu: *Check for TTS updates…*; Settings › Interface: automatic check switch + "Check now".

## Tests
`gbtts_update_selftest` (35): versions, feed rules, notes, checksum, helper script **executed** against a fake client (graceful exit; never-exiting client killed; locked DLL retried). `gbtts_host_harness --update` (15): file:// feed + `GBTTS_UPDATE_NO_LAUNCH=1`. `--code-sync`: engine code refreshed before the engine starts.

Related: [[engine-installer]], [[build-and-install]], [[testing]].
