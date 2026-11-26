// vault.h -- the decrypted vault in RAM (parsed from /lock.bin after login),
// list filtering, and website matching. Storage/crypto is in secure.h.
#pragma once
#include "secure.h"
#include <utility>

#define VAULT_MAX      512
#define VAULT_NAME_LEN 23
#define VAULT_USER_LEN 31
#define VAULT_PW_LEN   TOTP_SECRET_LEN      // the secret field also holds 2FA secrets
#define VAULT_SITE_LEN 47                   // website host, e.g. accounts.google.com

// Entry kinds
#define KIND_PW   0
#define KIND_CARD 1
#define KIND_TOTP 2

struct VaultEntry {
  char    name[VAULT_NAME_LEN + 1];
  char    username[VAULT_USER_LEN + 1];
  char    secret[VAULT_PW_LEN + 1];         // password, card number, or base32 2FA secret
  uint8_t tabCount;                         // TABs typed between username and password
  uint8_t kind;                             // KIND_PW / KIND_CARD / KIND_TOTP
  char    expiry[6];                        // card expiry, or 2FA digit count
  char    cvv[5];                           // card CVV, or 2FA period (seconds)
  uint8_t pressEnter, favorite, icon;
  uint8_t backupCodeCount;                  // 2FA backup codes (the codes stay in vaultBlob)
  char    site[VAULT_SITE_LEN + 1];
};
struct VaultStore { int count; VaultEntry entries[VAULT_MAX]; };
static VaultStore vault;
static String vaultBlob;                    // sorted plaintext lines, as stored in /lock.bin

// Call `handleLine(line)` for each '\n'-separated line of text.
template <typename LineHandler>
static void forEachLine(const String &text, LineHandler handleLine) {
  int pos = 0, length = text.length();
  while (pos <= length) {
    int lineEnd = text.indexOf('\n', pos);
    if (lineEnd == -1) lineEnd = length;
    String line = text.substring(pos, lineEnd);
    pos = lineEnd + 1;
    if (!handleLine(line)) return;
    if (pos > length) break;
  }
}

static void copyField(char *dest, const String &source, int maxLength) {
  strncpy(dest, source.c_str(), maxLength);
  dest[maxLength] = 0;
}

static void vaultParseBlob() {
  vault.count = 0;
  forEachLine(vaultBlob, [&](const String &line) {
    if (vault.count >= VAULT_MAX) return false;
    ParsedEntry parsed;
    if (parseEntry(line, parsed)) {
      VaultEntry &entry = vault.entries[vault.count++];
      copyField(entry.name, parsed.label, VAULT_NAME_LEN);
      copyField(entry.username, parsed.username, VAULT_USER_LEN);
      copyField(entry.secret, parsed.secret, VAULT_PW_LEN);
      entry.tabCount = parsed.tabCount;
      entry.kind = parsed.isCard ? KIND_CARD : parsed.isTotp ? KIND_TOTP : KIND_PW;
      entry.pressEnter = parsed.pressEnter ? 1 : 0;
      entry.favorite = parsed.favorite ? 1 : 0;
      entry.icon = parsed.icon;
      entry.backupCodeCount = parsed.backupCodeCount;
      copyField(entry.site, parsed.site, VAULT_SITE_LEN);
      copyField(entry.expiry, parsed.expiry, 5);
      copyField(entry.cvv, parsed.cvv, 4);
    }
    return true;
  });
}

static String flagsField(bool pressEnter, bool favorite, int icon) {
  String flags;
  if (pressEnter) flags += "E";
  if (favorite) flags += "F";
  if (icon) flags += "I" + String(icon);
  return flags;
}

// Password line: label<TAB>user<TAB>pw[<TAB>tabs[<TAB>flags[<TAB>site]]]
// (tabs and flags columns are written whenever a later column follows)
static String pwLine(const char *name, const char *username, const char *password, int tabCount,
                     bool pressEnter, bool favorite, int icon = 0, const char *site = "") {
  String line = String(name) + "\t" + username + "\t" + password;
  bool hasFlags = pressEnter || favorite || icon;
  bool hasSite = site && *site;
  if (tabCount || hasFlags || hasSite) line += "\t" + String(tabCount);
  if (hasFlags || hasSite) line += "\t" + flagsField(pressEnter, favorite, icon);
  if (hasSite) { line += "\t"; line += site; }
  return line;
}

