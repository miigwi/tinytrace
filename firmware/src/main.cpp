// Boot launcher for the Feather.
//
// One firmware image, two apps. On every reset this menu comes up; the three
// front buttons pick which app owns the device until the next reset:
//   D0 → move up   ·   D2 → move down   ·   D1 → select
//
//   0  TINYTRACE      the Dynatrace desk panel (WiFi + Grail tenant)
//   1  TRACE RUNNER   a one-button noir endless runner (offline)
//   2  WIFI SETUP     re-enter just the WiFi network + password
//   3  DYNATRACE      re-enter just the tenant URL + platform token
//
// The two setup entries open the captive portal scoped to their half only,
// preserving the other fields. Hold D0 while selecting TINYTRACE to force the
// full (all-fields) portal.
//
// The two apps are separate compiled modules (tinytrace_app.cpp,
// game_tracerunner.cpp); a runtime chooser needs both linked into one binary,
// which is why this is a launcher and not two PlatformIO envs.

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

static const char *ITEMS[] = {"TINYTRACE", "TRACE RUNNER", "WIFI SETUP", "DYNATRACE SETUP"};
static const char *BLURB[] = {"dynatrace desk panel", "one-button endless runner",
                              "change wifi network", "change tenant + token"};
static const int NITEMS = 4;

// Draw the menu with the given item highlighted. Kept deliberately plain — GFX
// text, no dependency on the tinytrace renderer's private helpers.
static void drawMenu(int sel) {
  const int w = tft.width(), h = tft.height();
  tft.fillScreen(0x0000);
  tft.setTextWrap(false);

  // title
  tft.setTextSize(2);
  tft.setTextColor(0x5D9F);  // blue
  const char *title = "TINYTRACER";
  tft.setCursor(w / 2 - (int)strlen(title) * 6, 6);
  tft.print(title);
  tft.setTextSize(1);
  tft.setTextColor(0x8410);
  const char *sub = "select an app";
  tft.setCursor(w / 2 - (int)strlen(sub) * 3, 24);
  tft.print(sub);

  const int top = 40, rowH = 19;
  for (int i = 0; i < NITEMS; i++) {
    int y = top + i * rowH;
    bool on = i == sel;
    if (on) {
      tft.fillRect(6, y - 2, w - 12, rowH - 2, 0x18E3);  // dim panel
      tft.drawRect(6, y - 2, w - 12, rowH - 2, 0x2E8B);  // teal edge
    }
    tft.setTextSize(2);
    tft.setTextColor(on ? 0xFFFF : 0x8410);
    tft.setCursor(12, y);
    tft.print(on ? ">" : " ");
    tft.setCursor(28, y);
    tft.print(ITEMS[i]);
  }

  // description of the highlighted item, then the control hint
  tft.setTextSize(1);
  tft.setTextColor(0x2E8B);
  tft.setCursor(w / 2 - (int)strlen(BLURB[sel]) * 3, top + NITEMS * rowH + 2);
  tft.print(BLURB[sel]);
  tft.setTextColor(0x8410);
  const char *hint = "D0 up  D2 down  D1 select";
  tft.setCursor(w / 2 - (int)strlen(hint) * 3, h - 10);
  tft.print(hint);
}

// Block on the menu until D1 selects an item; returns the chosen index.
static int launcherMenu() {
  int sel = 0;
  drawMenu(sel);
  for (;;) {
    Action a = inputPoll();
    if (a == ACT_PREV) {  // D0
      sel = (sel - 1 + NITEMS) % NITEMS;
      drawMenu(sel);
    } else if (a == ACT_NEXT) {  // D2
      sel = (sel + 1) % NITEMS;
      drawMenu(sel);
    } else if (a == ACT_REFRESH || a == ACT_IDLE) {  // D1 (tap or long-press)
      return sel;
    }
    delay(15);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[launcher] boot");

  displayPowerOn();  // TFT/backlight power rail — must precede tft.init()
  beaconBegin();
  inputBegin();
  renderInit(tft);  // init(135,240) + rotation + clear

  int choice = launcherMenu();
  bool forcePortal = btnDown(0);  // hold D0 while selecting → full tinytrace setup

  // Wait for every button to be released so the selecting press doesn't leak
  // into the app (e.g. tinytrace reading D1 as a fresh provisioning hold, or the
  // game treating it as an immediate jump).
  while (btnDown(0) || btnDown(1) || btnDown(2)) delay(10);

  switch (choice) {
    case 1:
      tracerunnerRun(tft);
      break;
    case 2:  // WiFi-only captive portal
      beacon(SEV_OK, true);  // blue = setup mode
      renderStatus(tft, "WIFI SETUP", "join wifi 'dynaglance-setup'");
      runPortal(PORTAL_WIFI);  // saves + reboots
      break;
    case 3:  // Dynatrace-only captive portal
      beacon(SEV_OK, true);
      renderStatus(tft, "DYNATRACE", "join wifi 'dynaglance-setup'");
      runPortal(PORTAL_DT);  // saves + reboots
      break;
    default:
      tinytraceRun(tft, forcePortal);
      break;
  }
  // Nothing here returns.
}

void loop() {}
