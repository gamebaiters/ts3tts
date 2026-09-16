#include "net/BackendProcess.h"
#include "ts3log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" const char *getTs3ConfigPath();

namespace gbtts {

namespace {

bool isHome(const QString &home, BackendProcess::Layout *out)
{
    if (home.isEmpty()) return false;
    const QDir d(home);
    if (!d.exists()) return false;
    BackendProcess::Layout l;
    l.home = QDir::toNativeSeparators(QDir::cleanPath(d.absolutePath()));
    for (const QString &rel : {QStringLiteral("venv/Scripts/python.exe"), QStringLiteral(".venv/Scripts/python.exe")}) {
        if (QFileInfo::exists(d.filePath(rel))) {
            l.python = QDir::toNativeSeparators(d.filePath(rel));
            break;
        }
    }
    for (const QString &rel : {QStringLiteral("app"), QStringLiteral(".")}) {
        if (QFileInfo::exists(d.filePath(rel + QStringLiteral("/gbtts/server.py")))) {
            l.appDir = QDir::toNativeSeparators(QDir::cleanPath(d.filePath(rel)));
            break;
        }
    }
    l.logFile = QDir::toNativeSeparators(d.filePath(QStringLiteral("logs/backend.log")));
    if (!l.python.isEmpty()) {
        // A venv only works while the Python it was created from exists. After a Windows
        // reinstall (or uninstalling that Python) python.exe is still there but every start
        // fails with "No Python at ...": report it as broken, so the user is offered a repair.
        QFile cfg(QFileInfo(QFileInfo(l.python).absolutePath()).absolutePath() + QStringLiteral("/pyvenv.cfg"));
        if (cfg.open(QIODevice::ReadOnly | QIODevice::Text)) {
            for (const QByteArray &raw : cfg.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (!line.startsWith(QLatin1String("home"))) continue;
                const QString base = line.section(QLatin1Char('='), 1).trimmed();
                if (!base.isEmpty() && !QFileInfo::exists(QDir(base).filePath(QStringLiteral("python.exe")))) {
                    l.python.clear();
                    l.problem = BackendProcess::tr("the Python environment is broken: %1 no longer exists").arg(QDir::toNativeSeparators(base));
                }
                break;
            }
        }
    }
    if (l.problem.isEmpty()) {
        if (l.python.isEmpty()) l.problem = BackendProcess::tr("Python environment missing in %1").arg(l.home);
        else if (l.appDir.isEmpty()) l.problem = BackendProcess::tr("engine code (gbtts/server.py) missing in %1").arg(l.home);
    }
    l.valid = l.problem.isEmpty();
    *out = l;
    return l.valid;
}

#ifdef _WIN32
std::wstring quoteArg(const QString &arg)
{
    // CommandLineToArgvW rules: backslashes are literal unless they precede a
    // quote; paths from QDir::cleanPath never end with a backslash.
    std::wstring w = arg.toStdWString();
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;
    std::wstring out = L"\"";
    for (wchar_t c : w) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}
#endif

} // namespace

BackendProcess::BackendProcess(QObject *parent) : QObject(parent)
{
    m_poll.setInterval(500);
    connect(&m_poll, &QTimer::timeout, this, &BackendProcess::poll);
}

BackendProcess::~BackendProcess()
{
    kill();
}

QString BackendProcess::defaultHome()
{
    // Tests: keep a real installation on this PC out of a harness run.
    const QString forced = QProcessEnvironment::systemEnvironment().value(QStringLiteral("GBTTS_DEFAULT_HOME"));
    if (!forced.isEmpty()) return QDir::toNativeSeparators(forced);
    const QString local = QProcessEnvironment::systemEnvironment().value(QStringLiteral("LOCALAPPDATA"));
    if (!local.isEmpty()) return QDir::toNativeSeparators(local + QStringLiteral("/GameBaitersTTS"));
    return QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/GameBaitersTTS"));
}

BackendProcess::Layout BackendProcess::inspect(const QString &home)
{
    Layout l;
    if (!isHome(home, &l) && l.home.isEmpty()) {
        l.home = QDir::toNativeSeparators(home);
        l.problem = tr("voice engine not installed");
    }
    return l;
}

BackendProcess::Layout BackendProcess::resolve(const QString &configuredHome)
{
    Layout l;
    if (isHome(configuredHome, &l)) return l;
    Layout first = l;
    if (isHome(defaultHome(), &l)) return l;
    const QString pluginBackend = QString::fromUtf8(getTs3ConfigPath()) + QStringLiteral("plugins/gb_tts/backend");
    if (isHome(pluginBackend, &l)) return l;
    if (!configuredHome.isEmpty()) return first;
    l = Layout();
    l.home = defaultHome();
    l.problem = BackendProcess::tr("voice engine not installed");
    return l;
}

namespace {

bool copyTree(const QString &from, const QString &to, QString *error)
{
    QDir src(from);
    if (!QDir().mkpath(to)) {
        *error = QStringLiteral("cannot create %1").arg(to);
        return false;
    }
    const auto entries = src.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &e : entries) {
        const QString dst = QDir(to).filePath(e.fileName());
        if (e.isDir()) {
            if (e.fileName() == QLatin1String("__pycache__")) continue;
            if (!copyTree(e.absoluteFilePath(), dst, error)) return false;
        } else if (!QFile::copy(e.absoluteFilePath(), dst)) {
            *error = QStringLiteral("cannot copy %1").arg(QDir::toNativeSeparators(e.absoluteFilePath()));
            return false;
        }
    }
    return true;
}

// SHA-1 over every file's relative path and bytes (sorted, __pycache__ skipped). Empty for a
// missing tree.
QByteArray treeDigest(const QString &root)
{
    QDir base(root);
    if (!base.exists()) return QByteArray();
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!path.contains(QLatin1String("__pycache__"))) files << base.relativeFilePath(path);
    }
    files.sort();
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const QString &rel : files) {
        h.addData(rel.toUtf8());
        h.addData("\0", 1);
        QFile f(base.filePath(rel));
        if (f.open(QIODevice::ReadOnly)) h.addData(&f);
        h.addData("\n", 1);
    }
    return h.result();
}

