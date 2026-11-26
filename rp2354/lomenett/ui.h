// ui.h -- the 128x32 UI: status bar, icon carousel menus, vault lists, PIN
// entry, text builders, and a nav stack (hold = back one level). Drawing and
// UI state only; the sketch owns power, USB and the I2C bus.
#pragma once

static const uint16_t PIXEL_ON = SSD1306_WHITE;
#define DEG_TO_RAD_F 0.0174533f

// ---- 16x16 bitmap icons, 1 bit per pixel, MSB first, row by row -----------
static const uint8_t PROGMEM ICON_LOCK[32] = {
  0x07,0xE0, 0x0C,0x30, 0x18,0x18, 0x18,0x18, 0x18,0x18, 0x7F,0xFE, 0x7F,0xFE, 0x7F,0xFE,
  0x7E,0x7E, 0x7E,0x7E, 0x7F,0x7E, 0x7F,0x7E, 0x7F,0xFE, 0x7F,0xFE, 0x3F,0xFC, 0x00,0x00 };
static const uint8_t PROGMEM ICON_KEY[32] = {
  0x00,0x00, 0x00,0x00, 0x3C,0x00, 0x7E,0x00, 0xE7,0x00, 0xC3,0x00, 0xC3,0xFF, 0xE7,0xFF,
  0x7E,0x0F, 0x3C,0x0F, 0x00,0x0D, 0x00,0x0D, 0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00 };
static const uint8_t PROGMEM ICON_KEYBOARD[32] = {
  0x00,0x00, 0x00,0x00, 0x00,0x00, 0x7F,0xFE, 0x80,0x01, 0xAA,0xAD, 0x80,0x01, 0xAA,0xAD,
  0x80,0x01, 0xAA,0xAD, 0x80,0x01, 0x9F,0xF9, 0x80,0x01, 0x7F,0xFE, 0x00,0x00, 0x00,0x00 };
static const uint8_t PROGMEM ICON_GEAR[32] = {
  0x01,0x80, 0x19,0x98, 0x3D,0xBC, 0x1F,0xF8, 0x1E,0x78, 0x3C,0x3C, 0xF8,0x1F, 0xF8,0x1F,
  0xF8,0x1F, 0xF8,0x1F, 0x3C,0x3C, 0x1E,0x78, 0x1F,0xF8, 0x3D,0xBC, 0x19,0x98, 0x01,0x80 };
static const uint8_t PROGMEM ICON_INFO[32] = {
  0x07,0xE0, 0x1F,0xF8, 0x3F,0xFC, 0x7E,0x7E, 0x7E,0x7E, 0xFF,0xFF, 0xFC,0x3F, 0xFE,0x7F,
  0xFE,0x7F, 0xFE,0x7F, 0xFE,0x7F, 0x7C,0x3E, 0x7F,0xFE, 0x3F,0xFC, 0x1F,0xF8, 0x07,0xE0 };
static const uint8_t PROGMEM ICON_POWER[32] = {
  0x01,0x80, 0x01,0x80, 0x19,0x98, 0x39,0x9C, 0x71,0x8E, 0x61,0x86, 0xE1,0x87, 0xE1,0x87,
  0xE0,0x07, 0xE0,0x07, 0x60,0x06, 0x70,0x0E, 0x38,0x1C, 0x1F,0xF8, 0x07,0xE0, 0x00,0x00 };
static const uint8_t PROGMEM ICON_EDIT[32] = {
  0x00,0x00, 0x00,0x06, 0x00,0x0F, 0x00,0x1F, 0x00,0x3E, 0x00,0x7C, 0x00,0xF8, 0x01,0xF0,
  0x03,0xE0, 0x07,0xC0, 0x0F,0x80, 0x1F,0x00, 0x1E,0x00, 0x18,0x00, 0x7F,0xFE, 0x00,0x00 };
static const uint8_t PROGMEM ICON_RESET[32] = {
  0x00,0x00, 0x07,0xE0, 0x1F,0xF8, 0x3C,0x3C, 0x30,0x7F, 0x70,0x3E, 0x60,0x1C, 0x60,0x08,
  0x60,0x00, 0x60,0x00, 0x70,0x0E, 0x30,0x0C, 0x3C,0x3C, 0x1F,0xF8, 0x07,0xE0, 0x00,0x00 };
static const uint8_t PROGMEM ICON_BOLT[7] = { 0x30, 0x60, 0xC0, 0xF8, 0x18, 0x30, 0x60 };   // 5x7 charging bolt

#include "icons_user.h"

// ---- icons ----------------------------------------------------------------
// Ids >= 128 are user entry icons: 128 = default key, then USER_PICTO
// pictograms, then letters A-Z and digits 0-9.
#define USER_ICON_COUNT (1 + USER_PICTO_N + 36)
#define USER_ICON(n) ((uint8_t)(128 + (n)))
enum IconId : uint8_t {
  IC_KEYBOARD, IC_CARD, IC_EDIT, IC_GEAR, IC_POWER, IC_KEY, IC_USER, IC_BOTH, IC_SLIDERS, IC_BACK,
  IC_TAB, IC_ENTER, IC_STAR, IC_TRASH, IC_NUMBER, IC_CALENDAR, IC_CVV, IC_TYPEOLD, IC_TYPENEW, IC_AGAIN, IC_SAVE, IC_REGEN,
  IC_PLUS, IC_PIN, IC_BACKUP, IC_RESTORE, IC_WHEEL, IC_CLOCK, IC_WIPE, IC_RESET, IC_INFO, IC_YES, IC_NO, IC_LOCK, IC_SMILEY, IC_OTP };

static void drawIconArc(Adafruit_GFX &gfx, int centerX, int centerY, int radius, int startDegrees, int endDegrees) {
  for (int degrees = startDegrees; degrees < endDegrees; degrees += 12) {
    float radians = degrees * DEG_TO_RAD_F;
    gfx.drawPixel(centerX + (int)lroundf(radius * cosf(radians)), centerY + (int)lroundf(radius * sinf(radians)), PIXEL_ON);
  }
}

// User icon number: 0 = key, 1..USER_PICTO_N = pictogram, then 26 letters, then 10 digits.
static char userIconLetter(int iconNumber) {
  int index = iconNumber - 1 - USER_PICTO_N;
  return index < 26 ? 'A' + index : '0' + (index - 26);
}

static void drawUserIcon(Adafruit_GFX &gfx, int iconNumber, int x, int y) {
  if (iconNumber <= 0 || iconNumber >= USER_ICON_COUNT) { gfx.drawBitmap(x, y, ICON_KEY, 16, 16, PIXEL_ON); return; }
  if (iconNumber <= USER_PICTO_N) { gfx.drawBitmap(x, y, USER_PICTO[iconNumber - 1], 16, 16, PIXEL_ON); return; }
  gfx.setTextSize(2); gfx.setTextColor(PIXEL_ON); gfx.setCursor(x + 3, y + 1);
  gfx.write(userIconLetter(iconNumber));
  gfx.setTextSize(1);
}

static void userIconName(int iconNumber, char *name, int capacity) {
  if (iconNumber <= 0 || iconNumber >= USER_ICON_COUNT) { strncpy(name, "KEY", capacity); return; }
  if (iconNumber <= USER_PICTO_N) { strncpy(name, USER_PICTO_NAME[iconNumber - 1], capacity); name[capacity - 1] = 0; return; }
  name[0] = userIconLetter(iconNumber); name[1] = 0;
}

