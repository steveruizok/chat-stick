#include "HeavenGame.h"

#include "Config.h"
#include "hal/Board.h"
#include "ui/SmartBrickFont.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace {
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
// Figma: full-bleed button_bg #FFC300.
constexpr uint16_t kGameYellow = 0xFE00;
constexpr uint8_t kGameBrightness = 255;
constexpr uint32_t kSampleIntervalUs = 4000;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr size_t kFramebufferBytes =
    static_cast<size_t>(SCREEN_WIDTH_PX) * SCREEN_HEIGHT_PX * sizeof(uint16_t);
constexpr int kContentX = 32;
constexpr int kContentWidth = 304;
constexpr int kTextRowHeight = 32;

float magnitude(float x, float y, float z) {
  return sqrtf(x * x + y * y + z * z);
}
} // namespace

void HeavenGame::setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("[Heaven] booting");

  Board::init();
  Board::setAudioAmpEnabled(false);
  auto &display = Board::display();
  Serial.printf("[Heaven] display controller %s\n",
                Board::displayControllerName());
  if (!display.begin()) {
    Serial.println("[Heaven] display initialization failed");
  }
  display.setRotation(0);
  Board::setDisplayBrightness(kGameBrightness);
  allocateFramebuffer();

#if DEBUG_CONSOLE_ENABLED
  configureDebugConsole();
#endif

  if (!_imu.begin(Wire)) {
    Serial.println("[Heaven] QMI8658 not found");
    showError("QMI8658 NOT FOUND");
    return;
  }
  Serial.printf("[Heaven] QMI8658 online at 0x%02X\n", _imu.address());

  _preferences.begin("heaven", false);
  _bestHeightMeters = _preferences.getFloat("best_m", 0.0f);
  if (!isfinite(_bestHeightMeters) || _bestHeightMeters < 0.0f ||
      _bestHeightMeters > 20.0f) {
    _bestHeightMeters = 0.0f;
  }

  _buttonRaw = Board::buttonAIsPressed();
  _buttonStable = _buttonRaw;
  _buttonChangedAtMs = millis();
  showSafety();
}

void HeavenGame::loop() {
  Board::update();
#if DEBUG_CONSOLE_ENABLED
  _console.poll();
#endif
  const uint32_t nowMs = millis();
  updateButton(nowMs);

#if DEBUG_CONSOLE_ENABLED
  if (_debugPreview) {
    delay(1);
    return;
  }
#endif

  if (_screen != Screen::Safety && _screen != Screen::Error &&
      Board::usbConnected() && _screen != Screen::CableLock) {
    _detector.cancel();
    showCableLock();
  } else if (_screen == Screen::CableLock && !Board::usbConnected()) {
    showReady();
  }

  if (_screen == Screen::Armed) {
    sampleMotion(nowMs);
  } else if (Board::usbConnected() && nowMs - _lastDiagnosticMs >= 1000) {
    // Keep a low-rate sensor heartbeat available during tethered bring-up.
    _lastDiagnosticMs = nowMs;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (_imu.readAcceleration(x, y, z)) {
      Serial.printf("[Heaven] imu=%.2fg usb=1 screen=%d\n",
                    magnitude(x, y, z), static_cast<int>(_screen));
    } else {
      Serial.println("[Heaven] IMU read failed");
    }
  }

  delay(1);
}

void HeavenGame::updateButton(uint32_t nowMs) {
  const bool raw = Board::buttonAIsPressed();
  if (raw != _buttonRaw) {
    _buttonRaw = raw;
    _buttonChangedAtMs = nowMs;
  }
  if (raw == _buttonStable || nowMs - _buttonChangedAtMs < kButtonDebounceMs) {
    return;
  }

  const bool wasPressed = _buttonStable;
  _buttonStable = raw;
  if (wasPressed && !raw) {
    handleClick(nowMs);
  }
}

