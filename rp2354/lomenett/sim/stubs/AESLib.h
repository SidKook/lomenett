#pragma once
#include "Arduino.h"
class AESLib { public:
  int encrypt(uint8_t*, int n, uint8_t*, const uint8_t*, int, uint8_t*){return n;}
  int decrypt(uint8_t*, int n, uint8_t*, const uint8_t*, int, uint8_t*){return n;} };
