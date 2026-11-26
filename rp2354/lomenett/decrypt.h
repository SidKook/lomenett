// decrypt.h -- the unlock animation shown during key derivation: padlock with
// a progress ring and "DECRYPT-ING"; success lifts the shackle ("ACCESS
// GRANTED"), failure shakes ("DENIED").
#pragma once

#define DECRYPT_WORKING 0
#define DECRYPT_SUCCESS 1
#define DECRYPT_FAILED  2
#define DEGREES_TO_RADIANS 0.0174533f

static inline float decryptEaseOut(float t) { if (t < 0) t = 0; if (t > 1) t = 1; return 1 - (1 - t) * (1 - t) * (1 - t); }

static inline void drawDottedArc(Adafruit_SSD1306 &gfx, int centerX, int centerY, float radius,
                                 int startDegrees, int endDegrees, int stepDegrees) {
  for (int degrees = startDegrees; degrees < endDegrees; degrees += stepDegrees) {
    float radians = degrees * DEGREES_TO_RADIANS;
    gfx.drawPixel(centerX + (int)lroundf(radius * cosf(radians)), centerY + (int)lroundf(radius * sinf(radians)), SSD1306_WHITE);
  }
}

static void drawLockRing(Adafruit_SSD1306 &gfx, int centerX, int centerY, int phase, uint32_t frameMs, float progress) {
  const int bodyW = 17, bodyH = 12;
  int bodyX = centerX - bodyW / 2, bodyY = centerY - 1;
  int shackleLift = (phase == DECRYPT_SUCCESS) ? (int)lroundf(4 * decryptEaseOut((frameMs % 700) / 350.0f)) : 0;
  const int shackleRadius = 5;
  int shackleTop = bodyY - 2 - shackleLift;
  for (int degrees = 180; degrees <= 360; degrees += 10) {
    float radians = degrees * DEGREES_TO_RADIANS;
    gfx.drawPixel(centerX + (int)lroundf(shackleRadius * cosf(radians)), shackleTop + (int)lroundf(shackleRadius * sinf(radians)), SSD1306_WHITE);
  }
  gfx.drawFastVLine(centerX + shackleRadius, shackleTop, bodyY - shackleTop, SSD1306_WHITE);
  gfx.drawFastVLine(centerX - shackleRadius, shackleTop, phase != DECRYPT_SUCCESS ? (bodyY - shackleTop) : 1, SSD1306_WHITE);
  gfx.drawRoundRect(bodyX, bodyY, bodyW, bodyH, 3, SSD1306_WHITE);
  gfx.fillCircle(centerX, bodyY + 5, 1, SSD1306_WHITE);              // keyhole
  gfx.fillRect(centerX, bodyY + 5, 1, 4, SSD1306_WHITE);
  if (phase == DECRYPT_WORKING) {                                     // scan line
    int scanY = bodyY + 2 + (int)((bodyH - 4) * ((frameMs % 500) / 500.0f));
    gfx.drawFastHLine(bodyX + 2, scanY, bodyW - 4, SSD1306_WHITE);
  }
  const float ringRadius = 15;
  if (phase == DECRYPT_WORKING) {
    int sweepDegrees = (int)(360 * decryptEaseOut(progress));
    drawDottedArc(gfx, centerX, centerY, ringRadius, -90, -90 + sweepDegrees, 3);
    drawDottedArc(gfx, centerX, centerY, ringRadius - 1, -90, -90 + sweepDegrees, 3);
    float headRadians = (-90 + sweepDegrees) * DEGREES_TO_RADIANS;
    gfx.fillCircle(centerX + (int)lroundf(ringRadius * cosf(headRadians)), centerY + (int)lroundf(ringRadius * sinf(headRadians)), 2, SSD1306_WHITE);
  } else if (phase == DECRYPT_SUCCESS) {
    drawDottedArc(gfx, centerX, centerY, ringRadius, -90, 270, 3);
    drawDottedArc(gfx, centerX, centerY, ringRadius - 1, -90, 270, 3);
  }
}

// phase: DECRYPT_WORKING / DECRYPT_SUCCESS / DECRYPT_FAILED; progress 0..1.
static void drawDecrypt(Adafruit_SSD1306 &gfx, float progress, int phase, uint32_t frameMs) {
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  gfx.clearDisplay(); gfx.setTextWrap(false); gfx.setTextSize(1); gfx.setTextColor(SSD1306_WHITE);
  if (!portrait) {
    int shakeX = (phase == DECRYPT_FAILED) ? (int)lroundf(sinf(frameMs / 18.0f) * 2) : 0;
    drawLockRing(gfx, 19 + shakeX, 16, phase, frameMs, progress);
    const int textX = 40;
    if (phase == DECRYPT_SUCCESS) {
      gfx.setCursor(textX, 5); gfx.print("ACCESS");
      gfx.setCursor(textX, 17); gfx.print("GRANTED");
    } else if (phase == DECRYPT_FAILED) {
      gfx.setCursor(textX, 11); gfx.print("DENIED");
    } else {
      int dotCount = 1 + (frameMs / 250) % 3;
      char dots[4] = "..."; dots[dotCount] = 0;
      gfx.setCursor(textX, 5); gfx.print("DECRYPT");
      gfx.setCursor(textX, 17); gfx.print("-ING"); gfx.print(dots);
      gfx.drawRect(textX, 27, width - textX - 2, 4, SSD1306_WHITE);
      gfx.fillRect(textX + 1, 28, (int)((width - textX - 4) * decryptEaseOut(progress)), 2, SSD1306_WHITE);
    }
  } else {
    drawLockRing(gfx, width / 2, 44, phase, frameMs, progress);
    const char *label = phase == DECRYPT_FAILED ? "NO" : phase == DECRYPT_SUCCESS ? "OK" : "DEC";
    gfx.setCursor(width / 2 - 3 * strlen(label) + 1, 72); gfx.print(label);
    if (phase == DECRYPT_WORKING) {
      gfx.drawRect(3, 84, width - 6, 4, SSD1306_WHITE);
      gfx.fillRect(4, 85, (int)((width - 8) * decryptEaseOut(progress)), 2, SSD1306_WHITE);
    }
  }
  gfx.display();
}
