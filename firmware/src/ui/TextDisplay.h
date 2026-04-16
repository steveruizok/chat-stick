#pragma once

#include "../state/StateTypes.h"
#include <Arduino.h>
#include <M5Unified.h>

class TextDisplay {
public:
  static constexpr int kCharsPerLine = 29;
  static constexpr int kLines = 7;

  void init();
  void setBrightness(uint8_t brightness);
  void render(const DisplayState &state);
  int pageCountForText(const String &text) const;

private:
  static constexpr int kBodyRows = 5;

  mutable M5Canvas _canvas;
  bool _canvasReady = false;

  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  int wrapBodyText(const String &text, String out[], int maxRows) const;
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawRecordingProgress(float progress) const;
  void drawLoadingAnimation(uint8_t phase) const;
  void drawPageIndicator(int pageIndex, int pageCount) const;
  void drawMenu(const DisplayState &state) const;
};
