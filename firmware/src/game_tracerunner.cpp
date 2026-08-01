// Trace Runner — a one-button noir endless runner for the Feather.
//
// A port, in spirit, of the browser game (github.com/miigwi/trace-runner):
// Agent Decoder runs the graveyard shift across a rain-soaked city, and you
// keep the SLO alive. The original's four-choice incident triage can't exist
// with a single button, so it folds into the run — the SLO bar is your health,
// incident beacons are worth grabbing, and the whole thing is an endless,
// distance-scored sprint. One control: D1 = jump, held = higher (the original's
// variable-jump mechanic). D0/D2 also jump, for forgiveness.
//
// Rendering: everything is drawn into an off-screen GFXcanvas16 and pushed to
// the ST7789 in one SPI blit per frame, so it never flickers.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

#include "app.h"
#include "hal.h"

// ------------------------------------------------------------------ geometry
static const int VW = 240, VH = 135;
static const int GROUND_Y = 112;  // top of the ground platforms (player feet)
static const int LEDGE_Y = GROUND_Y - 40;
static const int PW = 10, PH = 18;   // player collision box
static const int PLAYER_SX = 46;     // player's fixed screen x; the world scrolls

// ------------------------------------------------------------------ physics
static const float JUMPV = -150.f, GRAV = 560.f, RISE_HELD = 0.5f;
static const float TAPCLAMP = -110.f, MAXFALL = 260.f;
static const float COYOTE = 0.10f, BUFFER = 0.12f;

// ------------------------------------------------------------------ colour
#define C16(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t COL_BLACK = 0x0000;
static const uint16_t COL_CYAN = C16(46, 230, 255);
static const uint16_t COL_AMBER = C16(255, 176, 46);
static const uint16_t COL_RED = C16(255, 48, 80);
static const uint16_t COL_OK = C16(46, 255, 168);
static const uint16_t COL_WHITE = C16(223, 230, 242);
static const uint16_t COL_DIM = C16(120, 130, 150);

// district accent (city / interior / rooftop), cycled by distance
static const uint16_t ACCENT[3] = {COL_AMBER, C16(232, 200, 96), C16(154, 180, 232)};

// ------------------------------------------------------------------ sprites
// Each sprite is rows of palette keys ('.' = transparent); blit paints them a
// pixel at a time into the canvas. Small enough to hand-author for this screen.
struct Pal {
  char k;
  uint16_t c;
};
static uint16_t palLook(const Pal *p, int n, char k) {
  for (int i = 0; i < n; i++)
    if (p[i].k == k) return p[i].c;
  return C16(255, 0, 255);
}
static void blit(GFXcanvas16 &cv, const char *const *rows, int h, int w, int x, int y,
                 const Pal *pal, int np) {
  for (int r = 0; r < h; r++) {
    const char *row = rows[r];
    int rl = (int)strlen(row);
    for (int c = 0; c < w && c < rl; c++) {
      char ch = row[c];
      if (ch == '.') continue;
      int px = x + c, py = y + r;
      if ((unsigned)px < VW && (unsigned)py < VH) cv.drawPixel(px, py, palLook(pal, np, ch));
    }
  }
}

// Agent Decoder — fedora + trench coat, 12x18. Upper body is shared; the legs
// swap per frame. K hat/outline · S skin · s skin-shade · C coat · c coat-shade
// · T shirt · P pants · F shoes.
static const Pal DECK_PAL[] = {
    {'K', C16(26, 18, 32)},  {'S', C16(238, 195, 154)}, {'s', C16(201, 141, 94)},
    {'C', C16(156, 107, 63)}, {'c', C16(111, 69, 39)},  {'T', C16(47, 125, 109)},
    {'P', C16(63, 68, 89)},  {'F', C16(31, 28, 38)},
};
static const int DECK_NP = sizeof(DECK_PAL) / sizeof(DECK_PAL[0]);
// rows 0..12 shared, then 5 leg rows per frame → assembled below.
static const char *DECK_UPPER[] = {
    "....KKKK....", "...KKKKKK...", "..KKKKKKKK..", "....SSSS....", "....SsSS....",
    "....SSSS....", "...CCCCCC...", "..CCCTTCCC..", "..cCCTTCCc..", "..cCCCCCCc..",
    "..cCCCCCCc..", "...cCCCCc...", "...PPPPPP...",
};
static const char *DECK_RUNA[] = {"...PP.PP....", "...PP.PP....", "..PP..PP....",
                                  "..FF...F....", "..FF...FF..."};
