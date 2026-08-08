// settings_ui.h -- on-device settings shade, pulled down by swiping from the top edge.
//
// A full-screen touch UI that taskDisplay renders as a display mode (the same pattern as the
// timer/alarm alert): while open it overlays whatever is on the panel and reads the GT911
// touch point to drive its controls. ONE implementation for both LCD boards (shared codebase
// + touch stack). Opened by touchSwipeDown() (touch.cpp); closed by the Done handle.
#pragma once
#include <stdint.h>

bool settingsActive();     // the shade is open (taskDisplay checks this in its dispatch)
void settingsOpen();       // open it (taskDisplay: on a top-edge down-swipe)
void settingsClose();      // close it and hand the panel back to the wall
bool settingsRender();     // taskDisplay: draw one frame + service touch; true = still open
