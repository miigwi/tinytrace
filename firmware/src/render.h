#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "model.h"

// Adafruit_ST7789 (not TFT_eSPI, whose init crash-loops on this S3 board).
void renderInit(Adafruit_ST7789 &tft);
// renderStatus paints a full-screen message — boot, WiFi, provisioning states.
void renderStatus(Adafruit_ST7789 &tft, const char *line1, const char *line2);
// renderScreen paints one panel, branching on Screen::kind. index/total drive
// the position dots.
void renderScreen(Adafruit_ST7789 &tft, const Screen &s, int index, int total);
// renderScrollTick animates the marquee for long list rows; call every loop.
void renderScrollTick(Adafruit_ST7789 &tft, const Screen &s);
// drawMascot draws the robot face, used by the idle and all-clear states.
void drawMascot(Adafruit_ST7789 &tft, int cx, int cy, Mascot m, uint16_t color);
