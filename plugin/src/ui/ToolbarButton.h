#pragma once

class QWidget;

// Checkable TTS button overlaid on the TeamSpeak main toolbar - the Soundboard's
// proven v3 overlay technique (a plain child widget positioned with move(),
// never an action in the host toolbar's layout, host properties read and never
// written). Additionally aware of OTHER plugin overlays (the Soundboard's own
// button sits in the same spot): it places itself to their right.
namespace gbtts::ToolbarButton {

void install();
void setUserEnabled(bool on);
void watchWindow(QWidget *w);
void remove();          // mandatory in shutdown, before the DLL unloads

} // namespace gbtts::ToolbarButton
