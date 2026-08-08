#include "render.h"

// Palette is shared verbatim with the CYD renderer (RGB565, same format as
// Adafruit_GFX): flat, high-contrast, read from across a desk.
static const uint16_t C_BG = 0x0000;     // black
static const uint16_t C_FG = 0xFFFF;     // white
static const uint16_t C_DIM = 0x8410;    // grey — headers, labels, stale text
static const uint16_t C_OK = 0x2E8B;     // teal
static const uint16_t C_INFO = 0x5D9F;   // blue
static const uint16_t C_WARN = 0xFD20;   // amber
static const uint16_t C_ERROR = 0xF986;  // red
static const uint16_t C_RULE = 0x2124;   // hairline separators

// Tuned for 240x135: header + footer + four rows is the whole budget.
static const int HEADER_H = 18;
static const int ROW_H = 21;   // list rows
static const int SIG_H = 25;   // signal rows (taller — they carry a sparkline)
static const int FOOTER_H = 14;

// Marquee state for scrolling long list rows. renderScreen resets it; the app
// loop calls renderScrollTick to advance it.
static int g_phase = 0;
static uint32_t g_lastTick = 0;
static const uint32_t SCROLL_MS = 220;

// Adafruit_GFX's built-in font is a 6x8 cell per character (monospaced),
// scaled by setTextSize. TFT_eSPI's font 1/2/4 map to sizes 1/2/3 here.
static int textW(const String &s, uint8_t sz) { return (int)s.length() * 6 * sz; }

// Truncate a string until it fits maxW pixels at the given text size.
static String clipToW(const String &s, uint8_t sz, int maxW) {
  String r = s;
  while (r.length() > 1 && textW(r, sz) > maxW) r.remove(r.length() - 1);
  return r;
}

// put{L,R,C}: draw text left-aligned / right-aligned / centred, vertically
// centred on midY (L/R) or with its top at topY (C). GFX has no text datums, so
// alignment is computed from the (monospaced) width.
static void putL(Adafruit_ST7789 &t, int x, int midY, const String &s, uint8_t sz, uint16_t fg) {
  t.setTextSize(sz);
  t.setTextColor(fg);
  t.setCursor(x, midY - 4 * sz);
  t.print(s);
}
static void putR(Adafruit_ST7789 &t, int xr, int midY, const String &s, uint8_t sz, uint16_t fg) {
  t.setTextSize(sz);
  t.setTextColor(fg);
  t.setCursor(xr - textW(s, sz), midY - 4 * sz);
  t.print(s);
}
static void putC(Adafruit_ST7789 &t, int cx, int topY, const String &s, uint8_t sz, uint16_t fg) {
  t.setTextSize(sz);
  t.setTextColor(fg);
  t.setCursor(cx - textW(s, sz) / 2, topY);
  t.print(s);
}

static uint16_t sevColor(Sev s) {
  switch (s) {
    case SEV_ERROR: return C_ERROR;
    case SEV_WARN: return C_WARN;
    case SEV_INFO: return C_INFO;
    default: return C_OK;
  }
}

void renderInit(Adafruit_ST7789 &tft) {
  tft.init(135, 240);  // 1.14" ST7789; power rail is enabled in board.cpp first
  tft.setRotation(TT_ROTATION);
  tft.setTextWrap(false);  // clip long strings rather than wrapping them
  tft.fillScreen(C_BG);
}

void renderStatus(Adafruit_ST7789 &tft, const char *line1, const char *line2) {
  tft.fillScreen(C_BG);
  int cx = tft.width() / 2;
  drawMascot(tft, cx, tft.height() / 2 - 22, MASCOT_THINKING, C_DIM);
  putC(tft, cx, tft.height() / 2 + 8, line1, 2, C_FG);
  if (line2 && *line2) putC(tft, cx, tft.height() / 2 + 30, line2, 1, C_DIM);
}

// --- battery indicator ------------------------------------------------------
// Pushed in by the app; the renderer owns only how it looks and when it needs
// repainting.
static bool g_battPresent = false;
static int g_battPct = 0;
static bool g_battChg = false;
static bool g_battDirty = false;

void renderSetBattery(bool present, int percent, bool charging) {
  if (present == g_battPresent && percent == g_battPct && charging == g_battChg) return;
  g_battPresent = present;
  g_battPct = percent;
  g_battChg = charging;
  g_battDirty = true;
}

// battW is the width the indicator needs — zero when there is nothing to show,
// so a board with no cell attached costs the title no space at all.
static int battW() {
  if (!g_battPresent) return 0;
  return 16 + 3 + textW(String(g_battPct) + "%", 1);
}

