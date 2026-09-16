#include "ui/TtsIcons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>
#include <functional>

namespace gbtts::TtsIcons {

namespace {

constexpr int kCanvas = 64;

QIcon fromPainter(const std::function<void(QPainter &)> &paint)
{
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / double(kCanvas), size / double(kCanvas));
        paint(p);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace

QIcon app()
{
    return fromPainter([](QPainter &p) {
        // Dark tile, same family as the Soundboard toolbar glyph.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x22, 0x25, 0x2B));
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);

        // Speech bubble.
        QPainterPath bubble;
        bubble.addRoundedRect(QRectF(9, 12, 30, 24), 8, 8);
        QPainterPath tail;
        tail.moveTo(15, 34);
        tail.lineTo(12, 46);
        tail.lineTo(25, 35);
        tail.closeSubpath();
        p.setBrush(QColor(0x4A, 0x90, 0xE2));
        p.drawPath(bubble.united(tail));

        // Text lines inside the bubble.
        p.setBrush(QColor(0xEA, 0xF2, 0xFC));
        p.drawRoundedRect(QRectF(15, 19, 18, 3.5), 1.5, 1.5);
        p.drawRoundedRect(QRectF(15, 26, 12, 3.5), 1.5, 1.5);

        // Sound waves.
        QPen pen(QColor(0x57, 0xC4, 0x5E), 4.0, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(30, 16, 16, 30), -50 * 16, 100 * 16);
        pen.setColor(QColor(0xE6, 0xA8, 0x3C));
        p.setPen(pen);
        p.drawArc(QRectF(30, 9, 26, 44), -50 * 16, 100 * 16);
    });
}

QIcon voices(const QColor &c)
{
    return fromPainter([c](QPainter &p) {
        QPen pen(c, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(10, 10, 20, 22));
        QPainterPath body;
        body.moveTo(6, 56);
        body.cubicTo(6, 40, 34, 40, 34, 56);
        p.drawPath(body);
        p.drawArc(QRectF(34, 16, 12, 22), -60 * 16, 120 * 16);
        p.drawArc(QRectF(36, 8, 22, 38), -60 * 16, 120 * 16);
    });
}

QIcon settings(const QColor &c)
{
    return fromPainter([c](QPainter &p) {
        p.translate(32, 32);
        QPainterPath gear;
        constexpr int teeth = 8;
        for (int i = 0; i < teeth; ++i) {
            p.save();
            p.rotate(i * 360.0 / teeth);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(-5, -27, 10, 12), 2, 2);
            p.restore();
        }
        p.setPen(QPen(c, 6.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(0, 0), 16, 16);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(0, 0), 5, 5);
    });
}

QIcon send(const QColor &c)
{
    return fromPainter([c](QPainter &p) {
        QPainterPath plane;
        plane.moveTo(6, 30);
        plane.lineTo(58, 8);
        plane.lineTo(44, 56);
        plane.lineTo(30, 38);
        plane.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(plane);
        p.setPen(QPen(QColor(0x22, 0x25, 0x2B), 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(30, 38), QPointF(58, 8));
    });
}

} // namespace gbtts::TtsIcons
