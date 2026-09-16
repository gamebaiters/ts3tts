// ---------------------------------------------------------------------------
// Lifecycle / leak / crash harness: loads the REAL gb_tts_win64.dll the way the
// TeamSpeak client does (setFunctionPointers, registerPluginID, initMenus,
// initHotkeys, init, audio callbacks from a 20 ms audio thread, menu events,
// /tts command, shutdown, FreeLibrary) inside a QApplication, and measures the
// host process after every cycle.
//
//   cycles:  gbtts_host_harness --dll <dll> --cycles 10 --run-ms 1500 [--unload]
//            [--backend-home <dir> --engine kokoro|qwen|supertonic]
//   crash:   gbtts_host_harness --crash terminate|segv --dll <dll> --backend-home <dir>
//            --engine kokoro [--wait-ms 8000] [--vram]
//   update:  gbtts_host_harness --update --dll <dll>
//            (file:// feed + GBTTS_UPDATE_NO_LAUNCH=1: offer, "Later", unload with the
//            offer / the progress window open, verified download, damaged download
//            refused, "up to date")
//   setup:   gbtts_host_harness --setup-fake --dll <dll> --installer plugin\tests\fake_install_backend.ps1
//            (installation window opens by itself, progress, two-click cancel kills the whole
//            process tree, plugin unloaded mid-install: the installer survives and a reloaded
//            plugin re-attaches, settings point to the new engine)
//            gbtts_host_harness --setup-real --dll <dll> --installer installer\install_backend.ps1
//            --mode gpu|cpu [--home <dir>]   (REAL installation through the window, then the
//            engine starts by itself and speaks)
//   code:    gbtts_host_harness --code-sync --dll <dll> --backend-home <dev backend>
//            (installer-style home with an OLDER backend in app\: replaced by the
//            packaged code before the engine starts, then up to date, requirements drift)
//
// Settings are isolated: GBTTS_SETTINGS_APP=TTS-harness (HKCU\Software\GameBaiters\TTS-harness).
// Exit code 0 = every check passed.
// ---------------------------------------------------------------------------
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "teamspeak/clientlib_publicdefinitions.h"
#include "ts3_functions.h"
#include "plugin_definitions.h"

#include <QAbstractButton>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QElapsedTimer>
#include <QPixmapCache>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <malloc.h>
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

namespace {

// ---- crash reporter: module+offset+symbol per frame, and a minidump ----------------
std::string whereIs(DWORD64 addr)
{
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "<no module: unloaded DLL?>";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod))
        GetModuleFileNameA(mod, path, MAX_PATH);
    const char *base = std::strrchr(path, '\\');
    base = base ? base + 1 : path;
    char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::string name;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) name = std::string(sym->Name) + "+0x" + [&] {
        char b[32];
        snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(disp));
        return std::string(b);
    }();
    char out[700];
    snprintf(out, sizeof(out), "%s+0x%llx  %s", base,
             static_cast<unsigned long long>(mod ? addr - reinterpret_cast<DWORD64>(mod) : addr), name.c_str());
    return out;
}

