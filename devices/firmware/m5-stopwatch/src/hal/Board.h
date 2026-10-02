#pragma once

#include <Arduino.h>
#include <M5Unified.h>
#include "hal/DeviceCapabilities.h"

namespace Board {
bool init();
bool initDisplay();
void drawDisplayBitmap(int x, int y, uint16_t *pixels, int w, int h);
void configureSpeaker(m5::speaker_config_t &cfg, bool external, int gain);
const DeviceCapabilities &capabilities();
void update();
M5GFX &display();
bool buttonAIsPressed();
bool buttonBIsPressed();
void setDisplayBrightness(uint8_t brightness);
uint8_t displayBrightness();
void setAudioAmpEnabled(bool enabled);
int batteryLevel();
uint16_t batteryVoltageMv();
uint16_t vbusVoltageMv();
bool usbConnected();
const char *powerSourceLabel();
LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs);
DeepSleepWakeReason deepSleepWakeReason();
void enterDeepSleep(uint64_t sleepUs);
void powerOff();
} // namespace Board
