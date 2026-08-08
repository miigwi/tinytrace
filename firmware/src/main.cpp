// Boot launcher for the Feather.
//
// One firmware image, several apps. On every reset this menu comes up; the
// three front buttons drive it everywhere: D2 = up, D0 = down, D1 = select.
// (The panel is mounted rotated 180°, so D2 is the physically upper button.)
//
//   TINYTRACE      the Dynatrace desk panel (live if configured, else demo)
//   TRACE RUNNER   a one-button noir endless runner (offline)
//   SETTINGS       → Config Portal (captive portal) / Reset Settings / Back
//
// SETTINGS → Config Portal opens the captive portal to manage WiFi networks
// and Dynatrace tenants. SETTINGS → Reset Settings wipes everything back to a
// non-connected state. Neither TINYTRACE nor TRACE RUNNER returns; SETTINGS
// returns here on Back.
//
// The apps are separate compiled modules; a runtime chooser needs them all
// linked into one binary, which is why this is a launcher and not several envs.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>

#include "app.h"
#include "config.h"
#include "hal.h"
#include "render.h"

// TLS handshakes (tinytrace) need more stack than the 8 KB default loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// TFT_CS/TFT_DC/TFT_RST come from the board variant. Hardware SPI (SCK 36 / MOSI 35).
static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

// palette (RGB565)
static const uint16_t CL_BG = 0x0000, CL_BLUE = 0x5D9F, CL_DIM = 0x8410, CL_TEAL = 0x2E8B,
                      CL_WHITE = 0xFFFF, CL_PANEL = 0x18E3, CL_RED = 0xF986;

// Draw a highlighted list with a title, a bottom description line for the
// selected item, and the control hint. Generic over item count.
static void drawList(const char *title, const char *sub, const char *const *items,
                     const char *const *blurbs, int n, int sel) {
  const int w = tft.width(), h = tft.height();
  tft.fillScreen(CL_BG);
  tft.setTextWrap(false);

  tft.setTextSize(2);
  tft.setTextColor(CL_BLUE);
  tft.setCursor(w / 2 - (int)strlen(title) * 6, 6);
  tft.print(title);
  if (sub && *sub) {
    tft.setTextSize(1);
    tft.setTextColor(CL_DIM);
    tft.setCursor(w / 2 - (int)strlen(sub) * 3, 24);
    tft.print(sub);
  }

  const int top = 42, rowH = 22;
  for (int i = 0; i < n; i++) {
    int y = top + i * rowH;
    bool on = i == sel;
    if (on) {
      tft.fillRect(6, y - 2, w - 12, rowH - 3, CL_PANEL);
      tft.drawRect(6, y - 2, w - 12, rowH - 3, CL_TEAL);
    }
    tft.setTextSize(2);
    tft.setTextColor(on ? CL_WHITE : CL_DIM);
    tft.setCursor(12, y);
    tft.print(on ? ">" : " ");
    tft.setCursor(28, y);
    tft.print(items[i]);
  }

  tft.setTextSize(1);
  if (blurbs && blurbs[sel]) {
    tft.setTextColor(CL_TEAL);
    tft.setCursor(w / 2 - (int)strlen(blurbs[sel]) * 3, top + n * rowH + 2);
    tft.print(blurbs[sel]);
  }
  tft.setTextColor(CL_DIM);
  const char *hint = "D2 up  D0 down  D1 select";
  tft.setCursor(w / 2 - (int)strlen(hint) * 3, h - 10);
  tft.print(hint);
}

// Run a menu to completion; returns the selected index (D1 confirms).
static int runMenu(const char *title, const char *sub, const char *const *items,
                   const char *const *blurbs, int n) {
  int sel = 0;
  drawList(title, sub, items, blurbs, n, sel);
  for (;;) {
    Action a = inputPoll();
    if (a == ACT_PREV) {
      sel = (sel - 1 + n) % n;
      drawList(title, sub, items, blurbs, n, sel);
    } else if (a == ACT_NEXT) {
      sel = (sel + 1) % n;
      drawList(title, sub, items, blurbs, n, sel);
    } else if (a == ACT_REFRESH || a == ACT_IDLE) {  // D1 (tap or long-press)
      return sel;
    }
    delay(15);
  }
}

static void waitRelease() {
  while (btnDown(0) || btnDown(1) || btnDown(2)) delay(10);
}

// A yes/no prompt: D1 = yes, D0/D2 = no.
static bool confirm(const char *l1, const char *l2) {
  const int w = tft.width(), h = tft.height();
  tft.fillScreen(CL_BG);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(CL_RED);
  tft.setCursor(w / 2 - (int)strlen(l1) * 6, 30);
  tft.print(l1);
  tft.setTextSize(1);
  tft.setTextColor(CL_DIM);
  tft.setCursor(w / 2 - (int)strlen(l2) * 3, 58);
  tft.print(l2);
  tft.setTextColor(CL_WHITE);
  const char *hint = "D1 = yes    D0/D2 = no";
  tft.setCursor(w / 2 - (int)strlen(hint) * 3, h - 24);
  tft.print(hint);
  for (;;) {
    Action a = inputPoll();
    if (a == ACT_REFRESH || a == ACT_IDLE) return true;   // D1
    if (a == ACT_PREV || a == ACT_NEXT) return false;     // D0 / D2
    delay(15);
  }
}

// The Settings submenu. Returns when the user picks Back.
static void settingsFlow() {
  static const char *items[] = {"Config Portal", "Reset Settings", "Back"};
  static const char *blurbs[] = {"WiFi + Dynatrace setup", "erase all networks/tenants",
                                 "return to launcher"};
  for (;;) {
    int c = runMenu("SETTINGS", "", items, blurbs, 3);
    waitRelease();
    if (c == 0) {  // captive portal — blocks and reboots on Apply
      beacon(SEV_OK, true);  // blue = setup mode
      renderStatus(tft, "CONFIG PORTAL", "join wifi 'tinytrace-setup'");
      runPortal();
    } else if (c == 1) {  // reset to non-connected state
      if (confirm("RESET?", "erase all networks & tenants")) {
        settingsReset();
        renderStatus(tft, "RESET DONE", "restarting...");
        delay(1200);
        ESP.restart();
      }
    } else {
      return;  // Back
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[launcher] boot");

  displayPowerOn();  // TFT/backlight power rail — must precede tft.init()
  beaconBegin();
  inputBegin();
  batteryBegin();  // fuel gauge shares the rail displayPowerOn() just enabled
  renderInit(tft);  // init(135,240) + rotation + clear

  static const char *items[] = {"TINYTRACE", "TRACE RUNNER", "SETTINGS"};
  static const char *blurbs[] = {"dynatrace desk panel", "one-button endless runner",
                                 "configure / reset"};
  for (;;) {
    int c = runMenu("MENU", "select an app", items, blurbs, 3);
    waitRelease();  // don't let the selecting press leak into the app
    if (c == 0) tinytraceRun(tft);       // never returns
    else if (c == 1) tracerunnerRun(tft);  // never returns
    else settingsFlow();                 // returns on Back → re-show launcher
  }
}

void loop() {}