LONG WINAPI crashReporter(EXCEPTION_POINTERS *ep)
{
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    std::fprintf(stderr, "\n*** CRASH 0x%08lX at %s\n", er->ExceptionCode,
                 whereIs(reinterpret_cast<DWORD64>(er->ExceptionAddress)).c_str());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::fprintf(stderr, "    %s of address %p\n", er->ExceptionInformation[0] == 8 ? "execute"
                                                     : er->ExceptionInformation[0] ? "write" : "read",
                     reinterpret_cast<void *>(er->ExceptionInformation[1]));
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip;
    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp;
    f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp;
    f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        std::fprintf(stderr, "    #%02d %s\n", i, whereIs(f.AddrPC.Offset).c_str());
    }
    wchar_t dump[MAX_PATH];
    GetTempPathW(MAX_PATH, dump);
    wcscat_s(dump, L"gbtts_harness_crash.dmp");
    HANDLE file = CreateFileW(dump, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(proc, GetCurrentProcessId(), file, MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr, nullptr);
        CloseHandle(file);
        std::fprintf(stderr, "    minidump: %ls\n", dump);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

constexpr const char *kSettingsApp = "TTS-harness";

int g_pass = 0, g_fail = 0;

void check(const char *name, bool ok, const std::string &detail = std::string())
{
    std::printf("  %s %-58s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    (ok ? g_pass : g_fail)++;
}

std::string fmt(const char *f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

// ---- options --------------------------------------------------------------------
struct Options {
    std::wstring dll;
    int cycles = 5;
    int runMs = 1500;
    int waitMs = 8000;
    bool unload = false;
    bool vram = false;
    bool noUi = false;           // bisection: never open plugin windows
    bool noInit = false;         // bisection: load + function pointers + menus only
    bool noFinalUnload = false;  // bisection: keep the DLL loaded while QApplication dies
    bool trim = false;           // empty host caches + compact heaps before each sample
    bool child = false;
    bool update = false;
    bool codeSync = false;
    bool setupFake = false;
    bool setupReal = false;
    QString installer;
    QString setupMode = QStringLiteral("cpu");
    QString setupHome;
    QString engineVenv;
    QString engineModels;
    QString backendHome;
    QString engine = QStringLiteral("kokoro");
    std::string crash;          // "", "terminate", "segv"
    DWORD parent = 0;
};

Options parse(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--dll") o.dll = QString::fromLocal8Bit(next().c_str()).toStdWString();
        else if (a == "--cycles") o.cycles = std::atoi(next().c_str());
        else if (a == "--run-ms") o.runMs = std::atoi(next().c_str());
        else if (a == "--wait-ms") o.waitMs = std::atoi(next().c_str());
        else if (a == "--unload") o.unload = true;
        else if (a == "--vram") o.vram = true;
        else if (a == "--no-ui") o.noUi = true;
        else if (a == "--no-init") o.noInit = true;
        else if (a == "--no-final-unload") o.noFinalUnload = true;
        else if (a == "--trim") o.trim = true;
        else if (a == "--child") o.child = true;
        else if (a == "--update") o.update = true;
        else if (a == "--code-sync") o.codeSync = true;
        else if (a == "--setup-fake") o.setupFake = true;
        else if (a == "--setup-real") o.setupReal = true;
        else if (a == "--installer") o.installer = QFileInfo(QString::fromLocal8Bit(next().c_str())).absoluteFilePath();
        else if (a == "--mode") o.setupMode = QString::fromLatin1(next().c_str());
        else if (a == "--engine-venv") o.engineVenv = QDir::toNativeSeparators(QString::fromLocal8Bit(next().c_str()));
        else if (a == "--engine-models") o.engineModels = QDir::toNativeSeparators(QString::fromLocal8Bit(next().c_str()));
        else if (a == "--home") o.setupHome = QDir::toNativeSeparators(QDir(QString::fromLocal8Bit(next().c_str())).absolutePath());
        else if (a == "--backend-home") o.backendHome = QDir(QString::fromLocal8Bit(next().c_str())).absolutePath();
        else if (a == "--engine") o.engine = QString::fromLatin1(next().c_str());
        else if (a == "--crash") o.crash = next();
        else if (a == "--parent") o.parent = static_cast<DWORD>(std::strtoul(next().c_str(), nullptr, 10));
    }
    return o;
}

// ---- fake TeamSpeak ------------------------------------------------------------------
std::mutex               g_logMutex;
std::vector<std::string> g_log;
std::string              g_configDir;

unsigned int fakeLog(const char *msg, enum LogLevel severity, const char *, uint64)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_log.push_back(fmt("[%d] %s", static_cast<int>(severity), msg ? msg : ""));
    if (severity <= LogLevel_WARNING) std::printf("        log(%d): %s\n", static_cast<int>(severity), msg ? msg : "");
    return 0;
}
void fakeConfigPath(char *path, size_t maxLen) { strncpy_s(path, maxLen, g_configDir.c_str(), _TRUNCATE); }
void fakePluginPath(char *path, size_t maxLen, const char *) { strncpy_s(path, maxLen, (g_configDir + "plugins\\").c_str(), _TRUNCATE); }
uint64 fakeCurrentServer() { return 1; }
unsigned int fakeConnStatus(uint64, int *result) { if (result) *result = STATUS_DISCONNECTED; return 0; }
constexpr anyID kOwnClientId = 42;
unsigned int fakeClientID(uint64, anyID *result)
{
    if (result) *result = kOwnClientId;
    return 0;
}
unsigned int fakeFreeMemory(void *p) { free(p); return 0; }
unsigned int fakeGetPre(uint64, const char *, char **) { return 1; }
unsigned int fakeSetPre(uint64, const char *, const char *) { return 1; }
unsigned int fakeGetSelfInt(uint64, size_t, int *) { return 1; }
unsigned int fakeSetSelfInt(uint64, size_t, int) { return 1; }
unsigned int fakeFlush(uint64, const char *) { return 1; }
unsigned int fakeActivate(uint64) { return 1; }
unsigned int fakeChannelOf(uint64, anyID, uint64 *) { return 1; }
unsigned int fakeSendChannel(uint64, const char *, uint64, const char *) { return 1; }

TS3Functions makeFunctions()
{
    TS3Functions f;
    std::memset(&f, 0, sizeof(f));
    f.logMessage = fakeLog;
    f.getConfigPath = fakeConfigPath;
    f.getPluginPath = fakePluginPath;
    f.getCurrentServerConnectionHandlerID = fakeCurrentServer;
    f.getConnectionStatus = fakeConnStatus;
    f.getClientID = fakeClientID;
    f.freeMemory = fakeFreeMemory;
    f.getPreProcessorConfigValue = fakeGetPre;
    f.setPreProcessorConfigValue = fakeSetPre;
    f.getClientSelfVariableAsInt = fakeGetSelfInt;
    f.setClientSelfVariableAsInt = fakeSetSelfInt;
    f.flushClientSelfUpdates = fakeFlush;
    f.activateCaptureDevice = fakeActivate;
    f.getChannelOfClient = fakeChannelOf;
    f.requestSendChannelTextMsg = fakeSendChannel;
    return f;
}

bool logContains(const char *needle)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    for (const std::string &l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// ---- the plugin DLL ---------------------------------------------------------------------
struct Plugin {
    HMODULE lib = nullptr;
    void (*setFunctionPointers)(const struct TS3Functions) = nullptr;
    void (*registerPluginID)(const char *) = nullptr;
    int  (*init)() = nullptr;
    void (*shutdown)() = nullptr;
    void (*capture)(uint64, short *, int, int, int *) = nullptr;
    void (*playback)(uint64, short *, int, int, const unsigned int *, unsigned int *) = nullptr;
    void (*menu)(uint64, enum PluginMenuType, int, uint64) = nullptr;
    void (*hotkey)(const char *) = nullptr;
    int  (*command)(uint64, const char *) = nullptr;
    void (*initHotkeys)(struct PluginHotkey ***) = nullptr;
    void (*initMenus)(struct PluginMenuItem ***, char **) = nullptr;
    void (*freeMemory)(void *) = nullptr;
    int  (*textMessage)(uint64, anyID, anyID, anyID, const char *, const char *, const char *, int) = nullptr;

    bool load(const std::wstring &path)
    {
        lib = LoadLibraryW(path.c_str());
        if (!lib) return false;
        auto sym = [this](const char *n) { return GetProcAddress(lib, n); };
        setFunctionPointers = reinterpret_cast<decltype(setFunctionPointers)>(sym("ts3plugin_setFunctionPointers"));
        registerPluginID = reinterpret_cast<decltype(registerPluginID)>(sym("ts3plugin_registerPluginID"));
        init = reinterpret_cast<decltype(init)>(sym("ts3plugin_init"));
        shutdown = reinterpret_cast<decltype(shutdown)>(sym("ts3plugin_shutdown"));
        capture = reinterpret_cast<decltype(capture)>(sym("ts3plugin_onEditCapturedVoiceDataEvent"));
        playback = reinterpret_cast<decltype(playback)>(sym("ts3plugin_onEditMixedPlaybackVoiceDataEvent"));
        menu = reinterpret_cast<decltype(menu)>(sym("ts3plugin_onMenuItemEvent"));
        hotkey = reinterpret_cast<decltype(hotkey)>(sym("ts3plugin_onHotkeyEvent"));
        command = reinterpret_cast<decltype(command)>(sym("ts3plugin_processCommand"));
        initHotkeys = reinterpret_cast<decltype(initHotkeys)>(sym("ts3plugin_initHotkeys"));
        initMenus = reinterpret_cast<decltype(initMenus)>(sym("ts3plugin_initMenus"));
        freeMemory = reinterpret_cast<decltype(freeMemory)>(sym("ts3plugin_freeMemory"));
        textMessage = reinterpret_cast<decltype(textMessage)>(sym("ts3plugin_onTextMessageEvent"));
        return setFunctionPointers && registerPluginID && init && shutdown && capture && playback && menu && hotkey &&
               command && initHotkeys && initMenus && freeMemory && textMessage;
    }

    void unload()
    {
        if (lib) FreeLibrary(lib);
        lib = nullptr;
    }

    // What the client does right after loading: take menus and hotkeys, free them.
    std::string previewHotkey()
    {
        std::string keyword;
        PluginHotkey **hk = nullptr;
        initHotkeys(&hk);
        for (int i = 0; hk && hk[i]; ++i) {
            if (std::strstr(hk[i]->description, "preview")) keyword = hk[i]->keyword;
            freeMemory(hk[i]);
        }
        freeMemory(hk);
        PluginMenuItem **items = nullptr;
        char *icon = nullptr;
        initMenus(&items, &icon);
        for (int i = 0; items && items[i]; ++i) freeMemory(items[i]);
        freeMemory(items);
        freeMemory(icon);
        return keyword;
    }
};

// ---- fake audio device -------------------------------------------------------------------
struct AudioThread {
    std::atomic<bool> run{false};
    std::atomic<int64_t> calls{0};
    std::atomic<int64_t> audibleFrames{0};
    std::thread th;

    void start(Plugin &p)
    {
        run = true;
        auto *cap = p.capture;
        auto *pb = p.playback;
        th = std::thread([this, cap, pb] {
            short mono[960];
            short stereo[1920];
            constexpr unsigned int speakers[2] = {0x1 /*FRONT_LEFT*/, 0x2 /*FRONT_RIGHT*/};
            auto next = std::chrono::steady_clock::now();
            while (run.load()) {
                std::memset(mono, 0, sizeof(mono));
                std::memset(stereo, 0, sizeof(stereo));
                int edited = 0;
                cap(1, mono, 960, 1, &edited);
                unsigned int mask = 0;
                pb(1, stereo, 960, 2, speakers, &mask);
                int audible = 0;
                for (short s : stereo) audible += s != 0;
                audibleFrames += audible / 2;
                ++calls;
                next += std::chrono::milliseconds(20);
                std::this_thread::sleep_until(next);
            }
        });
    }

    void stop()
    {
        run = false;
        if (th.joinable()) th.join();
    }
};

// ---- process measurements ------------------------------------------------------------------
void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        Sleep(2);
    }
}

