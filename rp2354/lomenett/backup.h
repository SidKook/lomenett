// backup.h -- portable encrypted backup, keyed by the PIN only (no board ID,
// no SE050) so it restores onto a new device. Restoring re-binds it to that
// device's SE050. Plain hex text file:
//
//   LMBK1
//   hex(salt 16)
//   hex(iv 16)
//   hex(AES-128-CBC(PIN_CHECK_MARKER "\n" vault lines))   key = PBKDF2(PIN, salt)
#pragma once

static String hexEncode(const uint8_t *data, int length) {
  static const char *HEX_DIGITS = "0123456789ABCDEF";
  String hex; hex.reserve(length * 2);
  for (int i = 0; i < length; i++) { hex += HEX_DIGITS[data[i] >> 4]; hex += HEX_DIGITS[data[i] & 15]; }
  return hex;
}

static int hexDecode(const String &hex, uint8_t *output, int capacity) {
  auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10; };
  int length = 0;
  for (int i = 0; i + 1 < (int)hex.length() && length < capacity; i += 2)
    output[length++] = (nibble(hex[i]) << 4) | nibble(hex[i+1]);
  return length;
}

// Build the backup file text, encrypted under `pin`.
static String makeBackup(const String &pin) {
  byte salt[SALT_LEN], iv[IV_LEN];
  roscRandomBytes(salt, SALT_LEN); roscRandomBytes(iv, IV_LEN);
  byte key[16];
  pbkdf2_hmac_sha256((const byte*)pin.c_str(), pin.length(), salt, SALT_LEN, PBKDF2_ITERATIONS, key, 16);   // salt only: portable

  String plaintext = String(PIN_CHECK_MARKER) + "\n" + vaultBlob;
  size_t length = plaintext.length();
  size_t paddedLength = ((length + 15) / 16) * 16;
  if (paddedLength == 0) paddedLength = 16;
  uint8_t *buffer = (uint8_t*)calloc(paddedLength, 1);
  if (!buffer) return "";
  memcpy(buffer, plaintext.c_str(), length);
  byte ivWorking[IV_LEN]; memcpy(ivWorking, iv, IV_LEN);
  aesLib.encrypt(buffer, paddedLength, buffer, key, 128, ivWorking);

  String file = "LMBK1\r\n" + hexEncode(salt, SALT_LEN) + "\r\n" + hexEncode(iv, IV_LEN) + "\r\n"
              + hexEncode(buffer, paddedLength) + "\r\n";
  memset(key, 0, 16); memset(buffer, 0, paddedLength); free(buffer);
  return file;
}

// Restore a backup with its PIN; that PIN becomes this device's PIN.
static bool restoreBackup(const String &file, const String &pin) {
  int line1End = file.indexOf('\n');
  if (line1End < 0 || file.substring(0, 5) != "LMBK1") return false;
  int line2End = file.indexOf('\n', line1End + 1);
  int line3End = file.indexOf('\n', line2End + 1);
  int line4End = file.indexOf('\n', line3End + 1);
  if (line2End < 0 || line3End < 0 || line4End < 0) return false;

  auto trimmed = [](String text) { text.trim(); return text; };
  String saltHex   = trimmed(file.substring(line1End + 1, line2End));
  String ivHex     = trimmed(file.substring(line2End + 1, line3End));
  String cipherHex = trimmed(file.substring(line3End + 1, line4End));

  byte salt[SALT_LEN], iv[IV_LEN];
  if (hexDecode(saltHex, salt, SALT_LEN) != SALT_LEN || hexDecode(ivHex, iv, IV_LEN) != IV_LEN) return false;
  int cipherLength = cipherHex.length() / 2;
  if (cipherLength < 16 || cipherLength > 8192 || (cipherLength % 16)) return false;
  uint8_t *buffer = (uint8_t*)malloc(cipherLength + 1);
  if (!buffer) return false;
  hexDecode(cipherHex, buffer, cipherLength);

  byte key[16];
  pbkdf2_hmac_sha256((const byte*)pin.c_str(), pin.length(), salt, SALT_LEN, PBKDF2_ITERATIONS, key, 16);
  aesLib.decrypt(buffer, cipherLength, buffer, key, 128, iv);
  buffer[cipherLength] = 0;
  memset(key, 0, 16);

  int markerLength = strlen(PIN_CHECK_MARKER);
  bool pinCorrect = !memcmp(buffer, PIN_CHECK_MARKER, markerLength) && buffer[markerLength] == '\n';
  if (pinCorrect) {
    String lines = String((char*)(buffer + markerLength + 1));
    memset(buffer, 0, cipherLength); free(buffer);
    setupPin(pin);                         // bind to this device's SE050 under the backup PIN
    return vaultCommit(lines);
  }
  memset(buffer, 0, cipherLength); free(buffer);
  return false;
}