void HeavenGame::handleClick(uint32_t nowMs) {
#if DEBUG_CONSOLE_ENABLED
  _debugPreview = false;
#endif
  switch (_screen) {
  case Screen::Safety:
    if (Board::usbConnected()) {
      showCableLock();
    } else {
      _detector.arm(nowMs);
      _armedAtMs = nowMs;
      showArmed();
    }
    break;
  case Screen::Ready:
  case Screen::Result:
    if (Board::usbConnected()) {
      showCableLock();
      break;
    }
    _detector.arm(nowMs);
    _armedAtMs = nowMs;
    showArmed();
    break;
  case Screen::Armed:
    _detector.cancel();
    showReady();
    break;
  case Screen::CableLock:
  case Screen::Error:
    break;
  }
}

void HeavenGame::sampleMotion(uint32_t nowMs) {
  const uint32_t nowUs = micros();
  if (nowUs - _lastSampleUs < kSampleIntervalUs) {
    return;
  }
  _lastSampleUs = nowUs;

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!_imu.readAcceleration(x, y, z)) {
    return;
  }

  const TossDetector::Update update =
      _detector.update(nowMs, magnitude(x, y, z));
  switch (update.event) {
  case TossDetector::Event::FlightStarted:
    // Do not touch the relatively slow display bus during flight; preserving
    // the 250 Hz motion sampling cadence is more important than animation.
    Serial.println("[Heaven] flight started");
    break;
  case TossDetector::Event::TossComplete: {
    const bool newBest = update.heightMeters > _bestHeightMeters;
    if (newBest) {
      _bestHeightMeters = update.heightMeters;
      _preferences.putFloat("best_m", _bestHeightMeters);
    }
    Serial.printf("[Heaven] airtime=%lums height=%.3fm best=%.3fm\n",
                  static_cast<unsigned long>(update.airtimeMs),
                  update.heightMeters, _bestHeightMeters);
    showResult(update.heightMeters, update.airtimeMs, newBest);
    break;
  }
  case TossDetector::Event::TimedOut:
    showFailure("NO TOSS", "PRESS BOOT TO RETRY");
    break;
  case TossDetector::Event::Invalid:
    showFailure("TOO QUICK", "TRY A GENTLE TOSS");
    break;
  case TossDetector::Event::None:
    break;
  }
}

void HeavenGame::showSafety() {
  _screen = Screen::Safety;
  drawGameFrame();
  centeredText(32, "S.M.T.H", 1, kBlack);
  centeredText(96, "SEND ME TO HEAVEN", 1, kBlack);
  centeredAction(320, "Play game");
  flushFrame();
}

void HeavenGame::showCableLock() {
  _screen = Screen::CableLock;
  drawGameFrame();
  drawHeader("SAFETY LOCK");
  drawText(kContentX, 128, "Unplug the USB", 1, kBlack);
  drawText(kContentX, 160, "cable to play.", 1, kBlack);
  centeredText(256, "Waiting...", 1, kBlack);
  flushFrame();
}

void HeavenGame::showReady() {
  _screen = Screen::Ready;
  drawGameFrame();
  centeredText(32, "S.M.T.H", 1, kBlack);
  centeredText(96, "SEND ME TO HEAVEN", 1, kBlack);
  centeredAction(320, "Play game");
  flushFrame();
}

void HeavenGame::showArmed() {
  _screen = Screen::Armed;
  drawGameFrame();
  drawText(kContentX, 160, "Throw your device", 1, kBlack);
  drawText(kContentX, 192, "as high as you can.", 1, kBlack);
  flushFrame();
}

void HeavenGame::showResult(float heightMeters, uint32_t airtimeMs,
                            bool newBest, float previewBestMeters) {
  _screen = Screen::Result;
  (void)airtimeMs;
  drawGameFrame();
  drawHeader(newBest ? "NEW HIGH SCORE" : "RESULT");

  char scoreLine[20];
  char bestLine[20];
  snprintf(scoreLine, sizeof(scoreLine), "%-14s%4.2fm", "Score",
           heightMeters);
  snprintf(bestLine, sizeof(bestLine), "%-14s%4.2fm", "High score",
           previewBestMeters >= 0.0f ? previewBestMeters : _bestHeightMeters);
  drawText(kContentX, 96, scoreLine, 1, kBlack);
  drawText(kContentX, 160, bestLine, 1, kBlack);
  centeredAction(320, "Play again");
  flushFrame();
}

