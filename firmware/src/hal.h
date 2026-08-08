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

// btnDown reports the instantaneous state of one button (d = 0/1/2 → D0/D1/D2),
// bypassing inputPoll's release-latching. The launcher menu and the game need
// the raw held state (e.g. hold-to-jump-higher), not tap-on-release actions.
bool btnDown(uint8_t d);

// Battery: the onboard MAX17048 fuel gauge (I2C 0x36). This board has no analog
// VBAT divider, so the gauge is the only source of charge state.
//
// Two honest limits, both hardware:
//   · The charger's CHG LED is not wired to a GPIO, so "charging" is inferred
//     from the gauge's signed charge-rate register (%/hour). A full battery on
//     USB settles to ~0 %/h and so reads as not charging — which is true, but
//     it is not the same as "unplugged".
//   · With no cell attached, the charger drives the BAT pin to ~4.2 V, so the
//     gauge reports a plausible full battery. Absent and full are genuinely
//     indistinguishable in software; `present` is a sanity check, not proof.
struct Battery {
  bool present = false;   // gauge answered with a plausible cell voltage
  bool charging = false;  // charge rate is meaningfully positive
  float percent = 0;      // 0..100 state of charge
  float volts = 0;
  float rate = 0;  // %/hour, signed: >0 filling, <0 draining
};

// batteryBegin must run after displayPowerOn() — the I2C rail shares the
// TFT_I2C_POWER gate. Safe to call when no gauge is present; reads then report
// present=false and batteryRead retries the probe cheaply.
void batteryBegin();
Battery batteryRead();

// Beacon: the single onboard NeoPixel, the severity light — green when all is
// clear, amber for warn, red for error, blue when the data is stale.
//
// It deliberately stays lit on all-clear rather than going dark. Green is a
// positive "device is alive and things are fine" signal, and it is cheap:
// measured against the panel's idle draw it is ~3 mA of ~65 mA, about 4.5%, or
// roughly 45 minutes of an 18-hour battery life. Not worth trading the signal
// for. (This comment previously claimed the beacon went off when there was
// nothing to say, which the code never did.)
void beaconBegin();
void beacon(Sev sev, bool stale);

// beaconColor drives the NeoPixel to an arbitrary colour — used by the game as
// an SLO light (green→amber→red) and a red flash on a hit.
void beaconColor(uint8_t r, uint8_t g, uint8_t b);

// Display power: the Reverse TFT's panel (and NeoPixel) sit behind a power rail
// that must be driven high before TFT_eSPI can talk to the ST7789.
void displayPowerOn();
void backlight(bool on);
