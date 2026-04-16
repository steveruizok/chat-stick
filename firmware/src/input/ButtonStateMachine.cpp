#include "ButtonStateMachine.h"

namespace {
bool consumeFlag(bool &flag) {
  const bool value = flag;
  flag = false;
  return value;
}
} // namespace

void ButtonStateMachine::update(bool pressed, unsigned long nowMs) {
  if (_awaitingSecondClick &&
      nowMs - _lastReleaseMs > _config.doubleClickMs) {
    _clickEvent = true;
    _awaitingSecondClick = false;
  }

  if (pressed != _pressed) {
    _pressed = pressed;

    if (pressed) {
      _pressStartMs = nowMs;
      _pressedEvent = true;
      return;
    }

    _releasedEvent = true;
    if (_held) {
      _held = false;
      _holdReleaseEvent = true;
      _awaitingSecondClick = false;
    } else if (_awaitingSecondClick &&
               nowMs - _lastReleaseMs <= _config.doubleClickMs) {
      _doubleClickEvent = true;
      _awaitingSecondClick = false;
    } else {
      _awaitingSecondClick = true;
      _lastReleaseMs = nowMs;
    }
    return;
  }

  if (_pressed && !_held && nowMs - _pressStartMs >= _config.holdMs) {
    _held = true;
    _holdStartEvent = true;
    _awaitingSecondClick = false;
  }
}

void ButtonStateMachine::clearEvents() {
  _pressedEvent = false;
  _releasedEvent = false;
  _clickEvent = false;
  _doubleClickEvent = false;
  _holdStartEvent = false;
  _holdReleaseEvent = false;
}

bool ButtonStateMachine::consumePressed() { return consumeFlag(_pressedEvent); }

bool ButtonStateMachine::consumeReleased() {
  return consumeFlag(_releasedEvent);
}

bool ButtonStateMachine::consumeClick() { return consumeFlag(_clickEvent); }

bool ButtonStateMachine::consumeDoubleClick() {
  return consumeFlag(_doubleClickEvent);
}

bool ButtonStateMachine::consumeHoldStart() {
  return consumeFlag(_holdStartEvent);
}

bool ButtonStateMachine::consumeHoldRelease() {
  return consumeFlag(_holdReleaseEvent);
}
