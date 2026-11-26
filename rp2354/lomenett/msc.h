// msc.h -- EDIT mode: the board appears as a USB drive "Lomenett Vault" with
// SECRETS.TXT, CARDS.TXT and 2FA.TXT. A FAT12 image is built in RAM on entry;
// host edits are parsed after writes settle and saved. Reports "no medium"
// outside EDIT mode. Also used for backup export and restore import.
#pragma once
#include <Adafruit_TinyUSB.h>
extern Print &LogRef;

#define MSC_BLOCK  512
#define MSC_BLOCKS 200                       // 100 KB: boot, FAT, root dir, 197 data clusters
#define MSC_SECTOR_BOOT 0
#define MSC_SECTOR_FAT  1
#define MSC_SECTOR_ROOT 2                    // data cluster N lives in sector N + 1
#define MSC_ROOT_ENTRIES 16

#define MSC_MODE_EDIT    0
#define MSC_MODE_BACKUP  1                   // drive holds BACKUP.LMB for export
#define MSC_MODE_RESTORE 2                   // empty drive; user drops BACKUP.LMB

static uint8_t mscDisk[MSC_BLOCKS][MSC_BLOCK];
static Adafruit_USBD_MSC usb_msc;
static volatile bool mscReady = false;       // medium present
static volatile uint32_t mscLastWrite = 0;   // millis of the last host write, 0 = clean
static volatile bool mscEjected = false;
static int mscMode = MSC_MODE_EDIT;
String mscBackupText;                        // built for export / captured on restore

static const char README_TXT[] =
  "Lomenett Vault\r\n"
  "==============\r\n"
  "Edit Secrets.txt: one entry per line, fields separated by TAB:\r\n"
  "  Label <TAB> Password\r\n"
  "  Label <TAB> Username <TAB> Password\r\n"
  "  Label <TAB> Username <TAB> Password <TAB> Tabs  (Tab key presses\r\n"
  "  between username and password when typing both)\r\n"
  "  ... <TAB> Tabs <TAB> Flags <TAB> Site  (the website, e.g.\r\n"
  "  github.com, so the browser extension can find the login)\r\n"
  "Stored passwords show as ******** - leave the stars to keep a\r\n"
  "password, type over them to change it. Delete a line to remove\r\n"
  "the entry. Entries are sorted by label. Save, then eject; the\r\n"
  "device encrypts and stores it and says SAVED.\r\n"
  "\r\nOr drop a Google/Chrome or Firefox password .CSV here\r\n(name,url,username,password) and eject to import it.\r\n"
  "\r\n2FA.txt holds the 2FA (authenticator) codes, one per line:\r\n"
  "  Label <TAB> Secret\r\n"
  "  Label <TAB> Secret <TAB> Digits <TAB> Period <TAB> Flags\r\n"
  "The secret is the text key a site shows under its QR code\r\n"
  "(\"can't scan?\"), spaces allowed. Or paste the whole\r\n"
  "otpauth://totp/... link as the line. Digits default to 6,\r\n"
  "period to 30 s. Flags: E = press Enter after the code.\r\n"
  "Up to 5 one-time backup (recovery) codes can follow the\r\n"
  "flags, one per column; stored ones show as ********. Clear\r\n"
  "a column once you have used that code.\r\n"
  "Codes need the time: the device gets it over USB (see\r\n"
  "clock_sync.html, or send \"time <unix seconds>\" on the console).\r\n";
#define PW_MASK "********"
static const char *TOTP_HEADER = "Label\tSecret(Base32 or otpauth://)\tDigits\tPeriod\tFlags(E=enter F=fav)\tBackup1\tBackup2\tBackup3\tBackup4\tBackup5";
#define TOTP_SIG    "Label\tSecret("
#define SECRETS_SIG "Label\tUsername(Optional)"
#define CARDS_SIG   "Label\tCardNumber"

// ---- FAT12 image -----------------------------------------------------------
// 32-byte directory entry: 8.3 name, attributes, first cluster, size.
static void mscDirEntry(uint8_t *entry, const char *name83, uint16_t firstCluster, uint32_t size, uint8_t attributes) {
  memset(entry, 0, 32);
  memcpy(entry, name83, 11);
  entry[11] = attributes;
  entry[26] = firstCluster & 0xFF; entry[27] = firstCluster >> 8;
  entry[28] = size & 0xFF; entry[29] = (size >> 8) & 0xFF; entry[30] = (size >> 16) & 0xFF; entry[31] = size >> 24;
}
static uint16_t dirEntryCluster(const uint8_t *entry) { return entry[26] | (entry[27] << 8); }
static uint32_t dirEntrySize(const uint8_t *entry) {
  return entry[28] | (entry[29] << 8) | (entry[30] << 16) | ((uint32_t)entry[31] << 24);
}

