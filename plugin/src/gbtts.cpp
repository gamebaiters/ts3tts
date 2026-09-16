// ---------------------------------------------------------------------------
// Plugin lifecycle and TeamSpeak callback bridge.
//
// Crash-safety rules inherited from the GameBaiters Soundboard (vault:
// ghost-crash-forensics, threading-model) and applied here:
//  1. never block inside a TeamSpeak callback;
//  2. every deferred callback is bound to a plugin-owned QObject that is
//     deleted FIRST in shutdown, so nothing can fire into the unloaded DLL;
//  3. audio callbacks read an atomic pointer; shutdown nulls it and pumps
//     the event loop for 50 ms before the object dies;
//  4. no widget of ours survives in the host (toolbar button removed first,
//     top-level windows deleted, deferred deletes drained, font cache purged).
// ---------------------------------------------------------------------------
#include "gbtts.h"

#include "audio/TtsAudio.h"
#include "core/Controller.h"
#include "core/Defer.h"
#include "core/Updater.h"
#include "ui/EngineSetupDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/ToolbarButton.h"
#include "ui/TtsIcons.h"
#include "ui/TtsWindow.h"
#include "ui/VoiceDialog.h"

#include "common.h"
#include "modules/channel_sandbox_dialog.h"
#include "modules/theme.h"
#include "ts3log.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" const char *getTs3ConfigPath();

namespace gbtts {

namespace {

QObject                 *g_ctx = nullptr;
Controller              *g_ctrl = nullptr;
std::atomic<TtsAudio *>  g_audio{nullptr};
std::atomic<bool>        g_shuttingDown{false};
QPointer<TtsWindow>      g_window;
QPointer<SettingsDialog> g_settingsDlg;
QPointer<VoiceDialog>    g_voiceDlg;
QPointer<EngineSetupDialog> g_setupDlg;
Updater                 *g_updater = nullptr;
QTranslator             *g_trOwn = nullptr;
QTranslator             *g_trShared = nullptr;

void installTranslations(const QString &pref)
{
    // Italian unless the user explicitly picked English (Settings migrates the
    // v1.0 "auto" value to "it").
    if (pref == QLatin1String("en")) return;

    // Plugin-unique resource paths (see CMakeLists): QTranslator maps the data
    // in place, so it must never resolve to another plugin's DLL.
    auto *own = new QTranslator();
    if (own->load(QStringLiteral(":/gbtts/i18n/gbtts_it.qm"))) {
        qApp->installTranslator(own);
        g_trOwn = own;
    } else {
        delete own;
    }
    auto *shared = new QTranslator();
    if (shared->load(QStringLiteral(":/gbtts/i18n/soundboard_it.qm"))) {
        qApp->installTranslator(shared);
        g_trShared = shared;
    } else {
        delete shared;
    }
}

// Same palette as the Soundboard when it is installed and themed: both plugins
// then look like one product. Read-only access to its INI.
void loadSoundboardTheme()
{
    const QString ini = QString::fromUtf8(getTs3ConfigPath()) + QStringLiteral("rp_soundboard.ini");
    if (!QFile::exists(ini)) return;
    QSettings s(ini, QSettings::IniFormat);
    Theme::Colors c = Theme::defaultColors();
    c.enabled = s.value(QStringLiteral("theme_enabled"), false).toBool();
    auto color = [&s](const char *key, const QColor &fallback) {
        const QString v = s.value(QString::fromLatin1(key)).toString();
        QColor q(v);
        return (v.isEmpty() || !q.isValid()) ? fallback : q;
    };
    c.accent = color("theme_accent", c.accent);
    c.waveform = color("theme_waveform", c.waveform);
    c.background = color("theme_background", c.background);
    c.text = color("theme_text", QColor());
    c.button = color("theme_button", QColor());
    c.contrast = s.value(QStringLiteral("theme_contrast"), c.contrast).toInt();
    Theme::setColors(c);
    Theme::setUiFontPointSize(s.value(QStringLiteral("ui_font_pt"), 0).toInt());
}

bool alive()
{
    return g_ctrl && !g_shuttingDown.load(std::memory_order_acquire);
}

void writeMenuIcon()
{
    const QString dir = QString::fromUtf8(getTs3ConfigPath()) + QStringLiteral("plugins/gb_tts");
    const QString png = dir + QStringLiteral("/gbtts_16.png");
    if (QFile::exists(png)) return;
    QDir().mkpath(dir);
    TtsIcons::app().pixmap(16, 16).save(png, "PNG");
}

void deleteWidget(QWidget *w)
{
    if (!w) return;
    w->hide();
    w->setParent(nullptr);
    delete w;
}

uint64_t ownClientServer(uint64_t sch, uint16_t clientID)
{
    anyID me = 0;
    if (ts3Functions.getClientID(sch, &me) != ERROR_ok) return 0;
    return me == clientID ? sch : 0;
}

} // namespace

// ===========================================================================
void init()
{
    g_shuttingDown.store(false, std::memory_order_release);
    if (!g_ctx) g_ctx = new QObject();

    // Deferred like the Soundboard: TeamSpeak's own UI is still being built
    // during ts3plugin_init. A child timer of g_ctx (core/Defer.h - never a
    // QTimer::singleShot functor) so a disable within these few ms cancels
    // instead of half-initialising, and nothing stays queued past the unload.
    deferCall(g_ctx, 10, [] {
        Settings prefs;
        prefs.load();
        installTranslations(prefs.uiLanguage);
        SandboxModules::loadIntoDsp();
        loadSoundboardTheme();
        writeMenuIcon();

        g_ctrl = new Controller();
        g_ctrl->startup();
        g_audio.store(g_ctrl->audio(), std::memory_order_release);

        if (g_ctrl->settings().toolbarButton) ToolbarButton::install();
        g_updater = new Updater(g_ctrl);
        logInfo("GameBaiters TTS %s ready", GBTTS_VERSION);

        // Like the Soundboard, but not while TeamSpeak is still connecting and
        // drawing its own windows: a few seconds later, at most once a day.
        deferCall(g_ctx, 4000, [] {
            if (alive() && g_updater) g_updater->checkAutomatically();
        });

        // No voice engine on this PC (first install, or Windows reinstalled): offer the
        // installation right away instead of waiting for the user to find it in Settings.
        deferCall(g_ctx, 2500, [] {
            if (alive() && g_ctrl->state() == Controller::Backend::NotInstalled) {
                logInfo("GBTTS: voice engine not installed (%s): opening the installation window",
                        g_ctrl->backendProblem().toUtf8().constData());
                openEngineSetup();
            }
        });
    });
}

void shutdown()
{
    g_shuttingDown.store(true, std::memory_order_release);

    ToolbarButton::remove();

    delete g_ctx;
    g_ctx = nullptr;

    // Network replies, message boxes and the parentless progress window of an
    // update: all of them hold vtables/slots in this DLL.
    delete g_updater;
    g_updater = nullptr;

    // Audio callbacks: stop handing out the pointer, let in-flight blocks finish.
    // Pump instead of sleeping: this is the client's STA thread and DirectSound
    // marshals COM calls through it during teardown.
    g_audio.store(nullptr, std::memory_order_release);
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 50) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    deleteWidget(g_setupDlg.data());
    deleteWidget(g_voiceDlg.data());
    deleteWidget(g_settingsDlg.data());
    deleteWidget(g_window.data());