// A 5x9 lightning bolt — the charging marker, drawn over the cell.
static void drawBolt(Adafruit_ST7789 &t, int cx, int cy, uint16_t c) {
  t.fillTriangle(cx + 2, cy - 4, cx - 2, cy + 1, cx + 1, cy + 1, c);
  t.fillTriangle(cx - 2, cy + 4, cx + 2, cy - 1, cx - 1, cy - 1, c);
}

// drawBattery paints [cell][NN%] right-aligned at xr, vertically centred on
// midY. Colour carries the state: blue while charging, then amber/red as the
// charge falls, so the level reads without parsing the number.
static void drawBattery(Adafruit_ST7789 &t, int xr, int midY) {
  if (!g_battPresent) return;

  String txt = String(g_battPct) + "%";
  const int tw = textW(txt, 1);
  const int x0 = xr - (16 + 3 + tw);  // left edge of the cell glyph

  uint16_t c = C_OK;
  if (g_battChg) c = C_INFO;
  else if (g_battPct <= 15) c = C_ERROR;
  else if (g_battPct <= 30) c = C_WARN;

  t.drawRect(x0, midY - 4, 14, 9, c);        // cell body
  t.fillRect(x0 + 14, midY - 2, 2, 5, c);    // positive terminal nub
  int fill = (12 * g_battPct) / 100;         // inner bar, 12px of usable width
  if (fill > 0) t.fillRect(x0 + 1, midY - 3, fill, 7, c);
  if (g_battChg) drawBolt(t, x0 + 7, midY, C_FG);

  putR(t, xr, midY, txt, 1, c);
}

// drawHeader: severity dot, title, battery. Stale greys the whole bar and marks
// the title — the panel never presents old numbers as current.
//
// The refresh clock used to sit top-right and carry the stale "!" prefix. It
// was dropped to give the title back that width, so the marker moved onto the
// title itself; the footer still names the reason, and the beacon still goes
// blue. s.ts stays in the model, just unrendered.
static void drawHeader(Adafruit_ST7789 &tft, const Screen &s) {
  const int w = tft.width();
  tft.fillRect(0, 0, w, HEADER_H, C_BG);

  uint16_t accent = s.stale ? C_DIM : sevColor(s.sev);
  tft.fillCircle(7, HEADER_H / 2, 3, accent);

  const int bw = battW();
  if (bw) drawBattery(tft, w - 4, HEADER_H / 2);

  String title = clipToW(s.stale ? ("!" + s.title) : s.title, 2,
                         w - 15 - 4 - (bw ? bw + 8 : 0));
  putL(tft, 15, HEADER_H / 2, title, 2, s.stale ? C_DIM : C_FG);

  tft.drawFastHLine(0, HEADER_H - 1, w, C_RULE);
}

// renderBatteryTick repaints the indicator in place when the reading changed.
// The header bar is cheap to redraw whole; the idle face has no header, so the
// glyph gets its own corner cleared.
void renderBatteryTick(Adafruit_ST7789 &tft, const Screen &s) {
  if (!g_battDirty) return;
  g_battDirty = false;
  if (s.id == "idle") {
    const int bw = battW();
    tft.fillRect(tft.width() - 8 - bw, 2, bw + 8, 12, C_BG);
    if (bw) drawBattery(tft, tft.width() - 4, 8);
  } else {
    drawHeader(tft, s);
  }
}

// mAvail returns the pixel width the main column gets, after L and R.
static int mAvail(Adafruit_ST7789 &tft, const Row &row) {
  int left = 8 + (row.l.length() ? textW(row.l, 1) + 6 : 0);
  int right = tft.width() - 4 - (row.r.length() ? textW(row.r, 1) + 6 : 0);
  return right - left;
}

// rowOverflows reports whether the main text is wider than its column — i.e.
// whether it needs to scroll.
static bool rowOverflows(Adafruit_ST7789 &tft, const Row &row) {
  return textW(row.m, 2) > mAvail(tft, row);
}