// FAT12 packs two 12-bit entries into three bytes.
static void fat12Set(uint8_t *fat, int cluster, uint16_t value) {
  int offset = cluster * 3 / 2;
  if (cluster & 1) { fat[offset] = (fat[offset] & 0x0F) | ((value << 4) & 0xF0); fat[offset+1] = value >> 4; }
  else             { fat[offset] = value & 0xFF; fat[offset+1] = (fat[offset+1] & 0xF0) | ((value >> 8) & 0x0F); }
}
static uint16_t fat12Get(const uint8_t *fat, int cluster) {
  int offset = cluster * 3 / 2;
  return (cluster & 1) ? ((fat[offset] >> 4) | (fat[offset+1] << 4)) : (fat[offset] | ((fat[offset+1] & 0x0F) << 8));
}

// Write a file into consecutive clusters from firstCluster; returns the next free cluster.
static int mscPlaceFile(const char *name83, int dirSlot, const uint8_t *data, uint32_t size, int firstCluster) {
  uint8_t *fat = mscDisk[MSC_SECTOR_FAT];
  int cluster = firstCluster;
  int clusterCount = (size + MSC_BLOCK - 1) / MSC_BLOCK;
  if (clusterCount == 0) clusterCount = 1;
  for (int i = 0; i < clusterCount; i++) {
    uint8_t *sector = mscDisk[cluster + 1];
    memset(sector, 0, MSC_BLOCK);
    uint32_t offset = i * MSC_BLOCK, length = size - offset;
    if (length > MSC_BLOCK) length = MSC_BLOCK;
    if (offset < size) memcpy(sector, data + offset, length);
    fat12Set(fat, cluster, (i == clusterCount - 1) ? 0xFFF : cluster + 1);
    cluster++;
  }
  mscDirEntry(&mscDisk[MSC_SECTOR_ROOT][dirSlot * 32], name83, firstCluster, size, 0x20);
  return cluster;
}

// SECRETS.TXT / CARDS.TXT / 2FA.TXT contents, secrets masked.
static String buildSecretsText() {
  String text = String(SECRETS_HEADER) + "\r\n";
  for (int i = 0; i < vault.count; i++) {
    const VaultEntry &entry = vault.entries[i];
    if (entry.kind) continue;
    text += pwLine(entry.name, entry.username, PW_MASK, entry.tabCount, entry.pressEnter, entry.favorite, entry.icon, entry.site) + "\r\n";
  }
  return text;
}
static String buildCardsText() {
  String text = "Label\tCardNumber\tExp(MM/YY)\tCVV\r\n";
  for (int i = 0; i < vault.count; i++) {
    const VaultEntry &entry = vault.entries[i];
    if (entry.kind != KIND_CARD) continue;
    text += String(entry.name) + "\t" + PW_MASK + "\t" + entry.expiry + "\t***\r\n";
  }
  return text;
}
static String buildTotpText() {
  String text = String(TOTP_HEADER) + "\r\n";
  for (int i = 0; i < vault.count; i++) {
    const VaultEntry &entry = vault.entries[i];
    if (entry.kind != KIND_TOTP) continue;
    String maskedCodes[TOTP_MAX_BACKUPS];
    for (int k = 0; k < entry.backupCodeCount && k < TOTP_MAX_BACKUPS; k++) maskedCodes[k] = PW_MASK;
    String line = totpLine(entry.name, PW_MASK, totpDigits(entry.expiry), totpPeriod(entry.cvv),
                           entry.pressEnter, entry.favorite, entry.icon, maskedCodes, entry.backupCodeCount);
    int markerAt = line.indexOf("\t$TOTP");
    text += line.substring(0, markerAt) + line.substring(markerAt + 6) + "\r\n";   // the file implies $TOTP
  }
  return text;
}

