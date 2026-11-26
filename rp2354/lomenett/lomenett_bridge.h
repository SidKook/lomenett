// lomenett_bridge.h -- streams the OLED framebuffer over USB serial so the PC
// viewer in sim/bridge/ can show the screen live. Non-blocking: bridgePoll()
// writes only what fits in the USB FIFO each loop. Off until the console gets
// "bridge on" (the viewer sends it); turns off when the port closes.
// Usage: close the Serial Monitor, then `make -C sim/bridge run`.
// Everything on screen goes over the cable while on; remove the #define below
// to compile it out.

#define LOMENETT_BRIDGE

#ifdef LOMENETT_BRIDGE

// Frame format (must match sim/bridge/bridge_viewer.cpp):
//   A5 5A 'L' 'M'   magic
//   01              type: full framebuffer
//   rr              rotation 0-3
//   512 bytes       SSD1306 buffer
//   s1 s2           Fletcher-16 over type, rotation and buffer
// Console text may interleave; the viewer drops frames that fail the checksum.

namespace lomenettBridge {

const uint8_t  FRAME_TYPE_FRAMEBUFFER = 1;
const size_t   FRAMEBUFFER_BYTES = SCREEN_W * SCREEN_H / 8;
const size_t   FRAME_BYTES       = 4 + 2 + FRAMEBUFFER_BYTES + 2;
const uint32_t MIN_FRAME_GAP_MS  = 33;     // ~30 fps cap
const uint32_t KEEPALIVE_MS      = 1000;   // resend an unchanged screen

bool     enabled = false;
uint8_t  frame[FRAME_BYTES];
size_t   bytesSent = FRAME_BYTES;          // == FRAME_BYTES: nothing in flight
uint8_t  lastFramebuffer[FRAMEBUFFER_BYTES];
uint8_t  lastRotation = 0xFF;
uint32_t lastSendMs = 0;
uint32_t framesBuilt = 0, framesSent = 0;
const char* idleReason = "never polled";   // why the last poll sent nothing

void buildFrame(const uint8_t* framebuffer, uint8_t rotation) {
  frame[0] = 0xA5; frame[1] = 0x5A; frame[2] = 'L'; frame[3] = 'M';
  frame[4] = FRAME_TYPE_FRAMEBUFFER;
  frame[5] = rotation;
  memcpy(frame + 6, framebuffer, FRAMEBUFFER_BYTES);

  uint16_t sum1 = 0, sum2 = 0;
  for (size_t i = 4; i < 6 + FRAMEBUFFER_BYTES; i++) {
    sum1 = (sum1 + frame[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  frame[6 + FRAMEBUFFER_BYTES]     = (uint8_t)sum1;
  frame[6 + FRAMEBUFFER_BYTES + 1] = (uint8_t)sum2;
  bytesSent = 0;
}

}  // namespace lomenettBridge

void bridgeSet(bool enable) {
  using namespace lomenettBridge;
  enabled = enable;
  bytesSent = FRAME_BYTES;
  lastRotation = 0xFF;   // send the current screen right away
}

// Call from loop(); returns immediately when idle or the FIFO is full.
void bridgePoll() {
  using namespace lomenettBridge;
  if (!enabled) { idleReason = "off"; return; }
  if (!Serial) { bridgeSet(false); idleReason = "port closed (no DTR)"; return; }

  if (bytesSent == FRAME_BYTES) {
    uint32_t now = millis();
    if (now - lastSendMs < MIN_FRAME_GAP_MS) return;
    const uint8_t* framebuffer = oled.getBuffer();
    if (!framebuffer || !oledReady) { idleReason = framebuffer ? "oled not ok" : "no framebuffer"; return; }
    uint8_t rotation = oled.getRotation();
    bool changed = rotation != lastRotation || memcmp(framebuffer, lastFramebuffer, FRAMEBUFFER_BYTES) != 0;
    if (!changed && now - lastSendMs < KEEPALIVE_MS) return;
    buildFrame(framebuffer, rotation);
    framesBuilt++;
    memcpy(lastFramebuffer, framebuffer, FRAMEBUFFER_BYTES);
    lastRotation = rotation;
    lastSendMs = now;
  }

  int room = Serial.availableForWrite();
  if (room <= 0) { idleReason = "usb fifo full"; return; }
  size_t chunk = min((size_t)room, FRAME_BYTES - bytesSent);
  bytesSent += Serial.write(frame + bytesSent, chunk);
  Serial.flush();
  if (bytesSent == FRAME_BYTES) framesSent++;
  idleReason = "sending";
}

// "bridge" with no argument: diagnostics for when no frames show up.
void bridgeStatus() {
  using namespace lomenettBridge;
  Serial.printf("bridge %s  port %s  oled %s  buffer %s  fifo %d free\n",
                enabled ? "on" : "off", Serial ? "open" : "closed", oledVirtual ? "virtual" : oledReady ? "ok" : "NOT ok",
                oled.getBuffer() ? "yes" : "NULL", Serial.availableForWrite());
  Serial.printf("frames built %lu  sent %lu  last poll: %s\n",
                (unsigned long)framesBuilt, (unsigned long)framesSent, idleReason);
}

#else

void bridgeSet(bool) { Serial.println("bridge not compiled in (LOMENETT_BRIDGE)"); }
void bridgeStatus() { bridgeSet(false); }
void bridgePoll() {}

#endif  // LOMENETT_BRIDGE
