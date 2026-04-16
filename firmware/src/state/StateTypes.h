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

constexpr int MAX_MENU_VISIBLE_ITEMS = 4;

struct DisplayState {
  AppState appState = AppState::Connecting;
  String headerLeft;
  String headerRight;
  String bodyText;
  String footerLeft;
  String footerRight;
  bool showRecordingProgress = false;
  float recordingProgress = 0.0f;
  bool showMenu = false;
  String menuTitle;
  String menuItems[MAX_MENU_VISIBLE_ITEMS];
  int menuItemCount = 0;
  int menuSelectedIndex = 0;
  bool menuHasMoreAbove = false;
  bool menuHasMoreBelow = false;
  int pageIndex = 0;
  int pageCount = 1;
  uint8_t animationPhase = 0;
};
