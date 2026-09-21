# Cross-plugin audio bridge (Soundboard ↔ TTS)

`src/ipc/GbAudioBridge.h` — header-only, **byte-for-byte identical** copy in both repos (`SOUNDBOARD_4.0/upstream-clone/src/ipc/` and `TTS TS3/plugin/src/ipc/`). Bump `kAbiVersion` (validated on open) if the layout ever changes; never edit one copy without the other.

## The bug it fixes

Both plugins are separate DLLs that can load into the SAME `ts3client_win64.exe`. TS3 chains every loaded plugin's `ts3plugin_onEditCapturedVoiceDataEvent` through the SAME `short *samples` buffer, in whatever order it happened to load them. Each plugin's own "replace the mic while I'm talking" logic (Soundboard's VAD-gate silence in `sb_handleCaptureData`, TTS's `MicReplace`/`MicDuck` in `TtsAudio::onCapture`) used to blindly zero/overwrite the WHOLE buffer with no idea the other plugin might have already written something real there — whichever plugin's callback TS3 invoked second stomped the first's contribution. Symptom: playing a soundboard sound could cut the TTS off mid-sentence, or vice versa.

## The fix: Soundboard is the single writer

- Both plugins write a heartbeat (`soundboardHeartbeatMs` / `ttsHeartbeatMs`, `nowMsSteady()`) on every capture callback, independent of whether they have audio to contribute — that's the presence check (`kStaleMs = 250`).
- TTS additionally publishes, every capture callback via `Shared::publishTts()` (seqlock: odd=writing, even=consistent): this block's already-fully-DSP-processed stereo audio, its desired mic policy (`None`/`Duck`/`Silence`), and whether it currently has audio to contribute (`active`). Published even when `active==false` (e.g. mid-phrase gaps, the ~600 ms trailing hold) so the Soundboard's read of the *policy* doesn't go stale early even though the *audio* is empty.
- **When `bridge->soundboardFresh()` is true**, TTS's own `onCapture` does NOT touch the shared `samples` buffer at all (neither mic policy nor its own audio mix) — it only publishes to the bridge. Falls straight back to its original solo behaviour (direct writes) the instant Soundboard is absent or stale.
- Soundboard's `sb_handleCaptureData` applies TTS's published mic policy to the REAL mic BEFORE `Sampler::fetchInputSamples()` mixes the soundboard's own slot audio in (so silencing the mic for TTS never also mutes the soundboard's own sound), then — as the LAST step, after `fetchInputSamples` — additively mixes TTS's published audio in. This is what "TTS is inserted at the end, after its own effects, before playback" means concretely.

Works regardless of which plugin's callback TS3 invokes first for a given tick: whichever runs second either finds the buffer already correctly combined (Soundboard ran first) or finds Soundboard fresh and no-ops (TTS ran first, Soundboard's own callback picks up the still-raw mic a moment later and folds TTS in itself). Solo-plugin installs are completely unaffected (the other side's heartbeat is simply always stale).

## Talk-state (NOT yet unified)

Each plugin's TalkStateManager/TalkState port still independently forces `TS_CONT_TRANS` and restores the user's mode on its own schedule. Both already reject a `TS_CONT_TRANS` snapshot (fallback to last known user mode), so the documented worst case when both overlap is "restores to PTT for a VAD user" — not addressed by the audio bridge, which is purely about the capture SAMPLE buffer, not SDK talk-state calls.

Related: [[system-overview]], [[audio-core]], [[ts3-integration]]. Soundboard vault: `platform/audio-bridge.md`, `platform/ts3-talkstate.md`, `history/v2.4.0.md`.
