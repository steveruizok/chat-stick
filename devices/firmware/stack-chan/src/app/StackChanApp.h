#pragma once

#include "hal/StackChanBoard.h"
#include "services/AudioService.h"
#include "services/CameraService.h"
#include "services/WebControlService.h"
#include "ui/FaceRenderer.h"

class StackChanApp {
public:
  void setup();
  void loop();

private:
  StackChanBoard _board;
  FaceRenderer _face;
  AudioService _audio;
  CameraService _camera;
  WebControlService _web;
  String _serialLine;
  uint32_t _lastFrameMs = 0;

  void handleInputs();
  void handleSerial();
  void runCommand(String line);
  void applyExpression(Expression expression);
  void printHelp() const;
  void printStatus();
};
