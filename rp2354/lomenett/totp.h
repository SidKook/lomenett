// totp.h -- 2FA codes (RFC 6238 TOTP / RFC 4226 HOTP, HMAC-SHA1) and the wall
// clock they need. No RTC: the host sends "time <unix>" each session and
// millis() carries it. No clock = no codes.
// Secrets are canonical base32 (A-Z2-7, no padding). SHA1 lives here because
// the Crypto library lacks it.
#pragma once

// ---- SHA1 / HMAC-SHA1 -------------------------------------------------------
struct Sha1 {
  uint32_t state[5];
  uint8_t  block[64];
  uint32_t blockLength;
  uint64_t totalBytes;
};

static inline uint32_t sha1RotateLeft(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

static void sha1ProcessBlock(Sha1 &ctx, const uint8_t *block) {
  uint32_t schedule[80];
  for (int i = 0; i < 16; i++)
    schedule[i] = (uint32_t)block[4*i] << 24 | (uint32_t)block[4*i+1] << 16 | (uint32_t)block[4*i+2] << 8 | block[4*i+3];
  for (int i = 16; i < 80; i++)
    schedule[i] = sha1RotateLeft(schedule[i-3] ^ schedule[i-8] ^ schedule[i-14] ^ schedule[i-16], 1);

  uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3], e = ctx.state[4];
  for (int i = 0; i < 80; i++) {
    uint32_t mix, constant;
    if (i < 20)      { mix = (b & c) | (~b & d);          constant = 0x5A827999; }
    else if (i < 40) { mix = b ^ c ^ d;                   constant = 0x6ED9EBA1; }
    else if (i < 60) { mix = (b & c) | (b & d) | (c & d); constant = 0x8F1BBCDC; }
    else             { mix = b ^ c ^ d;                   constant = 0xCA62C1D6; }
    uint32_t temp = sha1RotateLeft(a, 5) + mix + e + constant + schedule[i];
    e = d; d = c; c = sha1RotateLeft(b, 30); b = a; a = temp;
  }
  ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d; ctx.state[4] += e;
  memset(schedule, 0, sizeof(schedule));
}

static void sha1Init(Sha1 &ctx) {
  ctx.state[0] = 0x67452301; ctx.state[1] = 0xEFCDAB89; ctx.state[2] = 0x98BADCFE;
  ctx.state[3] = 0x10325476; ctx.state[4] = 0xC3D2E1F0;
  ctx.blockLength = 0; ctx.totalBytes = 0;
}

static void sha1Update(Sha1 &ctx, const uint8_t *data, int length) {
  ctx.totalBytes += length;
  while (length > 0) {
    int chunk = 64 - ctx.blockLength;
    if (chunk > length) chunk = length;
    memcpy(ctx.block + ctx.blockLength, data, chunk);
    ctx.blockLength += chunk; data += chunk; length -= chunk;
    if (ctx.blockLength == 64) { sha1ProcessBlock(ctx, ctx.block); ctx.blockLength = 0; }
  }
}

static void sha1Final(Sha1 &ctx, uint8_t digest[20]) {
  uint64_t totalBits = ctx.totalBytes * 8;
  uint8_t marker = 0x80, zero = 0;
  sha1Update(ctx, &marker, 1);
  while (ctx.blockLength != 56) sha1Update(ctx, &zero, 1);
  uint8_t lengthBytes[8];
  for (int i = 0; i < 8; i++) lengthBytes[i] = totalBits >> (56 - 8 * i);
  sha1Update(ctx, lengthBytes, 8);
  for (int i = 0; i < 5; i++) {
    digest[4*i] = ctx.state[i] >> 24; digest[4*i+1] = ctx.state[i] >> 16;
    digest[4*i+2] = ctx.state[i] >> 8; digest[4*i+3] = ctx.state[i];
  }
  memset(&ctx, 0, sizeof(ctx));
}

static void hmacSha1(const uint8_t *key, int keyLength, const uint8_t *message, int messageLength, uint8_t mac[20]) {
  uint8_t paddedKey[64] = {0}, pad[64];
  Sha1 ctx;
  if (keyLength > 64) { sha1Init(ctx); sha1Update(ctx, key, keyLength); sha1Final(ctx, paddedKey); }
  else memcpy(paddedKey, key, keyLength);

  for (int i = 0; i < 64; i++) pad[i] = paddedKey[i] ^ 0x36;    // inner
  sha1Init(ctx); sha1Update(ctx, pad, 64); sha1Update(ctx, message, messageLength); sha1Final(ctx, mac);
  for (int i = 0; i < 64; i++) pad[i] = paddedKey[i] ^ 0x5C;    // outer
  sha1Init(ctx); sha1Update(ctx, pad, 64); sha1Update(ctx, mac, 20); sha1Final(ctx, mac);
  memset(paddedKey, 0, sizeof(paddedKey)); memset(pad, 0, sizeof(pad));
}

// ---- base32 -----------------------------------------------------------------
// Clean a typed secret: upper-case; drop spaces, dashes, '=' padding.
// False on any other character, if it won't fit, or if under 8 chars.
static bool totpNormalize(const char *input, char *output, int capacity) {
  int length = 0;
  for (; *input; input++) {
    char ch = *input;
    if (ch == ' ' || ch == '-' || ch == '=' || ch == '\t' || ch == '\r' || ch == '\n') continue;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7'))) return false;
    if (length >= capacity - 1) return false;
    output[length++] = ch;
  }
  output[length] = 0;
  return length >= 8;
}