static void mscBuildImage() {
  memset(mscDisk, 0, sizeof(mscDisk));
  uint8_t *boot = mscDisk[MSC_SECTOR_BOOT];
  static const uint8_t BOOT_SECTOR[] = {
    0xEB,0x3C,0x90, 'M','S','D','O','S','5','.','0',   // jump, OEM name
    0x00,0x02, 0x01, 0x01,0x00, 0x01,                   // 512 B/sector, 1 sector/cluster, 1 reserved, 1 FAT
    0x10,0x00, MSC_BLOCKS,0x00, 0xF8, 0x01,0x00,        // 16 root entries, sector count, media, 1 sector/FAT
    0x01,0x00, 0x01,0x00, 0,0,0,0, 0,0,0,0,             // geometry, hidden, large count
    0x80, 0x00, 0x29, 0x4C,0x4F,0x4D,0x31 };            // drive, sig, serial "LOM1"
  memcpy(boot, BOOT_SECTOR, sizeof(BOOT_SECTOR));
  memcpy(boot + 43, "LOMENETT   ", 11);
  memcpy(boot + 54, "FAT12   ", 8);
  boot[510] = 0x55; boot[511] = 0xAA;
  uint8_t *fat = mscDisk[MSC_SECTOR_FAT]; fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF;
  mscDirEntry(&mscDisk[MSC_SECTOR_ROOT][0], "LOMENETT   ", 0, 0, 0x08);   // volume label

  const char *readme =
      mscMode == MSC_MODE_BACKUP  ? "Lomenett Vault - BACKUP\r\nCopy BACKUP.LMB somewhere safe. It is encrypted\r\nwith your PIN and can be restored onto any device.\r\n" :
      mscMode == MSC_MODE_RESTORE ? "Lomenett Vault - RESTORE\r\nDrop your BACKUP.LMB file here, then eject. You\r\nwill be asked for the PIN it was made with.\r\n" :
                                    README_TXT;
  int nextCluster = mscPlaceFile("README  TXT", 1, (const uint8_t*)readme, strlen(readme), 2);
  if (mscMode == MSC_MODE_BACKUP) {
    mscPlaceFile("BACKUP  LMB", 2, (const uint8_t*)mscBackupText.c_str(), mscBackupText.length(), nextCluster);
  } else if (mscMode == MSC_MODE_EDIT) {
    String secrets = buildSecretsText();
    nextCluster = mscPlaceFile("SECRETS TXT", 2, (const uint8_t*)secrets.c_str(), secrets.length(), nextCluster);
    String cards = buildCardsText();
    nextCluster = mscPlaceFile("CARDS   TXT", 3, (const uint8_t*)cards.c_str(), cards.length(), nextCluster);
    String totp = buildTotpText();
    mscPlaceFile("2FA     TXT", 4, (const uint8_t*)totp.c_str(), totp.length(), nextCluster);
  }
}

// First file with this 3-letter extension, read by following its FAT chain.
static String mscReadFile(const char *extension3) {
  uint8_t *root = mscDisk[MSC_SECTOR_ROOT];
  int firstCluster = -1; uint32_t size = 0;
  for (int i = 0; i < MSC_ROOT_ENTRIES; i++) {
    uint8_t *entry = &root[i * 32];
    bool unusedOrDeleted = entry[0] == 0 || entry[0] == 0xE5;
    bool volumeLabelOrLongName = (entry[11] & 0x08) || (entry[11] & 0x0F) == 0x0F;
    if (unusedOrDeleted || volumeLabelOrLongName) continue;
    if (!memcmp(entry + 8, extension3, 3)) { firstCluster = dirEntryCluster(entry); size = dirEntrySize(entry); break; }
  }
  if (firstCluster < 2 || size == 0 || size > (uint32_t)(MSC_BLOCKS - 3) * MSC_BLOCK) return "";
  String contents; contents.reserve(size + 1);
  uint32_t bytesRead = 0;
  int cluster = firstCluster, guard = 0;
  while (cluster >= 2 && cluster < 0xFF8 && bytesRead < size && guard++ < MSC_BLOCKS) {
    uint32_t length = size - bytesRead;
    if (length > MSC_BLOCK) length = MSC_BLOCK;
    for (uint32_t k = 0; k < length; k++) contents += (char)mscDisk[cluster + 1][k];
    bytesRead += length;
    cluster = fat12Get(mscDisk[MSC_SECTOR_FAT], cluster);
  }
  return contents;
}

// Restore mode: capture a dropped BACKUP.LMB into mscBackupText.
static bool mscCaptureBackup() {
  String file = mscReadFile("LMB");
  if (file.indexOf("LMBK1") < 0) return false;
  mscBackupText = file;
  return true;
}

// Find a text file by scanning the whole RAM disk for its header line,
// ignoring FAT/directory (immune to how the host lays files out). If the
// header appears more than once, the longest copy (the edited one) wins.
// Strips '\r'. Returns "" if absent.
static String mscReadSig(const char *header) {
  const uint8_t *disk = &mscDisk[0][0];
  int diskSize = MSC_BLOCKS * MSC_BLOCK, headerLength = strlen(header);
  int bestStart = -1, bestLength = 0;
  for (int i = 0; i + headerLength < diskSize; i++) {
    if (memcmp(disk + i, header, headerLength)) continue;
    int end = i;
    while (end < diskSize && disk[end] != 0x00) end++;     // text runs to the zero padding (no size cap)
    if (end - i > bestLength) { bestLength = end - i; bestStart = i; }
  }
  if (bestStart < 0) return "";
  String text; text.reserve(bestLength + 1);
  for (int k = bestStart; k < bestStart + bestLength; k++) { char ch = disk[k]; if (ch != '\r') text += ch; }
  return text;
}

