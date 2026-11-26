// lomenett.ino -- RP2354A firmware for the Lomenett V3.6 board.
// Serial console at 115200 ("help" lists commands).
//
// Wiring:
//   GP8/GP9   I2C SDA/SCL: OLED, LIS3DH accelerometer, SE050
//   GP15      OLED reset
//   GP12      wheel push switch (to GND, internal pull-up)
//   GP13/14   wheel A/B (external pull-ups + RC)
//   GP11  <-  CH32 PA1: select button level, plus ack pulses
//   GP18  ->  CH32 PC1: "OLED ready" at boot, double pulse = "cut power"
//   GP16 power mux status (high = USB), GP25 LED, GP26 battery sense (/2)
// The select button is wired to the CH32, which debounces it and mirrors it here.

#include <Wire.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "lomenett_logic.h"
#include "ui.h"
#include "splash.h"
#include "msc.h"
#include "decrypt.h"
#include "backup.h"
#include "flip.h"
#include "remote.h"
#include <EEPROM.h>

// ------------------------------------------------------------------- pins

#define PIN_SDA         8
#define PIN_SCL         9
#define PIN_OLED_RESET 15
#define PIN_WHEEL_SWITCH 12
#define PIN_WHEEL_A    13
#define PIN_WHEEL_B    14
#define PIN_MIRROR     11      // from CH32: select button / acks
#define PIN_READY      18      // to CH32
#define PIN_USB_POWER  16      // power mux status
#define PIN_STATUS_LED 25
#define PIN_VBAT_SENSE 26

#define OLED_I2C_ADDRESS  0x3C
#define SE050_I2C_ADDRESS 0x48
static uint8_t accelI2cAddress = 0x18;   // 0x18 or 0x19 (SA0), probed at boot

#define SCREEN_W 128
#define SCREEN_H 32

#define SHUTDOWN_PRESSES   5
#define SHUTDOWN_WINDOW_MS 2000

// SSD1306 commands
#define OLED_CMD_DISPLAY_OFF   0xAE
#define OLED_CMD_DISPLAY_ON    0xAF
#define OLED_CMD_CHARGE_PUMP   0x8D
#define OLED_CMD_CONTRAST      0x81
#define OLED_CMD_FOLLOW_RAM    0xA4
#define OLED_CMD_ALL_PIXELS_ON 0xA5

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

bool     oledReady   = false;
bool     oledVirtual = false;           // no panel: drawing to the framebuffer for the bridge only
bool     accelReady  = false;
bool     i2cBusStuck = false;           // SDA/SCL held low at boot: I2C disabled
volatile long wheelDetents = 0;          // updated in the wheel timer interrupt
int8_t   wheelQuarterSteps = 0;
uint16_t selectPressCount = 0;
uint32_t shutdownPressTimes[SHUTDOWN_PRESSES];
uint8_t  shutdownPressCount = 0;
bool     accelStreaming = false;
bool     statusScreenEnabled = true;    // status screen when the carousel UI is off
bool     carouselUiEnabled = true;      // "ui on|off"
Ui       ui;
Splash   splash;

#include "lomenett_bridge.h"            // "bridge on": live OLED view on the PC (sim/bridge/)
bool     ledForcedOn = false;

// Screen rotation follows gravity: the accelerometer sector is mapped onto
// the panel's rotation for how the chip sits on the board.
#define ORIENT_SECTOR_OFFSET 3   // calibrated 2026-09-12
#define ORIENT_MIRRORED      1   // chip Y runs the other way
#define ORIENT_DWELL_POLLS   4   // 200 ms at 20 Hz
orient_t orientation;
bool     autoRotate = true;
int      displayRotation = 0;    // current Adafruit GFX rotation
int      accelX = 0, accelY = 0, accelZ = 0;   // last sample, milli-g

// ------------------------------------------------------------------- OLED

// Init sequence for this bare flex panel (datasheet 4.4.2, internal DC/DC).
void oledInit() {
  // begin() allocates the framebuffer (without it display() streams garbage).
  static bool framebufferAllocated = false;
  if (!framebufferAllocated) framebufferAllocated = oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);

  pinMode(PIN_OLED_RESET, OUTPUT);
  digitalWrite(PIN_OLED_RESET, LOW);
  delayMicroseconds(20);
  digitalWrite(PIN_OLED_RESET, HIGH);
  delay(10);

  oled.ssd1306_command(OLED_CMD_DISPLAY_OFF);
  oled.ssd1306_command(0xD5); oled.ssd1306_command(0x80);   // clock divide
  oled.ssd1306_command(0xA8); oled.ssd1306_command(0x1F);   // multiplex: 32 rows
  oled.ssd1306_command(0xD3); oled.ssd1306_command(0x00);   // display offset
  oled.ssd1306_command(0x40);                               // start line 0
  oled.ssd1306_command(0xA1);                               // segment remap
  oled.ssd1306_command(0xC8);                               // COM scan reversed
  oled.ssd1306_command(0xDA); oled.ssd1306_command(0x00);   // COM pins
  oled.ssd1306_command(OLED_CMD_CONTRAST); oled.ssd1306_command(0x8F);
  oled.ssd1306_command(0xD9); oled.ssd1306_command(0x1F);   // precharge
  oled.ssd1306_command(0xDB); oled.ssd1306_command(0x40);   // VCOMH
  oled.ssd1306_command(OLED_CMD_FOLLOW_RAM);
  oled.ssd1306_command(0x20); oled.ssd1306_command(0x00);   // horizontal addressing (reset left page mode)
  oled.clearDisplay();
  oled.display();
}

void oledPowerOn() {
  oled.ssd1306_command(OLED_CMD_CHARGE_PUMP); oled.ssd1306_command(0x14);
  oled.ssd1306_command(OLED_CMD_DISPLAY_ON);
}

void oledLines(const char *line1, const char *line2 = "", const char *line3 = "") {
  if (!oledReady) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.println(line1);
  if (*line2) oled.println(line2);
  if (*line3) oled.println(line3);
  oled.display();
}

// ------------------------------------------------------- accelerometer (LIS3DH)

#define LIS3DH_WHO_AM_I   0x0F
#define LIS3DH_CTRL_REG1  0x20
#define LIS3DH_CTRL_REG4  0x23
#define LIS3DH_OUT_X_L    0x28
#define LIS3DH_AUTO_INCREMENT 0x80

void accelWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(accelI2cAddress);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t accelReadRegister(uint8_t reg) {
  Wire.beginTransmission(accelI2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom(accelI2cAddress, 1);
  return Wire.available() ? Wire.read() : 0;
}

bool accelBegin() {
  for (uint8_t address = 0x18; address <= 0x19; address++) {
    accelI2cAddress = address;
    if (accelReadRegister(LIS3DH_WHO_AM_I) != 0x33) continue;
    accelWriteRegister(LIS3DH_CTRL_REG1, 0x57);   // 100 Hz, XYZ on
    accelWriteRegister(LIS3DH_CTRL_REG4, 0x00);   // +/-2 g
    return true;
  }
  return false;
}

// Milli-g per axis (10-bit left-justified, 4 mg/count at +/-2 g).
void accelRead(int &x, int &y, int &z) {
  Wire.beginTransmission(accelI2cAddress);
  Wire.write(LIS3DH_OUT_X_L | LIS3DH_AUTO_INCREMENT);
  if (Wire.endTransmission(false) != 0) { x = y = z = 0; return; }
  Wire.requestFrom(accelI2cAddress, 6);
  if (Wire.available() < 6) { x = y = z = 0; return; }
  int16_t raw[3];
  for (int i = 0; i < 3; i++) {
    uint8_t low = Wire.read(), high = Wire.read();
    raw[i] = (int16_t)((high << 8) | low);
  }
  x = (raw[0] >> 6) * 4;
  y = (raw[1] >> 6) * 4;
  z = (raw[2] >> 6) * 4;
}

// ------------------------------------------------------- settings in EEPROM
// Each record starts with a magic number; older versions are still read.

#define EEPROM_GAUGE_ADDR    0
#define EEPROM_ROTATION_ADDR 32
#define EEPROM_AMPS_ADDR     48
#define EEPROM_WHEEL_ADDR    64
#define EEPROM_PREFS_ADDR    112

struct WheelSettings { uint32_t magic; uint8_t quarterStepsPerNotch, flipMask; };
#define WHEEL_MAGIC 0x57484C32u                 // "WHL2"
uint8_t wheelQuarterStepsPerNotch = 2;          // "notch N"
uint8_t wheelFlipMask = 0x06;                   // bit r = invert wheel in rotation r ("wheelflip M")

struct Prefs {
  uint32_t magic;
  uint8_t  autoEnter, autoWipe, haveBackup;
  uint16_t decryptMs;
  uint8_t  autoOffSeconds;
  uint8_t  clockSyncOsPlusOne;                  // 0 = not picked
};
#define PREFS_MAGIC    0x50524633u              // "PRF3"
#define PREFS_MAGIC_V2 0x50524632u              // "PRF2": no clock-sync OS
#define PREFS_MAGIC_V1 0x50524631u              // "PRF1": no auto-off
uint16_t decryptMs = 250;                       // learned duration of loginPin

void prefsLoad() {
  Prefs saved; EEPROM.get(EEPROM_PREFS_ADDR, saved);
  bool anyVersion = saved.magic == PREFS_MAGIC || saved.magic == PREFS_MAGIC_V2 || saved.magic == PREFS_MAGIC_V1;
  if (anyVersion) {
    ui.autoEnter = saved.autoEnter; ui.autoWipe = saved.autoWipe; ui.haveBackup = saved.haveBackup;
    if (saved.decryptMs >= 80 && saved.decryptMs <= 3000) decryptMs = saved.decryptMs;
  }
  if ((saved.magic == PREFS_MAGIC || saved.magic == PREFS_MAGIC_V2) && saved.autoOffSeconds <= 60) ui.autoOffSeconds = saved.autoOffSeconds;
  if (saved.magic == PREFS_MAGIC && saved.clockSyncOsPlusOne >= 1 && saved.clockSyncOsPlusOne <= 3) ui.clockSyncOs = saved.clockSyncOsPlusOne - 1;
}
void prefsSave() {
  Prefs saved = { PREFS_MAGIC, (uint8_t)ui.autoEnter, (uint8_t)ui.autoWipe, (uint8_t)ui.haveBackup, decryptMs,
                  ui.autoOffSeconds, (uint8_t)(ui.clockSyncOs + 1) };
  EEPROM.put(EEPROM_PREFS_ADDR, saved); EEPROM.commit();
}

void wheelSettingsLoad() {
  WheelSettings saved; EEPROM.get(EEPROM_WHEEL_ADDR, saved);
  if (saved.magic == WHEEL_MAGIC && saved.quarterStepsPerNotch >= 1 && saved.quarterStepsPerNotch <= 4) {
    wheelQuarterStepsPerNotch = saved.quarterStepsPerNotch;
    wheelFlipMask = saved.flipMask & 15;
  }
}
void wheelSettingsSave() {
  WheelSettings saved = { WHEEL_MAGIC, wheelQuarterStepsPerNotch, wheelFlipMask };
  EEPROM.put(EEPROM_WHEEL_ADDR, saved); EEPROM.commit();
}

// -------------------------------------------------------------- wheel

uint8_t wheelAbPrevious = 0;
volatile long wheelNotches = 0;       // 2 transitions per notch (shown by "status")
uint8_t wheelRestMask = 0;            // shown by "status"

// Runs from the wheel timer interrupt at 5 kHz.
void wheelPoll() {
  uint8_t ab = (digitalRead(PIN_WHEEL_A) << 1) | digitalRead(PIN_WHEEL_B);
  // Second decoder: this wheel gives 2 transitions per notch, not 4.
  {
    static uint8_t abPrevious = 3; static int8_t halfNotch = 0;
    int8_t step = QUADRATURE_STEP_TABLE[((abPrevious & 3) << 2) | (ab & 3)];
    abPrevious = ab;
    if (step) {
      halfNotch += step;
      if (halfNotch >= 2)       { halfNotch -= 2; wheelNotches++; }
      else if (halfNotch <= -2) { halfNotch += 2; wheelNotches--; }
    }
  }
  wheelDetents += wheel_decode_step(ab, &wheelAbPrevious, &wheelQuarterSteps);
}

static repeating_timer wheelTimer;
static bool wheelTimerCallback(repeating_timer *) { wheelPoll(); return true; }

// -------------------------------------------------------------- rotation

int sectorToRotation(int sector) {
  if (ORIENT_MIRRORED) sector = (4 - sector) & 3;
  return (sector + ORIENT_SECTOR_OFFSET) & 3;
}
int rotationToSector(int rotation) {
  int sector = (rotation - ORIENT_SECTOR_OFFSET) & 3;
  if (ORIENT_MIRRORED) sector = (4 - sector) & 3;
  return sector;
}

void applyRotation(int rotation) {
  if (rotation == displayRotation) return;
  if (oledReady && splash.done) {                       // animate from the current frame
    memcpy(rotationOldFrame, oled.getBuffer(), sizeof(rotationOldFrame));
    rotationAnimationPending = true;
    int quarterTurns = (rotation - displayRotation) & 3;
    rotationDeltaDegrees = (quarterTurns == 1) ? 90 : (quarterTurns == 3) ? -90 : 180;
  }
  displayRotation = rotation;
  oled.setRotation(rotation);
  Serial.printf("screen rotation %d\n", rotation);
}

// Rotation is saved at power-off and after holding 5 s (so the CH32's
// 8 s force-off still leaves a fresh copy), and restored at boot.
struct SavedRotation { uint32_t magic; uint8_t rotation; };
#define ROTATION_MAGIC 0x524F5431u   // "ROT1"
int rotationLoad() {
  SavedRotation saved; EEPROM.get(EEPROM_ROTATION_ADDR, saved);
  return (saved.magic == ROTATION_MAGIC && saved.rotation < 4) ? saved.rotation : -1;
}
void rotationSave() {
  SavedRotation saved; EEPROM.get(EEPROM_ROTATION_ADDR, saved);
  if (saved.magic == ROTATION_MAGIC && saved.rotation == displayRotation) return;
  saved = { ROTATION_MAGIC, (uint8_t)displayRotation };
  EEPROM.put(EEPROM_ROTATION_ADDR, saved); EEPROM.commit();
  Serial.printf("rotation %d saved\n", displayRotation);
}
void rotationPoll() {
  static int lastRotation = -1; static uint32_t heldSince = 0;
  if (displayRotation != lastRotation) { lastRotation = displayRotation; heldSince = millis(); return; }
  if (heldSince && millis() - heldSince > 5000) { heldSince = 0; rotationSave(); }
}

// 20 Hz: refresh the sample and, when following gravity, the rotation.
void accelPoll() {
  static uint32_t lastPollMs = 0;
  if (!accelReady || millis() - lastPollMs < 50) return;
  lastPollMs = millis();
  accelRead(accelX, accelY, accelZ);
  int sector = orient_update(&orientation, accelX, accelY, ORIENT_DWELL_POLLS);
  if (autoRotate && splash.done) applyRotation(sectorToRotation(sector));   // never mid-splash
}

bool wheelPressed() { return digitalRead(PIN_WHEEL_SWITCH) == LOW; }
bool selectHeld()   { return digitalRead(PIN_MIRROR) == HIGH; }

// ------------------------------------------------- select button + power

bool mirrorLinePrevious = false;

// True once per press (rising edge of the mirrored button).
bool selectPressedEdge() {
  bool level = digitalRead(PIN_MIRROR);
  bool risingEdge = (level && !mirrorLinePrevious);
  mirrorLinePrevious = level;
  return risingEdge;
}

// 300k/300k divider on GP26: cell voltage = 2 x ADC voltage.
float batteryVolts() {
  return (analogRead(PIN_VBAT_SENSE) * 3.3f / 4095.0f) * 2.0f;
}

#define BATTERY_BOOT_MIN_V 3.35f   // refuse to boot below this on battery
#define BATTERY_RUN_MIN_V  3.05f   // shut down below this on battery

bool onUsb() { return digitalRead(PIN_USB_POWER); }

float batteryVoltsAverage() {
  float sum = 0;
  for (int i = 0; i < 8; i++) { sum += batteryVolts(); delay(2); }
  return sum / 8;
}

// After power-on the sense node charges slowly through the divider: sample
// until two averages agree within 30 mV (max ~1 s).
float batteryVoltsSettled() {
  float previous = batteryVoltsAverage();
  for (int i = 0; i < 50; i++) {
    delay(20);
    float volts = batteryVoltsAverage();
    if (fabsf(volts - previous) < 0.03f) return volts;
    previous = volts;
  }
  return previous;
}

// Battery gauge (lomenett_logic.h), fed at 4 Hz. Everything that shows a
// percent asks it, so the console and screen agree.
gauge_t gauge;

// Learned offsets are saved; each unplug is folded into a running average of
// up to GAUGE_MAX_SAMPLES so one odd unplug can't swing the gauge.
struct SavedGauge { uint32_t magic; float batteryOffset, usbOffset; uint8_t sampleCount; };
#define GAUGE_MAGIC    0x47415533u   // "GAU3"
#define GAUGE_MAGIC_V2 0x47415532u   // "GAU2": no sample count
#define GAUGE_MAX_SAMPLES 6
static uint8_t gaugeSampleCount = 0;

void gaugeLoad() {
  SavedGauge saved; EEPROM.get(EEPROM_GAUGE_ADDR, saved);
  bool knownVersion = saved.magic == GAUGE_MAGIC || saved.magic == GAUGE_MAGIC_V2;
  if (knownVersion && saved.batteryOffset > 0.0f && saved.batteryOffset < 0.5f &&
      saved.usbOffset >= 0.0f && saved.usbOffset < 0.5f) {
    gauge.battery_offset_volts = saved.batteryOffset; gauge.usb_offset_volts = saved.usbOffset;
    gaugeSampleCount = (saved.magic == GAUGE_MAGIC && saved.sampleCount <= GAUGE_MAX_SAMPLES) ? saved.sampleCount : 1;
    Serial.printf("gauge offsets from flash: bat +%.3f V, usb -%.3f V (%d samples)\n", saved.batteryOffset, saved.usbOffset, gaugeSampleCount);
  }
}

// gauge holds this unplug's fresh offsets; fold them into the saved average.
void gaugeSave() {
  static float averageBattery = 0, averageUsb = 0; static bool seeded = false;
  if (!seeded) {
    SavedGauge previous; EEPROM.get(EEPROM_GAUGE_ADDR, previous); seeded = true;
    bool knownVersion = previous.magic == GAUGE_MAGIC || previous.magic == GAUGE_MAGIC_V2;
    if (knownVersion && previous.batteryOffset > 0.0f && previous.batteryOffset < 0.5f) {
      averageBattery = previous.batteryOffset; averageUsb = previous.usbOffset;
    } else {
      gaugeSampleCount = 0;
    }
  }
  if (gaugeSampleCount == 0) {
    averageBattery = gauge.battery_offset_volts; averageUsb = gauge.usb_offset_volts;
  } else {
    averageBattery += (gauge.battery_offset_volts - averageBattery) / (gaugeSampleCount + 1);
    averageUsb += (gauge.usb_offset_volts - averageUsb) / (gaugeSampleCount + 1);
  }
  if (gaugeSampleCount < GAUGE_MAX_SAMPLES) gaugeSampleCount++;
  gauge.battery_offset_volts = averageBattery; gauge.usb_offset_volts = averageUsb;
  SavedGauge saved = { GAUGE_MAGIC, averageBattery, averageUsb, gaugeSampleCount };
  EEPROM.put(EEPROM_GAUGE_ADDR, saved); EEPROM.commit();
  Serial.printf("gauge learned: bat +%.3f V, usb -%.3f V (avg of %d, saved)\n", averageBattery, averageUsb, gaugeSampleCount);
}

void gaugePoll() {
  static uint32_t lastPollMs = 0;
  if (millis() - lastPollMs < 250) return;
  lastPollMs = millis();
  float sum = 0;
  for (int i = 0; i < 4; i++) sum += batteryVolts();
  gauge_update(&gauge, sum / 4.0f, onUsb(), millis());
  if (gauge.offsets_learned) { gauge.offsets_learned = 0; gaugeSave(); }
}

int batteryPercent() {
  if (gauge.shown_percent < 0) gaugePoll();
  return gauge.shown_percent < 0 ? gauge_percent_from_ocv(batteryVolts()) : gauge.shown_percent;
}

float gaugeOrRawVolts() { return gauge.filtered_volts > 0 ? gauge.filtered_volts : batteryVolts(); }

// Sad-battery picture (landscape only); the caller powers off.
void drawSadBattery(float volts) {
  oled.setRotation(displayRotation & 2);
  if (!oledReady) return;
  oled.clearDisplay();
  oled.drawRect(6, 6, 30, 18, SSD1306_WHITE);      // body
  oled.fillRect(36, 11, 3, 8, SSD1306_WHITE);      // nub
  oled.fillRect(8, 8, 4, 14, SSD1306_WHITE);       // last sliver of charge
  oled.fillCircle(18, 12, 1, SSD1306_WHITE);       // eyes
  oled.fillCircle(26, 12, 1, SSD1306_WHITE);
  for (int x = -4; x <= 4; x++)                    // frown
    oled.drawPixel(22 + x, 19 - (4 - abs(x)) / 2, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(48, 6);  oled.println("BATTERY LOW");
  oled.setCursor(48, 18); oled.printf("%.2f V", volts);
  oled.display();
}

// Two rising edges on READY within 150 ms = "shutdown done, cut power".
// (A single edge is the boot handshake.) READY must be low on entry.
void sendShutdownDoublePulse() {
  digitalWrite(PIN_READY, HIGH); delay(30);
  digitalWrite(PIN_READY, LOW);  delay(30);
  digitalWrite(PIN_READY, HIGH);
}

// Turn the panel off, ask the CH32 to cut power, then wait for it.
void powerOff() {
  rotationSave();
  Serial.println("shutting down");
  oledLines("Shutting down...");
  delay(150);

  if (oledReady) {
    oled.ssd1306_command(OLED_CMD_DISPLAY_OFF);
    oled.ssd1306_command(OLED_CMD_CHARGE_PUMP); oled.ssd1306_command(0x10);   // charge pump off
  }
  delay(100);                                  // let the panel discharge

  sendShutdownDoublePulse();
  Serial.println("waiting for the CH32 to cut power");
  // Still running: either RT_EN is bridged (rail can't drop) or the CH32
  // missed it. Resend a few times (harmless if it's already off). A new
  // MIRROR rising edge means the user powered back on: reboot. MIRROR edges
  // are ignored for 500 ms after each send (acks, the held off-press).
  const uint32_t RETRY_INTERVAL_MS = 1000;
  const int      MAX_RETRIES = 5;
  int      retries = 0;
  uint32_t ignoreMirrorSince = millis();
  uint32_t lastPulseMs = ignoreMirrorSince;
  bool mirrorWasHigh = true;                   // a press may still be held from the off gesture
  while (1) {
    digitalWrite(PIN_STATUS_LED, millis() / 250 % 2);
    if (retries < MAX_RETRIES && millis() - lastPulseMs >= RETRY_INTERVAL_MS) {
      retries++;
      Serial.printf("still powered, resending shutdown (%d/%d)\n", retries, MAX_RETRIES);
      digitalWrite(PIN_READY, LOW); delay(30);   // READY is high: drop it so the next rise is an edge
      sendShutdownDoublePulse();
      lastPulseMs = ignoreMirrorSince = millis();
    }
    bool mirrorHigh = digitalRead(PIN_MIRROR);
    if (mirrorHigh && !mirrorWasHigh && millis() - ignoreMirrorSince > 500) rp2040.reboot();
    mirrorWasHigh = mirrorHigh;
    delay(10);
  }
}

// ------------------------------------------------------------ commands

void cmdHelp() {
  Serial.println();
  Serial.println("commands");
  Serial.println("  help          this list");
#if REMOTE_PANEL
  Serial.println("  mirror on|off stream the screen (remote panel build)");
  Serial.println("  rc l|r|c|b|d|u  wheel left/right, click, back, press/release");
#endif
  Serial.println("  status        everything at once");
  Serial.println("  accel         read the accelerometer once");
  Serial.println("  stream        toggle live accelerometer output");
  Serial.println("  wheel         wheel position and switch");
  Serial.println("  zero          reset the wheel to 0");
  Serial.println("  buttons       select button press count");
  Serial.println("  scan          scan the I2C bus");
  Serial.println("  pins          check SDA/SCL at the pin level");
  Serial.println("  oled <text>   write a line to the display");
  Serial.println("  screen        toggle the live status screen");
  Serial.println("  bridge on|off stream the OLED to the PC viewer (sim/bridge); bare: status");
  Serial.println("  led on|off    onboard LED (also lights on either button)");
  Serial.println("  enc           stream encoder A/B/switch changes for 8 s");
  Serial.println("  orient        what the accelerometer thinks is down");
  Serial.println("  rot 0-3       force a screen rotation (turns autorot off)");
  Serial.println("  autorot on|off  screen follows gravity");
  Serial.println("  battery       battery voltage and charger mux state");
  Serial.println("  time [unix]   set the clock for 2FA codes (date +%s); bare: show it");
  Serial.println("  site <host> [both|user|pw]  offer the logins for a website (browser extension); site off: close");
  Serial.println("  totp          list 2FA entries with their current codes");
  Serial.println("  totp add <name> <base32 secret | otpauth:// link>");
  Serial.println("  test          check every peripheral");
  Serial.println("  off           power the board down");
  Serial.println();
  Serial.println("The wheel turns, the wheel clicks, and the select button on");
  Serial.println("the side is handled by the CH32 and mirrored to us.");
  Serial.println("Five quick select presses power the board off.");
}

float currentEstimateMa();

void cmdStatus() {
  int x = accelX, y = accelY, z = accelZ;
  Serial.println();
  Serial.printf("uptime    %lu s\n", millis() / 1000);
  Serial.printf("wheel     %ld  notches %ld  rest states %d%d%d%d  switch %s\n", wheelDetents, wheelNotches,
                (wheelRestMask>>3)&1, (wheelRestMask>>2)&1, (wheelRestMask>>1)&1, wheelRestMask&1, wheelPressed() ? "down" : "up");
  Serial.printf("select    %u presses\n", selectPressCount);
  if (totpClockValid()) Serial.printf("clock     %llu (unix, synced %lu s ago)\n", (unsigned long long)totpClockNow(), (millis() - totpSyncMs) / 1000);
  else                  Serial.println("clock     not set - 2FA codes need \"time <unix>\"");
  if (accelReady) Serial.printf("accel     x %5d  y %5d  z %5d  mg\n", x, y, z);
  else            Serial.println("accel     not responding");
  Serial.printf("oled      %s  ui %s %.0f fps\n", oledVirtual ? "virtual (no panel)" : oledReady ? "ok" : "not responding", carouselUiEnabled ? "on" : "off", ui.fps);
  if (i2cBusStuck) Serial.println("I2C BUS STUCK LOW at boot - bus quarantined (pins cmd to inspect)");
  float volts = batteryVoltsAverage();
  Serial.printf("battery   %.2f V  %d%%   mux %s\n", volts, batteryPercent(), onUsb() ? "USB" : "battery");
  float currentMa = currentEstimateMa();
  if (!isnan(currentMa)) Serial.printf("current   ~%.0f mA (est.)\n", currentMa);
}

// ---------------------------------------------------------------- SE050
// ISO 7816 T=1 over I2C (NXP UM11225). Frame: NAD PCB LEN INF... CRC(lo, hi).
// Host->chip NAD 0x5A, chip->host 0xA5. CRC is X.25.
#define T1_NAD_TO_CHIP   0x5A
#define T1_NAD_FROM_CHIP 0xA5
#define T1_PCB_GET_ATR   0xC7   // S-block requests, response = request | 0x20
#define T1_PCB_SOFT_RESET 0xCF
#define T1_PCB_WTX_REQUEST 0xC3
#define T1_PCB_WTX_RESPONSE 0xE3

static uint16_t t1Crc(const uint8_t *data, int length) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return crc ^ 0xFFFF;
}
static void appendCrc(uint8_t *frame, int length) {
  uint16_t crc = t1Crc(frame, length);
  frame[length] = crc & 0xFF; frame[length + 1] = crc >> 8;
}
static void hexdump(const uint8_t *data, int length) {
  for (int i = 0; i < length; i++) Serial.printf("%02X ", data[i]);
  Serial.println();
}

// "se050" sends GET ATR; "se050 r" sends a soft reset. Prints the raw reply.
void cmdSe050(const char *arg) {
  uint8_t frame[8] = { T1_NAD_TO_CHIP, T1_PCB_GET_ATR, 0x00 };
  if (*arg == 'r') frame[1] = T1_PCB_SOFT_RESET;
  appendCrc(frame, 3);
  Serial.print("tx: "); hexdump(frame, 5);
  Wire.beginTransmission(SE050_I2C_ADDRESS); Wire.write(frame, 5);
  int error = Wire.endTransmission();
  if (error) { Serial.printf("write failed (%d)\n", error); return; }
  uint8_t response[64]; int length = 0;
  for (int tries = 0; tries < 50 && length == 0; tries++) {   // chip NACKs reads until ready
    delay(2);
    length = Wire.requestFrom(SE050_I2C_ADDRESS, (uint8_t)64);
    for (int i = 0; i < length; i++) response[i] = Wire.read();
  }
  if (!length) { Serial.println("no response (read NACKed 50x)"); return; }
  Serial.printf("rx (%d bytes): ", length); hexdump(response, length);
  if (response[0] != T1_NAD_FROM_CHIP) { Serial.println("  NAD is not A5 -- not a T=1 frame"); return; }
  int infoLength = response[2];
  Serial.printf("  NAD %02X  PCB %02X  LEN %d\n", response[0], response[1], infoLength);
  if (infoLength + 5 <= length) {
    uint16_t expected = t1Crc(response, infoLength + 3), received = response[infoLength + 3] | (response[infoLength + 4] << 8);
    Serial.printf("  CRC %s (calc %04X, frame %04X)\n", expected == received ? "ok" : "MISMATCH", expected, received);
    if ((response[1] & 0xC0) == 0x80) Serial.printf("  R-block: %s\n", (response[1] & 0x03) ? "error (CRC/parity)" : "ack");
    if (response[1] == 0xE7) { Serial.print("  ATR: "); hexdump(response + 3, infoLength); }
  }
}

// --- T=1 link layer
static uint8_t t1SendSequence = 0;               // I-block sequence bit

// Send one frame, read one back (up to ~1 s). Returns bytes read, 0 none, -1 write failed.
static int t1Transfer(const uint8_t *frame, int frameLength, uint8_t *response, int responseCapacity) {
  Wire.beginTransmission(SE050_I2C_ADDRESS); Wire.write(frame, frameLength);
  if (Wire.endTransmission()) return -1;
  for (int tries = 0; tries < 200; tries++) {
    delay(5);
    int length = Wire.requestFrom(SE050_I2C_ADDRESS, (uint8_t)responseCapacity);
    if (length) { for (int i = 0; i < length; i++) response[i] = Wire.read(); return length; }
  }
  return 0;
}
static bool t1FrameValid(const uint8_t *frame, int length) {
  if (length < 5 || frame[0] != T1_NAD_FROM_CHIP) return false;
  int infoLength = frame[2];
  if (infoLength + 5 > length) return false;
  return t1Crc(frame, infoLength + 3) == (frame[infoLength + 3] | (frame[infoLength + 4] << 8));
}
static bool t1SendSBlock(uint8_t pcb, uint8_t expectedPcb) {
  uint8_t frame[5] = { T1_NAD_TO_CHIP, pcb, 0x00 };
  appendCrc(frame, 3);
  uint8_t response[64];
  int length = t1Transfer(frame, 5, response, 64);
  bool ok = t1FrameValid(response, length) && response[1] == expectedPcb;
  if (!ok) { Serial.printf("  S-block %02X got: ", pcb); hexdump(response, length > 0 ? (length < 8 ? length : 8) : 0); }
  return ok;
}
// Resync sequence numbers. The chip rejects a soft reset unless it served an ATR first.
static bool t1LinkReset() {
  t1SendSequence = 0;
  return t1SendSBlock(T1_PCB_GET_ATR, 0xE7) && t1SendSBlock(T1_PCB_SOFT_RESET, 0xEF);
}
// Send an APDU in an I-block, answering wait-time-extension requests.
// Returns response length, or negative on error.
static int t1SendApdu(const uint8_t *apdu, int apduLength, uint8_t *output, int outputCapacity) {
  uint8_t frame[64];
  if (apduLength > 58) return -2;
  frame[0] = T1_NAD_TO_CHIP; frame[1] = t1SendSequence ? 0x40 : 0x00; frame[2] = apduLength;
  memcpy(frame + 3, apdu, apduLength);
  appendCrc(frame, apduLength + 3);
  t1SendSequence ^= 1;
  uint8_t response[80];
  int length = t1Transfer(frame, apduLength + 5, response, 80);
  for (int hop = 0; hop < 20; hop++) {
    if (!t1FrameValid(response, length)) { Serial.printf("  bad frame (%d bytes): ", length); hexdump(response, length > 0 ? length : 0); return -3; }
    if (response[1] == T1_PCB_WTX_REQUEST) {                     // chip wants more time: grant it
      uint8_t reply[6] = { T1_NAD_TO_CHIP, T1_PCB_WTX_RESPONSE, 0x01, response[3] };
      appendCrc(reply, 4);
      length = t1Transfer(reply, 6, response, 80);
      continue;
    }
    if ((response[1] & 0x80) == 0) {                             // I-block: the answer
      int infoLength = response[2];
      if (infoLength > outputCapacity) infoLength = outputCapacity;
      memcpy(output, response + 3, infoLength);
      return infoLength;
    }
    Serial.printf("  unexpected PCB %02X: ", response[1]); hexdump(response, length);
    return -4;
  }
  return -5;
}

// "apdu 80 04 00 20 00": send hex bytes, print response + status word.
void cmdApdu(const char *arg) {
  uint8_t apdu[58]; int apduLength = 0;
  for (const char *cursor = arg; *cursor && apduLength < 58; ) {
    while (*cursor == ' ') cursor++;
    if (!*cursor) break;
    apdu[apduLength++] = strtoul(cursor, (char**)&cursor, 16);
  }
  if (!apduLength) { Serial.println("usage: apdu <hex bytes>"); return; }
  if (!t1LinkReset()) { Serial.println("link reset failed"); return; }
  Serial.print("apdu: "); hexdump(apdu, apduLength);
  uint8_t response[80];
  int length = t1SendApdu(apdu, apduLength, response, 80);
  if (length < 0) { Serial.printf("failed (%d)\n", length); return; }
  Serial.printf("resp (%d): ", length); hexdump(response, length);
  if (length >= 2) Serial.printf("  SW %02X%02X %s\n", response[length-2], response[length-1], (response[length-2]==0x90 && response[length-1]==0) ? "OK" : "");
}

char infoLineBuffer1[24] = "SE050: ?", infoLineBuffer2[24] = "fw demo 2026-09-13";
// Fill the INFO screen with the SE050's applet version.
void uiFillInfo() {
  uint8_t response[32]; int length = -1;
  if (t1LinkReset()) length = t1SendApdu(SE_SELECT_APPLET, sizeof(SE_SELECT_APPLET), response, sizeof(response));
  if (length >= 9 && response[length-2] == 0x90 && response[length-1] == 0x00)
    snprintf(infoLineBuffer1, sizeof(infoLineBuffer1), "SE050 v%d.%d.%d  0x%02X%02X", response[0], response[1], response[2], response[3], response[4]);
  else
    snprintf(infoLineBuffer1, sizeof(infoLineBuffer1), "SE050: no answer (%d)", length);
  ui.infoLine1 = infoLineBuffer1; ui.infoLine2 = infoLineBuffer2;
}
void cmdVer() { cmdApdu("80 04 00 20 00"); }
void cmdSel() { cmdApdu("00 A4 04 00 10 A0 00 00 03 96 54 53 00 00 00 01 03 00 00 00 00 00"); }

// ------------------------------------------------------------- current estimate
// No shunt: current is estimated from the cell's sag, I = (V0 - V) / R.
// R is measured with a known load step (all OLED pixels on). V0 is anchored
// at a USB unplug, where the cell goes from +42 mA charging to -I_load, so
// the voltage step = (42 + I_load) * R. Unknown until an unplug is seen.
// (The unplug tracker is currently parked: AMPS_TRACKER_ENABLED.)
#define OLED_STEP_MA_DEFAULT 8.0f      // all-on step at max contrast, referred to the cell
#define CHARGE_CURRENT_MA 42.0f        // MCP73831, R15 = 24k
#define CELL_CAPACITY_MAH 60.0f
#define AMPS_TRACKER_ENABLED false
struct SavedAmps { uint32_t magic; float oledStepMa, cellResistance; };
#define AMPS_MAGIC 0x414D5032u         // "AMP2"
float oledStepMa = OLED_STEP_MA_DEFAULT;
float cellResistanceOhms = 0;          // 0 = not measured
float anchorOpenCircuitVolts = 0;
float anchorPercent = 0;               // OCV-table percent at the anchor
float mahUsedSinceAnchor = 0;
uint32_t lastIntegrateMs = 0;
bool  currentAnchored = false;
float lastSagMv = 0, lastUnplugStepMv = 0, lastLoadMa = 0;

void ampsLoad() {
  SavedAmps saved; EEPROM.get(EEPROM_AMPS_ADDR, saved);
  if (saved.magic == AMPS_MAGIC && saved.oledStepMa > 1 && saved.oledStepMa < 200) {
    oledStepMa = saved.oledStepMa; cellResistanceOhms = saved.cellResistance;
  }
}
void ampsSave() {
  SavedAmps saved = { AMPS_MAGIC, oledStepMa, cellResistanceOhms };
  EEPROM.put(EEPROM_AMPS_ADDR, saved); EEPROM.commit();
}

// Cell voltage averaged over 32 samples (millivolt resolution).
float cellVoltsPrecise() {
  float sum = 0;
  for (int i = 0; i < 32; i++) { sum += batteryVolts(); delayMicroseconds(300); }
  return sum / 32;
}

// One load step: voltage before/after minus during all-pixels-on.
static float oledLoadStepSagVolts() {
  float before = cellVoltsPrecise();
  oled.ssd1306_command(OLED_CMD_CONTRAST); oled.ssd1306_command(0xFF);
  oled.ssd1306_command(OLED_CMD_ALL_PIXELS_ON);
  delay(500);
  float loaded = cellVoltsPrecise();
  oled.ssd1306_command(OLED_CMD_FOLLOW_RAM);
  oled.ssd1306_command(OLED_CMD_CONTRAST); oled.ssd1306_command(0x8F);
  delay(500);
  float after = cellVoltsPrecise();
  return ((before + after) / 2) - loaded;
}

static float medianOfThree(float a, float b, float c) {
  return (a > b) ? ((b > c) ? b : (a > c ? c : a)) : ((a > c) ? a : (b > c ? c : b));
}

// Measure cell resistance with the OLED load step (battery only, real panel).
bool ampsMeasureResistance() {
  if (!oledReady || oledVirtual || onUsb()) return false;
  float sag1 = oledLoadStepSagVolts(), sag2 = oledLoadStepSagVolts(), sag3 = oledLoadStepSagVolts();
  float sag = medianOfThree(sag1, sag2, sag3);
  lastSagMv = sag * 1000;
  Serial.printf("R: OLED step sags %.1f %.1f %.1f mV, median %.1f\n", sag1*1000, sag2*1000, sag3*1000, lastSagMv);
  float resistance = sag / (oledStepMa / 1000.0f);
  if (sag < 0.004f || resistance < 0.15f || resistance > 2.5f) {
    Serial.printf("R: %.2f ohm not plausible, keeping %s\n", resistance, cellResistanceOhms > 0 ? "old value" : "unknown");
    return cellResistanceOhms > 0;
  }
  cellResistanceOhms = resistance;
  Serial.printf("R: %.1f mV / %.0f mA = %.2f ohm\n", lastSagMv, oledStepMa, cellResistanceOhms);
  ampsSave();
  return true;
}

// Inverse of the gauge's OCV curve: percent -> resting voltage.
float ocvForPercent(int percent) {
  static const struct { float volts; int percent; } curve[] = {
      {3.30f,0},{3.50f,10},{3.60f,25},{3.70f,45},{3.80f,60},
      {3.90f,75},{4.00f,85},{4.10f,95},{4.20f,100}};
  if (percent <= 0) return curve[0].volts;
  for (int i = 1; i < 9; i++)
    if (percent < curve[i].percent)
      return curve[i-1].volts + (float)(percent - curve[i-1].percent) / (curve[i].percent - curve[i-1].percent) * (curve[i].volts - curve[i-1].volts);
  return 4.20f;
}

// V0 now: the anchor walked down the OCV curve by the charge used since.
float openCircuitVoltsNow() {
  float percent = anchorPercent - mahUsedSinceAnchor / CELL_CAPACITY_MAH * 100.0f;
  if (percent < 0) percent = 0;
  return anchorOpenCircuitVolts + ocvForPercent((int)(percent + 0.5f)) - ocvForPercent((int)(anchorPercent + 0.5f));
}

// Current in mA (+ = discharging), NAN if unknown.
float currentEstimateMa() {
  if (onUsb() || !currentAnchored || cellResistanceOhms <= 0) return NAN;
  return (openCircuitVoltsNow() - gauge.filtered_volts) / cellResistanceOhms * 1000.0f;
}

void ampsIntegrate() {
  if (!currentAnchored) return;
  uint32_t now = millis();
  if (lastIntegrateMs) {
    float currentMa = currentEstimateMa();
    if (!isnan(currentMa) && currentMa > 0) mahUsedSinceAnchor += currentMa * (now - lastIntegrateMs) / 3600000.0f;
  }
  lastIntegrateMs = now;
}

// Every loop: watch for the USB unplug edge and read the cell 100 ms later
// (early enough to see only the I*R step, before the slow sag).
void ampsPoll() {
  static uint32_t lastUsbSampleMs = 0; static bool wasOnUsb = true; static float usbVolts = 0;
  static uint32_t unpluggedAt = 0; static bool measuredAfterUnplug = false; static bool firstCall = true;
  bool usb = onUsb();
  if (firstCall) { firstCall = false; wasOnUsb = usb; if (!usb) return; }   // booted on battery: no edge
  if (usb) {
    if (millis() - lastUsbSampleMs > 250) { lastUsbSampleMs = millis(); usbVolts = cellVoltsPrecise(); }
    currentAnchored = false; unpluggedAt = 0;
  } else if (wasOnUsb) {
    unpluggedAt = millis(); measuredAfterUnplug = false;
  } else if (!unpluggedAt) {
    static uint32_t lastIntegrateCallMs = 0;
    if (millis() - lastIntegrateCallMs >= 250) { lastIntegrateCallMs = millis(); ampsIntegrate(); }
  } else if (AMPS_TRACKER_ENABLED && unpluggedAt && millis() - unpluggedAt >= 100 && !measuredAfterUnplug) {
    float batteryNow = cellVoltsPrecise();
    lastUnplugStepMv = (usbVolts - batteryNow) * 1000;
    measuredAfterUnplug = true;
    ampsMeasureResistance();
    if (cellResistanceOhms > 0) {
      float loadMa = lastUnplugStepMv / 1000.0f / cellResistanceOhms * 1000.0f - CHARGE_CURRENT_MA;
      if (loadMa < 0) loadMa = 0;
      if (loadMa > 150) {
        Serial.printf("amps: implied load %.0f mA not plausible (step %.1f mV, R %.2f) - not anchoring\n", loadMa, lastUnplugStepMv, cellResistanceOhms);
      } else {
        lastLoadMa = loadMa;
        anchorOpenCircuitVolts = batteryNow + loadMa / 1000.0f * cellResistanceOhms;
        anchorPercent = gauge_percent_from_ocv(anchorOpenCircuitVolts);
        mahUsedSinceAnchor = 0; lastIntegrateMs = 0; currentAnchored = true;
        Serial.printf("amps: unplug step %.1f mV -> load %.0f mA, V0 %.3f V\n", lastUnplugStepMv, loadMa, anchorOpenCircuitVolts);
      }
    }
    unpluggedAt = 0;
  }
  wasOnUsb = usb;
}

void cmdAmps(const char *arg) {
  if (!strncmp(arg, "cal", 3)) {                 // "amps cal 21": meter says the OLED step is 21 mA
    float stepMa = atof(arg + 3);
    if (stepMa > 1 && stepMa < 200) {
      oledStepMa = stepMa; cellResistanceOhms = 0; ampsSave();
      Serial.printf("OLED step set to %.1f mA; R will be re-measured\n", stepMa);
    } else {
      Serial.println("usage: amps cal <mA of the OLED all-on step, from a meter>");
    }
    return;
  }
  if (!strcmp(arg, "r")) { ampsMeasureResistance(); return; }
  float currentMa = currentEstimateMa();
  Serial.printf("cell %.3f V  R %s  anchor %s  oled step %.0f mA  [last: sag %.1f mV, unplug step %.1f mV, load %.0f mA]\n  now: ",
                gauge.filtered_volts, cellResistanceOhms > 0 ? String(cellResistanceOhms, 2).c_str() : "?", currentAnchored ? "yes" : "no",
                oledStepMa, lastSagMv, lastUnplugStepMv, lastLoadMa);
  if (isnan(currentMa)) Serial.println(onUsb() ? "on USB (charging at up to 42 mA)" : "-- (unplug USB once to anchor)");
  else Serial.printf("~%.0f mA  (V0 now %.3f, %.2f mAh used since anchor)\n", currentMa, openCircuitVoltsNow(), mahUsedSinceAnchor);
  Serial.println("  amps r      re-measure R with the OLED step (battery only)\n  amps cal N  set the OLED step current from a meter reading");
}

// --- Edge traces: 1 ms samples of the cell around USB unplug / plug-in,
// kept in RAM and printed with "trace".
#define TRACE_SAMPLES 400
#define TRACE_PRETRIGGER 64
uint16_t traceUnplug[TRACE_SAMPLES], tracePlugIn[TRACE_SAMPLES];
int traceUnplugCount = 0, tracePlugInCount = 0;
static uint16_t traceRing[TRACE_PRETRIGGER]; static uint8_t traceRingIndex = 0;

// Fill the rest of a trace (the first `preSamples` are already there).
static void captureTrace(uint16_t *buffer, int *count, int preSamples) {
  for (int i = preSamples; i < TRACE_SAMPLES; i++) { buffer[i] = analogRead(PIN_VBAT_SENSE); delayMicroseconds(950); }
  *count = TRACE_SAMPLES;
}

void traceTick() {
  static uint32_t lastSampleMs = 0; static bool wasOnUsb = true; static bool firstCall = true;
  uint32_t now = millis();
  if (firstCall) { firstCall = false; wasOnUsb = onUsb(); }
  if (now != lastSampleMs) { lastSampleMs = now; traceRing[traceRingIndex++ & 63] = analogRead(PIN_VBAT_SENSE); }
  bool usb = onUsb();
  if (usb != wasOnUsb) {
    uint16_t *buffer = usb ? tracePlugIn : traceUnplug;
    int *count = usb ? &tracePlugInCount : &traceUnplugCount;
    for (int i = 0; i < TRACE_PRETRIGGER; i++) buffer[i] = traceRing[(traceRingIndex + i) & 63];   // oldest first
    captureTrace(buffer, count, TRACE_PRETRIGGER);
    wasOnUsb = usb;
  }
}

void cmdTrace() {
  for (int k = 0; k < 2; k++) {
    uint16_t *buffer = k ? tracePlugIn : traceUnplug;
    int count = k ? tracePlugInCount : traceUnplugCount;
    Serial.printf("%s trace: %d samples, 1 ms each, edge at sample 64, mV at the cell\n", k ? "PLUG-IN" : "UNPLUG", count);
    for (int i = 0; i < count; i++) Serial.printf("%d%c", (int)(buffer[i] * 3300.0f / 4095.0f * 2.0f), (i % 20 == 19) ? '\n' : ' ');
    Serial.println();
  }
}

// ------------------------------------------------------------ USB keyboard
// The board is a CDC console and a HID keyboard. Text is typed one key
// event per 8 ms from loop() so the UI keeps animating.
uint8_t const hidReportDescriptor[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
Adafruit_USBD_HID usb_hid(hidReportDescriptor, sizeof(hidReportDescriptor), HID_ITF_PROTOCOL_KEYBOARD, 2, false);
void hidBegin() {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.setManufacturerDescriptor("Lomenett");
  TinyUSBDevice.setProductDescriptor("Lomenett Vault");
  usb_hid.begin();
  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }   // re-enumerate with HID
}

const char *typeText = nullptr;        // string being typed, nullptr = idle
int      typeIndex = 0;
bool     typeKeyDown = false;
uint32_t typeLastStepMs = 0;
bool     typePressEnter = false;       // press Enter after the string
bool     typeEnterSent = false;
uint8_t  typeShortcutModifier = 0, typeShortcutKey = 0;   // key combo before the string (e.g. Win+R)
bool     typeShortcutDown = false;
uint32_t typeShortcutWaitMs = 0;       // wait after the combo for its window to open
uint32_t typePauseUntil = 0;
static char typeComposeBuffer[VAULT_USER_LEN + 8 + VAULT_PW_LEN + 2];

void typeStart(const char *text) {
  typeText = text; typeIndex = 0; typeKeyDown = false; typeEnterSent = false; typePressEnter = false;
  typeShortcutKey = 0; typeShortcutDown = false; typePauseUntil = 0; typeLastStepMs = millis();
  vaultUi.isTyping = true; vaultUi.charsTyped = 0; vaultUi.charsTotal = strlen(text);
}

// Type a vault entry: TYPE_PASSWORD, TYPE_USERNAME, or username + TABs
// (entry's count, default 1) + password. Enter follows the password if the
// entry asks for it, never after a bare username.
void typeStartEntry(int index, int mode) {
  if (index < 0 || index >= vault.count) return;
  VaultEntry &entry = vault.entries[index];
  typeComposeBuffer[0] = 0;
  if (mode == TYPE_USERNAME) {
    strlcpy(typeComposeBuffer, entry.username, sizeof(typeComposeBuffer));
  } else if (mode == TYPE_USER_AND_PASSWORD) {
    strlcpy(typeComposeBuffer, entry.username, sizeof(typeComposeBuffer));
    int tabCount = entry.tabCount ? entry.tabCount : 1;
    for (int i = 0; i < tabCount; i++) strlcat(typeComposeBuffer, "\t", sizeof(typeComposeBuffer));
    strlcat(typeComposeBuffer, entry.secret, sizeof(typeComposeBuffer));
  } else {
    strlcpy(typeComposeBuffer, entry.secret, sizeof(typeComposeBuffer));
  }
  typeStart(typeComposeBuffer);
  typePressEnter = entry.pressEnter && mode != TYPE_USERNAME;
}

// One key event per call (8 ms apart): shortcut down/up, then each char down/up, then Enter.
void typeTick() {
  if (!typeText) return;
  if (millis() - typeLastStepMs < 8) return;
  typeLastStepMs = millis();
  if (!usb_hid.ready()) return;
  if (typePauseUntil) {
    if ((int32_t)(millis() - typePauseUntil) < 0) return;
    typePauseUntil = 0;
  }
  if (typeShortcutDown) {
    usb_hid.keyboardRelease(0); typeShortcutDown = false;
    typePauseUntil = millis() + typeShortcutWaitMs;
    if (!typePauseUntil) typePauseUntil = 1;
    return;
  }
  if (typeShortcutKey) {
    uint8_t keycodes[6] = { typeShortcutKey };
    usb_hid.keyboardReport(0, typeShortcutModifier, keycodes);
    typeShortcutKey = 0; typeShortcutDown = true;
    return;
  }
  if (typeKeyDown) {
    usb_hid.keyboardRelease(0); typeKeyDown = false;
    typeIndex++; vaultUi.charsTyped = typeIndex;
    return;
  }
  if (!typeText[typeIndex]) {
    if (typePressEnter && !typeEnterSent) {
      uint8_t keycodes[6] = { HID_KEY_ENTER };
      usb_hid.keyboardReport(0, 0, keycodes);
      typeKeyDown = true; typeEnterSent = true;
      typeIndex--;                          // the release step advances it back
      return;
    }
    typeText = nullptr; vaultUi.isTyping = false;
    return;
  }
  usb_hid.keyboardPress(0, typeText[typeIndex]);   // ASCII -> US-layout keycode
  typeKeyDown = true;
}

// Clock sync with nothing installed: type a one-line command that writes
// "time <unix>" to this device's serial port. Linux/Mac need a terminal
// focused; Windows gets Win+R and a hidden PowerShell. US layout.
static const char *const CLOCK_SYNC_COMMAND[3] = {
  "echo time $(date +%s) > /dev/serial/by-id/usb-Lomenett*",
  "echo time $(date +%s) > /dev/cu.usbmodem*",
  "powershell -w h -c \"$p=[IO.Ports.SerialPort]::new((gcim Win32_SerialPort|? PNPDeviceID -like '*VID_239A*').DeviceID);"
    "$p.DtrEnable=1;$p.Open();$p.WriteLine('time '+[DateTimeOffset]::UtcNow.ToUnixTimeSeconds());sleep 1;$p.Close()\"" };
void typeClockSync(int os) {
  if (os < OS_LINUX || os > OS_WINDOWS) return;
  typeStart(CLOCK_SYNC_COMMAND[os]);
  typePressEnter = true;
  if (os == OS_WINDOWS) {                       // open the Run box first
    typeShortcutModifier = KEYBOARD_MODIFIER_LEFTGUI; typeShortcutKey = HID_KEY_R; typeShortcutWaitMs = 900;
  }
}

// ------------------------------------------------------------ PIN flows
// Wrong PINs are counted in /fails.bin; after 3, a 5 s penalty per try.
uint32_t pinPenaltyUntil = 0;

void redrawWhileBusy(uint32_t now) {
  ui.busy = true;
  uiFrame(oled, ui, 0, false, false, batteryPercent(), onUsb(), gauge.filtered_volts, now);
  ui.busy = false;
}

void pinHandle(String typedPin, uint32_t now) {
  if (ui.pinMode == PIN_UNLOCK) {
    if (now < pinPenaltyUntil) { ui.pinShake = 1; Serial.println("PIN: penalty wait"); return; }
    // Ring sweeps to 80%, key derivation runs, quick finish, then the result.
    {
      const uint32_t PRE_MS = 1200; uint32_t startMs = millis(), elapsed;
      while ((elapsed = millis() - startMs) < PRE_MS) { if (oledReady) drawDecrypt(oled, 0.80f * elapsed / PRE_MS, DECRYPT_WORKING, millis()); }
    }
    bool pinCorrect = loginPin(typedPin);
    {
      uint32_t startMs = millis(), elapsed;
      while ((elapsed = millis() - startMs) < 200) { if (oledReady) drawDecrypt(oled, 0.80f + 0.20f * elapsed / 200.0f, DECRYPT_WORKING, millis()); }
    }
    {
      uint32_t startMs = millis();
      while (millis() - startMs < 450) {
        if (oledReady) drawDecrypt(oled, 1.0f, pinCorrect ? DECRYPT_SUCCESS : DECRYPT_FAILED, millis());
        if (!oledReady) break;
      }
    }
    if (pinCorrect) {
      failReset(); vaultLoad(); ui.locked = false; uiHome(ui);
      Serial.printf("PIN ok, %d entries, %s\n", vault.count, vaultIsSeBound() ? "SE050-bound" : "LEGACY (not chip-bound)");
      if (!vaultIsSeBound()) {                                   // one-time migration onto the SE050
        String keep = vaultBlob;
        if (setupPin(typedPin) && vaultIsSeBound()) { vaultCommit(keep); Serial.println("vault migrated: now SE050-bound"); }
        else Serial.println("migration skipped: SE050 not answering");
      }
    } else {
      failIncrement(); ui.pinShake = 1;
      int failures = failCount();
      Serial.printf("wrong PIN (%d)\n", failures);
      if (failures >= 3) pinPenaltyUntil = now + 5000;
    }
  } else if (ui.pinMode == PIN_CURRENT) {
    redrawWhileBusy(now);
    if (loginPin(typedPin)) ui.pinMode = PIN_NEW; else ui.pinShake = 1;
  } else if (ui.pinMode == PIN_BACKUP) {                         // confirm identity, then export
    if (loginPin(typedPin)) {
      mscBackupText = makeBackup(typedPin); mscMode = MSC_MODE_BACKUP;
      uiBackOne(ui); uiGo(ui, UI_EDITMODE);
      ui.haveBackup = true; prefsSave();
      Serial.println("backup: export drive up");
    } else {
      ui.pinShake = 1;
    }
  } else if (ui.pinMode == PIN_RESTORE) {                        // decrypt the dropped backup with its PIN
    if (restoreBackup(mscBackupText, typedPin)) {
      mscBackupText = ""; vaultLoad(); ui.locked = false; ui.pinMode = PIN_UNLOCK;
      uiHome(ui); uiToast(ui, "RESTORED", now + 1200, UI_HOME);
      Serial.printf("restore ok, %d entries\n", vault.count);
    } else {
      ui.pinShake = 1; Serial.println("restore: wrong PIN or bad file");
    }
  } else if (ui.pinMode == PIN_NEW) {
    strcpy(ui.newPin, typedPin.c_str()); ui.pinMode = PIN_CONFIRM;
  } else {                                                       // PIN_CONFIRM
    if (typedPin != String(ui.newPin)) { ui.pinShake = 1; ui.pinMode = PIN_NEW; return; }
    redrawWhileBusy(now);
    String keep = vaultBlob;                                     // re-encrypt under the new key
    setupPin(typedPin); vaultCommit(keep); failReset();
    bool firstBoot = ui.navDepth == 0;
    ui.locked = false; ui.pinMode = PIN_UNLOCK;
    if (firstBoot) { uiHome(ui); uiToast(ui, "PIN SET", now + 900, UI_HOME); }
    else { uiBackOne(ui); uiToast(ui, "PIN SET", now + 900, ui.screen); }
    Serial.println("PIN set, vault re-encrypted");
  }
  memset(ui.pinTyped, 0, sizeof(ui.pinTyped));
}

// ------------------------------------------------------------ log buffer
// Serial drops across the EDIT re-enumeration, so the last ~3 KB of output
// is also kept here and printed by "log".
static char logBuffer[3072]; static int logPosition = 0; static bool logWrapped = false;
class LogPrint : public Print {
public:
  size_t write(uint8_t ch) override {
    logBuffer[logPosition++] = ch;
    if (logPosition >= (int)sizeof(logBuffer)) { logPosition = 0; logWrapped = true; }
    return Serial.write(ch);
  }
  size_t write(const uint8_t *data, size_t length) override {
    for (size_t i = 0; i < length; i++) write(data[i]);
    return length;
  }
};
static LogPrint Log;
Print &LogRef = Log;
void cmdLog() {
  Serial.println("---- log ----");
  if (logWrapped) Serial.write((const uint8_t*)logBuffer + logPosition, sizeof(logBuffer) - logPosition);
  Serial.write((const uint8_t*)logBuffer, logPosition);
  Serial.println("---- end ----");
}

// ------------------------------------------------------------ password generator
// Random bytes from the SE050 TRNG (falls back to the RP ring oscillator).
static char newPasswordCandidate[VAULT_PW_LEN + 1];
void generatePassword(char *output, int length) {
  static const char *ALPHABET = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789!@#$%&*?-_+";  // no 0O1lI
  int alphabetSize = strlen(ALPHABET);
  uint8_t randomBytes[64];
  if (length > 63) length = 63;
  if (!seRandom(randomBytes, length)) roscRandomBytes(randomBytes, length);
  for (int i = 0; i < length; i++) output[i] = ALPHABET[randomBytes[i] % alphabetSize];
  output[length] = 0;
  memset(randomBytes, 0, sizeof(randomBytes));
}

// CSV import progress (msc.h mscProgress hook).
static char importToastText[24];
void csvProgress(int done, int total) {
  if (!oledReady) return;
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print("Importing...");
  char countText[24]; snprintf(countText, sizeof(countText), "%d / %d", done, total);
  oled.setCursor(0, 13); oled.print(countText);
  int barWidth = oled.width() - 4;
  oled.drawRect(2, 24, barWidth, 6, SSD1306_WHITE);
  oled.fillRect(3, 25, total ? (barWidth - 2) * done / total : 0, 4, SSD1306_WHITE);
  oled.display();
}

// Save progress on screen (secure.h saveStageHook): step name + elapsed time,
// so a freeze shows where it stopped. Screen only, no serial.
static uint32_t saveStageStartMs = 0;
void saveStageShow(const char *stepName) {
  if (!oledReady) return;
  if (!strcmp(stepName, "tidying lines")) saveStageStartMs = millis();
  char elapsedText[24]; snprintf(elapsedText, sizeof(elapsedText), "%lu ms", (unsigned long)(millis() - saveStageStartMs));
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print(stepName);
  oled.setCursor(0, 13); oled.print(elapsedText);
  oled.display();
}

// ------------------------------------------------------------ I2C diagnostics

void restartI2c() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
}

