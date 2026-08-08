// Tinytrace — the Dynatrace desk panel, as a launcher-dispatched app.
//
// Resolves the active connection from Settings (selected WiFi + tenant). If the
// WiFi associates and a tenant is configured, it runs live: builds the screens
// from DQL every 60 s. Otherwise it boots non-connected — a "DEMO MODE" set of
// canned screens — so the panel is useful offline. Provisioning is never
// entered from here; that lives behind the launcher's Settings entry.
//
// Input (three front buttons, see board.cpp):
//   D2  → previous screen (the physically upper button — panel is rotated 180°)
//   D1  → refresh (live only)      ·   D1 long-press → idle / mascot
//   D0  → next screen

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <WiFi.h>

#include "app.h"
#include "config.h"
#include "demo.h"
#include "dt_screens.h"
#include "hal.h"
#include "model.h"
#include "net.h"
#include "render.h"

// The display is owned by main.cpp; this app borrows it through a pointer set at
// the top of tinytraceRun so the free helpers below can reach it.
static Adafruit_ST7789 *gTft = nullptr;

static Config gCfg;
static Screen screens[TT_MAX_SCREENS];
static int screenCount = 0;
static int current = 0;
static bool gLive = false;  // live tenant vs. non-connected demo

static uint32_t lastInput = 0;
static uint32_t lastRefresh = 0;
static bool asleep = false;

static const uint32_t REFRESH_MS = 60000;  // tenant query cadence

static uint32_t lastBatt = 0;
static const uint32_t BATT_MS = 10000;  // fuel-gauge cadence; charge moves slowly

// battPoll reads the gauge and hands the reading to the renderer, which repaints
// the indicator only if the displayed value changed.
static void battPoll() {
  Battery b = batteryRead();
  renderSetBattery(b.present, (int)lroundf(b.percent), b.charging);
  lastBatt = millis();
}

// The beacon tracks the worst severity on the CURRENT screen, so it changes as
// you page — green/amber/red, blue when stale.
static void updateBeacon() { beacon(screens[current].sev, screens[current].stale); }

static void show() {
  renderScreen(*gTft, screens[current], current, screenCount);
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

void tinytraceRun(Adafruit_ST7789 &tft) {
  gTft = &tft;
  Serial.println("[tt] tinytrace start");

  Settings st;
  settingsLoad(st);
  Config act = settingsActive(st);

  bool wifiOk = false;
  if (act.ssid.length()) {
    renderStatus(tft, "connecting", act.ssid.c_str());
    wifiOk = wifiConnect(act);
    if (wifiOk) {
      Serial.printf("[tt] wifi ok - ip %s\n", WiFi.localIP().toString().c_str());
      renderStatus(tft, "syncing time", "");
      Serial.println(timeSync() ? "[tt] time synced" : "[tt] time sync failed (continuing)");
    } else {
      Serial.println("[tt] wifi failed");
    }
  }

  gLive = wifiOk && act.tenant.length() && act.token.length();
  if (gLive) {
    gCfg = act;
    renderStatus(tft, "loading", act.tenant.c_str());
    refreshAll();
    Serial.println("[tt] live");
  } else {
    screenCount = buildDemoScreens(screens, TT_MAX_SCREENS);
    Serial.println("[tt] non-connected - demo mode");
  }

  current = 0;
  lastInput = millis();
  battPoll();  // seed the indicator so the first paint already carries it
  show();

  for (;;) {
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
            if (gLive) refreshAll();  // middle button = re-query now (live only)
            show();
            break;
          case ACT_IDLE: gotoIdle(); break;
          default: break;
        }
      }
    }

    uint32_t now = millis();
    if (gLive && now - lastRefresh > REFRESH_MS) {
      refreshAll();  // keep querying even while asleep, so the beacon stays honest
      if (!asleep) show();
    }

    if (!asleep && TT_SLEEP_MS > 0 && now - lastInput > (uint32_t)TT_SLEEP_MS) {
      asleep = true;
      backlight(false);
    }

    if (now - lastBatt > BATT_MS) battPoll();  // keep polling while asleep too

    if (!asleep) {
      renderBatteryTick(*gTft, screens[current]);          // repaints only on change
      renderScrollTick(*gTft, screens[current]);           // marquee long titles
    }

    delay(20);
  }
}
