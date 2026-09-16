#include "core/Settings.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSettings>
#include <QVariant>

namespace gbtts {

QStringList Settings::engineKeys()
{
    return {QStringLiteral("qwen"), QStringLiteral("kokoro"), QStringLiteral("supertonic")};
}

QString Settings::engineOfVoice(const QString &voiceId)
{
    if (voiceId.startsWith(QLatin1String("ko:"))) return QStringLiteral("kokoro");
    if (voiceId.startsWith(QLatin1String("st:"))) return QStringLiteral("supertonic");
    return QStringLiteral("qwen");
}

QString Settings::storeApplication()
{
    const QString override = QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_SETTINGS_APP"));
    return override.isEmpty() ? QStringLiteral("TTS") : override;
}

void Settings::load()
{
    QSettings s(QStringLiteral("GameBaiters"), storeApplication());
    const Settings d;

    s.beginGroup(QStringLiteral("backend"));
    backendHome     = s.value(QStringLiteral("home"), d.backendHome).toString();
    autostart       = s.value(QStringLiteral("autostart"), d.autostart).toBool();
    engine          = s.value(QStringLiteral("engine"), d.engine).toString();
    qwenSize        = s.value(QStringLiteral("qwen_size"), d.qwenSize).toString();
    kokoroVariant   = s.value(QStringLiteral("kokoro_variant"), d.kokoroVariant).toString();
    chunkSize       = s.value(QStringLiteral("chunk_size"), d.chunkSize).toInt();
    fullTextPrefill = s.value(QStringLiteral("full_text_prefill"), d.fullTextPrefill).toBool();
    temperature     = s.value(QStringLiteral("temperature"), d.temperature).toDouble();
    supertonicSteps = s.value(QStringLiteral("supertonic_steps"), d.supertonicSteps).toInt();
    idleUnloadMin   = s.value(QStringLiteral("idle_unload_min"), d.idleUnloadMin).toInt();
    leveler         = s.value(QStringLiteral("leveler"), d.leveler).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("voice"));
    voices.clear();
    const QVariantMap perEngine = s.value(QStringLiteral("per_engine")).toMap();
    for (auto it = perEngine.cbegin(); it != perEngine.cend(); ++it)
        voices.insert(it.key(), it.value().toString());
    const QString legacy = s.value(QStringLiteral("id")).toString();   // v1.0/v1.1 single voice
    lang     = s.value(QStringLiteral("lang"), d.lang).toString();
    speed    = s.value(QStringLiteral("speed"), d.speed).toDouble();
    instruct = s.value(QStringLiteral("instruct"), d.instruct).toString();
    s.endGroup();

    s.beginGroup(QStringLiteral("audio"));
    remoteDb       = s.value(QStringLiteral("remote_db"), d.remoteDb).toDouble();
    localDb        = s.value(QStringLiteral("local_db"), d.localDb).toDouble();
    voiceDb        = s.value(QStringLiteral("voice_db"), d.voiceDb).toDouble();
    pitchSt        = s.value(QStringLiteral("pitch_st"), d.pitchSt).toDouble();
    monitor        = s.value(QStringLiteral("monitor"), d.monitor).toBool();
    micMode        = s.value(QStringLiteral("mic_mode"), d.micMode).toInt();
    speakWhenMuted = s.value(QStringLiteral("speak_when_muted"), d.speakWhenMuted).toBool();
    previewOnly    = s.value(QStringLiteral("preview_only"), d.previewOnly).toBool();
    jitterMs       = s.value(QStringLiteral("jitter_ms"), d.jitterMs).toInt();
    sandboxJson    = s.value(QStringLiteral("sandbox"), d.sandboxJson).toByteArray();
    presetIndex    = s.value(QStringLiteral("preset"), d.presetIndex).toInt();
    s.endGroup();