// ---- CSV import --------------------------------------------------------------
static int mscCsvSkipped = 0;                                  // rows skipped by the last import
static void (*mscProgress)(int done, int total) = nullptr;     // optional UI hook

// Parse one CSV record starting at *pos into fields[0..maxFields). Handles
// quoted fields with commas, quotes and newlines. Returns the field count (0 = end).
static int csvRecord(const String &csv, int *pos, int csvLength, String *fields, int maxFields) {
  int fieldIndex = 0;
  String current = "";
  bool inQuotes = false, readAnything = false;
  for (int k = 0; k < maxFields; k++) fields[k] = "";
  while (*pos < csvLength) {
    char ch = csv[(*pos)++];
    readAnything = true;
    if (inQuotes) {
      if (ch == '"') {
        if (*pos < csvLength && csv[*pos] == '"') { current += '"'; (*pos)++; }   // escaped quote
        else inQuotes = false;
      } else {
        current += ch;
      }
    }
    else if (ch == '"') inQuotes = true;
    else if (ch == ',') { if (fieldIndex < maxFields) fields[fieldIndex] = current; fieldIndex++; current = ""; }
    else if (ch == '\r') {}
    else if (ch == '\n') break;
    else current += ch;
  }
  if (fieldIndex < maxFields) fields[fieldIndex] = current;
  fieldIndex++;
  return readAnything ? fieldIndex : 0;
}

static int csvColumn(String *header, int headerCount, const char *name) {
  for (int k = 0; k < headerCount; k++) {
    String column = header[k]; column.trim();
    if (!strcasecmp(column.c_str(), name)) return k;
  }
  return -1;
}

// URL -> host without scheme, path or leading "www.".
static String csvHost(String url) {
  url.trim();
  int schemeEnd = url.indexOf("://"); if (schemeEnd >= 0) url = url.substring(schemeEnd + 3);
  int pathStart = url.indexOf('/');   if (pathStart >= 0) url = url.substring(0, pathStart);
  if (url.startsWith("www.")) url = url.substring(4);
  return url;
}

// A label that is a host becomes the site's name: "www.facebook.com" ->
// "facebook", "shop.bbc.co.uk" -> "bbc", "account.battle.net" -> "battlenet"
// (.com/.org/.edu/.gov and country endings are dropped, others joined on).
// Non-host labels ("My Bank") are returned unchanged.
static String csvSiteLabel(const String &label) {
  if (label.indexOf(' ') >= 0 || label.indexOf('.') < 0) return label;
  String host = siteNormalize(label);
  if (!host.length() || host.indexOf('.') < 0) return label;
  String baseDomain = siteBase(host);                          // "battle.net", "bbc.co.uk"
  int dot = baseDomain.indexOf('.');
  if (dot <= 0) return label;
  String name = baseDomain.substring(0, dot), suffix = baseDomain.substring(dot + 1);
  bool dropSuffix = suffix == "com" || suffix == "org" || suffix == "edu" || suffix == "gov"
                 || suffix.indexOf('.') >= 0 || suffix.length() == 2;
  return dropSuffix ? name : name + suffix;
}

static uint32_t fnv1aHash(const String &text) {
  uint32_t hash = 2166136261u;
  for (int k = 0; k < (int)text.length(); k++) hash = (hash ^ (uint8_t)text[k]) * 16777619u;
  return hash;
}