// Draw a 16x16 icon with its top-left at (x, y).
static void drawIcon(Adafruit_GFX &gfx, uint8_t id, int x, int y) {
  const uint16_t C = PIXEL_ON;
  int centerX = x + 8, centerY = y + 8;
  if (id >= 128) { drawUserIcon(gfx, id - 128, x, y); return; }
  switch (id) {
    case IC_KEYBOARD: gfx.drawBitmap(x, y, ICON_KEYBOARD, 16, 16, C); break;
    case IC_EDIT:     gfx.drawBitmap(x, y, ICON_EDIT, 16, 16, C); break;
    case IC_GEAR:     gfx.drawBitmap(x, y, ICON_GEAR, 16, 16, C); break;
    case IC_POWER:    gfx.drawBitmap(x, y, ICON_POWER, 16, 16, C); break;
    case IC_KEY:      gfx.drawBitmap(x, y, ICON_KEY, 16, 16, C); break;
    case IC_INFO:     gfx.drawBitmap(x, y, ICON_INFO, 16, 16, C); break;
    case IC_LOCK:     gfx.drawBitmap(x, y, ICON_LOCK, 16, 16, C); break;
    case IC_RESET:    gfx.drawBitmap(x, y, ICON_RESET, 16, 16, C); break;
    case IC_CARD:
      gfx.drawRoundRect(x + 1, y + 3, 14, 10, 2, C); gfx.drawFastHLine(x + 1, y + 6, 14, C); gfx.drawFastHLine(x + 3, y + 10, 4, C);
      break;
    case IC_USER:
      gfx.fillCircle(centerX, y + 5, 3, C); gfx.fillRoundRect(x + 3, y + 9, 10, 6, 3, C);
      break;
    case IC_BOTH:     // person + key
      gfx.fillCircle(x + 4, y + 5, 2, C); gfx.fillRoundRect(x + 1, y + 8, 7, 5, 2, C);
      gfx.drawCircle(x + 11, y + 11, 2, C); gfx.drawLine(x + 12, y + 9, x + 15, y + 6, C); gfx.drawFastVLine(x + 14, y + 6, 2, C);
      break;
    case IC_SLIDERS:
      gfx.drawFastHLine(x + 1, y + 3, 14, C);  gfx.fillRect(x + 4, y + 2, 3, 3, C);
      gfx.drawFastHLine(x + 1, y + 8, 14, C);  gfx.fillRect(x + 10, y + 7, 3, 3, C);
      gfx.drawFastHLine(x + 1, y + 13, 14, C); gfx.fillRect(x + 6, y + 12, 3, 3, C);
      break;
    case IC_BACK:     // chevron, 2 px
      gfx.drawLine(x + 11, y + 3, x + 5, centerY, C); gfx.drawLine(x + 5, centerY, x + 11, y + 13, C);
      gfx.drawLine(x + 11, y + 4, x + 6, centerY, C); gfx.drawLine(x + 6, centerY, x + 11, y + 12, C);
      break;
    case IC_TAB:
      gfx.drawFastHLine(x + 1, centerY, 10, C); gfx.drawLine(x + 8, centerY - 3, x + 11, centerY, C);
      gfx.drawLine(x + 8, centerY + 3, x + 11, centerY, C); gfx.drawFastVLine(x + 13, y + 3, 11, C);
      break;
    case IC_ENTER:
      gfx.drawFastVLine(x + 12, y + 3, 6, C); gfx.drawFastHLine(x + 4, y + 8, 9, C);
      gfx.drawLine(x + 4, y + 8, x + 7, y + 5, C); gfx.drawLine(x + 4, y + 8, x + 7, y + 11, C);
      break;
    case IC_STAR: {   // filled 5-point star from 10 triangles
      int pointX[10], pointY[10];
      for (int i = 0; i < 10; i++) {
        float radius = (i & 1) ? 3.2f : 7.5f, angle = (-90 + i * 36) * DEG_TO_RAD_F;
        pointX[i] = centerX + (int)lroundf(radius * cosf(angle));
        pointY[i] = centerY + 1 + (int)lroundf(radius * sinf(angle));
      }
      for (int i = 0; i < 10; i++)
        gfx.fillTriangle(centerX, centerY + 1, pointX[i], pointY[i], pointX[(i + 1) % 10], pointY[(i + 1) % 10], C);
    } break;
    case IC_TRASH:
      gfx.drawRect(x + 3, y + 4, 10, 11, C); gfx.drawFastHLine(x + 1, y + 3, 14, C); gfx.fillRect(x + 6, y + 1, 4, 2, C);
      gfx.drawFastVLine(x + 6, y + 6, 7, C); gfx.drawFastVLine(x + 9, y + 6, 7, C);
      break;
    case IC_NUMBER:   // '#'
      gfx.drawFastVLine(x + 5, y + 2, 13, C); gfx.drawFastVLine(x + 10, y + 2, 13, C);
      gfx.drawFastHLine(x + 2, y + 5, 13, C); gfx.drawFastHLine(x + 2, y + 11, 13, C);
      break;
    case IC_CALENDAR:
      gfx.drawRect(x + 2, y + 3, 12, 11, C); gfx.drawFastHLine(x + 2, y + 6, 12, C);
      gfx.drawFastVLine(x + 5, y + 1, 3, C); gfx.drawFastVLine(x + 10, y + 1, 3, C);
      break;
    case IC_CVV:
      gfx.drawRoundRect(x + 1, y + 3, 14, 10, 2, C);
      gfx.fillRect(x + 3, y + 7, 2, 3, C); gfx.fillRect(x + 7, y + 7, 2, 3, C); gfx.fillRect(x + 11, y + 7, 2, 3, C);
      break;
    case IC_TYPEOLD:  // page + arrow out
      gfx.drawRect(x + 2, y + 2, 8, 12, C); gfx.drawFastHLine(x + 4, y + 5, 4, C); gfx.drawFastHLine(x + 4, y + 8, 4, C);
      gfx.drawLine(x + 9, y + 11, x + 15, y + 11, C); gfx.drawLine(x + 13, y + 9, x + 15, y + 11, C); gfx.drawLine(x + 13, y + 13, x + 15, y + 11, C);
      break;
    case IC_TYPENEW:  // sparkle
      gfx.drawLine(centerX, y + 1, centerX, y + 15, C); gfx.drawLine(x + 1, centerY, x + 15, centerY, C);
      gfx.drawLine(x + 3, y + 3, x + 13, y + 13, C); gfx.drawLine(x + 13, y + 3, x + 3, y + 13, C); gfx.fillCircle(centerX, centerY, 1, C);
      break;
    case IC_AGAIN:    // double chevron
      for (int offset = 4; offset <= 8; offset += 4) {
        gfx.drawLine(x + offset, y + 4, x + offset + 4, centerY, C);
        gfx.drawLine(x + offset + 4, centerY, x + offset, y + 12, C);
      }
      break;
    case IC_SAVE:     // circled tick
      gfx.drawCircle(centerX, centerY, 7, C); gfx.drawLine(x + 4, centerY, x + 7, y + 11, C); gfx.drawLine(x + 7, y + 11, x + 12, y + 4, C);
      break;
    case IC_REGEN:    // two arcs with arrowheads
      drawIconArc(gfx, centerX, centerY, 6, 200, 360); drawIconArc(gfx, centerX, centerY, 6, 20, 180);
      gfx.drawLine(x + 13, y + 3, x + 14, y + 7, C); gfx.drawLine(x + 13, y + 3, x + 10, y + 5, C);
      gfx.drawLine(x + 3, y + 13, x + 2, y + 9, C); gfx.drawLine(x + 3, y + 13, x + 6, y + 11, C);
      break;
    case IC_PLUS:
      gfx.fillRect(x + 2, centerY - 1, 13, 3, C); gfx.fillRect(centerX - 1, y + 2, 3, 13, C);
      break;
    case IC_PIN:      // four dots over a line
      for (int i = 0; i < 4; i++) gfx.fillCircle(x + 2 + i * 4, centerY - 1, 1, C);
      gfx.drawFastHLine(x + 1, y + 12, 14, C);
      break;
    case IC_BACKUP:   // tray + down arrow
      gfx.drawFastHLine(x + 2, y + 14, 13, C); gfx.drawFastVLine(x + 2, y + 11, 3, C); gfx.drawFastVLine(x + 14, y + 11, 3, C);
      gfx.drawFastVLine(centerX, y + 1, 9, C); gfx.drawLine(centerX - 3, y + 6, centerX, y + 9, C); gfx.drawLine(centerX + 3, y + 6, centerX, y + 9, C);
      break;
    case IC_RESTORE:  // tray + up arrow
      gfx.drawFastHLine(x + 2, y + 14, 13, C); gfx.drawFastVLine(x + 2, y + 11, 3, C); gfx.drawFastVLine(x + 14, y + 11, 3, C);
      gfx.drawFastVLine(centerX, y + 2, 9, C); gfx.drawLine(centerX - 3, y + 5, centerX, y + 2, C); gfx.drawLine(centerX + 3, y + 5, centerX, y + 2, C);
      break;
    case IC_WHEEL:
      gfx.drawCircle(centerX, centerY, 7, C); gfx.drawCircle(centerX, centerY, 2, C);
      gfx.drawFastVLine(centerX, y + 1, 3, C); gfx.drawFastVLine(centerX, y + 13, 3, C);
      gfx.drawFastHLine(x + 1, centerY, 3, C); gfx.drawFastHLine(x + 13, centerY, 3, C);
      break;
    case IC_CLOCK:
      gfx.drawCircle(centerX, centerY, 7, C); gfx.drawFastVLine(centerX, centerY - 4, 5, C); gfx.drawFastHLine(centerX, centerY, 4, C);
      break;
    case IC_WIPE:     // boxed X
      gfx.drawRect(x + 2, y + 2, 12, 12, C); gfx.drawLine(x + 4, y + 4, x + 11, y + 11, C); gfx.drawLine(x + 11, y + 4, x + 4, y + 11, C);
      break;
    case IC_YES:      // tick, 2 px
      gfx.drawLine(x + 2, centerY, x + 6, y + 13, C); gfx.drawLine(x + 6, y + 13, x + 14, y + 3, C);
      gfx.drawLine(x + 2, centerY + 1, x + 6, y + 14, C); gfx.drawLine(x + 6, y + 14, x + 14, y + 4, C);
      break;
    case IC_SMILEY:
      gfx.drawCircle(centerX, centerY, 7, C); gfx.fillRect(x + 5, y + 5, 2, 2, C); gfx.fillRect(x + 9, y + 5, 2, 2, C);
      drawIconArc(gfx, centerX, centerY, 4, 25, 155);
      break;
    case IC_OTP:      // shield with a clock face
      gfx.drawFastHLine(x + 2, y + 1, 13, C); gfx.drawFastVLine(x + 2, y + 1, 8, C); gfx.drawFastVLine(x + 14, y + 1, 8, C);
      gfx.drawLine(x + 2, y + 9, centerX, y + 15, C); gfx.drawLine(x + 14, y + 9, centerX, y + 15, C);
      gfx.drawCircle(centerX, y + 7, 3, C); gfx.drawFastVLine(centerX, y + 5, 2, C); gfx.drawPixel(centerX + 1, y + 7, C);
      break;
    case IC_NO:       // X, 2 px
      gfx.drawLine(x + 3, y + 3, x + 13, y + 13, C); gfx.drawLine(x + 13, y + 3, x + 3, y + 13, C);
      gfx.drawLine(x + 4, y + 3, x + 14, y + 13, C); gfx.drawLine(x + 12, y + 3, x + 2, y + 13, C);
      break;
  }
}

// ---- shared menu widget --------------------------------------------------
struct MenuItem { const char *label; const char *shortLabel; uint8_t icon; };   // shortLabel <= 5 chars (portrait)
struct MenuState {
  int   selected = 0;
  float position = 0;          // animated, wraps in [0, itemCount)
  float clickBounce = 0;       // 1 -> 0 ring after a click
  float targetPosition = 0;    // unwrapped, so the ring spins the way you scrolled
};

enum UiScreen { UI_HOME, UI_PIN, UI_INFO, UI_MSG, UI_VAULT, UI_CARDS, UI_PWMENU, UI_PWSET, UI_CARDMENU, UI_SETTINGS,
                UI_EDITMODE, UI_CHGPW, UI_NEWLABEL, UI_NEWTYPE, UI_NEWNUM, UI_CONFIRM, UI_ICONPICK, UI_CODES, UI_CODE,
                UI_CODESET, UI_BACKUPS, UI_CLOCKASK, UI_CLOCKSYNC, UI_LOGIN, UI_SCREEN_COUNT };
enum UiPinMode { PIN_UNLOCK, PIN_CURRENT, PIN_NEW, PIN_CONFIRM, PIN_BACKUP, PIN_RESTORE };

// Actions handed to the sketch through ui.pendingAction.
#define ACT_OFF        99     // orderly power off
#define UI_OFF         ACT_OFF
#define ACT_WHEELDIR   100
#define ACT_RESET      101    // factory reset
#define ACT_PREFS      102    // a global pref changed: save
#define ACT_RESTORE    103
#define ACT_AUTOWIPE   104
#define ACT_INFO       110    // fill the INFO lines (asks the SE050)
#define ACT_NEWPW      200    // create password entry ui.newLabel
#define ACT_NEWCARD    201    // create card from ui.newLabel / newCardNumber / newCardExpiry / newCardCvv
#define ACT_CARDNUM    210    // type card number of ui.cardIndex
#define ACT_CARDEXP    211    //   ... expiry
#define ACT_CARDCVV    212    //   ... CVV
#define ACT_ENTRYSET   230    // per-entry settings changed: rewrite the vault
#define ACT_DELETE     231    // delete ui.entryIndex
#define ACT_TOTPTYPE   240    // type the current 2FA code of ui.entryIndex
#define ACT_TOTPBACKUP 241    // type backup code ui.backupCodeIndex of ui.entryIndex
#define ACT_CLOCKSYNC  242    // type the clock-sync command for ui.clockSyncOs

// ui.typeMode
#define TYPE_PASSWORD          0
#define TYPE_USERNAME          1
#define TYPE_USER_AND_PASSWORD 2   // username, TABs, password

// ui.changeAction (password-change screen), consumed by the sketch
#define CHANGE_NONE           -1
#define CHANGE_TYPE_OLD        0
#define CHANGE_TYPE_NEW        1   // generates a candidate first if needed
#define CHANGE_TYPE_AGAIN      2
#define CHANGE_SAVE            3
#define CHANGE_REGENERATE      4   // new candidate, then type it
#define CHANGE_GENERATE_SILENT 10  // on entering the screen

// ui.confirmKind
#define CONFIRM_FACTORY_RESET 0
#define CONFIRM_DELETE_ENTRY  1

// ui.editState (EDIT / USB drive mode)
#define EDIT_IDLE    0
#define EDIT_WRITING 1
#define EDIT_SAVED   2
#define EDIT_EJECTED 3
#define EDIT_BAD     4

// Clock-sync target computer
#define OS_LINUX   0
#define OS_MAC     1
#define OS_WINDOWS 2

#define NAV_STACK_DEPTH 8
#define LOGIN_MAX_MATCHES 12

struct Ui {
  UiScreen screen = UI_HOME;
  UiScreen navStack[NAV_STACK_DEPTH]; int navDepth = 0;
  MenuState menus[UI_SCREEN_COUNT];

  // Toast message (UI_MSG)
  const char *toastText = "";
  uint32_t toastUntil = 0;
  UiScreen toastReturnScreen = UI_HOME;