void cmdScan() {
  Serial.println("scanning I2C");
  int found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X", address);
      if (address == OLED_I2C_ADDRESS)  Serial.print("  OLED");
      if (address == accelI2cAddress)   Serial.print("  LIS3DH accelerometer");
      if (address == SE050_I2C_ADDRESS) Serial.print("  SE050 secure element");
      Serial.println();
      found++;
    }
  }
  if (!found) Serial.println("  nothing answered");
}

// SDA/SCL as plain pins: idle both should read 1 (4.7k pull-ups); stuck low =
// something holds the bus; never high = pull-ups unpowered. Ends with 9
// clocks to free a slave stuck mid-byte.
void cmdPins() {
  Wire.end();
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(2);
  Serial.printf("idle       SDA %d  SCL %d   (expect 1 1)\n", digitalRead(PIN_SDA), digitalRead(PIN_SCL));

  pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, LOW);
  pinMode(PIN_SCL, OUTPUT); digitalWrite(PIN_SCL, LOW);
  delay(2);
  Serial.printf("driven low SDA %d  SCL %d   (expect 0 0)\n", digitalRead(PIN_SDA), digitalRead(PIN_SCL));

  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(5);
  Serial.printf("released   SDA %d  SCL %d   (expect 1 1)\n", digitalRead(PIN_SDA), digitalRead(PIN_SCL));

  Serial.print("bus recovery... ");
  pinMode(PIN_SCL, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(PIN_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(PIN_SCL, HIGH); delayMicroseconds(5);
  }
  pinMode(PIN_SCL, INPUT);
  delay(2);
  Serial.printf("SDA now %d\n", digitalRead(PIN_SDA));

  restartI2c();
  Serial.println("I2C restarted, try scan again");
}

