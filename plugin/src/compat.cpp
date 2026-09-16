// ---------------------------------------------------------------------------
// Host facade for the Soundboard sources compiled into this plugin.
//
// The shared DSP / effects-editor files reference a handful of plugin-side
// symbols (logging, TeamSpeak function table, config path, the FFmpeg factory
// used only by the convolution reverb's "load IR from file"). This translation
// unit is their single definition for GameBaiters TTS.
// ---------------------------------------------------------------------------
#include "common.h"
#include "ts3log.h"
#include "inputfile.h"

#include <QDateTime>
#include <QString>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

extern "C" {
struct TS3Functions ts3Functions;
int g_rpsbLogsEnabled    = 0;
int g_rpsbExtremeLogging = 0;
int g_rpsbPreviewOnly    = 0;
}

namespace gbtts {
std::string g_pluginId;
}

namespace {

constexpr int kRingCap = 600;
std::deque<LogRingEntry> g_ring;
std::mutex               g_ringMutex;
char                     g_configPath[PATH_BUFSIZE] = {0};

void ringPush(int level, const char *body)
{
    LogRingEntry e;
    e.level = level;
    e.text = QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                           QString::fromUtf8(body));
    std::lock_guard<std::mutex> lk(g_ringMutex);
    if (g_ring.size() >= static_cast<size_t>(kRingCap)) g_ring.pop_front();
    g_ring.push_back(std::move(e));
}

void emitLog(const char *buf, LogLevel level)
{
    if (ts3Functions.logMessage)
        ts3Functions.logMessage(buf, level, "GBTTS", 0);
    ringPush(static_cast<int>(level), buf);
}

} // namespace

void logMessage(const char *msg, LogLevel level, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, level);
    vsnprintf(buf, sizeof(buf), msg ? msg : "", ap);
    va_end(ap);
    emitLog(buf, level);
}

UINT checkError(UINT code, const char *msg, ...)
{
    if (code == ERROR_ok) return code;
    char buf[1024];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg ? msg : "", ap);
    va_end(ap);
    char line[1100];
    snprintf(line, sizeof(line), "%s (error %u)", buf, code);
    emitLog(line, LogLevel_ERROR);
    return code;
}

QVector<LogRingEntry> logRingSnapshot()
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    QVector<LogRingEntry> out;
    out.reserve(static_cast<int>(g_ring.size()));
    for (const auto &e : g_ring) out.push_back(e);
    return out;
}

void logRingClear()
{
    std::lock_guard<std::mutex> lk(g_ringMutex);
    g_ring.clear();
}

extern "C" void rpsbDebugRingPush(const char *line)
{
    if (line) ringPush(static_cast<int>(LogLevel_DEBUG), line);
}

extern "C" const char *getPluginID()
{
    return gbtts::g_pluginId.c_str();
}

extern "C" const char *getTs3ConfigPath()
{
    if (g_configPath[0] == '\0' && ts3Functions.getConfigPath) {
        ts3Functions.getConfigPath(g_configPath, PATH_BUFSIZE);
        size_t len = strlen(g_configPath);
        if (len > 0 && g_configPath[len - 1] != '/' && g_configPath[len - 1] != '\\' && len < PATH_BUFSIZE - 1) {
            g_configPath[len] = '\\';
            g_configPath[len + 1] = '\0';
        }
    }
    return g_configPath;
}

// The convolution reverb can load a user impulse response through the
// Soundboard's FFmpeg decoder. GameBaiters TTS ships without FFmpeg (a TTS
// plugin has nothing to decode): the factory reports "unavailable" and the
// reverb keeps using its procedural presets. ConvolutionReverb checks for null.
InputFile *CreateInputFileFFmpeg(InputFileOptions)
{
    return nullptr;
}

void InitFFmpegLibrary() {}

namespace InputFileNet {
void setShutdownAbort(bool) {}
}