  // Typing requests for the sketch
  int  typeEntryRequest = -1;      // entry to type, -1 none
  int  typeMode = TYPE_PASSWORD;
  bool typeForceEnter = false;     // press Enter regardless of the entry setting (LOG IN)

  // LOG IN: entries matching the browser's "site <host> <mode>"
  int  loginMatches[LOGIN_MAX_MATCHES]; int loginMatchCount = 0;
  int  loginMode = TYPE_USER_AND_PASSWORD;
  int  loginLastPicked = -1;       // kept for the following password step
  char loginHost[24] = "";

  int entryIndex = -1;             // entry behind PWMENU / PWSET / CARDMENU / CODE
  int cardIndex = -1;              // == entryIndex while in CARDMENU
  int confirmKind = CONFIRM_FACTORY_RESET;
  int backupCodeIndex = -1;        // 2FA backup code to type
  int clockSyncOs = -1;            // OS_*, -1 = not picked yet (saved in prefs)

  // PIN entry
  int   pinDigits[4] = {0,0,0,0};
  int   pinSlot = 0;
  float pinRoll = 0;               // drum animation for the active digit
  float pinShake = 0;              // 1 -> 0 wrong-PIN shake
  char  pinTyped[5] = "";
  bool  pinSubmit = false;         // 4 digits entered; the sketch consumes it
  char  newPin[5] = "";
  UiPinMode pinMode = PIN_UNLOCK;
  bool  busy = false;              // "CHECKING" while the sketch derives keys

  // Preferences
  bool    autoEnter = false;       // legacy global; Enter is per entry now
  bool    autoWipe = false;        // wipe after N wrong PINs (needs a backup)
  bool    haveBackup = false;      // a backup was exported at least once
  uint8_t autoOffSeconds = 30;     // idle power-off on battery, 0 = never

  // Change-password flow
  bool pickingForPasswordChange = false;
  int  changeEntryIndex = -1;
  bool changeHasCandidate = false;
  int  changeAction = CHANGE_NONE;

  // New-entry text builders
  char  newLabel[24] = ""; int newLabelLength = 0;
  int   builderSelected = 0; float builderPosition = 0; float builderTarget = 0;
  char  newCardNumber[20] = ""; char newCardExpiry[5] = ""; char newCardCvv[5] = "";
  int   newCardStage = 0;          // 0 number, 1 expiry, 2 CVV

  // EDIT mode
  int editState = EDIT_IDLE;
  uint32_t editStateSince = 0;

  // INFO screen
  const char *infoLine1 = "";
  const char *infoLine2 = "";

  uint32_t lastFrameMs = 0;
  uint32_t introStartedAt = 0;     // home-screen wipe-in
  float fps = 0;
  bool  locked = true;
  int   pendingAction = -1;        // ACT_* for the sketch, -1 none
};

static inline float easeOut(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }

// Frame-rate independent ease: move `rate` of the gap per 16.7 ms.
static inline float approach(float current, float target, float rate, float dtMs) {
  float fraction = 1 - powf(1 - rate, dtMs / 16.7f);
  return current + (target - current) * fraction;
}

// Animate a wrap-around ring of itemCount items. The wheel moves an
// unwrapped target so the animation always travels the way you scrolled;
// selections set directly elsewhere take the short way round.
static void ringStep(int &selected, float &position, float &target, int itemCount, int wheelDelta,
                     float rate, float snapDistance, float dtMs) {
  if (itemCount <= 0) return;
  if (((int)lroundf(target) % itemCount + itemCount) % itemCount != selected) {   // target out of sync
    target = selected;
    float distance = target - position;
    if (distance > itemCount / 2.0f) target -= itemCount;
    else if (distance < -itemCount / 2.0f) target += itemCount;
  }
  if (wheelDelta) {
    target = lroundf(target) + wheelDelta;
    selected = ((int)lroundf(target) % itemCount + itemCount) % itemCount;
  }
  while (target - position >= itemCount) target -= itemCount;   // at most one lap of spin
  while (target - position <= -itemCount) target += itemCount;
  position = approach(position, target, rate, dtMs);
  if (fabsf(position - target) < snapDistance) position = target;
  float wholeLaps = floorf(position / itemCount) * itemCount;   // renormalise both together
  position -= wholeLaps; target -= wholeLaps;
}

// ---- navigation: push on enter, hold pops one level -----------------------
static void uiGo(Ui &ui, UiScreen screen) {
  if (ui.navDepth < NAV_STACK_DEPTH) ui.navStack[ui.navDepth++] = ui.screen;
  ui.screen = screen;
  ui.menus[screen] = MenuState();
}
static void uiBackOne(Ui &ui) { ui.screen = ui.navDepth > 0 ? ui.navStack[--ui.navDepth] : UI_HOME; }
static void uiHome(Ui &ui) { ui.navDepth = 0; ui.screen = UI_HOME; }
static void uiBackTo(Ui &ui, UiScreen screen) {
  while (ui.navDepth > 0 && ui.screen != screen) uiBackOne(ui);
  if (ui.screen != screen) uiHome(ui);
}
// Show a message until `until`, then go to returnScreen (UI_HOME clears the stack).
static void uiToast(Ui &ui, const char *text, uint32_t until, UiScreen returnScreen) {
  ui.toastText = text; ui.toastUntil = until; ui.toastReturnScreen = returnScreen; ui.screen = UI_MSG;
}

// Print only the 6-px glyphs that fit fully inside [minX, maxX).
static void printClipped(Adafruit_GFX &gfx, int x, int y, const char *text, int minX, int maxX) {
  int length = strlen(text), first = 0, last = length;
  while (first < length && x + first * 6 < minX) first++;
  while (last > first && x + last * 6 > maxX) last--;
  if (last <= first) return;
  char visible[48];
  int visibleLength = last - first;
  if (visibleLength > 47) visibleLength = 47;
  memcpy(visible, text + first, visibleLength); visible[visibleLength] = 0;
  gfx.setCursor(x + first * 6, y); gfx.print(visible);
}

#include "vault.h"

static void uiDrawBattery(Adafruit_GFX &gfx, int x, int y, int percent, bool charging) {
  gfx.drawRect(x, y, 12, 6, PIXEL_ON);                       // body
  gfx.drawFastVLine(x + 12, y + 2, 2, PIXEL_ON);             // nub
  int fillWidth = (percent * 10 + 50) / 100;
  if (fillWidth < 0) fillWidth = 0;
  if (fillWidth > 10) fillWidth = 10;
  if (fillWidth) gfx.fillRect(x + 1, y + 1, fillWidth, 4, PIXEL_ON);
  if (charging) gfx.drawBitmap(x + 3, y - 1, ICON_BOLT, 5, 7, SSD1306_INVERSE);
}

// Status bar (rows 0-8). Landscape: title left, battery right (percent only
// when there's no title). Portrait: battery + percent.
#define BAR_H 9
static void uiStatusBar(Adafruit_GFX &gfx, int batteryPercent, bool onUsb, const char *title) {
  int width = gfx.width();
  gfx.setTextSize(1); gfx.setTextColor(PIXEL_ON);
  char percentText[6];
  if (width < 64) {
    uiDrawBattery(gfx, 1, 1, batteryPercent, onUsb);
    snprintf(percentText, sizeof(percentText), "%d", batteryPercent);
    gfx.setCursor(width - 6 * strlen(percentText), 0); gfx.print(percentText);
  } else {
    uiDrawBattery(gfx, width - 14, 1, batteryPercent, onUsb);
    if (!*title) {
      snprintf(percentText, sizeof(percentText), "%d%%", batteryPercent);
      gfx.setCursor(width - 14 - 6 * strlen(percentText) - 2, 0); gfx.print(percentText);
    } else {
      char shownTitle[24]; strncpy(shownTitle, title, 23); shownTitle[23] = 0;
      int maxChars = (width - 16) / 6;
      if ((int)strlen(shownTitle) > maxChars) shownTitle[maxChars] = 0;
      gfx.setCursor(0, 0); gfx.print(shownTitle);
    }
  }
}

// Portrait labels are 5 chars wide; longer text scrolls through that window.
static float uiMarqueeMs = 0;
static void uiMarquee(const char *text, char *window5) {
  int length = strlen(text);
  if (length <= 5) { strcpy(window5, text); return; }
  int cycle = length + 3, offset = ((int)(uiMarqueeMs / 300)) % cycle;
  for (int k = 0; k < 5; k++) { int i = (offset + k) % cycle; window5[k] = i < length ? text[i] : ' '; }
  window5[5] = 0;
}

static inline int menuCenterY(bool portrait, bool hasStatusBar) {
  return portrait ? (hasStatusBar ? 60 : 64) : (hasStatusBar ? BAR_H + 8 : 11);
}

// The one menu renderer. Landscape: icons in a row, the centred one labelled
// on the bottom row. Portrait: a vertical column, short label (and value)
// under the icon. selectedLabel/selectedValue override the selected item's
// text for live values. Returns the clicked index, else -1.
template <typename ItemAt>
static int uiMenuCore(Adafruit_GFX &gfx, MenuState &menu, int itemCount, ItemAt itemAt, int wheelDelta, bool click,
                      float dtMs, const char *selectedLabel, const char *selectedValue, bool hasStatusBar = true) {
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  if (portrait) uiMarqueeMs += dtMs;
  ringStep(menu.selected, menu.position, menu.targetPosition, itemCount, wheelDelta, 0.22f, 0.002f, dtMs);
  if (click) menu.clickBounce = 1;
  menu.clickBounce = approach(menu.clickBounce, 0, 0.18f, dtMs);
  const int itemPitch = 36;
  int ringCenterX = width / 2, ringCenterY = menuCenterY(portrait, hasStatusBar);
  int contentTop = hasStatusBar ? BAR_H : 0;
  gfx.setTextSize(1); gfx.setTextColor(PIXEL_ON);

  // Two items are a fixed pair (item 1 left of item 0), not a ring.
  float pairView = menu.position <= 1 ? -menu.position : menu.position - 2;
  for (int k = -2; k <= 2; k++) {
    int index; float offset;
    if (itemCount == 2) {
      if (k < 0 || k > 1) continue;
      index = k;
      offset = (k == 0 ? 0 : -1) - pairView;
    } else {
      index = (((int)floorf(menu.position) + k) % itemCount + itemCount) % itemCount;
      if (itemCount < 5 && (k < -(itemCount - 1) / 2 || k > itemCount / 2)) continue;   // don't draw an item twice
      offset = (floorf(menu.position) + k) - menu.position;
    }
    int itemCenterX = portrait ? ringCenterX : ringCenterX + (int)lroundf(offset * itemPitch);
    int itemCenterY = portrait ? ringCenterY + (int)lroundf(offset * itemPitch) : ringCenterY;
    int iconX = itemCenterX - 8, iconY = itemCenterY - 8;
    if (portrait) { if (iconY < contentTop || iconY + 16 > height) continue; }
    else          { if (iconX + 16 <= 0 || iconX >= width) continue; }
    MenuItem item = itemAt(index);
    drawIcon(gfx, item.icon, iconX, iconY);
    if (fabsf(offset) < 0.5f) {
      bool isSelected = (index == menu.selected);
      if (portrait) {
        char window[6]; uiMarquee(item.shortLabel, window);
        int textWidth = 6 * strlen(window);
        gfx.setCursor(itemCenterX - textWidth / 2, iconY + 18); gfx.print(window);
        if (isSelected && selectedValue && *selectedValue) {
          char valueWindow[6]; uiMarquee(selectedValue, valueWindow);
          textWidth = 6 * strlen(valueWindow);
          gfx.setCursor(itemCenterX - textWidth / 2, iconY + 26); gfx.print(valueWindow);
        }
      } else {
        const char *source = (isSelected && selectedLabel && *selectedLabel) ? selectedLabel : item.label;
        char label[24]; strncpy(label, source, 23); label[23] = 0;
        if ((int)strlen(label) > width / 6) label[width / 6] = 0;
        int textWidth = 6 * strlen(label), labelX = itemCenterX - textWidth / 2;
        if (labelX < 0) labelX = 0;
        if (labelX + textWidth > width) labelX = width - textWidth;
        gfx.setCursor(labelX, height - 7); gfx.print(label);
      }
    }
  }
  if (menu.clickBounce > 0.02f) {
    int radius = (int)lroundf((1 - menu.clickBounce) * 14) + 2;
    gfx.drawCircle(ringCenterX, ringCenterY, radius, SSD1306_INVERSE);
  }
  return click ? menu.selected : -1;
}

