#include "StackChanApp.h"

#include "Config.h"
#include <WiFi.h>

namespace {
void expressionColor(Expression expression, uint8_t &red, uint8_t &green,
                     uint8_t &blue) {
  red = green = blue = 0;
  switch (expression) {
  case Expression::Happy:
    red = 24;
    green = 10;
    break;
  case Expression::Listening:
    green = 20;
    break;
  case Expression::Thinking:
    red = 12;
    blue = 20;
    break;
  case Expression::Speaking:
    blue = 24;
    break;
  case Expression::Surprised:
    red = 20;
    green = 16;
    blue = 4;
    break;
  case Expression::Sleepy:
    blue = 5;
    break;
  case Expression::Neutral:
  case Expression::Count:
    break;
  }
}
}

void StackChanApp::setup() {
  Serial.begin(115200);
  delay(250);
  Serial.printf("\n[%s] firmware v%d\n", Config::kDeviceName,
                Config::kFirmwareVersion);

  if (!_board.begin()) {
    Serial.println("[Board] initialization failed");
    return;
  }
  if (!_face.begin(_board.display())) {
    Serial.println("[Face] canvas allocation failed");
  }

  applyExpression(Expression::Neutral);
  if (Config::kEnableCamera) {
    _camera.begin();
  } else {
    Serial.println("[Camera] disabled (control service has priority)");
  }
  _audio.begin();
  _web.begin(_board, _face, _camera, _audio);
  printHelp();
  printStatus();
}

void StackChanApp::loop() {
  _board.update();
  _audio.update();
  _web.update();
  handleInputs();
  handleSerial();

  const uint32_t now = millis();
  if (now - _lastFrameMs >= Config::kFrameIntervalMs) {
    _lastFrameMs = now;
    _face.update(now);
  }
  delay(1);
}

void StackChanApp::handleInputs() {
  if (_board.topWasPressed()) {
    if (_audio.beginPushToTalk()) {
      applyExpression(Expression::Listening);
      Serial.println("[Input] push-to-talk pressed");
    } else {
      Serial.println("[Input] push-to-talk unavailable");
    }
  }

  if (_board.topWasReleased()) {
    _audio.endPushToTalk();
    applyExpression(Expression::Neutral);
    Serial.println("[Input] push-to-talk released");
  }

  int16_t x = 0;
  int16_t y = 0;
  if (_board.screenWasTapped(x, y)) {
    _face.cycleExpression();
    applyExpression(_face.expression());
    Serial.printf("[Input] screen tap (%d, %d), expression=%s\n", x, y,
                  expressionName(_face.expression()));
  }
}

void StackChanApp::handleSerial() {
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      _serialLine.trim();
      if (!_serialLine.isEmpty()) {
        runCommand(_serialLine);
      }
      _serialLine = "";
      continue;
    }
    if (_serialLine.length() < 128) {
      _serialLine += character;
    }
  }
}

void StackChanApp::runCommand(String line) {
  line.trim();
  const int separator = line.indexOf(' ');
  String command = separator < 0 ? line : line.substring(0, separator);
  String arguments = separator < 0 ? "" : line.substring(separator + 1);
  command.toLowerCase();
  arguments.trim();

  if (command == "help") {
    printHelp();
  } else if (command == "status") {
    printStatus();
  } else if (command == "network" && arguments.equalsIgnoreCase("off")) {
    _web.stop();
  } else if (command == "mic") {
    _audio.setMicrophoneEnabled(arguments.equalsIgnoreCase("on"));
  } else if (command == "speaker") {
    _audio.setSpeakerEnabled(arguments.equalsIgnoreCase("on"));
  } else if (command == "neutral") {
    _board.lookNeutral();
  } else if (command == "stop") {
    _board.stopMotion();
  } else if (command == "torque") {
    _board.setMotionTorqueEnabled(arguments.equalsIgnoreCase("on"));
    Serial.printf("[Motion] torque %s\n",
                  arguments.equalsIgnoreCase("on") ? "on" : "off");
  } else if (command == "look") {
    float yaw = 0.0f;
    float pitch = 45.0f;
    if (sscanf(arguments.c_str(), "%f %f", &yaw, &pitch) == 2) {
      _board.lookAtDegrees(yaw, pitch);
      Serial.printf("[Motion] target yaw=%.1f pitch=%.1f\n", yaw, pitch);
    } else {
      Serial.println("Usage: look <yaw-deg> <pitch-deg>");
    }
  } else if (command == "rgb") {
    int red = 0;
    int green = 0;
    int blue = 0;
    if (sscanf(arguments.c_str(), "%d %d %d", &red, &green, &blue) == 3) {
      _board.setRgb(constrain(red, 0, 255), constrain(green, 0, 255),
                    constrain(blue, 0, 255));
    } else {
      Serial.println("Usage: rgb <red> <green> <blue>");
    }
  } else if (command == "expression") {
    Expression expression;
    if (parseExpression(arguments, expression)) {
      applyExpression(expression);
    } else {
      Serial.println("Unknown expression. Try: neutral, happy, listening, "
                     "thinking, speaking, surprised, sleepy");
    }
  } else {
    Serial.printf("Unknown command: %s (type 'help')\n", command.c_str());
  }
}

void StackChanApp::applyExpression(Expression expression) {
  _face.setExpression(expression);
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  expressionColor(expression, red, green, blue);
  _board.setRgb(red, green, blue);
}

void StackChanApp::printHelp() const {
  Serial.println(
      "Commands:\n"
      "  status                         battery, pose, expression\n"
      "  expression <name>              set face and state LEDs\n"
      "  look <yaw-deg> <pitch-deg>     safe clamped head target\n"
      "  neutral | stop                 motion controls\n"
      "  torque <on|off>                servo torque\n"
      "  rgb <red> <green> <blue>       all 12 body LEDs\n"
      "  mic <on|off>                   device microphone\n"
      "  speaker <on|off>               device speaker\n"
      "  network off                    stop Wi-Fi until reboot\n"
      "  help                            show commands");
}

void StackChanApp::printStatus() {
  Serial.printf(
      "[Status] battery=%.3fV current=%.3fA level=%d%% yaw=%.1f pitch=%.1f "
      "expression=%s heap=%uK psram=%uK\n",
      _board.batteryVoltage(), _board.batteryCurrent(),
      _board.batteryPercent(), _board.yawTenths() / 10.0f,
      _board.pitchTenths() / 10.0f, expressionName(_face.expression()),
      static_cast<unsigned>(ESP.getFreeHeap() / 1024),
      static_cast<unsigned>(ESP.getFreePsram() / 1024));
  Serial.printf("[Network] wifi_status=%d rssi=%d control=%s camera=%s%s%s "
                "audio=%s mic=%s speaker=%s ptt=%s sequence=%u\n",
                static_cast<int>(WiFi.status()),
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                _web.address().c_str(), _camera.available() ? "ready" : "off",
                _camera.available() ? "" : " error=",
                _camera.available() ? "" : _camera.error(),
                _audio.available() ? "ready" : "off",
                _audio.microphoneEnabled() ? "on" : "muted",
                _audio.speakerEnabled() ? "on" : "muted",
                _audio.pushToTalkActive() ? "recording" : "ready",
                static_cast<unsigned>(_audio.pushToTalkSequence()));
}
