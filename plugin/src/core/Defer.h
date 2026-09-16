#pragma once

#include <QObject>
#include <QTimer>

#include <utility>

namespace gbtts {

// Deferred call that can NEVER outlive the plugin DLL.
//
// QTimer::singleShot(ms, context, lambda) is NOT safe in a plugin, even with a
// plugin-owned context: Qt 5.15 parents the internal QSingleShotTimer to the
// HOST's event dispatcher and keeps the context only as a QPointer. Deleting
// the context skips the call, but the timer stays queued; when it fires, its
// destructor runs slotObj->destroyIfLastRef(), i.e. the lambda's impl function
// compiled into this DLL - after FreeLibrary that is a jump into unmapped
// memory (reproduced by tests/host_harness.cpp: execute AV under
// QSingleShotTimer::timerEvent, no plugin module on the stack).
//
// Here the timer is a CHILD of the context: deleting the context (done in
// ts3plugin_shutdown, before the unload) kills the timer and destroys the
// connection's slot object while the DLL is still mapped.
template <typename Fn>
void deferCall(QObject *context, int msec, Fn &&fn)
{
    if (!context) return;
    auto *timer = new QTimer(context);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, f = std::forward<Fn>(fn)]() mutable {
        timer->deleteLater();
        f();
    });
    timer->start(msec);
}

} // namespace gbtts