static const char *DECK_RUNB[] = {"...PP.PP....", "...PP.PP....", "..PP...PP..",
                                  "...F...FF..", "..FF...FF.."};
static const char *DECK_JUMP[] = {"...PPPPP....", "...PP.PP....", "..FF...FF...",
                                  "............", "............"};
static const char *DECK_HIT[] = {"..PP...PP...", ".PP.....PP..", "FF.......FF.",
                                 "............", "............"};
// assembled frames (18 rows) built at runtime into these row-pointer buffers
static const char *deckFrame[4][18];
static void buildDeckard() {
  const char **legs[4] = {DECK_RUNA, DECK_RUNB, DECK_JUMP, DECK_HIT};
  for (int f = 0; f < 4; f++) {
    for (int r = 0; r < 13; r++) deckFrame[f][r] = DECK_UPPER[r];
    for (int r = 0; r < 5; r++) deckFrame[f][13 + r] = legs[f][r];
  }
}
// frame index: 0/1 run cycle, 2 jump, 3 hit

// Patrol drone — 13x6, red eye. O outline · W hull · w hull-shade · R eye.
static const Pal DRONE_PAL[] = {
    {'O', C16(20, 16, 24)}, {'W', C16(223, 227, 240)}, {'w', C16(169, 173, 194)},
    {'R', C16(255, 59, 48)},
};
static const int DRONE_NP = sizeof(DRONE_PAL) / sizeof(DRONE_PAL[0]);
static const char *DRONE_ROWS[] = {
    "..OOOOOOOOO..", ".OWWWWWWWWWO.", ".OWWwwwwwRRWO",
    ".OWwwwwwwwRWO", ".OWWWWWWWWWO.", "..OOOOOOOOO..",
};
static const int DRONE_W = 13, DRONE_H = 6;

// Origami unicorn — 12x10, gold. Ported from the original UNI_SMALL.
static const Pal UNI_PAL[] = {{'A', C16(255, 215, 90)}, {'B', C16(212, 168, 40)}};
static const int UNI_NP = 2;
static const char *UNI_ROWS[] = {
    ".........A..", ".........A..", ".......AAABB", "......AAAB..", ".BAAAAAABB..",
    ".BABBAAABB..", ".A.BBAAABB..", "...BA...AB..", "...B....A...", "...B....A...",
};
static const int UNI_W = 12, UNI_H = 10;

// ------------------------------------------------------------------ world
struct Plat {
  float x0, x1, y;
  bool live;
};
struct Vent {
  float x, ph;
  bool live;
};
struct Drone {
  float x, y, lo, hi, dir, spd;
  bool live;
};
struct Shard {
  float x, y;
  uint8_t kind;  // 0 = data shard (10cr), 1 = incident beacon (40cr)
  bool got, live;
};
struct Uni {
  float x, y;
  bool got, live;
};

static Plat plats[28];
static Vent vents[16];
static Drone drones[12];
static Shard shards[48];
static Uni unis[8];

template <typename T, int N>
static int freeSlot(T (&a)[N]) {
  for (int i = 0; i < N; i++)
    if (!a[i].live) return i;
  return -1;
}

// ------------------------------------------------------------------ state
enum Mode { TITLE, COUNT, PLAY, OVER };
static Mode mode;
static float camX;        // world scroll (screen-left edge in world coords)
static float py, vy;      // player top (screen space) and vertical velocity
static bool onGround, jumpHeld;
static float coyote, buffer, anim, invuln, countT, genX;
static float slo;
static long credits, uni;
static uint32_t best;
static Preferences prefs;

// lightning
static float litT, litNext, litFlash;

// rain
struct Drop {
  float x, y, v;
  uint8_t len;
};
static Drop drops[40];

