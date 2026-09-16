#include "core/UpdateFeed.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

namespace gbtts {
namespace update {

QUrl feedUrl()
{
    const QString env = QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_UPDATE_FEED"));
    return QUrl(env.isEmpty() ? QString::fromLatin1(kDefaultFeed) : env);
}

int versionNumber(const QString &version)
{
    static const QRegularExpression re(QStringLiteral("^(\\d{1,2})\\.(\\d{1,2})\\.(\\d{1,2})$"));
    const QRegularExpressionMatch m = re.match(version.trimmed());
    if (!m.hasMatch()) return -1;
    return m.captured(1).toInt() * 10000 + m.captured(2).toInt() * 100 + m.captured(3).toInt();
}

Info parseFeed(const QByteArray &xml)
{
    Info info;
    QXmlStreamReader r(xml);
    bool inProduct = false, sawProduct = false, inDownload = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const QStringRef n = r.name();
            if (n == QLatin1String("product")) {
                if (r.attributes().value(QStringLiteral("name")) == QLatin1String(kProduct) &&
                    r.attributes().value(QStringLiteral("descVersion")) == QLatin1String("1")) {
                    inProduct = sawProduct = true;
                    info.product = QString::fromLatin1(kProduct);
                }
            } else if (inProduct && n == QLatin1String("latestVersion")) {
                bool ok = false;
                info.build = r.readElementText().trimmed().toInt(&ok);
                if (!ok) info.build = 0;
            } else if (inProduct && n == QLatin1String("latestVersionString")) {
                info.version = r.readElementText().trimmed();
            } else if (inProduct && n == QLatin1String("latestDownload")) {
                inDownload = true;
            } else if (inProduct && inDownload && n == QLatin1String("url")) {
                info.url = r.readElementText().trimmed();
            } else if (inProduct && (n == QLatin1String("featureUrl") || n == QLatin1String("featuresUrl"))) {
                info.notesUrl = r.readElementText().trimmed();
            } else if (inProduct && n == QLatin1String("sha256")) {
                info.sha256 = r.readElementText().trimmed().toLower();
            }
        } else if (r.isEndElement()) {
            if (r.name() == QLatin1String("latestDownload")) inDownload = false;
            else if (r.name() == QLatin1String("product")) inProduct = false;
        }
    }
    if (r.hasError()) {
        info.error = QStringLiteral("malformed XML: %1").arg(r.errorString());
        return info;
    }
    if (!sawProduct) info.error = QStringLiteral("no <product name=\"%1\" descVersion=\"1\">").arg(QLatin1String(kProduct));
    else if (info.build <= 0) info.error = QStringLiteral("latestVersion missing");
    else if (versionNumber(info.version) != info.build)
        info.error = QStringLiteral("latestVersionString \"%1\" does not match latestVersion %2").arg(info.version).arg(info.build);
    else if (info.url.isEmpty()) info.error = QStringLiteral("download url missing");
    else if (!info.sha256.isEmpty() && !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(info.sha256).hasMatch())
        info.error = QStringLiteral("sha256 is not 64 hex digits");
    else {
        const QUrl u(info.url);
        if (!u.isValid() || (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("file")))
            info.error = QStringLiteral("download url must be https");
    }
    return info;
}

QString notesSince(const QString &notes, const QString &currentVersion, int maxChars)
{
    const int current = versionNumber(currentVersion);
    const QString header = QString::fromLatin1(kNotesHeader);
    QStringList out;
    bool anyHeader = false, keep = false;
    for (const QString &line : notes.split(QLatin1Char('\n'))) {
        QString l = line;
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
        if (l.startsWith(header)) {
            anyHeader = true;
            const int v = versionNumber(l.mid(header.size()).section(QLatin1Char(' '), 0, 0));
            if (v >= 0 && current >= 0 && v <= current) break;   // sections are newest first
            keep = true;
        }
        if (keep) out << l;
    }
    QString text = anyHeader ? out.join(QLatin1Char('\n')).trimmed() : notes.trimmed();
    if (maxChars > 0 && text.size() > maxChars) text = text.left(maxChars).trimmed() + QStringLiteral("\n…");
    return text;
}

QString fileSha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

bool looksLikeZip(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.read(4) == QByteArray("PK\x03\x04", 4);
}