std::vector<DWORD> descendantPythons(DWORD root)
{
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    std::map<DWORD, DWORD> parent;
    std::map<DWORD, std::wstring> name;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        parent[pe.th32ProcessID] = pe.th32ParentProcessID;
        name[pe.th32ProcessID] = pe.szExeFile;
    }
    CloseHandle(snap);
    for (const auto &kv : parent) {
        std::wstring n = name[kv.first];
        for (auto &c : n) c = towlower(c);
        if (n.find(L"python") == std::wstring::npos) continue;
        DWORD p = kv.second;
        for (int depth = 0; depth < 8 && p; ++depth) {
            if (p == root) { out.push_back(kv.first); break; }
            auto it = parent.find(p);
            if (it == parent.end() || it->second == p) break;
            p = it->second;
        }
    }
    return out;
}

int threadCount()
{
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te{sizeof(te)};
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == self) ++n;
    CloseHandle(snap);
    return n;
}

struct Metrics {
    double privateMb = 0;
    unsigned handles = 0, gdi = 0, user = 0;
    int threads = 0;
    size_t pythons = 0;
};

Metrics sample()
{
    Metrics m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    m.privateMb = pmc.PrivateUsage / 1048576.0;
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    m.handles = h;
    m.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    m.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    m.threads = threadCount();
    m.pythons = descendantPythons(GetCurrentProcessId()).size();
    return m;
}

// Separates real leaks from host caches and heap fragmentation: Qt's pixmap
// cache (10 MB budget) and committed-but-free heap pages both look like growth.
void trimProcess()
{
    QPixmapCache::clear();
    _heapmin();
    HANDLE heaps[128];
    const DWORD n = GetProcessHeaps(128, heaps);
    for (DWORD i = 0; i < n && i < 128; ++i) HeapCompact(heaps[i], 0);
}

int gpuUsedMb()
{
    FILE *p = _wpopen(L"nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>NUL", L"rt");
    if (!p) return -1;
    int v = -1;
    if (std::fscanf(p, "%d", &v) != 1) v = -1;
    _pclose(p);
    return v;
}

void prepareSettings(const Options &o)
{
    QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
    s.clear();
    const bool backend = !o.backendHome.isEmpty();
    s.setValue(QStringLiteral("backend/home"), backend ? QDir::toNativeSeparators(o.backendHome)
                                                      : QStringLiteral("C:\\gbtts-harness-no-backend"));
    s.setValue(QStringLiteral("backend/autostart"), backend);
    s.setValue(QStringLiteral("backend/engine"), o.engine);
    s.setValue(QStringLiteral("backend/qwen_size"), QStringLiteral("0.6B"));
    s.setValue(QStringLiteral("ui/toolbar_button"), true);
    s.setValue(QStringLiteral("audio/preview_only"), false);
    s.setValue(QStringLiteral("text/read_channel_chat"), true);
    // Only the update mode checks (against a file:// feed): the others never touch the network.
    s.setValue(QStringLiteral("update/auto_check"), o.update);
    s.sync();
}

bool waitBackendConnected(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (logContains("connected")) return true;
        pump(50);
    }
    return false;
}

size_t waitNoPythons(DWORD root, int timeoutMs, int *elapsedMs)
{
    QElapsedTimer t;
    t.start();
    size_t n = 0;
    do {
        n = descendantPythons(root).size();
        if (n == 0) break;
        pump(20);
    } while (t.elapsed() < timeoutMs);
    if (elapsedMs) *elapsedMs = static_cast<int>(t.elapsed());
    return n;
}