    if (g_ctrl) {
        g_ctrl->teardown();
        delete g_ctrl;
        g_ctrl = nullptr;
    }

    if (g_trOwn) {
        qApp->removeTranslator(g_trOwn);
        delete g_trOwn;
        g_trOwn = nullptr;
    }
    if (g_trShared) {
        qApp->removeTranslator(g_trShared);
        delete g_trShared;
        g_trShared = nullptr;
    }

    // Font cache purge (Soundboard dump-proven crash-on-close fix): Qt keeps a
    // global QFontCache whose keys can reference strings owned by this DLL and
    // clears it only after the DLL is gone.
    {
        using FcInstanceFn = void *(*)();
        using FcClearFn = void (*)(void *);
        FcInstanceFn fcInstance = nullptr;
        FcClearFn fcClear = nullptr;
#ifdef _WIN32
        if (HMODULE qtgui = GetModuleHandleW(L"Qt5Gui.dll")) {
            fcInstance = reinterpret_cast<FcInstanceFn>(GetProcAddress(qtgui, "?instance@QFontCache@@SAPEAV1@XZ"));
            fcClear = reinterpret_cast<FcClearFn>(GetProcAddress(qtgui, "?clear@QFontCache@@QEAAXXZ"));
        }
#endif
        if (fcInstance && fcClear) {
            if (void *fc = fcInstance()) fcClear(fc);
        }
    }

