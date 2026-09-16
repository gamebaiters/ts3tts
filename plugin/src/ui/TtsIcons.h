#pragma once

#include <QColor>
#include <QIcon>

// Painted icons (no SVG module: TeamSpeak does not ship Qt5Svg).
namespace gbtts::TtsIcons {

QIcon app();                                           // speech bubble + sound waves
QIcon voices(const QColor &c = QColor(0xDC, 0xDC, 0xDC));   // head silhouette + waves
QIcon settings(const QColor &c = QColor(0xDC, 0xDC, 0xDC)); // gear
QIcon send(const QColor &c = QColor(0x57, 0xC4, 0x5E));     // paper plane

} // namespace gbtts::TtsIcons