// drawRow lays out one list line as [L] main.......... [R]. L and R use the
// small size; main gets the larger size. When main overflows it marquee-scrolls
// (phase advances over time) so a long problem title can still be read in full;
// L (the problem id) and R (the age) stay put.
static void drawRow(Adafruit_ST7789 &tft, const Row &row, int y, int phase) {
  const int w = tft.width();
  const int mid = y + ROW_H / 2;

  tft.fillRect(0, y, w, ROW_H, C_BG);
  tft.fillRect(0, y + 3, 3, ROW_H - 6, sevColor(row.sev));

  int left = 8;
  if (row.l.length()) {
    putL(tft, left, mid, row.l, 1, sevColor(row.sev));
    left += textW(row.l, 1) + 6;
  }
  int right = w - 4;
  if (row.r.length()) {
    putR(tft, right, mid, row.r, 1, C_DIM);
    right -= textW(row.r, 1) + 6;
  }

  int avail = right - left;
  if (textW(row.m, 2) <= avail) {
    putL(tft, left, mid, row.m, 2, C_FG);
  } else {
    // Marquee: slide a window over "text   text" so it wraps seamlessly.
    String loop = row.m + "   " + row.m;
    int period = row.m.length() + 3;
    int start = period > 0 ? (phase % period) : 0;
    int fit = avail / 12 + 2;  // chars that fit at size 2 (~12px each), +margin
    putL(tft, left, mid, clipToW(loop.substring(start, start + fit), 2, avail), 2, C_FG);
  }
}

// drawSparkline renders a normalized 0..100 series as a polyline. No axes, no
// fill — it is a shape you read the slope of, nothing more.
static void drawSparkline(Adafruit_ST7789 &tft, int x, int y, int w, int h, const uint8_t *v, int n,
                          uint16_t color) {
  if (n <= 0) return;
  if (n == 1) {
    tft.fillCircle(x + w / 2, y + h - 1 - (v[0] * (h - 1)) / 100, 1, color);
    return;
  }
  int px = x, py = y + h - 1 - (v[0] * (h - 1)) / 100;
  for (int i = 1; i < n; i++) {
    int nx = x + (i * (w - 1)) / (n - 1);
    int ny = y + h - 1 - (v[i] * (h - 1)) / 100;
    tft.drawLine(px, py, nx, ny, color);
    px = nx;
    py = ny;
  }
}

// drawArrow: a tiny trend glyph drawn from primitives. Up/down triangles, flat
// is a dash.
static void drawArrow(Adafruit_ST7789 &tft, int cx, int cy, int trend, uint16_t color) {
  if (trend > 0) {
    tft.fillTriangle(cx - 4, cy + 3, cx + 4, cy + 3, cx, cy - 4, color);
  } else if (trend < 0) {
    tft.fillTriangle(cx - 4, cy - 3, cx + 4, cy - 3, cx, cy + 4, color);
  } else {
    tft.fillRect(cx - 4, cy - 1, 8, 2, color);
  }
}

// drawSignal is one golden-signal row: label · big value · sparkline · trend.
static void drawSignal(Adafruit_ST7789 &tft, const Row &row, int y) {
  const int w = tft.width();
  const int mid = y + SIG_H / 2;
  uint16_t accent = sevColor(row.sev);

  tft.fillRect(0, y, w, SIG_H, C_BG);
  putL(tft, 6, mid, row.l, 1, C_DIM);    // label, e.g. "LAT"
  putL(tft, 56, mid, row.m, 2, accent);  // value, e.g. "128 ms"
  drawSparkline(tft, 126, y + 5, 84, SIG_H - 12, row.spark, row.nspark, accent);
  drawArrow(tft, w - 8, mid, row.trend, accent);
}

// drawFooter shows position dots and, when something is wrong, the reason;
// otherwise the tenant.
static void drawFooter(Adafruit_ST7789 &tft, const Screen &s, int index, int total) {
  const int w = tft.width();
  const int h = tft.height();
  const int y = h - FOOTER_H;
  const int mid = y + FOOTER_H / 2;

  tft.fillRect(0, y, w, FOOTER_H, C_BG);
  tft.drawFastHLine(0, y, w, C_RULE);

  int dotsX = w - 6 - (total - 1) * 9;
  if (s.stale && s.err.length()) {
    putL(tft, 4, mid, clipToW(s.err, 1, dotsX - 8), 1, C_ERROR);
  } else if (s.env.length()) {
    String env = s.env;
    env.replace("https://", "");
    putL(tft, 4, mid, clipToW(env, 1, dotsX - 8), 1, C_DIM);
  }

  for (int i = 0; i < total; i++) {
    if (i == index) {
      tft.fillCircle(dotsX + i * 9, mid, 3, C_FG);
    } else {
      tft.drawCircle(dotsX + i * 9, mid, 2, C_DIM);
    }
  }
}

