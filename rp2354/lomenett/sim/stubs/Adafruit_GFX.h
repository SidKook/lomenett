// Mock Adafruit_GFX + Adafruit_SSD1306 for the host simulator. Draws into a
// 128x32 SSD1306-layout buffer with the real 5x7 font and rotation mapping,
// records text boxes (overlap check) and counts glyphs drawn off-screen.
#pragma once
#include "Arduino.h"
#include "glcdfont.h"
#include <vector>

#define SSD1306_BLACK 0
#define SSD1306_WHITE 1
#define SSD1306_INVERSE 2
#define SSD1306_DISPLAYON 0xAF
#define SSD1306_DISPLAYOFF 0xAE

struct SimBox { int x, y, w, h, run; };   // one glyph; `run` = which setCursor() call drew it

class Adafruit_GFX : public Print {
public:
  static const int PHYS_W = 128, PHYS_H = 32;
  uint8_t buf[PHYS_W * PHYS_H / 8];         // byte x + 128 * (y / 8), bit y % 8
  int rotation = 0;
  int cursor_x = 0, cursor_y = 0, textsize = 1, textcolor = 1, textbg = 1;
  bool wrap = true, hasBg = false;
  // instrumentation
  std::vector<SimBox> textRuns;
  int curRun = -1;
  int textClip = 0;                         // glyphs outside the logical screen

  int width() const { return (rotation & 1) ? PHYS_H : PHYS_W; }
  int height() const { return (rotation & 1) ? PHYS_W : PHYS_H; }
  void setRotation(int newRotation) { rotation = newRotation & 3; }
  uint8_t *getBuffer() { return buf; }

  void physSet(int x, int y, int color) {
    if (x < 0 || x >= PHYS_W || y < 0 || y >= PHYS_H) return;
    int byteIndex = x + (y / 8) * PHYS_W, bitMask = 1 << (y & 7);
    if (color == SSD1306_INVERSE) buf[byteIndex] ^= bitMask;
    else if (color) buf[byteIndex] |= bitMask;
    else buf[byteIndex] &= ~bitMask;
  }
  // Logical (rotated) pixel -> physical pixel, as the real driver does.
  void drawPixel(int x, int y, int color) {
    if (x < 0 || x >= width() || y < 0 || y >= height()) return;
    int physX = x, physY = y, swap;
    switch (rotation) {
      case 1: swap = physX; physX = PHYS_W - 1 - y; physY = swap; break;
      case 2: physX = PHYS_W - 1 - x; physY = PHYS_H - 1 - y; break;
      case 3: swap = physX; physX = y; physY = PHYS_H - 1 - swap; break;
    }
    physSet(physX, physY, color);
  }
  void clearDisplay() { memset(buf, 0, sizeof(buf)); textRuns.clear(); curRun = -1; textClip = 0; }
  void display() {}

