#pragma once
#include "Arduino.h"
struct File { operator bool() const { return true; } size_t size(){return 0;}
  int read(){return -1;} int read(void*,size_t){return 0;}
  size_t write(uint8_t){return 1;} size_t write(const void*,size_t n){return n;} void close(){} };
struct FS { bool begin(){return true;} bool exists(const char*){return false;} bool remove(const char*){return true;} bool rename(const char*, const char*){return true;}
  bool format(){return true;} File open(const char*, const char*){return File();} };
extern FS LittleFS;