// drawMascot: the robot face from primitives, scaled for the 240x135 panel.
void drawMascot(Adafruit_ST7789 &tft, int cx, int cy, Mascot m, uint16_t color) {
  const int hw = 16;  // half width
  const int hh = 12;  // half height

  tft.drawFastVLine(cx, cy - hh - 6, 6, color);
  tft.fillCircle(cx, cy - hh - 7, 2, color);
  tft.drawRoundRect(cx - hw, cy - hh, hw * 2, hh * 2, 5, color);

  int ey = cy - 2;
  int ex = 7;
  switch (m) {
    case MASCOT_ALARM:
      tft.fillCircle(cx - ex, ey, 3, color);
      tft.fillCircle(cx + ex, ey, 3, color);
      tft.drawLine(cx - ex - 4, ey - 6, cx - ex + 3, ey - 4, color);
      tft.drawLine(cx + ex + 4, ey - 6, cx + ex - 3, ey - 4, color);
      tft.drawRoundRect(cx - 5, cy + 4, 10, 5, 2, color);
      break;
    case MASCOT_THINKING:
      tft.drawFastHLine(cx - ex - 3, ey, 6, color);
      tft.drawFastHLine(cx + ex - 3, ey, 6, color);
      for (int i = -1; i <= 1; i++) tft.fillCircle(cx + i * 4, cy + 7, 1, color);
      break;
    default:
      tft.fillCircle(cx - ex, ey, 3, color);
      tft.fillCircle(cx + ex, ey, 3, color);
      tft.drawLine(cx - 5, cy + 5, cx - 1, cy + 8, color);
      tft.drawLine(cx - 1, cy + 8, cx + 1, cy + 8, color);
      tft.drawLine(cx + 1, cy + 8, cx + 5, cy + 5, color);
      break;
  }
}

// renderIdle is the sleep face: mascot, one summary line, tenant.
static void renderIdle(Adafruit_ST7789 &tft, const Screen &s) {
  tft.fillScreen(C_BG);
  const int cx = tft.width() / 2;
  uint16_t accent = sevColor(s.sev);

  drawMascot(tft, cx, tft.height() / 2 - 24, s.mascot, accent);
  if (s.nrows > 0) putC(tft, cx, tft.height() / 2 + 8, s.rows[0].m, 2, accent);
  String env = s.env;
  env.replace("https://", "");
  putC(tft, cx, tft.height() / 2 + 30, env, 1, C_DIM);
  drawBattery(tft, tft.width() - 4, 8);  // idle has no header — own corner
}

void renderScreen(Adafruit_ST7789 &tft, const Screen &s, int index, int total) {
  if (s.id == "idle") {
    renderIdle(tft, s);
    return;
  }

  tft.fillScreen(C_BG);
  drawHeader(tft, s);

  if (s.kind == KIND_SIGNALS) {
    for (int i = 0; i < s.nrows; i++) {
      drawSignal(tft, s.rows[i], HEADER_H + i * SIG_H);
    }
  } else if (s.nrows == 0) {
    const int cx = tft.width() / 2;
    drawMascot(tft, cx, tft.height() / 2 - 12, MASCOT_IDLE, C_OK);
    putC(tft, cx, tft.height() / 2 + 16, "all clear", 2, C_OK);
  } else {
    g_phase = 0;
    g_lastTick = millis();
    for (int i = 0; i < s.nrows; i++) {
      drawRow(tft, s.rows[i], HEADER_H + i * ROW_H, 0);
    }
    // The query's limit can hide rows; say so rather than implying completeness.
    if (s.count > s.nrows) {
      int y = HEADER_H + s.nrows * ROW_H;
      if (y + 12 < tft.height() - FOOTER_H) {
        putL(tft, 8, y + 6, "+" + String(s.count - s.nrows) + " more", 1, C_DIM);
      }
    }
  }

  drawFooter(tft, s, index, total);
}

// renderScrollTick advances the marquee for the current screen and repaints only
// the rows that actually overflow (so static rows never flicker). Call it every
// loop iteration; it throttles itself to SCROLL_MS.
void renderScrollTick(Adafruit_ST7789 &tft, const Screen &s) {
  if (s.kind != KIND_LIST || s.nrows == 0) return;
  uint32_t now = millis();
  if (now - g_lastTick < SCROLL_MS) return;
  g_lastTick = now;
  g_phase++;
  for (int i = 0; i < s.nrows; i++) {
    if (rowOverflows(tft, s.rows[i])) drawRow(tft, s.rows[i], HEADER_H + i * ROW_H, g_phase);
  }
}