// seeded skyline (two parallax layers), generated once
struct Bldg {
  int x, w, h;
  uint8_t seed;
};
static Bldg far_[26], mid_[20];
static uint32_t lcg = 0x1234567;
static float rnd() {
  lcg = lcg * 1664525u + 1013904223u;
  return (lcg >> 8) / 16777216.0f;
}
static float rrange(float a, float b) { return a + (b - a) * rnd(); }

static float pwx() { return camX + PLAYER_SX; }  // player world x

// -------------------------------------------------------------- generation
static void addPlat(float x0, float x1, float y) {
  int i = freeSlot(plats);
  if (i < 0) return;
  plats[i] = {x0, x1, y, true};
}
static void addShard(float x, float y, uint8_t kind) {
  int i = freeSlot(shards);
  if (i < 0) return;
  shards[i] = {x, y, kind, false, true};
}

// Difficulty tier by distance travelled (0..3-ish).
static int tier() { return (int)fminf(3.f, camX / 1600.f); }

// Generate one platform segment (+ decorations) starting at genX, then a gap.
static void genSegment(bool safe) {
  int t = tier();
  float w = safe ? 150.f : rrange(58.f, 108.f - t * 6.f);
  float x0 = genX, x1 = genX + w;
  addPlat(x0, x1, GROUND_Y);

  if (!safe) {
    // a steam vent past the front edge (jump it)
    if (w > 70 && rnd() < 0.45f + t * 0.06f) {
      int vi = freeSlot(vents);
      if (vi >= 0) vents[vi] = {x0 + w * 0.55f, rrange(0.f, 2.8f), true};
    }
    // a patrol drone bobbing over this platform (jump it)
    if (rnd() < 0.30f + t * 0.05f) {
      int di = freeSlot(drones);
      if (di >= 0) {
        float dy = GROUND_Y - PH - (float)(int)rrange(2, 10);  // low enough to force a jump
        drones[di] = {x0 + w * 0.5f, dy, x0 + 16, x1 - 16, 1.f, rrange(22.f, 40.f), true};
      }
    }
    // a ledge above with an origami unicorn (rare reward)
    if (rnd() < 0.10f) {
      float lx = x0 + w * 0.35f;
      addPlat(lx, lx + 60, LEDGE_Y);
      int ui = freeSlot(unis);
      if (ui >= 0) unis[ui] = {lx + 30, LEDGE_Y - 14, false, true};
    }
    // a line of data shards along the platform
    if (rnd() < 0.7f) {
      int n = 2 + (int)rrange(0, 3);
      for (int k = 1; k <= n; k++) addShard(x0 + w * k / (n + 1), GROUND_Y - 26, 0);
    }
    // an occasional incident beacon at head height (jump to grab, worth 40)
    if (rnd() < 0.22f) addShard(x0 + w * 0.5f, GROUND_Y - 44, 1);
  }

  genX = x1;

  if (!safe) {
    float g = rrange(26.f, 40.f + t * 8.f);
    // a shard arc over the gap, tempting the jump
    addShard(genX + g * 0.5f, GROUND_Y - 34, 0);
    genX += g;
  }
}

static void ensureWorld() {
  while (genX < camX + VW + 90) genSegment(false);
}

// Cull anything scrolled well off the left edge, freeing its slot for reuse.
static void cullAll() {
  float L = camX - 60;
  for (auto &p : plats)
    if (p.live && p.x1 < L) p.live = false;
  for (auto &v : vents)
    if (v.live && v.x < L) v.live = false;
  for (auto &d : drones)
    if (d.live && d.hi < L) d.live = false;
  for (auto &s : shards)
    if (s.live && s.x < L) s.live = false;
  for (auto &u : unis)
    if (u.live && u.x < L) u.live = false;
}

// -------------------------------------------------------------- lifecycle
static void resetRun() {
  for (auto &p : plats) p.live = false;
  for (auto &v : vents) v.live = false;
  for (auto &d : drones) d.live = false;
  for (auto &s : shards) s.live = false;
  for (auto &u : unis) u.live = false;
  camX = 0;
  py = GROUND_Y - PH;
  vy = 0;
  onGround = true;
  jumpHeld = false;
  coyote = buffer = 0;
  invuln = 0;
  slo = 100;
  credits = 0;
  uni = 0;
  genX = 0;
  genSegment(true);  // long safe runway to start on
  ensureWorld();
}

