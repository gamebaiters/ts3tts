# UI map

All windows: `setProperty("isGBSoundboard", true)` + `Theme::compositeStyleSheet()` + `Theme::trackThemedWidget` → identical look to the Soundboard and immune to the host QSS. Theme colours read at init from the Soundboard INI (`rp_soundboard.ini`: `theme_enabled/accent/waveform/background/contrast/text/button`, `ui_font_pt`) when present.

## TtsWindow (plugin/src/ui/TtsWindow.*)
- status row: `StatusDot` (grey stopped/not installed, amber starting/loading/busy, green ready, red error) + caption + detail (model · GPU · VRAM · last TTFA · server connected), context action (Install… / Start), Voices, Settings;
- SectionBox "Voice" (`sections/gbtts_window_voice`), **active engine only (v1.2)**: engine badge (label + tagline, tooltip = measured cost) + "Change engine…" (opens Settings); voice combo with that engine's voices grouped by `VoiceText::fillVoices` (Qwen: your voices / stock ⬇ · Kokoro: per language, Italian first · Supertonic: women / men · ♀/♂) + Listen (local preview); language combo = `Controller::engineLanguages(engine)`; speed 0.5–2.0 (WSOLA for Qwen, native for Kokoro/Supertonic); style instruct visible only for Qwen. Every control persists through the Controller (sliders: 300 ms debounce + save on release); geometry saved on move/resize;
- SectionBox "Output and effects": Channel dB, Me dB + Hear it, Pitch st (bipolar), preset combo (+ "Custom" when state ≠ preset), Effects… (`ChannelSandboxDialog` micMode, retitled), `ChannelMeter`, mic mode, Speak even when muted, Only for me;
- SectionBox "Live voice (AI)" (`gbtts_window_vc`, v1.3): "Change my voice while I talk", target combo (library voices from `vc_targets`) + add-from-recording button (consent question), latency preset Reactive 40 ms / Light 120 ms, "Hear myself", coloured status (off / loading with "normal voice meanwhile" / on with delay / error), output meter. [[voice-changer]]
- history list (session jobs: time, text, voice, state, "only me"), double-click = say again, context menu;
- `InputBox`: Enter speaks, Shift/Ctrl+Enter newline, Esc stops, ↑/↓ history; Speak/Stop buttons; hint with queue count; notice line (auto-hide).

## VoiceDialog
Active engine only: title "N voices of <engine>", search (name / group / description), group headers (non-selectable), "✓ in use"; use, listen = local preview, rename/delete for your voices. Right side is a `QStackedWidget`: with Qwen the tabs "Design from a description" (examples, sample sentence, preview → save) and "Clone from a recording" (file, transcript, language, **consent checkbox required**); with Kokoro/Supertonic an info page (what the engine is, fixed voice set, creation needs Qwen) + "Choose the engine in Settings…".

## SettingsDialog
Scroll + SectionBoxes, Close outside the scroll (Soundboard rule 10). **Voice engine**: one `EngineCard` per engine (radio, tagline, description, measured resources, status: red "Not available on this PC: reason" / green "Active — N voices"; whole card clickable, accent border when selected) → `Controller::setEngine`; below, "<engine> options" = `QStackedWidget` page of the selected engine only (Qwen: model size, streaming step with ms hint, whole-sentence prefill, expressiveness, idle unload, free GPU, manage voices · Kokoro: fp32/int8 · Supertonic: quality steps) · Engine installation (folder, status, autostart, start/stop, log, folder, install/repair with explicit download consent) · Audio (voice level, leveler, start buffer) · Text (numbers, abbreviations, chat echo + prefix, pronunciation dictionary) · Interface (toolbar button, clear after send, always on top, language, "Every change is saved immediately").

## Toolbar overlay (plugin/src/ui/ToolbarButton.*)
Soundboard v3 overlay (child widget + `move()`, never an action). Sits after any foreign overlay QToolButton near the left cluster (the Soundboard button), `objectName gbTtsToolbarButton`, two-pass reposition (0 ms + 60 ms) so it sees the Soundboard button's final geometry.

## i18n
English sources; `plugin/i18n/gbtts_it.ts` filled by `translate_it.py`; compiled by the build (`qt5_add_translation`) into `:/gbtts/i18n/gbtts_it.qm`. The Soundboard catalogue is embedded too (`:/gbtts/i18n/soundboard_it.qm`) for the reused effects editor. Language: `ui/language` it|en (v1.0 `auto` migrated to it), applied at plugin load.
Backend status messages are Italian strings from Python.

Related: [[crash-safety]], [[ts3-integration]].
