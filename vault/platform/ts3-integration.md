# TeamSpeak integration

## Exports (plugin/src/plugin_exports.cpp)
API 26. Name "GameBaiters - TTS". `commandKeyword` = `tts` → `/tts <text>` speaks, `/tts stop`, `/tts repeat|ripeti`, `/tts` opens the window. Menus (4, count asserted): Open, Voices…, TTS settings…, Restart TTS engine; icon `plugins/gb_tts/gbtts_16.png` (packaged, also written at init if missing).
Hotkeys: `gbtts_focus`, `gbtts_stop`, `gbtts_repeat`, `gbtts_toggle_window`, `gbtts_preview`, `gbtts_clipboard`. Hotkey + command callbacks are **deferred** with `deferCall(g_ctx, 0, …)` (`core/Defer.h`; never `QTimer::singleShot` with a lambda — see [[crash-safety]]).

## Server targeting
`Controller::targetServer()` = last `currentServerConnectionChanged` tab, else `getCurrentServerConnectionHandlerID()`. `TtsAudio::setActiveServer` only changes while nothing is playing (an utterance never jumps tabs). Disconnect of the active server → `flush` + `cancel_all` + `TalkState::onConnectionLost` (no ts3Functions).
Not connected → jobs are forced local (only you hear them) and a notice says so.

## Talk state (plugin/src/ts3/TalkState.*) — port of Soundboard `TalkStateManager`
Driven by `Controller::onTick` (20 ms): transmission is wanted while a non-local job is `Speaking` or `TtsAudio::remoteActive()`, released 350 ms after the last activity. Preview-only never touches TeamSpeak.
Guards kept verbatim: 150 ms INPUT_ACTIVE watchdog, reject TS_CONT_TRANS snapshot (fallback last user mode per server, else PTT), VAD re-init on restore, restore-verify at 80/220/500 ms, `onConnectionLost` without SDK calls. See Soundboard vault `platform/ts3-talkstate.md`.

### Muted microphone (new)
"Speak even when muted": at `begin`, if `CLIENT_INPUT_MUTED == MUTEINPUT_MUTED` → `setForceMicSilence(true)` **first**, then lift the mute; at `end` put `MUTEINPUT_MUTED` back only if it is still `NONE` (user did not change it), release mic silence 300 ms later. If `CLIENT_INPUT_HARDWARE == 0` → `activateCaptureDevice`.

### Coexistence with the Soundboard
Both plugins can override talk state. Each rejects a `TS_CONT_TRANS` snapshot, so the worst case when both overlap is a restore to PTT for a VAD user on that server (same fallback the Soundboard uses).

## Chat echo
Optional: `requestSendChannelTextMsg(sch, prefix + text, getChannelOfClient(me))`. Sent immediately at Enter, even while the engine is still starting (the speech is queued, v1.4). Each echo is remembered (BBCode-stripped, 2 min) so "read my channel chat" does not speak it a second time.

## Read my channel chat aloud (v1.4)
`ts3plugin_onTextMessageEvent(sch, targetMode, toID, fromID, fromName, fromUID, message, ffIgnored)` → `gbtts::onTextMessage`. The client delivers the user's own messages too; filtered to `TextMessageTarget_CHANNEL` + `fromID == getClientID(sch)` + not ignored, setting `text/read_channel_chat` on. Returns 0 (never hides a message), defers the work, `Controller::onOwnChannelMessage` strips BBCode and speaks on that server tab (no history, no echo). Private (`TextMessageTarget_CLIENT`) and server chat are never read. Harness-verified: [[v1.4.0]].

## Messages typed while the engine starts (v1.4)
Queued jobs (`sent = false`) instead of a refusal; sent by `flushPendingJobs()` on `voices` / status `ready`; cancelled by stop, failed on disconnect or when restarts give up.

Related: [[crash-safety]], [[audio-core]].