static long scoreNow() { return (long)(camX / 4.f) + credits; }

static const char *rankFor(float s) {
  if (s >= 100) return "FIVE-NINES MYTH";
  if (s >= 99.5f) return "GUARDIAN OF NINES";
  if (s >= 99) return "RELIABILITY ORACLE";
  if (s >= 97.5f) return "AUTONOMOUS LEGEND";
  if (s >= 95) return "PRINCIPAL AGENT";
  if (s >= 92) return "STAFF AGENT";
  if (s >= 88) return "SENIOR AGENT";
  if (s >= 82) return "AGENT 2ND CLASS";
  if (s >= 74) return "JUNIOR AGENT";
  if (s >= 64) return "ON-CALL GREENHORN";
  if (s >= 50) return "INTERN W/ ROOT";
  return "PAGER GOBLIN";
}

static void gameOver() {
  mode = OVER;
  long sc = scoreNow();
  if (sc > (long)best) {
    best = sc;
    prefs.putUInt("best", best);
  }
}

static void takeHit() {
  if (invuln > 0) return;
  slo -= 9;
  invuln = 1.0f;
  beaconColor(180, 20, 20);  // red flash
  if (slo <= 0) {
    slo = 0;
    gameOver();
  }
}

// After falling into a gap, snap up onto the next ground platform ahead.
static void recoverFromFall() {
  float best0 = 1e9f;
  Plat *tgt = nullptr;
  for (auto &p : plats) {
    if (!p.live || p.y != GROUND_Y) continue;
    if (p.x1 > pwx() && p.x0 < best0) {
      best0 = p.x0;
      tgt = &p;
    }
  }
  if (tgt) camX = tgt->x0 + 8 - PLAYER_SX;
  py = GROUND_Y - PH;
  vy = 0;
  onGround = true;
}

// -------------------------------------------------------------- simulation
static void stepPlay(float dt) {
  float speed = 78.f + fminf(66.f, camX / 90.f);
  camX += speed * dt;
  ensureWorld();
  cullAll();

  coyote -= dt;
  buffer -= dt;
  if (buffer > 0 && (onGround || coyote > 0)) {
    vy = JUMPV;
    onGround = false;
    coyote = 0;
    buffer = 0;
  }
  if (!jumpHeld && vy < TAPCLAMP) vy = TAPCLAMP;
  float prevFeet = py + PH;
  float g = (jumpHeld && vy < 0) ? GRAV * RISE_HELD : GRAV;
  vy = fminf(MAXFALL, vy + g * dt);
  py += vy * dt;
  float newFeet = py + PH;

  bool landed = false;
  if (vy >= 0) {
    for (auto &p : plats) {
      if (!p.live) continue;
      if (prevFeet <= p.y + 1 && newFeet >= p.y && pwx() + PW > p.x0 + 1 && pwx() < p.x1 - 1) {
        py = p.y - PH;
        vy = 0;
        landed = true;
        break;
      }
    }
  }
  if (landed) {
    onGround = true;
    coyote = COYOTE;
  } else if (onGround) {
    bool sup = false;
    for (auto &p : plats)
      if (p.live && fabsf((py + PH) - p.y) < 2 && pwx() + PW > p.x0 && pwx() < p.x1) {
        sup = true;
        break;
      }
    if (!sup) {
      onGround = false;
      coyote = COYOTE;
    }
  }

  // drones
  for (auto &d : drones) {
    if (!d.live) continue;
    d.x += d.dir * d.spd * dt;
    if (d.x < d.lo) {
      d.x = d.lo;
      d.dir = 1;
    }
    if (d.x > d.hi) {
      d.x = d.hi;
      d.dir = -1;
    }
    if (pwx() < d.x + DRONE_W && pwx() + PW > d.x && py < d.y + DRONE_H && py + PH > d.y)
      takeHit();
  }
  // vents (flare on during part of the cycle)
  for (auto &v : vents) {
    if (!v.live) continue;
    float cyc = fmodf(anim + v.ph, 2.8f);
    if (cyc > 1.6f && pwx() + PW > v.x - 2 && pwx() < v.x + 12 && py + PH > GROUND_Y - 32)
      takeHit();
  }
  // shards / beacons
  for (auto &s : shards) {
    if (!s.live || s.got) continue;
    if (fabsf(pwx() + PW / 2 - s.x) < 11 && fabsf(py + PH / 2 - s.y) < 14) {
      s.got = true;
      credits += s.kind ? 40 : 10;
    }
  }
  // unicorns
  for (auto &u : unis) {
    if (!u.live || u.got) continue;
    if (fabsf(pwx() + PW / 2 - u.x) < 14 && fabsf(py + PH / 2 - u.y) < 16) {
      u.got = true;
      uni++;
      credits += 50;
    }
  }

  // fell into a gap
  if (py > VH + 20) {
    takeHit();
    if (mode == OVER) return;
    recoverFromFall();
  }
}

