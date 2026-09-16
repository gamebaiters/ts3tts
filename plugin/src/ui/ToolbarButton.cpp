#include "ui/ToolbarButton.h"

#include "gbtts.h"
#include "ts3log.h"
#include "ui/TtsIcons.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>
#include "core/Defer.h"

#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <algorithm>

namespace gbtts::ToolbarButton {

namespace {

QPointer<QToolBar>    s_toolbar;
QPointer<QToolButton> s_btn;
QPointer<QWidget>     s_window;
QObject              *s_ctx = nullptr;
int                   s_attempts = 0;
bool                  s_userEnabled = true;
bool                  s_pending = false;

const char *kObjectName = "gbTtsToolbarButton";

void tryInstall();

class WindowWatcher : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (obj == s_window && s_btn) {
            if (ev->type() == QEvent::Show) {
                QSignalBlocker b(s_btn);
                s_btn->setChecked(true);
            } else if (ev->type() == QEvent::Hide || ev->type() == QEvent::Close) {
                QSignalBlocker b(s_btn);
                s_btn->setChecked(false);
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
WindowWatcher *s_winWatcher = nullptr;

void reposition()
{
    if (!s_btn || !s_toolbar) return;
    QToolBar *tb = s_toolbar;
    const int W = tb->width();
    const int H = tb->height();
    if (W <= 0 || H <= 0) return;

    int rightEdge = -1;
    QRect lastRect;
    const auto acts = tb->actions();
    for (QAction *a : acts) {
        if (!a->isVisible()) continue;
        const QRect r = tb->actionGeometry(a);
        if (!r.isValid() || r.width() <= 0) continue;
        const bool buttonLike = r.width() <= H * 2 + 8;
        const bool contiguous = (rightEdge < 0) || (r.x() <= rightEdge + 24);
        if (!buttonLike || !contiguous || r.x() > W / 2) break;
        if (r.right() > rightEdge) {
            rightEdge = r.right();
            lastRect = r;
        }
    }
    if (rightEdge < 0) rightEdge = 2;

    // Other plugins' overlays (e.g. the GameBaiters Soundboard button) are
    // direct QToolButton children that belong to no action: sit after them.
    QSet<QWidget *> actionWidgets;
    for (QAction *a : acts)
        if (QWidget *w = tb->widgetForAction(a)) actionWidgets.insert(w);
    const auto children = tb->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QToolButton *other : children) {
        if (other == s_btn || !other->isVisible() || actionWidgets.contains(other)) continue;
        if (other->objectName() == QLatin1String("qt_toolbar_ext_button")) continue;
        const QRect g = other->geometry();
        if (g.x() >= rightEdge - 2 && g.x() <= rightEdge + 48 && g.right() > rightEdge) {
            rightEdge = g.right();
            if (!lastRect.isValid()) lastRect = g;
        }
    }

    const QSize btnSize = lastRect.isValid() ? lastRect.size() : QSize(std::min(H - 4, 24), std::min(H - 4, 24));
    QSize icoSize = tb->iconSize();
    if (!icoSize.isValid() || icoSize.width() <= 0) icoSize = QSize(btnSize.width() - 4, btnSize.height() - 4);
    icoSize = icoSize.boundedTo(btnSize - QSize(2, 2));

    int rightClusterStart = W;
    for (QAction *a : acts) {
        if (!a->isVisible()) continue;
        const QRect r = tb->actionGeometry(a);
        if (r.isValid() && r.width() > 0 && r.x() > W / 2) rightClusterStart = std::min(rightClusterStart, r.x());
    }
    const int x = rightEdge + 4;
    if (x + btnSize.width() > rightClusterStart || x + btnSize.width() > W) {
        s_btn->hide();
        return;
    }
    s_btn->setFixedSize(btnSize);
    s_btn->setIconSize(icoSize);
    s_btn->move(x, lastRect.isValid() ? lastRect.y() : (H - btnSize.height()) / 2);
    s_btn->raise();
    s_btn->show();
}

void scheduleReposition()
{
    if (s_pending || !s_ctx) return;
    s_pending = true;
    // Two passes: the Soundboard overlay repositions on the same events, and
    // whichever runs second must see the other's final geometry.
    // deferCall, not QTimer::singleShot: a queued singleShot functor outlives
    // s_ctx and jumps into this DLL after the unload (see core/Defer.h).
    deferCall(s_ctx, 0, [] {
        reposition();
        deferCall(s_ctx, 60, [] {
            s_pending = false;
            reposition();
        });
    });
}

class ToolbarWatcher : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (obj == s_toolbar) {
            switch (ev->type()) {
            case QEvent::Resize:
            case QEvent::LayoutRequest:
            case QEvent::Show:
            case QEvent::StyleChange:
            case QEvent::ActionAdded:
            case QEvent::ActionRemoved:
            case QEvent::ActionChanged:
            case QEvent::ChildAdded:
            case QEvent::ChildRemoved:
                scheduleReposition();
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
ToolbarWatcher *s_tbWatcher = nullptr;

QToolBar *findHostToolbar()
{
    QToolBar *best = nullptr;
    int bestScore = -1;
    for (QWidget *w : qApp->topLevelWidgets()) {
        auto *mw = qobject_cast<QMainWindow *>(w);
        if (!mw) continue;
        for (QToolBar *tb : mw->findChildren<QToolBar *>()) {
            const int score = tb->actions().size() + (tb->isVisible() ? 100 : 0);
            if (score > bestScore) {
                bestScore = score;
                best = tb;
            }
        }
    }
    return best;
}

void destroyButton()
{
    if (s_toolbar && s_tbWatcher) s_toolbar->removeEventFilter(s_tbWatcher);
    if (s_btn) {
        s_btn->hide();
        delete s_btn.data();
    }
    s_btn.clear();
    s_toolbar.clear();
}

void tryInstall()
{
    if (!s_userEnabled || s_btn) return;
    QToolBar *tb = findHostToolbar();
    if (!tb) {
        if (++s_attempts <= 8 && s_ctx) deferCall(s_ctx, 1500, [] { tryInstall(); });
        return;
    }
    auto *btn = new QToolButton(tb);
    btn->setObjectName(QString::fromLatin1(kObjectName));
    btn->setIcon(TtsIcons::app());
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setCheckable(true);
    btn->setAutoRaise(true);
    btn->setToolTip(QObject::tr("GameBaiters TTS - show / hide (plugin)"));
    QObject::connect(btn, &QToolButton::toggled, s_ctx, [](bool on) {
        if (on) openWindow(true);
        else if (s_window) s_window->hide();
    });
    s_toolbar = tb;
    s_btn = btn;
    tb->removeEventFilter(s_tbWatcher);
    tb->installEventFilter(s_tbWatcher);
    if (s_window && s_window->isVisible()) {
        QSignalBlocker b(btn);
        btn->setChecked(true);
    }
    scheduleReposition();
    logInfo("GBTTS: toolbar overlay installed on '%s'", tb->objectName().toUtf8().constData());
}

} // namespace

void install()
{
    if (!qApp) return;
    if (!s_ctx) {
        s_ctx = new QObject();
        s_winWatcher = new WindowWatcher(s_ctx);
        s_tbWatcher = new ToolbarWatcher(s_ctx);
    }
    s_attempts = 0;
    tryInstall();
}

void setUserEnabled(bool on)
{
    s_userEnabled = on;
    if (!on) destroyButton();
    else install();
}

void watchWindow(QWidget *w)
{
    if (!w) return;
    if (!s_ctx) {
        s_ctx = new QObject();
        s_winWatcher = new WindowWatcher(s_ctx);
        s_tbWatcher = new ToolbarWatcher(s_ctx);
    }
    if (s_window == w) return;
    if (s_window) s_window->removeEventFilter(s_winWatcher);
    s_window = w;
    w->installEventFilter(s_winWatcher);
    if (s_btn) {
        QSignalBlocker b(s_btn);
        s_btn->setChecked(w->isVisible());
    }
}

void remove()
{
    if (s_window && s_winWatcher) s_window->removeEventFilter(s_winWatcher);
    s_window.clear();
    destroyButton();
    delete s_ctx;
    s_ctx = nullptr;
    s_winWatcher = nullptr;
    s_tbWatcher = nullptr;
}

} // namespace gbtts::ToolbarButton
