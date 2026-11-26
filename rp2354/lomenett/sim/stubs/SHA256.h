#pragma once
#include "Arduino.h"
class SHA256 { public:
  void resetHMAC(const void*, int){} void update(const void*, int){}
  void finalizeHMAC(const void*, int, void* out, int n){ memset(out,0,n); } };
