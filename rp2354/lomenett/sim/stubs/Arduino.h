// Host stand-in for the Arduino core: String, Print, Serial, millis() (driven by g_sim_millis).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <string>
#include <cctype>

typedef uint8_t byte;
#define PROGMEM
#define F(x) (x)
#define pgm_read_byte(a) (*(const unsigned char *)(a))

// simulated clock, advanced by the harness
extern uint32_t g_sim_millis;
static inline uint32_t millis() { return g_sim_millis; }
static inline void delay(uint32_t) {}
static inline void delayMicroseconds(uint32_t) {}

// ---- Arduino String backed by std::string
class String {
public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(char c) { s = std::string(1, c); }
  String(const std::string &o) : s(o) {}
  String(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); s = b; }
  String(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); s = b; }
  String(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); s = b; }
  String(float v, int prec = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", prec, v); s = b; }
  int length() const { return (int)s.size(); }
  const char *c_str() const { return s.c_str(); }
  char charAt(int i) const { return (i >= 0 && i < (int)s.size()) ? s[i] : 0; }
  char operator[](int i) const { return charAt(i); }
  void reserve(int) {}
  long toInt() const { return atol(s.c_str()); }
  void trim() {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    s = s.substr(a, b - a);
  }
  int indexOf(char c, int from = 0) const { auto p = s.find(c, from < 0 ? 0 : from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char *c, int from = 0) const { auto p = s.find(c, from < 0 ? 0 : from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const String &o, int from = 0) const { return indexOf(o.c_str(), from); }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c, int from) const { if (from < 0) return -1; auto p = s.rfind(c, from); return p == std::string::npos ? -1 : (int)p; }
  bool endsWith(const char *p) const { size_t n = strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; }
  bool endsWith(const String &p) const { return endsWith(p.c_str()); }
  void toLowerCase() { for (auto &c : s) c = tolower((unsigned char)c); }
  void remove(int i) { if (i >= 0 && i < (int)s.size()) s.erase(i); }
  String substring(int a) const { if (a < 0) a = 0; if (a > (int)s.size()) a = s.size(); return String(s.substr(a)); }
  String substring(int a, int b) const {
    if (a < 0) a = 0; if (b > (int)s.size()) b = s.size(); if (b < a) b = a;
    return String(s.substr(a, b - a));
  }
  bool startsWith(const char *p) const { return s.rfind(p, 0) == 0; }
  bool startsWith(const String &p) const { return startsWith(p.c_str()); }
  void replace(char a, char b) { for (auto &c : s) if (c == a) c = b; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(char o) { s += o; return *this; }
  bool operator==(const char *o) const { return s == o; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator!=(const String &o) const { return s != o.s; }
};
static inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
static inline String operator+(const String &a, const char *b) { return String(a.s + b); }
static inline String operator+(const char *a, const String &b) { return String(std::string(a) + b.s); }
static inline bool operator<(const String &a, const String &b) { return a.s < b.s; }

// ---- Print / Serial: the mock display overrides write(); Serial just no-ops loudly if wanted
class Print {
public:
  virtual size_t write(uint8_t) { return 1; }
  virtual size_t write(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) write(b[i]); return n; }
  void print(const char *s) { write((const uint8_t *)s, strlen(s)); }
  void print(const String &s) { print(s.c_str()); }
  void print(char c) { write((uint8_t)c); }
  void print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); print(b); }
  void print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); print(b); }
  void println(const char *s = "") { print(s); write('\n'); }
  void printf(const char *fmt, ...) { char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a); print(b); }
};
struct SerialT { void begin(int) {} void printf(const char *, ...) {} void println(const char * = "") {} void print(const char *) {} operator bool() { return true; } };
extern SerialT Serial;

// min/max as macros, defined last so STL headers above are unaffected
#ifndef min
template <class T> static inline T tb_min(T a, T b) { return a < b ? a : b; }
template <class T> static inline T tb_max(T a, T b) { return a > b ? a : b; }
#define min(a, b) tb_min(a, b)
#define max(a, b) tb_max(a, b)
#endif
