// Entry points for the two apps the boot launcher can start. Both take the
// already-initialised display (power rail, ST7789 init, rotation and the
// NeoPixel/buttons are all brought up once in main.cpp before either runs), and
// neither returns — the chosen app owns the device until the next reset.
#pragma once

#include <Adafruit_ST7789.h>

// The Dynatrace desk panel. forcePortal jumps straight into captive-portal
// provisioning even when a config exists (the launcher sets it when D0 is held
// at selection time).
void tinytraceRun(Adafruit_ST7789 &tft, bool forcePortal);

// Trace Runner — the one-button noir endless runner. D1 = jump (hold = higher).
void tracerunnerRun(Adafruit_ST7789 &tft);
