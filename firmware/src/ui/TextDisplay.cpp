#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr int LINE_HEIGHT = 16;
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
}

void TextDisplay::setBrightness(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
}

void TextDisplay::render(const DisplayState &state) {
  String body[kBodyRows];
  wrapBodyText(state.bodyText, body);

  M5.Display.fillScreen(COLOR_BLACK);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);

  drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  for (int i = 0; i < kBodyRows; i++) {
    drawLine(i + 1, body[i], COLOR_WHITE);
  }
  drawLine(6, mergeEdgeText(state.footerLeft, state.footerRight), COLOR_GRAY);

  if (state.showRecordingProgress) {
    drawRecordingProgress(state.recordingProgress);
  }
}

String TextDisplay::fitLine(const String &text) const {
  String out;
  out.reserve(kCharsPerLine);

  for (int i = 0; i < static_cast<int>(text.length()) && out.length() < kCharsPerLine;
       i++) {
    const char c = text[i];
    out += (c >= 32 && c <= 126) ? c : ' ';
  }

  return out;
}

String TextDisplay::mergeEdgeText(const String &left, const String &right) const {
  const String safeLeft = fitLine(left);
  const String safeRight = fitLine(right);

  if (safeLeft.isEmpty()) return safeRight;
  if (safeRight.isEmpty()) return safeLeft;

  if (safeLeft.length() + safeRight.length() >= kCharsPerLine) {
    const int reservedForRight = safeRight.length() + 1;
    const int leftBudget = max(0, kCharsPerLine - reservedForRight);
    return fitLine(safeLeft.substring(0, leftBudget)) + " " + safeRight;
  }

  const int spaces = kCharsPerLine - safeLeft.length() - safeRight.length();
  return safeLeft + this->spaces(spaces) + safeRight;
}

String TextDisplay::spaces(int count) const {
  String out;
  for (int i = 0; i < count; i++) {
    out += ' ';
  }
  return out;
}

void TextDisplay::wrapBodyText(const String &text, String out[kBodyRows]) const {
  for (int i = 0; i < kBodyRows; i++) {
    out[i] = "";
  }

  int row = 0;
  String line;
  String word;

  auto flushLine = [&]() {
    if (row >= kBodyRows) {
      return;
    }
    out[row++] = fitLine(line);
    line = "";
  };

  auto appendWord = [&](const String &token) {
    if (token.isEmpty()) {
      return;
    }

    if (line.isEmpty()) {
      if (token.length() <= kCharsPerLine) {
        line = token;
        return;
      }

      int start = 0;
      while (start < token.length() && row < kBodyRows) {
        out[row++] = fitLine(token.substring(start, start + kCharsPerLine));
        start += kCharsPerLine;
      }
      line = "";
      return;
    }

    const String candidate = line + " " + token;
    if (candidate.length() <= kCharsPerLine) {
      line = candidate;
      return;
    }

    flushLine();
    if (row >= kBodyRows) {
      return;
    }

    if (token.length() <= kCharsPerLine) {
      line = token;
      return;
    }

    int start = 0;
    while (start < token.length() && row < kBodyRows) {
      out[row++] = fitLine(token.substring(start, start + kCharsPerLine));
      start += kCharsPerLine;
    }
    line = "";
  };

  for (int i = 0; i <= static_cast<int>(text.length()) && row < kBodyRows; i++) {
    const char c = i < static_cast<int>(text.length()) ? text[i] : '\n';
    if (c == '\n') {
      appendWord(word);
      word = "";
      flushLine();
      continue;
    }

    if (c == ' ') {
      appendWord(word);
      word = "";
      continue;
    }

    if (c >= 32 && c <= 126) {
      word += c;
    }
  }

  if (row < kBodyRows) {
    appendWord(word);
    if (!line.isEmpty() && row < kBodyRows) {
      out[row] = fitLine(line);
    }
  }
}

void TextDisplay::drawLine(int row, const String &text, uint16_t color) const {
  M5.Display.setTextColor(color);
  M5.Display.setCursor(4, row * LINE_HEIGHT);
  M5.Display.print(fitLine(text));
}

void TextDisplay::drawRecordingProgress(float progress) const {
  const int barWidth = 8;
  const int margin = 2;
  const int x = SCREEN_WIDTH_PX - barWidth - margin;
  const int y = margin;
  const int height = SCREEN_HEIGHT_PX - (margin * 2);
  const int clampedHeight =
      constrain(static_cast<int>(height * constrain(progress, 0.0f, 1.0f)), 0,
                height);

  M5.Display.drawRect(x, y, barWidth, height, COLOR_GRAY);
  if (clampedHeight > 2) {
    M5.Display.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                        clampedHeight - 2, COLOR_RED);
  }
}