// Import a browser password CSV (Google/Chrome: name,url,username,password;
// Firefox: url,username,password; columns found from the header). One pass,
// bad rows skipped and counted, vault rewritten once at the end.
// Returns entries imported, or -1 if there's no .CSV on the drive.
static int mscImportCsv() {
  String newVault; int imported = 0, totalRows = 0;
  mscCsvSkipped = 0;
  {                                                            // scope frees the CSV text before the commit
    String csv = mscReadFile("CSV");
    if (csv.length() == 0) return -1;
    int pos = 0, csvLength = csv.length();
    String header[12];
    int headerCount = csvRecord(csv, &pos, csvLength, header, 12);
    int nameColumn = csvColumn(header, headerCount, "name"), urlColumn = csvColumn(header, headerCount, "url");
    int userColumn = csvColumn(header, headerCount, "username"), passwordColumn = csvColumn(header, headerCount, "password");
    if (passwordColumn < 0) { urlColumn = 0; userColumn = 1; passwordColumn = 2; nameColumn = -1; }   // unknown header: Firefox order
    for (int k = 0; k < csvLength; k++) if (csv[k] == '\n') totalRows++;
    newVault.reserve(vaultBlob.length() + csvLength);
    newVault += vaultBlob;                                     // new rows after the vault; sort dedups (last wins)

    String row[12];
    // Hashes of labels/users used by this import, to number second accounts.
    static uint32_t usedLabelHash[VAULT_MAX], usedUserHash[VAULT_MAX];
    int usedCount = 0;
    auto column = [&](int index, int fieldCount) { return (index >= 0 && index < fieldCount) ? row[index] : String(""); };
    while (pos < csvLength) {
      int fieldCount = csvRecord(csv, &pos, csvLength, row, 12);
      if (fieldCount == 0) break;
      String password = column(passwordColumn, fieldCount); password.trim();
      String username = column(userColumn, fieldCount); username.trim();
      String label;
      if (nameColumn >= 0 && nameColumn < fieldCount) { label = row[nameColumn]; label.trim(); }
      if (label.length() == 0 && urlColumn >= 0 && urlColumn < fieldCount) label = csvHost(row[urlColumn]);
      label = csvSiteLabel(label);
      if (label.length() == 0) label = "import";
      if (password.length() == 0) { mscCsvSkipped++; continue; }
      if (vault.count + imported >= VAULT_MAX) { mscCsvSkipped++; continue; }   // full
      label.replace('\t', ' '); username.replace('\t', ' '); password.replace('\t', ' ');

      String site;                                             // android:// app logins have none
      if (urlColumn >= 0 && urlColumn < fieldCount) {
        String url = row[urlColumn]; url.trim(); url.toLowerCase();
        if (url.startsWith("http://") || url.startsWith("https://")) site = siteNormalize(url);
      }
      if (label.length() > VAULT_NAME_LEN) label = label.substring(0, VAULT_NAME_LEN);
      if (username.length() > VAULT_USER_LEN) username = username.substring(0, VAULT_USER_LEN);
      if (password.length() > VAULT_PW_LEN) password = password.substring(0, VAULT_PW_LEN);

      // Same label, different user = a second account: number it ("github.com 2").
      uint32_t userHash = fnv1aHash(username), labelHash = fnv1aHash(label);
      auto labelTakenByOtherUser = [&](uint32_t hash) {
        for (int m = 0; m < usedCount; m++) if (usedLabelHash[m] == hash && usedUserHash[m] != userHash) return true;
        return false;
      };
      if (labelTakenByOtherUser(labelHash)) {
        String baseLabel = label, candidate;
        for (int number = 2; number < 1000; number++) {
          String suffix = " " + String(number);
          candidate = baseLabel.substring(0, min((int)baseLabel.length(), VAULT_NAME_LEN - (int)suffix.length())) + suffix;
          if (!labelTakenByOtherUser(fnv1aHash(candidate))) break;
        }
        label = candidate; labelHash = fnv1aHash(label);
      }
      if (usedCount < VAULT_MAX) { usedLabelHash[usedCount] = labelHash; usedUserHash[usedCount] = userHash; usedCount++; }

      // Re-importing over an entry keeps its device settings.
      int tabCount = 0; bool pressEnter = false, favorite = false; int icon = 0;
      for (int k = 0; k < vault.count; k++) {
        const VaultEntry &existing = vault.entries[k];
        if (existing.kind == KIND_PW && label == existing.name) {
          tabCount = existing.tabCount; pressEnter = existing.pressEnter; favorite = existing.favorite; icon = existing.icon;
          break;
        }
      }
      newVault += pwLine(label.c_str(), username.c_str(), password.c_str(), tabCount, pressEnter, favorite, icon, site.c_str()) + "\n";
      imported++;
      if (mscProgress && (imported % 16 == 0)) mscProgress(imported, totalRows);
    }
  }
  if (mscProgress) mscProgress(imported, totalRows);
  if (imported > 0 && !vaultCommit(newVault)) { LogRef.println("  CSV: save failed, vault unchanged"); return 0; }
  reportSaveStage("serial log");
  LogRef.printf("  CSV: %d imported, %d skipped\n", imported, mscCsvSkipped);
  reportSaveStage("saved");
  return imported;
}

// ---- parsing the edited text files -----------------------------------------
// "*" to "********": the user left the stored value in place.
static bool isMasked(const String &value) {
  int maskLength = strlen(PW_MASK);
  return value.length() > 0 && value.indexOf('*') == 0
      && value == String(PW_MASK).substring(0, value.length() < maskLength ? value.length() : maskLength);
}

static int countTabs(const String &line) {
  int tabs = 0;
  for (int i = 0; i < (int)line.length(); i++) if (line[i] == '\t') tabs++;
  return tabs;
}

static int findEntryByName(int kind, const char *name) {
  for (int i = 0; i < vault.count; i++)
    if (vault.entries[i].kind == kind && !strcmp(vault.entries[i].name, name)) return i;
  return -1;
}

