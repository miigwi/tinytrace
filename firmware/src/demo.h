// Canned, network-free screens for non-connected (demo) mode: when no WiFi is
// selected or the tenant can't be reached, the panel still shows a plausible
// active-problems / golden-signals / last-logs / idle set marked "DEMO MODE",
// so the device is useful offline and obviously not live.
#pragma once

#include "model.h"

// Fills out[0..3] with the four demo screens; returns the count (4). cap guards
// the array size.
int buildDemoScreens(Screen out[], int cap);
