// ---------------------------------------------------------------------------
// TeamSpeak 3 plugin SDK exports. Everything here is a thin C shim: the real
// work lives in gbtts.cpp. Audio callbacks run on TeamSpeak's audio threads
// and only touch lock-free state; every other callback runs on the client GUI
// thread and must never block (see SOUNDBOARD vault: ghost-crash-forensics).
// ---------------------------------------------------------------------------
#include "gbtts.h"

#include "common.h"
#include "plugin_definitions.h"
#include "ts3log.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define GBTTS_EXPORT __declspec(dllexport)
#else
#define GBTTS_EXPORT __attribute__((visibility("default")))
#endif

#define PLUGIN_API_VERSION 26

namespace gbtts {
extern std::string g_pluginId;
}

namespace {

void copyStr(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

PluginMenuItem *makeMenuItem(PluginMenuType type, int id, const char *text, const char *icon)
{
    auto *m = static_cast<PluginMenuItem *>(malloc(sizeof(PluginMenuItem)));
    m->type = type;
    m->id = id;
    copyStr(m->text, PLUGIN_MENU_BUFSZ, text);
    copyStr(m->icon, PLUGIN_MENU_BUFSZ, icon);
    return m;
}

PluginHotkey *makeHotkey(const char *keyword, const char *description)
{
    auto *h = static_cast<PluginHotkey *>(malloc(sizeof(PluginHotkey)));
    copyStr(h->keyword, PLUGIN_HOTKEY_BUFSZ, keyword);
    copyStr(h->description, PLUGIN_HOTKEY_BUFSZ, description);
    return h;
}

enum MenuId {
    MENU_OPEN = 1,
    MENU_SETTINGS,
    MENU_VOICES,
    MENU_RESTART_BACKEND,
    MENU_CHECK_UPDATES,
};

} // namespace

extern "C" {

GBTTS_EXPORT const char *ts3plugin_name() { return "GameBaiters - TTS"; }
GBTTS_EXPORT const char *ts3plugin_version() { return GBTTS_VERSION; }
GBTTS_EXPORT int ts3plugin_apiVersion() { return PLUGIN_API_VERSION; }
GBTTS_EXPORT const char *ts3plugin_author() { return "GameBaiters (gamebaiters.net)"; }
GBTTS_EXPORT const char *ts3plugin_description()
{
    return "Talk in the channel without a microphone: type, press Enter, a local neural voice "
           "(Qwen3-TTS on your GPU, Kokoro or Supertonic on the CPU) speaks for you. Voice cloning, "
           "voice design, a real-time AI voice changer for your microphone and the full GameBaiters "
           "Soundboard effects chain. Everything runs on this PC.";
}

GBTTS_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) { ts3Functions = funcs; }

GBTTS_EXPORT int ts3plugin_init()
{
    gbtts::init();
    return 0;
}

GBTTS_EXPORT void ts3plugin_shutdown()
{
    gbtts::shutdown();
}

GBTTS_EXPORT void ts3plugin_registerPluginID(const char *id)
{
    gbtts::g_pluginId = id ? id : "";
}

GBTTS_EXPORT const char *ts3plugin_commandKeyword() { return "tts"; }

GBTTS_EXPORT int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char *command)
{
    return gbtts::onCommand(serverConnectionHandlerID, command ? command : "") ? 0 : 1;
}

GBTTS_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID)
{
    gbtts::onCurrentServerChanged(serverConnectionHandlerID);
}

GBTTS_EXPORT void ts3plugin_freeMemory(void *data) { free(data); }
GBTTS_EXPORT int ts3plugin_requestAutoload() { return 1; }
GBTTS_EXPORT int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_CONFIGURE_QT_THREAD; }

GBTTS_EXPORT void ts3plugin_configure(void *, void *)
{
    gbtts::openSettings();
}