static bool isHeaderOrBlank(const String &line) {
  String trimmed = line; trimmed.trim();
  return !trimmed.length() || trimmed.startsWith("Label\t") || trimmed.startsWith("Label ");
}

// SECRETS.TXT -> password lines. Masked passwords, and omitted flags / site
// columns, keep the stored values.
static void parseSecretsText(const String &text, String &output) {
  forEachLine(text, [&](const String &line) {
    ParsedEntry parsed;
    if (!parseEntry(line, parsed) || parsed.isCard || parsed.isTotp) return true;
    int existing = findEntryByName(KIND_PW, parsed.label.c_str());
    if (isMasked(parsed.secret) && existing >= 0) parsed.secret = vault.entries[existing].secret;
    if (existing >= 0 && !parsed.pressEnter && !parsed.favorite && !parsed.icon && countTabs(line) < 4) {
      parsed.pressEnter = vault.entries[existing].pressEnter;
      parsed.favorite = vault.entries[existing].favorite;
      parsed.icon = vault.entries[existing].icon;
    }
    if (existing >= 0 && !parsed.site.length() && countTabs(line) < 5) parsed.site = vault.entries[existing].site;
    if (parsed.secret.length() && !isMasked(parsed.secret))
      output += pwLine(parsed.label.c_str(), parsed.username.c_str(), parsed.secret.c_str(), parsed.tabCount,
                       parsed.pressEnter, parsed.favorite, parsed.icon, siteNormalize(parsed.site).c_str()) + "\n";
    return true;
  });
}

// CARDS.TXT -> card lines. Fields split on any run of tabs/spaces (card
// fields never contain spaces). Masked number / "***" CVV keep stored values.
static void parseCardsText(const String &text, String &output) {
  forEachLine(text, [&](const String &line) {
    if (isHeaderOrBlank(line)) return true;
    String fields[4]; int fieldCount = 0;
    String current = "";
    int length = line.length();
    for (int k = 0; k <= length && fieldCount < 4; k++) {
      char ch = (k < length) ? (char)line[k] : ' ';
      if (ch == '\t' || ch == ' ') { if (current.length()) { fields[fieldCount++] = current; current = ""; } }
      else current += ch;
    }
    if (fieldCount < 3 || !fields[0].length()) return true;
    String number = fields[1], expiry = fields[2], cvv = (fieldCount > 3) ? fields[3] : "";
    int existing = findEntryByName(KIND_CARD, fields[0].c_str());
    if (isMasked(number)) number = existing >= 0 ? String(vault.entries[existing].secret) : String("");
    if ((cvv == "***" || cvv.length() == 0) && existing >= 0) cvv = vault.entries[existing].cvv;
    if (number.length()) output += cardLine(fields[0], number, expiry, cvv) + "\n";
    return true;
  });
}

#define TOTP_COLUMNS (5 + TOTP_MAX_BACKUPS)   // label, secret, digits, period, flags, backups

// Editors often save spaces where TABs were meant: a run of 2+ spaces becomes
// a TAB, a single space stays.
static String collapseSpaceRunsToTabs(const String &text) {
  String result = "";
  int spaces = 0;
  for (int k = 0; k <= (int)text.length(); k++) {
    char ch = k < (int)text.length() ? text[k] : 0;
    if (ch == ' ') { spaces++; continue; }
    if (spaces) { result += spaces >= 2 ? "\t" : " "; spaces = 0; }
    if (ch) result += ch;
  }
  return result;
}

