// Feather (ESP32-S3 Reverse TFT) implementation of the HAL.
//
// Pin macros (TFT_I2C_POWER, PIN_NEOPIXEL, TFT_BL) come from Adafruit's board
// variant. The D0/D1/D2 mapping below has been verified on hardware: D0 = GPIO0
// (pulled high, reads LOW pressed), D1 = GPIO1 and D2 = GPIO2 (pulled low, read
// HIGH pressed), matching Adafruit's pinout for the Reverse TFT Feather.
#include <Adafruit_MAX1704X.h>
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Wire.h>

#include "hal.h"

// --- board pins (fallbacks so this compiles even if a variant macro is absent)
#ifndef TFT_I2C_POWER
#define TFT_I2C_POWER 7  // powers the TFT panel (and the STEMMA/I2C rail)
#endif
#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 33
#endif
#ifndef NEOPIXEL_POWER
#define NEOPIXEL_POWER 21  // the NeoPixel sits behind its own power gate here
#endif
#ifndef TFT_BL
#define TFT_BL 45
#endif

// The three front buttons. D0 is the BOOT pin: externally pulled high, so it
// reads LOW when pressed. D1/D2 are pulled low, so they read HIGH when pressed.
#define BTN_D0 0
#define BTN_D1 1
#define BTN_D2 2

static Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ------------------------------------------------------------------- power/backlight

void displayPowerOn() {
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  delay(10);  // let the rail settle before the ST7789 reset sequence
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
}

void backlight(bool on) { digitalWrite(TFT_BL, on ? HIGH : LOW); }

// --------------------------------------------------------------------------- input

static bool pressed(int pin) {
  bool level = digitalRead(pin);
  return pin == BTN_D0 ? (level == LOW) : (level == HIGH);
}

static int heldPin = -1;
static uint32_t pressStart = 0;

void inputBegin() {
  pinMode(BTN_D0, INPUT_PULLUP);
  pinMode(BTN_D1, INPUT_PULLDOWN);
  pinMode(BTN_D2, INPUT_PULLDOWN);
}

// inputPoll emits nothing until a button is released, so a long press (jump to
// idle) is distinguishable from a tap. Only one button is tracked at a time;
// simultaneous presses resolve to whichever registered first.
Action inputPoll() {
  int p = pressed(BTN_D0) ? BTN_D0 : pressed(BTN_D1) ? BTN_D1 : pressed(BTN_D2) ? BTN_D2 : -1;
  uint32_t now = millis();

  if (p >= 0 && heldPin < 0) {  // press begins
    heldPin = p;
    pressStart = now;
    return ACT_NONE;
  }
  if (p >= 0 || heldPin < 0) return ACT_NONE;  // still held, or nothing held

  uint32_t held = now - pressStart;  // release
  int which = heldPin;
  heldPin = -1;
  if (held < 30) return ACT_NONE;  // debounce a bounce as a non-event

  // Middle button held long → the idle/mascot screen, mirroring the CYD's
  // long-press gesture.
  if (held >= TT_LONGPRESS_MS && which == BTN_D1) return ACT_IDLE;

  // The panel is mounted rotated 180°, so the button column runs bottom-to-top
  // relative to the silkscreen: D2 sits at the top of the screen and D0 at the
  // bottom. Map them to what they point at, not to what they are labelled.
  switch (which) {
    case BTN_D0: return ACT_NEXT;
    case BTN_D1: return ACT_REFRESH;
    case BTN_D2: return ACT_PREV;
  }
  return ACT_NONE;
}

bool btnDown(uint8_t d) {
  int pin = d == 0 ? BTN_D0 : d == 1 ? BTN_D1 : BTN_D2;
  return pressed(pin);
}

// ------------------------------------------------------------------------- battery

// Below this charge rate (%/hour) the cell is treated as not charging. The
// gauge's rate register is noisy around zero, so a flat threshold beats testing
// for > 0 — otherwise a resting battery flickers between states.
//
// Note the settling delay: Adafruit_MAX17048::begin() issues a reset(), which
// clears the CRATE accumulator, and CRATE is a heavily filtered value. For the
// first few minutes after boot it reads near zero even while the charger is
// visibly running, so the panel shows a normal level and only switches to the
// charging colour once the gauge has converged. Verified on hardware against
// the board's CHG LED.
#ifndef TT_BATT_CHG_RATE
#define TT_BATT_CHG_RATE 0.5f
#endif

static Adafruit_MAX17048 gauge;
static bool gaugeOk = false;

void batteryBegin() {
  Wire.begin();
  gaugeOk = gauge.begin(&Wire);
  Serial.println(gaugeOk ? "[batt] MAX17048 ready" : "[batt] MAX17048 not found");
}

Battery batteryRead() {
  Battery b;
  if (!gaugeOk) {
    gaugeOk = gauge.begin(&Wire);  // rail may have come up late; retry cheaply
    if (!gaugeOk) return b;
  }

  float v = gauge.cellVoltage();
  if (isnan(v)) {  // the library reports NaN when the gauge stops answering
    gaugeOk = false;
    return b;
  }

  b.volts = v;
  float p = gauge.cellPercent();
  b.percent = p < 0 ? 0 : (p > 100 ? 100 : p);
  // chargeRate() returns NaN when the gauge stops answering; NaN fails this
  // comparison, so a bad read reads as "not charging" rather than flickering.
  b.rate = gauge.chargeRate();
  b.charging = b.rate > TT_BATT_CHG_RATE;
  // A LiPo out of this range is not a LiPo — most likely no cell is attached
  // and we are reading the charger driving an open BAT pin.
  b.present = v > 2.6f && v < 4.6f;
  return b;
}

// -------------------------------------------------------------------------- beacon

void beaconBegin() {
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);  // the NeoPixel sits behind its own power gate
  pixel.begin();
  pixel.setBrightness(40);  // bright enough to notice, not to annoy across a desk
  pixel.clear();
  pixel.show();
}

// beacon shows the worst severity on the current screen as a traffic-light:
// green = ok, amber = warn, red = error; blue overrides for stale data.
void beacon(Sev sev, bool stale) {
  uint32_t c;
  if (stale) {
    c = pixel.Color(0, 40, 120);  // blue — data is old
  } else if (sev == SEV_ERROR) {
    c = pixel.Color(200, 0, 0);  // red
  } else if (sev == SEV_WARN) {
    c = pixel.Color(200, 90, 0);  // amber
  } else {
    c = pixel.Color(0, 120, 30);  // green — all clear
  }
  pixel.setPixelColor(0, c);
  pixel.show();
}

void beaconColor(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}
