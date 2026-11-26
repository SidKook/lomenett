// Mock TinyUSB mass-storage + device for the host tests.
#pragma once
#include "Arduino.h"
typedef int32_t (*MscReadCallback)(uint32_t block, void *buffer, uint32_t length);
typedef int32_t (*MscWriteCallback)(uint32_t block, uint8_t *buffer, uint32_t length);
typedef void (*MscFlushCallback)(void);
typedef bool (*MscReadyCallback)(void);
typedef bool (*MscStartStopCallback)(uint8_t powerCondition, bool start, bool loadEject);
struct Adafruit_USBD_MSC {
  void setID(const char *, const char *, const char *) {}
  void setCapacity(uint32_t, uint32_t) {}
  void setReadWriteCallback(MscReadCallback, MscWriteCallback, MscFlushCallback) {}
  void setReadyCallback(MscReadyCallback) {}
  void setStartStopCallback(MscStartStopCallback) {}
  void setUnitReady(bool) {}
  void begin() {}
};
struct TinyUSBDevice_t { bool mounted() { return false; } void detach() {} void attach() {} };
static TinyUSBDevice_t TinyUSBDevice;