// Bit-banged scan (no I2C peripheral). Finds chips the hardware scan misses =
// peripheral misconfigured; finds nothing = nothing is answering.
// Lines are only driven low (OUTPUT, value LOW) or released (INPUT).
static uint32_t bitBangDelayUs = 5;
static int bitBangSdaPin = PIN_SDA, bitBangSclPin = PIN_SCL;
static void bitBangWait()       { delayMicroseconds(bitBangDelayUs); }
static void bitBangSdaLow()     { pinMode(bitBangSdaPin, OUTPUT); }
static void bitBangSdaRelease() { pinMode(bitBangSdaPin, INPUT);  }
static void bitBangSclLow()     { pinMode(bitBangSclPin, OUTPUT); }
static void bitBangSclRelease() { pinMode(bitBangSclPin, INPUT);  }

static void bitBangStart() {
  bitBangSdaRelease(); bitBangSclRelease(); bitBangWait();
  bitBangSdaLow(); bitBangWait();
  bitBangSclLow(); bitBangWait();
}
static void bitBangStop() {
  bitBangSdaLow(); bitBangWait();
  bitBangSclRelease(); bitBangWait();
  bitBangSdaRelease(); bitBangWait();
}

// Returns 1 if the slave acknowledged.
static int bitBangWriteByte(uint8_t value) {
  for (int i = 0; i < 8; i++) {
    if (value & 0x80) bitBangSdaRelease(); else bitBangSdaLow();
    value <<= 1;
    bitBangWait(); bitBangSclRelease(); bitBangWait(); bitBangSclLow(); bitBangWait();
  }
  bitBangSdaRelease();
  bitBangWait(); bitBangSclRelease(); bitBangWait();
  int acknowledged = (digitalRead(bitBangSdaPin) == LOW);
  bitBangSclLow(); bitBangWait();
  return acknowledged;
}

