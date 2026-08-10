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

  // 135 px tall leaves room for the title, three rows, the blurb and the hint —
  // no more. Anything longer scrolls a window that follows the selection, rather
  // than drawing rows off the bottom edge where they simply vanish.
  const int top = 42, rowH = 22, maxVis = 3;
  int first = 0;
  if (n > maxVis) {
    if (sel >= maxVis) first = sel - maxVis + 1;
    if (first > n - maxVis) first = n - maxVis;
  }
  const int vis = n < maxVis ? n : maxVis;

  for (int k = 0; k < vis; k++) {
    const int i = first + k;
    int y = top + k * rowH;
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
  // Say where we are in a scrolled list, and mark that there is more above or
  // below — otherwise a window that moves under you is just confusing.
  if (n > maxVis) {
    char pos[12];
    snprintf(pos, sizeof(pos), "%c %d/%d %c", first > 0 ? '^' : ' ', sel + 1, n,
             first + vis < n ? 'v' : ' ');
    tft.setTextColor(CL_DIM);
    tft.setCursor(w - (int)strlen(pos) * 6 - 4, 10);
    tft.print(pos);
  }

  if (blurbs && blurbs[sel]) {
    tft.setTextColor(CL_TEAL);
    tft.setCursor(w / 2 - (int)strlen(blurbs[sel]) * 3, top + vis * rowH + 2);
    tft.print(blurbs[sel]);
  }
  tft.setTextColor(CL_DIM);
  const char *hint = "D2 up  D0 down  D1 select";
  tft.setCursor(w / 2 - (int)strlen(hint) * 3, h - 10);
  tft.print(hint);
}

// Run a menu to completion; returns the selected index (D1 confirms). start is
// where the cursor opens — a picker showing a stored value must open on it, or
// confirming without moving silently changes the setting.
static int runMenu(const char *title, const char *sub, const char *const *items,
                   const char *const *blurbs, int n, int start = 0) {
  int sel = (start >= 0 && start < n) ? start : 0;
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

// The refresh cadence the picker offers, in minutes. Querying is what battery
// life mostly turns on, so the choice runs from "as fresh as possible" to
// "lasts for days".
static const int REFRESH_CHOICES[] = {1, 2, 5, 10, 15, 30};
static const int N_REFRESH_CHOICES = 6;

// Pick the refresh interval on-device. This is the one setting worth changing
// without a laptop, so it lives here rather than in the captive portal.
static void refreshFlow() {
  Settings st;
  settingsLoad(st);

  // One extra row for Back: without it the only way out of this screen is to
  // pick something, which means an accidental visit changes the setting and
  // reboots the panel.
  static const int N_ROWS = N_REFRESH_CHOICES + 1;
  static char labels[N_REFRESH_CHOICES][12];
  static char blurbs[N_REFRESH_CHOICES][26];
  const char *items[N_ROWS];
  const char *notes[N_ROWS];
  int sel = 0;
  for (int i = 0; i < N_REFRESH_CHOICES; i++) {
    int m = REFRESH_CHOICES[i];
    snprintf(labels[i], sizeof(labels[i]), "%d min%s", m, st.refreshMin == m ? " *" : "");
    // Rough battery hours against a 1200 mAh cell, calibrated to the measured
    // ~18 mA at a five-minute cadence with light sleep, which fixes the idle
    // term near 10 mA and the per-query term near 40/m mA.
    //
    // Deliberately not derived from a raw before/after comparison: a freshly
    // charged cell sheds surface charge for the best part of an hour and the
    // gauge reads that as consumption, which inflated the early figures roughly
    // threefold. Only settled windows were used.
    snprintf(blurbs[i], sizeof(blurbs[i]), "~%d h on a 1200mAh cell",
             (int)(1200.0 / (10.0 + 40.0 / m)));
    items[i] = labels[i];
    notes[i] = blurbs[i];
    if (st.refreshMin == m) sel = i;
  }

  items[N_REFRESH_CHOICES] = "Back";
  notes[N_REFRESH_CHOICES] = "leave unchanged";

  int c = runMenu("REFRESH", "* = current", items, notes, N_ROWS, sel);
  waitRelease();
  if (c == N_REFRESH_CHOICES) return;  // Back — no save, no restart
  st.refreshMin = REFRESH_CHOICES[c];
  settingsSave(st);
  renderStatus(tft, (String(st.refreshMin) + " MIN").c_str(), "saved - restarting...");
  delay(1000);
  ESP.restart();  // the panel reads the cadence once at start
}

// The Settings submenu. Returns when the user picks Back.
static void settingsFlow() {
  static const char *items[] = {"Refresh Rate", "Config Portal", "Reset Settings", "Back"};
  static const char *blurbs[] = {"how often to query", "WiFi + Dynatrace setup",
                                 "erase all networks/tenants", "return to launcher"};
  for (;;) {
    // The subtitle slot is otherwise unused, so the build stamp costs no menu
    // row: the one place you look when asking "what is on this board?".
    int c = runMenu("SETTINGS", "build " TT_BUILD, items, blurbs, 4);
    waitRelease();
    if (c == 0) {  // on-device refresh cadence — no portal, no laptop
      refreshFlow();
    } else if (c == 1) {  // captive portal — blocks and reboots on Apply
      beacon(SEV_OK, true);  // blue = setup mode
      renderStatus(tft, "CONFIG PORTAL", "join wifi 'tinytrace-setup'");
      runPortal();
    } else if (c == 2) {  // reset to non-connected state
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
  // Drop the CPU before anything else (including Serial, whose divisors are
  // recomputed on the change). The panel is idle almost all of the time and
  // spends it polling buttons at 240 MHz — measured, that idle loop is the
  // largest single term in a ~65 mA blanked draw. 80 MHz is the floor that
  // still supports WiFi. APB stays at 80 MHz on the S3, so SPI to the ST7789
  // is unaffected; the visible cost is slower TLS handshakes in refreshAll().
  setCpuFrequencyMhz(TT_CPU_MHZ);

  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[launcher] boot %s @ %u MHz\n", TT_BUILD, (unsigned)getCpuFrequencyMhz());

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
