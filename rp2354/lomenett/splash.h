// splash.h -- boot animation: the NL monogram draws itself, NULENKE slides
// in, then LOMENETT drops in letter by letter.
#pragma once
#include "logo.h"

struct Splash { uint32_t startedAt = 0; bool done = true; };

static inline float easeInOut(float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t < 0.5f ? 4*t*t*t : 1 - powf(-2*t + 2, 3) / 2;
}
static inline float easeOutBack(float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  const float overshoot = 1.70158f, overshootPlusOne = overshoot + 1;
  return 1 + overshootPlusOne * powf(t - 1, 3) + overshoot * powf(t - 1, 2);
}

// 2 px stroke: the line plus a copy offset by one pixel across it.
static inline void drawThickLine(Adafruit_GFX &gfx, float x0, float y0, float x1, float y1) {
  int startX = (int)lroundf(x0), startY = (int)lroundf(y0), endX = (int)lroundf(x1), endY = (int)lroundf(y1);
  gfx.drawLine(startX, startY, endX, endY, SSD1306_WHITE);
  if (fabsf(x1 - x0) >= fabsf(y1 - y0)) gfx.drawLine(startX, startY + 1, endX, endY + 1, SSD1306_WHITE);
  else                                  gfx.drawLine(startX + 1, startY, endX + 1, endY, SSD1306_WHITE);
}

// Draw the monogram in a box of height `size` at (x, y), with the pen at
// `progress` (0..1) of the total stroke length. End dots pop when reached.
static void logoDraw(Adafruit_GFX &gfx, int x, int y, int size, float progress) {
  float scale = size;
  auto pointX = [&](int i) { return x + LOGO_PTS[i][0] * scale; };
  auto pointY = [&](int i) { return y + LOGO_PTS[i][1] * scale; };

  float totalLength = 0;
  for (int path = 0; path < LOGO_NPATHS; path++) {
    int first = LOGO_PATHS[path][0], count = LOGO_PATHS[path][1];
    for (int i = 1; i < count; i++)
      totalLength += hypotf((LOGO_PTS[first+i][0] - LOGO_PTS[first+i-1][0]) * scale, (LOGO_PTS[first+i][1] - LOGO_PTS[first+i-1][1]) * scale);
  }
  float lengthBudget = progress * totalLength, drawnLength = 0;
  float penX = -1, penY = -1;
  bool dotReached[LOGO_NVIAS] = {0};
  auto markDotsNear = [&](float px, float py) {
    for (int dot = 0; dot < LOGO_NVIAS; dot++)
      if (hypotf(px - (x + LOGO_VIAS[dot][0] * scale), py - (y + LOGO_VIAS[dot][1] * scale)) < 1.6f) dotReached[dot] = true;
  };
  for (int path = 0; path < LOGO_NPATHS && lengthBudget > 0; path++) {
    int first = LOGO_PATHS[path][0], count = LOGO_PATHS[path][1];
    markDotsNear(pointX(first), pointY(first));
    for (int i = 1; i < count; i++) {
      float x0 = pointX(first + i - 1), y0 = pointY(first + i - 1);
      float x1 = pointX(first + i),     y1 = pointY(first + i);
      float segmentLength = hypotf(x1 - x0, y1 - y0);
      if (drawnLength + segmentLength <= lengthBudget) {
        drawThickLine(gfx, x0, y0, x1, y1);
        drawnLength += segmentLength; penX = x1; penY = y1; markDotsNear(x1, y1);
      } else {                                         // partial segment, then stop
        float fraction = (lengthBudget - drawnLength) / segmentLength;
        float endX = x0 + (x1 - x0) * fraction, endY = y0 + (y1 - y0) * fraction;
        drawThickLine(gfx, x0, y0, endX, endY);
        penX = endX; penY = endY; markDotsNear(endX, endY);
        lengthBudget = 0;
        break;
      }
    }
  }
  for (int dot = 0; dot < LOGO_NVIAS; dot++)
    if (dotReached[dot]) gfx.fillCircle((int)lroundf(x + LOGO_VIAS[dot][0] * scale), (int)lroundf(y + LOGO_VIAS[dot][1] * scale), 2, SSD1306_WHITE);
  if (progress < 0.999f && penX >= 0) gfx.fillCircle((int)lroundf(penX), (int)lroundf(penY), 2, SSD1306_WHITE);   // the pen
}

// Wipe that blacks out everything before `edge` along the long axis.
static void blackOutUpTo(Adafruit_SSD1306 &gfx, int edge, bool portrait, int width, int height) {
  if (portrait) gfx.fillRect(0, 0, width, edge, SSD1306_BLACK);
  else gfx.fillRect(0, 0, edge, height, SSD1306_BLACK);
}

