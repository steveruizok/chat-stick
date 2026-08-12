#pragma once

#include "Config.h"
#include <Arduino.h>
#include <functional>

#if DEBUG_CONSOLE_ENABLED

/**
 * Line-oriented serial console copied from the translator firmware and kept
 * development-only for screenshot capture and synthetic input.
 */
class DebugConsole {
public:
  struct Handlers {
    std::function<void()> screenshot;
    std::function<void(bool pressed, int x, int y)> touch;
    std::function<void(char button, bool hold)> button;
    std::function<void()> state;
    std::function<void(const String &name)> screen;
  };

  void init(const Handlers &handlers) { _handlers = handlers; }
  void poll();

private:
  Handlers _handlers;
  String _line;

  void dispatch(const String &line);
};

#endif // DEBUG_CONSOLE_ENABLED