// Requirement lines without comments and blanks: formatting-only edits do not count.
QStringList requirementLines(const QString &path)
{
    QFile f(path);
    QStringList out;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    for (QString line : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
        const int hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0) line.truncate(hash);
        line = line.trimmed();
        if (!line.isEmpty()) out << line.toLower();
    }
    return out;
}

} // namespace

QString BackendProcess::readPythonVersion(const QString &initPy)
{
    QFile f(initPy);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    static const QRegularExpression re(QStringLiteral("^__version__\\s*=\\s*[\"']([^\"']+)[\"']"),
                                       QRegularExpression::MultilineOption);
    return re.match(QString::fromUtf8(f.readAll())).captured(1);
}

BackendProcess::CodeSync BackendProcess::syncPackagedCode(const Layout &layout, const QString &packagedDir)
{
    CodeSync r;
    if (!layout.valid) return r;
    const QString home = QDir::cleanPath(layout.home);
    const QString app = QDir::cleanPath(home + QStringLiteral("/app"));
    if (QDir::cleanPath(layout.appDir).compare(app, Qt::CaseInsensitive) != 0) return r;   // developer layout
    const QString pkg = QDir::cleanPath(packagedDir.isEmpty()
                                            ? QString::fromUtf8(getTs3ConfigPath()) + QStringLiteral("plugins/gb_tts/backend")
                                            : packagedDir);
    if (pkg.compare(home, Qt::CaseInsensitive) == 0 || !QFileInfo::exists(pkg + QStringLiteral("/gbtts/server.py")))
        return r;

    // pip requirements: remembered in <home>\app by the installer. Missing (older
    // installer) = assume the installed libraries match and start remembering.
    const QString pkgReq = pkg + QStringLiteral("/requirements.txt");
    const QString appReq = app + QStringLiteral("/requirements.txt");
    if (QFileInfo::exists(pkgReq)) {
        if (!QFileInfo::exists(appReq)) QFile::copy(pkgReq, appReq);
        else r.requirementsChanged = requirementLines(pkgReq) != requirementLines(appReq);
    }

    r.packaged = readPythonVersion(pkg + QStringLiteral("/gbtts/__init__.py"));
    r.installed = readPythonVersion(app + QStringLiteral("/gbtts/__init__.py"));
    // Content, not the version number: a package with the same __version__ but different
    // files (a hotfix, new stock voices) must still be installed. ~60 files / 5 MB: a few ms.
    if (treeDigest(pkg + QStringLiteral("/gbtts")) == treeDigest(app + QStringLiteral("/gbtts"))) {
        r.result = CodeSync::UpToDate;
        return r;
    }

    const QString live = app + QStringLiteral("/gbtts");
    const QString fresh = app + QStringLiteral("/gbtts.new");
    const QString old = app + QStringLiteral("/gbtts.old");
    QDir(fresh).removeRecursively();
    QDir(old).removeRecursively();
    r.result = CodeSync::Failed;
    if (!copyTree(pkg + QStringLiteral("/gbtts"), fresh, &r.detail)) {
        QDir(fresh).removeRecursively();
        return r;
    }
    if (QFileInfo::exists(live) && !QDir().rename(live, old)) {
        QDir(fresh).removeRecursively();
        r.detail = QStringLiteral("%1 is in use").arg(QDir::toNativeSeparators(live));
        return r;
    }
    if (!QDir().rename(fresh, live)) {
        QDir().rename(old, live);
        QDir(fresh).removeRecursively();
        r.detail = QStringLiteral("cannot replace %1").arg(QDir::toNativeSeparators(live));
        return r;
    }
    QDir(old).removeRecursively();
    const QString tool = pkg + QStringLiteral("/tools/download_models.py");
    if (QFileInfo::exists(tool)) {
        QDir().mkpath(app + QStringLiteral("/tools"));
        const QString dst = app + QStringLiteral("/tools/download_models.py");
        QFile::remove(dst);
        QFile::copy(tool, dst);
    }
    r.result = CodeSync::Updated;
    return r;
}

