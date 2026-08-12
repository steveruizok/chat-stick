#include "DebugConsole.h"

#if DEBUG_CONSOLE_ENABLED

void DebugConsole::poll() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (_line.length() > 0) {
        dispatch(_line);
        _line = "";
      }
      continue;
    }
    if (_line.length() < 64) {
      _line += c;
    }
  }
}

void DebugConsole::dispatch(const String &line) {
  String command = line;
  command.trim();

  String tokens[4];
  int count = 0;
  int start = 0;
  while (count < 4 && start < command.length()) {
    int space = command.indexOf(' ', start);
    if (space < 0) {
      space = command.length();
    }
    const String token = command.substring(start, space);
    if (token.length() > 0) {
      tokens[count++] = token;
    }
    start = space + 1;
  }
  if (count == 0) {
    return;
  }

  const String &verb = tokens[0];
  if (verb == "screenshot") {
    if (_handlers.screenshot) {
      _handlers.screenshot();
    }
    return;
  }
  if (verb == "state") {
    if (_handlers.state) {
      _handlers.state();
    }
    return;
  }
  if (verb == "screen" && count >= 2) {
    if (_handlers.screen) {
      _handlers.screen(tokens[1]);
    }
    return;
  }
  if (verb == "tap" && count >= 3) {
    if (_handlers.touch) {
      const int x = tokens[1].toInt();
      const int y = tokens[2].toInt();
      _handlers.touch(true, x, y);
      _handlers.touch(false, x, y);
    }
    return;
  }
  if (verb == "press" && count >= 3) {
    if (_handlers.touch) {
      _handlers.touch(true, tokens[1].toInt(), tokens[2].toInt());
    }
    return;
  }
  if (verb == "release") {
    if (_handlers.touch) {
      _handlers.touch(false, 0, 0);
    }
    return;
  }
  if (verb == "btn" && count >= 3) {
    if (_handlers.button) {
      const char button = tokens[1].length() > 0 ? tokens[1][0] : 'a';
      _handlers.button(button, tokens[2] == "hold");
    }
    return;
  }

  Serial.printf("DBG unknown command: %s\n", command.c_str());
}

#endif // DEBUG_CONSOLE_ENABLED