// -------------------------------------------------------------- rendering
static void drawSky(GFXcanvas16 &cv, int district) {
  // vertical gradient via a handful of bands
  static const uint16_t city[] = {C16(5, 3, 8), C16(13, 7, 20), C16(26, 14, 32)};
  static const uint16_t intr[] = {C16(23, 16, 8), C16(35, 23, 8), C16(26, 18, 6)};
  static const uint16_t roof[] = {C16(4, 5, 11), C16(12, 14, 30), C16(24, 20, 40)};
  const uint16_t *g = district == 1 ? intr : district == 2 ? roof : city;
  int bands = 9;
  for (int i = 0; i < bands; i++) {
    float t = i / (float)(bands - 1);
    uint16_t c = t < 0.5f ? g[0] : t < 0.85f ? g[1] : g[2];
    cv.fillRect(0, i * VH / bands, VW, VH / bands + 1, c);
  }
  // lightning flash lifts the sky for a beat (city + rooftop)
  if ((district == 0 || district == 2) && litFlash > 0) {
    cv.fillRect(0, 0, VW, 70, C16(70, 84, 120));
  }
}

static void drawSkyline(GFXcanvas16 &cv, int district) {
  // far layer
  for (auto &b : far_) {
    int sx = (int)fmodf(b.x - camX * 0.16f, 1500.f);
    if (sx < 0) sx += 1500;
    sx -= 400;
    if (sx < -60 || sx > VW) continue;
    int top = 96 - b.h;
    cv.fillRect(sx, top, b.w, b.h + 40, C16(10, 8, 18));
    for (int wy = top + 4; wy < 92; wy += 7)
      for (int wx = sx + 2; wx < sx + b.w - 2; wx += 6)
        if (((wx * 7 + wy * 13) % 19) < 3) cv.drawPixel(wx, wy, C16(90, 70, 30));
    if (b.seed < 60) {  // beacon light blinking atop some towers
      bool on = sinf(anim * 3 + b.x) > 0;
      cv.drawPixel(sx + b.w / 2, top, on ? COL_RED : C16(60, 12, 20));
    }
  }
  // mid layer
  for (auto &b : mid_) {
    int sx = (int)fmodf(b.x - camX * 0.34f, 1700.f);
    if (sx < 0) sx += 1700;
    sx -= 500;
    if (sx < -90 || sx > VW) continue;
    int top = 100 - b.h;
    cv.fillRect(sx, top, b.w, b.h + 30, C16(16, 10, 24));
    for (int wy = top + 3; wy < 98; wy += 6)
      for (int wx = sx + 3; wx < sx + b.w - 3; wx += 6)
        if (((wx * 11 + wy * 7) % 23) < 4) cv.drawPixel(wx, wy, C16(60, 80, 110));
  }
}

static void drawPlats(GFXcanvas16 &cv, int district) {
  uint16_t accent = ACCENT[district];
  for (auto &p : plats) {
    if (!p.live) continue;
    int sx = (int)(p.x0 - camX);
    int w = (int)(p.x1 - p.x0);
    if (sx + w < 0 || sx > VW) continue;
    int h = (p.y >= GROUND_Y) ? VH - (int)p.y : 12;
    cv.fillRect(sx, (int)p.y, w, h, C16(13, 9, 22));
    cv.drawFastHLine(sx, (int)p.y, w, C16(38, 32, 60));
    cv.drawFastHLine(sx, (int)p.y, w, accent);  // 1px lit lip
    // service duct hints
    for (int dx = 6; dx < w - 6; dx += 16) cv.fillRect(sx + dx, (int)p.y + 5, 6, 2, C16(24, 22, 48));
  }
}