GBTTS_EXPORT void ts3plugin_initMenus(struct PluginMenuItem ***menuItems, char **menuIcon)
{
    constexpr size_t count = 5;
    *menuItems = static_cast<PluginMenuItem **>(malloc(sizeof(PluginMenuItem *) * (count + 1)));
    size_t n = 0;
    (*menuItems)[n++] = makeMenuItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_OPEN, "Open GameBaiters TTS", "gbtts_16.png");
    (*menuItems)[n++] = makeMenuItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_VOICES, "Voices...", "gbtts_16.png");
    (*menuItems)[n++] = makeMenuItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_SETTINGS, "TTS settings...", "gbtts_16.png");
    (*menuItems)[n++] = makeMenuItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_RESTART_BACKEND, "Restart TTS engine", "gbtts_16.png");
    (*menuItems)[n++] = makeMenuItem(PLUGIN_MENU_TYPE_GLOBAL, MENU_CHECK_UPDATES, "Check for TTS updates...", "gbtts_16.png");
    (*menuItems)[n++] = nullptr;
    assert(n == count + 1);

    *menuIcon = static_cast<char *>(malloc(PLUGIN_MENU_BUFSZ));
    copyStr(*menuIcon, PLUGIN_MENU_BUFSZ, "gbtts_16.png");
}

GBTTS_EXPORT void ts3plugin_initHotkeys(struct PluginHotkey ***hotkeys)
{
    constexpr size_t count = 6;
    *hotkeys = static_cast<PluginHotkey **>(malloc(sizeof(PluginHotkey *) * (count + 1)));
    size_t n = 0;
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyFocus, "TTS: open window and focus the text box");
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyStop, "TTS: stop speaking (clear queue)");
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyRepeat, "TTS: repeat last message");
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyToggleWindow, "TTS: show / hide window");
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyPreview, "TTS: toggle 'only I hear it' preview");
    (*hotkeys)[n++] = makeHotkey(gbtts::kHotkeyClipboard, "TTS: speak clipboard text");
    (*hotkeys)[n++] = nullptr;
    assert(n == count + 1);
}

GBTTS_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus,
                                                      unsigned int errorNumber)
{
    gbtts::onConnectStatusChange(serverConnectionHandlerID, newStatus, errorNumber);
}

GBTTS_EXPORT void ts3plugin_onEditMixedPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, short *samples,
                                                             int sampleCount, int channels,
                                                             const unsigned int *channelSpeakerArray,
                                                             unsigned int *channelFillMask)
{
    gbtts::onPlayback(serverConnectionHandlerID, samples, sampleCount, channels, channelSpeakerArray,
                      channelFillMask);
}

GBTTS_EXPORT void ts3plugin_onEditCapturedVoiceDataEvent(uint64 serverConnectionHandlerID, short *samples,
                                                        int sampleCount, int channels, int *edited)
{
    gbtts::onCapture(serverConnectionHandlerID, samples, sampleCount, channels, edited);
}

GBTTS_EXPORT void ts3plugin_onMenuItemEvent(uint64, enum PluginMenuType type, int menuItemID, uint64)
{
    if (type != PLUGIN_MENU_TYPE_GLOBAL) return;
    switch (menuItemID) {
    case MENU_OPEN: gbtts::openWindow(false); break;
    case MENU_VOICES: gbtts::openVoices(); break;
    case MENU_SETTINGS: gbtts::openSettings(); break;
    case MENU_RESTART_BACKEND: gbtts::restartBackend(); break;
    case MENU_CHECK_UPDATES: gbtts::checkForUpdates(); break;
    default: break;
    }
}

GBTTS_EXPORT void ts3plugin_onHotkeyEvent(const char *keyword)
{
    gbtts::onHotkey(keyword ? keyword : "");
}

GBTTS_EXPORT int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID,
                                             anyID fromID, const char *fromName, const char *fromUniqueIdentifier,
                                             const char *message, int ffIgnored)
{
    (void)toID;
    (void)fromName;
    (void)fromUniqueIdentifier;
    return gbtts::onTextMessage(serverConnectionHandlerID, targetMode, fromID, message, ffIgnored);
}

GBTTS_EXPORT void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status,
                                                   int isReceivedWhisper, anyID clientID)
{
    gbtts::onTalkStatusChange(serverConnectionHandlerID, status, isReceivedWhisper, clientID);
}

} // extern "C"
