#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
constexpr uint16_t COLOR_DARK_GRAY = 0x39E7;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_BLUE = 0x041F;
constexpr int LINE_HEIGHT = 16;
constexpr int PAGE_DOT_RADIUS = 2;
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
  _canvas.setColorDepth(16);
  _canvas.createSprite(SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX);
  _canvasReady = true;
}

void TextDisplay::setBrightness(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
}

void TextDisplay::render(const DisplayState &state) {
  if (_canvasReady) {
    _canvas.fillScreen(COLOR_BLACK);
    _canvas.setFont(&fonts::AsciiFont8x16);
    _canvas.setTextSize(1);
  } else {
    M5.Display.fillScreen(COLOR_BLACK);
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.setTextSize(1);
  }

  drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  if (state.showMenu) {
    drawMenu(state);
  } else {
    String wrapped[32];
    const int wrappedCount = wrapBodyText(state.bodyText, wrapped, 32);
    const int pageCount =
        max(1, (wrappedCount + kBodyRows - 1) / kBodyRows);
    const int safePageIndex =
        constrain(state.pageIndex, 0, max(0, pageCount - 1));

    for (int i = 0; i < kBodyRows; i++) {
      const int lineIndex = safePageIndex * kBodyRows + i;
      drawLine(i + 1, lineIndex < wrappedCount ? wrapped[lineIndex] : "",
               COLOR_WHITE);
    }
    if (pageCount > 1) {
      drawPageIndicator(safePageIndex, pageCount);
    }
  }

  drawLine(kFooterRow, mergeEdgeText(state.footerLeft, state.footerRight),
           COLOR_GRAY);

  if (state.showRecordingProgress) {
    drawRecordingProgress(state.recordingProgress);
  }

  if (_canvasReady) {
    _canvas.pushSprite(&M5.Display, 0, 0);
  }
}

int TextDisplay::pageCountForText(const String &text) const {
  String wrapped[32];
  const int wrappedCount = wrapBodyText(text, wrapped, 32);
  return max(1, (wrappedCount + kBodyRows - 1) / kBodyRows);
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

int TextDisplay::wrapBodyText(const String &text, String out[], int maxRows) const {
  for (int i = 0; i < maxRows; i++) {
    out[i] = "";
  }

  int row = 0;
  String line;
  String word;

  auto flushLine = [&]() {
    if (row >= maxRows) {
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
      while (start < token.length() && row < maxRows) {
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
    if (row >= maxRows) {
      return;
    }

    if (token.length() <= kCharsPerLine) {
      line = token;
      return;
    }

    int start = 0;
    while (start < token.length() && row < maxRows) {
      out[row++] = fitLine(token.substring(start, start + kCharsPerLine));
      start += kCharsPerLine;
    }
    line = "";
  };

  for (int i = 0; i <= static_cast<int>(text.length()) && row < maxRows; i++) {
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

  if (row < maxRows) {
    appendWord(word);
    if (!line.isEmpty() && row < maxRows) {
      out[row] = fitLine(line);
      row++;
    }
  }

  return max(1, row);
}

void TextDisplay::drawLine(int row, const String &text, uint16_t color) const {
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(4, row * LINE_HEIGHT);
    _canvas.print(fitLine(text));
    return;
  }

  M5.Display.setTextColor(color);
  M5.Display.setCursor(4, row * LINE_HEIGHT);
  M5.Display.print(fitLine(text));
}

void TextDisplay::drawGlyphAtRight(int row, char glyph, uint16_t color) const {
  const int x = 4 + (kCharsPerLine - 1) * 8;
  const int y = row * LINE_HEIGHT;
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(x, y);
    _canvas.print(glyph);
    return;
  }
  M5.Display.setTextColor(color);
  M5.Display.setCursor(x, y);
  M5.Display.print(glyph);
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

  if (_canvasReady) {
    _canvas.drawRect(x, y, barWidth, height, COLOR_GRAY);
  } else {
    M5.Display.drawRect(x, y, barWidth, height, COLOR_GRAY);
  }
  if (clampedHeight > 2) {
    if (_canvasReady) {
      _canvas.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                       clampedHeight - 2, COLOR_RED);
    } else {
      M5.Display.fillRect(x + 1, y + height - clampedHeight + 1, barWidth - 2,
                          clampedHeight - 2, COLOR_RED);
    }
  }
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }

  const int totalWidth = pageCount * (PAGE_DOT_RADIUS * 2 + 4) - 4;
  int x = (SCREEN_WIDTH_PX - totalWidth) / 2;
  const int y = SCREEN_HEIGHT_PX - 8;
  for (int i = 0; i < pageCount; i++) {
    const uint16_t color = i == pageIndex ? COLOR_WHITE : COLOR_DARK_GRAY;
    if (_canvasReady) {
      _canvas.fillCircle(x + PAGE_DOT_RADIUS, y, PAGE_DOT_RADIUS, color);
    } else {
      M5.Display.fillCircle(x + PAGE_DOT_RADIUS, y, PAGE_DOT_RADIUS, color);
    }
    x += PAGE_DOT_RADIUS * 2 + 4;
  }
}

void TextDisplay::drawMenu(const DisplayState &state) const {
  const int startRow = kLines - state.menuItemCount;
  for (int i = 0; i < state.menuItemCount; i++) {
    const int row = startRow + i;
    const bool selected = i == state.menuSelectedIndex;
    const String prefix = selected ? "> " : "  ";
    drawLine(row, prefix + state.menuItems[i],
             selected ? COLOR_WHITE : COLOR_GRAY);
  }

  if (state.menuHasMoreAbove) {
    drawGlyphAtRight(startRow - 1 >= 1 ? startRow - 1 : 1, 'v', COLOR_GRAY);
  }
  if (state.menuHasMoreBelow) {
    drawGlyphAtRight(kLines - 1, 'v', COLOR_GRAY);
  }
}
