// Entry points for the two apps the boot launcher can start. Both take the
// already-initialised display (power rail, ST7789 init, rotation and the
// NeoPixel/buttons are all brought up once in main.cpp before either runs), and
// neither returns — the chosen app owns the device until the next reset.
#pragma once

#include <Adafruit_ST7789.h>

// The Dynatrace desk panel. Runs live if the selected WiFi + tenant connect,
// otherwise boots non-connected in demo mode. Provisioning lives in the
// launcher's Settings entry, not here.
void tinytraceRun(Adafruit_ST7789 &tft);

// Trace Runner — the one-button noir endless runner. D1 = jump (hold = higher).
void tracerunnerRun(Adafruit_ST7789 &tft);