static void bitBangScan(const char *label) {
  Wire.end();
  digitalWrite(PIN_SDA, LOW);
  digitalWrite(PIN_SCL, LOW);
  bitBangSdaRelease(); bitBangSclRelease();
  delay(2);

  Serial.println(label);
  int found = 0;
  for (uint8_t address = 1; address < 127; address++) {
    bitBangStart();
    int acknowledged = bitBangWriteByte(address << 1);
    bitBangStop();
    if (acknowledged) { Serial.printf("  0x%02X answered\n", address); found++; }
  }
  if (!found) Serial.println("  nothing answered");
  restartI2c();
}

void cmdBitBangScan() { bitBangDelayUs = 5;   bitBangScan("bit-banged scan"); }
void cmdSlowScan()    { bitBangDelayUs = 150; bitBangScan("slow scan (~2 kHz, tolerates >100 us rise)"); bitBangDelayUs = 5; }

// Bit-banged scan with SDA/SCL swapped: catches a swap anywhere in the chain.
void cmdSwapScan() {
  bitBangSdaPin = PIN_SCL; bitBangSclPin = PIN_SDA;
  bitBangDelayUs = 150;
  bitBangScan("SWAPPED scan (SDA<->SCL roles exchanged, ~2 kHz)");
  bitBangDelayUs = 5;
  bitBangSdaPin = PIN_SDA; bitBangSclPin = PIN_SCL;
}

// Toggle both lines at 500 Hz for 30 s, to trace with a DC voltmeter:
// ~1.6 V = signal arrives, ~3.3 V = not arriving, ~0 V = stuck low.
void cmdWiggle() {
  Wire.end();
  Serial.println("wiggling SDA+SCL at 500 Hz for 30 s - probe the bus with a DC voltmeter");
  Serial.println("  ~1.6 V = signal arrives there   ~3.3 V = signal NOT arriving   ~0 V = stuck");
  uint32_t endMs = millis() + 30000;
  while (millis() < endMs) {
    pinMode(PIN_SDA, OUTPUT); pinMode(PIN_SCL, OUTPUT);   // drive low
    delay(1);
    pinMode(PIN_SDA, INPUT);  pinMode(PIN_SCL, INPUT);    // release
    delay(1);
  }
  Serial.println("done");
  restartI2c();
}

// Hold one line low, read the other: both must read 1, else SDA and SCL are shorted.
void cmdCrossShort() {
  Wire.end();
  pinMode(PIN_SDA, INPUT); pinMode(PIN_SCL, INPUT);
  delayMicroseconds(100);
  pinMode(PIN_SDA, OUTPUT); digitalWrite(PIN_SDA, LOW);
  delayMicroseconds(100);
  int sclWhileSdaLow = digitalRead(PIN_SCL);
  pinMode(PIN_SDA, INPUT);
  delayMicroseconds(100);
  pinMode(PIN_SCL, OUTPUT); digitalWrite(PIN_SCL, LOW);
  delayMicroseconds(100);
  int sdaWhileSclLow = digitalRead(PIN_SDA);
  pinMode(PIN_SCL, INPUT);
  Serial.printf("SDA held low -> SCL reads %d   (1 = ok, 0 = BRIDGED)\n", sclWhileSdaLow);
  Serial.printf("SCL held low -> SDA reads %d   (1 = ok, 0 = BRIDGED)\n", sdaWhileSclLow);
  if (!sclWhileSdaLow || !sdaWhileSclLow) Serial.println("*** SDA and SCL are SHORTED TOGETHER somewhere on the board ***");
  else                                    Serial.println("no SDA<->SCL bridge");
  restartI2c();
}

// Rise time of each line after release (4.7k pull-ups: ~0 us).
void cmdRise() {
  Wire.end();
  const struct { const char *name; int pin; } lines[] = {{"SDA", PIN_SDA}, {"SCL", PIN_SCL}};
  for (auto &line : lines) {
    pinMode(line.pin, OUTPUT);
    digitalWrite(line.pin, LOW);
    delayMicroseconds(300);
    uint32_t releasedAt = micros();
    pinMode(line.pin, INPUT);
    while (!digitalRead(line.pin) && micros() - releasedAt < 100000) {}
    uint32_t riseUs = micros() - releasedAt;
    if (riseUs >= 100000) Serial.printf("%s never rose within 100 ms\n", line.name);
    else Serial.printf("%s rose in %lu us%s\n", line.name, riseUs, riseUs <= 3 ? "  (healthy for 4.7k)" : "  (TOO SLOW for 4.7k)");
  }
  restartI2c();
}

