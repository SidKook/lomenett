#pragma once
#include "Arduino.h"
struct TwoWire { void begin() {} void setSDA(int){} void setSCL(int){} void setClock(uint32_t){}
  void beginTransmission(int){} size_t write(uint8_t){return 1;} size_t write(const uint8_t*,size_t n){return n;}
  int endTransmission(){return 0;} int requestFrom(int,uint8_t){return 0;} int read(){return 0;} int available(){return 0;} };
extern TwoWire Wire;