// ---- mode: repeated init/shutdown cycles -----------------------------------------------------
int runCycles(const Options &o)
{
    const bool backend = !o.backendHome.isEmpty();
    std::printf("GameBaiters TTS - host lifecycle harness: %d cycles, %s, %s\n", o.cycles,
                o.unload ? "FreeLibrary every cycle" : "DLL stays loaded",
                backend ? QStringLiteral("backend %1 (%2)").arg(o.engine, o.backendHome).toUtf8().constData()
                        : "no backend");
    prepareSettings(o);
    const Metrics before = sample();
    std::printf("  before   : private %.1f MB, handles %u, threads %d, GDI %u, USER %u\n", before.privateMb,
                before.handles, before.threads, before.gdi, before.user);

    Plugin plugin;
    std::vector<Metrics> rows;
    bool allBackendsGone = true, allLoaded = true, anyAudio = false;
    int worstExitMs = 0;
    for (int c = 1; c <= o.cycles; ++c) {
        if (!plugin.lib && !plugin.load(o.dll)) {
            check("LoadLibrary + all exports", false, fmt("error %lu", GetLastError()));
            return 1;
        }
        {
            std::lock_guard<std::mutex> lk(g_logMutex);
            g_log.clear();
        }
        plugin.setFunctionPointers(makeFunctions());
        plugin.registerPluginID("gbtts-harness");
        plugin.previewHotkey();
        if (o.noInit) {
            pump(o.runMs);
            if (o.unload) plugin.unload();
            rows.push_back(sample());
            std::printf("  cycle %2d : load/unload only\n", c);
            continue;
        }
        plugin.init();
        AudioThread audio;
        audio.start(plugin);
        pump(300);
        if (!o.noUi) {
            plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, 1, 0);   // open window
            plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, 3, 0);   // voices
            plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, 2, 0);   // settings
        }
        if (backend) {
            if (!waitBackendConnected(60000)) allLoaded = false;
            if (c == 1) {
                // Enter pressed while the engine is still loading its model and voices:
                // queued and spoken when ready (up to v1.3 it was refused).
                const int64_t before = audio.audibleFrames.load();
                plugin.command(1, "Messaggio scritto mentre il motore si sta ancora caricando.");
                pump(9000);
                const int64_t spoken = audio.audibleFrames.load() - before;
                check("queue: message typed while the engine loads is spoken when ready", spoken > 20000,
                      fmt("%lld audible frames", (long long)spoken));
            }
            pump(o.runMs);
            plugin.command(1, "Prova del ciclo di vita del plugin.");
            pump(3000);
            if (c == 1) {
                // "Read my channel chat aloud": only OWN messages to the CHANNEL are spoken.
                auto audibleAfter = [&](const char *msg, anyID mode, anyID from) {
                    pump(2500);                                  // previous audio has ended
                    const int64_t before = audio.audibleFrames.load();
                    plugin.textMessage(1, mode, 0, from, "me", "uid", msg, 0);
                    pump(4000);
                    return audio.audibleFrames.load() - before;
                };
                const int64_t priv = audibleAfter("messaggio privato", TextMessageTarget_CLIENT, kOwnClientId);
                const int64_t other = audibleAfter("messaggio di un altro", TextMessageTarget_CHANNEL, 7);
                const int64_t own = audibleAfter("[b]Ciao[/b] a tutti dal [color=red]canale[/color]",
                                                 TextMessageTarget_CHANNEL, kOwnClientId);
                check("chat: own private message is not read", priv < 2000, fmt("%lld audible frames", (long long)priv));
                check("chat: someone else's channel message is not read", other < 2000,
                      fmt("%lld audible frames", (long long)other));
                check("chat: own channel message is read aloud", own > 20000, fmt("%lld audible frames", (long long)own));
                check("chat: BBCode stripped before speaking", logContains("(23 characters)"));
            }
            if (audio.audibleFrames.load() > 0) anyAudio = true;
        } else {
            plugin.command(1, "nessun motore");
            pump(o.runMs);
        }
        // TeamSpeak keeps its audio threads running while it calls shutdown.
        plugin.shutdown();
        pump(200);
        audio.stop();
        if (o.unload) plugin.unload();
        int exitMs = 0;
        const size_t left = waitNoPythons(GetCurrentProcessId(), 5000, &exitMs);
        if (left) allBackendsGone = false;
        worstExitMs = std::max(worstExitMs, exitMs);
        pump(300);
        if (o.trim) trimProcess();
        const Metrics m = sample();
        rows.push_back(m);
        std::printf("  cycle %2d : private %7.1f MB  handles %5u  threads %3d  GDI %4u  USER %4u  backend left %zu"
                    "  (audio calls %lld, audible frames %lld)\n",
                    c, m.privateMb, m.handles, m.threads, m.gdi, m.user, left,
                    static_cast<long long>(audio.calls.load()), static_cast<long long>(audio.audibleFrames.load()));
    }
    if (plugin.lib && !o.noFinalUnload) plugin.unload();
    std::fprintf(stderr, "[harness] cycles done, DLL %s\n", plugin.lib ? "still loaded" : "unloaded");

    // Cycle 1 pays Qt's one-time costs (styles, fonts, metatypes): growth is measured after it.
    const Metrics &first = rows.front();
    const Metrics &last = rows.back();
    const int n = static_cast<int>(rows.size()) - 1;
    std::printf("\n");
    if (backend) {
        check("backend connected in every cycle", allLoaded);
        check("voice audio reached the playback callback", anyAudio);
    }
    check("no backend process survives shutdown (<= 5 s)", allBackendsGone, fmt("slowest exit %d ms", worstExitMs));
    if (n > 0) {
        const double slope = (last.privateMb - first.privateMb) / n;
        check("private bytes stable across cycles (< 1.5 MB/cycle)", slope < 1.5,
              fmt("%+.2f MB/cycle (%.1f -> %.1f MB)", slope, first.privateMb, last.privateMb));
        if (rows.size() >= 20) {
            // A leak grows at a constant rate; a cache fills up and flattens.
            // Least-squares slope over the second half of the run.
            const size_t h = rows.size() / 2;
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            const double cnt = static_cast<double>(rows.size() - h);
            for (size_t i = h; i < rows.size(); ++i) {
                const double x = static_cast<double>(i), y = rows[i].privateMb;
                sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            const double s2 = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
            check("no steady growth in the second half (< 0.05 MB/cycle)", s2 < 0.05,
                  fmt("%+.3f MB/cycle over cycles %zu-%zu (%s)", s2, h + 1, rows.size(), o.trim ? "trimmed" : "raw"));
        }
        check("handles stable (<= +3 per cycle)", static_cast<int>(last.handles) - static_cast<int>(first.handles) <= 3 * n,
              fmt("%u -> %u", first.handles, last.handles));
        check("threads return to the same count", last.threads <= first.threads + 1,
              fmt("%d -> %d", first.threads, last.threads));
        check("GDI objects stable", static_cast<int>(last.gdi) - static_cast<int>(first.gdi) <= 4,
              fmt("%u -> %u", first.gdi, last.gdi));
        check("USER objects stable", static_cast<int>(last.user) - static_cast<int>(first.user) <= 4,
              fmt("%u -> %u", first.user, last.user));
    }
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- mode: child that "is TeamSpeak" and then crashes ------------------------------------------
int runChild(const Options &o)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Plugin plugin;
    if (!plugin.load(o.dll)) return 10;
    plugin.setFunctionPointers(makeFunctions());
    plugin.registerPluginID("gbtts-harness");
    const std::string previewKey = plugin.previewHotkey();
    plugin.init();
    AudioThread audio;
    audio.start(plugin);
    pump(300);
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, 1, 0);
    waitBackendConnected(60000);
    pump(o.waitMs);
    plugin.command(1, "Adesso il client va in crash.");
    pump(1500);
    // A setting changed 50 ms before dying must already be on disk.
    if (!previewKey.empty()) plugin.hotkey(previewKey.c_str());
    pump(50);

    const std::wstring evName = L"Local\\gbtts_harness_ready_" + std::to_wstring(o.parent);
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, evName.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    if (o.crash == "segv") {
        Sleep(200);
        volatile int *bad = nullptr;
        *bad = 42;   // unhandled access violation, no shutdown, no destructors: a real client crash
    }
    for (;;) pump(100);   // wait to be TerminateProcess'd
}

// ---- mode: parent of the crashing child -------------------------------------------------------
int runCrash(const Options &o, int argc, char **argv)
{
    std::printf("GameBaiters TTS - crash harness: client dies by %s with the %s engine loaded\n", o.crash.c_str(),
                o.engine.toUtf8().constData());
    prepareSettings(o);
    const int gpu0 = o.vram ? gpuUsedMb() : -1;

    const std::wstring evName = L"Local\\gbtts_harness_ready_" + std::to_wstring(GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, evName.c_str());
    std::wstring cmd = L"\"" + QCoreApplication::applicationFilePath().replace('/', '\\').toStdWString() + L"\" --child";
    for (int i = 1; i < argc; ++i) cmd += L" \"" + QString::fromLocal8Bit(argv[i]).toStdWString() + L"\"";
    cmd += L" --parent " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        check("spawn child client", false, fmt("error %lu", GetLastError()));
        return 1;
    }
    CloseHandle(pi.hThread);
    HANDLE waits[2] = {ready, pi.hProcess};
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 240000);
    if (w != WAIT_OBJECT_0) {
        check("child loaded the plugin and the engine", false, w == WAIT_OBJECT_0 + 1 ? "child exited early" : "timeout");
        TerminateProcess(pi.hProcess, 1);
        return 1;
    }
    const std::vector<DWORD> pids = descendantPythons(pi.dwProcessId);
    std::vector<HANDLE> procs;
    for (DWORD pid : pids)
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) procs.push_back(h);
    const int gpuPeak = o.vram ? gpuUsedMb() : -1;
    check("backend running inside the client", !procs.empty(), fmt("%zu python process(es)", procs.size()));

    if (o.crash == "terminate") TerminateProcess(pi.hProcess, 0xDEAD);
    const auto t0 = std::chrono::steady_clock::now();
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    const auto tDead = std::chrono::steady_clock::now();
    const double clientMs = std::chrono::duration<double, std::milli>(tDead - t0).count();
    std::printf("  client exit code 0x%08lX after %.0f ms\n", code, clientMs);
    CloseHandle(pi.hProcess);

    bool allGone = true;
    double worst = 0;
    for (HANDLE h : procs) {
        if (WaitForSingleObject(h, 10000) != WAIT_OBJECT_0) allGone = false;
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tDead).count());
        CloseHandle(h);
    }
    check("every backend process dies with the client (<= 3 s)", allGone && worst <= 3000,
          fmt("slowest %.0f ms after the client died", worst));

    if (o.vram && gpu0 >= 0) {
        int gpuAfter = gpuUsedMb();
        QElapsedTimer t;
        t.start();
        while (gpuAfter > gpu0 + 300 && t.elapsed() < 15000) {
            Sleep(250);
            gpuAfter = gpuUsedMb();
        }
        check("GPU memory returns to the baseline", gpuAfter <= gpu0 + 300,
              fmt("%d MB before, %d MB loaded, %d MB after %.1f s", gpu0, gpuPeak, gpuAfter, t.elapsed() / 1000.0));
    }

    QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
    check("setting changed 50 ms before the crash was saved", s.value(QStringLiteral("audio/preview_only")).toBool());
    check("engine choice persisted", s.value(QStringLiteral("backend/engine")).toString() == o.engine);
    s.clear();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- mode: auto-update (no network, nothing is launched) ------------------------------------------
