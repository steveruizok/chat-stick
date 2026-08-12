#pragma once

#include "Qmi8658.h"
#include "TossDetector.h"
#include "DebugConsole.h"
#include <Preferences.h>

class HeavenGame {
public:
  void setup();
  void loop();

private:
  enum class Screen { Safety, CableLock, Ready, Armed, Result, Error };

  void updateButton(uint32_t nowMs);
  void handleClick(uint32_t nowMs);
  void sampleMotion(uint32_t nowMs);
  void showSafety();
  void showCableLock();
  void showReady();
  void showArmed();
  void showResult(float heightMeters, uint32_t airtimeMs, bool newBest,
                  float previewBestMeters = -1.0f);
  void showFailure(const char *title, const char *detail);
  void showError(const char *detail);
  void drawGameFrame();
  void drawHeader(const String &text);
  void drawText(int x, int y, const String &text, uint8_t scale,
                uint16_t color);
  void centeredText(int y, const String &text, uint8_t size,
                    uint16_t color);
  void centeredAction(int y, const String &text);
  bool allocateFramebuffer();
  void clearFrame(uint16_t color);
  void flushFrame();
  void putPixel(int x, int y, uint16_t color);
  void fillRect(int x, int y, int w, int h, uint16_t color);
#if DEBUG_CONSOLE_ENABLED
  void configureDebugConsole();
  void dumpFramebuffer();
  void dumpState();
  void previewScreen(const String &name);
  void previewNext();
#endif

  Qmi8658 _imu;
  TossDetector _detector;
  Preferences _preferences;
  Screen _screen = Screen::Safety;
  float _bestHeightMeters = 0.0f;
  uint32_t _lastSampleUs = 0;
  uint32_t _lastDiagnosticMs = 0;
  uint32_t _armedAtMs = 0;
  bool _buttonRaw = false;
  bool _buttonStable = false;
  uint32_t _buttonChangedAtMs = 0;
  uint16_t *_framebuffer = nullptr;
#if DEBUG_CONSOLE_ENABLED
  DebugConsole _console;
  bool _debugPreview = false;
  bool _syntheticTouchPressed = false;
  bool _previewHighScore = false;
#endif
};