bool BackendProcess::running() const
{
#ifdef _WIN32
    return m_proc && WaitForSingleObject(static_cast<HANDLE>(m_proc), 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

#ifdef _WIN32
bool BackendProcess::start(const Layout &layout, quint16 port, const QString &token, QString *error)
{
    kill();
    if (!layout.valid) {
        if (error) *error = layout.problem;
        return false;
    }

    QDir().mkpath(QFileInfo(layout.logFile).absolutePath());
    const QString prev = layout.logFile.left(layout.logFile.size() - 4) + QStringLiteral(".prev.log");
    QFile::remove(prev);
    QFile::rename(layout.logFile, prev);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileW(layout.logFile.toStdWString().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        if (error) *error = QStringLiteral("cannot create %1").arg(layout.logFile);
        return false;
    }

    // Environment: inherit, minus anything that would hijack the venv.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *k : {"PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "VIRTUAL_ENV", "CONDA_PREFIX"})
        env.remove(QString::fromLatin1(k));
    env.insert(QStringLiteral("PYTHONPATH"), layout.appDir);
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    QStringList keys = env.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    std::wstring envBlock;
    for (const QString &k : keys) {
        envBlock += (k + QLatin1Char('=') + env.value(k)).toStdWString();
        envBlock.push_back(L'\0');
    }
    envBlock.push_back(L'\0');

    std::wstring cmd = quoteArg(layout.python) + L" -X utf8 -u -m gbtts.server --port " +
                       std::to_wstring(port) + L" --token " + token.toStdWString() + L" --home " +
                       quoteArg(layout.home) + L" --parent-pid " + std::to_wstring(GetCurrentProcessId());

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    HANDLE inherit[1] = {log};
    const bool attrOk = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) &&
                        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                  sizeof(inherit), nullptr, nullptr);

    PROCESS_INFORMATION pi{};
    std::wstring cmdMutable = cmd;
    std::wstring cwd = layout.appDir.toStdWString();
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                        ABOVE_NORMAL_PRIORITY_CLASS | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL ok = CreateProcessW(nullptr, &cmdMutable[0], nullptr, nullptr, TRUE, flags, &envBlock[0], cwd.c_str(),
                                   attrOk ? &si.StartupInfo : &si.StartupInfo, &pi);
    const DWORD createErr = GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(log);

    if (!ok) {
        if (job) CloseHandle(job);
        if (error) *error = QStringLiteral("CreateProcess failed (%1)").arg(createErr);
        return false;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        logWarning("GBTTS: AssignProcessToJobObject failed (%lu); backend relies on its parent watchdog",
                   GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    m_job = job;
    m_proc = pi.hProcess;
    m_poll.start();
    logInfo("GBTTS: backend started (pid %lu) from %s", pi.dwProcessId, layout.home.toUtf8().constData());
    return true;
}

void BackendProcess::kill()
{
    m_poll.stop();
    if (m_job) {
        TerminateJobObject(static_cast<HANDLE>(m_job), 0);
        CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    } else if (m_proc) {
        TerminateProcess(static_cast<HANDLE>(m_proc), 0);
    }
    if (m_proc) {
        CloseHandle(static_cast<HANDLE>(m_proc));
        m_proc = nullptr;
    }
}

void BackendProcess::poll()
{
    if (!m_proc) {
        m_poll.stop();
        return;
    }
    if (WaitForSingleObject(static_cast<HANDLE>(m_proc), 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(m_proc), &code);
    m_poll.stop();
    CloseHandle(static_cast<HANDLE>(m_proc));
    m_proc = nullptr;
    if (m_job) {
        CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    }
    logWarning("GBTTS: backend exited with code %lu", code);
    emit exited(static_cast<int>(code));
}
#else
bool BackendProcess::start(const Layout &, quint16, const QString &, QString *error)
{
    if (error) *error = QStringLiteral("only Windows is supported");
    return false;
}
void BackendProcess::kill() {}
void BackendProcess::poll() {}
#endif

} // namespace gbtts
