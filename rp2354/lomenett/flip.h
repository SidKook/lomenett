// flip.h -- transitions when the screen rotates, on the raw 128x32 physical
// framebuffer: a card flip (180) and a spin with a scale dip (+/-90).
#pragma once
#define FB_W 128
#define FB_H 32
#define FB_BYTES (FB_W * FB_H / 8)

// SSD1306 layout: byte x + 128 * (y / 8), bit y % 8.
static inline bool framebufferGet(const uint8_t *buffer, int x, int y) { return (buffer[x + (y >> 3) * FB_W] >> (y & 7)) & 1; }
static inline void framebufferSet(uint8_t *buffer, int x, int y) { buffer[x + (y >> 3) * FB_W] |= (1 << (y & 7)); }

static uint8_t rotationOldFrame[FB_BYTES];      // captured before the rotation changes
static bool    rotationAnimationPending = false;
static int     rotationDeltaDegrees = 0;        // +90 / -90 / 180

// Flip: squash oldFrame to the centre line, then grow newFrame out of it.
// axis 0 squashes vertically (180 flip); otherwise horizontally.
static void animateFlip(Adafruit_SSD1306 &oled, const uint8_t *oldFrame, const uint8_t *newFrame, int axis) {
  uint8_t *live = oled.getBuffer();
  static uint8_t newCopy[FB_BYTES];
  memcpy(newCopy, newFrame, sizeof(newCopy));
  const int STEPS = 12, centerX = FB_W / 2, centerY = FB_H / 2;
  for (int step = 1; step < STEPS; step++) {
    float t = (float)step / STEPS;
    const uint8_t *source = (t < 0.5f) ? oldFrame : newCopy;
    float scale = (t < 0.5f) ? (1 - t * 2) : (t - 0.5f) * 2;   // 1 -> 0 -> 1
    if (scale < 0.02f) scale = 0.02f;
    memset(live, 0, FB_BYTES);
    if (axis == 0) {
      for (int y = 0; y < FB_H; y++) {
        int sourceY = (int)lroundf((y - centerY) / scale) + centerY;
        if (sourceY < 0 || sourceY >= FB_H) continue;
        for (int x = 0; x < FB_W; x++) if (framebufferGet(source, x, sourceY)) framebufferSet(live, x, y);
      }
    } else {
      for (int x = 0; x < FB_W; x++) {
        int sourceX = (int)lroundf((x - centerX) / scale) + centerX;
        if (sourceX < 0 || sourceX >= FB_W) continue;
        for (int y = 0; y < FB_H; y++) if (framebufferGet(source, sourceX, y)) framebufferSet(live, x, y);
      }
    }
    oled.display();
  }
  memcpy(live, newCopy, sizeof(newCopy));
  oled.display();
}

// Spin: oldFrame turns 0 -> deltaDegrees/2, newFrame continues -delta/2 -> 0,
// shrinking to ~6% at the swap so the handoff is invisible.
static void animateRotate(Adafruit_SSD1306 &oled, const uint8_t *oldFrame, const uint8_t *newFrame, int deltaDegrees) {
  uint8_t *live = oled.getBuffer();
  static uint8_t newCopy[FB_BYTES];
  memcpy(newCopy, newFrame, sizeof(newCopy));
  const int STEPS = 15;
  const float centerX = FB_W / 2.0f, centerY = FB_H / 2.0f;
  for (int step = 1; step <= STEPS; step++) {
    float t = (float)step / STEPS;
    const uint8_t *source = (t < 0.5f) ? oldFrame : newCopy;
    float degrees = (t < 0.5f) ? (deltaDegrees * t) : (deltaDegrees * (t - 1.0f));
    float radians = degrees * 0.0174533f, cosine = cosf(radians), sine = sinf(radians);
    float scale = 1.0f - 0.94f * sinf(3.14159f * t);
    if (scale < 0.05f) scale = 0.05f;
    float inverseScale = 1.0f / scale;
    memset(live, 0, FB_BYTES);
    for (int y = 0; y < FB_H; y++) {
      float dy = y - centerY;
      for (int x = 0; x < FB_W; x++) {
        float dx = x - centerX;
        int sourceX = (int)lroundf((dx * cosine + dy * sine) * inverseScale + centerX);
        int sourceY = (int)lroundf((-dx * sine + dy * cosine) * inverseScale + centerY);
        if ((unsigned)sourceX < FB_W && (unsigned)sourceY < FB_H && framebufferGet(source, sourceX, sourceY))
          framebufferSet(live, x, y);
      }
    }
    oled.display();
  }
}
