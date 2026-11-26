// Host test for totp.h: RFC 4226 / RFC 6238 vectors, base32 cleanup,
// otpauth:// parsing and the session clock.
#include "Arduino.h"
#include "totp.h"
#include <cstdio>

uint32_t g_sim_millis = 1000;
static int pass = 0, fail = 0;
static void check(const char *what, bool ok) { printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (ok) pass++; else fail++; }

int main() {
  // RFC 4226 appendix D: secret "12345678901234567890", 6 digits, counters 0-9
  static const uint32_t hv[10] = { 755224, 287082, 359152, 969429, 338314, 254676, 287922, 162583, 399871, 520489 };
  const uint8_t *k = (const uint8_t *)"12345678901234567890";
  bool ok = true; for (int i = 0; i < 10; i++) ok &= hotp(k, 20, i, 6) == hv[i];
  check("RFC 4226 HOTP vectors", ok);

  // RFC 6238 appendix B, SHA1 rows (8 digits, 30 s)
  const char *B32 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";   // base32("12345678901234567890")
  struct { uint64_t t; const char *code; } tv[] = {
    { 59, "94287082" }, { 1111111109, "07081804" }, { 1111111111, "14050471" },
    { 1234567890, "89005924" }, { 2000000000, "69279037" }, { 20000000000ULL, "65353130" } };
  for (auto &v : tv) { char c[10]; totpCode(B32, 8, 30, v.t, c); char what[64]; snprintf(what, sizeof(what), "RFC 6238 t=%llu -> %s (got %s)", (unsigned long long)v.t, v.code, c); check(what, !strcmp(c, v.code)); }
  { char c[10]; totpCode(B32, 6, 30, 59, c); check("6-digit is the low digits (287082)", !strcmp(c, "287082")); }

  // base32 cleanup: Google-style grouped lower case with padding
  { char n[48]; check("normalize 'gezd gnbv gy3t qojq ==' ", totpNormalize("gezd gnbv-gy3t qojq ==", n, sizeof(n)) && !strcmp(n, "GEZDGNBVGY3TQOJQ")); }
  { char n[48]; check("reject '0' and '1' (not base32)", !totpNormalize("GEZDGNBV0Y3TQOJQ", n, sizeof(n))); }
  { char n[48]; check("reject too short", !totpNormalize("ABCD", n, sizeof(n))); }
  { char n[48]; check("reject masked ********", !totpNormalize("********", n, sizeof(n))); }
  { char n[8]; check("reject secret longer than the buffer", !totpNormalize("GEZDGNBVGY3TQOJQ", n, sizeof(n))); }
  { char n[104]; char c[10]; const char *s52 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567ABCDEFGHIJKLMNOPQRST";   // 52 chars = 32-byte key, the Amazon size
    check("accept a 52-character secret", totpNormalize(s52, n, sizeof(n)) && strlen(n) == 52 && totpCode(n, 6, 30, 1700000000ULL, c) && strlen(c) == 6); }

  // otpauth://
  { String l, s; int d, p;
    bool r = totpParseUri("otpauth://totp/GitHub:octocat?secret=JBSWY3DPEHPK3PXP&issuer=GitHub", l, s, d, p);
    check("uri: issuer, secret, defaults", r && l == "GitHub" && s == "JBSWY3DPEHPK3PXP" && d == 6 && p == 30); }
  { String l, s; int d, p;
    bool r = totpParseUri("otpauth://totp/My%20Bank:me%40x.com?digits=8&period=60&secret=ABCDEFGHIJ234567", l, s, d, p);
    check("uri: label from path, url-decoded, digits/period", r && l == "My Bank" && d == 8 && p == 60); }
  { String l, s; int d, p;
    bool r = totpParseUri("otpauth://totp/me@x.com?secret=ABCDEFGHIJ234567", l, s, d, p);
    check("uri: no issuer -> account as label", r && l == "me@x.com"); }
  { String l, s; int d, p;
    check("uri: SHA256 refused", !totpParseUri("otpauth://totp/X?secret=ABCDEFGHIJ234567&algorithm=SHA256", l, s, d, p)); }
  { String l, s; int d, p;
    check("uri: hotp refused", !totpParseUri("otpauth://hotp/X?secret=ABCDEFGHIJ234567&counter=1", l, s, d, p)); }

  // clock
  check("clock invalid before sync", !totpClockValid());
  check("clock refuses 1970", !totpClockSet(59) && !totpClockValid());
  check("clock accepts 2026", totpClockSet(1793836800ULL) && totpClockValid());
  g_sim_millis += 45500;
  check("clock follows millis (+45 s)", totpClockNow() == 1793836845ULL);
  check("seconds left in 30 s window", totpSecondsLeft(30) == 15);

  printf("\n%d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
