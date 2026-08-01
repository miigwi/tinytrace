// Tinytrace — firmware for the Adafruit ESP32-S3 Reverse TFT Feather.
//
// STANDALONE: talks directly to a Dynatrace Grail tenant over HTTPS with a
// read-only platform token (provisioned via captive portal into NVS).
// Everything hardware-specific lives behind hal.h (board.cpp).
//
// Display: Adafruit_ST7789 (+ Adafruit_GFX) — TFT_eSPI's init crash-loops here.
//
// Input (three front buttons, see board.cpp):
//   D0  → previous screen
//   D1  → refresh (re-query now)   ·   D1 long-press → idle / mascot
//   D2  → next screen
//
// Screens (built from DQL every 60 s into RAM; buttons switch instantly):
//   0 active problems   1 golden signals   2 last logs   3 idle

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "dt_screens.h"
#include "hal.h"
#include "model.h"
#include "net.h"
#include "render.h"

// TLS handshakes need more stack than the 8 KB default loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// TFT_CS/TFT_DC/TFT_RST come from the board variant. Hardware SPI (SCK 36 / MOSI 35).
static Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);

static Config gCfg;
static Screen screens[DG_MAX_SCREENS];
static int screenCount = 0;
static int current = 0;

static uint32_t lastInput = 0;
static uint32_t lastRefresh = 0;
static bool asleep = false;

static const uint32_t REFRESH_MS = 60000;  // tenant query cadence

// The beacon tracks the worst severity on the CURRENT screen, so it changes as
// you page — green/amber/red, blue when stale.
static void updateBeacon() { beacon(screens[current].sev, screens[current].stale); }

static void show() {
  renderScreen(tft, screens[current], current, screenCount);
  updateBeacon();
}

static void gotoScreen(int index) {
  if (screenCount == 0) return;
  current = (index + screenCount) % screenCount;
  show();
}

static void gotoIdle() {
  for (int i = 0; i < screenCount; i++) {
    if (screens[i].id == "idle") {
      gotoScreen(i);
      return;
    }
  }
}

static void markStale(Screen &s) {
  if (s.valid) {
    s.stale = true;
    s.err = "tenant unreachable";
  }
}

// refreshAll re-queries every screen into RAM and sets the beacon to the worst
// severity seen (blue if anything is stale). A failed query keeps the last good
// screen and dims it. This blocks for the few seconds of TLS/DQL — the buttons
// resume responding between refreshes, which is why the cadence is 60 s and the
// panel is glanceable rather than interactive.
static void refreshAll() {
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

  Screen p, g, l;
  if (buildProblems(gCfg, p)) screens[0] = p; else markStale(screens[0]);
  if (buildGolden(gCfg, g)) screens[1] = g; else markStale(screens[1]);
  if (buildLogs(gCfg, l)) screens[2] = l; else markStale(screens[2]);
  int probs = screens[0].valid ? screens[0].count : 0;
  screens[3] = buildIdle(probs, gCfg.tenant);
  screenCount = 4;

  updateBeacon();  // reflect the current screen after the rebuild
  lastRefresh = millis();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[dg] boot");

  displayPowerOn();  // TFT/backlight power rail — must precede tft.init()
  beaconBegin();
  inputBegin();
  renderInit(tft);

  // Provisioning: NVS config, or the captive portal. Hold D1 at boot to force it.
  Config cfg;
  bool have = configLoad(cfg);
  if (!have || provisioningHeld()) {
    Serial.println(have ? "[dg] reconfigure requested" : "[dg] no config - setup portal");
    beacon(SEV_OK, true);  // blue = setup mode
    renderStatus(tft, "SETUP MODE", "join wifi 'dynaglance-setup'");
    runPortal();  // blocks; saves to NVS and reboots
  }

  renderStatus(tft, "connecting", cfg.ssid.c_str());
  if (!wifiConnect(cfg)) {
    Serial.println("[dg] wifi failed");
    beacon(SEV_ERROR, false);
    renderStatus(tft, "wifi failed", "opening setup...");
    delay(3000);
    runPortal();
  }
  Serial.printf("[dg] wifi ok - ip %s\n", WiFi.localIP().toString().c_str());

  renderStatus(tft, "syncing time", "");
  Serial.println(timeSync() ? "[dg] time synced" : "[dg] time sync failed (continuing)");

  gCfg = cfg;
  renderStatus(tft, "loading", cfg.tenant.c_str());
  refreshAll();
  current = 0;  // start on active problems
  lastInput = millis();
  show();
  Serial.println("[dg] setup complete (live)");
}

void loop() {
  Action a = inputPoll();
  if (a != ACT_NONE) {
    lastInput = millis();
    if (asleep) {  // first press only wakes the panel
      asleep = false;
      backlight(true);
      show();
    } else {
      switch (a) {
        case ACT_PREV: gotoScreen(current - 1); break;
        case ACT_NEXT: gotoScreen(current + 1); break;
        case ACT_REFRESH:
          refreshAll();  // middle button = re-query now
          show();
          break;
        case ACT_IDLE: gotoIdle(); break;
        default: break;
      }
    }
  }

  uint32_t now = millis();
  if (now - lastRefresh > REFRESH_MS) {
    refreshAll();  // keep querying even while asleep, so the beacon stays honest
    if (!asleep) show();
  }

  if (!asleep && DG_SLEEP_MS > 0 && now - lastInput > (uint32_t)DG_SLEEP_MS) {
    asleep = true;
    backlight(false);
  }

  if (!asleep) renderScrollTick(tft, screens[current]);  // marquee long titles

  delay(20);
}