static int uiMenuFrame(Adafruit_GFX &gfx, MenuState &menu, const MenuItem *items, int itemCount, int wheelDelta, bool click,
                       float dtMs, const char *selectedLabel = nullptr, const char *selectedValue = nullptr) {
  return uiMenuCore(gfx, menu, itemCount, [&](int i) { return items[i]; }, wheelDelta, click, dtMs, selectedLabel, selectedValue);
}

#define VAULT_MENU_BACK -2
// Vault list as a carousel (last item = BACK). Returns the clicked vault
// index, VAULT_MENU_BACK, or -1. Portrait first shows a ring of the initial
// letters present; picking one jumps to that letter.
static int uiVaultMenu(Adafruit_GFX &gfx, MenuState &menu, int kind, int wheelDelta, bool click, float dtMs, char *counterText) {
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  static int filtered[VAULT_MAX];
  int filteredCount = vaultFilter(kind, filtered);
  if (counterText) counterText[0] = 0;
  const bool hasStatusBar = false;

  if (portrait && vaultUi.showLetterRing && filteredCount > 0) {
    static char letters[28]; static int firstEntryOfLetter[28];
    int letterCount = 0; char previousLetter = 0;
    for (int k = 0; k < filteredCount; k++) {
      char letter = vault.entries[filtered[k]].name[0];
      letter = (letter >= 'a' && letter <= 'z') ? letter - 32 : letter;
      if (!(letter >= 'A' && letter <= 'Z')) letter = '#';
      if (letter != previousLetter) {
        letters[letterCount] = letter; firstEntryOfLetter[letterCount] = k;
        letterCount++; previousLetter = letter;
        if (letterCount >= 27) break;
      }
    }
    static char letterName[2] = { 0, 0 };
    auto letterItem = [&](int i) {
      if (i >= letterCount) return MenuItem{ "BACK", "BACK", IC_BACK };
      letterName[0] = letters[i];
      uint8_t icon = letters[i] == '#' ? (uint8_t)IC_NUMBER : USER_ICON(1 + USER_PICTO_N + (letters[i] - 'A'));
      return MenuItem{ letterName, letterName, icon };
    };
    MenuState &letterMenu = vaultUi.letterMenu;
    if (letterMenu.selected > letterCount) { letterMenu.selected = letterCount; letterMenu.position = letterCount; }
    int clicked = uiMenuCore(gfx, letterMenu, letterCount + 1, letterItem, wheelDelta, click, dtMs, nullptr, nullptr, hasStatusBar);
    if (counterText) strcpy(counterText, "A-Z");
    if (clicked < 0) return -1;
    if (clicked >= letterCount) return VAULT_MENU_BACK;
    vaultUi.showLetterRing = false;
    menu.selected = firstEntryOfLetter[clicked]; menu.position = menu.selected;
    return -1;
  }

  int itemCount = filteredCount + 1;                                 // + BACK
  if (menu.selected >= itemCount) { menu.selected = itemCount - 1; menu.position = menu.selected; }
  auto entryItem = [&](int i) {
    if (i >= filteredCount) {
      if (filteredCount) return MenuItem{ "BACK", "BACK", IC_BACK };
      const char *emptyText = kind == KIND_CARD ? "NO CARDS" : kind == KIND_TOTP ? "NO 2FA CODES" : "NO ENTRIES";
      return MenuItem{ emptyText, "EMPTY", IC_BACK };
    }
    const VaultEntry &entry = vault.entries[filtered[i]];
    if (entry.kind == KIND_TOTP) return MenuItem{ entry.name, entry.name, entry.icon ? USER_ICON(entry.icon) : (uint8_t)IC_OTP };
    return MenuItem{ entry.name, entry.name, entry.kind ? (uint8_t)IC_CARD : USER_ICON(entry.icon) };
  };
  char usernameValue[VAULT_USER_LEN + 1] = "";
  if (portrait && menu.selected < filteredCount && vault.entries[filtered[menu.selected]].kind == KIND_PW)
    strncpy(usernameValue, vault.entries[filtered[menu.selected]].username, VAULT_USER_LEN);
  int clicked = uiMenuCore(gfx, menu, itemCount, entryItem, wheelDelta, click, dtMs, nullptr, usernameValue, hasStatusBar);
  if (counterText) {
    if (filteredCount == 0) strcpy(counterText, "EMPTY");
    else if (menu.selected < filteredCount) snprintf(counterText, 12, "%d/%d", menu.selected + 1, filteredCount);
  }
  if (clicked < 0) return -1;
  if (clicked >= filteredCount) {
    if (portrait && filteredCount > 0) { vaultUi.showLetterRing = true; return -1; }   // portrait BACK -> letters
    return VAULT_MENU_BACK;
  }
  return filtered[clicked];
}

// Typing progress bar under the status bar.
static void uiTypingBar(Adafruit_GFX &gfx, const VaultUi &state) {
  if (!state.isTyping) return;
  int width = gfx.width();
  int fillWidth = state.charsTotal ? width * state.charsTyped / state.charsTotal : 0;
  gfx.fillRect(0, 7, fillWidth, 2, PIXEL_ON);
}

// ---- PIN screen: four slots, the active one is a rolling drum -------------
static void uiPinFrame(Adafruit_GFX &gfx, Ui &ui, int wheelDelta, bool click, float dtMs, uint32_t now) {
  int width = gfx.width(), height = gfx.height();
  if (wheelDelta) {
    ui.pinDigits[ui.pinSlot] = ((ui.pinDigits[ui.pinSlot] + wheelDelta) % 10 + 10) % 10;
    ui.pinRoll = 1;
  }
  ui.pinRoll = approach(ui.pinRoll, 0, 0.3f, dtMs);
  ui.pinShake = approach(ui.pinShake, 0, 0.12f, dtMs);
  if (click && ui.pinShake < 0.05f) {
    if (ui.pinSlot < 3) {
      ui.pinSlot++;
    } else {
      for (int i = 0; i < 4; i++) ui.pinTyped[i] = '0' + ui.pinDigits[i];
      ui.pinTyped[4] = 0;
      ui.pinSubmit = true;                                         // the sketch checks it
      ui.pinSlot = 0;
      for (int i = 0; i < 4; i++) ui.pinDigits[i] = 0;
    }
  }
  int shakeX = (int)lroundf(sinf(ui.pinShake * 25) * ui.pinShake * 6);   // damped side-to-side
  gfx.setTextSize(2); gfx.setTextColor(PIXEL_ON);
  bool portrait = height > width;
  const int slotWidth = 18;
  int firstSlotX = width / 2 - 2 * slotWidth + 2 + shakeX, slotY = 13;
  for (int i = 0; i < 4; i++) {
    int slotX = portrait ? width / 2 - 5 + shakeX : firstSlotX + i * slotWidth;
    if (portrait) slotY = 14 + i * 24;
    if (i < ui.pinSlot) { gfx.fillCircle(slotX + 5, slotY + 7, 3, PIXEL_ON); continue; }      // entered: dot
    if (i > ui.pinSlot) { gfx.drawFastHLine(slotX, slotY + 14, 11, PIXEL_ON); continue; }     // pending: underline
    int rollOffset = (int)lroundf(ui.pinRoll * 10);                                          // active drum
    gfx.setCursor(slotX, slotY - rollOffset); gfx.print(ui.pinDigits[i]);
    gfx.fillRect(slotX - 1, slotY - 3, 14, 3, SSD1306_BLACK);                                // clip the drum
    gfx.drawFastHLine(slotX, slotY + 16, 11, PIXEL_ON); gfx.drawFastHLine(slotX, slotY + 17, 11, PIXEL_ON);
  }
  if (ui.pinShake > 0.05f) { gfx.setTextSize(1); gfx.setCursor(portrait ? 1 : width - 36, height - 8); gfx.print("WRONG"); }
}

// ---- text builder: the wheel scrolls a ring of chars + DEL / OK / EXIT ----
#define BUILDER_NONE 0
#define BUILDER_OK   1
#define BUILDER_EXIT 2
static int uiBuilderFrame(Adafruit_GFX &gfx, Ui &ui, const char *chars, char *text, int capacity, int minLength,
                          const char *title, int wheelDelta, bool click, float dtMs, uint32_t now) {
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  int charCount = strlen(chars), ringSize = charCount + 3, textLength = strlen(text);
  const int DEL = charCount, OK = charCount + 1, EXIT = charCount + 2;
  ringStep(ui.builderSelected, ui.builderPosition, ui.builderTarget, ringSize, wheelDelta, 0.35f, 0.01f, dtMs);
  gfx.setTextWrap(false); gfx.setTextColor(PIXEL_ON); gfx.setTextSize(1);

  // Top line: title, then the typed text right-aligned (tail when long) + cursor.
  int maxChars = width / 6, titleLength = strlen(title);
  int room = maxChars - (portrait ? 0 : titleLength + 1);
  int keep = textLength + 1;
  if (keep > room) keep = room;
  if (keep < 1) keep = 1;
  int from = textLength - (keep - 1);
  if (from < 0) from = 0;
  char shown[24];
  snprintf(shown, sizeof(shown), "%s%s", text + from, ((now / 400) & 1) ? "_" : " ");
  if (!portrait || textLength == 0) { gfx.setCursor(0, 0); gfx.print(title); }
  if (textLength > 0 || !portrait) { int shownWidth = 6 * strlen(shown); gfx.setCursor(width - shownWidth, 0); gfx.print(shown); }
  gfx.drawFastHLine(0, 7, width, PIXEL_ON);

  auto glyphText = [&](int index) -> const char* {
    static char single[8];
    if (index == DEL) return "DEL";
    if (index == OK) return "OK";
    if (index == EXIT) return "EXIT";
    single[0] = chars[index]; single[1] = 0;
    return single;
  };
  int ringCenterX = width / 2, ringCenterY = portrait ? 64 : 21, pitch = portrait ? 24 : 32;
  for (int k = -2; k <= 2; k++) {
    int index = (((int)floorf(ui.builderPosition) + k) % ringSize + ringSize) % ringSize;
    float offset = (floorf(ui.builderPosition) + k) - ui.builderPosition;
    int centerX = portrait ? ringCenterX : ringCenterX + (int)lroundf(offset * pitch);
    int centerY = portrait ? ringCenterY + (int)lroundf(offset * pitch) : ringCenterY;
    bool isSelected = fabsf(offset) < 0.5f;
    const char *glyph = glyphText(index);
    int textSize = isSelected ? 2 : 1;                         // selected item grows
    gfx.setTextSize(textSize);
    int glyphWidth = 6 * textSize * strlen(glyph), glyphHeight = 8 * textSize;
    int textX = centerX - glyphWidth / 2, textY = centerY - glyphHeight / 2 + 1;
    if (textX < 0 || textX + glyphWidth > width || textY < BAR_H || textY + glyphHeight > height) continue;
    gfx.setCursor(textX, textY); gfx.print(glyph);
  }
  gfx.setTextSize(1);
  if (click) {
    if (ui.builderSelected == DEL)       { if (textLength > 0) text[textLength - 1] = 0; }
    else if (ui.builderSelected == OK)   { if (textLength >= minLength) return BUILDER_OK; }
    else if (ui.builderSelected == EXIT) return BUILDER_EXIT;
    else if (textLength < capacity)      { text[textLength] = chars[ui.builderSelected]; text[textLength + 1] = 0; }
  }
  return BUILDER_NONE;
}

