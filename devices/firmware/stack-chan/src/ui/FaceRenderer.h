#pragma once

#include <Arduino.h>
#include <M5GFX.h>

enum class Expression : uint8_t {
  Neutral,
  Happy,
  Listening,
  Thinking,
  Speaking,
  Surprised,
  Sleepy,
  Count,
};

const char *expressionName(Expression expression);
bool parseExpression(const String &name, Expression &expression);

class FaceRenderer {
public:
  bool begin(LGFX_Device &display);
  void update(uint32_t nowMs);
  void setExpression(Expression expression);
  Expression expression() const { return _expression; }
  void cycleExpression();

private:
  LGFX_Device *_display = nullptr;
  M5Canvas _canvas;
  Expression _expression = Expression::Neutral;
  bool _dirty = true;
  bool _blinkClosed = false;
  uint32_t _nextBlinkMs = 0;
  uint32_t _blinkEndsMs = 0;

  void draw();
  void scheduleBlink(uint32_t nowMs);
};
