// test_cardsave.cpp -- regression: a card added to CARDS.TXT is saved as a
// card and the existing password entry survives.
#include "Arduino.h"
#include "Adafruit_GFX.h"
#include "Adafruit_SSD1306.h"
#include "Wire.h"
#include "LittleFS.h"
#include "Adafruit_TinyUSB.h"
#include "lomenett_logic.h"
#include "ui.h"
#include "msc.h"
#include <cstdio>

SerialT Serial; TwoWire Wire; FS LittleFS; uint32_t g_sim_millis = 1000;
static bool t1LinkReset() { return false; }
static int t1SendApdu(const uint8_t *, int, uint8_t *, int) { return -1; }
class NullLog : public Print {
public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t length) override { return length; }
};
static NullLog nullLog; Print &LogRef = nullLog;
extern bool sessionKeyReady;

static int findEntry(const char *name) {
  for (int i = 0; i < vault.count; i++) if (!strcmp(vault.entries[i].name, name)) return i;
  return -1;
}

// Overwrite an existing 8.3 file in the RAM disk, as a host edit would.
static void hostWrite(const char *name83, const String &text) {
  uint8_t *root = mscDisk[2];
  int slot = -1;
  for (int i = 0; i < 16; i++) if (!memcmp(&root[i * 32], name83, 11)) { slot = i; break; }
  if (slot < 0) { printf("    !! file %s not found in dir\n", name83); return; }
  uint8_t *entry = &root[slot * 32];
  int cluster = entry[26] | (entry[27] << 8);
  int length = text.length(), offset = 0;
  while (offset < length) {
    memset(mscDisk[cluster + 1], 0, 512);
    int chunk = length - offset; if (chunk > 512) chunk = 512;
    memcpy(mscDisk[cluster + 1], text.c_str() + offset, chunk);
    offset += 512;
    if (offset < length) { fat12Set(mscDisk[1], cluster, cluster + 1); cluster++; }
    else fat12Set(mscDisk[1], cluster, 0xFFF);
  }
  entry[28] = length & 0xFF; entry[29] = (length >> 8) & 0xFF; entry[30] = (length >> 16) & 0xFF; entry[31] = (length >> 24) & 0xFF;
}

int main() {
  sessionKeyReady = true;
  vaultBlob = String("web\t\twebpw\n"); vaultParseBlob();      // one password entry, no cards
  mscMode = 0; mscBuildImage();
  printf("after build: CARDS.TXT = [%s]\n", mscReadSig("Label\tCardNumber").c_str());

  hostWrite("CARDS   TXT", String("Label\tCardNumber\tExp(MM/YY)\tCVV\r\nVisa\t4111111111111111\t08/28\t123\r\n"));
  int imported = -9;
  bool ok = mscCommitEdit(&imported);
  printf("commit ok=%d imported=%d  entries=%d\n", ok, imported, vault.count);

  int web = findEntry("web"), visa = findEntry("Visa");
  printf("web=%d Visa=%d\n", web, visa);
  if (visa >= 0) printf("Visa card=%d pan=%s exp=%s cvv=%s\n", vault.entries[visa].kind, vault.entries[visa].secret, vault.entries[visa].expiry, vault.entries[visa].cvv);
  int failures = 0;
  if (web < 0) { printf("FAIL web lost\n"); failures++; }
  if (visa < 0) { printf("FAIL card not saved\n"); failures++; }
  else if (!vault.entries[visa].kind) { printf("FAIL saved but not a card\n"); failures++; }
  printf(failures ? "\nFAILED\n" : "\nOK\n");
  return failures;
}