QByteArray helperScript(const HelperSpec &spec)
{
    const QString plugins = QDir::toNativeSeparators(QDir::cleanPath(spec.pluginsDir));
    const QString dll = plugins + QLatin1Char('\\') + spec.dllName;
    const QString code = plugins + QLatin1Char('\\') + spec.assetsDir + QStringLiteral("\\backend\\gbtts");
    const QString pkg = QDir::toNativeSeparators(QDir::cleanPath(spec.packagePath));
    const QString image = spec.processImage;
    // Absolute tools: a GNU find.exe earlier on PATH (Git, MSYS) would break the process test.
    const QString sys = QStringLiteral("%SystemRoot%\\System32\\");
    // ping as the sleep: `timeout` refuses to run when stdin is redirected.
    auto sleep = [](int s) { return QStringLiteral("ping -n %1 127.0.0.1 >nul").arg(s + 1); };

    QStringList b;
    b << QStringLiteral("@echo off")
      << QStringLiteral("chcp 65001 >nul")
      << QStringLiteral("setlocal")
      << QStringLiteral("REM GameBaiters TTS auto-update helper (generated by the plugin)")
      << QStringLiteral("title GameBaiters TTS - Update")
      << QStringLiteral("echo ==============================================")
      << QStringLiteral("echo   GameBaiters TTS automatic update")
      << QStringLiteral("echo ==============================================")
      << QStringLiteral("echo.")
      // [1/4] graceful exit first: killing the client mid-shutdown corrupts its
      // settings write and shows TeamSpeak's "crashed last time" dialog.
      << QStringLiteral("echo [1/4] Waiting for TeamSpeak to close...")
      << QStringLiteral("set tries=0")
      << QStringLiteral(":waitts")
      // CSV: the table format truncates image names longer than 25 characters.
      << QStringLiteral("%1tasklist.exe /FI \"IMAGENAME eq %2\" /FO CSV /NH 2>nul | %1find.exe /I \"%2\" >nul").arg(sys, image)
      << QStringLiteral("if errorlevel 1 goto tsdead")
      << QStringLiteral("set /a tries+=1")
      << QStringLiteral("if %tries% GEQ %1 goto forcekill").arg(spec.gracefulWaitSeconds)
      << sleep(1)
      << QStringLiteral("goto waitts")
      << QStringLiteral(":forcekill")
      << QStringLiteral("echo        TeamSpeak did not close by itself - closing it now...")
      << QStringLiteral("%1taskkill.exe /IM %2 >nul 2>&1").arg(sys, image)
      << sleep(4)
      << QStringLiteral("%1taskkill.exe /F /IM %2 >nul 2>&1").arg(sys, image)
      << sleep(2)
      << QStringLiteral(":tsdead")
      << QStringLiteral("echo        TeamSpeak is closed.")
      << QStringLiteral("echo.")
      // [2/4] the DLL lock can outlive the process by a second or two.
      << QStringLiteral("echo [2/4] Removing the old GameBaiters TTS plugin...")
      << QStringLiteral("set delTries=0")
      << QStringLiteral(":delloop")
      << QStringLiteral("del /F /Q \"%1\" >nul 2>&1").arg(dll)
      << QStringLiteral("if not exist \"%1\" goto deldone").arg(dll)
      << QStringLiteral("set /a delTries+=1")
      << QStringLiteral("if %delTries% GEQ 15 goto delfail")
      << QStringLiteral("echo        Plugin file still locked - retrying...")
      << sleep(1)
      << QStringLiteral("goto delloop")
      << QStringLiteral(":delfail")
      << QStringLiteral("echo        WARNING: could not remove the old plugin file.")
      << QStringLiteral("echo        The installer will try to overwrite it.")
      << QStringLiteral(":deldone")
      // The package brings the complete backend code again: no stale module survives.
      << QStringLiteral("if exist \"%1\" rmdir /S /Q \"%1\" >nul 2>&1").arg(code)
      << QStringLiteral("echo        Done.")
      << QStringLiteral("echo.")
      << QStringLiteral("echo [3/4] Starting the plugin installer...")
      << QStringLiteral("start \"\" \"%1\"").arg(pkg)
      << QStringLiteral("echo.")
      << QStringLiteral("echo [4/4] Confirm the TeamSpeak plugin installer window, then start")
      << QStringLiteral("echo        TeamSpeak again. The voice engine is updated by the plugin")
      << QStringLiteral("echo        at its next start. This window closes in 10 seconds.")
      << sleep(10)
      << QStringLiteral("endlocal")
      << QStringLiteral("exit /b 0");
    return (b.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8();
}

} // namespace update
} // namespace gbtts
