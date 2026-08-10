// The Feather's screen model.
//
// Unlike the CYD firmware — which is a dumb client mirroring a schema the Go
// sidecar owns — the Feather is standalone: it builds these structs itself from
// DQL results (see dt.cpp). So this is the authoritative model, not a mirror,
// and it carries one thing the CYD schema never needed: a sparkline, for the
// golden-signals screen.
#pragma once

#include <Arduino.h>

// Set in platformio.ini; fallbacks here so every translation unit compiles even
// if a flag is missing. TT_ROTATION 3 is landscape with the buttons below the
// panel on the Reverse TFT (confirm orientation on the board).
#ifndef TT_ROTATION
#define TT_ROTATION 3
#endif
#ifndef TT_LONGPRESS_MS
#define TT_LONGPRESS_MS 800
#endif
#ifndef TT_SLEEP_MS
#define TT_SLEEP_MS 300000
#endif
// Build stamp, set from the git commit by version.py at build time. A trailing
// "+" means the tree was dirty, so the image matches no commit. The fallback
// applies when building without git (a source tarball, say).
#ifndef TT_BUILD
#define TT_BUILD "nogit"
#endif
// CPU clock. 80 MHz is the lowest that still runs WiFi, and measured idle draw
// is ~26 mA against ~41 mA at 240 — about 15 mA, which on this panel is most of
// the budget once it is not querying.
//
// It does cost query time: a refresh takes 14.6 s at 80 MHz against 10.2 s at
// 240. Under the old 60 s cadence that cancelled the idle saving exactly (both
// landed near 66 mA, which is why an earlier measurement wrongly concluded the
// clock did not matter). At the 5-minute default the query window is only ~3%
// of the time, so the idle term dominates and the low clock wins.
#ifndef TT_CPU_MHZ
#define TT_CPU_MHZ 80
#endif
// Default refresh cadence in minutes, used when the device has no stored
// preference. Measured on a 1200 mAh cell: ~16 h at a one-minute cadence
// against ~65 h at five minutes, so the default is deliberately not one minute.
#ifndef TT_REFRESH_MIN_DEFAULT
#define TT_REFRESH_MIN_DEFAULT 5
#endif
#ifndef TT_TZ
#define TT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"  // Central European; POSIX TZ string
#endif

enum Sev : uint8_t { SEV_OK = 0, SEV_INFO = 1, SEV_WARN = 2, SEV_ERROR = 3 };
enum Mascot : uint8_t { MASCOT_IDLE = 0, MASCOT_THINKING = 1, MASCOT_ALARM = 2 };

// A screen is either a list of rows (problems, logs) or a stack of signals
// (golden signals). The renderer branches on this once.
enum Kind : uint8_t { KIND_LIST = 0, KIND_SIGNALS = 1 };

// 240x135 is small: four rows under the header is all that fits. The list
// screens report a total count so the panel can still say "+N more".
static const int TT_MAX_ROWS = 4;
static const int TT_MAX_SCREENS = 6;
// Sparkline samples are downsampled host-side... here, device-side — to at most
// this many points, which is already finer than the ~84px we draw them into.
static const int TT_SPARK_MAX = 32;

// Row is one line of a list screen, or one signal on the signals screen.
struct Row {
  String l;  // short left label — a problem id, a log level, a signal name
  String m;  // main text (list) or the big value (signals)
  String r;  // short right value — an age, a unit
  Sev sev = SEV_OK;

  // Signals only: a normalized 0..100 series and a coarse direction. nspark==0
  // means "no sparkline", so list rows simply leave it zero.
  uint8_t spark[TT_SPARK_MAX];
  uint8_t nspark = 0;
  int8_t trend = 0;  // -1 falling, 0 flat, +1 rising
};

// Screen is one full panel's worth of state.
struct Screen {
  String id;
  String title;
  String ts;   // "15:06", wall-clock of the last good refresh (no longer drawn)
  String env;  // tenant, so a misconfigured device is obvious
  String err;  // short reason when stale
  Kind kind = KIND_LIST;

  Row rows[TT_MAX_ROWS];
  int nrows = 0;
  int count = 0;  // underlying records; may exceed nrows (query limit)

  Sev sev = SEV_OK;
  Mascot mascot = MASCOT_IDLE;
  bool stale = false;
  // valid stays false until the first successful build, so the renderer can
  // show a connecting state instead of an empty panel.
  bool valid = false;
};