static uint16_t flameCol(float t) {
  return t < 0.16f ? C16(255, 246, 216)
         : t < 0.4f ? C16(255, 210, 74)
         : t < 0.7f ? C16(255, 140, 30)
                    : C16(224, 64, 26);
}
static void drawVents(GFXcanvas16 &cv) {
  for (auto &v : vents) {
    if (!v.live) continue;
    int sx = (int)(v.x - camX);
    if (sx < -20 || sx > VW) continue;
    float cyc = fmodf(anim + v.ph, 2.8f);
    bool on = cyc > 1.6f, warn = cyc > 1.35f && cyc <= 1.6f;
    cv.fillRect(sx - 4, GROUND_Y - 8, 12, 8, C16(74, 74, 86));  // nozzle
    cv.fillRect(sx - 2, GROUND_Y - 10, 8, 2, C16(138, 146, 164));
    if (on) {
      int H = 34 + (int)(sinf(anim * 34 + v.ph * 7) * 4);
      for (int k = 0; k < H; k += 4) {
        float t = k / (float)H;
        int w = (int)(11 * (1 - t * 0.55f));
        int cx = sx + 2 + (int)(sinf(anim * 18 + k * 0.7f) * 1.5f);
        cv.fillRect(cx - w / 2, GROUND_Y - 12 - k, w, 4, flameCol(t));
      }
    } else if (warn) {
      cv.fillRect(sx + 1, GROUND_Y - 12, 3, 3, C16(255, 230, 150));
    } else {
      cv.drawPixel(sx + 2, GROUND_Y - 11, C16(255, 120, 30));
    }
  }
}

static void drawShards(GFXcanvas16 &cv) {
  for (auto &s : shards) {
    if (!s.live || s.got) continue;
    int sx = (int)(s.x - camX);
    int sy = (int)(s.y + sinf(anim * 4 + s.x) * 2);
    if (sx < -6 || sx > VW) continue;
    if (s.kind == 0) {  // data shard — cyan diamond
      cv.drawFastHLine(sx - 3, sy, 7, COL_CYAN);
      cv.drawFastVLine(sx, sy - 3, 7, COL_CYAN);
      cv.drawPixel(sx, sy, COL_WHITE);
    } else {  // incident beacon — amber rotating diamond
      float pulse = 0.5f + 0.5f * sinf(anim * 6);
      int r = 4 + (int)(pulse * 3);
      cv.drawRect(sx - r, sy - r, r * 2, r * 2, COL_AMBER);
      cv.fillRect(sx - 3, sy - 3, 6, 6, COL_AMBER);
      cv.fillRect(sx - 1, sy - 1, 2, 2, COL_BLACK);
    }
  }
}

static void drawUnis(GFXcanvas16 &cv) {
  for (auto &u : unis) {
    if (!u.live || u.got) continue;
    int sx = (int)(u.x - camX);
    int sy = (int)(u.y + sinf(anim * 3 + u.x) * 2);
    if (sx < -14 || sx > VW) continue;
    float glow = 0.5f + 0.5f * sinf(anim * 5);
    if (glow > 0.6f) cv.drawRect(sx - 8, sy - 6, UNI_W + 4, UNI_H + 6, C16(120, 100, 30));
    blit(cv, UNI_ROWS, UNI_H, UNI_W, sx - UNI_W / 2, sy - 4, UNI_PAL, UNI_NP);
  }
}

static void drawDrones(GFXcanvas16 &cv) {
  for (auto &d : drones) {
    if (!d.live) continue;
    int sx = (int)(d.x - camX);
    if (sx < -DRONE_W || sx > VW) continue;
    int bob = (int)(sinf(anim * 6 + d.x) * 1.5f);
    blit(cv, DRONE_ROWS, DRONE_H, DRONE_W, sx, (int)d.y + bob, DRONE_PAL, DRONE_NP);
  }
}

