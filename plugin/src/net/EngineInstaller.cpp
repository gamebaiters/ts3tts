#include "net/EngineInstaller.h"

#include "ts3log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStorageInfo>

#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

extern "C" const char *getTs3ConfigPath();

namespace gbtts {

namespace {

constexpr int kPollMs = 400;
constexpr int kLogTailChars = 60000;

#ifdef _WIN32
std::wstring quoteArg(const QString &arg)
{
    // CommandLineToArgvW rules; our paths never end with a backslash before the quote.
    std::wstring w = arg.toStdWString();
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::wstring out = L"\"";
    for (wchar_t c : w) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

// Hidden, detached, not in any job of ours: survives the window, the plugin and TeamSpeak.
HANDLE launchHidden(const std::wstring &commandLine, const QString &workDir, bool cleanPowerShellEnv, DWORD *pid, DWORD *error)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (cleanPowerShellEnv) {
        // A PSModulePath inherited from PowerShell 7 makes Windows PowerShell 5.1 load
        // the wrong module versions (Get-FileHash, ConvertTo-Json fail).
        env.remove(QStringLiteral("PSModulePath"));
        for (const char *k : {"PYTHONHOME", "PYTHONPATH", "VIRTUAL_ENV", "CONDA_PREFIX"}) env.remove(QString::fromLatin1(k));
    }
    QStringList keys = env.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    std::wstring block;
    for (const QString &k : keys) {
        block += (k + QLatin1Char('=') + env.value(k)).toStdWString();
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    const std::wstring cwd = QDir::toNativeSeparators(workDir).toStdWString();
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, &block[0],
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)   // host in a job that forbids breakaway
        ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, flags, &block[0], cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok) {
        if (error) *error = GetLastError();
        return nullptr;
    }
    CloseHandle(pi.hThread);
    if (pid) *pid = pi.dwProcessId;
    return pi.hProcess;
}

QString imageName(HANDLE process)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    if (!QueryFullProcessImageNameW(process, 0, buf, &len)) return QString();
    return QFileInfo(QString::fromWCharArray(buf, int(len))).fileName().toLower();
}
#endif

} // namespace

EngineInstaller::EngineInstaller(QObject *parent) : QObject(parent)
{
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &EngineInstaller::poll);
}

EngineInstaller::~EngineInstaller()
{
    // Never waits and never kills: the installation goes on without us.
    m_poll.stop();
#ifdef _WIN32
    if (m_process) CloseHandle(static_cast<HANDLE>(m_process));
#endif
    m_process = nullptr;
}

QString EngineInstaller::statusDir(const QString &home)
{
    return QDir(home).filePath(QStringLiteral("install"));
}

QString EngineInstaller::logFile() const
{
    return QDir::toNativeSeparators(QDir(statusDir(m_home)).filePath(QStringLiteral("install.log")));
}

QString EngineInstaller::scriptPath(const QString &configuredHome)
{
    const QString env = QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_INSTALLER_SCRIPT"));
    QStringList candidates;
    if (!env.isEmpty()) candidates << env;
    candidates << QString::fromUtf8(getTs3ConfigPath()) + QStringLiteral("plugins/gb_tts/backend/install_backend.ps1");
    if (!configuredHome.isEmpty())
        candidates << QDir(configuredHome).filePath(QStringLiteral("../installer/install_backend.ps1"));
    for (const QString &c : candidates)
        if (QFileInfo::exists(c)) return QDir::toNativeSeparators(QFileInfo(c).absoluteFilePath());
    return QString();
}

EngineInstaller::Gpu EngineInstaller::detectNvidiaGpu()
{
    Gpu gpu;
#ifdef _WIN32
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) return gpu;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            qint64(desc.DedicatedVideoMemory) > gpu.memoryBytes) {
            gpu.name = QString::fromWCharArray(desc.Description).trimmed();
            gpu.memoryBytes = qint64(desc.DedicatedVideoMemory);
        }
        adapter->Release();
    }
    factory->Release();
#endif
    return gpu;
}

