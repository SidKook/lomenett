// secure.h -- storage: LittleFS + AES-128-CBC files, key = PBKDF2-HMAC-SHA256
// of the PIN with a salt bound to this RP's unique ID. Session key lives in
// RAM only; plaintext is never written to flash.
//
//   /vault.bin  salt(16) + iv(16) + AES(PIN_CHECK_MARKER)(32) [+ "SE"]  proves the PIN
//   /lock.bin   iv(16) + AES(entry lines)                               the vault
//   /fails.bin  1 byte: wrong-PIN counter
#pragma once
#include <LittleFS.h>
#include <AESLib.h>
#include <SHA256.h>
#include "pico/unique_id.h"
#include "hardware/structs/rosc.h"
#include "totp.h"

static const char *PIN_CHECK_MARKER = "M4ttS1dH3lpP33ps!";   // fixed: existing vaults depend on it
static const int   PBKDF2_ITERATIONS = 5000;
#define SALT_LEN 16
#define IV_LEN   16
#define VAULT_BIN_SIZE_SE_BOUND (SALT_LEN + IV_LEN + 32 + 2)
static AESLib aesLib;
static byte sessionKey[16] = {};
static bool sessionKeyReady = false;
static char boardID[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

// Random bytes from the RP's ring oscillator.
static void roscRandomBytes(byte *output, int count) {
  for (int i = 0; i < count; i++) {
    byte value = 0;
    for (int bit = 0; bit < 8; bit++) value = (value << 1) | (rosc_hw->randombit & 1);
    output[i] = value;
  }
}
#include "se050.h"

static void pbkdf2_hmac_sha256(const byte *password, int passwordLength, const byte *salt, int saltLength,
                               int iterations, byte *output, int outputLength) {
  SHA256 hmac; byte accumulated[32], previous[32];
  hmac.resetHMAC(password, passwordLength); hmac.update(salt, saltLength);
  byte blockIndex[4] = {0, 0, 0, 1}; hmac.update(blockIndex, 4);
  hmac.finalizeHMAC(password, passwordLength, accumulated, 32);
  memcpy(previous, accumulated, 32);
  for (int i = 1; i < iterations; i++) {
    hmac.resetHMAC(password, passwordLength); hmac.update(previous, 32);
    hmac.finalizeHMAC(password, passwordLength, previous, 32);
    for (int j = 0; j < 32; j++) accumulated[j] ^= previous[j];
  }
  memcpy(output, accumulated, outputLength);
  memset(accumulated, 0, 32); memset(previous, 0, 32);
}

static bool constantTimeEquals(const byte *a, const byte *b, int length) {
  byte difference = 0;
  for (int i = 0; i < length; i++) difference |= a[i] ^ b[i];
  return difference == 0;
}

// salt(16) + first 8 chars of the board ID -> 24-byte device-bound salt.
static void buildDeviceSalt(const byte *salt, byte *deviceSalt24) {
  memcpy(deviceSalt24, salt, SALT_LEN);
  memcpy(deviceSalt24 + SALT_LEN, boardID, 8);
}

// Optional on-screen progress for long saves (the sketch sets the hook).
static void (*saveStageHook)(const char *stepName) = nullptr;
static void reportSaveStage(const char *stepName) { if (saveStageHook) saveStageHook(stepName); }

// ---- encrypted files
// Written to <path>.tmp then renamed over <path>, so a power cut never
// leaves a truncated vault.
static bool encryptToFile(const char *path, const String &plaintext) {
  if (!sessionKeyReady) return false;
  size_t length = plaintext.length();
  size_t paddedLength = ((length + 15) / 16) * 16;
  if (paddedLength == 0) paddedLength = 16;
  reportSaveStage("encrypting");
  uint8_t *buffer = (uint8_t*)calloc(paddedLength, 1);
  if (!buffer) { reportSaveStage("no memory to encrypt"); return false; }
  memcpy(buffer, plaintext.c_str(), length);
  byte iv[IV_LEN], ivWorking[IV_LEN];
  roscRandomBytes(iv, IV_LEN); memcpy(ivWorking, iv, IV_LEN);
  aesLib.encrypt(buffer, paddedLength, buffer, sessionKey, 128, ivWorking);

  reportSaveStage("writing flash");
  String tempPath = String(path) + ".tmp";
  File file = LittleFS.open(tempPath.c_str(), "w");
  if (!file) { free(buffer); reportSaveStage("flash open failed"); return false; }
  size_t written = file.write(iv, IV_LEN);
  written += file.write(buffer, paddedLength);
  file.close();
  memset(buffer, 0, paddedLength); free(buffer);
  if (written != IV_LEN + paddedLength) { LittleFS.remove(tempPath.c_str()); reportSaveStage("flash full?"); return false; }
  reportSaveStage("replacing vault");
  return LittleFS.rename(tempPath.c_str(), path);
}

static String decryptFromFile(const char *path) {
  if (!sessionKeyReady) return "";
  File file = LittleFS.open(path, "r");
  if (!file) return "";
  size_t fileSize = file.size();
  if (fileSize <= IV_LEN) { file.close(); return ""; }
  byte iv[IV_LEN]; file.read(iv, IV_LEN);
  size_t cipherLength = fileSize - IV_LEN;
  uint8_t *buffer = (uint8_t*)malloc(cipherLength + 1);
  if (!buffer) { file.close(); return ""; }
  file.read(buffer, cipherLength); file.close();
  aesLib.decrypt(buffer, cipherLength, buffer, sessionKey, 128, iv);
  buffer[cipherLength] = 0;
  String plaintext = (char*)buffer;
  memset(buffer, 0, cipherLength); free(buffer);
  return plaintext;
}

// ---- PIN
// Legacy:     key = PBKDF2(PIN, deviceSalt)
// SE050-bound: pinHash = PBKDF2(PIN, deviceSalt); chipMac = SE050.HMAC(KEK, pinHash);
//              key = PBKDF2(chipMac, deviceSalt, 2 or 3)   -- the chip must take part.
static bool vaultExists() { return LittleFS.exists("/vault.bin"); }
static bool vaultIsSeBound() {
  File file = LittleFS.open("/vault.bin", "r");
  if (!file) return false;
  size_t size = file.size(); file.close();
  return size == VAULT_BIN_SIZE_SE_BOUND;
}

// iterations == PBKDF2_ITERATIONS gives the PIN-check key; +1 gives the session key.
static bool deriveKey(const String &pin, const byte *deviceSalt, int deviceSaltLength, bool seBound,
                      int iterations, byte *key16) {
  byte pinHash[16];
  pbkdf2_hmac_sha256((const byte*)pin.c_str(), pin.length(), deviceSalt, deviceSaltLength, PBKDF2_ITERATIONS, pinHash, 16);
  if (!seBound) {
    pbkdf2_hmac_sha256((const byte*)pin.c_str(), pin.length(), deviceSalt, deviceSaltLength, iterations, key16, 16);
    memset(pinHash, 0, 16);
    return true;
  }
  byte chipMac[32];
  if (!seKekMac(pinHash, 16, chipMac)) { memset(pinHash, 0, 16); return false; }
  pbkdf2_hmac_sha256(chipMac, 32, deviceSalt, deviceSaltLength, iterations == PBKDF2_ITERATIONS ? 2 : 3, key16, 16);
  memset(pinHash, 0, 16); memset(chipMac, 0, 32);
  return true;
}

// New PIN: write /vault.bin (SE050-bound if the chip answers) and set the session key.
static bool setupPin(const String &pin) {
  bool seBound = seKekProvision();
  if (!seBound) Serial.println("SE050: not available - vault will NOT be chip-bound");
  byte salt[SALT_LEN], iv[IV_LEN];
  roscRandomBytes(salt, SALT_LEN); roscRandomBytes(iv, IV_LEN);
  byte deviceSalt[SALT_LEN + 8]; buildDeviceSalt(salt, deviceSalt);

  byte checkKey[16];
  if (!deriveKey(pin, deviceSalt, sizeof(deviceSalt), seBound, PBKDF2_ITERATIONS, checkKey)) return false;
  byte markerCipher[32] = {0}; byte ivWorking[IV_LEN]; memcpy(ivWorking, iv, IV_LEN);
  aesLib.encrypt((byte*)PIN_CHECK_MARKER, strlen(PIN_CHECK_MARKER), markerCipher, checkKey, 128, ivWorking);
  memset(checkKey, 0, 16);

  File file = LittleFS.open("/vault.bin", "w");
  if (file) {
    file.write(salt, SALT_LEN); file.write(iv, IV_LEN); file.write(markerCipher, 32);
    if (seBound) file.write((const uint8_t*)"SE", 2);
    file.close();
  }
  bool ok = deriveKey(pin, deviceSalt, sizeof(deviceSalt), seBound, PBKDF2_ITERATIONS + 1, sessionKey);
  sessionKeyReady = ok;
  return ok;
}

// Check a PIN against /vault.bin; on success the session key is ready.
static bool loginPin(const String &pin) {
  File file = LittleFS.open("/vault.bin", "r");
  if (!file) return false;
  bool seBound = file.size() == VAULT_BIN_SIZE_SE_BOUND;
  byte salt[SALT_LEN], iv[IV_LEN], markerCipher[32];
  file.read(salt, SALT_LEN); file.read(iv, IV_LEN); file.read(markerCipher, 32); file.close();
  byte deviceSalt[SALT_LEN + 8]; buildDeviceSalt(salt, deviceSalt);

  byte checkKey[16], decrypted[32] = {0};
  if (!deriveKey(pin, deviceSalt, sizeof(deviceSalt), seBound, PBKDF2_ITERATIONS, checkKey)) return false;
  aesLib.decrypt(markerCipher, 32, decrypted, checkKey, 128, iv);
  bool ok = constantTimeEquals(decrypted, (const byte*)PIN_CHECK_MARKER, strlen(PIN_CHECK_MARKER));
  if (ok) {
    ok = deriveKey(pin, deviceSalt, sizeof(deviceSalt), seBound, PBKDF2_ITERATIONS + 1, sessionKey);
    sessionKeyReady = ok;
  }
  memset(checkKey, 0, 16); memset(decrypted, 0, 32); memset(deviceSalt, 0, sizeof(deviceSalt));
  return ok;
}

static void lockSession() { memset(sessionKey, 0, 16); sessionKeyReady = false; }

static int failCount() {
  File file = LittleFS.open("/fails.bin", "r");
  if (!file) return 0;
  int count = file.read(); file.close();
  return count < 0 ? 0 : count;
}
static void failIncrement() {
  int count = failCount() + 1;
  File file = LittleFS.open("/fails.bin", "w");
  if (file) { file.write((uint8_t)count); file.close(); }
}
static void failReset() { if (LittleFS.exists("/fails.bin")) LittleFS.remove("/fails.bin"); }

// ---- entry lines (Secrets.txt format; kept compatible with existing files)
static const char *SECRETS_HEADER = "Label\tUsername(Optional)\tPassword(Required)\tTab Times(Optional)\tFlags(E=enter F=fav)\tSite(Optional)";
static bool isHeaderLine(const String &line) {
  String trimmed = line; trimmed.trim();
  return trimmed.startsWith("Label\tUsername") || trimmed.startsWith("Label Username(Optional)");
}
// Label = text up to the first tab (or first space, for old space-separated lines).
static String lineLabel(const String &line) {
  int end = line.indexOf('\t');
  if (end == -1) end = line.indexOf(' ');
  return end == -1 ? line : line.substring(0, end);
}
// A 2FA line can share a label with a password line, so the two kinds never replace each other.
static bool lineIsTotp(const String &line) {
  int firstTab = line.indexOf('\t');
  return firstTab >= 0 && line.indexOf("\t$TOTP\t") == firstTab;
}
// Case-insensitive sort key; on a tie lower case sorts first.
static String labelSortKey(const String &label) {
  String key = "";
  for (int i = 0; i < (int)label.length(); i++) {
    char ch = label.charAt(i);
    if (ch >= 'a' && ch <= 'z')      { key += ch; key += '0'; }
    else if (ch >= 'A' && ch <= 'Z') { key += (char)(ch + ('a' - 'A')); key += '1'; }
    else                             { key += ch; key += '0'; }
  }
  return key;
}
// Insert newLine into sorted text, replacing a line with the same label and kind.
static void sortedInsert(String &sorted, const String &newLine) {
  if (newLine.length() == 0) return;
  String newLabel = lineLabel(newLine);
  bool newIsTotp = lineIsTotp(newLine);
  String result = "";
  bool inserted = false, replaced = false;
  int pos = 0, length = sorted.length();
  while (pos <= length) {
    int lineEnd = sorted.indexOf('\n', pos);
    if (lineEnd == -1) lineEnd = length;
    String line = sorted.substring(pos, lineEnd); pos = lineEnd + 1;
    if (line.length() == 0) { if (pos > length) break; continue; }
    String existingLabel = lineLabel(line);
    if (existingLabel == newLabel && lineIsTotp(line) == newIsTotp) {
      if (!replaced) { result += newLine + "\n"; replaced = true; inserted = true; }
      if (pos > length) break;
      continue;
    }
    if (!inserted && labelSortKey(newLabel) < labelSortKey(existingLabel)) { result += newLine + "\n"; inserted = true; }
    result += line + "\n";
    if (pos > length) break;
  }
  if (!inserted) result += newLine + "\n";
  sorted = result;
}
// Same order as comparing labelSortKey() strings, without allocating.
static int labelCompare(const char *a, int aLength, const char *b, int bLength) {
  int shorter = aLength < bLength ? aLength : bLength;
  for (int i = 0; i < shorter; i++) {
    unsigned char charA = a[i], charB = b[i];
    bool upperA = charA >= 'A' && charA <= 'Z', upperB = charB >= 'A' && charB <= 'Z';
    unsigned char lowerA = upperA ? charA + 32 : charA, lowerB = upperB ? charB + 32 : charB;
    if (lowerA != lowerB) return lowerA < lowerB ? -1 : 1;
    if (upperA != upperB) return upperA ? 1 : -1;
  }
  return aLength - bLength;
}
// Fallback for sortAllEntries() when its index can't be allocated.
static String sortAllEntriesSlow(const String &raw) {
  String sorted = "";
  int pos = 0, length = raw.length();
  while (pos <= length) {
    int lineEnd = raw.indexOf('\n', pos);
    if (lineEnd == -1) lineEnd = length;
    String line = raw.substring(pos, lineEnd); pos = lineEnd + 1; line.trim();
    if (line.length() && !isHeaderLine(line)) sortedInsert(sorted, line);
    if (pos > length) break;
  }
  return sorted;
}
// Sort lines by label and drop duplicates (same label + kind: last content
// wins, at the first one's position). Same result as sortedInsert() per
// line, but O(n^2) compares instead of rebuilding the text per line (which
// ran out of RAM on a 434-entry import).
struct SortLine { int start, length, labelLength; bool isTotp; };
static String sortAllEntries(const String &raw) {
  const char *text = raw.c_str();
  int textLength = raw.length();
  int capacity = 1;
  for (int i = 0; i < textLength; i++) if (text[i] == '\n') capacity++;
  SortLine *lines = (SortLine *)malloc(capacity * sizeof(SortLine));
  if (!lines) return sortAllEntriesSlow(raw);

  int lineCount = 0, pos = 0;
  while (pos <= textLength) {
    int lineEnd = pos;
    while (lineEnd < textLength && text[lineEnd] != '\n') lineEnd++;
    int first = pos, last = lineEnd;
    pos = lineEnd + 1;
    while (first < last && isspace((unsigned char)text[first])) first++;     // trim
    while (last > first && isspace((unsigned char)text[last - 1])) last--;
    if (last > first) {
      const char *line = text + first;
      int length = last - first;
      bool isHeader = (length >= 14 && !strncmp(line, "Label\tUsername", 14))
                   || (length >= 24 && !strncmp(line, "Label Username(Optional)", 24));
      if (!isHeader) {
        int firstTab = -1, firstSpace = -1;
        for (int k = 0; k < length; k++) {
          if (line[k] == '\t') { firstTab = k; break; }
          if (firstSpace < 0 && line[k] == ' ') firstSpace = k;
        }
        SortLine &entry = lines[lineCount++];
        entry.start = first;
        entry.length = length;
        entry.labelLength = firstTab >= 0 ? firstTab : firstSpace >= 0 ? firstSpace : length;   // as lineLabel()
        entry.isTotp = firstTab >= 0 && length - firstTab >= 7 && !strncmp(line + firstTab, "\t$TOTP\t", 7);
      }
    }
    if (pos > textLength) break;
  }

  for (int i = 1; i < lineCount; i++) {                 // stable insertion sort
    SortLine current = lines[i];
    int j = i - 1;
    while (j >= 0 && labelCompare(text + lines[j].start, lines[j].labelLength,
                                  text + current.start, current.labelLength) > 0) {
      lines[j + 1] = lines[j]; j--;
    }
    lines[j + 1] = current;
  }

  String sorted; sorted.reserve(textLength + 1);
  for (int groupStart = 0; groupStart < lineCount; ) {   // runs of equal labels
    int groupEnd = groupStart + 1;
    while (groupEnd < lineCount && labelCompare(text + lines[groupStart].start, lines[groupStart].labelLength,
                                                text + lines[groupEnd].start, lines[groupEnd].labelLength) == 0) groupEnd++;
    int lastPassword = -1, lastTotp = -1;
    for (int k = groupStart; k < groupEnd; k++) { if (lines[k].isTotp) lastTotp = k; else lastPassword = k; }
    bool passwordDone = false, totpDone = false;
    for (int k = groupStart; k < groupEnd; k++) {        // each kind at its first position, last content
      bool isTotp = lines[k].isTotp;
      if (isTotp ? totpDone : passwordDone) continue;
      if (isTotp) totpDone = true; else passwordDone = true;
      const SortLine &winner = lines[isTotp ? lastTotp : lastPassword];
      sorted += raw.substring(winner.start, winner.start + winner.length);
      sorted += "\n";
    }
    groupStart = groupEnd;
  }
  free(lines);
  return sorted;
}

#define TOTP_MAX_BACKUPS 5
#define TOTP_BACKUP_LEN  32
#define TOTP_SECRET_LEN  103                 // base32 chars for a 64-byte key

struct ParsedEntry {
  String label, username;
  String secret;                             // password, card number, or base32 2FA secret
  String expiry;                             // card expiry, or 2FA digit count
  String cvv;                                // card CVV, or 2FA period
  String site;
  int    tabCount;
  bool   isCard, isTotp;
  bool   pressEnter, favorite;
  int    icon;
  String backupCodes[TOTP_MAX_BACKUPS];
  int    backupCodeCount;
};

static bool isAllDigits(const String &text) {
  if (!text.length()) return false;
  for (int i = 0; i < (int)text.length(); i++) if (!isdigit((unsigned char)text[i])) return false;
  return true;
}

// Line formats (TAB-separated):
//   Password: Label  Password | Label  Username  Password [ Tabs [ Flags [ Site ]]]
//   Card:     Label  $CARD  Number  Exp  CVV
//   2FA:      Label  $TOTP  Base32Secret  Digits  Period [ Flags [ Backup1..5 ]]
//   Flags:    E = press Enter after typing, F = favourite, I<n> = icon number
// Lines with no TAB use the old space-separated format.
static bool parseEntrySpaces(const String &line, ParsedEntry &entry) {
  int firstSpace = line.indexOf(' ');
  if (firstSpace == -1) { entry.label = line; return false; }
  entry.label = line.substring(0, firstSpace);
  String rest = line.substring(firstSpace + 1); rest.trim();
  int secondSpace = rest.indexOf(' ');
  if (secondSpace == -1) { entry.secret = rest; return entry.secret.length() > 0; }
  entry.username = rest.substring(0, secondSpace);
  String afterUser = rest.substring(secondSpace + 1); afterUser.trim();
  int lastSpace = afterUser.lastIndexOf(' ');
  if (lastSpace != -1) {
    String tail = afterUser.substring(lastSpace + 1);
    if (isAllDigits(tail)) {
      entry.tabCount = tail.toInt();
      entry.secret = afterUser.substring(0, lastSpace); entry.secret.trim();
      return entry.secret.length() > 0;
    }
  }
  entry.secret = afterUser;
  return entry.secret.length() > 0;
}

static bool parseEntry(const String &rawLine, ParsedEntry &entry) {
  String line = rawLine; line.trim();
  entry.label = ""; entry.username = ""; entry.secret = ""; entry.expiry = ""; entry.cvv = ""; entry.site = "";
  entry.tabCount = 0; entry.isCard = false; entry.isTotp = false; entry.pressEnter = false; entry.favorite = false;
  entry.icon = 0; entry.backupCodeCount = 0;
  if (!line.length() || isHeaderLine(line)) return false;
  if (line.indexOf('\t') == -1) return parseEntrySpaces(line, entry);

  const int MAX_FIELDS = 6 + TOTP_MAX_BACKUPS;
  String fields[MAX_FIELDS];
  int fieldCount = 0, pos = 0;
  while (fieldCount < MAX_FIELDS) {
    int tab = line.indexOf('\t', pos);
    if (tab == -1) { fields[fieldCount++] = line.substring(pos); break; }
    fields[fieldCount++] = line.substring(pos, tab);
    pos = tab + 1;
  }
  for (int i = 0; i < fieldCount; i++) fields[i].trim();
  entry.label = fields[0];

  if (fieldCount >= 2 && fields[1] == "$CARD") {
    entry.isCard = true;
    entry.secret = (fieldCount > 2) ? fields[2] : "";
    entry.expiry = (fieldCount > 3) ? fields[3] : "";
    entry.cvv    = (fieldCount > 4) ? fields[4] : "";
    return entry.label.length() > 0 && entry.secret.length() > 0;
  }

  auto applyFlags = [&](const String &flags) {
    for (int i = 0; i < (int)flags.length(); i++) {
      char flag = toupper((unsigned char)flags[i]);
      if (flag == 'E') entry.pressEnter = true;
      else if (flag == 'F') entry.favorite = true;
      else if (flag == 'I') {
        int iconNumber = 0;
        while (i + 1 < (int)flags.length() && isdigit((unsigned char)flags[i + 1])) iconNumber = iconNumber * 10 + (flags[++i] - '0');
        if (iconNumber >= 0 && iconNumber < 200) entry.icon = iconNumber;
      }
    }
  };

  if (fieldCount >= 3 && fields[1] == "$TOTP") {
    char secret[TOTP_SECRET_LEN + 1];
    if (!totpNormalize(fields[2].c_str(), secret, sizeof(secret))) return false;
    entry.isTotp = true;
    entry.secret = secret;
    entry.expiry = String(totpDigits(fieldCount > 3 ? fields[3].c_str() : ""));
    entry.cvv    = String(totpPeriod(fieldCount > 4 ? fields[4].c_str() : ""));
    if (fieldCount > 5) applyFlags(fields[5]);
    for (int k = 6; k < fieldCount; k++)                     // backup codes, gaps closed
      if (fields[k].length()) entry.backupCodes[entry.backupCodeCount++] = fields[k].substring(0, TOTP_BACKUP_LEN);
    memset(secret, 0, sizeof(secret));
    return entry.label.length() > 0;
  }

  if (fieldCount == 2) {
    entry.secret = fields[1];
  } else if (fieldCount >= 3) {
    entry.username = fields[1];
    entry.secret = fields[2];
    if (fieldCount >= 4) { if (isAllDigits(fields[3])) entry.tabCount = fields[3].toInt(); else applyFlags(fields[3]); }
    if (fieldCount >= 5) applyFlags(fields[4]);
    if (fieldCount >= 6) entry.site = fields[5];
  }
  return entry.label.length() > 0 && entry.secret.length() > 0;
}