static void drawPlayer(GFXcanvas16 &cv) {
  int f;
  if (mode == PLAY && invuln > 0 && ((int)(anim * 20) & 1)) return;  // hit blink
  if (!onGround) f = 2;                                              // jump
  else f = ((int)(anim * 12) & 1);                                  // run cycle
  blit(cv, deckFrame[f], 18, 12, PLAYER_SX - 1, (int)py, DECK_PAL, DECK_NP);
}

static void drawRain(GFXcanvas16 &cv, int district) {
  uint16_t rc = district == 1 ? C16(255, 224, 150) : C16(160, 190, 255);
  for (auto &r : drops) {
    r.y += r.v * 0.016f;
    r.x -= r.v * 0.0035f;
    if (r.y > VH + 6) {
      r.y = -8;
      r.x = rrange(0, VW);
    }
    if (r.x < 0) r.x += VW;
    int x = (int)r.x, y = (int)r.y;
    cv.drawLine(x, y, x - r.len, y + r.len * 3, rc);
  }
}

static void drawHud(GFXcanvas16 &cv) {
  // SLO bar
  cv.drawRect(6, 6, 90, 8, C16(42, 50, 64));
  int wfill = (int)(fmaxf(0.f, slo) / 100.f * 86);
  uint16_t bc = slo < 40 ? COL_RED : slo < 75 ? COL_AMBER : COL_CYAN;
  cv.fillRect(8, 8, wfill, 4, bc);
  cv.setTextSize(1);
  cv.setTextColor(COL_DIM);
  cv.setCursor(100, 7);
  cv.print("SLO ");
  cv.setTextColor(bc);
  cv.print((int)fmaxf(0.f, slo));
  cv.print("%");
  // score / best
  char buf[24];
  snprintf(buf, sizeof(buf), "%ld", scoreNow());
  cv.setTextColor(COL_WHITE);
  cv.setCursor(VW - 6 - (int)strlen(buf) * 6, 6);
  cv.print(buf);
  snprintf(buf, sizeof(buf), "BEST %u", best);
  cv.setTextColor(COL_DIM);
  cv.setCursor(VW - 6 - (int)strlen(buf) * 6, 16);
  cv.print(buf);
  if (uni > 0) {
    cv.setTextColor(COL_AMBER);
    cv.setCursor(6, 18);
    cv.print("uni ");
    cv.print((int)uni);
  }
}

static void centerText(GFXcanvas16 &cv, int y, const char *s, uint8_t sz, uint16_t col) {
  cv.setTextSize(sz);
  cv.setTextColor(col);
  cv.setCursor(VW / 2 - (int)strlen(s) * 3 * sz, y);
  cv.print(s);
}

static void drawScene(GFXcanvas16 &cv) {
  int district = (int)(camX / 1400.f) % 3;
  drawSky(cv, district);
  drawSkyline(cv, district);
  drawPlats(cv, district);
  drawVents(cv);
  drawShards(cv);
  drawUnis(cv);
  drawDrones(cv);
  drawPlayer(cv);
  drawRain(cv, district);
  drawHud(cv);
}