  void drawFastHLine(int x, int y, int length, int color) { for (int i = 0; i < length; i++) drawPixel(x + i, y, color); }
  void drawFastVLine(int x, int y, int length, int color) { for (int i = 0; i < length; i++) drawPixel(x, y + i, color); }
  void fillRect(int x, int y, int w, int h, int color) { for (int row = 0; row < h; row++) drawFastHLine(x, y + row, w, color); }
  void drawRect(int x, int y, int w, int h, int color) {
    drawFastHLine(x, y, w, color); drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color); drawFastVLine(x + w - 1, y, h, color);
  }
  // Bresenham.
  void drawLine(int x0, int y0, int x1, int y1, int color) {
    int deltaX = abs(x1 - x0), deltaY = -abs(y1 - y0);
    int stepX = x0 < x1 ? 1 : -1, stepY = y0 < y1 ? 1 : -1;
    int error = deltaX + deltaY;
    for (;;) {
      drawPixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      int doubled = 2 * error;
      if (doubled >= deltaY) { error += deltaY; x0 += stepX; }
      if (doubled <= deltaX) { error += deltaX; y0 += stepY; }
    }
  }
  // Midpoint circle outline.
  void drawCircle(int centerX, int centerY, int radius, int color) {
    int decision = 1 - radius, ddX = 1, ddY = -2 * radius, x = 0, y = radius;
    drawPixel(centerX, centerY + radius, color); drawPixel(centerX, centerY - radius, color);
    drawPixel(centerX + radius, centerY, color); drawPixel(centerX - radius, centerY, color);
    while (x < y) {
      if (decision >= 0) { y--; ddY += 2; decision += ddY; }
      x++; ddX += 2; decision += ddX;
      drawPixel(centerX + x, centerY + y, color); drawPixel(centerX - x, centerY + y, color);
      drawPixel(centerX + x, centerY - y, color); drawPixel(centerX - x, centerY - y, color);
      drawPixel(centerX + y, centerY + x, color); drawPixel(centerX - y, centerY + x, color);
      drawPixel(centerX + y, centerY - x, color); drawPixel(centerX - y, centerY - x, color);
    }
  }
  void fillCircle(int centerX, int centerY, int radius, int color) {
    drawFastVLine(centerX, centerY - radius, 2 * radius + 1, color);
    for (int dx = 1; dx <= radius; dx++) {
      int halfHeight = (int)sqrt((double)radius * radius - dx * dx);
      drawFastVLine(centerX + dx, centerY - halfHeight, 2 * halfHeight + 1, color);
      drawFastVLine(centerX - dx, centerY - halfHeight, 2 * halfHeight + 1, color);
    }
  }
  void drawRoundRect(int x, int y, int w, int h, int radius, int color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    drawFastHLine(x + radius, y, w - 2 * radius, color); drawFastHLine(x + radius, y + h - 1, w - 2 * radius, color);
    drawFastVLine(x, y + radius, h - 2 * radius, color); drawFastVLine(x + w - 1, y + radius, h - 2 * radius, color);
    for (int dx = 0; dx <= radius; dx++) {           // approximate quarter-circle corners
      int dy = (int)sqrt((double)radius * radius - dx * dx);
      drawPixel(x + radius - dx, y + radius - dy, color);             drawPixel(x + w - 1 - radius + dx, y + radius - dy, color);
      drawPixel(x + radius - dx, y + h - radius + dy - 1, color);     drawPixel(x + w - 1 - radius + dx, y + h - radius + dy - 1, color);
    }
  }
  void fillRoundRect(int x, int y, int w, int h, int radius, int color) {
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    fillRect(x + radius, y, w - 2 * radius, h, color);
    for (int column = 0; column < radius; column++) {
      int bulge = (int)sqrt((double)radius * radius - (radius - column) * (radius - column));
      drawFastVLine(x + column, y + radius - bulge, h - 2 * (radius - bulge), color);
      drawFastVLine(x + w - 1 - column, y + radius - bulge, h - 2 * (radius - bulge), color);
    }
  }
  void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, int color) {
    drawLine(x0, y0, x1, y1, color); drawLine(x1, y1, x2, y2, color); drawLine(x2, y2, x0, y0, color);
  }
  // Fill every pixel inside all three edges (either winding).
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, int color) {
    int minX = min(x0, min(x1, x2)), maxX = max(x0, max(x1, x2)), minY = min(y0, min(y1, y2)), maxY = max(y0, max(y1, y2));
    auto edgeSide = [](int ax, int ay, int bx, int by, int px, int py) { return (px - ax) * (by - ay) - (py - ay) * (bx - ax); };
    for (int py = minY; py <= maxY; py++)
      for (int px = minX; px <= maxX; px++) {
        int side0 = edgeSide(x1, y1, x2, y2, px, py), side1 = edgeSide(x2, y2, x0, y0, px, py), side2 = edgeSide(x0, y0, x1, y1, px, py);
        if ((side0 >= 0 && side1 >= 0 && side2 >= 0) || (side0 <= 0 && side1 <= 0 && side2 <= 0)) drawPixel(px, py, color);
      }
  }
  // 1-bit bitmap, MSB first, rows padded to whole bytes.
  void drawBitmap(int x, int y, const uint8_t *bitmap, int w, int h, int color) {
    int bytesPerRow = (w + 7) / 8;
    for (int row = 0; row < h; row++)
      for (int column = 0; column < w; column++)
        if (pgm_read_byte(bitmap + row * bytesPerRow + column / 8) & (0x80 >> (column & 7))) drawPixel(x + column, y + row, color);
  }

  // ---- text
  void setCursor(int x, int y) { cursor_x = x; cursor_y = y; curRun++; }
  int getCursorX() { return cursor_x; }
  int getCursorY() { return cursor_y; }
  void setTextSize(int size) { textsize = size < 1 ? 1 : size; }
  void setTextColor(int color) { textcolor = color; hasBg = false; }
  void setTextColor(int color, int background) { textcolor = color; textbg = background; hasBg = true; }
  void setTextWrap(bool enabled) { wrap = enabled; }
  // 5x7 glyph: five column bytes, bit 0 = top row.
  void drawChar(int x, int y, unsigned char ch, int color, int size) {
    for (int column = 0; column < 5; column++) {
      uint8_t columnBits = pgm_read_byte(&glcdfont[ch * 5 + column]);
      for (int row = 0; row < 8; row++, columnBits >>= 1) {
        if (!(columnBits & 1)) continue;
        if (size == 1) drawPixel(x + column, y + row, color);
        else fillRect(x + column * size, y + row * size, size, size, color);
      }
    }
  }
  size_t write(uint8_t ch) override {
    if (ch == '\n') { cursor_x = 0; cursor_y += textsize * 8; return 1; }
    if (ch == '\r') return 1;
    if (wrap && cursor_x + textsize * 6 > width()) { cursor_x = 0; cursor_y += textsize * 8; }
    if (cursor_x < 0 || cursor_y < 0 || cursor_x + textsize * 5 > width() || cursor_y + textsize * 7 > height()) textClip++;
    drawChar(cursor_x, cursor_y, ch, textcolor, textsize);
    if (ch != ' ') textRuns.push_back({cursor_x, cursor_y, textsize * 5, textsize * 7, curRun});   // for overlap checks
    cursor_x += textsize * 6;
    return 1;
  }
  void beginRun() { curRun++; }
};

class Adafruit_SSD1306 : public Adafruit_GFX {
public:
  Adafruit_SSD1306(int, int, void *, int) {}
  bool begin(int = 0, int = 0) { return true; }
  void ssd1306_command(uint8_t) {}
};
