#pragma once

#include <QColor>
#include <QTimer>
#include <QWidget>

namespace gbtts {

// Painted progress bar (a QSS-styled QProgressBar on the Windows style repaints its
// chunk unreliably - Soundboard updater, v2.3.3). Eases towards the target value;
// setBusy(true) shows a moving stripe when the fraction is unknown.
class ProgressBar : public QWidget
{
public:
    explicit ProgressBar(QWidget *parent = nullptr);

    void setValue(double fraction);        // 0..1
    void snapTo(double fraction);
    void setBusy(bool busy);               // indeterminate stripe on top of the current value
    void setFill(const QColor &color);     // invalid colour = theme accent
    void setMonotonic(bool on) { m_monotonic = on; }   // setValue never goes back (snapTo still can)
    double value() const { return m_target; }

    QSize sizeHint() const override { return {240, 16}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void tick();

    double m_target = 0.0;
    double m_drawn = 0.0;
    double m_phase = 0.0;
    bool   m_busy = false;
    bool   m_monotonic = false;
    QColor m_fill;
    QTimer m_anim;
};

} // namespace gbtts
