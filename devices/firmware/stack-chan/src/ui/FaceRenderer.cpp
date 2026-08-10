#include "FaceRenderer.h"

#include "Config.h"

namespace {
constexpr uint16_t kBackground = TFT_BLACK;

struct AsciiFace {
  const char *eyes;
  const char *mouth;
};

AsciiFace asciiFace(Expression expression, bool blinkClosed) {
  if (blinkClosed) {
    return {"-       -", "   ---"};
  }

  switch (expression) {
  case Expression::Happy:
    return {"^       ^", "  \\___/"};
  case Expression::Listening:
    return {"O       O", "    o"};
  case Expression::Thinking:
    return {"o       ^", "    ."};
  case Expression::Speaking:
    return {"o       o", "   <=>"};
  case Expression::Surprised:
    return {"O       O", "    O"};
  case Expression::Sleepy:
    return {"-       -", "   ~~~"};
  case Expression::Neutral:
  case Expression::Count:
    return {"o       o", "   ---"};
  }

  return {"o       o", "   ---"};
}
}

const char *expressionName(Expression expression) {
  switch (expression) {
  case Expression::Neutral:
    return "neutral";
  case Expression::Happy:
    return "happy";
  case Expression::Listening:
    return "listening";
  case Expression::Thinking:
    return "thinking";
  case Expression::Speaking:
    return "speaking";
  case Expression::Surprised:
    return "surprised";
  case Expression::Sleepy:
    return "sleepy";
  case Expression::Count:
    break;
  }
  return "unknown";
}

bool parseExpression(const String &name, Expression &expression) {
  for (uint8_t index = 0; index < static_cast<uint8_t>(Expression::Count);
       ++index) {
    const auto candidate = static_cast<Expression>(index);
    if (name.equalsIgnoreCase(expressionName(candidate))) {
      expression = candidate;
      return true;
    }
  }
  return false;
}

bool FaceRenderer::begin(LGFX_Device &display) {
  _display = &display;
  _canvas.setColorDepth(16);
  // A full-screen 16-bit sprite is roughly 150 KB, which does not reliably
  // fit in one contiguous block of the CoreS3's internal heap after board
  // initialization. Keep that framebuffer in PSRAM and preserve internal RAM
  // for Wi-Fi, camera DMA, and task stacks.
  _canvas.setPsram(true);
  if (_canvas.createSprite(display.width(), display.height()) == nullptr) {
    return false;
  }
  if (Config::kBlinkEnabled) {
    scheduleBlink(millis());
  }
  draw();
  return true;
}

void FaceRenderer::update(uint32_t nowMs) {
  if (Config::kBlinkEnabled) {
    if (!_blinkClosed && nowMs >= _nextBlinkMs &&
        _expression != Expression::Surprised) {
      _blinkClosed = true;
      _blinkEndsMs = nowMs + Config::kBlinkDurationMs;
      _dirty = true;
    } else if (_blinkClosed && nowMs >= _blinkEndsMs) {
      _blinkClosed = false;
      scheduleBlink(nowMs);
      _dirty = true;
    }
  }

  if (_dirty) {
    draw();
  }
}

void FaceRenderer::setExpression(Expression expression) {
  if (expression == Expression::Count || expression == _expression) {
    return;
  }
  _expression = expression;
  _dirty = true;
}

void FaceRenderer::cycleExpression() {
  const uint8_t next =
      (static_cast<uint8_t>(_expression) + 1) %
      static_cast<uint8_t>(Expression::Count);
  setExpression(static_cast<Expression>(next));
}

void FaceRenderer::draw() {
  if (!_display) {
    return;
  }

  _canvas.fillScreen(kBackground);
  const int width = _canvas.width();
  const int height = _canvas.height();
  const AsciiFace face = asciiFace(_expression, _blinkClosed);

  _canvas.setTextDatum(middle_center);
  _canvas.setTextColor(TFT_WHITE, kBackground);
  _canvas.setFont(&fonts::FreeMonoBold18pt7b);
  _canvas.setTextSize(1);
  _canvas.drawString(face.eyes, width / 2, height * 40 / 100);
  _canvas.drawString(face.mouth, width / 2, height * 68 / 100);

  _canvas.setTextDatum(bottom_center);
  _canvas.setTextColor(TFT_DARKGREY, kBackground);
  _canvas.setFont(&fonts::Font0);
  _canvas.setTextSize(1);
  _canvas.drawString(expressionName(_expression), width / 2, height - 6);
  _canvas.pushSprite(_display, 0, 0);
  _dirty = false;
}

void FaceRenderer::scheduleBlink(uint32_t nowMs) {
  _nextBlinkMs = nowMs +
                 random(Config::kBlinkIntervalMinMs, Config::kBlinkIntervalMaxMs);
}
