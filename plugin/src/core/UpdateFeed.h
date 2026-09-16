#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

// The pure half of the auto-updater (Qt Core only, unit-tested by
// tests/update_selftest.cpp): feed parsing, version arithmetic, release-notes
// extraction, download verification and the Windows update helper script.
// The network + UI half is core/Updater and ui/UpdaterWindow.
//
// Feed = version.xml, same schema as the GameBaiters Soundboard, but published
// as an ASSET of the GitHub release by CI (.github/workflows/release.yml), so it
// appears atomically with the package it points to: there is no window in which
// clients see a new version whose download does not exist yet.
namespace gbtts {
namespace update {

constexpr const char *kProduct = "gb_tts";
constexpr const char *kNotesHeader = "GameBaiters TTS v";
constexpr const char *kDefaultFeed = "https://github.com/gamebaiters/ts3tts/releases/latest/download/version.xml";
constexpr const char *kReleasesPage = "https://github.com/gamebaiters/ts3tts/releases/latest";

// $GBTTS_UPDATE_FEED (tests: a file:// URL) or kDefaultFeed.
QUrl feedUrl();

// "1.5.0" -> 10500 (major*10000 + minor*100 + patch, each part 0..99), -1 if malformed.
// Same number the Soundboard feed calls latestVersion.
int versionNumber(const QString &version);

struct Info {
    QString product;
    QString version;     // latestVersionString
    int     build = 0;   // latestVersion
    QString url;         // latestDownload/url
    QString notesUrl;    // featureUrl (featuresUrl accepted too)
    QString sha256;      // lowercase hex, optional
    QString error;       // why the document was rejected

    // Complete, for our product, and self-consistent: build == versionNumber(version).
    // An inconsistent feed is rejected instead of offering the same update forever.
    bool valid() const { return error.isEmpty(); }
};

Info parseFeed(const QByteArray &xml);

// Sections of release-notes.txt newer than `currentVersion` (newest first), or the
// whole text when it has no "GameBaiters TTS vX.Y.Z" headers; cut at maxChars.
QString notesSince(const QString &notes, const QString &currentVersion, int maxChars = 8000);

// Lowercase hex SHA-256 of a file, empty if unreadable.
QString fileSha256(const QString &path);
// A .ts3_plugin is a zip archive: local file header magic "PK\3\4".
bool looksLikeZip(const QString &path);

struct HelperSpec {
    QString pluginsDir;                                          // <TS3 config>/plugins
    QString packagePath;                                         // downloaded .ts3_plugin
    QString processImage = QStringLiteral("ts3client_win64.exe");
    QString dllName      = QStringLiteral("gb_tts_win64.dll");
    QString assetsDir    = QStringLiteral("gb_tts");             // plugins/<assetsDir>/backend/gbtts is replaced
    int     gracefulWaitSeconds = 20;
};

// Windows batch script (UTF-8, CRLF, chcp 65001 so non-ASCII profile paths work):
// wait for a GRACEFUL client exit (then WM_CLOSE, then /F), delete the old DLL with
// retries while Windows releases the lock, drop the packaged backend code, start
// the package so the TeamSpeak installer runs.
QByteArray helperScript(const HelperSpec &spec);

} // namespace update
} // namespace gbtts
