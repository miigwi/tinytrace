// Hardware abstraction for the Feather: input, the NeoPixel beacon, and the
// display power rail. Everything board-specific lives behind these calls so the
// app loop in main.cpp reads the same on either form factor — the CYD resolves
// a touch into an Action, the Feather resolves three buttons into the same set.
#pragma once

#include "model.h"

// The four gestures the app loop understands, identical to the CYD's touch
// zones. WAKE is synthesized by main.cpp from "any action while asleep".
enum Action : uint8_t {
  ACT_NONE = 0,
  ACT_PREV,
  ACT_NEXT,
  ACT_REFRESH,
  ACT_IDLE,
};

// Input: three tactile buttons (D0/D1/D2). Poll once per loop; an action is
// emitted on release so a long press can be told from a tap.
void inputBegin();
Action inputPoll();

// provisioningHeld reports whether the middle button (D1) is held right now.
// Checked once at boot to force the captive portal even when config exists —
// the escape hatch for wrong WiFi/tenant/token.
bool provisioningHeld();

// Beacon: the single onboard NeoPixel, the "should you look up" light. Off when
// there is nothing to say — the panel is the detail.
void beaconBegin();
void beacon(Sev sev, bool stale);

// Display power: the Reverse TFT's panel (and NeoPixel) sit behind a power rail
// that must be driven high before TFT_eSPI can talk to the ST7789.
void displayPowerOn();
void backlight(bool on);
