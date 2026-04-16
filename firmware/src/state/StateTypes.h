#pragma once

#include <Arduino.h>

enum class AppState {
  Connecting,
  Ready,
  Recording,
  Thinking,
  Playing,
  Error,
};

struct DisplayState {
  AppState appState = AppState::Connecting;
  String headerLeft;
  String headerRight;
  String bodyText;
  String footerLeft;
  String footerRight;
};
