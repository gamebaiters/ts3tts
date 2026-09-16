#include "ui/ProgressBar.h"

#include "modules/theme.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace gbtts {

ProgressBar::ProgressBar(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(16);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_anim.setInterval(16);
    connect(&m_anim, &QTimer::timeout, this, [this] { tick(); });
}

void ProgressBar::setValue(double fraction)
{
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (m_monotonic && fraction < m_target) return;
    if (std::fabs(fraction - m_target) < 1e-6) return;
    m_target = fraction;
    if (!m_anim.isActive() && isVisible()) m_anim.start();
    else if (!isVisible()) m_drawn = m_target;
    update();
}

void ProgressBar::snapTo(double fraction)
{
    m_target = m_drawn = std::clamp(fraction, 0.0, 1.0);
    if (!m_busy) m_anim.stop();
    update();
}

void ProgressBar::setBusy(bool busy)
{
    if (busy == m_busy) return;
    m_busy = busy;
    if (busy) m_anim.start();
    update();
}

void ProgressBar::setFill(const QColor &color)
{
    m_fill = color;
    update();
}

void ProgressBar::tick()
{
    const double diff = m_target - m_drawn;
    if (std::fabs(diff) < 1e-4) m_drawn = m_target;
    else m_drawn += diff * 0.2;
    if (m_busy) m_phase = std::fmod(m_phase + 0.012, 1.0);
    if (!m_busy && m_drawn == m_target) m_anim.stop();
    update();
}

void ProgressBar::paintEvent(QPaintEvent *)
{
    const Theme::Derived &d = Theme::derivedCached();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);
    QPainterPath track;
    track.addRoundedRect(r, r.height() / 2, r.height() / 2);
    p.fillPath(track, d.border.darker(160));
    p.setPen(QPen(d.border, 1.0));
    p.drawPath(track);
    p.save();
    p.setClipPath(track);
    const QColor fill = m_fill.isValid() ? m_fill : d.accent;
    if (m_drawn > 0.0) p.fillRect(QRectF(r.left(), r.top(), r.width() * m_drawn, r.height()), fill);
    if (m_busy) {
        // A soft highlight sweeping across the bar: work is going on, amount unknown.
        const double w = r.width() * 0.25;
        const double x = r.left() - w + (r.width() + w) * m_phase;
        QLinearGradient g(x, 0, x + w, 0);
        QColor c = fill.lighter(150);
        c.setAlpha(0);
        g.setColorAt(0.0, c);
        c.setAlpha(150);
        g.setColorAt(0.5, c);
        c.setAlpha(0);
        g.setColorAt(1.0, c);
        p.fillRect(QRectF(x, r.top(), w, r.height()), g);
    }
    p.restore();
}

} // namespace gbtts
