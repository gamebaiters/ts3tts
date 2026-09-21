# GameBaiters TTS — Vault HOME (Map of Content)

Obsidian-style vault. Open this folder in Obsidian: the graph view is the mind map.
**Agents:** `Grep vault/ -i <keyword>` before reading source; identifiers are verbatim, line numbers drift.

## Architecture
- [[system-overview]] — the three processes/threads worlds, end-to-end flow from Enter to the channel
- [[audio-core]] — `TtsAudio`: rings, pump, readers, clock hand-over, fades, underrun, tail
- [[backend-protocol]] — framed loopback TCP, token handshake, ops/events, flow control
- [[engines-and-models]] — Qwen3-TTS / Kokoro / Supertonic, model roles, VRAM, measured latency + intelligibility, version pins
- [[text-normalization]] — Italian chat text → speakable text, segmentation
- [[voice-changer]] — real-time AI voice changer: model research, MeanVC2 measurements on Italian, plugin/backend streaming design, limits

## TeamSpeak integration
- [[ts3-integration]] — SDK exports, callbacks, hotkeys, `/tts`, talk-state override, muted-mic handling
- [[crash-safety]] — rules inherited from SOUNDBOARD ghost-crash forensics and how each is applied here
- [[audio-bridge]] — cross-plugin shared-memory bridge with the GameBaiters Soundboard (capture-buffer coexistence)

## UI
- [[ui-map]] — TtsWindow, VoiceDialog, SettingsDialog, toolbar overlay, reused Soundboard widgets

## Build, install, test
- [[build-and-install]] — build_local.ps1, package layout, backend installer, dev layout
- [[engine-installer]] — installation window, uv + private Python, status.json contract, detached/re-attach, measured times
- [[release-and-update]] — GitHub repo, tag → release workflow, version.xml feed, auto-update helper, engine code refresh
- [[testing]] — bench (TTFA/RTF/VRAM + Whisper CER), backend e2e, audio-core + settings selftests, host lifecycle/leak/crash harness

## History
- [[v1.1.0]] — ⭐ latest: engine off by default, power on/off button, DSP-tail ring-out no longer overholds for non-tail effects, cross-plugin audio bridge with the GameBaiters Soundboard.
- [[v1.0.0]] — first release: decisions, rejected alternatives, bugs found while building
- [[v1.1.0]] — Kokoro light engine, Italian default
- [[v1.2.0]] — engine chosen in Settings, per-engine voices, save on every change, singleShot-after-unload crash found and fixed, crash/leak harness
- [[v1.3.0]] — real-time AI voice changer (MeanVC2, CPU), reader starvation click fixed
- [[v1.4.0]] — read my own channel chat aloud; messages typed while the engine starts are queued (the "30 s" chat echo)
- [[v1.5.0]] — engine installation window (no Python needed), auto-update, GitHub release pipeline, chat switch next to the text box

## Upstream knowledge
The Soundboard vault (`../SOUNDBOARD_4.0/vault/HOME.md`) documents the reused DSP chain ([[dsp-pipeline]]), the sandbox dialog, theme traps and the original talk-state saga.