// ---- menus ------------------------------------------------------------------
static const MenuItem HOME_ITEMS[]    = { {"TYPER","TYPER",IC_KEYBOARD}, {"CARDS","CARDS",IC_CARD}, {"2FA","2FA",IC_OTP}, {"EDIT","EDIT",IC_EDIT}, {"SETUP","SETUP",IC_GEAR}, {"OFF","OFF",IC_POWER} };
enum { HOME_TYPER, HOME_CARDS, HOME_2FA, HOME_EDIT, HOME_SETUP, HOME_OFF };

static const MenuItem PWMENU_ITEMS[]  = { {"TYPE PW","PW",IC_KEY}, {"TYPE USER","USER",IC_USER}, {"USER+PW","BOTH",IC_BOTH}, {"SETTINGS","SETUP",IC_SLIDERS}, {"BACK","BACK",IC_BACK} };
enum { PWMENU_TYPE_PW, PWMENU_TYPE_USER, PWMENU_TYPE_BOTH, PWMENU_SETTINGS, PWMENU_BACK };   // first three == TYPE_* modes

static const MenuItem PWSET_ITEMS[]   = { {"ICON","ICON",IC_SMILEY}, {"TABS","TABS",IC_TAB}, {"ENTER","ENTER",IC_ENTER}, {"FAVORITE","FAV",IC_STAR}, {"CHANGE PW","CHGPW",IC_TYPENEW}, {"DELETE","DEL",IC_TRASH}, {"BACK","BACK",IC_BACK} };
enum { PWSET_ICON, PWSET_TABS, PWSET_ENTER, PWSET_FAVORITE, PWSET_CHANGE_PW, PWSET_DELETE, PWSET_BACK };

static const MenuItem CARD_ITEMS[]    = { {"NUMBER","NUM",IC_NUMBER}, {"EXP","EXP",IC_CALENDAR}, {"CVV","CVV",IC_CVV}, {"DELETE","DEL",IC_TRASH}, {"BACK","BACK",IC_BACK} };
enum { CARD_NUMBER, CARD_EXPIRY, CARD_CVV, CARD_DELETE, CARD_BACK };

static const MenuItem SET_ITEMS[]     = { {"CHANGE PWD","PWD",IC_KEY}, {"NEW ENTRY","NEW",IC_PLUS}, {"CHANGE PIN","PIN",IC_PIN}, {"BACKUP","BKUP",IC_BACKUP}, {"RESTORE","RSTR",IC_RESTORE},
                                          {"WHEEL DIR","WHEEL",IC_WHEEL}, {"AUTO-OFF","SLEEP",IC_CLOCK}, {"AUTO-WIPE","WIPE",IC_WIPE}, {"INFO","INFO",IC_INFO}, {"FACTORY RESET","RESET",IC_RESET},
                                          {"POWER OFF","OFF",IC_POWER}, {"BACK","BACK",IC_BACK} };
enum { SET_CHANGE_PWD, SET_NEW_ENTRY, SET_CHANGE_PIN, SET_BACKUP, SET_RESTORE, SET_WHEEL_DIR, SET_AUTO_OFF, SET_AUTO_WIPE,
       SET_INFO, SET_FACTORY_RESET, SET_POWER_OFF, SET_BACK };

static const MenuItem CHG_ITEMS[]     = { {"TYPE OLD","OLD",IC_TYPEOLD}, {"TYPE NEW","NEW",IC_TYPENEW}, {"TYPE AGAIN","AGAIN",IC_AGAIN}, {"SAVE","SAVE",IC_SAVE}, {"REGEN","REGEN",IC_REGEN}, {"BACK","BACK",IC_BACK} };
#define CHG_BACK 5                     // items 0-4 map to CHANGE_TYPE_OLD..CHANGE_REGENERATE

static const MenuItem NEWTYPE_ITEMS[] = { {"PASSWORD","PW",IC_KEY}, {"CARD","CARD",IC_CARD}, {"BACK","BACK",IC_BACK} };
enum { NEWTYPE_PASSWORD, NEWTYPE_CARD, NEWTYPE_BACK };

static const MenuItem CODESET_ITEMS[] = { {"ICON","ICON",IC_SMILEY}, {"ENTER","ENTER",IC_ENTER}, {"FAVORITE","FAV",IC_STAR}, {"DELETE","DEL",IC_TRASH}, {"BACK","BACK",IC_BACK} };
enum { CODESET_ICON, CODESET_ENTER, CODESET_FAVORITE, CODESET_DELETE, CODESET_BACK };

static const MenuItem CLOCKASK_ITEMS[] = { {"YES","YES",IC_YES}, {"NO","NO",IC_NO} };
static const MenuItem CLOCKSYNC_ITEMS[] = { {"LINUX","LINUX",IC_CLOCK}, {"MAC","MAC",IC_CLOCK}, {"WINDOWS","WIN",IC_CLOCK}, {"BACK","BACK",IC_BACK} };   // first three == OS_*
#define CLOCKSYNC_BACK 3
static const MenuItem CONFIRM_ITEMS[] = { {"NO","NO",IC_NO}, {"YES","YES",IC_YES} };

#define ITEM_COUNT(items) (int)(sizeof(items) / sizeof(items[0]))
static const char *LABEL_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_ ";
static const char *DIGIT_CHARS = "0123456789";
static const uint8_t AUTO_OFF_CHOICES[] = { 0, 10, 20, 30, 45, 60 };   // seconds, 0 = never

static bool entryIndexValid(int index) { return index >= 0 && index < vault.count; }
static bool totpEntryValid(int index) { return entryIndexValid(index) && vault.entries[index].kind == KIND_TOTP; }

static void uiEntryTitle(char *title, int capacity, const char *prefix, int index) {
  const char *name = entryIndexValid(index) ? vault.entries[index].name : "?";
  snprintf(title, capacity, "%s%s", prefix, name);
}

static const char *pinScreenTitle(const Ui &ui) {
  if (ui.busy) return "CHECKING";
  switch (ui.pinMode) {
    case PIN_UNLOCK:  return "ENTER PIN";
    case PIN_CURRENT: return "OLD PIN";
    case PIN_BACKUP:  return "PIN=BACKUP";
    case PIN_RESTORE: return "BACKUP PIN";
    case PIN_NEW:     return "NEW PIN";
    default:          return "AGAIN";
  }
}

static void setOnOffValue(char *label, int labelSize, char *shortValue, const char *name, bool on) {
  snprintf(label, labelSize, "%s %s", name, on ? "ON" : "OFF");
  strcpy(shortValue, on ? "ON" : "OFF");
}

static void resetBuilder(Ui &ui) { ui.builderSelected = 0; ui.builderPosition = 0; ui.builderTarget = 0; }

