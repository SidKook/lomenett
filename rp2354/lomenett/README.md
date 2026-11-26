# Lomenett firmware (RP2354A side, V3.6 board)

The RP2354A runs the UI; the CH32V003 handles power and the select button (`../../ch32_supervisor/`).

## Console

Serial at 115200, type `help`. Common commands:

```
status   everything at once        accel    read accelerometer
stream   live accelerometer        wheel    wheel position/switch
zero     reset wheel               buttons  select press count
scan     I2C scan                  oled hi  write to the display
screen   toggle status screen      led on   onboard LED
battery  voltage and source        test     check every peripheral
time     set clock (2FA)           totp     list 2FA codes
off      power down
```

`test` checks display and accelerometer, then asks you to turn the wheel and press the button.

## Hardware

- Wheel: GP13/GP14 (board pull-ups + 10 nF), switch on GP12 (internal pull-up).
- I2C: LIS3DH accelerometer 0x18, OLED (SSD1306-compatible) 0x3C, SE050 0x48.
- Select button goes to the CH32, which mirrors it as a level on GP11.

## RP <-> CH32

- GP18 -> PC1: one rising edge at boot = "OLED ready, turn panel supply on"; two edges within 150 ms = "shutdown done, cut power". Edge-triggered, so a stuck-high line is harmless.
- GP11 <- PA1: button level, plus a 100 ms ack when the panel supply is up.
- The panel needs logic power before display power and the reverse at off (getting this wrong killed earlier OLEDs).
- Five quick presses = off. Holding 8 s cuts power even if the RP hangs.
- RT_EN bridge (for picorvd flashing) is auto-detected; bridged, `powerOff()` waits and reboots on the next five-press power-on.

## Build

Board "Generic RP2350", USB stack "Adafruit TinyUSB", flash "2MB (Sketch: 1MB, FS: 1MB)" (the vault is in LittleFS). Libraries: Adafruit GFX, Adafruit SSD1306, AESLib, Crypto (rweather).

```
arduino-cli compile --fqbn 'rp2040:rp2040:generic_rp2350:usbstack=tinyusb,flash=2097152_1048576' .
```

Tests: `cd sim && make && ./sim` (wheel, gesture, gauge, orientation); `sim/build.sh` (vault, 2FA, UI screens).

## 2FA

**2FA** on the home screen shows each entry's live code; click types it, hold goes back. Wheel pages: BACKUP CODES (if any; click types one), SETTINGS (icon, Enter, favourite, delete), BACK. Standard RFC 6238 (SHA1, 6-8 digits, any period).

Adding codes:
- EDIT mode `2FA.TXT`: `Label <TAB> Secret [<TAB> Digits <TAB> Period <TAB> Flags <TAB> Backup1..5]`. Secret = the text key under the QR code (spaces/lower case fine) or a whole `otpauth://totp/...` link. Stored secrets and backups show as `********`: leave them to keep, clear a backup column once used, delete the line to remove. Flag `E` = press Enter after.
- Console: `totp add <name> <secret | otpauth:// link>`.

A 2FA entry can share a label with a password entry. Backups include 2FA secrets.

**Clock**: no RTC, so the time must be set after each power-on (until then 2FA shows NO CLOCK). Options:
- On device: opening 2FA without the time asks SYNC CLOCK?; pick LINUX/MAC/WINDOWS once (remembered) and it types a command that sends the time back over serial. Linux/Mac need a terminal focused; Windows opens Run itself. Commands are `CLOCK_SYNC_COMMAND` in lomenett.ino (US layout, port must be free).
- `clock_sync.html` in Chrome/Edge: connect once; re-syncs on every plug-in while open.
- `echo "time $(date +%s)" > /dev/ttyACM0`, or `time <unix>` on the console.

Drift is a few seconds a day. Storing 2FA next to passwords makes them one factor, not two.

## Browser login

The extension in `../../lomenett_extension/` sends `site <host> [both|user|pw]`; the device opens LOG IN with matching entries and a click types the login + Enter. `site off` closes it. Reply: `site: <n> match`, `none`, `locked` or `busy`; nothing secret goes over serial. Entries store their website in SECRETS.TXT's 6th column (`Site`), filled from the CSV `url` on import.