qint64 EngineInstaller::freeBytes(const QString &path)
{
    QDir d(QDir::cleanPath(path));
    while (!d.exists() && !d.isRoot() && d.cdUp()) {
    }
    QStorageInfo info(d.absolutePath());
    return info.isValid() ? info.bytesAvailable() : -1;
}

bool EngineInstaller::start(const QString &home, const QString &mode, const QStringList &models, QString *error)
{
    if (m_process) {
        if (error) *error = tr("an installation is already running");
        return false;
    }
    if (attach(home)) return true;   // still running from an earlier session: follow it

    const QString script = scriptPath();
    if (script.isEmpty()) {
        if (error) *error = tr("the installer is missing from the plugin package: reinstall the plugin");
        return false;
    }
#ifdef _WIN32
    const QString dir = statusDir(home);
    if (!QDir().mkpath(dir)) {
        if (error) *error = tr("cannot create %1").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    QFile::remove(QDir(dir).filePath(QStringLiteral("status.json")));   // never show a stale result
    const QString ps = QString::fromLocal8Bit(qgetenv("SystemRoot")) + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    std::wstring cmd = quoteArg(ps) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File " +
                       quoteArg(script) + L" -InstallDir " + quoteArg(QDir::toNativeSeparators(QDir::cleanPath(home))) +
                       L" -Mode " + quoteArg(mode.isEmpty() ? QStringLiteral("auto") : mode);
    if (!models.isEmpty()) cmd += L" -Models " + quoteArg(models.join(QLatin1Char(',')));
    const QString extra = QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_INSTALLER_ARGS"));
    if (!extra.isEmpty()) cmd += L" " + extra.toStdWString();   // tests only

    DWORD pid = 0, err = 0;
    HANDLE h = launchHidden(cmd, dir, true, &pid, &err);
    if (!h) {
        if (error) *error = tr("could not start PowerShell (error %1)").arg(err);
        return false;
    }
    m_process = h;
    m_home = home;
    m_cancelRequested = false;
    m_status = Status();
    m_status.state = QStringLiteral("running");
    m_status.mode = mode;
    m_logOffset = 0;
    m_logTail.clear();
    m_poll.start();
    logInfo("GBTTS: engine installer started (pid %lu, %s, mode %s)", pid, QDir::toNativeSeparators(home).toUtf8().constData(),
            mode.toUtf8().constData());
    emit progress();
    return true;
#else
    Q_UNUSED(models);
    if (error) *error = QStringLiteral("only Windows is supported");
    return false;
#endif
}

bool EngineInstaller::attach(const QString &home)
{
    if (m_process || home.isEmpty()) return m_process != nullptr;
    QFile f(QDir(statusDir(home)).filePath(QStringLiteral("status.json")));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("state")).toString() != QLatin1String("running")) return false;
#ifdef _WIN32
    const DWORD pid = DWORD(o.value(QStringLiteral("pid")).toDouble());
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h) return false;
    // A recycled pid would belong to anything else: it must still be PowerShell.
    if (WaitForSingleObject(h, 0) != WAIT_TIMEOUT || imageName(h) != QLatin1String("powershell.exe")) {
        CloseHandle(h);
        return false;
    }
    m_process = h;
    m_home = home;
    m_cancelRequested = false;
    m_logOffset = 0;
    m_logTail.clear();
    readStatus();
    readLog();
    m_poll.start();
    logInfo("GBTTS: following the engine installation already running in %s (pid %lu)",
            QDir::toNativeSeparators(home).toUtf8().constData(), pid);
    emit progress();
    return true;
#else
    return false;
#endif
}

void EngineInstaller::cancel()
{
    if (!m_process || m_cancelRequested) return;
    m_cancelRequested = true;
#ifdef _WIN32
    const DWORD pid = GetProcessId(static_cast<HANDLE>(m_process));
    const QString taskkill = QString::fromLocal8Bit(qgetenv("SystemRoot")) + QStringLiteral("\\System32\\taskkill.exe");
    // /T: uv, python and pip children go too; nothing waits for it here.
    const std::wstring cmd = quoteArg(taskkill) + L" /PID " + std::to_wstring(pid) + L" /T /F";
    DWORD err = 0;
    if (HANDLE h = launchHidden(cmd, QString(), false, nullptr, &err)) CloseHandle(h);
    else TerminateProcess(static_cast<HANDLE>(m_process), 1);
    logInfo("GBTTS: engine installation cancelled by the user (pid %lu)", pid);
#endif
    emit progress();
}