    if (QCoreApplication::instance()) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// ===========================================================================
void openWindow(bool focusInput)
{
    if (!alive()) return;
    if (!g_window) {
        g_window = new TtsWindow(g_ctrl);
        ToolbarButton::watchWindow(g_window);
    }
    g_window->showAndRaise(focusInput);
}

void openSettings()
{
    if (!alive()) return;
    if (!g_settingsDlg) g_settingsDlg = new SettingsDialog(g_ctrl);
    g_settingsDlg->show();
    g_settingsDlg->raise();
    g_settingsDlg->activateWindow();
}

void openVoices()
{
    if (!alive()) return;
    if (!g_voiceDlg) g_voiceDlg = new VoiceDialog(g_ctrl);
    g_voiceDlg->show();
    g_voiceDlg->raise();
    g_voiceDlg->activateWindow();
}

void restartBackend()
{
    if (alive()) g_ctrl->restartBackend();
}

void openEngineSetup()
{
    if (!alive()) return;
    if (!g_setupDlg) g_setupDlg = new EngineSetupDialog(g_ctrl);
    else g_setupDlg->present();
    g_setupDlg->show();
    if (g_setupDlg->isMinimized()) g_setupDlg->showNormal();
    g_setupDlg->raise();
    g_setupDlg->activateWindow();
}

void checkForUpdates()
{
    if (!alive() || !g_ctx) return;
    // Menu callback / settings button: start the request from the event loop.
    deferCall(g_ctx, 0, [] {
        if (alive() && g_updater) g_updater->checkNow();
    });
}

bool onCommand(uint64_t, const char *command)
{
    if (!alive()) return false;
    const QString text = QString::fromUtf8(command).trimmed();
    if (text.isEmpty() || text.compare(QLatin1String("open"), Qt::CaseInsensitive) == 0) {
        openWindow(true);
        return true;
    }
    if (text.compare(QLatin1String("stop"), Qt::CaseInsensitive) == 0) {
        g_ctrl->stopAll();
        return true;
    }
    if (text.compare(QLatin1String("repeat"), Qt::CaseInsensitive) == 0 ||
        text.compare(QLatin1String("ripeti"), Qt::CaseInsensitive) == 0) {
        g_ctrl->repeatLast();
        return true;
    }
    // Speaking from inside a TS3 command callback: post it, never do work here.
    deferCall(g_ctx, 0, [text] {
        if (alive()) g_ctrl->speak(text);
    });
    return true;
}

void onCurrentServerChanged(uint64_t sch)
{
    if (alive()) g_ctrl->onCurrentServer(sch);
}

void onConnectStatusChange(uint64_t sch, int newStatus, unsigned int)
{
    if (alive()) g_ctrl->onConnectStatus(sch, newStatus);
}

void onHotkey(const char *keyword)
{
    if (!alive() || !g_ctx) return;
    const QByteArray kw(keyword);
    // Defer: hotkey callbacks arrive from the client's input handling.
    deferCall(g_ctx, 0, [kw] {
        if (!alive()) return;
        if (kw == kHotkeyFocus) {
            openWindow(true);
        } else if (kw == kHotkeyStop) {
            g_ctrl->stopAll();
        } else if (kw == kHotkeyRepeat) {
            g_ctrl->repeatLast();
        } else if (kw == kHotkeyToggleWindow) {
            if (g_window && g_window->isVisible()) g_window->hide();
            else openWindow(true);
        } else if (kw == kHotkeyPreview) {
            g_ctrl->setPreviewOnly(!g_ctrl->settings().previewOnly);
        } else if (kw == kHotkeyClipboard) {
            const QString t = QGuiApplication::clipboard()->text().trimmed();
            if (!t.isEmpty()) g_ctrl->speak(t.left(2000));
        }
    });
}

void onTalkStatusChange(uint64_t sch, int status, int isReceivedWhisper, uint16_t clientID)
{
    if (!alive() || isReceivedWhisper || status != 0) return;
    if (ownClientServer(sch, clientID)) g_ctrl->onClientStopsTalking(sch);
}

int onTextMessage(uint64_t sch, uint16_t targetMode, uint16_t fromID, const char *message, int ffIgnored)
{
    // Only the user's OWN messages to the CHANNEL are read aloud: private and server
    // chat never, other people never. Filter here, work later (never inside a callback).
    if (!alive() || !g_ctx || ffIgnored || !message || targetMode != TextMessageTarget_CHANNEL) return 0;
    if (!g_ctrl->settings().readChannelChat || !ts3Functions.getClientID) return 0;
    anyID me = 0;
    if (ts3Functions.getClientID(sch, &me) != ERROR_ok || me != fromID) return 0;
    const QString text = QString::fromUtf8(message);
    deferCall(g_ctx, 0, [sch, text] {
        if (alive()) g_ctrl->onOwnChannelMessage(sch, text);
    });
    return 0;
}

void onCapture(uint64_t sch, short *samples, int frames, int channels, int *edited)
{
    TtsAudio *a = g_audio.load(std::memory_order_acquire);
    if (!a) return;
    // Bit 2 is set by the client when this block will be transmitted (PTT down / voice detected).
    if (a->onCapture(sch, samples, frames, channels, (*edited & 0x2) != 0)) *edited |= 0x1;
}

void onPlayback(uint64_t sch, short *samples, int frames, int channels, const unsigned int *speakers,
                unsigned int *fillMask)
{
    TtsAudio *a = g_audio.load(std::memory_order_acquire);
    if (a) a->onPlayback(sch, samples, frames, channels, speakers, fillMask);
}

} // namespace gbtts