QMessageBox *visibleBox(QMessageBox::Icon icon = QMessageBox::NoIcon)
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        auto *b = qobject_cast<QMessageBox *>(w);
        if (b && b->isVisible() && (icon == QMessageBox::NoIcon || b->icon() == icon)) return b;
    }
    return nullptr;
}

QMessageBox *waitBox(int timeoutMs, QMessageBox::Icon icon = QMessageBox::NoIcon)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (QMessageBox *b = visibleBox(icon)) return b;
        pump(50);
    }
    return nullptr;
}

bool waitLog(const char *needle, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (logContains(needle)) return true;
        pump(50);
    }
    return false;
}

// Top-level widgets the plugin could have left behind after shutdown.
int pluginWindowsLeft()
{
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        const QByteArray cls = w->metaObject()->className();
        if (cls.startsWith("gbtts::") || qobject_cast<QMessageBox *>(w)) ++n;
    }
    return n;
}

qint64 nextCheckInSeconds()
{
    QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
    return s.value(QStringLiteral("update/next_check")).toLongLong() - QDateTime::currentSecsSinceEpoch();
}

int runUpdate(const Options &o)
{
    std::printf("GameBaiters TTS - auto-update harness (file:// feed, dry run: nothing is launched)\n");
    prepareSettings(o);
    const QString dir = QDir::tempPath() + QStringLiteral("/gbtts_harness_update");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString downloads = QDir::temp().filePath(QStringLiteral("GameBaitersTTS-update"));
    QDir(downloads).removeRecursively();

    QByteArray pkg("PK\x03\x04", 4);
    for (int i = 0; i < 65536; ++i) pkg.append(char(i * 131 % 251));
    const QString pkgPath = dir + QStringLiteral("/gb_tts_9.9.9_win64.ts3_plugin");
    {
        QFile f(pkgPath);
        f.open(QIODevice::WriteOnly);
        f.write(pkg);
    }
    const QString goodSha = QString::fromLatin1(QCryptographicHash::hash(pkg, QCryptographicHash::Sha256).toHex());
    {
        QFile f(dir + QStringLiteral("/release-notes.txt"));
        f.open(QIODevice::WriteOnly);
        f.write("GameBaiters TTS v9.9.9\n======================\n\n* NEW - harness feature\n\n"
                "GameBaiters TTS v1.0.0\n======================\n\n* OLD - must not be shown\n");
    }
    auto writeFeed = [&](const QString &version, int build, const QString &sha) {
        QFile f(dir + QStringLiteral("/version.xml"));
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<versionDescription>\n"
                               "<product descVersion=\"1\" name=\"gb_tts\">\n"
                               "<latestVersion>%1</latestVersion><latestVersionString>%2</latestVersionString>\n"
                               "<latestDownload><url>%3</url></latestDownload>\n"
                               "<featureUrl>%4</featureUrl><sha256>%5</sha256>\n</product>\n</versionDescription>\n")
                    .arg(build)
                    .arg(version, QUrl::fromLocalFile(pkgPath).toString(),
                         QUrl::fromLocalFile(dir + QStringLiteral("/release-notes.txt")).toString(), sha)
                    .toUtf8());
    };
    writeFeed(QStringLiteral("9.9.9"), 90909, goodSha);
    qputenv("GBTTS_UPDATE_FEED", QUrl::fromLocalFile(dir + QStringLiteral("/version.xml")).toString().toUtf8());
    qputenv("GBTTS_UPDATE_NO_LAUNCH", "1");

    Plugin plugin;
    auto startPlugin = [&]() {
        if (!plugin.load(o.dll)) return false;
        plugin.setFunctionPointers(makeFunctions());
        plugin.registerPluginID("gbtts-harness");
        plugin.previewHotkey();
        plugin.init();
        return true;
    };
    auto stopPlugin = [&]() {
        plugin.shutdown();
        pump(200);
        plugin.unload();
        pump(500);
    };
    constexpr int kMenuCheckUpdates = 5;
    constexpr qint64 kDay = 24 * 3600;

    // 1. automatic check a few seconds after start
    if (!startPlugin()) {
        check("LoadLibrary + all exports", false, fmt("error %lu", GetLastError()));
        return 1;
    }
    QMessageBox *box = waitBox(10000);
    check("automatic check at startup offers the newer version", box && box->text().contains(QLatin1String("9.9.9")));
    check("offer lists only the notes newer than the installed version",
          box && box->detailedText().contains(QLatin1String("harness feature")) &&
              !box->detailedText().contains(QLatin1String("must not")));
    if (box && box->escapeButton()) box->escapeButton()->click();
    pump(400);
    qint64 next = nextCheckInSeconds();
    check("\"Later\" postpones the automatic check by 3 days", next > 3 * kDay - 120 && next <= 3 * kDay,
          fmt("%lld s", (long long)next));

    // 2. explicit check ignores the postponement; unloading with the offer open
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuCheckUpdates, 0);
    check("menu \"Check for updates\" asks again despite the postponement", waitBox(8000) != nullptr);
    stopPlugin();
    check("shutdown destroys the open offer", pluginWindowsLeft() == 0, fmt("%d left", pluginWindowsLeft()));

    // 3. update now: download, SHA-256 verified, helper written (not launched)
    if (!startPlugin()) return 1;
    pump(300);
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuCheckUpdates, 0);
    box = waitBox(8000);
    if (box && box->defaultButton()) box->defaultButton()->click();
    check("download verified and update helper written", waitLog("update helper written, not launched", 15000));
    QFile got(downloads + QStringLiteral("/gb_tts_9.9.9_win64.ts3_plugin"));
    const bool gotOpen = got.open(QIODevice::ReadOnly);
    check("downloaded package identical to the published one",
          gotOpen && QCryptographicHash::hash(got.readAll(), QCryptographicHash::Sha256).toHex() == goodSha.toLatin1());
    got.close();
    QFile helper(downloads + QStringLiteral("/gbtts_update_helper.bat"));
    const QString helperText = helper.open(QIODevice::ReadOnly) ? QString::fromUtf8(helper.readAll()) : QString();
    helper.close();
    check("helper targets this client's plugin folder",
          helperText.contains(QDir::toNativeSeparators(QString::fromStdString(g_configDir)) + QStringLiteral("plugins\\gb_tts_win64.dll")) &&
              helperText.contains(QDir::toNativeSeparators(got.fileName())));
    check("no leftover partial download", !QFileInfo::exists(got.fileName() + QStringLiteral(".part")));
    stopPlugin();
    check("shutdown destroys the progress window", pluginWindowsLeft() == 0, fmt("%d left", pluginWindowsLeft()));

    // 4. damaged download: refused and reported, nothing kept
    QDir(downloads).removeRecursively();
    writeFeed(QStringLiteral("9.9.9"), 90909, QString(64, QLatin1Char('0')));
    if (!startPlugin()) return 1;
    pump(300);
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuCheckUpdates, 0);
    box = waitBox(8000);
    if (box && box->defaultButton()) box->defaultButton()->click();
    QMessageBox *failed = waitBox(15000, QMessageBox::Warning);
    check("damaged download refused (checksum) and reported", failed && failed->text().contains(QLatin1String("9.9.9")));
    check("damaged package not kept", !QFileInfo::exists(downloads + QStringLiteral("/gb_tts_9.9.9_win64.ts3_plugin")) &&
                                          !QFileInfo::exists(downloads + QStringLiteral("/gbtts_update_helper.bat")));
    if (failed && failed->button(QMessageBox::Ok)) failed->button(QMessageBox::Ok)->click();
    pump(300);

    // 5. nothing newer: "up to date", next automatic check in a day
    writeFeed(QStringLiteral("0.0.1"), 1, goodSha);
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuCheckUpdates, 0);
    box = waitBox(8000, QMessageBox::Information);
    check("older feed: \"up to date\" reported", box != nullptr);
    next = nextCheckInSeconds();
    check("up to date: next automatic check in 1 day", next > kDay - 120 && next <= kDay, fmt("%lld s", (long long)next));
    stopPlugin();
    check("nothing of the plugin survives the last unload", pluginWindowsLeft() == 0);

    qunsetenv("GBTTS_UPDATE_FEED");
    qunsetenv("GBTTS_UPDATE_NO_LAUNCH");
    QDir(dir).removeRecursively();
    QDir(downloads).removeRecursively();
    QSettings(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp)).clear();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- mode: engine installation window --------------------------------------------------------------------
