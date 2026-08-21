#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "hal/DeviceCapabilities.h"

/**
 * @brief Hardware accessors and board-specific control helpers.
 */
namespace Board {
/// Initialize board peripherals.
bool init();

/// Static feature flags for this board target.
const DeviceCapabilities &capabilities();

/// Service board-level background work.
void update();

/// Shared display driver instance. The concrete panel (SH8601 on the original
/// board, CO5300 on V2) is chosen at build time; callers use the common
/// Arduino_OLED interface.
Arduino_OLED &display();

/// Whether button A is currently pressed.
bool buttonAIsPressed();

/// Whether button B is currently pressed.
bool buttonBIsPressed();

/**
 * @brief Set display backlight brightness.
 * @param brightness Backlight level.
 */
void setDisplayBrightness(uint8_t brightness);

/// Current display brightness level.
uint8_t displayBrightness();

/**
 * @brief Enable or disable the speaker amplifier.
 * @param enabled True to enable the amplifier.
 */
void setAudioAmpEnabled(bool enabled);

/// Battery charge estimate as a percentage.
int batteryLevel();

/// Battery voltage in millivolts.
uint16_t batteryVoltageMv();

/// USB VBUS voltage in millivolts.
uint16_t vbusVoltageMv();

/// Whether external USB power is present.
bool usbConnected();

/// Human-readable label for the current power source.
const char *powerSourceLabel();

/// Enter one light-sleep interval and report what woke the board.
LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs);

/// Report why this boot resumed from deep sleep.
DeepSleepWakeReason deepSleepWakeReason();

/**
 * @brief Enter deep sleep with board-specific wake sources configured.
 * @param sleepUs Timer wake interval in microseconds.
 */
void enterDeepSleep(uint64_t sleepUs);

/// Power down the board.
void powerOff();
} // namespace Board
