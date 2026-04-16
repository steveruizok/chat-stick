#pragma once

#include <Arduino.h>

class ButtonStateMachine {
public:
  struct Config {
    unsigned long holdMs;
    unsigned long doubleClickMs;
  };

  ButtonStateMachine() : _config({500, 350}) {}
  ButtonStateMachine(unsigned long holdMs, unsigned long doubleClickMs = 350)
      : _config({holdMs, doubleClickMs}) {}
  explicit ButtonStateMachine(const Config &config) : _config(config) {}

  void setConfig(const Config &config) { _config = config; }
  void update(bool pressed, unsigned long nowMs);
  void clearEvents();

  bool isPressed() const { return _pressed; }
  bool isHeld() const { return _held; }

  bool consumePressed();
  bool consumeReleased();
  bool consumeClick();
  bool consumeDoubleClick();
  bool consumeHoldStart();
  bool consumeHoldRelease();

private:
  Config _config;
  bool _pressed = false;
  bool _held = false;
  bool _awaitingSecondClick = false;
  unsigned long _pressStartMs = 0;
  unsigned long _lastReleaseMs = 0;

  bool _pressedEvent = false;
  bool _releasedEvent = false;
  bool _clickEvent = false;
  bool _doubleClickEvent = false;
  bool _holdStartEvent = false;
  bool _holdReleaseEvent = false;
};