// 2FA line: label<TAB>$TOTP<TAB>secret<TAB>digits<TAB>period[<TAB>flags[<TAB>backup...]]
static String totpLine(const char *name, const char *secret, int digits, int period, bool pressEnter, bool favorite,
                       int icon = 0, const String *backupCodes = nullptr, int backupCodeCount = 0) {
  String line = String(name) + "\t$TOTP\t" + secret + "\t" + String(digits) + "\t" + String(period);
  if (pressEnter || favorite || icon || backupCodeCount) line += "\t" + flagsField(pressEnter, favorite, icon);
  for (int k = 0; k < backupCodeCount; k++) line += "\t" + backupCodes[k];
  return line;
}

static String cardLine(const String &name, const String &number, const String &expiry, const String &cvv) {
  return name + "\t$CARD\t" + number + "\t" + expiry + "\t" + cvv;
}

// Rewrite every line in canonical form (old space-format lines included).
static String normalizeLines(const String &raw) {
  String normalized = "";
  forEachLine(raw, [&](const String &line) {
    ParsedEntry parsed;
    if (parseEntry(line, parsed)) {
      if (parsed.isCard)
        normalized += cardLine(parsed.label, parsed.secret, parsed.expiry, parsed.cvv) + "\n";
      else if (parsed.isTotp)
        normalized += totpLine(parsed.label.c_str(), parsed.secret.c_str(), parsed.expiry.toInt(), parsed.cvv.toInt(),
                               parsed.pressEnter, parsed.favorite, parsed.icon, parsed.backupCodes, parsed.backupCodeCount) + "\n";
      else
        normalized += pwLine(parsed.label.c_str(), parsed.username.c_str(), parsed.secret.c_str(), parsed.tabCount,
                             parsed.pressEnter, parsed.favorite, parsed.icon, parsed.site.c_str()) + "\n";
    }
    return true;
  });
  return normalized;
}

// After login: decrypt /lock.bin (create it empty if missing), sort, parse.
static void vaultLoad() {
  if (!LittleFS.exists("/lock.bin")) encryptToFile("/lock.bin", "");
  vaultBlob = sortAllEntries(normalizeLines(decryptFromFile("/lock.bin")));
  vaultParseBlob();
}

// Replace the whole vault with these lines (unsorted, header allowed).
static bool vaultCommit(const String &rawLines) {
  reportSaveStage("tidying lines");
  String normalized = normalizeLines(rawLines);
  reportSaveStage("sorting");
  String sorted = sortAllEntries(normalized);
  normalized = String();
  if (!encryptToFile("/lock.bin", sorted)) return false;
  reportSaveStage("loading list");
  vaultBlob = std::move(sorted);              // move: avoids a second vault-sized buffer
  vaultParseBlob();
  return true;
}

static bool vaultAdd(const char *name, const char *password, const char *username = "") {
  if (!*name || !*password) return false;
  String updated = vaultBlob;
  sortedInsert(updated, String(name) + "\t" + username + "\t" + password);
  return vaultCommit(updated);
}

static bool vaultAddCard(const char *name, const char *number, const char *expiry, const char *cvv) {
  if (!*name || !*number) return false;
  String updated = vaultBlob;
  sortedInsert(updated, cardLine(name, number, expiry, cvv));
  return vaultCommit(updated);
}

static bool vaultAddTotp(const char *name, const char *secret, int digits = 6, int period = 30) {
  char normalizedSecret[VAULT_PW_LEN + 1];
  if (!*name || !totpNormalize(secret, normalizedSecret, sizeof(normalizedSecret))) return false;
  String updated = vaultBlob;
  sortedInsert(updated, totpLine(name, normalizedSecret, digits, period, false, false));
  memset(normalizedSecret, 0, sizeof(normalizedSecret));
  return vaultCommit(updated);
}

