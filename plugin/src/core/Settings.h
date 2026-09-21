#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

namespace gbtts {

// Every user preference, persisted in QSettings("GameBaiters", "TTS")
// (HKCU\Software\GameBaiters\TTS). The backend installer writes backend/home
// into the same key so a fresh install is found without any configuration.
//
// Persistence policy (v1.2): the Controller writes on EVERY change - discrete
// choices immediately, continuous controls (sliders, typing) 300 ms after the
// last movement and immediately on release - so a TeamSpeak crash loses at most
// the last 300 ms of a slider drag.
struct Settings
{
    // ---- engine / backend ----
    QString backendHome;
    // Off by default: the engine only starts when the user explicitly
    // turns it on (toolbar power button / "Start" action / Settings
    // checkbox), never silently on every TeamSpeak launch.
    bool    autostart       = false;
    QString engine          = QStringLiteral("qwen");   // qwen | kokoro | supertonic
    QString qwenSize        = QStringLiteral("1.7B");   // 1.7B | 0.6B
    QString kokoroVariant   = QStringLiteral("fp32");   // fp32 (fast on CPU) | int8 (smaller, ~7x slower)
    int     chunkSize       = 4;                         // codec frames per streamed chunk (12 = 1 s)
    bool    fullTextPrefill = true;                      // model sees the whole sentence (better intonation)
    double  temperature     = 0.8;
    int     supertonicSteps = 10;
    int     idleUnloadMin   = 0;
    bool    leveler         = true;

    // ---- voice ----
    // The last voice chosen FOR EACH engine: switching engine and back restores it.
    QMap<QString, QString> voices;
    QString lang     = QStringLiteral("it");
    double  speed    = 1.0;
    QString instruct;

    // ---- audio ----
    double     remoteDb       = 0.0;
    double     localDb        = -6.0;
    double     voiceDb        = 0.0;
    double     pitchSt        = 0.0;
    bool       monitor        = true;
    int        micMode        = 0;       // TtsAudio::MicMode
    bool       speakWhenMuted = true;
    bool       previewOnly    = false;
    int        jitterMs       = 200;
    QByteArray sandboxJson;
    int        presetIndex    = 0;

    // ---- real-time voice changer ----
    bool    vcEnabled = false;                           // never on at startup unless the user left it on
    QString vcVoice;                                     // clone:<id> from the voice library
    QString vcPreset  = QStringLiteral("40ms");          // 40ms (reactive) | 120ms (light)
    bool    vcMonitor = false;

    // ---- text ----
    bool    chatSlang  = true;
    bool    numbers    = true;
    bool    echoToChat = false;
    bool    readChannelChat = false;   // speak the messages the user writes in the CHANNEL chat
    QString chatPrefix = QStringLiteral("[TTS] ");
    QMap<QString, QString> dictionary;

    // ---- ui ----
    bool        toolbarButton  = true;
    bool        clearAfterSend = true;
    bool        alwaysOnTop    = false;
    QStringList history;
    QByteArray  windowGeometry;
    QString     uiLanguage     = QStringLiteral("it");   // it | en

    // ---- updates (v1.5) ----
    bool   updateAutoCheck = true;
    qint64 updateNextCheck = 0;     // seconds since epoch: no automatic check before this

    QString currentVoice() const { return voices.value(engine); }

    void load();
    bool save() const;                 // false if the registry write failed
    QJsonObject backendConfig() const;

    static QStringList engineKeys();
    static QString engineOfVoice(const QString &voiceId);   // "ko:" kokoro, "st:" supertonic, else qwen
    // "TTS", or $GBTTS_SETTINGS_APP (self-tests use a throwaway key instead of
    // the user's real settings).
    static QString storeApplication();

    static constexpr int kHistoryMax = 60;
};

} // namespace gbtts