// Decode canonical base32; returns byte count or -1.
static int base32Decode(const char *text, uint8_t *output, int capacity) {
  uint32_t bitBuffer = 0; int bitsHeld = 0, length = 0;
  for (; *text; text++) {
    char ch = *text; int value;
    if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
    else if (ch >= '2' && ch <= '7') value = ch - '2' + 26;
    else return -1;
    bitBuffer = (bitBuffer << 5) | value; bitsHeld += 5;
    if (bitsHeld >= 8) {
      if (length >= capacity) return -1;
      output[length++] = bitBuffer >> (bitsHeld - 8);
      bitsHeld -= 8;
    }
  }
  return length;
}

// ---- HOTP / TOTP ------------------------------------------------------------
static uint32_t hotp(const uint8_t *key, int keyLength, uint64_t counter, int digits) {
  uint8_t counterBytes[8], mac[20];
  for (int i = 0; i < 8; i++) counterBytes[i] = counter >> (56 - 8 * i);
  hmacSha1(key, keyLength, counterBytes, 8, mac);
  int offset = mac[19] & 0x0F;
  uint32_t truncated = ((uint32_t)(mac[offset] & 0x7F) << 24) | ((uint32_t)mac[offset+1] << 16)
                     | ((uint32_t)mac[offset+2] << 8) | mac[offset+3];
  memset(mac, 0, sizeof(mac));
  uint32_t modulus = 1;
  for (int i = 0; i < digits; i++) modulus *= 10;
  return truncated % modulus;
}

static int totpDigits(const char *text) { int digits = atoi(text); return (digits >= 6 && digits <= 8) ? digits : 6; }
static int totpPeriod(const char *text) { int period = atoi(text); return (period >= 10 && period <= 300) ? period : 30; }

// Code for a base32 secret at unixSeconds, written to output (digits + NUL).
static bool totpCode(const char *base32Secret, int digits, int period, uint64_t unixSeconds, char *output) {
  uint8_t key[64];
  int keyLength = base32Decode(base32Secret, key, sizeof(key));
  if (keyLength <= 0) { output[0] = 0; return false; }
  uint32_t code = hotp(key, keyLength, unixSeconds / period, digits);
  memset(key, 0, sizeof(key));
  snprintf(output, 10, "%0*lu", digits, (unsigned long)code);
  return true;
}

// ---- wall clock -------------------------------------------------------------
static uint64_t totpSyncUnix = 0;     // unix seconds at last sync, 0 = never
static uint32_t totpSyncMs = 0;       // millis() at that moment
static inline bool totpClockValid() { return totpSyncUnix != 0; }
static inline uint64_t totpClockNow() { return totpSyncUnix + (uint32_t)(millis() - totpSyncMs) / 1000; }
// Accepts 2024..2100 only.
static bool totpClockSet(uint64_t unixSeconds) {
  if (unixSeconds < 1704067200ULL || unixSeconds > 4102444800ULL) return false;
  totpSyncUnix = unixSeconds; totpSyncMs = millis(); return true;
}
static inline int totpSecondsLeft(int period) { return period - (int)(totpClockNow() % period); }

// ---- otpauth:// links -------------------------------------------------------
// otpauth://totp/Issuer:account?secret=X&issuer=Issuer&digits=6&period=30
// Label = issuer param, else text before ':', else the account. SHA1 only.
static String urlDecode(const String &text) {
  String decoded;
  auto hexValue = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10; };
  for (int i = 0; i < text.length(); i++) {
    char ch = text[i];
    if (ch == '%' && i + 2 < text.length()) { decoded += (char)(hexValue(text[i+1]) * 16 + hexValue(text[i+2])); i += 2; }
    else if (ch == '+') decoded += ' ';
    else decoded += ch;
  }
  return decoded;
}

static bool totpParseUri(const String &uri, String &label, String &secret, int &digits, int &period) {
  int prefixAt = uri.indexOf("otpauth://totp/");
  if (prefixAt < 0) return false;
  String rest = uri.substring(prefixAt + 15); rest.trim();
  int queryAt = rest.indexOf('?');
  String path  = urlDecode(queryAt < 0 ? rest : rest.substring(0, queryAt));
  String query = queryAt < 0 ? String("") : rest.substring(queryAt + 1);

  String issuer = "", algorithm = "";
  secret = ""; digits = 6; period = 30;
  int pos = 0;
  while (pos <= query.length()) {
    int ampersand = query.indexOf('&', pos);
    if (ampersand < 0) ampersand = query.length();
    String pair = query.substring(pos, ampersand);
    int equals = pair.indexOf('=');
    if (equals > 0) {
      String key = pair.substring(0, equals), value = urlDecode(pair.substring(equals + 1));
      if (key == "secret") secret = value;
      else if (key == "issuer") issuer = value;
      else if (key == "algorithm") algorithm = value;
      else if (key == "digits") digits = totpDigits(value.c_str());
      else if (key == "period") period = totpPeriod(value.c_str());
    }
    pos = ampersand + 1;
  }
  if (algorithm.length() && !(algorithm == "SHA1" || algorithm == "sha1")) return false;

  int colon = path.indexOf(':');
  if (!issuer.length() && colon > 0) issuer = path.substring(0, colon);
  label = issuer.length() ? issuer : (colon >= 0 ? path.substring(colon + 1) : path);
  label.trim(); label.replace('\t', ' ');
  return secret.length() > 0;
}
