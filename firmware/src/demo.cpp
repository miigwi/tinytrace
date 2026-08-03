#include "demo.h"

#include <math.h>

#include "dt_screens.h"

static const char *DEMO_ENV = "DEMO MODE";

// Row isn't an aggregate here (default member initializers under gnu++11), so
// fill it field by field rather than with brace-init.
static void setRow(Row &row, const char *l, const char *m, const char *r, Sev sev) {
  row.l = l;
  row.m = m;
  row.r = r;
  row.sev = sev;
}

// A gentle normalized sparkline (0..100) with an optional upward bias, so demo
// signals look alive rather than flat.
static void fakeSpark(Row &r, float phase, bool rising) {
  r.nspark = 16;
  for (int i = 0; i < r.nspark; i++) {
    float t = i / (float)(r.nspark - 1);
    float v = 0.5f + 0.35f * sinf(phase + t * 6.28318f);
    if (rising) v = v * (0.55f + 0.45f * t);  // trend upward across the window
    int s = (int)(10 + v * 80);
    r.spark[i] = (uint8_t)(s < 0 ? 0 : s > 100 ? 100 : s);
  }
}

int buildDemoScreens(Screen out[], int cap) {
  if (cap < 4) return 0;

  // 0 — active problems
  {
    Screen s;
    s.id = "problems";
    s.title = "active problems";
    s.kind = KIND_LIST;
    s.ts = "03:07";
    s.env = DEMO_ENV;
    setRow(s.rows[0], "P-2481", "checkout deploy regression", "12m", SEV_ERROR);
    setRow(s.rows[1], "P-2479", "cart CPU saturation", "41m", SEV_ERROR);
    setRow(s.rows[2], "P-2475", "kafka broker-2 disk forecast full", "2h", SEV_WARN);
    s.nrows = 3;
    s.count = 3;
    s.sev = SEV_ERROR;
    s.mascot = MASCOT_ALARM;
    s.valid = true;
    out[0] = s;
  }

  // 1 — golden signals (checkout)
  {
    Screen s;
    s.id = "golden";
    s.title = "checkout";
    s.kind = KIND_SIGNALS;
    s.ts = "03:07";
    s.env = DEMO_ENV;
    s.rows[0].l = "LAT";
    s.rows[0].m = "128 ms";
    s.rows[0].sev = SEV_OK;
    s.rows[0].trend = 1;
    fakeSpark(s.rows[0], 0.4f, true);
    s.rows[1].l = "TPS";
    s.rows[1].m = "2.4k";
    s.rows[1].sev = SEV_OK;
    s.rows[1].trend = 0;
    fakeSpark(s.rows[1], 1.7f, false);
    s.rows[2].l = "ERR";
    s.rows[2].m = "3.1 %";
    s.rows[2].sev = SEV_WARN;
    s.rows[2].trend = 1;
    fakeSpark(s.rows[2], 2.9f, true);
    s.rows[3].l = "4xx";
    s.rows[3].m = "0.4";
    s.rows[3].sev = SEV_INFO;
    s.rows[3].trend = 0;
    fakeSpark(s.rows[3], 0.9f, false);
    s.nrows = 4;
    s.sev = SEV_WARN;
    s.mascot = MASCOT_ALARM;
    s.valid = true;
    out[1] = s;
  }

  // 2 — last logs
  {
    Screen s;
    s.id = "logs";
    s.title = "last logs";
    s.kind = KIND_LIST;
    s.ts = "03:07";
    s.env = DEMO_ENV;
    setRow(s.rows[0], "ERR", "currency: connection pool exhausted", "03:06", SEV_ERROR);
    setRow(s.rows[1], "WRN", "valkey evictions 40x on recommendationCache", "03:05", SEV_WARN);
    setRow(s.rows[2], "ERR", "payment: SQLi probe on /charge blocked", "03:03", SEV_ERROR);
    s.nrows = 3;
    s.count = 3;
    s.sev = SEV_ERROR;
    s.mascot = MASCOT_ALARM;
    s.valid = true;
    out[2] = s;
  }

  // 3 — idle (reuse the live builder; it needs no network)
  out[3] = buildIdle(3, DEMO_ENV);

  return 4;
}