// Delete the entry at index (counting non-empty lines of vaultBlob).
static bool vaultDel(int index) {
  if (index < 0 || index >= vault.count) return false;
  String remaining = "";
  int lineNumber = 0;
  forEachLine(vaultBlob, [&](const String &line) {
    if (line.length()) { if (lineNumber++ != index) remaining += line + "\n"; }
    return true;
  });
  return vaultCommit(remaining);
}

// Backup codes of 2FA entry `index`, read from its stored line. Returns the count.
static int totpBackups(int index, String codes[TOTP_MAX_BACKUPS]) {
  if (index < 0 || index >= vault.count) return 0;
  const VaultEntry &entry = vault.entries[index];
  if (entry.kind != KIND_TOTP || !entry.backupCodeCount) return 0;
  int found = 0;
  ParsedEntry parsed;
  forEachLine(vaultBlob, [&](const String &line) {
    if (lineIsTotp(line) && lineLabel(line) == entry.name && parseEntry(line, parsed)) {
      for (int k = 0; k < parsed.backupCodeCount; k++) codes[k] = parsed.backupCodes[k];
      found = parsed.backupCodeCount;
      return false;
    }
    return true;
  });
  return found;
}

static void vaultForget() { vaultBlob = ""; vault.count = 0; }

// Rebuild the stored text from the in-RAM entries (after an on-device edit).
static bool vaultRewriteFromEntries() {
  String rebuilt = "";
  for (int i = 0; i < vault.count; i++) {
    VaultEntry &entry = vault.entries[i];
    if (entry.kind == KIND_CARD) {
      rebuilt += cardLine(entry.name, entry.secret, entry.expiry, entry.cvv) + "\n";
    } else if (entry.kind == KIND_TOTP) {
      String codes[TOTP_MAX_BACKUPS];
      int codeCount = totpBackups(i, codes);
      rebuilt += totpLine(entry.name, entry.secret, totpDigits(entry.expiry), totpPeriod(entry.cvv),
                          entry.pressEnter, entry.favorite, entry.icon, codes, codeCount) + "\n";
    } else {
      rebuilt += pwLine(entry.name, entry.username, entry.secret, entry.tabCount,
                        entry.pressEnter, entry.favorite, entry.icon, entry.site) + "\n";
    }
  }
  return vaultCommit(rebuilt);
}

static bool vaultSetPw(int index, const char *password) {
  if (index < 0 || index >= vault.count || !*password) return false;
  copyField(vault.entries[index].secret, password, VAULT_PW_LEN);
  return vaultRewriteFromEntries();
}

// ---- list UI state shared with the sketch ---------------------------------
struct VaultUi {
  bool isTyping = false;           // the sketch is typing an entry over USB
  int  charsTyped = 0;
  int  charsTotal = 0;
  bool showLetterRing = true;      // portrait: A-Z ring before the entries
  MenuState letterMenu;
};
static VaultUi vaultUi;

// Indexes of entries of one kind: favourites first (not for cards), then the rest.
static int vaultFilter(int kind, int *indexes) {
  int count = 0;
  bool favoritesFirst = kind != KIND_CARD;
  if (favoritesFirst)
    for (int i = 0; i < vault.count; i++)
      if (vault.entries[i].kind == kind && vault.entries[i].favorite) indexes[count++] = i;
  for (int i = 0; i < vault.count; i++)
    if (vault.entries[i].kind == kind && !(favoritesFirst && vault.entries[i].favorite)) indexes[count++] = i;
  return count;
}

// ---- website matching (browser extension sends "site <host>") --------------
// Lower-case host without scheme, user@, port, path, query or leading "www.".
// Over-long hosts lose labels from the front (matching compares the end).
static String siteNormalize(String host) {
  host.trim(); host.toLowerCase();
  int schemeEnd = host.indexOf("://");  if (schemeEnd >= 0) host = host.substring(schemeEnd + 3);
  int pathStart = host.indexOf('/');    if (pathStart >= 0) host = host.substring(0, pathStart);
  int queryStart = host.indexOf('?');   if (queryStart >= 0) host = host.substring(0, queryStart);
  int userEnd = host.lastIndexOf('@');  if (userEnd >= 0) host = host.substring(userEnd + 1);
  int portStart = host.indexOf(':');    if (portStart >= 0) host = host.substring(0, portStart);
  if (host.startsWith("www.")) host = host.substring(4);
  while (host.endsWith(".")) host.remove(host.length() - 1);
  for (int i = 0; i < (int)host.length(); i++) {
    char ch = host[i];
    if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_')) return "";
  }
  while ((int)host.length() > VAULT_SITE_LEN) {
    int firstDot = host.indexOf('.');
    if (firstDot < 0) return "";
    host = host.substring(firstDot + 1);
  }
  return host;
}