void HeavenGame::showFailure(const char *title, const char *detail) {
  _screen = Screen::Result;
  drawGameFrame();
  drawHeader(title);
  centeredText(160, detail, 1, kBlack);
  centeredAction(320, "Play again");
  flushFrame();
}

void HeavenGame::showError(const char *detail) {
  _screen = Screen::Error;
  drawGameFrame();
  drawHeader("HARDWARE ERROR");
  centeredText(160, detail, 1, kBlack);
  centeredText(224, "Restart device", 1, kBlack);
  flushFrame();
}

void HeavenGame::drawGameFrame() {
  clearFrame(kGameYellow);
}

void HeavenGame::drawHeader(const String &text) {
  fillRect(kContentX, 32, kContentWidth, kTextRowHeight, kBlack);
  centeredText(32, text, 1, kWhite);
}

void HeavenGame::drawText(int x, int y, const String &text, uint8_t size,
                          uint16_t color) {
  const int unscaledWidth =
      static_cast<int>(text.length()) * SmartBrickFont::kCellW;
  const int maxScale = max(1, SCREEN_WIDTH_PX / max(1, unscaledWidth));
  const int scale = constrain(static_cast<int>(size), 1, maxScale);

  for (int i = 0; i < static_cast<int>(text.length()); i++) {
    const char c = text[i] >= SmartBrickFont::kFirstChar &&
                           text[i] <= SmartBrickFont::kLastChar
                       ? text[i]
                       : ' ';
    SmartBrickFont::Glyph glyph;
    memcpy_P(&glyph, SmartBrickFont::glyphFor(c), sizeof(glyph));
    const int glyphLeft =
        x + i * SmartBrickFont::kCellW * scale + glyph.ofsX * scale;
    const int glyphTop =
        y + ((SmartBrickFont::kLineHeight - SmartBrickFont::kBaseline) -
             glyph.boxH - glyph.ofsY) *
                scale;

    // Draw horizontal bitmap runs rather than one display transaction per
    // pixel. This is the same SmartBrickFont data and baseline calculation as
    // the chat UI, with integer scaling for large game scores.
    for (int gy = 0; gy < glyph.boxH; gy++) {
      int runStart = -1;
      for (int gx = 0; gx <= glyph.boxW; gx++) {
        const bool on = gx < glyph.boxW &&
                        SmartBrickFont::glyphPixelOn(glyph, gx, gy);
        if (on && runStart < 0) {
          runStart = gx;
        } else if (!on && runStart >= 0) {
          fillRect(glyphLeft + runStart * scale, glyphTop + gy * scale,
                   (gx - runStart) * scale, scale, color);
          runStart = -1;
        }
      }
    }
  }
}

void HeavenGame::centeredText(int y, const String &text, uint8_t size,
                              uint16_t color) {
  const int unscaledWidth =
      static_cast<int>(text.length()) * SmartBrickFont::kCellW;
  const int maxScale = max(1, SCREEN_WIDTH_PX / max(1, unscaledWidth));
  const int scale = constrain(static_cast<int>(size), 1, maxScale);
  const int width = unscaledWidth * scale;
  drawText(max(0, (SCREEN_WIDTH_PX - width) / 2), y, text, scale, color);
}

void HeavenGame::centeredAction(int y, const String &text) {
  const int width = static_cast<int>(text.length()) * SmartBrickFont::kCellW;
  const int x = (SCREEN_WIDTH_PX - width) / 2;
  drawText(x, y, text, 1, kBlack);
  fillRect(x, y + 30, width, 2, kBlack);
}

bool HeavenGame::allocateFramebuffer() {
  _framebuffer = static_cast<uint16_t *>(
      heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_framebuffer) {
    _framebuffer = static_cast<uint16_t *>(
        heap_caps_malloc(kFramebufferBytes, MALLOC_CAP_8BIT));
  }
  if (!_framebuffer) {
    Serial.println("[Heaven] framebuffer allocation failed; drawing directly");
    return false;
  }
  Serial.printf("[Heaven] framebuffer bytes=%u\n",
                static_cast<unsigned>(kFramebufferBytes));
  return true;
}

