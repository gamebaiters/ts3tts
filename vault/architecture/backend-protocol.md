# Backend protocol (backend/gbtts/protocol.py ⇄ plugin/src/net/BackendLink.cpp)

Frame: `u32 LE length (=1+payload)` · `u8 type` · payload. Max 16 MB.
- `0x01` JSON object (`op` plugin→backend, `ev` backend→plugin)
- `0x02` PCM backend→plugin: `u32 LE job id` + int16 LE mono 48 000 Hz, ≤ 4800 samples per frame
- `0x03` MIC plugin→backend (v1.3): `u32 LE seq` + int16 LE mono 48 000 Hz, 960 samples (20 ms); sent by `LinkWorker::pumpMic` (10 ms timer), dropped when the socket has > 256 KB unsent
- `0x04` VC backend→plugin (v1.3): `u32 LE seq` of the newest mic block used + converted int16 mono 48 kHz; `TtsAudio::pushVc` drops it when full/off (never parked: late voice is worse than a gap)

Topology: plugin **listens** on `127.0.0.1:0` (`LinkWorker::listen`), starts the backend with `--port --token --home --parent-pid`; backend connects and must send `{"ev":"hello","token":…}` first or the socket is aborted. Non-loopback peers are rejected. A new connection (restart) replaces the old one.

## Ops (plugin → backend)
| op | fields | handled |
|---|---|---|
| `configure` | engine, qwen_size, chunk_size, full_text_prefill, temperature, supertonic_steps, idle_unload_min, leveler, chat_slang, numbers, dictionary | worker; loads engine, first-run default voices |
| `speak` | id, text, lang, voice, speed, instruct, local | worker, FIFO |
| `cancel` / `cancel_all` | id | **reader thread, immediate** (epoch + current job event) |
| `voice_from_file` | req, name, path, ref_text, lang | worker |
| `voice_design_preview` | req, id, instruct, text, lang | worker; streams preview as job `id` |
| `voice_design_save` | req, name | worker |
| `voice_delete` / `voice_rename` | voice[, name] | worker |
| `list_voices`, `unload`, `ping`, `shutdown` | | |
| `vc_configure` (v1.3) | enabled, voice (`clone:<id>`), preset `40ms`/`120ms`, threads | reader → `VoiceChangerService` worker (download, load, embedding) |
| `vc_list_targets` (v1.3) | [req] | reader, immediate (library voices, any engine) |
| `vc_add_target` (v1.3) | req, name, path | VC worker; recording → library entry, no Qwen |

## Events (backend → plugin)
`hello`, `status{state: starting|loading|ready|busy|idle|warning|error, message, model, device, vram_used_mb, vram_total_mb}`, `engines[]`, `voices[]{id,name,kind,lang,engine,description,seconds}`, `job_start`, `job_first_audio{ttfa_ms}`, `job_end{cancelled,audio_ms,gen_ms,ttfa_ms,rtf}`, `job_error`, `design_ready{req,seconds}`, `voice_created{req,voice}`, `op_error{op,req,message}`, `pong`.
v1.2: `voices` carries `engine` (only that engine's voices) and per-voice `gender`, `lang`.
v1.3: `vc_status{state: off|downloading|loading|ready|error, message, preset, voice, latency_ms}`, `vc_targets{voices[]{id,name,seconds,source}}`, `vc_target_added{req,voice}`, `vc_stats{blocks,converted,dropped,phrases}` (every 5 s while active). See [[voice-changer]].

Voice ids: `clone:<12 hex>` (user library), `spk:<name>` (Qwen CustomVoice), `st:<M1..F5>` (Supertonic).

`job_end` is consumed **in-band** by `LinkWorker` (`pushEnd`) before being forwarded, so the audio core sees the end exactly after the last sample. If the rings are full the END is parked like a PCM frame.

Related: [[audio-core]], [[system-overview]].