// Registrable domain: "accounts.google.com" -> "google.com", "shop.bbc.co.uk" ->
// "bbc.co.uk" (short label under a 2-letter country code counts as suffix).
static String siteBase(const String &host) {
  int lastDot = host.lastIndexOf('.');
  if (lastDot < 0) return host;
  int previousDot = host.lastIndexOf('.', lastDot - 1);
  if (previousDot < 0) return host;
  bool countryCodeSuffix = host.length() - lastDot - 1 == 2 && lastDot - previousDot - 1 <= 3;
  if (countryCodeSuffix) {
    int thirdDot = host.lastIndexOf('.', previousDot - 1);
    return thirdDot < 0 ? host : host.substring(thirdDot + 1);
  }
  return host.substring(previousDot + 1);
}

static bool siteIsSubdomainOf(const String &host, const String &parent) {
  return host.length() > parent.length() && host.endsWith(parent)
      && host[host.length() - parent.length() - 1] == '.';
}

// How well a password entry fits the page host (already normalised):
// 3 same host, 2 subdomain either way, 1 same base domain (or, with no site
// stored, a label like "GitHub" for github.com), 0 no match.
static int siteScore(const VaultEntry &entry, const String &host, const String &baseDomain) {
  String site = entry.site[0] ? siteNormalize(entry.site) : String("");
  if (!site.length()) {                                     // no site: try the label
    int labelLength = strlen(entry.name), cut = labelLength;
    while (cut > 0 && isdigit((unsigned char)entry.name[cut - 1])) cut--;
    if (cut < labelLength && cut > 1 && entry.name[cut - 1] == ' ') labelLength = cut - 1;   // "github.com 2" -> "github.com"
    String label;
    for (int k = 0; k < labelLength; k++)
      if (entry.name[k] != ' ') label += (char)tolower((unsigned char)entry.name[k]);
    if (label.indexOf('.') >= 0) {
      site = siteNormalize(label);
    } else {
      int dot = baseDomain.indexOf('.');
      return (dot > 0 && label.length() && label == baseDomain.substring(0, dot)) ? 1 : 0;
    }
  }
  if (!site.length()) return 0;
  if (site == host) return 3;
  if (siteIsSubdomainOf(host, site) || siteIsSubdomainOf(site, host)) return 2;
  return siteBase(site) == baseDomain ? 1 : 0;
}

// Password entries for this host, best first (favourites first among equals,
// then alphabetical). Writes at most `capacity` indexes; returns the count.
static int vaultMatchSite(const char *pageHost, int *matches, int capacity) {
  String host = siteNormalize(pageHost);
  if (!host.length()) return 0;
  String baseDomain = siteBase(host);
  int count = 0;
  static uint8_t rank[VAULT_MAX];
  for (int i = 0; i < vault.count; i++) {
    if (vault.entries[i].kind != KIND_PW) continue;
    int score = siteScore(vault.entries[i], host, baseDomain);
    if (!score) continue;
    rank[i] = score * 2 + (vault.entries[i].favorite ? 1 : 0);
    if (count < capacity) matches[count++] = i;
    else if (rank[matches[capacity - 1]] < rank[i]) matches[capacity - 1] = i;   // full: replace the worst
    else continue;
    for (int j = count - 1; j > 0 && rank[matches[j - 1]] < rank[matches[j]]; j--) {   // stable insertion
      int swap = matches[j]; matches[j] = matches[j - 1]; matches[j - 1] = swap;
    }
  }
  return count;
}
