#pragma once

#include <Arduino.h>

enum class AppState {
  Connecting,
  Ready,
  Recording,
  Thinking,
  Playing,
  ConfirmReset,
  Error,
};

struct DisplayState {
  AppState appState = AppState::Connecting;
  String headerLeft;
  String headerRight;
  String bodyText;
  String footerLeft;
  String footerRight;
  bool showRecordingProgress = false;
  float recordingProgress = 0.0f;
};
