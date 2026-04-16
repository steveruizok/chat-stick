#pragma once

#include "../state/StateTypes.h"
#include <Arduino.h>

class TextDisplay {
public:
  static constexpr int kCharsPerLine = 29;
  static constexpr int kLines = 7;

  void init();
  void setBrightness(uint8_t brightness);
  void render(const DisplayState &state);

private:
  static constexpr int kBodyRows = 5;

  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  void wrapBodyText(const String &text, String out[kBodyRows]) const;
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawRecordingProgress(float progress) const;
};