void HeavenGame::clearFrame(uint16_t color) {
  if (!_framebuffer) {
    Board::display().fillScreen(color);
    return;
  }

  const uint8_t high = static_cast<uint8_t>(color >> 8);
  const uint8_t low = static_cast<uint8_t>(color & 0xFF);
  if (high == low) {
    memset(_framebuffer, high, kFramebufferBytes);
    return;
  }

  for (int x = 0; x < SCREEN_WIDTH_PX; x++) {
    _framebuffer[x] = color;
  }
  constexpr size_t kRowBytes = SCREEN_WIDTH_PX * sizeof(uint16_t);
  for (int y = 1; y < SCREEN_HEIGHT_PX; y++) {
    memcpy(_framebuffer + static_cast<size_t>(y) * SCREEN_WIDTH_PX,
           _framebuffer, kRowBytes);
  }
}

void HeavenGame::flushFrame() {
  if (_framebuffer) {
    Board::display().draw16bitRGBBitmap(0, 0, _framebuffer, SCREEN_WIDTH_PX,
                                       SCREEN_HEIGHT_PX);
  }
}

void HeavenGame::putPixel(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= SCREEN_WIDTH_PX || y >= SCREEN_HEIGHT_PX) {
    return;
  }
  if (_framebuffer) {
    _framebuffer[static_cast<size_t>(y) * SCREEN_WIDTH_PX + x] = color;
  } else {
    Board::display().drawPixel(x, y, color);
  }
}

void HeavenGame::fillRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0 || x >= SCREEN_WIDTH_PX || y >= SCREEN_HEIGHT_PX ||
      x + w <= 0 || y + h <= 0) {
    return;
  }
  const int x0 = max(0, x);
  const int y0 = max(0, y);
  const int x1 = min(SCREEN_WIDTH_PX, x + w);
  const int y1 = min(SCREEN_HEIGHT_PX, y + h);

  if (!_framebuffer) {
    Board::display().fillRect(x0, y0, x1 - x0, y1 - y0, color);
    return;
  }
  for (int yy = y0; yy < y1; yy++) {
    uint16_t *row = _framebuffer + static_cast<size_t>(yy) * SCREEN_WIDTH_PX;
    for (int xx = x0; xx < x1; xx++) {
      row[xx] = color;
    }
  }
}

#if DEBUG_CONSOLE_ENABLED

void HeavenGame::configureDebugConsole() {
  DebugConsole::Handlers handlers;
  handlers.screenshot = [this]() { dumpFramebuffer(); };
  handlers.state = [this]() { dumpState(); };
  handlers.screen = [this](const String &name) { previewScreen(name); };
  handlers.touch = [this](bool pressed, int x, int y) {
    if (pressed) {
      _syntheticTouchPressed = true;
      return;
    }
    const bool wasPressed = _syntheticTouchPressed;
    _syntheticTouchPressed = false;
    if (wasPressed && x >= 0 && x < SCREEN_WIDTH_PX && y >= 0 &&
        y < SCREEN_HEIGHT_PX) {
      previewNext();
    }
  };
  handlers.button = [this](char button, bool hold) {
    if (button == 'a' && !hold) {
      previewNext();
    }
  };
  _console.init(handlers);
  Serial.println("DBG console ready");
  Serial.println("DBG commands: screenshot, state, screen NAME, tap X Y, "
                 "btn a click");
}