void cmdTest() {
  Serial.println("\nself test");

  Serial.print("  OLED         ");
  Serial.println(oledVirtual ? "FAILED (virtual panel for the bridge)" : oledReady ? "ok" : "FAILED");

  Serial.print("  accelerometer ");
  if (!accelReady) {
    Serial.println("FAILED");
  } else {
    int x, y, z; accelRead(x, y, z);
    long magnitudeSquared = (long)x*x + (long)y*y + (long)z*z;   // ~1 g at rest
    Serial.println((magnitudeSquared > 250000L && magnitudeSquared < 3000000L) ? "ok" : "reads oddly, is it moving?");
  }

  Serial.print("  wheel        turn it now... ");
  long startPosition = wheelDetents;
  uint32_t deadline = millis() + 3000;
  while (millis() < deadline && wheelDetents == startPosition) delay(1);
  Serial.println(wheelDetents != startPosition ? "ok" : "no movement seen");

  Serial.print("  select btn   press it now... ");
  deadline = millis() + 3000;
  bool pressed = false;
  while (millis() < deadline && !pressed) if (selectPressedEdge()) pressed = true;
  Serial.println(pressed ? "ok" : "no press seen");

  Serial.printf("  battery      %.2f V\n", batteryVolts());
  Serial.println("done");
}

// ------------------------------------------------------------ 2FA
// Type the current code of a 2FA entry (refused without a synced clock).
static char totpTypeBuffer[10];
bool typeTotp(int index) {
  if (index < 0 || index >= vault.count || vault.entries[index].kind != KIND_TOTP) return false;
  if (!totpClockValid()) { Serial.println("2FA: clock not set (time <unix>)"); return false; }
  VaultEntry &entry = vault.entries[index];
  if (!totpCode(entry.secret, totpDigits(entry.expiry), totpPeriod(entry.cvv), totpClockNow(), totpTypeBuffer)) return false;
  typeStart(totpTypeBuffer);
  typePressEnter = entry.pressEnter;
  return true;
}

// Type backup code `codeIndex` of a 2FA entry.
static char totpBackupBuffer[TOTP_BACKUP_LEN + 1];
bool typeTotpBackup(int index, int codeIndex) {
  String codes[TOTP_MAX_BACKUPS];
  int codeCount = totpBackups(index, codes);
  if (codeIndex < 0 || codeIndex >= codeCount) return false;
  strlcpy(totpBackupBuffer, codes[codeIndex].c_str(), sizeof(totpBackupBuffer));
  typeStart(totpBackupBuffer);
  typePressEnter = vault.entries[index].pressEnter;
  return true;
}

void cmdTime(const char *arg) {
  if (*arg) {
    if (totpClockSet(strtoull(arg, nullptr, 10))) Serial.printf("clock set: %llu\n", (unsigned long long)totpClockNow());
    else Serial.println("usage: time <unix seconds, UTC>  e.g. time $(date +%s)");
    return;
  }
  if (totpClockValid()) Serial.printf("clock %llu (unix), synced %lu s ago\n", (unsigned long long)totpClockNow(), (millis() - totpSyncMs) / 1000);
  else Serial.println("clock not set");
}

void cmdTotp(const char *arg) {
  if (ui.locked) { Serial.println("locked"); return; }
  if (!strncmp(arg, "add ", 4)) {                     // totp add <name> <secret | otpauth://...>
    const char *nameStart = arg + 4;
    while (*nameStart == ' ') nameStart++;
    const char *nameEnd = strchr(nameStart, ' ');
    if (!nameEnd) { Serial.println("usage: totp add <name> <base32 secret | otpauth:// link>"); return; }
    char name[VAULT_NAME_LEN + 1];
    int nameLength = nameEnd - nameStart;
    if (nameLength > VAULT_NAME_LEN) nameLength = VAULT_NAME_LEN;
    memcpy(name, nameStart, nameLength); name[nameLength] = 0;
    const char *secret = nameEnd;
    while (*secret == ' ') secret++;
    String linkLabel, linkSecret; int digits = 6, period = 30;
    bool added;
    if (strncmp(secret, "otpauth://", 10)) added = vaultAddTotp(name, secret);
    else added = totpParseUri(String(secret), linkLabel, linkSecret, digits, period) && vaultAddTotp(name, linkSecret.c_str(), digits, period);
    Serial.println(added ? "added" : "bad secret (base32, or an otpauth://totp link with SHA1)");
    return;
  }
  int totpCount = 0;
  for (int i = 0; i < vault.count; i++) {
    VaultEntry &entry = vault.entries[i];
    if (entry.kind != KIND_TOTP) continue;
    totpCount++;
    int period = totpPeriod(entry.cvv);
    char code[10] = "------";
    if (totpClockValid()) totpCode(entry.secret, totpDigits(entry.expiry), period, totpClockNow(), code);
    Serial.printf("  %2d  %-23s  %s", i, entry.name, code);
    if (totpClockValid()) Serial.printf("  (%d s left)", totpSecondsLeft(period));
    if (entry.backupCodeCount) Serial.printf("  +%d backup code%s", entry.backupCodeCount, entry.backupCodeCount > 1 ? "s" : "");
    Serial.println();
  }
  if (!totpCount) Serial.println("no 2FA entries (add one: totp add <name> <secret>, or 2FA.TXT in EDIT)");
  else if (!totpClockValid()) Serial.println("clock not set: time <unix>");
}

// "site <host> [both|user|pw]" from the browser extension: offer LOG IN for
// that site. Only the match count is sent back; nothing is typed until clicked.
void cmdSite(char *arg) {
  if (!*arg) { Serial.println("site: usage: site <host> [both|user|pw] | site off"); return; }
  if (!strcmp(arg, "off")) { uiCancelLogin(ui); Serial.println("site: off"); return; }
  char *modeText = strchr(arg, ' ');
  int mode = TYPE_USER_AND_PASSWORD;
  if (modeText) {
    *modeText++ = 0;
    while (*modeText == ' ') modeText++;
    mode = !strcmp(modeText, "pw") ? TYPE_PASSWORD : !strcmp(modeText, "user") ? TYPE_USERNAME : TYPE_USER_AND_PASSWORD;
  }
  int matches = uiOfferLogin(ui, arg, mode);
  if (matches == LOGIN_LOCKED)    Serial.println("site: locked");
  else if (matches == LOGIN_BUSY) Serial.println("site: busy");
  else if (matches == 0)          Serial.println("site: none");
  else                            Serial.printf("site: %d match\n", matches);
}

// "add <name> [user] <password>"
void cmdAdd(char *arg) {
  char *second = strchr(arg, ' ');
  if (!second) { Serial.println("usage: add <name> [user] <password>"); return; }
  *second++ = 0;
  while (*second == ' ') second++;
  char *third = strchr(second, ' ');
  if (third) {                                     // name user password
    *third++ = 0;
    while (*third == ' ') third++;
    Serial.println(vaultAdd(arg, third, second) ? "added" : "vault full or bad input");
  } else {                                         // name password
    Serial.println(vaultAdd(arg, second) ? "added" : "vault full or bad input");
  }
}

void cmdList() {
  Serial.printf("%d entries\n", vault.count);
  for (int i = 0; i < vault.count; i++) {
    VaultEntry &entry = vault.entries[i];
    if (entry.kind == KIND_TOTP)
      Serial.printf("  %2d  %-23s  2FA %s digits / %s s\n", i, entry.name, entry.expiry, entry.cvv);
    else
      Serial.printf("  %2d  %-23s  %-31s  %d chars%s%s\n", i, entry.name, entry.username, (int)strlen(entry.secret),
                    entry.tabCount ? " +tabs" : "", entry.kind ? " (card)" : "");
  }
}

void cmdSeTest() {
  Serial.printf("open: %s\n", seOpen() ? "ok" : "FAIL");
  uint8_t data[8]; int dataLength = tlvPutU32(data, SE_TAG_1, SE_KEK_ID);
  uint8_t response[64]; uint16_t statusWord;
  int length = seCmd(0x04, 0x00, 0x27, data, dataLength, true, response, sizeof(response), &statusWord);
  Serial.printf("exists: SW %04X rn %d: ", statusWord, length); hexdump(response, length > 0 ? length : 0);
  uint8_t randomRequest[4] = { SE_TAG_1, 0x02, 0x00, 0x08 };
  length = seCmd(0x04, 0x00, 0x49, randomRequest, 4, true, response, sizeof(response), &statusWord);
  Serial.printf("random: SW %04X rn %d: ", statusWord, length); hexdump(response, length > 0 ? length : 0);
}

// Print A/B/switch whenever any changes (runs 20 s).
void cmdEncoder() {
  Serial.println("A B SW  (8 s, turn the wheel / hold between clicks)");
  uint32_t endMs = millis() + 20000; int lastState = -1;
  while ((int32_t)(endMs - millis()) > 0) {
    int a = digitalRead(PIN_WHEEL_A), b = digitalRead(PIN_WHEEL_B), switchUp = digitalRead(PIN_WHEEL_SWITCH);
    int state = (a << 2) | (b << 1) | switchUp;
    if (state != lastState) { lastState = state; Serial.printf("%d %d %s\n", a, b, switchUp ? "up" : "DOWN"); }
    delay(1);
  }
  Serial.println("done");
}

void cmdOledInit() {
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  oledReady = (Wire.endTransmission() == 0);
  if (oledReady) {
    oledInit();
    oledPowerOn();
    oledLines("LOMENETT", "ready");
    Serial.println("OLED claimed and initialized");
  } else {
    Serial.println("still no ACK at 0x3C");
  }
}

void runCommand(char *line) {
  while (*line == ' ') line++;
  char *arg = strchr(line, ' ');
  if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = (char*)"";
  #define IS(name) (!strcmp(line, name))

  if      (IS("help") || IS("?")) cmdHelp();
  else if (IS("status"))   cmdStatus();
  else if (IS("scan"))     cmdScan();
  else if (IS("se050"))    cmdSe050(arg);
  else if (IS("apdu"))     cmdApdu(arg);
  else if (IS("ver"))      cmdVer();
#if REMOTE_PANEL
  else if (IS("rc"))       rcCommand(arg);
  else if (IS("mirror"))   rcMirrorCommand(arg);
#endif
  else if (IS("sel"))      cmdSel();
  else if (IS("amps"))     cmdAmps(arg);
  else if (IS("trace"))    cmdTrace();
  else if (IS("notch")) {
    int quarterSteps = atoi(arg);
    if (quarterSteps >= 1 && quarterSteps <= 4) { wheelQuarterStepsPerNotch = quarterSteps; wheelSettingsSave(); }
    Serial.printf("notch = %d quarter-steps per scroll\n", wheelQuarterStepsPerNotch);
  }
  else if (IS("wheelflip")) {
    if (*arg) { wheelFlipMask = atoi(arg) & 15; wheelSettingsSave(); }
    Serial.printf("wheelflip mask = %d (bit r = invert in rotation r)\n", wheelFlipMask);
  }
  else if (IS("splash"))   { splash.startedAt = millis(); splash.done = false; }
  else if (IS("add"))      cmdAdd(arg);
  else if (IS("list"))     cmdList();
  else if (IS("del"))      Serial.println(vaultDel(atoi(arg)) ? "deleted" : "no such entry");
  else if (IS("type")) {
    int index = atoi(arg);
    if (index >= vault.count) Serial.println("no such entry");
    else if (vault.entries[index].kind == KIND_TOTP) typeTotp(index);
    else typeStart(vault.entries[index].secret);
  }
  else if (IS("time"))     cmdTime(arg);
  else if (IS("site"))     cmdSite(arg);
  else if (IS("totp"))     cmdTotp(arg);
  else if (IS("edit")) {
    if (ui.locked) Serial.println("locked");
    else ui.screen = strcmp(arg, "off") ? UI_EDITMODE : UI_HOME;
  }
  else if (IS("unlock")) {
    if (strlen(arg) == 4 && vaultExists()) { ui.pinMode = PIN_UNLOCK; pinHandle(String(arg), millis()); }
    else Serial.println("usage: unlock NNNN");
  }
  else if (IS("setpin")) {
    if (strlen(arg) == 4 && !vaultExists()) { ui.pinMode = PIN_CONFIRM; strcpy(ui.newPin, arg); pinHandle(String(arg), millis()); }
    else Serial.println("setpin NNNN only on a fresh device");
  }
  else if (IS("log"))      cmdLog();
  else if (IS("enter")) {
    if (*arg) { ui.autoEnter = !strcmp(arg, "on"); prefsSave(); }
    Serial.printf("enter after typing: %s\n", ui.autoEnter ? "on" : "off");
  }
  else if (IS("gen"))      { char sample[24]; generatePassword(sample, 16); Serial.printf("sample generated pw: %s\n", sample); }
  else if (IS("lsfs")) {
    Dir directory = LittleFS.openDir("/");
    while (directory.next()) Serial.printf("  %s  %d bytes\n", directory.fileName().c_str(), (int)directory.fileSize());
  }
  else if (IS("setest"))   cmdSeTest();
  else if (IS("sestat")) {
    Serial.printf("KEK object in SE050: %s   vault: %s\n", seKekExists() ? "present" : "absent",
                  !vaultExists() ? "none" : vaultIsSeBound() ? "SE050-bound" : "legacy");
  }
  else if (IS("ui"))       { carouselUiEnabled = strcmp(arg, "off") != 0; Serial.printf("ui %s (%.0f fps)\n", carouselUiEnabled ? "on" : "off", ui.fps); }
  else if (IS("pins"))     cmdPins();
  else if (IS("bbscan"))   cmdBitBangScan();
  else if (IS("slowscan")) cmdSlowScan();
  else if (IS("rise"))     cmdRise();
  else if (IS("xshort"))   cmdCrossShort();
  else if (IS("wiggle"))   cmdWiggle();
  else if (IS("oledinit")) cmdOledInit();
  else if (IS("swapscan")) cmdSwapScan();
  else if (IS("test"))     cmdTest();
  else if (IS("accel"))    { int x, y, z; accelRead(x, y, z); Serial.printf("x %5d  y %5d  z %5d  mg\n", x, y, z); }
  else if (IS("stream")) {
    accelStreaming = !accelStreaming;
    Serial.println(accelStreaming ? "streaming, type stream again to stop" : "stopped");
  }
  else if (IS("wheel"))    Serial.printf("wheel %ld  switch %s\n", wheelDetents, wheelPressed() ? "down" : "up");
  else if (IS("zero"))     { wheelDetents = 0; wheelQuarterSteps = 0; Serial.println("wheel zeroed"); }
  else if (IS("buttons"))  Serial.printf("select pressed %u times\n", selectPressCount);
  else if (IS("oled"))     { oledLines(arg); Serial.println("shown"); }
  else if (IS("bridge"))   { if (*arg) bridgeSet(!strcmp(arg, "on")); else bridgeStatus(); }
  else if (IS("screen")) {
    statusScreenEnabled = !statusScreenEnabled;
    Serial.println(statusScreenEnabled ? "live screen on" : "live screen off");
  }
  else if (IS("led"))      ledForcedOn = !strcmp(arg, "on");
  else if (IS("enc"))      cmdEncoder();
  else if (IS("orient")) {
    Serial.printf("accel x %d y %d z %d mg  sector %d  rotation %d  autorot %s\n",
                  accelX, accelY, accelZ, orientation.sector, displayRotation, autoRotate ? "on" : "off");
  }
  else if (IS("rot")) {
    autoRotate = false;
    applyRotation(atoi(arg) & 3);
    Serial.printf("rotation forced to %d (autorot off)\n", displayRotation);
  }
  else if (IS("autorot")) {
    autoRotate = !strcmp(arg, "on");
    Serial.println(autoRotate ? "following gravity" : "rotation frozen");
  }
  else if (IS("battery")) {
    float volts = batteryVoltsAverage();
    Serial.printf("%.2f V  %d%%, running from %s  (offsets bat +%.3f usb -%.3f)\n", volts, batteryPercent(),
                  onUsb() ? "USB" : "battery", gauge.battery_offset_volts, gauge.usb_offset_volts);
  }
  else if (IS("off"))      powerOff();
  else if (*line)          Serial.printf("no such command: %s (try help)\n", line);
  #undef IS
}