bool EngineInstaller::readStatus()
{
    QFile f(QDir(statusDir(m_home)).filePath(QStringLiteral("status.json")));
    if (!f.open(QIODevice::ReadOnly)) return false;   // being replaced right now: next poll
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();
    Status s;
    s.valid = true;
    s.state = o.value(QStringLiteral("state")).toString();
    s.mode = o.value(QStringLiteral("mode")).toString();
    s.stage = o.value(QStringLiteral("stage")).toString();
    for (const QJsonValue &v : o.value(QStringLiteral("stages")).toArray()) s.stages << v.toString();
    s.stageIndex = o.value(QStringLiteral("stageIndex")).toInt();
    s.overall = o.value(QStringLiteral("overall")).toDouble();
    s.stageFraction = o.value(QStringLiteral("stageFraction")).toDouble(-1);
    s.bytesDone = qint64(o.value(QStringLiteral("bytesDone")).toDouble());
    s.bytesTotal = qint64(o.value(QStringLiteral("bytesTotal")).toDouble());
    s.item = o.value(QStringLiteral("item")).toString();
    s.error = o.value(QStringLiteral("error")).toString();
    s.errorCode = o.value(QStringLiteral("errorCode")).toString();
    s.gpu = o.value(QStringLiteral("gpu")).toString();
    s.sizeBytes = qint64(o.value(QStringLiteral("sizeBytes")).toDouble());
    s.started = QDateTime::fromString(o.value(QStringLiteral("started")).toString(), Qt::ISODateWithMs);
    m_status = s;
    return true;
}

void EngineInstaller::readLog()
{
    QFile f(logFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    if (f.size() < m_logOffset) m_logOffset = 0;
    if (f.size() == m_logOffset) return;
    f.seek(m_logOffset);
    QByteArray chunk = f.read(1 << 20);
    // Only whole lines: the installer may be in the middle of writing one.
    const int nl = chunk.lastIndexOf('\n');
    if (nl < 0) return;
    chunk.truncate(nl + 1);
    m_logOffset += chunk.size();
    const QString text = QString::fromUtf8(chunk);
    m_logTail += text;
    if (m_logTail.size() > kLogTailChars) m_logTail = m_logTail.right(kLogTailChars / 2);
    emit logAppended(text);
}

void EngineInstaller::poll()
{
    if (!m_process) {
        m_poll.stop();
        return;
    }
    const bool changed = readStatus();
    readLog();
#ifdef _WIN32
    if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_OBJECT_0) {
        finish();
        return;
    }
#endif
    if (changed) emit progress();
}

void EngineInstaller::finish()
{
    m_poll.stop();
    DWORD code = 0;
#ifdef _WIN32
    GetExitCodeProcess(static_cast<HANDLE>(m_process), &code);
    CloseHandle(static_cast<HANDLE>(m_process));
#endif
    m_process = nullptr;
    readStatus();
    readLog();
    if (m_status.state != QLatin1String("done") && m_status.state != QLatin1String("error")) {
        // Killed (cancel, Task Manager, logoff) before it could write a result.
        m_status.state = QStringLiteral("error");
        if (m_cancelRequested) {
            m_status.errorCode = QStringLiteral("cancelled");
            m_status.error = tr("installation cancelled");
        } else {
            m_status.errorCode = QStringLiteral("interrupted");
            m_status.error = tr("the installer stopped unexpectedly (exit code %1)").arg(code);
        }
    }
    const bool ok = m_status.state == QLatin1String("done");
    logInfo("GBTTS: engine installer finished: %s%s%s", ok ? "done" : "failed", ok ? "" : " - ",
            ok ? "" : m_status.error.toUtf8().constData());
    emit progress();
    emit finished(ok);
}

} // namespace gbtts