    s.beginGroup(QStringLiteral("voice_changer"));
    vcEnabled = s.value(QStringLiteral("enabled"), d.vcEnabled).toBool();
    vcVoice   = s.value(QStringLiteral("voice"), d.vcVoice).toString();
    vcPreset  = s.value(QStringLiteral("preset"), d.vcPreset).toString();
    vcMonitor = s.value(QStringLiteral("monitor"), d.vcMonitor).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("text"));
    chatSlang  = s.value(QStringLiteral("chat_slang"), d.chatSlang).toBool();
    numbers    = s.value(QStringLiteral("numbers"), d.numbers).toBool();
    echoToChat = s.value(QStringLiteral("echo_to_chat"), d.echoToChat).toBool();
    readChannelChat = s.value(QStringLiteral("read_channel_chat"), d.readChannelChat).toBool();
    chatPrefix = s.value(QStringLiteral("chat_prefix"), d.chatPrefix).toString();
    dictionary.clear();
    const QVariantMap dict = s.value(QStringLiteral("dictionary")).toMap();
    for (auto it = dict.cbegin(); it != dict.cend(); ++it)
        dictionary.insert(it.key(), it.value().toString());
    s.endGroup();

    s.beginGroup(QStringLiteral("ui"));
    toolbarButton  = s.value(QStringLiteral("toolbar_button"), d.toolbarButton).toBool();
    clearAfterSend = s.value(QStringLiteral("clear_after_send"), d.clearAfterSend).toBool();
    alwaysOnTop    = s.value(QStringLiteral("always_on_top"), d.alwaysOnTop).toBool();
    history        = s.value(QStringLiteral("history")).toStringList();
    windowGeometry = s.value(QStringLiteral("geometry")).toByteArray();
    uiLanguage     = s.value(QStringLiteral("language"), d.uiLanguage).toString();
    s.endGroup();

    s.beginGroup(QStringLiteral("update"));
    updateAutoCheck = s.value(QStringLiteral("auto_check"), d.updateAutoCheck).toBool();
    updateNextCheck = s.value(QStringLiteral("next_check"), d.updateNextCheck).toLongLong();
    s.endGroup();

    // ---- sanitise + migrate -------------------------------------------------
    if (!engineKeys().contains(engine)) engine = d.engine;
    if (qwenSize != QLatin1String("1.7B") && qwenSize != QLatin1String("0.6B")) qwenSize = d.qwenSize;
    if (kokoroVariant != QLatin1String("int8") && kokoroVariant != QLatin1String("fp32")) kokoroVariant = d.kokoroVariant;
    if (vcPreset != QLatin1String("40ms") && vcPreset != QLatin1String("120ms")) vcPreset = d.vcPreset;
    if (!vcVoice.isEmpty() && !vcVoice.startsWith(QLatin1String("clone:"))) vcVoice.clear();
    // Per-engine voice map (v1.2): keep only entries whose prefix matches their
    // engine; seed it from the single v1.0/v1.1 voice the first time.
    for (auto it = voices.begin(); it != voices.end();) {
        if (!engineKeys().contains(it.key()) || it.value().isEmpty() || engineOfVoice(it.value()) != it.key())
            it = voices.erase(it);
        else
            ++it;
    }
    if (!legacy.isEmpty() && !voices.contains(engineOfVoice(legacy))) voices.insert(engineOfVoice(legacy), legacy);
    // Italian is the default interface language (v1.1): the old "auto" value is
    // migrated; only an explicit English choice keeps English.
    if (uiLanguage != QLatin1String("en")) uiLanguage = QStringLiteral("it");
    if (lang.isEmpty()) lang = d.lang;
    chunkSize = qBound(1, chunkSize, 12);
    temperature = qBound(0.3, temperature, 1.2);
    supertonicSteps = qBound(4, supertonicSteps, 16);
    speed = qBound(0.5, speed, 2.0);
    micMode = qBound(0, micMode, 2);
    jitterMs = qBound(40, jitterMs, 1000);
    idleUnloadMin = qBound(0, idleUnloadMin, 240);
    // A next-check time far in the future (clock was wrong once) must not silence updates forever.
    if (updateNextCheck < 0 || updateNextCheck > QDateTime::currentSecsSinceEpoch() + 7 * 24 * 3600) updateNextCheck = 0;
    while (history.size() > kHistoryMax) history.removeFirst();
}

