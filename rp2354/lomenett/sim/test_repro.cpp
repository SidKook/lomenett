// test_repro.cpp -- reproduces a reported vault: a card line typed with
// spaces into CARDS.TXT, then prints the resulting entries.
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

// Overwrite an existing 8.3 file in the RAM disk, as a host edit would.
static void hostWrite(const char *name83, const String &text) {
  uint8_t *root = mscDisk[2];
  int slot = -1;
  for (int i = 0; i < 16; i++) if (!memcmp(&root[i * 32], name83, 11)) { slot = i; break; }
  if (slot < 0) { printf("  file %s not found\n", name83); return; }
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
  vaultBlob = String("FIREFOX\t\t1234567890123456\nJunkmail\tmattsjunk9999@gmail.com\tpass8888\n"
                     "mouser.com\tInsanePlotTwist\t12345678901234567\nring\tmattvalerickson@gmail.com\t1234567890123456\n");
  vaultParseBlob();
  printf("before: %d entries\n", vault.count);
  mscMode = 0; mscBuildImage();
  hostWrite("CARDS   TXT", String("Label\tCardNumber\tExp(MM/YY)\tCVV\r\nbunk 1234652198887856 11/31 988\r\n"));
  printf("CARDS sig read = [%s]\n", mscReadSig("Label\tCardNumber").c_str());
  int imported = -9;
  bool ok = mscCommitEdit(&imported);
  printf("commit ok=%d imp=%d -> %d entries\n", ok, imported, vault.count);
  for (int i = 0; i < vault.count; i++) {
    const VaultEntry &entry = vault.entries[i];
    printf("  %-12s card=%d pw=%s exp=%s cvv=%s\n", entry.name, entry.kind, entry.secret, entry.expiry, entry.cvv);
  }
  return 0;
}
