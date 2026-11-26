// iconsheet.cpp -- renders every user icon into out/icons.ppm (16 per row).
#include "Arduino.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1306.h"
#include "Wire.h"
#include "LittleFS.h"
#include "lomenett_logic.h"
#include "ui.h"
#include <cstdio>

SerialT Serial; TwoWire Wire; FS LittleFS; uint32_t g_sim_millis = 0;
static bool t1LinkReset() { return false; }
static int t1SendApdu(const uint8_t *, int, uint8_t *, int) { return -1; }

int main() {
  Adafruit_SSD1306 display(128, 32, nullptr, -1);
  const int COLUMNS = 16, CELL = 20;
  int rows = (USER_ICON_COUNT + COLUMNS - 1) / COLUMNS;
  int imageWidth = COLUMNS * CELL, imageHeight = rows * CELL;
  static unsigned char image[200 * 400];
  memset(image, 0, sizeof(image));
  for (int icon = 0; icon < USER_ICON_COUNT; icon++) {
    display.clearDisplay();
    drawUserIcon(display, icon, 2, 2);
    int originX = (icon % COLUMNS) * CELL, originY = (icon / COLUMNS) * CELL;
    for (int y = 0; y < CELL; y++)
      for (int x = 0; x < CELL; x++) {
        int on = (display.buf[x + (y / 8) * 128] >> (y & 7)) & 1;
        image[(originY + y) * imageWidth + originX + x] = on ? 255 : 0;
      }
  }
  FILE *file = fopen("out/icons.ppm", "wb");
  fprintf(file, "P5\n%d %d\n255\n", imageWidth, imageHeight);
  fwrite(image, 1, imageWidth * imageHeight, file);
  fclose(file);
  printf("%d icons\n", USER_ICON_COUNT);
}