bool Settings::save() const
{
    QSettings s(QStringLiteral("GameBaiters"), storeApplication());

    s.beginGroup(QStringLiteral("backend"));
    s.setValue(QStringLiteral("home"), backendHome);
    s.setValue(QStringLiteral("autostart"), autostart);
    s.setValue(QStringLiteral("engine"), engine);
    s.setValue(QStringLiteral("qwen_size"), qwenSize);
    s.setValue(QStringLiteral("kokoro_variant"), kokoroVariant);
    s.setValue(QStringLiteral("chunk_size"), chunkSize);
    s.setValue(QStringLiteral("full_text_prefill"), fullTextPrefill);
    s.setValue(QStringLiteral("temperature"), temperature);
    s.setValue(QStringLiteral("supertonic_steps"), supertonicSteps);
    s.setValue(QStringLiteral("idle_unload_min"), idleUnloadMin);
    s.setValue(QStringLiteral("leveler"), leveler);
    s.endGroup();

    s.beginGroup(QStringLiteral("voice"));
    QVariantMap perEngine;
    for (auto it = voices.cbegin(); it != voices.cend(); ++it) perEngine.insert(it.key(), it.value());
    s.setValue(QStringLiteral("per_engine"), perEngine);
    s.setValue(QStringLiteral("id"), currentVoice());   // readable by v1.1 if the user downgrades
    s.setValue(QStringLiteral("lang"), lang);
    s.setValue(QStringLiteral("speed"), speed);
    s.setValue(QStringLiteral("instruct"), instruct);
    s.endGroup();

    s.beginGroup(QStringLiteral("audio"));
    s.setValue(QStringLiteral("remote_db"), remoteDb);
    s.setValue(QStringLiteral("local_db"), localDb);
    s.setValue(QStringLiteral("voice_db"), voiceDb);
    s.setValue(QStringLiteral("pitch_st"), pitchSt);
    s.setValue(QStringLiteral("monitor"), monitor);
    s.setValue(QStringLiteral("mic_mode"), micMode);
    s.setValue(QStringLiteral("speak_when_muted"), speakWhenMuted);
    s.setValue(QStringLiteral("preview_only"), previewOnly);
    s.setValue(QStringLiteral("jitter_ms"), jitterMs);
    s.setValue(QStringLiteral("sandbox"), sandboxJson);
    s.setValue(QStringLiteral("preset"), presetIndex);
    s.endGroup();

    s.beginGroup(QStringLiteral("voice_changer"));
    s.setValue(QStringLiteral("enabled"), vcEnabled);
    s.setValue(QStringLiteral("voice"), vcVoice);
    s.setValue(QStringLiteral("preset"), vcPreset);
    s.setValue(QStringLiteral("monitor"), vcMonitor);
    s.endGroup();

    s.beginGroup(QStringLiteral("text"));
    s.setValue(QStringLiteral("chat_slang"), chatSlang);
    s.setValue(QStringLiteral("numbers"), numbers);
    s.setValue(QStringLiteral("echo_to_chat"), echoToChat);
    s.setValue(QStringLiteral("read_channel_chat"), readChannelChat);
    s.setValue(QStringLiteral("chat_prefix"), chatPrefix);
    QVariantMap dict;
    for (auto it = dictionary.cbegin(); it != dictionary.cend(); ++it) dict.insert(it.key(), it.value());
    s.setValue(QStringLiteral("dictionary"), dict);
    s.endGroup();

    s.beginGroup(QStringLiteral("ui"));
    s.setValue(QStringLiteral("toolbar_button"), toolbarButton);
    s.setValue(QStringLiteral("clear_after_send"), clearAfterSend);
    s.setValue(QStringLiteral("always_on_top"), alwaysOnTop);
    s.setValue(QStringLiteral("history"), history);
    s.setValue(QStringLiteral("geometry"), windowGeometry);
    s.setValue(QStringLiteral("language"), uiLanguage);
    s.endGroup();

    s.beginGroup(QStringLiteral("update"));
    s.setValue(QStringLiteral("auto_check"), updateAutoCheck);
    s.setValue(QStringLiteral("next_check"), updateNextCheck);
    s.endGroup();

    // Flush now (not at some later event-loop idle): this is the durability point.
    s.sync();
    return s.status() == QSettings::NoError;
}

QJsonObject Settings::backendConfig() const
{
    QJsonObject dict;
    for (auto it = dictionary.cbegin(); it != dictionary.cend(); ++it) dict.insert(it.key(), it.value());
    return QJsonObject{
        {QStringLiteral("op"), QStringLiteral("configure")},
        {QStringLiteral("engine"), engine},
        {QStringLiteral("qwen_size"), qwenSize},
        {QStringLiteral("kokoro_variant"), kokoroVariant},
        {QStringLiteral("chunk_size"), chunkSize},
        {QStringLiteral("full_text_prefill"), fullTextPrefill},
        {QStringLiteral("temperature"), temperature},
        {QStringLiteral("supertonic_steps"), supertonicSteps},
        {QStringLiteral("idle_unload_min"), idleUnloadMin},
        {QStringLiteral("leveler"), leveler},
        {QStringLiteral("chat_slang"), chatSlang},
        {QStringLiteral("numbers"), numbers},
        {QStringLiteral("dictionary"), dict},
    };
}

} // namespace gbtts
