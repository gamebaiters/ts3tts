// ---------------------------------------------------------------------------
// Auto-updater self-test (no network, no TeamSpeak):
//   versions   "X.Y.Z" -> build number, malformed strings rejected
//   feed       version.xml parsing and every rejection rule (wrong product,
//              inconsistent build, plain http, bad checksum, malformed XML)
//   notes      only the release-notes sections newer than the installed version
//   download   zip magic + SHA-256 of a known content
//   helper     the generated batch script EXECUTED as the plugin runs it:
//              waits for a fake client process to exit, retries the delete while
//              the DLL is locked, removes the packaged backend code, starts the
//              package; second run: a client that never exits is killed.
//
//   gbtts_update_selftest [--no-exec]
//   gbtts_update_selftest --validate-feed <version.xml> [--expect-version X.Y.Z]   (CI)
// ---------------------------------------------------------------------------
#include "core/UpdateFeed.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace gbtts;

namespace {

int g_pass = 0, g_fail = 0;

void check(const char *name, bool ok, const QString &detail = QString())
{
    std::printf("  %s %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.toUtf8().constData());
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

QByteArray feed(const QString &product, const QString &version, int build, const QString &url,
                const QString &sha = QString(), const QString &notesTag = QStringLiteral("featureUrl"))
{
    QString x = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<versionDescription>\n"
                               "  <product descVersion=\"1\" name=\"%1\">\n"
                               "    <latestVersion>%2</latestVersion>\n"
                               "    <latestVersionString>%3</latestVersionString>\n"
                               "    <latestDownload><url>%4</url></latestDownload>\n"
                               "    <%5>https://example.org/notes.txt</%5>\n")
                    .arg(product).arg(build).arg(version, url, notesTag);
    if (!sha.isEmpty()) x += QStringLiteral("    <sha256>%1</sha256>\n").arg(sha);
    x += QStringLiteral("  </product>\n</versionDescription>\n");
    return x.toUtf8();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QThread::msleep(50);
    }
    return cond();
}

#ifdef _WIN32
// Runs the helper exactly like UpdaterWindow does, against a fake client.
//  graceful: the client exits by itself after ~3 s, nothing locks the DLL -> only the
//            wait can keep the installer from starting early;
//  forced:   the client never exits (killed after the grace time) and its DLL stays
//            locked 2.5 s longer -> the delete must be retried.
void helperRun(const QString &base, bool clientExitsByItself)
{
    const QString tag = clientExitsByItself ? QStringLiteral("graceful") : QStringLiteral("forced");
    // Space + non-ASCII in every path: user profiles like "C:\Users\Nicolò Rossi".
    const QString root = QDir(base).filePath(QStringLiteral("run %1 è").arg(tag));
    QDir(root).removeRecursively();
    const QString plugins = root + QStringLiteral("/plugins");
    const QString dll = plugins + QStringLiteral("/gb_tts_win64.dll");
    const QString code = plugins + QStringLiteral("/gb_tts/backend/gbtts");
    const QString marker = root + QStringLiteral("/installer_started.txt");
    // The "package": a script that proves `start ""` ran it, and closes its window.
    const QString package = root + QStringLiteral("/fake package.cmd");
    writeFile(dll, QByteArray(4096, 'd'));
    writeFile(code + QStringLiteral("/server.py"), "print('old')\n");
    writeFile(package, QStringLiteral("@echo off\r\nchcp 65001 >nul\r\necho started> \"%1\"\r\nexit\r\n")
                           .arg(QDir::toNativeSeparators(marker)).toUtf8());

    // Fake client: a renamed ping.exe (a real console process with a unique image name,
    // deliberately longer than the 25 characters tasklist's table format shows).
    const QString image = QStringLiteral("gbtts_fake_client_%1.exe").arg(tag);
    const QString exe = root + QLatin1Char('/') + image;
    const QString sysdir = QString::fromLocal8Bit(qgetenv("SystemRoot")) + QStringLiteral("/System32/PING.EXE");
    QFile::copy(sysdir, exe);
    QProcess client;
    client.start(exe, {QStringLiteral("-n"), clientExitsByItself ? QStringLiteral("4") : QStringLiteral("120"),
                       QStringLiteral("127.0.0.1")});
    const bool clientUp = client.waitForStarted(5000);

    // Lock the DLL the way a just-exited client can: open without FILE_SHARE_DELETE.
    HANDLE lock = clientExitsByItself
                      ? nullptr
                      : CreateFileW(QDir::toNativeSeparators(dll).toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    update::HelperSpec spec;
    spec.pluginsDir = plugins;
    spec.packagePath = package;
    spec.processImage = image;
    spec.gracefulWaitSeconds = clientExitsByItself ? 20 : 2;
    const QString helper = root + QStringLiteral("/gbtts_update_helper.bat");
    writeFile(helper, update::helperScript(spec));
    QElapsedTimer t;
    t.start();
    const bool launched = QProcess::startDetached(QStringLiteral("cmd.exe"),
                                                  {QStringLiteral("/c"), QDir::toNativeSeparators(helper)});
    const QByteArray label = tag.toUtf8();
    check(("helper[" + label + "]: fake client and helper started").constData(),
          clientUp && launched && lock != INVALID_HANDLE_VALUE);

    if (clientExitsByItself) {
        QThread::msleep(1500);
        check("helper[graceful]: while the client runs, nothing is touched",
              client.state() == QProcess::Running && QFileInfo::exists(dll) && !QFileInfo::exists(marker));
        const bool clientGone = client.waitForFinished(15000);
        const qint64 clientGoneMs = t.elapsed();
        const bool started = waitFor([&] { return QFileInfo::exists(marker); }, 20000);
        check("helper[graceful]: installer starts right after the client exits",
              clientGone && started && !QFileInfo::exists(dll) && t.elapsed() - clientGoneMs < 6000,
              QStringLiteral("client gone at %1 ms, installer at %2 ms").arg(clientGoneMs).arg(t.elapsed()));
    } else {
        const bool clientGone = client.waitForFinished(20000);
        const qint64 clientGoneMs = t.elapsed();
        check("helper[forced]: client that never exits is killed after the grace time",
              clientGone && clientGoneMs < 15000, QStringLiteral("gone after %1 ms").arg(clientGoneMs));
        QThread::msleep(2500);   // the helper is now retrying the delete of the locked DLL
        check("helper[forced]: locked DLL survives, installer waits for the delete",
              QFileInfo::exists(dll) && !QFileInfo::exists(marker));
        CloseHandle(lock);
        const bool started = waitFor([&] { return QFileInfo::exists(marker); }, 20000);
        check("helper[forced]: DLL deleted once unlocked, then package started", started && !QFileInfo::exists(dll),
              QStringLiteral("%1 ms").arg(t.elapsed()));
    }
    check(("helper[" + label + "]: packaged backend code removed").constData(), !QFileInfo::exists(code));
    if (client.state() != QProcess::NotRunning) client.kill();
}
#endif

int validateFeed(const QString &path, const QString &expectVersion)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("cannot read %s\n", path.toUtf8().constData());
        return 2;
    }
    const update::Info info = update::parseFeed(f.readAll());
    std::printf("feed %s: version %s build %d\n  url %s\n  notes %s\n  sha256 %s\n", path.toUtf8().constData(),
                info.version.toUtf8().constData(), info.build, info.url.toUtf8().constData(),
                info.notesUrl.toUtf8().constData(), info.sha256.toUtf8().constData());
    check("feed accepted by the plugin's parser", info.valid(), info.error);
    check("feed carries a SHA-256", !info.sha256.isEmpty());
    check("feed points to a GitHub release asset over https", info.url.startsWith(QLatin1String("https://github.com/")));
    if (!expectVersion.isEmpty()) check("feed version is the released version", info.version == expectVersion, info.version);
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool exec = true;
    QString validate, expect;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-exec")) exec = false;
        else if (!std::strcmp(argv[i], "--validate-feed") && i + 1 < argc) validate = QString::fromLocal8Bit(argv[++i]);
        else if (!std::strcmp(argv[i], "--expect-version") && i + 1 < argc) expect = QString::fromLocal8Bit(argv[++i]);
    }
    if (!validate.isEmpty()) return validateFeed(validate, expect);

    std::printf("GameBaiters TTS - auto-update self-test (plugin %s)\n", GBTTS_VERSION);

    // ---- versions -----------------------------------------------------------------
    check("version 1.5.0 -> 10500", update::versionNumber(QStringLiteral("1.5.0")) == 10500);
    check("version 2.3.6 -> 20306 (Soundboard numbering)", update::versionNumber(QStringLiteral(" 2.3.6 ")) == 20306);
    check("compiled version is a valid release number", update::versionNumber(QStringLiteral(GBTTS_VERSION)) > 0);
    check("malformed versions rejected",
          update::versionNumber(QStringLiteral("1.5")) < 0 && update::versionNumber(QStringLiteral("1.5.0-rc1")) < 0 &&
              update::versionNumber(QStringLiteral("100.0.0")) < 0 && update::versionNumber(QString()) < 0);
    check("ordering across minor/patch", update::versionNumber(QStringLiteral("1.10.0")) > update::versionNumber(QStringLiteral("1.9.99")));

    // ---- feed ---------------------------------------------------------------------
    const QString sha(64, QLatin1Char('a'));
    const QString url = QStringLiteral("https://github.com/gamebaiters/ts3tts/releases/download/v1.6.0/gb_tts_1.6.0_win64.ts3_plugin");
    update::Info ok = update::parseFeed(feed(QStringLiteral("gb_tts"), QStringLiteral("1.6.0"), 10600, url, sha.toUpper()));
    check("valid feed parsed", ok.valid() && ok.build == 10600 && ok.version == QLatin1String("1.6.0") && ok.url == url &&
                                   ok.notesUrl == QLatin1String("https://example.org/notes.txt"), ok.error);
    check("checksum normalised to lowercase", ok.sha256 == sha);
    check("Soundboard's featuresUrl spelling accepted too",
          update::parseFeed(feed(QStringLiteral("gb_tts"), QStringLiteral("1.6.0"), 10600, url, QString(), QStringLiteral("featuresUrl")))
              .notesUrl.size() > 0);
    check("feed of another product rejected (Soundboard)",
          !update::parseFeed(feed(QStringLiteral("rp_soundboard"), QStringLiteral("2.3.6"), 20306, url)).valid());
    const update::Info loop = update::parseFeed(feed(QStringLiteral("gb_tts"), QStringLiteral("1.6.0"), 10601, url));
    check("build number inconsistent with the version string rejected (update loop)", !loop.valid(), loop.error);
    check("plain http download rejected",
          !update::parseFeed(feed(QStringLiteral("gb_tts"), QStringLiteral("1.6.0"), 10600, QStringLiteral("http://example.org/x.ts3_plugin"))).valid());
    check("malformed checksum rejected",
          !update::parseFeed(feed(QStringLiteral("gb_tts"), QStringLiteral("1.6.0"), 10600, url, QStringLiteral("abc"))).valid());
    check("malformed XML rejected", !update::parseFeed("<versionDescription><product").valid());
    check("empty document rejected", !update::parseFeed(QByteArray()).valid());

    // ---- release notes --------------------------------------------------------------
    const QString notes = QStringLiteral(
        "GameBaiters TTS v1.7.0\r\n======================\r\n\r\n* NEW - seventeen\r\n\r\n"
        "GameBaiters TTS v1.6.0\r\n======================\r\n\r\n* FIX - sixteen\r\n\r\n"
        "GameBaiters TTS v1.5.0\r\n======================\r\n\r\n* NEW - fifteen\r\n");
    const QString since15 = update::notesSince(notes, QStringLiteral("1.5.0"));
    check("notes: every newer version, not the installed one",
          since15.contains(QLatin1String("seventeen")) && since15.contains(QLatin1String("sixteen")) &&
              !since15.contains(QLatin1String("fifteen")));
    const QString since16 = update::notesSince(notes, QStringLiteral("1.6.0"));
    check("notes: newest first, stops at the installed version",
          since16.startsWith(QLatin1String("GameBaiters TTS v1.7.0")) && !since16.contains(QLatin1String("sixteen")));
    check("notes: text without version headers shown whole",
          update::notesSince(QStringLiteral("just text"), QStringLiteral("1.5.0")) == QLatin1String("just text"));
    check("notes: cut to the maximum length", update::notesSince(QString(20000, QLatin1Char('x')), QStringLiteral("1.5.0"), 100).size() <= 102);

    // ---- download verification -------------------------------------------------------
    const QString base = QDir::temp().filePath(QStringLiteral("gbtts_update_selftest"));
    QDir(base).removeRecursively();
    const QString abc = base + QStringLiteral("/abc.bin");
    writeFile(abc, "abc");
    check("SHA-256 of a known content", update::fileSha256(abc) ==
                                            QLatin1String("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    check("missing file has no checksum", update::fileSha256(base + QStringLiteral("/missing")).isEmpty());
    const QString zip = base + QStringLiteral("/pkg.ts3_plugin");
    writeFile(zip, QByteArray("PK\x03\x04", 4) + QByteArray(100, 'z'));
    check("zip magic recognised, other content refused", update::looksLikeZip(zip) && !update::looksLikeZip(abc));

    // ---- helper script -----------------------------------------------------------------
    update::HelperSpec spec;
    spec.pluginsDir = QStringLiteral("C:/Users/Nicolò Rossi/AppData/Roaming/TS3Client/plugins");
    spec.packagePath = QStringLiteral("C:/Users/Nicolò Rossi/AppData/Local/Temp/GameBaitersTTS-update/gb_tts_1.6.0_win64.ts3_plugin");
    const QByteArray script = update::helperScript(spec);
    const QString s = QString::fromUtf8(script);
    check("helper: UTF-8 code page and non-ASCII paths intact",
          s.contains(QLatin1String("chcp 65001")) &&
              s.contains(QStringLiteral("C:\\Users\\Nicolò Rossi\\AppData\\Roaming\\TS3Client\\plugins\\gb_tts_win64.dll")));
    check("helper: CRLF line endings only", script.count("\n") == script.count("\r\n") && script.count("\r\n") > 40);
    check("helper: starts the downloaded package",
          s.contains(QStringLiteral("start \"\" \"C:\\Users\\Nicolò Rossi\\AppData\\Local\\Temp\\GameBaitersTTS-update\\gb_tts_1.6.0_win64.ts3_plugin\"")));
    check("helper: graceful wait before WM_CLOSE before /F",
          s.indexOf(QLatin1String(":waitts")) < s.indexOf(QLatin1String("taskkill.exe /IM")) &&
              s.indexOf(QLatin1String("taskkill.exe /IM")) < s.indexOf(QLatin1String("taskkill.exe /F")));
    check("helper: no `|| exit /b %errorlevel%` and no `timeout`",
          !s.contains(QLatin1String("%errorlevel%")) && !s.contains(QLatin1String("timeout ")));

#ifdef _WIN32
    if (exec) {
        helperRun(base, true);
        helperRun(base, false);
    } else {
        std::printf("  (helper execution skipped: --no-exec)\n");
    }
#endif
    QDir(base).removeRecursively();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
