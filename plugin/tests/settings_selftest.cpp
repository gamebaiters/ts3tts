// ---------------------------------------------------------------------------
// Settings persistence self-test (isolated key HKCU\Software\GameBaiters\TTS-selftest):
//   migration   v1.1 single voice -> per-engine map, "auto" UI language -> it,
//               entries whose prefix does not match their engine are dropped
//   roundtrip   every field written by save() reads back identical
//   cost        one save() = one registry flush; measured, because v1.2 saves on
//               every change
// ---------------------------------------------------------------------------
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSettings>
#include <QVariantMap>

#include <cstdio>

using namespace gbtts;

namespace {

int g_pass = 0, g_fail = 0;

void check(const char *name, bool ok, const QString &detail = QString())
{
    std::printf("  %s %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.toUtf8().constData());
    (ok ? g_pass : g_fail)++;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("GBTTS_SETTINGS_APP", "TTS-selftest");
    QCoreApplication app(argc, argv);
    std::printf("GameBaiters TTS - settings self-test\n");
    check("isolated store in use", Settings::storeApplication() == QLatin1String("TTS-selftest"),
          Settings::storeApplication());

    {   // a v1.1 install, plus some corrupt values
        QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("TTS-selftest"));
        s.clear();
        s.setValue(QStringLiteral("backend/engine"), QStringLiteral("kokoro"));
        s.setValue(QStringLiteral("voice/id"), QStringLiteral("clone:377c96f36c56"));
        s.setValue(QStringLiteral("ui/language"), QStringLiteral("auto"));
        s.setValue(QStringLiteral("backend/chunk_size"), 99);
        s.setValue(QStringLiteral("backend/kokoro_variant"), QStringLiteral("fp16"));
        s.setValue(QStringLiteral("voice_changer/preset"), QStringLiteral("10ms"));
        s.setValue(QStringLiteral("voice_changer/voice"), QStringLiteral("ko:if_sara"));   // not a library voice
        s.setValue(QStringLiteral("update/next_check"), QDateTime::currentSecsSinceEpoch() + 400LL * 24 * 3600);
        QVariantMap pe;
        pe.insert(QStringLiteral("kokoro"), QStringLiteral("st:F1"));      // wrong engine prefix
        pe.insert(QStringLiteral("supertonic"), QStringLiteral("st:M2"));
        pe.insert(QStringLiteral("bogus"), QStringLiteral("x"));
        s.setValue(QStringLiteral("voice/per_engine"), pe);
        s.sync();
    }
    Settings a;
    a.load();
    check("engine kept", a.engine == QLatin1String("kokoro"), a.engine);
    check("legacy voice migrated to its own engine", a.voices.value(QStringLiteral("qwen")) == QLatin1String("clone:377c96f36c56"));
    check("voice with a foreign prefix dropped", !a.voices.contains(QStringLiteral("kokoro")));
    check("valid per-engine voice kept", a.voices.value(QStringLiteral("supertonic")) == QLatin1String("st:M2"));
    check("unknown engine key dropped", !a.voices.contains(QStringLiteral("bogus")));
    check("current voice empty until one is picked", a.currentVoice().isEmpty());
    check("UI language auto -> it", a.uiLanguage == QLatin1String("it"), a.uiLanguage);
    check("out-of-range chunk clamped", a.chunkSize == 12, QString::number(a.chunkSize));
    check("unknown Kokoro variant reset", a.kokoroVariant == QLatin1String("fp32"), a.kokoroVariant);

    a.voices.insert(QStringLiteral("kokoro"), QStringLiteral("ko:if_sara"));
    a.speed = 1.37;
    a.remoteDb = -4.5;
    a.previewOnly = true;
    a.history = QStringList{QStringLiteral("uno"), QStringLiteral("due è così")};
    a.windowGeometry = QByteArray("\x01\x02\x00\x03", 4);
    a.dictionary.insert(QStringLiteral("GB"), QStringLiteral("gi bi"));
    check("voice changer: unknown preset reset", a.vcPreset == QLatin1String("40ms"), a.vcPreset);
    check("voice changer: non-library target dropped", a.vcVoice.isEmpty(), a.vcVoice);
    check("voice changer: off by default", !a.vcEnabled);
    check("read channel chat: off by default", !a.readChannelChat);
    check("updates: automatic check on by default", a.updateAutoCheck);
    check("updates: next check a year away (bad clock) reset", a.updateNextCheck == 0,
          QString::number(a.updateNextCheck));
    const qint64 nextCheck = QDateTime::currentSecsSinceEpoch() + 3 * 24 * 3600;
    a.updateAutoCheck = false;
    a.updateNextCheck = nextCheck;
    a.sandboxJson = QByteArray("{\"enabled\":true}");
    a.vcEnabled = true;
    a.vcVoice = QStringLiteral("clone:377c96f36c56");
    a.vcPreset = QStringLiteral("120ms");
    a.vcMonitor = true;
    a.readChannelChat = true;
    check("save() reports success", a.save());

    Settings b;
    b.load();
    check("per-engine voices round-trip", b.voices == a.voices);
    check("current voice follows the engine", b.currentVoice() == QLatin1String("ko:if_sara"), b.currentVoice());
    check("numbers round-trip", qFuzzyCompare(b.speed, 1.37) && qFuzzyCompare(b.remoteDb, -4.5));
    check("flags round-trip", b.previewOnly);
    check("history round-trip (unicode)", b.history == a.history);
    check("binary geometry round-trip", b.windowGeometry == a.windowGeometry);
    check("dictionary round-trip", b.dictionary == a.dictionary);
    check("effects state round-trip", b.sandboxJson == a.sandboxJson);
    check("voice changer settings round-trip", b.vcEnabled && b.vcMonitor && b.vcVoice == a.vcVoice &&
                                                   b.vcPreset == QLatin1String("120ms"));
    check("read channel chat round-trip", b.readChannelChat);
    check("update settings round-trip", !b.updateAutoCheck && b.updateNextCheck == nextCheck);
    {
        QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("TTS-selftest"));
        check("v1.1-readable voice/id written", s.value(QStringLiteral("voice/id")).toString() == QLatin1String("ko:if_sara"));
    }

    b.engine = QStringLiteral("qwen");
    b.save();
    Settings c;
    c.load();
    check("switching engine restores that engine's voice", c.currentVoice() == QLatin1String("clone:377c96f36c56"),
          c.currentVoice());

    QElapsedTimer t;
    t.start();
    constexpr int kSaves = 200;
    for (int i = 0; i < kSaves; ++i) {
        c.speed = 0.5 + (i % 100) / 100.0;
        c.save();
    }
    const double avg = t.nsecsElapsed() / 1e6 / kSaves;
    check("one save() is cheap enough to run on every change", avg < 15.0, QStringLiteral("%1 ms average").arg(avg, 0, 'f', 2));

    QSettings(QStringLiteral("GameBaiters"), QStringLiteral("TTS-selftest")).clear();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
