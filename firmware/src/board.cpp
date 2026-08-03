// Feather (ESP32-S3 Reverse TFT) implementation of the HAL.
//
// ⚠ Pin macros (TFT_I2C_POWER, PIN_NEOPIXEL, the D0/D1/D2 GPIOs, and their
// physical left-to-right order) come from Adafruit's board variant and have NOT
// yet been checked against a board in hand — same honesty as the CYD's touch
// calibration. If a button feels swapped, adjust the mapping in inputPoll.
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

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
  if (held >= DG_LONGPRESS_MS && which == BTN_D1) return ACT_IDLE;

  switch (which) {
    case BTN_D0: return ACT_PREV;
    case BTN_D1: return ACT_REFRESH;
    case BTN_D2: return ACT_NEXT;
  }
  return ACT_NONE;
}

bool btnDown(uint8_t d) {
  int pin = d == 0 ? BTN_D0 : d == 1 ? BTN_D1 : BTN_D2;
  return pressed(pin);
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