bool waitUntil(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        pump(50);
    }
    return cond();
}

QWidget *setupDialog()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && QByteArray(w->metaObject()->className()) == "gbtts::EngineSetupDialog") return w;
    return nullptr;
}

int setupPage()
{
    QWidget *d = setupDialog();
    auto *pages = d ? d->findChild<QStackedWidget *>(QStringLiteral("gbttsSetupPages")) : nullptr;
    return pages ? pages->currentIndex() : -1;
}

void clickSetup(const char *name)
{
    if (QWidget *d = setupDialog())
        if (auto *b = d->findChild<QAbstractButton *>(QString::fromLatin1(name))) b->click();
}

bool processAlive(DWORD pid)
{
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

DWORD readPid(const QString &file, const char *jsonKey = nullptr)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray data = f.readAll();
    if (!jsonKey) return DWORD(data.trimmed().toULong());
    return DWORD(QJsonDocument::fromJson(data).object().value(QString::fromLatin1(jsonKey)).toDouble());
}

int runSetup(const Options &o)
{
    const bool real = o.setupReal;
    if (o.installer.isEmpty() || !QFileInfo::exists(o.installer)) {
        std::printf("--setup-fake/--setup-real need --installer <ps1>\n");
        return 2;
    }
    QString home = o.setupHome;
    if (home.isEmpty())
        home = real ? QDir::toNativeSeparators(qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/GameBaitersTTS"))
                    : QDir::toNativeSeparators(QDir::tempPath() + QStringLiteral("/gbtts_harness_setup_home"));
    std::printf("GameBaiters TTS - installation window harness (%s, mode %s, %s)\n", real ? "REAL installation" : "fake installer",
                o.setupMode.toUtf8().constData(), home.toUtf8().constData());
    if (!real) QDir(home).removeRecursively();
    qputenv("GBTTS_INSTALLER_SCRIPT", QDir::toNativeSeparators(o.installer).toUtf8());
    qputenv("GBTTS_DEFAULT_HOME", home.toUtf8());
    if (!real) qputenv("GBTTS_FAKE_STEP_MS", "1500");

    Options so = o;
    so.engine = o.setupMode == QLatin1String("gpu") ? QStringLiteral("qwen") : QStringLiteral("kokoro");
    prepareSettings(so);
    {
        QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
        s.remove(QStringLiteral("backend/home"));   // a PC that never had the engine
        s.setValue(QStringLiteral("backend/autostart"), true);
        s.sync();
    }
    const QString statusFile = home + QStringLiteral("\\install\\status.json");

    Plugin plugin;
    auto startPlugin = [&]() {
        if (!plugin.load(o.dll)) return false;
        {
            std::lock_guard<std::mutex> lk(g_logMutex);
            g_log.clear();
        }
        plugin.setFunctionPointers(makeFunctions());
        plugin.registerPluginID("gbtts-harness");
        plugin.previewHotkey();
        plugin.init();
        return true;
    };
    auto stopPlugin = [&]() {
        plugin.shutdown();
        pump(200);
        plugin.unload();
        pump(300);
    };

    if (!startPlugin()) {
        check("LoadLibrary + all exports", false);
        return 1;
    }
    AudioThread audio;
    audio.start(plugin);
    check("installation window opens by itself when no engine is installed", waitUntil([] { return setupDialog() != nullptr; }, 8000));
    QWidget *dlg = setupDialog();
    auto *folder = dlg ? dlg->findChild<QLineEdit *>(QStringLiteral("gbttsSetupFolder")) : nullptr;
    auto *install = dlg ? dlg->findChild<QPushButton *>(QStringLiteral("gbttsSetupInstall")) : nullptr;
    check("proposed folder is the default engine folder", folder && folder->text() == home, folder ? folder->text().toStdString() : std::string());
    check("install button enabled (enough space)", install && install->isEnabled());
    clickSetup(o.setupMode == QLatin1String("gpu") ? "gbttsSetupFull" : "gbttsSetupLight");
    if (install) install->click();
    check("progress page shown, installer running", waitUntil([] { return setupPage() == 1; }, 5000) &&
                                                        waitLog("engine installer started", 5000));
    auto percent = []() -> int {
        QWidget *d = setupDialog();
        auto *l = d ? d->findChild<QLabel *>(QStringLiteral("gbttsSetupPercent")) : nullptr;
        return l ? l->text().section(QLatin1Char(' '), 0, 0).toInt() : -1;
    };

    if (!real) {
        const bool moved = waitUntil([&] { return percent() >= 10; }, 20000);
        check("progress moves", moved, fmt("%d %%", percent()));
        const DWORD installerPid = readPid(statusFile, "pid");
        const DWORD childPid = readPid(home + QStringLiteral("\\install\\fake_child.pid"));
        check("installer and its child process run hidden", processAlive(installerPid) && processAlive(childPid),
              fmt("pid %lu, child %lu", installerPid, childPid));
        clickSetup("gbttsSetupCancel");
        pump(500);
        check("one click on Cancel does not cancel", processAlive(installerPid));
        clickSetup("gbttsSetupCancel");
        check("second click cancels: result page", waitUntil([] { return setupPage() == 2; }, 15000));
        check("the whole process tree is gone (installer + child)", waitUntil([&] { return !processAlive(installerPid) && !processAlive(childPid); }, 5000));
        check("cancel logged", logContains("engine installation cancelled by the user"));

        clickSetup("gbttsSetupRetry");
        check("Retry goes back to the choice", waitUntil([] { return setupPage() == 0; }, 2000));
        clickSetup("gbttsSetupInstall");
        check("second attempt running", waitUntil([] { return setupPage() == 1; }, 5000));
        pump(3000);
        const DWORD secondPid = readPid(statusFile, "pid");
        std::printf("        before unload: installer %lu alive=%d\n", secondPid, int(processAlive(secondPid)));
        audio.stop();
        stopPlugin();
        const bool survived = processAlive(secondPid);
        check("plugin unloaded mid-install: installer keeps running", survived, fmt("pid %lu", secondPid));
        if (!survived) {
            QFile lf(home + QStringLiteral("\\install\\install.log"));
            if (lf.open(QIODevice::ReadOnly)) std::printf("%s\n", lf.readAll().right(1500).constData());
            QFile sf(statusFile);
            if (sf.open(QIODevice::ReadOnly)) std::printf("%s\n", sf.readAll().constData());
        }
        check("no window of the plugin left", pluginWindowsLeft() == 0, fmt("%d left", pluginWindowsLeft()));

        if (!startPlugin()) return 1;
        audio.start(plugin);
        check("reloaded plugin re-attaches to the running installation",
              waitLog("following the engine installation already running", 5000));
        check("installation finishes and is reported", waitLog("engine installer finished: done", 30000));
        {
            QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
            check("settings point to the installed engine", QDir::toNativeSeparators(s.value(QStringLiteral("backend/home")).toString()) == home,
                  s.value(QStringLiteral("backend/home")).toString().toStdString());
        }
        audio.stop();
        stopPlugin();
        check("nothing of the plugin survives the unload", pluginWindowsLeft() == 0);
        waitUntil([&] { return !processAlive(readPid(home + QStringLiteral("\\install\\fake_child.pid"))); }, 3000);
        QDir(home).removeRecursively();
    } else {
        int last = -1;
        QElapsedTimer t;
        t.start();
        bool done = false;
        while (t.elapsed() < 120 * 60 * 1000) {
            pump(1000);
            const int p = percent();
            if (p != last && p >= 0) {
                std::printf("        %3d %%  (%lld s)\n", p, static_cast<long long>(t.elapsed() / 1000));
                std::fflush(stdout);
                last = p;
            }
            if (logContains("engine installer finished")) {
                done = true;
                break;
            }
        }
        check("installation finished successfully through the window", done && logContains("engine installer finished: done"),
              fmt("%lld min", static_cast<long long>(t.elapsed() / 60000)));
        check("result page shown", waitUntil([] { return setupPage() == 2; }, 5000));
        check("engine starts by itself after the installation", waitBackendConnected(6 * 60 * 1000));
        // Ready = voices listed (first Qwen start designs Giulia and Marco, about a minute).
        const int64_t before = audio.audibleFrames.load();
        plugin.command(1, "Installazione completata: la voce funziona.");
        check("the new engine speaks", waitUntil([&] { return audio.audibleFrames.load() - before > 20000; }, 5 * 60 * 1000),
              fmt("%lld audible frames", (long long)(audio.audibleFrames.load() - before)));
        audio.stop();
        stopPlugin();
        int exitMs = 0;
        check("engine gone after shutdown", waitNoPythons(GetCurrentProcessId(), 5000, &exitMs) == 0, fmt("%d ms", exitMs));
    }
    QSettings(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp)).clear();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- mode: backend code refreshed after a plugin update ---------------------------------------------
// cmd /c with several quoted arguments needs the whole command quoted once more.
int runCmd(const QString &command)
{
    QProcess p;
    p.setProgram(QStringLiteral("cmd.exe"));
    p.setNativeArguments(QStringLiteral("/d /c \"%1\"").arg(command));
    p.start();
    p.waitForFinished(15000);
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

bool makeJunction(const QString &link, const QString &target)
{
    runCmd(QStringLiteral("mklink /J \"%1\" \"%2\"").arg(QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)));
    return QFileInfo(link).isDir();
}

// rmdir on a junction removes the link only, never the target's content.
void removeJunction(const QString &link)
{
    if (QFileInfo(link).exists() || QFileInfo(link).isSymLink())
        runCmd(QStringLiteral("rmdir \"%1\"").arg(QDir::toNativeSeparators(link)));
}

bool copyDir(const QString &from, const QString &to)
{
    QDir().mkpath(to);
    for (const QFileInfo &e : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (e.isDir()) {
            if (e.fileName() != QLatin1String("__pycache__") && !copyDir(e.absoluteFilePath(), to + QLatin1Char('/') + e.fileName()))
                return false;
        } else if (!QFile::copy(e.absoluteFilePath(), to + QLatin1Char('/') + e.fileName())) {
            return false;
        }
    }
    return true;
}

QString pyVersion(const QString &initPy)
{
    QFile f(initPy);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QString text = QString::fromUtf8(f.readAll());
    const int at = text.indexOf(QLatin1String("__version__ = \""));
    return at < 0 ? QString() : text.mid(at + 15, text.indexOf(QLatin1Char('"'), at + 15) - at - 15);
}

int runCodeSync(const Options &o)
{
    if (o.backendHome.isEmpty()) {
        std::printf("--code-sync needs --backend-home <development backend with .venv and models>\n");
        return 2;
    }
    const QString src = o.backendHome;
    const QString packaged = pyVersion(src + QStringLiteral("/gbtts/__init__.py"));
    std::printf("GameBaiters TTS - engine code refresh harness (packaged backend %s, engine %s)\n",
                packaged.toUtf8().constData(), o.engine.toUtf8().constData());
    prepareSettings(o);

    const QString home = QDir::tempPath() + QStringLiteral("/gbtts_harness_home");
    auto cleanHome = [&] {
        removeJunction(home + QStringLiteral("/venv"));
        removeJunction(home + QStringLiteral("/models"));
        if (QFileInfo(home + QStringLiteral("/venv")).exists() || QFileInfo(home + QStringLiteral("/models")).exists())
            return false;   // never delete recursively through a junction
        return QDir(home).removeRecursively();
    };
    if (!cleanHome()) {
        check("stale test home removed", false);
        return 1;
    }
    // An installer-style home: venv\ + models\ (junctions to the dev install) and app\ with OLDER code.
    QDir().mkpath(home + QStringLiteral("/app/tools"));
    // --engine-venv / --engine-models: borrow an installed engine's (the dev .venv can be dead
    // after a Windows reinstall); the code under test always comes from --backend-home.
    const QString venvSrc = o.engineVenv.isEmpty() ? src + QStringLiteral("/.venv") : o.engineVenv;
    const QString modelsSrc = o.engineModels.isEmpty() ? src + QStringLiteral("/models") : o.engineModels;
    const bool linked = makeJunction(home + QStringLiteral("/venv"), venvSrc) &&
                        makeJunction(home + QStringLiteral("/models"), modelsSrc);
    const QString app = home + QStringLiteral("/app");
    bool prepared = linked && copyDir(src + QStringLiteral("/gbtts"), app + QStringLiteral("/gbtts")) &&
                    QFile::copy(src + QStringLiteral("/requirements.txt"), app + QStringLiteral("/requirements.txt"));
    {
        QFile init(app + QStringLiteral("/gbtts/__init__.py"));
        init.open(QIODevice::ReadOnly);
        QString text = QString::fromUtf8(init.readAll());
        init.close();
        text.replace(QStringLiteral("__version__ = \"%1\"").arg(packaged), QStringLiteral("__version__ = \"1.3.0\""));
        init.open(QIODevice::WriteOnly | QIODevice::Truncate);
        init.write(text.toUtf8());
        init.close();
        QFile stale(app + QStringLiteral("/gbtts/stale_module.py"));
        stale.open(QIODevice::WriteOnly);
        stale.write("# removed from the package in a later version\n");
    }
    // The plugin package as TeamSpeak extracted it.
    const QString pkg = QString::fromStdString(g_configDir) + QStringLiteral("plugins/gb_tts/backend");
    QDir(pkg).removeRecursively();
    prepared = prepared && copyDir(src + QStringLiteral("/gbtts"), pkg + QStringLiteral("/gbtts")) &&
               QFile::copy(src + QStringLiteral("/requirements.txt"), pkg + QStringLiteral("/requirements.txt")) &&
               QDir().mkpath(pkg + QStringLiteral("/tools")) &&
               QFile::copy(src + QStringLiteral("/tools/download_models.py"), pkg + QStringLiteral("/tools/download_models.py"));
    check("test home: junctions to the dev venv/models, app\\ at 1.3.0", prepared && pyVersion(app + QStringLiteral("/gbtts/__init__.py")) == QLatin1String("1.3.0"));
    {
        QSettings s(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp));
        s.setValue(QStringLiteral("backend/home"), QDir::toNativeSeparators(home));
        s.sync();
    }

    Plugin plugin;
    if (!plugin.load(o.dll)) {
        check("LoadLibrary + all exports", false);
        return 1;
    }
    plugin.setFunctionPointers(makeFunctions());
    plugin.registerPluginID("gbtts-harness");
    plugin.previewHotkey();
    plugin.init();
    const bool connected = waitBackendConnected(90000);
    const QByteArray updatedLine = QStringLiteral("voice engine code updated 1.3.0 -> %1").arg(packaged).toUtf8();
    check("older installed code replaced before the engine starts", logContains(updatedLine.constData()));
    check("engine runs the new code (reports the packaged version)",
          connected && logContains(QStringLiteral("backend %1 connected").arg(packaged).toUtf8().constData()));
    check("app\\gbtts now at the packaged version", pyVersion(app + QStringLiteral("/gbtts/__init__.py")) == packaged);
    check("module dropped by the package is gone", !QFileInfo::exists(app + QStringLiteral("/gbtts/stale_module.py")));
    check("no gbtts.new / gbtts.old left behind", !QFileInfo::exists(app + QStringLiteral("/gbtts.new")) &&
                                                      !QFileInfo::exists(app + QStringLiteral("/gbtts.old")));
    check("no library warning while requirements match", !logContains("needs different Python libraries"));

    constexpr int kMenuRestart = 4;
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_log.clear();
    }
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuRestart, 0);
    const bool again = waitBackendConnected(90000);
    check("restart: code already current, nothing copied", again && !logContains("voice engine code updated"));

    {   // same __version__, different content (a hotfix, new stock voices): still installed
        QFile f(pkg + QStringLiteral("/gbtts/textproc.py"));
        f.open(QIODevice::Append);
        f.write("\n# hotfix without a version bump\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_log.clear();
    }
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuRestart, 0);
    const bool third = waitBackendConnected(90000);
    const QByteArray sameVersion = QStringLiteral("voice engine code updated %1 -> %1").arg(packaged).toUtf8();
    QFile patched(app + QStringLiteral("/gbtts/textproc.py"));
    check("same version, changed files: installed anyway",
          third && logContains(sameVersion.constData()) && patched.open(QIODevice::ReadOnly) &&
              patched.readAll().contains("hotfix without a version bump"));
    patched.close();

    {
        QFile req(app + QStringLiteral("/requirements.txt"));
        req.open(QIODevice::Append);
        req.write("fakepkg==1.0\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_log.clear();
    }
    plugin.menu(0, PLUGIN_MENU_TYPE_GLOBAL, kMenuRestart, 0);
    waitBackendConnected(90000);
    check("requirements drift reported (Install / repair needed)", logContains("needs different Python libraries"));

    plugin.shutdown();
    pump(200);
    plugin.unload();
    int exitMs = 0;
    check("engine gone after shutdown", waitNoPythons(GetCurrentProcessId(), 5000, &exitMs) == 0, fmt("%d ms", exitMs));
    const bool removed = cleanHome();
    check("test home removed, dev venv and models untouched",
          removed && QFileInfo::exists(src + QStringLiteral("/.venv/Scripts/python.exe")) &&
              QFileInfo(src + QStringLiteral("/models")).isDir());
    QDir(pkg).removeRecursively();
    QSettings(QStringLiteral("GameBaiters"), QString::fromLatin1(kSettingsApp)).clear();
    std::printf("\nRESULT: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("GBTTS_SETTINGS_APP", kSettingsApp);
    // An engine really installed on this PC (%LOCALAPPDATA%\GameBaitersTTS) must not leak
    // into the harness: only --backend-home or the setup modes provide one.
    qputenv("GBTTS_DEFAULT_HOME", QByteArray(qgetenv("TEMP")) + "\\gbtts_harness_no_default_home");
    QApplication app(argc, argv);
    const Options o = parse(argc, argv);
    if (o.dll.empty()) {
        std::printf("usage: see the header of host_harness.cpp\n");
        return 2;
    }
    const QString cfg = QDir::toNativeSeparators(QDir::tempPath() + QStringLiteral("/gbtts_harness_cfg/"));
    QDir().mkpath(cfg);
    g_configDir = cfg.toStdString();

    if (o.child) return runChild(o);
    if (!o.crash.empty()) return runCrash(o, argc, argv);
    SetUnhandledExceptionFilter(crashReporter);
    const int rc = o.update                        ? runUpdate(o)
                   : o.codeSync                    ? runCodeSync(o)
                   : (o.setupFake || o.setupReal)  ? runSetup(o)
                                                   : runCycles(o);
    std::fflush(stdout);
    std::fprintf(stderr, "[harness] leaving main: QApplication is destroyed next\n");
    return rc;
}