// ----------------------------------------------------------------- setup

// Boot handshake: one rising edge on READY = "OLED registers loaded", so the
// CH32 turns on the panel's VBAT (never VCC before VDD). It acks on MIRROR.
// Works after a reflash too (the CH32 acks a lone edge while ON). Never
// pulse twice here: two edges within 150 ms means "cut power".
void bootHandshake() {
  Serial.println("telling the CH32 the OLED is ready");
  digitalWrite(PIN_READY, HIGH);

  uint32_t startMs = millis();
  while (!digitalRead(PIN_MIRROR) && millis() - startMs < 3000) delay(1);
  if (digitalRead(PIN_MIRROR)) {
    Serial.println("got the ack, VBAT is up");
    while (digitalRead(PIN_MIRROR) && millis() - startMs < 5000) delay(1);
  } else {
    Serial.println("no ack in 3s -- carrying on, but the panel may be dark");
  }

  digitalWrite(PIN_READY, LOW);   // release: the next edge means shutdown
  mirrorLinePrevious = false;
  delay(50);
}

void setup() {
  Serial.begin(115200);
  uint32_t startMs = millis();
  while (!Serial && millis() - startMs < 3000) delay(10);

  pinMode(PIN_STATUS_LED, OUTPUT);
  pinMode(PIN_MIRROR, INPUT);
  pinMode(PIN_READY, OUTPUT);
  digitalWrite(PIN_READY, LOW);
  pinMode(PIN_WHEEL_SWITCH, INPUT_PULLUP);   // the only input without a board pull-up
  pinMode(PIN_WHEEL_A, INPUT);
  pinMode(PIN_WHEEL_B, INPUT);
  pinMode(PIN_USB_POWER, INPUT);
  analogReadResolution(12);
  gauge_init(&gauge);
  orient_init(&orientation, 0);
  EEPROM.begin(2048);
  gaugeLoad();
  ampsLoad();
  wheelSettingsLoad();
  prefsLoad();
  pico_get_unique_board_id_string(boardID, sizeof(boardID));
  if (!LittleFS.begin()) { Serial.println("LittleFS mount failed - formatting"); LittleFS.format(); LittleFS.begin(); }
  hidBegin();
  mscBegin();
  mscProgress = csvProgress;

  Serial.println("\nLomenett (V3.6 board)");

  // A bus line held low turns every Wire call into a timeout: disable I2C
  // so the console stays responsive.
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  delay(2);
  i2cBusStuck = !digitalRead(PIN_SDA) || !digitalRead(PIN_SCL);
  if (i2cBusStuck) Serial.println("!! I2C bus stuck low - OLED/accel disabled, console only");

  restartI2c();
  Wire.setClock(400000);                 // 60 fps needs ~13 ms frames

  // Release the panel from reset before probing (a floating RES can't ACK).
  pinMode(PIN_OLED_RESET, OUTPUT);
  digitalWrite(PIN_OLED_RESET, LOW);
  delayMicroseconds(20);
  digitalWrite(PIN_OLED_RESET, HIGH);
  delay(10);

  // The panel can take a couple of seconds after reset to ACK.
  for (int i = 0; i < 10 && !oledReady && !i2cBusStuck; i++) {
    Wire.beginTransmission(OLED_I2C_ADDRESS);
    oledReady = (Wire.endTransmission() == 0);
    if (!oledReady) delay(300);
  }
  Serial.println(oledReady ? "OLED found" : "OLED did not answer at 0x3C");
#ifdef LOMENETT_BRIDGE
  // No panel: run the UI on the framebuffer alone for the bridge viewer.
  if (!oledReady && !i2cBusStuck) {
    oledReady = oledVirtual = true;
    Serial.println("drawing to a virtual panel - view it with the bridge viewer");
  }
#endif
  if (oledReady) oledInit();
#if REMOTE_PANEL
  if (!oledReady) oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS, false, false);   // headless framebuffer
#endif

  // Restore the rotation from power-off and seed the gravity filter with it.
  int savedRotation = rotationLoad();
  if (savedRotation >= 0) {
    displayRotation = savedRotation; oled.setRotation(savedRotation);
    orient_init(&orientation, rotationToSector(savedRotation));
    Serial.printf("restored rotation %d\n", savedRotation);
  }

  accelReady = i2cBusStuck ? false : accelBegin();
  Serial.println(accelReady ? "accelerometer found" : "accelerometer did not answer at 0x18");

  // Orient to gravity before the first pixel shows (flat keeps the saved rotation).
  if (accelReady) {
    delay(60);                                   // first samples at 100 Hz
    long sumX = 0, sumY = 0, sumZ = 0;
    for (int i = 0; i < 8; i++) { accelRead(accelX, accelY, accelZ); sumX += accelX; sumY += accelY; sumZ += accelZ; delay(10); }
    int sector = orient_sector_from_accel((int)(sumX / 8), (int)(sumY / 8), rotationToSector(displayRotation));
    displayRotation = sectorToRotation(sector); oled.setRotation(displayRotation);
    orient_init(&orientation, sector);
    Serial.printf("boot orientation from gravity: sector %d -> rotation %d\n", sector, displayRotation);
  }

  bootHandshake();

  if (oledReady) {
    oledPowerOn();
    splash.startedAt = millis(); splash.done = false;
    ui.introStartedAt = 0;                       // home intro starts when the splash ends
  }

  // Nearly flat cell on battery: sad battery, power off. Wait for the sense
  // node to settle first (it reads low right after power-on).
  if (!onUsb()) {
    float volts = batteryVoltsSettled();
    if (volts < BATTERY_BOOT_MIN_V) {
      Serial.printf("battery too low to start (%.2f V)\n", volts);
      drawSadBattery(volts);
      delay(2500);
      powerOff();
    }
  }

  wheelAbPrevious = (digitalRead(PIN_WHEEL_A) << 1) | digitalRead(PIN_WHEEL_B);
  add_repeating_timer_us(-200, wheelTimerCallback, nullptr, &wheelTimer);   // every 200 us
  cmdHelp();
  Serial.print("> ");
}

// ------------------------------------------------------------------ loop
// The wheel is polled from a 5 kHz timer interrupt on core 0 (not core 1:
// pausing core 1 for flash writes could deadlock in arduino-pico 6.0.0).

// Actions the UI raised during uiFrame (before EDIT-mode transitions).
void handleUiActions(uint32_t now) {
  if (ui.changeAction != CHANGE_NONE) {          // password-change screen
    int action = ui.changeAction, index = ui.changeEntryIndex;
    ui.changeAction = CHANGE_NONE;
    if (index >= 0 && index < vault.count) {
      switch (action) {
        case CHANGE_GENERATE_SILENT: generatePassword(newPasswordCandidate, 16); ui.changeHasCandidate = true; break;
        case CHANGE_TYPE_OLD: typeStart(vault.entries[index].secret); break;
        case CHANGE_TYPE_NEW:
          if (!ui.changeHasCandidate) { generatePassword(newPasswordCandidate, 16); ui.changeHasCandidate = true; }
          typeStart(newPasswordCandidate);
          break;
        case CHANGE_TYPE_AGAIN: if (ui.changeHasCandidate) typeStart(newPasswordCandidate); break;
        case CHANGE_SAVE:
          if (ui.changeHasCandidate) {
            vaultSetPw(index, newPasswordCandidate);
            memset(newPasswordCandidate, 0, sizeof(newPasswordCandidate));
            ui.changeHasCandidate = false; ui.changeEntryIndex = -1;
            uiHome(ui); uiToast(ui, "CHANGED", now + 1000, UI_HOME);
            Serial.println("password changed & saved");
          }
          break;
        case CHANGE_REGENERATE:
          generatePassword(newPasswordCandidate, 16); ui.changeHasCandidate = true;
          typeStart(newPasswordCandidate);
          break;
      }
    }
  }
}

void handleUiActionsAfterFrame(uint32_t now) {
  if (ui.pendingAction == ACT_NEWPW) {
    ui.pendingAction = -1;
    generatePassword(newPasswordCandidate, 16);
    bool created = vaultAdd(ui.newLabel, newPasswordCandidate);
    memset(newPasswordCandidate, 0, sizeof(newPasswordCandidate));
    uiHome(ui); uiToast(ui, created ? "CREATED" : "FULL/DUP", now + 1100, UI_HOME);
    Serial.printf("new entry '%s': %s\n", ui.newLabel, created ? "created" : "failed");
    ui.newLabelLength = 0; ui.newLabel[0] = 0;
  }
  if (ui.pendingAction == ACT_NEWCARD) {
    ui.pendingAction = -1;
    char expiry[6]; snprintf(expiry, sizeof(expiry), "%.2s/%.2s", ui.newCardExpiry, ui.newCardExpiry + 2);
    bool created = vaultAddCard(ui.newLabel, ui.newCardNumber, expiry, ui.newCardCvv);
    memset(ui.newCardNumber, 0, sizeof(ui.newCardNumber)); memset(ui.newCardCvv, 0, sizeof(ui.newCardCvv));
    uiHome(ui); uiToast(ui, created ? "CREATED" : "FULL/DUP", now + 1100, UI_HOME);
    Serial.printf("new card '%s': %s\n", ui.newLabel, created ? "created" : "failed");
    ui.newLabelLength = 0; ui.newLabel[0] = 0;
  }
  if (ui.pendingAction == ACT_ENTRYSET) { ui.pendingAction = -1; vaultRewriteFromEntries(); Serial.println("entry settings saved"); }
  if (ui.pendingAction == ACT_DELETE) {
    ui.pendingAction = -1;
    int index = ui.entryIndex;
    int kind = index >= 0 && index < vault.count ? vault.entries[index].kind : KIND_PW;
    if (vaultDel(index)) Serial.printf("entry %d deleted\n", index);
    ui.entryIndex = -1; ui.cardIndex = -1;
    uiBackTo(ui, kind == KIND_CARD ? UI_CARDS : kind == KIND_TOTP ? UI_CODES : UI_VAULT);
  }
  if (ui.pendingAction == ACT_INFO) { ui.pendingAction = -1; uiFillInfo(); }
  if (ui.pendingAction == ACT_TOTPBACKUP) {
    ui.pendingAction = -1;
    if (typeTotpBackup(ui.entryIndex, ui.backupCodeIndex))
      Serial.printf("typing backup code %d of %s\n", ui.backupCodeIndex + 1, vault.entries[ui.entryIndex].name);
  }
  if (ui.pendingAction == ACT_CLOCKSYNC) {
    ui.pendingAction = -1;
    typeClockSync(ui.clockSyncOs);
    Prefs saved; EEPROM.get(EEPROM_PREFS_ADDR, saved);           // remember the OS picked
    if (saved.magic != PREFS_MAGIC || saved.clockSyncOsPlusOne != ui.clockSyncOs + 1) prefsSave();
    Serial.printf("typing clock sync command (%s)\n", ui.clockSyncOs == OS_LINUX ? "Linux" : ui.clockSyncOs == OS_MAC ? "Mac" : "Windows");
  }
  if (ui.pendingAction == ACT_TOTPTYPE) {
    ui.pendingAction = -1;
    if (typeTotp(ui.entryIndex)) Serial.printf("typing 2FA code of %s\n", vault.entries[ui.entryIndex].name);
  }
  if (ui.pendingAction == ACT_AUTOWIPE) {
    ui.pendingAction = -1;
    if (!ui.autoWipe && !ui.haveBackup) {
      uiToast(ui, "BACKUP 1ST", now + 1300, UI_SETTINGS);
    } else {
      ui.autoWipe = !ui.autoWipe; prefsSave();
      Serial.printf("auto-wipe %s\n", ui.autoWipe ? "on" : "off");
    }
  }
  if (ui.pendingAction == ACT_RESTORE) { ui.pendingAction = -1; mscMode = MSC_MODE_RESTORE; }   // next mscEnter builds an empty drive
  if (ui.pendingAction >= ACT_CARDNUM && ui.pendingAction <= ACT_CARDCVV && ui.cardIndex >= 0 && ui.cardIndex < vault.count) {
    int action = ui.pendingAction; ui.pendingAction = -1;
    VaultEntry &card = vault.entries[ui.cardIndex];
    if (action == ACT_CARDNUM) typeStart(card.secret);
    else if (action == ACT_CARDEXP) typeStart(card.expiry);
    else typeStart(card.cvv);
  }
}