// Split a 2FA line into columns. Returns the count; sets flagsGiven when the
// flags column was present.
static int splitTotpLine(String line, String fields[TOTP_COLUMNS], bool &flagsGiven) {
  int fieldCount = 0;
  flagsGiven = false;
  line = collapseSpaceRunsToTabs(line);
  // Single spaces only: "Label Secret Digits Period ..." splits on spaces when the 3rd word is a number.
  if (line.indexOf('\t') < 0 && line.indexOf("otpauth://") < 0) {
    int space1 = line.indexOf(' ');
    int space2 = space1 < 0 ? -1 : line.indexOf(' ', space1 + 1);
    int space3 = space2 < 0 ? -1 : line.indexOf(' ', space2 + 1);
    if (space2 > 0 && isAllDigits(line.substring(space2 + 1, space3 < 0 ? line.length() : space3))) line.replace(' ', '\t');
  }
  if (line.indexOf('\t') >= 0) {
    int pos = 0;
    while (fieldCount < TOTP_COLUMNS) {
      int tab = line.indexOf('\t', pos);
      if (tab == -1) { fields[fieldCount++] = line.substring(pos); break; }
      fields[fieldCount++] = line.substring(pos, tab);
      pos = tab + 1;
    }
    for (int k = 0; k < fieldCount; k++) fields[k].trim();
    if (fields[0].startsWith("otpauth://")) {                  // link with no label: shift right
      for (int k = fieldCount < TOTP_COLUMNS ? fieldCount : TOTP_COLUMNS - 1; k > 0; k--) fields[k] = fields[k - 1];
      fields[0] = "";
      if (fieldCount < TOTP_COLUMNS) fieldCount++;
    }
    flagsGiven = fieldCount >= 5;
    // Flags never contain spaces: words after them in the flags column are backup codes.
    if (fieldCount >= 5 && fields[4].indexOf(' ') >= 0) {
      String words[TOTP_COLUMNS]; int wordCount = 0, pos2 = 0;
      String flagsColumn = fields[4];
      while (wordCount < TOTP_COLUMNS && pos2 < (int)flagsColumn.length()) {
        int space = flagsColumn.indexOf(' ', pos2);
        if (space < 0) space = flagsColumn.length();
        if (space > pos2) words[wordCount++] = flagsColumn.substring(pos2, space);
        pos2 = space + 1;
      }
      bool firstWordIsFlags = wordCount > 0;
      for (int k = 0; k < (int)words[0].length() && firstWordIsFlags; k++)
        firstWordIsFlags = strchr("EeFfIi0123456789", words[0][k]) != nullptr;
      String laterColumns[TOTP_COLUMNS]; int laterCount = 0;
      for (int k = 5; k < fieldCount; k++) laterColumns[laterCount++] = fields[k];
      fieldCount = 4;
      fields[fieldCount++] = firstWordIsFlags ? words[0] : "";
      for (int k = firstWordIsFlags ? 1 : 0; k < wordCount && fieldCount < TOTP_COLUMNS; k++) fields[fieldCount++] = words[k];
      for (int k = 0; k < laterCount && fieldCount < TOTP_COLUMNS; k++) fields[fieldCount++] = laterColumns[k];
    }
  } else {                                                     // no tabs: "[Label ]otpauth://..." or "Label Secret"
    int linkAt = line.indexOf("otpauth://");
    if (linkAt >= 0) {
      fields[0] = line.substring(0, linkAt); fields[0].trim();
      fields[1] = line.substring(linkAt);
      fieldCount = 2;
    } else {
      int space = line.indexOf(' ');
      if (space > 0) {
        fields[0] = line.substring(0, space);
        fields[1] = line.substring(space + 1); fields[1].trim();
        fieldCount = 2;
      }
    }
  }
  return fieldCount;
}

// 2FA.TXT -> 2FA lines: Label<TAB>Secret[<TAB>Digits<TAB>Period<TAB>Flags<TAB>Backup1..5].
// The secret may be an otpauth:// link (label then optional). Masked
// secret/backups keep stored values; an empty backup column drops that code.
static void parseTotpText(const String &text, String &output) {
  int skippedLines = 0;
  forEachLine(text, [&](const String &line) {
    if (isHeaderOrBlank(line)) return true;
    String trimmed = line; trimmed.trim();
    String fields[TOTP_COLUMNS]; bool flagsGiven;
    int fieldCount = splitTotpLine(trimmed, fields, flagsGiven);

    if (fieldCount >= 2 && fields[1].startsWith("otpauth://")) {   // link supplies secret, label and defaults
      int digits, period; String linkLabel, linkSecret;
      if (!totpParseUri(fields[1], linkLabel, linkSecret, digits, period)) {
        skippedLines++;
        LogRef.printf("  2FA: bad or non-SHA1 link skipped\n");
        return true;
      }
      if (!fields[0].length()) fields[0] = linkLabel;
      fields[1] = linkSecret;
      if (fieldCount < 3 || !fields[2].length()) fields[2] = String(digits);
      if (fieldCount < 4 || !fields[3].length()) fields[3] = String(period);
      if (fieldCount < 4) fieldCount = 4;
    }
    if (fieldCount < 2 || fields[0].length() == 0) { skippedLines++; return true; }

    fields[0].replace('\t', ' ');
    if (fields[0].length() > VAULT_NAME_LEN) fields[0] = fields[0].substring(0, VAULT_NAME_LEN);
    int existing = findEntryByName(KIND_TOTP, fields[0].c_str());
    char secret[VAULT_PW_LEN + 1] = "";
    bool secretOk;
    if (isMasked(fields[1])) {
      secretOk = existing >= 0;
      if (secretOk) strncpy(secret, vault.entries[existing].secret, VAULT_PW_LEN);
    } else {
      secretOk = totpNormalize(fields[1].c_str(), secret, sizeof(secret));
    }
    if (secretOk) {
      bool pressEnter = false, favorite = false; int icon = 0;
      if (flagsGiven) {                                        // reuse the password flag parser
        ParsedEntry flagsOnly;
        parseEntry(String("x\tu\tp\t0\t") + fields[4], flagsOnly);
        pressEnter = flagsOnly.pressEnter; favorite = flagsOnly.favorite; icon = flagsOnly.icon;
      } else if (existing >= 0) {
        pressEnter = vault.entries[existing].pressEnter; favorite = vault.entries[existing].favorite; icon = vault.entries[existing].icon;
      }
      String storedCodes[TOTP_MAX_BACKUPS], codes[TOTP_MAX_BACKUPS];
      int storedCount = existing >= 0 ? totpBackups(existing, storedCodes) : 0, codeCount = 0;
      for (int k = 0; k < TOTP_MAX_BACKUPS && 5 + k < fieldCount; k++) {
        String code = fields[5 + k];
        if (isMasked(code)) { if (k < storedCount) codes[codeCount++] = storedCodes[k]; }
        else if (code.length()) codes[codeCount++] = code.substring(0, TOTP_BACKUP_LEN);
      }
      output += totpLine(fields[0].c_str(), secret, totpDigits(fieldCount > 2 ? fields[2].c_str() : ""),
                         totpPeriod(fieldCount > 3 ? fields[3].c_str() : ""), pressEnter, favorite, icon, codes, codeCount) + "\n";
    } else {
      skippedLines++;
      LogRef.printf("  2FA: '%s' skipped (secret is not base32, or longer than %d characters)\n", fields[0].c_str(), VAULT_PW_LEN);
    }
    memset(secret, 0, sizeof(secret));
    return true;
  });
  if (skippedLines) LogRef.printf("  2FA: %d line(s) skipped\n", skippedLines);
}

