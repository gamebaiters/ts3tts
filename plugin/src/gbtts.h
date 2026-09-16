#pragma once

#include <cstdint>

// Plugin lifecycle + TeamSpeak callback bridge. Called from plugin_exports.cpp.
namespace gbtts {

constexpr const char *kHotkeyFocus        = "gbtts_focus";
constexpr const char *kHotkeyStop         = "gbtts_stop";
constexpr const char *kHotkeyRepeat       = "gbtts_repeat";
constexpr const char *kHotkeyToggleWindow = "gbtts_toggle_window";
constexpr const char *kHotkeyPreview      = "gbtts_preview";
constexpr const char *kHotkeyClipboard    = "gbtts_clipboard";

void init();
void shutdown();

// GUI thread
bool onCommand(uint64_t sch, const char *command);
void onCurrentServerChanged(uint64_t sch);
void onConnectStatusChange(uint64_t sch, int newStatus, unsigned int errorNumber);
void onHotkey(const char *keyword);
void onTalkStatusChange(uint64_t sch, int status, int isReceivedWhisper, uint16_t clientID);
// Returns the value for ts3plugin_onTextMessageEvent (always 0: never hides a message).
int onTextMessage(uint64_t sch, uint16_t targetMode, uint16_t fromID, const char *message, int ffIgnored);
void openWindow(bool focusInput);
void openSettings();
void openVoices();
void restartBackend();
void openEngineSetup();
void checkForUpdates();

// TeamSpeak audio threads
void onCapture(uint64_t sch, short *samples, int frames, int channels, int *edited);
void onPlayback(uint64_t sch, short *samples, int frames, int channels,
                const unsigned int *speakers, unsigned int *fillMask);

} // namespace gbtts