// ------------------------------------------------------------------ entry
void tracerunnerRun(Adafruit_ST7789 &tft) {
  buildDeckard();
  prefs.begin("tracerun", false);
  best = prefs.getUInt("best", 0);

  // seeded skyline + rain
  lcg = 0xC0FFEE;
  for (auto &b : far_) b = {(int)rrange(0, 1500), (int)rrange(20, 46), (int)rrange(30, 78), (uint8_t)(int)rrange(0, 255)};
  for (auto &b : mid_) b = {(int)rrange(0, 1700), (int)rrange(30, 60), (int)rrange(24, 60), (uint8_t)(int)rrange(0, 255)};
  for (auto &d : drops) d = {rrange(0, VW), rrange(0, VH), rrange(150, 320), (uint8_t)(rnd() < 0.3f ? 2 : 1)};

  GFXcanvas16 *cv = new GFXcanvas16(VW, VH);
  if (!cv || !cv->getBuffer()) {  // out of RAM — bail back to a black screen
    tft.fillScreen(COL_BLACK);
    for (;;) delay(1000);
  }
  cv->setTextWrap(false);

  mode = TITLE;
  anim = 0;
  litT = 0;
  litNext = 3.0f;
  litFlash = 0;
  resetRun();  // so the title screen has a world behind it

  bool prevBtn = false;
  uint32_t last = micros();
  for (;;) {
    uint32_t now = micros();
    float dt = (now - last) / 1000000.f;
    last = now;
    if (dt > 0.05f) dt = 0.05f;  // clamp after a stall
    anim += dt;

    // input — D1 is jump; D0/D2 also jump (forgiving). Edge = press.
    bool btn = btnDown(1) || btnDown(0) || btnDown(2);
    bool pressed = btn && !prevBtn;
    bool released = !btn && prevBtn;
    prevBtn = btn;

    if (mode == TITLE) {
      camX += 34 * dt;  // idle parallax drift behind the title
      ensureWorld();
      cullAll();
      if (pressed) {
        resetRun();
        mode = COUNT;
        countT = 3.2f;
      }
    } else if (mode == COUNT) {
      countT -= dt;
      if (countT <= 0) mode = PLAY;
    } else if (mode == PLAY) {
      if (pressed) buffer = BUFFER, jumpHeld = true;
      if (released) jumpHeld = false;
      if (!btn) jumpHeld = false;
      if (invuln > 0) invuln -= dt;
      stepPlay(dt);
      // SLO beacon (green→amber→red), unless mid hit-flash
      if (invuln <= 0.7f)
        beaconColor(slo < 40 ? 200 : slo < 75 ? 200 : 0, slo < 40 ? 0 : slo < 75 ? 90 : 140,
                    slo < 40 ? 0 : slo < 75 ? 0 : 40);
    } else if (mode == OVER) {
      if (pressed) {
        resetRun();
        mode = COUNT;
        countT = 3.2f;
      }
    }

    // lightning bookkeeping (visual only)
    litT += dt;
    if (litT >= litNext) {
      litT = 0;
      litNext = rrange(4.f, 11.f);
      litFlash = 0.4f;
    }
    if (litFlash > 0) litFlash -= dt;

    // ---- render ----
    drawScene(*cv);

    if (mode == TITLE) {
      cv->fillRect(0, 30, VW, 78, COL_BLACK);  // dim panel for legibility
      centerText(*cv, 36, "TRACE", 3, COL_RED);
      centerText(*cv, 60, "RUNNER", 3, COL_CYAN);
      centerText(*cv, 86, "starring AGENT DECODER", 1, COL_AMBER);
      if (((int)(anim * 2) & 1)) centerText(*cv, 100, "PRESS D1 TO GO ON SHIFT", 1, COL_WHITE);
      char b[24];
      snprintf(b, sizeof(b), "BEST %u", best);
      centerText(*cv, 118, b, 1, COL_DIM);
    } else if (mode == COUNT) {
      int n = (int)ceilf(countT - 0.35f);
      const char *s = n >= 1 ? (n == 3 ? "3" : n == 2 ? "2" : "1") : "RUN.";
      centerText(*cv, VH / 2 - 8, s, 3, n >= 1 ? COL_CYAN : COL_OK);
    } else if (mode == OVER) {
      cv->fillRect(0, 22, VW, 96, COL_BLACK);
      centerText(*cv, 28, "SLO BREACHED", 2, COL_RED);
      char b[28];
      snprintf(b, sizeof(b), "SCORE %ld", scoreNow());
      centerText(*cv, 50, b, 2, COL_CYAN);
      centerText(*cv, 70, rankFor(fmaxf(0.f, slo)), 1, COL_WHITE);
      snprintf(b, sizeof(b), "SLO %d%%  uni %d  cr %ld", (int)fmaxf(0.f, slo), (int)uni, credits);
      centerText(*cv, 84, b, 1, COL_DIM);
      snprintf(b, sizeof(b), "BEST %u", best);
      centerText(*cv, 96, b, 1, COL_AMBER);
      if (((int)(anim * 2) & 1)) centerText(*cv, 110, "PRESS D1 FOR NEXT SHIFT", 1, COL_WHITE);
    }

    tft.drawRGBBitmap(0, 0, cv->getBuffer(), VW, VH);

    // cap the frame rate (~60fps); yield so WiFi/idle housekeeping can run
    uint32_t spent = micros() - now;
    if (spent < 16000) delay((16000 - spent) / 1000);
    else delay(1);
  }
}