// Read the three text files back out of the host-edited image and save the
// result if it changed. Refuses to replace a vault with nothing.
static bool mscParseVault() {
  String newVault = "";
  String secretsText = mscReadSig(SECRETS_SIG);
  String cardsText = mscReadSig(CARDS_SIG);
  String totpText = mscReadSig(TOTP_SIG);
  if (secretsText.length()) parseSecretsText(secretsText, newVault);
  if (cardsText.length()) parseCardsText(cardsText, newVault);
  if (totpText.length()) parseTotpText(totpText, newVault);
  String sorted = sortAllEntries(newVault);
  if (sorted == vaultBlob) { LogRef.println("  edit: unchanged"); return true; }
  if (sorted.length() == 0 && vaultBlob.length() > 0) { LogRef.println("  edit: empty - kept vault"); return false; }
  return vaultCommit(sorted);
}

// Commit an edit session: a dropped CSV wins (import), else the edited text
// files. *imported = CSV count, or -1 for a text-file edit.
static bool mscCommitEdit(int *imported) {
  int csvImported = mscImportCsv();
  if (csvImported >= 0) { *imported = csvImported; return csvImported > 0; }
  *imported = -1;
  return mscParseVault();
}

// ---- TinyUSB callbacks
static int32_t mscRead(uint32_t block, void *buffer, uint32_t length) {
  if (block >= MSC_BLOCKS) return -1;
  memcpy(buffer, mscDisk[block], length);
  return length;
}
static volatile uint32_t mscWrites = 0;
static int32_t mscWrite(uint32_t block, uint8_t *buffer, uint32_t length) {
  if (block >= MSC_BLOCKS) return -1;
  memcpy(mscDisk[block], buffer, length);
  mscLastWrite = millis(); mscWrites++;
  return length;
}
static void mscFlush() {}
static bool mscReadyCallback() { return mscReady; }
static bool mscStartStop(uint8_t, bool start, bool loadEject) { if (loadEject && !start) mscEjected = true; return true; }

static void mscBegin() {
  usb_msc.setID("Nulenke", "Lomenett Vault", "1.0");
  usb_msc.setCapacity(MSC_BLOCKS, MSC_BLOCK);
  usb_msc.setReadWriteCallback(mscRead, mscWrite, mscFlush);
  usb_msc.setReadyCallback(mscReadyCallback);
  usb_msc.setStartStopCallback(mscStartStop);
  usb_msc.setUnitReady(false);
  usb_msc.begin();
}
static void mscReenumerate() { if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(150); TinyUSBDevice.attach(); } }
static void mscEnterBuilt() { mscLastWrite = 0; mscEjected = false; mscReady = true; usb_msc.setUnitReady(true); mscReenumerate(); }
static void mscEnter() { mscBuildImage(); mscEnterBuilt(); }
static void mscLeave() { mscReady = false; usb_msc.setUnitReady(false); mscReenumerate(); }