void HeavenGame::dumpFramebuffer() {
  if (!_framebuffer) {
    Serial.println("SCREENSHOT ERROR no framebuffer");
    return;
  }

  const int total = SCREEN_WIDTH_PX * SCREEN_HEIGHT_PX;
  Serial.printf("SCREENSHOT BEGIN w=%d h=%d fmt=rle565\n", SCREEN_WIDTH_PX,
                SCREEN_HEIGHT_PX);

  static const char kBase64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t carry[3];
  int carryLen = 0;
  int lineLen = 0;

  auto emit = [&](uint8_t byte) {
    carry[carryLen++] = byte;
    if (carryLen < 3) {
      return;
    }
    const uint32_t triple =
        (static_cast<uint32_t>(carry[0]) << 16) | (carry[1] << 8) | carry[2];
    char out[4] = {kBase64[(triple >> 18) & 0x3F],
                   kBase64[(triple >> 12) & 0x3F],
                   kBase64[(triple >> 6) & 0x3F], kBase64[triple & 0x3F]};
    Serial.write(reinterpret_cast<uint8_t *>(out), 4);
    carryLen = 0;
    lineLen += 4;
    if (lineLen >= 76) {
      Serial.write('\n');
      lineLen = 0;
    }
  };

  int index = 0;
  while (index < total) {
    const uint16_t value = _framebuffer[index];
    int run = 1;
    while (index + run < total && _framebuffer[index + run] == value &&
           run < 0xFFFF) {
      run++;
    }
    emit(static_cast<uint8_t>(run & 0xFF));
    emit(static_cast<uint8_t>((run >> 8) & 0xFF));
    emit(static_cast<uint8_t>(value & 0xFF));
    emit(static_cast<uint8_t>((value >> 8) & 0xFF));
    index += run;
  }

  if (carryLen > 0) {
    for (int i = carryLen; i < 3; i++) {
      carry[i] = 0;
    }
    const uint32_t triple =
        (static_cast<uint32_t>(carry[0]) << 16) | (carry[1] << 8) | carry[2];
    char out[4] = {kBase64[(triple >> 18) & 0x3F],
                   kBase64[(triple >> 12) & 0x3F],
                   carryLen > 1 ? kBase64[(triple >> 6) & 0x3F] : '=',
                   carryLen > 2 ? kBase64[triple & 0x3F] : '='};
    Serial.write(reinterpret_cast<uint8_t *>(out), 4);
  }
  Serial.println();
  Serial.println("SCREENSHOT END");
  // Ensure the final marker reaches the host before a one-shot capture tool
  // closes the port. This keeps the USB Serial/JTAG endpoint responsive for
  // the next debug command.
  Serial.flush();
}

void HeavenGame::dumpState() {
  const char *name = "unknown";
  switch (_screen) {
  case Screen::Safety:
    name = "home";
    break;
  case Screen::CableLock:
    name = "cable";
    break;
  case Screen::Ready:
    name = "ready";
    break;
  case Screen::Armed:
    name = "armed";
    break;
  case Screen::Result:
    name = _previewHighScore ? "highscore" : "result";
    break;
  case Screen::Error:
    name = "error";
    break;
  }
  Serial.printf("STATE screen=%s best=%.2fm preview=%d usb=%d\n", name,
                _bestHeightMeters, _debugPreview ? 1 : 0,
                Board::usbConnected() ? 1 : 0);
}

void HeavenGame::previewScreen(const String &name) {
  _debugPreview = true;
  _detector.cancel();
  _previewHighScore = false;

  if (name == "home" || name == "ready") {
    showReady();
  } else if (name == "armed") {
    showArmed();
  } else if (name == "result") {
    showResult(2.34f, 1381, false, 3.21f);
  } else if (name == "highscore" || name == "high-score") {
    _previewHighScore = true;
    showResult(3.21f, 1618, true, 3.21f);
  } else if (name == "cable" || name == "safety") {
    showCableLock();
  } else if (name == "failure") {
    showFailure("NO TOSS", "PRESS BOOT TO RETRY");
  } else if (name == "error") {
    showError("QMI8658 NOT FOUND");
  } else {
    Serial.printf("DBG unknown screen: %s\n", name.c_str());
    return;
  }
  Serial.printf("DBG preview screen=%s\n", name.c_str());
}

void HeavenGame::previewNext() {
  _debugPreview = true;
  _detector.cancel();
  switch (_screen) {
  case Screen::Safety:
  case Screen::CableLock:
  case Screen::Ready:
    _previewHighScore = false;
    showArmed();
    break;
  case Screen::Armed:
    _previewHighScore = false;
    showResult(2.34f, 1381, false, 3.21f);
    break;
  case Screen::Result:
    if (!_previewHighScore) {
      _previewHighScore = true;
      showResult(3.21f, 1618, true, 3.21f);
    } else {
      _previewHighScore = false;
      showReady();
    }
    break;
  case Screen::Error:
    _previewHighScore = false;
    showReady();
    break;
  }
}

#endif // DEBUG_CONSOLE_ENABLED