// Draw and handle one frame. Returns true (still animating).
static bool uiFrame(Adafruit_SSD1306 &gfx, Ui &ui, int wheelDelta, bool click, bool back,
                    int batteryPercent, bool onUsb, float batteryVolts, uint32_t now) {
  float dtMs = ui.lastFrameMs ? (now - ui.lastFrameMs) : 16.7f;
  if (dtMs > 100) dtMs = 100;
  ui.lastFrameMs = now;
  if (dtMs > 0) ui.fps += ((1000.0f / dtMs) - ui.fps) * 0.1f;

  if (ui.locked && ui.screen != UI_PIN) { ui.screen = UI_PIN; ui.pinSlot = 0; ui.navDepth = 0; }   // only PIN while locked
  if (back && ui.screen == UI_PIN && ui.pinSlot > 0) { ui.pinSlot--; ui.pinRoll = 1; back = false; }   // hold on PIN: previous digit
  if (back && !ui.locked && ui.screen != UI_HOME) {
    if (ui.screen == UI_PIN) ui.pinMode = PIN_UNLOCK;
    if (ui.screen == UI_VAULT) ui.pickingForPasswordChange = false;
    if (ui.screen == UI_NEWLABEL) { ui.newLabelLength = 0; ui.newLabel[0] = 0; }
    uiBackOne(ui);
  }
  if (ui.screen == UI_MSG && now > ui.toastUntil) {
    ui.screen = ui.toastReturnScreen;
    if (ui.screen == UI_HOME) ui.navDepth = 0;
  }

  UiScreen screen = ui.screen;               // fixed for this frame
  int width = gfx.width(), height = gfx.height();
  bool portrait = height > width;
  gfx.clearDisplay(); gfx.setTextWrap(false); gfx.setTextSize(1); gfx.setTextColor(PIXEL_ON);
  char title[24] = "", counterText[12] = "";
  char valueLabel[20] = "", valueShort[8] = "";   // live value for the selected menu item

  switch (screen) {
    case UI_HOME:      title[0] = 0; break;
    case UI_PIN:       strcpy(title, pinScreenTitle(ui)); break;
    case UI_INFO:      strcpy(title, "INFO"); break;
    case UI_MSG:       // entry toasts keep the entry's name as title
      if (ui.toastReturnScreen == UI_PWMENU) uiEntryTitle(title, sizeof(title), "", ui.entryIndex);
      else strncpy(title, ui.toastText, 23);
      break;
    case UI_VAULT:     strcpy(title, ui.pickingForPasswordChange ? "PICK" : "TYPER"); break;
    case UI_CARDS:     strcpy(title, "CARDS"); break;
    case UI_CODES:     strcpy(title, "2FA"); break;
    case UI_CODE:      uiEntryTitle(title, sizeof(title), "", ui.entryIndex); break;
    case UI_PWMENU:    uiEntryTitle(title, sizeof(title), "", ui.entryIndex); break;
    case UI_PWSET:     uiEntryTitle(title, sizeof(title), "SET ", ui.entryIndex); break;
    case UI_ICONPICK:  strcpy(title, "ICON"); break;
    case UI_CODESET:   uiEntryTitle(title, sizeof(title), "SET ", ui.entryIndex); break;
    case UI_BACKUPS:   strcpy(title, "BACKUP CODES"); break;
    case UI_CLOCKASK:  strcpy(title, "SYNC CLOCK?"); break;
    case UI_CLOCKSYNC: strcpy(title, "SYNC CLOCK FROM"); break;
    case UI_LOGIN:     strcpy(title, ui.loginHost); break;
    case UI_CARDMENU:  uiEntryTitle(title, sizeof(title), "", ui.entryIndex); break;
    case UI_SETTINGS:  strcpy(title, "SETTINGS"); break;
    case UI_EDITMODE:  strcpy(title, "EDIT"); break;
    case UI_CHGPW:     uiEntryTitle(title, sizeof(title), "CHG ", ui.changeEntryIndex); break;
    case UI_NEWTYPE:   snprintf(title, sizeof(title), "NEW %s", ui.newLabel); break;
    case UI_CONFIRM:   strcpy(title, ui.confirmKind == CONFIRM_DELETE_ENTRY ? "DELETE?" : "ERASE ALL?"); break;
    default: break;
  }

  // ---- full-panel screens (no status bar)
  if (screen == UI_NEWLABEL) {
    int result = uiBuilderFrame(gfx, ui, LABEL_CHARS, ui.newLabel, sizeof(ui.newLabel) - 1, 1, "NAME", wheelDelta, click, dtMs, now);
    ui.newLabelLength = strlen(ui.newLabel);
    if (result == BUILDER_OK) uiGo(ui, UI_NEWTYPE);
    else if (result == BUILDER_EXIT) { ui.newLabelLength = 0; ui.newLabel[0] = 0; uiBackOne(ui); }
    gfx.display(); return true;
  }
  if (screen == UI_NEWNUM) {
    static const char *STAGE_TITLES[3] = { "NUMBER", "EXP MMYY", "CVV" };
    char *stageText[3] = { ui.newCardNumber, ui.newCardExpiry, ui.newCardCvv };
    const int stageCapacity[3] = { 19, 4, 4 }, stageMinLength[3] = { 8, 4, 3 };
    int stage = ui.newCardStage < 0 ? 0 : ui.newCardStage > 2 ? 2 : ui.newCardStage;
    int result = uiBuilderFrame(gfx, ui, DIGIT_CHARS, stageText[stage], stageCapacity[stage], stageMinLength[stage],
                                STAGE_TITLES[stage], wheelDelta, click, dtMs, now);
    if (result == BUILDER_OK) {
      if (stage < 2) { ui.newCardStage = stage + 1; resetBuilder(ui); }
      else ui.pendingAction = ACT_NEWCARD;
    } else if (result == BUILDER_EXIT) {
      uiBackOne(ui);
    }
    gfx.display(); return true;
  }
  if (screen == UI_VAULT || screen == UI_CARDS || screen == UI_CODES) {
    int kind = (screen == UI_CARDS) ? KIND_CARD : (screen == UI_CODES) ? KIND_TOTP : KIND_PW;
    bool typing = vaultUi.isTyping;                               // ignore input mid-type
    int picked = uiVaultMenu(gfx, ui.menus[screen], kind, typing ? 0 : wheelDelta, typing ? false : click, dtMs, counterText);
    if (vaultUi.isTyping) gfx.fillRect(0, 0, vaultUi.charsTotal ? width * vaultUi.charsTyped / vaultUi.charsTotal : 0, 2, PIXEL_ON);
    if (picked == VAULT_MENU_BACK) { ui.pickingForPasswordChange = false; uiBackOne(ui); }
    if (picked >= 0) {
      if (ui.pickingForPasswordChange) {
        ui.pickingForPasswordChange = false;
        ui.changeEntryIndex = picked; ui.changeHasCandidate = false; ui.changeAction = CHANGE_GENERATE_SILENT;
        uiBackOne(ui); uiGo(ui, UI_CHGPW);
      } else if (kind == KIND_CARD) {
        ui.entryIndex = picked; ui.cardIndex = picked; uiGo(ui, UI_CARDMENU);
      } else if (kind == KIND_TOTP) {
        ui.entryIndex = picked; uiGo(ui, UI_CODE);
      } else {
        ui.entryIndex = picked; uiGo(ui, UI_PWMENU);
      }
    }
    gfx.display(); return true;
  }

  // ---- everything else: status bar + body
  uiStatusBar(gfx, batteryPercent, onUsb, title);

  switch (screen) {
    case UI_HOME: {
      int clicked = uiMenuFrame(gfx, ui.menus[UI_HOME], HOME_ITEMS, ITEM_COUNT(HOME_ITEMS), wheelDelta, click, dtMs);
      if (clicked == HOME_TYPER) { vaultUi.showLetterRing = true; uiGo(ui, UI_VAULT); }
      if (clicked == HOME_CARDS) { vaultUi.showLetterRing = true; uiGo(ui, UI_CARDS); }
      if (clicked == HOME_2FA) {
        vaultUi.showLetterRing = true; uiGo(ui, UI_CODES);
        if (!totpClockValid() && onUsb) uiGo(ui, UI_CLOCKASK);    // no time yet: offer to fetch it
      }
      if (clicked == HOME_EDIT)  uiGo(ui, UI_EDITMODE);
      if (clicked == HOME_SETUP) uiGo(ui, UI_SETTINGS);
      if (clicked == HOME_OFF)   ui.pendingAction = ACT_OFF;
      if (ui.introStartedAt && now - ui.introStartedAt < 600) {    // intro: wipe in from the left
        float progress = easeOut((now - ui.introStartedAt) / 500.0f);
        int edgeX = (int)(progress * width);
        gfx.fillRect(edgeX, 0, width - edgeX, height, SSD1306_BLACK);
        gfx.drawFastVLine(edgeX, 0, height, PIXEL_ON);
      }
    } break;

    case UI_PIN: uiPinFrame(gfx, ui, wheelDelta, click, dtMs, now); break;

    case UI_INFO: {
      char batteryLine[32]; snprintf(batteryLine, sizeof(batteryLine), "Battery %.2fV  %d%%", batteryVolts, batteryPercent);
      if (portrait) {
        char voltsText[8]; snprintf(voltsText, sizeof(voltsText), "%.2fV", batteryVolts);
        char percentText[8]; snprintf(percentText, sizeof(percentText), "%d%%", batteryPercent);
        printClipped(gfx, 1, 12, ui.infoLine1, 0, width); printClipped(gfx, 1, 22, ui.infoLine2, 0, width);
        printClipped(gfx, 1, 34, voltsText, 0, width);   printClipped(gfx, 1, 44, percentText, 0, width);
      } else {
        printClipped(gfx, 0, 9, ui.infoLine1, 0, width); printClipped(gfx, 0, 17, ui.infoLine2, 0, width);
        printClipped(gfx, 0, 25, batteryLine, 0, width);
      }
      if (click) uiBackOne(ui);
    } break;

    case UI_MSG: {
      const char *text = ui.toastText;
      int textSize = (!portrait && 12 * (int)strlen(text) <= width) ? 2 : 1;   // big when it fits
      gfx.setTextSize(textSize);
      int textWidth = 6 * textSize * strlen(text), textX = width / 2 - textWidth / 2;
      if (textX < 0) textX = 0;
      if (textSize == 2) {
        gfx.setCursor(textX, 13); gfx.print(text);
      } else if (!portrait) {
        printClipped(gfx, textX, 16, text, 0, width);
      } else {                                                      // portrait: wrap on spaces into 5-char rows
        int rowY = 20, i = 0, length = strlen(text);
        char row[6];
        while (i < length && rowY < height - 8) {
          int rowLength = 0;
          while (i < length && rowLength < 5) {
            if (text[i] == ' ' && rowLength) { i++; break; }
            if (text[i] != ' ') row[rowLength++] = text[i];
            i++;
          }
          row[rowLength] = 0;
          if (rowLength) { gfx.setCursor(1, rowY); gfx.print(row); rowY += 10; }
        }
      }
      gfx.setTextSize(1);
    } break;

    case UI_PWMENU: {
      if (!entryIndexValid(ui.entryIndex)) { uiBackOne(ui); break; }
      int clicked = uiMenuFrame(gfx, ui.menus[UI_PWMENU], PWMENU_ITEMS, ITEM_COUNT(PWMENU_ITEMS), wheelDelta, click, dtMs);
      uiTypingBar(gfx, vaultUi);
      bool needsUsername = clicked == PWMENU_TYPE_USER || clicked == PWMENU_TYPE_BOTH;
      if (needsUsername && !vault.entries[ui.entryIndex].username[0]) uiToast(ui, "USER NOT SET", now + 1200, UI_PWMENU);
      else if (clicked >= PWMENU_TYPE_PW && clicked <= PWMENU_TYPE_BOTH) { ui.typeEntryRequest = ui.entryIndex; ui.typeMode = clicked; }
      if (clicked == PWMENU_SETTINGS) uiGo(ui, UI_PWSET);
      if (clicked == PWMENU_BACK) uiBackOne(ui);
    } break;

    case UI_PWSET: {
      if (!entryIndexValid(ui.entryIndex)) { uiBackOne(ui); break; }
      VaultEntry &entry = vault.entries[ui.entryIndex];
      MenuState &menu = ui.menus[UI_PWSET];
      char iconName[16]; userIconName(entry.icon, iconName, sizeof(iconName));
      if (menu.selected == PWSET_ICON) { snprintf(valueLabel, sizeof(valueLabel), "ICON %s", iconName); strncpy(valueShort, iconName, 5); valueShort[5] = 0; }
      if (menu.selected == PWSET_TABS) { snprintf(valueLabel, sizeof(valueLabel), "TABS %d", entry.tabCount); snprintf(valueShort, sizeof(valueShort), "%d", entry.tabCount); }
      if (menu.selected == PWSET_ENTER) setOnOffValue(valueLabel, sizeof(valueLabel), valueShort, "ENTER", entry.pressEnter);
      if (menu.selected == PWSET_FAVORITE) setOnOffValue(valueLabel, sizeof(valueLabel), valueShort, "FAVORITE", entry.favorite);
      int clicked = uiMenuFrame(gfx, menu, PWSET_ITEMS, ITEM_COUNT(PWSET_ITEMS), wheelDelta, click, dtMs, valueLabel, valueShort);
      if (clicked == PWSET_ICON) { uiGo(ui, UI_ICONPICK); ui.menus[UI_ICONPICK].selected = entry.icon; ui.menus[UI_ICONPICK].position = entry.icon; }
      if (clicked == PWSET_TABS) { entry.tabCount = (entry.tabCount + 1) % 6; ui.pendingAction = ACT_ENTRYSET; }
      if (clicked == PWSET_ENTER) { entry.pressEnter = !entry.pressEnter; ui.pendingAction = ACT_ENTRYSET; }
      if (clicked == PWSET_FAVORITE) { entry.favorite = !entry.favorite; ui.pendingAction = ACT_ENTRYSET; }
      if (clicked == PWSET_CHANGE_PW) {
        ui.changeEntryIndex = ui.entryIndex; ui.changeHasCandidate = false; ui.changeAction = CHANGE_GENERATE_SILENT;
        uiGo(ui, UI_CHGPW);
      }
      if (clicked == PWSET_DELETE) { ui.confirmKind = CONFIRM_DELETE_ENTRY; uiGo(ui, UI_CONFIRM); }
      if (clicked == PWSET_BACK) uiBackOne(ui);
    } break;

    case UI_ICONPICK: {                                             // every user icon in one ring
      if (!entryIndexValid(ui.entryIndex)) { uiBackOne(ui); break; }
      static char longName[16], shortName[16];
      bool isTotp = vault.entries[ui.entryIndex].kind == KIND_TOTP;  // a 2FA entry's default icon is the shield
      auto iconItem = [&](int i) {
        if (isTotp && i == 0) return MenuItem{ "2FA", "2FA", IC_OTP };
        userIconName(i, longName, sizeof(longName));
        if (i > 0 && i <= USER_PICTO_N) strncpy(shortName, USER_PICTO_SHORT[i - 1], 15);
        else strcpy(shortName, longName);
        shortName[15] = 0;
        return MenuItem{ longName, shortName, USER_ICON(i) };
      };
      int clicked = uiMenuCore(gfx, ui.menus[UI_ICONPICK], USER_ICON_COUNT, iconItem, wheelDelta, click, dtMs, nullptr, nullptr);
      if (clicked >= 0) { vault.entries[ui.entryIndex].icon = clicked; ui.pendingAction = ACT_ENTRYSET; uiBackOne(ui); }
    } break;

    case UI_CARDMENU: {
      if (!entryIndexValid(ui.entryIndex)) { uiBackOne(ui); break; }
      VaultEntry &entry = vault.entries[ui.entryIndex];
      MenuState &menu = ui.menus[UI_CARDMENU];
      if (menu.selected == CARD_EXPIRY && entry.expiry[0]) { snprintf(valueLabel, sizeof(valueLabel), "EXP %s", entry.expiry); strncpy(valueShort, entry.expiry, 7); }
      int clicked = uiMenuFrame(gfx, menu, CARD_ITEMS, ITEM_COUNT(CARD_ITEMS), wheelDelta, click, dtMs, valueLabel, valueShort);
      uiTypingBar(gfx, vaultUi);
      if (clicked >= CARD_NUMBER && clicked <= CARD_CVV) { ui.cardIndex = ui.entryIndex; ui.pendingAction = ACT_CARDNUM + clicked; }
      if (clicked == CARD_DELETE) { ui.confirmKind = CONFIRM_DELETE_ENTRY; uiGo(ui, UI_CONFIRM); }
      if (clicked == CARD_BACK) uiBackOne(ui);
    } break;

    case UI_SETTINGS: {
      MenuState &menu = ui.menus[UI_SETTINGS];
      if (menu.selected == SET_AUTO_OFF) {
        if (ui.autoOffSeconds) {
          snprintf(valueLabel, sizeof(valueLabel), "AUTO-OFF %ds", ui.autoOffSeconds);
          snprintf(valueShort, sizeof(valueShort), "%ds", ui.autoOffSeconds);
        } else {
          strcpy(valueLabel, "AUTO-OFF NEVER"); strcpy(valueShort, "NEVER");
        }
      }
      if (menu.selected == SET_AUTO_WIPE) setOnOffValue(valueLabel, sizeof(valueLabel), valueShort, "AUTO-WIPE", ui.autoWipe);
      int clicked = uiMenuFrame(gfx, menu, SET_ITEMS, ITEM_COUNT(SET_ITEMS), wheelDelta, click, dtMs, valueLabel, valueShort);
      switch (clicked) {
        case SET_CHANGE_PWD:    ui.pickingForPasswordChange = true; vaultUi.showLetterRing = true; uiGo(ui, UI_VAULT); break;
        case SET_NEW_ENTRY:     ui.newLabelLength = 0; ui.newLabel[0] = 0; resetBuilder(ui); uiGo(ui, UI_NEWLABEL); break;
        case SET_CHANGE_PIN:    ui.pinMode = PIN_CURRENT; ui.pinSlot = 0; uiGo(ui, UI_PIN); break;
        case SET_BACKUP:        ui.pinMode = PIN_BACKUP; ui.pinSlot = 0; uiGo(ui, UI_PIN); break;
        case SET_RESTORE:       ui.pendingAction = ACT_RESTORE; uiGo(ui, UI_EDITMODE); break;
        case SET_WHEEL_DIR:     ui.pendingAction = ACT_WHEELDIR; break;
        case SET_AUTO_OFF: {
          int current = 0;
          for (int i = 0; i < 6; i++) if (AUTO_OFF_CHOICES[i] == ui.autoOffSeconds) current = i;
          ui.autoOffSeconds = AUTO_OFF_CHOICES[(current + 1) % 6];
          ui.pendingAction = ACT_PREFS;
        } break;
        case SET_AUTO_WIPE:     ui.pendingAction = ACT_AUTOWIPE; break;
        case SET_INFO:          ui.pendingAction = ACT_INFO; uiGo(ui, UI_INFO); break;
        case SET_FACTORY_RESET: ui.confirmKind = CONFIRM_FACTORY_RESET; uiGo(ui, UI_CONFIRM); break;
        case SET_POWER_OFF:     ui.pendingAction = ACT_OFF; break;
        case SET_BACK:          uiBackOne(ui); break;
      }
    } break;

    case UI_EDITMODE: {
      static const char *LANDSCAPE_LINE1[] = { "flash drive mode", "writing...", "SAVED", "ejected - hold", "bad file, kept" };
      static const char *LANDSCAPE_LINE2[] = { "edit the TXT files", "", "", "to leave", "old entries" };
      static const char *PORTRAIT_TEXT[]   = { "USB", "wait", "SAVED", "eject", "bad" };
      int state = ui.editState < EDIT_IDLE ? EDIT_IDLE : ui.editState > EDIT_BAD ? EDIT_BAD : ui.editState;
      gfx.setCursor(2, 11); gfx.print(portrait ? PORTRAIT_TEXT[state] : LANDSCAPE_LINE1[state]);
      if (!portrait) {
        gfx.setCursor(2, 22); gfx.print(LANDSCAPE_LINE2[state]);
        int driveX = width - 12, driveY = 13;                       // pulsing "drive" glyph
        gfx.drawRoundRect(driveX, driveY, 10, 14, 2, PIXEL_ON);
        if ((now / 400) & 1) gfx.fillRect(driveX + 3, driveY + 3, 4, 3, PIXEL_ON);
      }
    } break;

    case UI_CHGPW: {
      if (!entryIndexValid(ui.changeEntryIndex)) { uiBackOne(ui); break; }
      int clicked = uiMenuFrame(gfx, ui.menus[UI_CHGPW], CHG_ITEMS, ITEM_COUNT(CHG_ITEMS), wheelDelta, click, dtMs);
      uiTypingBar(gfx, vaultUi);
      if (clicked == CHG_BACK) { ui.changeEntryIndex = -1; uiBackOne(ui); }
      else if (clicked >= 0) ui.changeAction = clicked;
    } break;

    case UI_NEWTYPE: {
      int clicked = uiMenuFrame(gfx, ui.menus[UI_NEWTYPE], NEWTYPE_ITEMS, ITEM_COUNT(NEWTYPE_ITEMS), wheelDelta, click, dtMs);
      if (clicked == NEWTYPE_PASSWORD) ui.pendingAction = ACT_NEWPW;
      if (clicked == NEWTYPE_CARD) {
        ui.newCardNumber[0] = 0; ui.newCardExpiry[0] = 0; ui.newCardCvv[0] = 0; ui.newCardStage = 0;
        resetBuilder(ui); uiGo(ui, UI_NEWNUM);
      }
      if (clicked == NEWTYPE_BACK) uiBackOne(ui);
    } break;

    case UI_CODE: {
      // Pages the wheel slides between: the live code (click types it),
      // BACKUP CODES (only if any), SETTINGS, BACK.
      if (!totpEntryValid(ui.entryIndex)) { uiBackOne(ui); break; }
      VaultEntry &entry = vault.entries[ui.entryIndex];
      MenuState &menu = ui.menus[UI_CODE];
      bool typing = vaultUi.isTyping;
      enum { PAGE_CODE, PAGE_BACKUPS, PAGE_SETTINGS, PAGE_BACK };
      static const MenuItem PAGES[] = { {"", "", 0}, {"BACKUP CODES","BKUP",IC_KEY}, {"SETTINGS","SETUP",IC_SLIDERS}, {"BACK","BACK",IC_BACK} };
      int pageOf[4], pageCount = 0;
      pageOf[pageCount++] = PAGE_CODE;
      if (entry.backupCodeCount) pageOf[pageCount++] = PAGE_BACKUPS;
      pageOf[pageCount++] = PAGE_SETTINGS;
      pageOf[pageCount++] = PAGE_BACK;
      if (wheelDelta && !typing) menu.selected += wheelDelta > 0 ? 1 : -1;
      if (menu.selected < 0) menu.selected = 0;
      if (menu.selected > pageCount - 1) menu.selected = pageCount - 1;
      menu.position = approach(menu.position, menu.selected, 0.25f, dtMs);
      if (fabsf(menu.position - menu.selected) < 0.01f) menu.position = menu.selected;
      int pageSpan = portrait ? height : width;
      int codePageOffset = (int)lroundf(-menu.position * pageSpan);   // offset along the slide axis
      auto cursorAt = [&](int offset, int x, int y) {
        if (portrait) gfx.setCursor(x, y + offset); else gfx.setCursor(x + offset, y);
      };

      if (codePageOffset > -pageSpan) {                               // page 0: the code
        if (!totpClockValid()) {
          if (portrait) {
            static const char *LINES[5] = { "NO", "CLOCK", "CLICK", "TO", "SYNC" };
            static const int LINE_Y[5] = { 20, 30, 46, 56, 66 };
            for (int i = 0; i < 5; i++) { cursorAt(codePageOffset, 1, LINE_Y[i]); gfx.print(LINES[i]); }
          } else {
            cursorAt(codePageOffset, 2, 12); gfx.print("NO CLOCK");
            cursorAt(codePageOffset, 2, 22); gfx.print("CLICK TO SYNC");
          }
        } else {
          int digits = totpDigits(entry.expiry), period = totpPeriod(entry.cvv), secondsLeft = totpSecondsLeft(period);
          char code[10]; totpCode(entry.secret, digits, period, totpClockNow(), code);
          int half = digits / 2;
          gfx.setTextSize(2);
          if (portrait) {                                             // two digits per row
            for (int i = 0; i < digits; i += 2) {
              char pair[3] = { code[i], code[i + 1], 0 };
              cursorAt(codePageOffset, 4, 14 + (i / 2) * 18); gfx.print(pair);
            }
          } else {                                                    // "123 456", centred
            char shown[12]; snprintf(shown, sizeof(shown), "%.*s %s", half, code, code + half);
            int textWidth = 12 * strlen(shown), textX = (width - textWidth) / 2 - 6;
            if (textX > width - textWidth - 20) textX = width - textWidth - 20;   // clear of seconds + page dots
            if (textX < 0) textX = 0;
            cursorAt(codePageOffset, textX, 12); gfx.print(shown);
          }
          gfx.setTextSize(1);
          char secondsText[4]; snprintf(secondsText, sizeof(secondsText), "%d", secondsLeft);
          if (portrait) { cursorAt(codePageOffset, 1, height - 16); gfx.print(secondsText); gfx.print("s"); }
          else { cursorAt(codePageOffset, width - 6 * strlen(secondsText) - 4, 16); gfx.print(secondsText); }
          int barWidth = width * secondsLeft / period;
          if (portrait) gfx.fillRect(0, height - 2 + codePageOffset, barWidth, 2, PIXEL_ON);
          else gfx.fillRect(codePageOffset, height - 2, barWidth, 2, PIXEL_ON);
          memset(code, 0, sizeof(code));
        }
      }
      for (int page = 1; page < pageCount; page++) {                  // other pages, drawn like menu items
        int offset = codePageOffset + page * pageSpan;
        if (offset <= -pageSpan || offset >= pageSpan) continue;
        const MenuItem &item = PAGES[pageOf[page]];
        int centerX = portrait ? width / 2 : width / 2 + offset, centerY = portrait ? 60 + offset : BAR_H + 8;
        drawIcon(gfx, item.icon, centerX - 8, centerY - 8);
        const char *label = portrait ? item.shortLabel : item.label;
        int textWidth = 6 * strlen(label);
        if (portrait) gfx.setCursor(centerX - textWidth / 2, centerY + 10);
        else gfx.setCursor(centerX - textWidth / 2, height - 7);
        gfx.print(label);
      }
      for (int i = 0; i < pageCount; i++) {                           // page dots
        int dotX = width - 2, dotY = portrait ? height - 8 - pageCount * 5 + 5 + i * 5 : 12 + i * 5;
        if (i == menu.selected) gfx.fillRect(dotX, dotY, 2, 2, PIXEL_ON); else gfx.drawPixel(dotX, dotY, PIXEL_ON);
      }
      uiTypingBar(gfx, vaultUi);
      if (click && !typing) {
        int page = pageOf[menu.selected];
        if (page == PAGE_CODE) {
          if (totpClockValid()) {
            ui.pendingAction = ACT_TOTPTYPE;
          } else {
            uiGo(ui, UI_CLOCKSYNC);
            if (ui.clockSyncOs >= 0) { ui.menus[UI_CLOCKSYNC].selected = ui.clockSyncOs; ui.menus[UI_CLOCKSYNC].position = ui.clockSyncOs; }
          }
        }
        else if (page == PAGE_BACKUPS) uiGo(ui, UI_BACKUPS);
        else if (page == PAGE_SETTINGS) uiGo(ui, UI_CODESET);
        else uiBackOne(ui);
      }
    } break;

    case UI_CODESET: {
      if (!totpEntryValid(ui.entryIndex)) { uiBackOne(ui); break; }
      VaultEntry &entry = vault.entries[ui.entryIndex];
      MenuState &menu = ui.menus[UI_CODESET];
      char iconName[16];
      if (entry.icon) userIconName(entry.icon, iconName, sizeof(iconName)); else strcpy(iconName, "2FA");
      if (menu.selected == CODESET_ICON) { snprintf(valueLabel, sizeof(valueLabel), "ICON %s", iconName); strncpy(valueShort, iconName, 5); valueShort[5] = 0; }
      if (menu.selected == CODESET_ENTER) setOnOffValue(valueLabel, sizeof(valueLabel), valueShort, "ENTER", entry.pressEnter);
      if (menu.selected == CODESET_FAVORITE) setOnOffValue(valueLabel, sizeof(valueLabel), valueShort, "FAVORITE", entry.favorite);
      int clicked = uiMenuFrame(gfx, menu, CODESET_ITEMS, ITEM_COUNT(CODESET_ITEMS), wheelDelta, click, dtMs, valueLabel, valueShort);
      if (clicked == CODESET_ICON) { uiGo(ui, UI_ICONPICK); ui.menus[UI_ICONPICK].selected = entry.icon; ui.menus[UI_ICONPICK].position = entry.icon; }
      if (clicked == CODESET_ENTER) { entry.pressEnter = !entry.pressEnter; ui.pendingAction = ACT_ENTRYSET; }
      if (clicked == CODESET_FAVORITE) { entry.favorite = !entry.favorite; ui.pendingAction = ACT_ENTRYSET; }
      if (clicked == CODESET_DELETE) { ui.confirmKind = CONFIRM_DELETE_ENTRY; uiGo(ui, UI_CONFIRM); }   // opens on NO
      if (clicked == CODESET_BACK) uiBackOne(ui);
    } break;

    case UI_BACKUPS: {                                              // click types that backup code
      if (!totpEntryValid(ui.entryIndex)) { uiBackOne(ui); break; }
      int codeCount = vault.entries[ui.entryIndex].backupCodeCount;
      bool typing = vaultUi.isTyping;
      static char longLabel[12], shortLabel[6];
      auto codeItem = [&](int i) {
        if (i >= codeCount) return MenuItem{ "BACK", "BACK", IC_BACK };
        snprintf(longLabel, sizeof(longLabel), "BACKUP %d", i + 1);
        snprintf(shortLabel, sizeof(shortLabel), "BK %d", i + 1);
        return MenuItem{ longLabel, shortLabel, IC_KEY };
      };
      int clicked = uiMenuCore(gfx, ui.menus[UI_BACKUPS], codeCount + 1, codeItem, typing ? 0 : wheelDelta, typing ? false : click, dtMs, nullptr, nullptr);
      uiTypingBar(gfx, vaultUi);
      if (clicked >= 0 && clicked < codeCount) { ui.backupCodeIndex = clicked; ui.pendingAction = ACT_TOTPBACKUP; }
      if (clicked == codeCount) uiBackOne(ui);
    } break;

    case UI_CLOCKASK: {                                             // entering 2FA without the time
      if (totpClockValid()) { uiBackOne(ui); break; }
      int clicked = uiMenuFrame(gfx, ui.menus[UI_CLOCKASK], CLOCKASK_ITEMS, ITEM_COUNT(CLOCKASK_ITEMS), wheelDelta, click, dtMs);
      if (clicked == 0) {                                           // YES: sync for the last-used OS, else ask which
        ui.screen = UI_CLOCKSYNC; ui.menus[UI_CLOCKSYNC] = MenuState();
        if (ui.clockSyncOs >= 0) {
          ui.menus[UI_CLOCKSYNC].selected = ui.clockSyncOs; ui.menus[UI_CLOCKSYNC].position = ui.clockSyncOs;
          ui.pendingAction = ACT_CLOCKSYNC;
        }
      }
      if (clicked == 1) uiBackOne(ui);
    } break;

    case UI_CLOCKSYNC: {                                            // pick the OS; the sketch types the command
      if (totpClockValid()) { uiBackOne(ui); uiToast(ui, "CLOCK SET", now + 1200, ui.screen); break; }
      bool typing = vaultUi.isTyping;
      MenuState &menu = ui.menus[UI_CLOCKSYNC];
      if (menu.selected == OS_LINUX) strcpy(valueLabel, "LINUX: IN TERMINAL");   // needs a terminal focused
      if (menu.selected == OS_MAC) strcpy(valueLabel, "MAC: IN TERMINAL");
      int clicked = uiMenuFrame(gfx, menu, CLOCKSYNC_ITEMS, ITEM_COUNT(CLOCKSYNC_ITEMS), typing ? 0 : wheelDelta, typing ? false : click,
                                dtMs, valueLabel[0] ? valueLabel : nullptr, nullptr);
      uiTypingBar(gfx, vaultUi);
      if (clicked >= OS_LINUX && clicked <= OS_WINDOWS) { ui.clockSyncOs = clicked; ui.pendingAction = ACT_CLOCKSYNC; }
      if (clicked == CLOCKSYNC_BACK) uiBackOne(ui);
    } break;

    case UI_LOGIN: {                                                // accounts for the browser's page
      for (int k = 0; k < ui.loginMatchCount; k++)
        if (!entryIndexValid(ui.loginMatches[k])) { ui.loginMatchCount = 0; break; }   // vault changed under us
      if (!ui.loginMatchCount) { uiBackOne(ui); break; }
      bool typing = vaultUi.isTyping;
      MenuState &menu = ui.menus[UI_LOGIN];
      auto accountItem = [&](int i) {
        if (i >= ui.loginMatchCount) return MenuItem{ "BACK", "BACK", IC_BACK };
        const VaultEntry &entry = vault.entries[ui.loginMatches[i]];
        return MenuItem{ entry.username[0] ? entry.username : entry.name, entry.name, USER_ICON(entry.icon) };   // title already shows the site
      };
      char usernameValue[VAULT_USER_LEN + 1] = "";
      if (menu.selected < ui.loginMatchCount) strncpy(usernameValue, vault.entries[ui.loginMatches[menu.selected]].username, VAULT_USER_LEN);
      int clicked = uiMenuCore(gfx, menu, ui.loginMatchCount + 1, accountItem, typing ? 0 : wheelDelta, typing ? false : click,
                               dtMs, nullptr, portrait ? usernameValue : nullptr);
      uiTypingBar(gfx, vaultUi);
      if (clicked >= 0 && clicked < ui.loginMatchCount) {
        int index = ui.loginMatches[clicked];
        int mode = ui.loginMode;
        if (mode != TYPE_PASSWORD && !vault.entries[index].username[0])   // no username: password only, or nothing
          mode = mode == TYPE_USER_AND_PASSWORD ? TYPE_PASSWORD : -1;
        if (mode < 0) {
          uiToast(ui, "USER NOT SET", now + 1200, UI_LOGIN);
        } else {
          ui.typeEntryRequest = index; ui.typeMode = mode; ui.typeForceEnter = true;
          ui.loginLastPicked = index; ui.loginMatchCount = 0;
          uiBackOne(ui); uiToast(ui, "LOGGING IN", now + 900, ui.screen);
        }
      }
      if (clicked == ui.loginMatchCount) { ui.loginMatchCount = 0; uiBackOne(ui); }
    } break;

    case UI_CONFIRM: {
      int clicked = uiMenuFrame(gfx, ui.menus[UI_CONFIRM], CONFIRM_ITEMS, ITEM_COUNT(CONFIRM_ITEMS), wheelDelta, click, dtMs);
      if (clicked == 0) uiBackOne(ui);
      if (clicked == 1) ui.pendingAction = ui.confirmKind == CONFIRM_DELETE_ENTRY ? ACT_DELETE : ACT_RESET;
    } break;

    default: break;
  }
  gfx.display();
  return true;
}