// Draws one frame; returns false once the splash has finished.
static bool splashFrame(Adafruit_SSD1306 &gfx, Splash &splash, uint32_t now) {
  if (splash.done) return false;
  uint32_t elapsed = now - splash.startedAt;
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  gfx.clearDisplay(); gfx.setTextColor(SSD1306_WHITE); gfx.setTextWrap(false);   // sliding text must clip

  const uint32_t LOGO_DRAW_MS = 1500, TEXT_SLIDE_MS = 900, SCREEN1_END_MS = 2700, WIPE_MS = 250,
                 LETTER_GAP_MS = 90, FADE_START_MS = 4600, END_MS = 5000;
  if (elapsed < SCREEN1_END_MS) {
    // Screen 1: monogram draws, NULENKE slides in.
    int logoSize = portrait ? 24 : 26;
    int logoX = portrait ? (width - (int)(logoSize * LOGO_ASPECT)) / 2 : 3, logoY = portrait ? 4 : 3;
    logoDraw(gfx, logoX, logoY, logoSize, easeInOut(elapsed / (float)LOGO_DRAW_MS));
    if (elapsed > LOGO_DRAW_MS - 400) {
      float slide = easeOutBack((elapsed - (LOGO_DRAW_MS - 400)) / (float)TEXT_SLIDE_MS);
      if (!portrait) {
        gfx.setTextSize(2);
        int textX = (int)lroundf(width + 4 - slide * (width + 4 - 44));   // off-right to x=44
        gfx.setCursor(textX, 9); gfx.print("NULENKE");
      } else {
        gfx.setTextSize(1);
        const char *text = "NULENKE";
        for (int i = 0; i < 7; i++) {
          int textY = (int)lroundf(height + 8 - slide * (height + 8 - (40 + i * 10)));
          gfx.setCursor(width / 2 - 3, textY); gfx.write(text[i]);
        }
      }
    }
  } else if (elapsed < END_MS) {
    // Screen 2: LOMENETT letters drop in with a bounce.
    const char *text = "LOMENETT";
    const int letterCount = 8;
    uint32_t screen2Elapsed = elapsed - SCREEN1_END_MS;
    if (!portrait) {
      gfx.setTextSize(2);
      int textWidth = letterCount * 12, textX = (width - textWidth) / 2;
      for (int i = 0; i < letterCount; i++) {
        float drop = easeOutBack((screen2Elapsed - (WIPE_MS + i * LETTER_GAP_MS)) / 350.0f);
        if (drop < 0) continue;
        int letterY = (int)lroundf(-16 + drop * (9 + 16));
        gfx.setCursor(textX + i * 12, letterY); gfx.write(text[i]);
      }
      float underline = (screen2Elapsed - (WIPE_MS + letterCount * LETTER_GAP_MS + 200)) / 400.0f;   // after the last letter
      if (underline > 0) gfx.drawFastHLine(textX, 27, (int)(textWidth * easeInOut(underline)), SSD1306_WHITE);
    } else {
      gfx.setTextSize(1);
      for (int i = 0; i < letterCount; i++) {
        float drop = easeOutBack((screen2Elapsed - (WIPE_MS + i * LETTER_GAP_MS)) / 350.0f);
        if (drop < 0) continue;
        int letterX = (int)lroundf(-8 + drop * (8 + width / 2 - 3));
        gfx.setCursor(letterX, 24 + i * 10); gfx.write(text[i]);
      }
    }
    if (elapsed > FADE_START_MS) {                    // wipe out at the end
      float wipe = easeInOut((elapsed - FADE_START_MS) / (float)(END_MS - FADE_START_MS));
      blackOutUpTo(gfx, (int)(wipe * (portrait ? height : width)), portrait, width, height);
    }
  } else {
    splash.done = true;
    return false;
  }
  if (elapsed > SCREEN1_END_MS - WIPE_MS && elapsed < SCREEN1_END_MS + WIPE_MS) {   // wipe between screens
    float wipe = (elapsed - (SCREEN1_END_MS - WIPE_MS)) / (float)(2 * WIPE_MS);
    int edge = (int)(wipe * (portrait ? height : width));
    if (elapsed < SCREEN1_END_MS) blackOutUpTo(gfx, edge, portrait, width, height);
    if (portrait) gfx.drawFastHLine(0, edge, width, SSD1306_WHITE); else gfx.drawFastVLine(edge, 0, height, SSD1306_WHITE);
  }
  gfx.display();
  return true;
}
