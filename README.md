# GameBaiters TTS

A TeamSpeak 3 plugin that lets you talk in a channel without a microphone: type a message, press
**Enter**, and a neural voice running **on your own PC** speaks it in the channel. Nothing is sent to
the cloud and no audio file is stored.

- **Three engines**, chosen in Settings: Qwen3-TTS (NVIDIA GPU, the most natural voice, ten
  Italian voices included, voice design and cloning), Kokoro and Supertonic (CPU, no VRAM — for
  when a game needs the GPU).
- **Live voice (AI)**: a real-time voice changer for your microphone (MeanVC2 on the CPU, ≈0.3 s).
- **Read my channel chat aloud** for whoever cannot read the chat.
- The full **GameBaiters Soundboard effects chain** on the voice.
- **Automatic updates**.

Italian interface by default (English in Settings › Interface). **Windows only** (TeamSpeak 3, 64-bit).

## Install

1. Download `gb_tts_<version>_win64.ts3_plugin` from the
   [latest release](https://github.com/gamebaiters/ts3tts/releases/latest), close TeamSpeak and
   double-click the file.
2. Start TeamSpeak: the **Install the voice engine** window opens by itself. Choose *Complete*
   (Qwen3-TTS on an NVIDIA graphics card + Kokoro, ≈8 GB download) or *Light* (Kokoro on the
   processor, ≈1 GB) and press **Install**. Nothing is needed beforehand — a private Python 3.12,
   the libraries and the models go into `%LOCALAPPDATA%\GameBaitersTTS` — and a progress bar
   shows every step. The engine starts by itself when it is done.

Quick guide (Italian): [GUIDA-RAPIDA.md](GUIDA-RAPIDA.md).

Later versions install themselves: the plugin asks when one is available (at most once a day,
or *Plugins › GameBaiters - TTS › Check for TTS updates…*).

## Repository layout

```
plugin/      C++17 / Qt 5.15.2 TeamSpeak plugin (audio injection, UI, talk state, updater)
backend/     Python 3.12 voice engine (TTS engines, voice changer), started by the plugin
installer/   install_backend.ps1: engine installer (uv, private Python, PyTorch, models), run by the plugin's window
tools/       package_plugin.ps1 (packaging, shared by build and CI), release_check.ps1
vault/       engineering notes (architecture, measurements, decisions, history)
```

## Build

Prerequisites: Visual Studio 2022 or Build Tools 2022 with the C++ x64 tools and a Windows SDK
(`winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`),
Qt 5.15.2 `msvc2019_64` (the Qt of the TeamSpeak client), CMake ≥ 3.16, Git, 7-Zip (optional),
and the GameBaiters Soundboard sources next to this folder: the DSP chain and effects editor are
compiled in place from [gamebaiters/RP-Soundboard](https://github.com/gamebaiters/RP-Soundboard).

```
Desktop\
├── SOUNDBOARD_4.0\upstream-clone\   git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard upstream-clone
└── TTS TS3\                         this repository
```

```powershell
.\build_local.ps1 -Local -QtDir <Qt>\5.15.2\msvc2019_64   # -> dist\gb_tts_<ver>_win64.ts3_plugin (Visual Studio found by vswhere)
.\build_local.ps1 -Local -Tests        # also the self-tests and the host harness (build\tests)
.\build_local.ps1 -Local -Install      # also copies into %APPDATA%\TS3Client\plugins (TeamSpeak closed)
```

Self-tests (`-DGBTTS_BUILD_TESTS=ON`, Qt `bin` on `PATH`): `gbtts_audio_selftest`,
`gbtts_settings_selftest`, `gbtts_update_selftest`, and `gbtts_host_harness`, which loads the real
DLL like the TeamSpeak client (`--cycles`, `--crash`, `--update`, `--setup-fake`, `--setup-real`,
`--code-sync`). Details in
[vault/build-release/testing.md](vault/build-release/testing.md).

## Releasing

Releases are built and published by GitHub Actions ([release.yml](.github/workflows/release.yml))
on a Windows runner.

1. Set the same version `X.Y.Z` in `plugin/CMakeLists.txt` (`GBTTS_VERSION`) and
   `backend/gbtts/__init__.py` (`__version__`).
2. Add a section on top of [release-notes.txt](release-notes.txt) starting with
   `GameBaiters TTS vX.Y.Z` and an `=====` line.
3. Check: `.\tools\release_check.ps1 -Version X.Y.Z`, then commit and push.
4. `git tag vX.Y.Z` and `git push origin vX.Y.Z`.

The workflow builds, runs the self-tests, packages the plugin, generates the updater feed
`version.xml` (with the package SHA-256) and publishes the release with the package, the feed,
`SHA256SUMS.txt` and the notes. Installed plugins find it through
`releases/latest/download/version.xml`. *Actions › Release (Windows) › Run workflow* builds a
snapshot artifact without publishing anything.

## Licenses

Third-party components and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