#define LOGIN_LOCKED -1
#define LOGIN_BUSY   -2
// Browser extension "site <host> [pw|user|both]": open LOG IN with the
// matching accounts (nothing typed until clicked). Returns the match count,
// LOGIN_LOCKED, or LOGIN_BUSY (typing, EDIT, PIN or another prompt open).
static int uiOfferLogin(Ui &ui, const char *host, int mode) {
  if (ui.locked) return LOGIN_LOCKED;
  UiScreen screen = ui.screen;
  if (vaultUi.isTyping || screen == UI_EDITMODE || screen == UI_PIN || screen == UI_NEWLABEL || screen == UI_NEWNUM
      || screen == UI_CONFIRM || screen == UI_CHGPW || screen == UI_CLOCKSYNC || screen == UI_CLOCKASK) return LOGIN_BUSY;
  int matches[LOGIN_MAX_MATCHES];
  int matchCount = vaultMatchSite(host, matches, LOGIN_MAX_MATCHES);
  if (!matchCount) {
    if (ui.screen == UI_LOGIN) { ui.loginMatchCount = 0; uiBackOne(ui); }
    return 0;
  }
  int keepSelected = (ui.screen == UI_LOGIN && ui.menus[UI_LOGIN].selected < ui.loginMatchCount)
                   ? ui.loginMatches[ui.menus[UI_LOGIN].selected] : ui.loginLastPicked;
  if (ui.screen == UI_MSG) {                                        // drop a toast so it can't return over LOG IN
    ui.screen = ui.toastReturnScreen;
    if (ui.screen == UI_HOME) ui.navDepth = 0;
  }
  if (ui.screen != UI_LOGIN) uiGo(ui, UI_LOGIN);
  memcpy(ui.loginMatches, matches, matchCount * sizeof(int));
  ui.loginMatchCount = matchCount; ui.loginMode = mode;
  String normalizedHost = siteNormalize(host);
  strncpy(ui.loginHost, normalizedHost.c_str(), sizeof(ui.loginHost) - 1);
  ui.loginHost[sizeof(ui.loginHost) - 1] = 0;
  MenuState &menu = ui.menus[UI_LOGIN];
  menu.selected = 0;
  for (int k = 0; k < matchCount; k++) if (matches[k] == keepSelected) menu.selected = k;   // stay on the same account (password step)
  menu.position = menu.selected; menu.targetPosition = menu.selected;
  return matchCount;
}

static void uiCancelLogin(Ui &ui) {
  if (ui.screen == UI_LOGIN) { ui.loginMatchCount = 0; uiBackOne(ui); }
}