// EDIT mode: mount the drive on entry, commit edits after writes settle or
// on eject, and handle backup/restore drives on exit.
void handleEditMode(uint32_t now) {
  static bool wasInEditMode = false;
  bool inEditMode = (ui.screen == UI_EDITMODE);
  if (inEditMode && !wasInEditMode) {
    mscEnter(); ui.editState = EDIT_IDLE;
    Log.printf("EDIT: drive mounted (mode %d)\n", mscMode);
  }
  if (!inEditMode && wasInEditMode) {
    if (mscMode == MSC_MODE_RESTORE) {                           // capture the dropped file, then ask its PIN
      bool captured = mscCaptureBackup(); mscLeave(); mscMode = MSC_MODE_EDIT;
      if (captured) {
        ui.screen = UI_PIN; ui.pinMode = PIN_RESTORE; ui.pinSlot = 0;
        Serial.println("restore: file captured, asking PIN");
      } else {
        Serial.println("restore: no BACKUP.LMB found");
      }
    } else if (mscMode == MSC_MODE_BACKUP) {
      mscLeave(); mscMode = MSC_MODE_EDIT; mscBackupText = "";
      Log.println("backup: drive removed");
    } else {
      if (mscLastWrite) {
        mscLastWrite = 0;
        int imported;
        bool saved = mscCommitEdit(&imported);
        if (imported >= 0) {
          snprintf(importToastText, sizeof(importToastText), "IMP %d SKIP %d", imported, mscCsvSkipped);
          uiHome(ui); uiToast(ui, importToastText, now + 1600, UI_HOME);
        }
        Log.printf("EDIT leave: %s\n", imported >= 0 ? "CSV import" : saved ? "saved" : "no change");
      }
      mscLeave(); Log.println("EDIT: drive removed");
    }
  }
  wasInEditMode = inEditMode;
  if (!inEditMode) return;

  bool writesSettled = mscLastWrite && now - mscLastWrite > 1000;
  if (writesSettled || (mscEjected && mscLastWrite)) {           // eject also means the host flushed
    mscLastWrite = 0;
    int imported;
    saveStageHook = saveStageShow;                               // progress on screen while committing
    bool saved = mscCommitEdit(&imported);
    if (imported > 0) {                                          // remount: CSV gone, SECRETS.TXT lists the new entries
      saveStageShow("building drive"); mscBuildImage();
      saveStageShow("reconnecting USB"); mscEnterBuilt();
    }
    ui.editState = (imported >= 0 || saved) ? EDIT_SAVED : EDIT_BAD;
    ui.editStateSince = now;
    if (imported >= 0) saveStageShow("serial log 2");
    Log.printf("EDIT: %s (%d entries)\n", imported >= 0 ? "CSV import" : saved ? "saved" : "bad file", vault.count);
    saveStageHook = nullptr;
  } else if (mscLastWrite) {
    ui.editState = EDIT_WRITING;
  } else if (mscEjected) {
    ui.editState = EDIT_EJECTED;
  } else if (ui.editState == EDIT_SAVED && now - ui.editStateSince > 2500) {
    ui.editState = EDIT_IDLE;
  }
}

// Actions handled after EDIT transitions and typing requests.
void handleLateUiActions() {
  if (ui.pendingAction == ACT_OFF) { ui.pendingAction = -1; powerOff(); }
  if (ui.pendingAction == ACT_PREFS) { prefsSave(); Serial.printf("prefs saved (auto-off %ds)\n", ui.autoOffSeconds); }
  // WHEEL DIR: pick mask 0x06 or 0x09 (each rotation opposite to the one
  // 180 degrees from it), whichever flips the direction in the current rotation.
  if (ui.pendingAction == ACT_WHEELDIR) {
    bool invertNow = !(wheelFlipMask & (1 << displayRotation));
    wheelFlipMask = (((0x06 >> displayRotation) & 1) == invertNow) ? 0x06 : 0x09;
    wheelSettingsSave();
    Serial.printf("wheel direction flipped (mask %d)\n", wheelFlipMask);
  }
  if (ui.pendingAction == ACT_RESET) {
    LittleFS.remove("/vault.bin"); LittleFS.remove("/lock.bin");
    failReset(); lockSession(); vaultForget(); seKekDelete();
    ui.locked = true; ui.navDepth = 0; ui.screen = UI_PIN; ui.pinMode = PIN_NEW; ui.pinSlot = 0;
    Serial.println("factory reset: vault wiped, set a new PIN");
  }
  ui.pendingAction = -1;
}

// Both buttons act as select: a short press clicks on release, a 600 ms hold
// is "back" (fires once; its release is not a click). The wheel switch
// chatters, so a new level must hold 25 ms.
void readButtons(uint32_t now, bool &click, bool &back, bool &buttonDown) {
  static bool debouncedDown = false, rawPrevious = false, wasDown = false, backFired = false;
  static uint32_t rawChangedAt = 0, pressedAt = 0;
  bool raw = wheelPressed() || selectHeld() || rcDown(now);
  if (raw != rawPrevious) { rawPrevious = raw; rawChangedAt = now; }
  else if (raw != debouncedDown && now - rawChangedAt >= 25) debouncedDown = raw;
  click = false; back = false;
  if (debouncedDown && !wasDown) { pressedAt = now; backFired = false; }
  if (debouncedDown && !backFired && now - pressedAt >= 600) { back = true; backFired = true; }
  if (!debouncedDown && wasDown && !backFired) click = true;
  wasDown = debouncedDown;
  buttonDown = debouncedDown;
}

// One frame of the carousel UI.
void runCarouselUi(uint32_t now) {
  if (!splash.done) {
    if (!splashFrame(oled, splash, now)) {       // splash over: PIN gate (or first-boot set PIN)
      ui.introStartedAt = millis(); ui.screen = UI_PIN;
      ui.pinMode = vaultExists() ? PIN_UNLOCK : PIN_NEW;
      ui.pinSlot = 0; ui.locked = true;
    }
    return;
  }
  static long wheelDetentsLastFrame = 0;
  int wheelDelta = (int)(wheelDetents - wheelDetentsLastFrame);
  wheelDetentsLastFrame = wheelDetents;
  if (wheelFlipMask & (1 << displayRotation)) wheelDelta = -wheelDelta;
  wheelDelta += rcTakeWheel();                   // remote panel (0 in normal builds)

  bool click, back, buttonDown;
  readButtons(now, click, back, buttonDown);

  // Auto-off on battery after autoOffSeconds without input (not while the
  // drive is mounted or typing). Unplugging counts as input.
  static uint32_t lastInputMs = 0; static bool wasOnUsb = true;
  bool usbNow = onUsb();
  if (!lastInputMs || wheelDelta || buttonDown || usbNow != wasOnUsb) lastInputMs = now;
  wasOnUsb = usbNow;
  if (ui.autoOffSeconds && !usbNow && ui.screen != UI_EDITMODE && !typeText && now - lastInputMs > (uint32_t)ui.autoOffSeconds * 1000UL) {
    Serial.printf("auto-off: idle %ds\n", ui.autoOffSeconds);
    powerOff();
  }

  handleUiActions(now);
  uiFrame(oled, ui, wheelDelta, click, back, batteryPercent(), onUsb(), gaugeOrRawVolts(), now);
  handleUiActionsAfterFrame(now);

  if (rotationAnimationPending) {
    rotationAnimationPending = false;
    if (rotationDeltaDegrees == 180) animateFlip(oled, rotationOldFrame, oled.getBuffer(), 0);
    else animateRotate(oled, rotationOldFrame, oled.getBuffer(), rotationDeltaDegrees);
  }
  handleEditMode(now);
  if (ui.pinSubmit) { ui.pinSubmit = false; pinHandle(String(ui.pinTyped), now); }
  if (ui.typeEntryRequest >= 0) {
    int index = ui.typeEntryRequest; ui.typeEntryRequest = -1;
    if (index < vault.count) {
      Serial.printf("typing entry %d (%s) mode %d\n", index, vault.entries[index].name, ui.typeMode);
      typeStartEntry(index, ui.typeMode);
      if (ui.typeForceEnter) typePressEnter = true;
    }
    ui.typeForceEnter = false;
  }
  handleLateUiActions();
}

// Fallback status screen when the carousel UI is off ("ui off").
void drawStatusScreen() {
  float volts = gaugeOrRawVolts();
  if (displayRotation & 1) {                     // portrait: one fact per row
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    char currentText[8];
    float currentMa = currentEstimateMa();
    if (isnan(currentMa)) strcpy(currentText, "--mA"); else snprintf(currentText, sizeof(currentText), "%.0fmA", currentMa);
    oled.printf("up\n%lus\n%.2fV\n%d%%\n%s\n%s\nwheel\n%ld%s\nsel %u\n",
                millis() / 1000, volts, batteryPercent(), onUsb() ? "USB" : "BAT", currentText,
                wheelDetents, wheelPressed() ? "*" : "", selectPressCount);
    if (accelReady) oled.printf("x%+.1f\ny%+.1f\nz%+.1f\n", accelX / 1000.0f, accelY / 1000.0f, accelZ / 1000.0f);
    else oled.print("no\naccel");
    oled.display();
  } else {
    char line1[24], line2[24], line3[24];
    snprintf(line1, sizeof(line1), "up %lus  wheel %ld%s", millis() / 1000, wheelDetents, wheelPressed() ? " *" : "");
    float currentMa = currentEstimateMa();
    if (isnan(currentMa)) snprintf(line2, sizeof(line2), "sel %u %.2fV %d%% %s", selectPressCount, volts, batteryPercent(), onUsb() ? "USB" : "BAT");
    else                  snprintf(line2, sizeof(line2), "%.2fV %d%% ~%.0fmA", volts, batteryPercent(), currentMa);
    snprintf(line3, sizeof(line3), "%d %d %d", accelX, accelY, accelZ);
    oledLines(line1, line2, accelReady ? line3 : "no accel");
  }
}

void loop() {
  gaugePoll();
  accelPoll();
  rotationPoll();
  typeTick();
  traceTick();
  ampsPoll();
  bridgePoll();

  // LED: lit while either button is held, or forced from the console.
  digitalWrite(PIN_STATUS_LED, (ledForcedOn || wheelPressed() || selectHeld()) ? HIGH : LOW);

  // Five select presses within 2 s power off.
  if (selectPressedEdge()) {
    selectPressCount++;
    uint32_t now = millis();
    Serial.printf("select press %u\n", selectPressCount);
    if (press_gesture_detected(shutdownPressTimes, &shutdownPressCount, SHUTDOWN_PRESSES, SHUTDOWN_WINDOW_MS, now)) powerOff();
  }

  // Serial console, one line at a time (echoed).
  static char commandLine[256];
  static uint8_t commandLength = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      commandLine[commandLength] = 0;
      Serial.println();
      runCommand(commandLine);
      commandLength = 0;
      Serial.print("> ");
    } else if (commandLength < sizeof(commandLine) - 1) {
      commandLine[commandLength++] = ch;
      Serial.print(ch);
    }
  }

  if (accelStreaming) {
    static uint32_t lastStreamMs = 0;
    if (millis() - lastStreamMs > 200) {
      lastStreamMs = millis();
      int x, y, z; accelRead(x, y, z);
      Serial.printf("x %5d  y %5d  z %5d\n", x, y, z);
    }
  }

  // No panel yet (absent, hot-plugged, flaky): probe once a second.
  static uint32_t lastProbeMs = 0;
  if ((!oledReady || oledVirtual) && !i2cBusStuck && millis() - lastProbeMs > 1000) {
    lastProbeMs = millis();
    Wire.beginTransmission(OLED_I2C_ADDRESS);
    if (Wire.endTransmission() == 0) {
      oledReady = true;
      oledVirtual = false;
      oledInit();
      oledPowerOn();
      Serial.println("OLED appeared - claimed and initialized");
    }
  }

  // On battery below BATTERY_RUN_MIN_V for 3 checks (15 s): sad battery, off.
  static uint32_t lastBatteryCheckMs = 0;
  static uint8_t lowBatteryChecks = 0;
  if (millis() - lastBatteryCheckMs > 5000) {
    lastBatteryCheckMs = millis();
    if (!onUsb() && batteryVoltsAverage() < BATTERY_RUN_MIN_V) {
      if (++lowBatteryChecks >= 3) {
        Serial.println("battery exhausted, shutting down");
        drawSadBattery(batteryVoltsAverage());
        delay(1500);
        powerOff();
      }
    } else {
      lowBatteryChecks = 0;
    }
  }

  static uint32_t lastStatusDrawMs = 0;
  if (carouselUiEnabled && (oledReady || (REMOTE_PANEL && oled.getBuffer())) && !i2cBusStuck) {
    runCarouselUi(millis());
  } else if (statusScreenEnabled && oledReady && !i2cBusStuck && millis() - lastStatusDrawMs > 250) {
    lastStatusDrawMs = millis();
    drawStatusScreen();
  }
  rcMirror(oled.getBuffer(), displayRotation);     // remote panel (no-op in normal builds)
}
