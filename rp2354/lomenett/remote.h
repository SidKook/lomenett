// remote.h -- remote panel for boards with a dead screen (build with
// -DREMOTE_PANEL=1). Streams the framebuffer over USB serial and accepts
// wheel/button commands, for remote_panel.html. Off by default: a host that
// can see the screen and press buttons could watch the PIN go in.
//
// Device -> host, one line per changed frame (max ~60/s):
//   #F<rotation 0-3><1024 hex chars of the 512-byte SSD1306 buffer>
// Host -> device (console lines):
//   mirror on|off   start/stop streaming (also stops when the port closes)
//   rc l | rc r     wheel back / forward one step
//   rc c            click          rc b   back (600 ms hold)
//   rc d | rc u     press / release
#pragma once
#ifndef REMOTE_PANEL
#define REMOTE_PANEL 0
#endif

#if REMOTE_PANEL
static bool     remoteMirrorOn = false;
static int      remoteWheelSteps = 0;          // pending steps from the host
static bool     remoteButtonHeld = false;      // rc d / rc u
static uint32_t remotePressedUntil = 0;        // rc c / rc b

static inline int rcTakeWheel() { int steps = remoteWheelSteps; remoteWheelSteps = 0; return steps; }
static inline bool rcDown(uint32_t now) { return remoteButtonHeld || (int32_t)(remotePressedUntil - now) > 0; }

static void rcCommand(const char *arg) {
  uint32_t now = millis();
  switch (arg[0]) {
    case 'l': remoteWheelSteps--; break;
    case 'r': remoteWheelSteps++; break;
    case 'c': remotePressedUntil = now + 60; break;     // short press, then release = click
    case 'b': remotePressedUntil = now + 700; break;    // past the 600 ms back threshold
    case 'd': remoteButtonHeld = true; break;
    case 'u': remoteButtonHeld = false; break;
    default: Serial.println("rc l|r|c|b|d|u"); break;
  }
}

static void rcMirrorCommand(const char *arg) {
  remoteMirrorOn = strcmp(arg, "off") != 0;
  Serial.printf("mirror %s\n", remoteMirrorOn ? "on" : "off");
}

// Send the frame if it changed, or once a second so a new viewer fills in.
static void rcMirror(const uint8_t *buffer, int rotation) {
  if (!remoteMirrorOn || !buffer) return;
  if (!Serial.dtr()) { remoteMirrorOn = false; return; }   // viewer closed: never block
  static uint32_t lastHash = 0, lastCheckAt = 0, lastSentAt = 0;
  uint32_t now = millis();
  if (now - lastCheckAt < 15) return;
  uint32_t hash = 2166136261u;                             // FNV-1a over buffer + rotation
  for (int i = 0; i < 512; i++) hash = (hash ^ buffer[i]) * 16777619u;
  hash ^= rotation;
  if (hash == lastHash && now - lastSentAt < 1000) return;
  lastCheckAt = now; lastHash = hash; lastSentAt = now;
  static char line[4 + 1024 + 1];
  static const char HEX_DIGITS[] = "0123456789abcdef";
  line[0] = '\n'; line[1] = '#'; line[2] = 'F'; line[3] = '0' + (rotation & 3);
  for (int i = 0; i < 512; i++) {
    line[4 + 2 * i] = HEX_DIGITS[buffer[i] >> 4];
    line[5 + 2 * i] = HEX_DIGITS[buffer[i] & 15];
  }
  line[4 + 1024] = '\n';
  Serial.write((const uint8_t *)line, sizeof(line));
}
#else
static inline int rcTakeWheel() { return 0; }
static inline bool rcDown(uint32_t) { return false; }
static inline void rcMirror(const uint8_t *, int) {}
#endif
