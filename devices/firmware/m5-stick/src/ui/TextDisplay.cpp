#include "TextDisplay.h"

#include "../Config.h"
#include <M5Unified.h>
#include <string.h>

namespace {
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_GRAY = 0x7BEF;
constexpr int LINE_HEIGHT = 16;

// Tilted bell glyph for the alarm screen — 1-bit packed, MSB-first, row-major.
// Generated from designs/bell-source.jpg at 16x16.
constexpr int kBellW = 16;
constexpr int kBellH = 16;
constexpr int kBellRowBytes = 2;
constexpr uint8_t kBellBits[] = {
    0x38, 0x00,
    0x28, 0x00,
    0x3F, 0x00,
    0x3F, 0x80,
    0x3F, 0xC0,
    0x7F, 0xC0,
    0x7F, 0xE0,
    0x7F, 0xC0,
    0x3F, 0xFC,
    0x3F, 0xC0,
    0x3F, 0x00,
    0x39, 0x80,
    0x31, 0xC0,
    0x21, 0xF0,
    0x00, 0x00,
    0x00, 0x00,
};

// Custom 8x16 glyphs grafted onto AsciiFont8x16 at unused control codepoints.
// Each row is a single byte, MSB = leftmost pixel. The glyph occupies one
// 8x16 character cell so it flows through the normal text layout.
constexpr uint8_t kGlyphTriangleDownBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFE, // X X X X X X X .
    0x7C, // . X X X X X . .
    0x38, // . . X X X . . .
    0x10, // . . . X . . . .
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kGlyphBulletFilledBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, // . . X X X . . .
    0x7C, // . X X X X X . .
    0x7C, // . X X X X X . .
    0x7C, // . X X X X X . .
    0x38, // . . X X X . . .
    0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr uint8_t kGlyphBulletHollowBits[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, // . . X X X . . .
    0x44, // . X . . . X . .
    0x44, // . X . . . X . .
    0x44, // . X . . . X . .
    0x38, // . . X X X . . .
    0x00, 0x00, 0x00, 0x00, 0x00,
};
} // namespace

void TextDisplay::init() {
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::AsciiFont8x16);
  M5.Display.setTextSize(1);
  M5.Display.fillScreen(COLOR_BLACK);
  _canvas.setColorDepth(16);
  _canvasReady = _canvas.createSprite(SCREEN_WIDTH_PX, SCREEN_HEIGHT_PX) != nullptr;
  if (_canvasReady) {
    _previousCanvas = static_cast<uint16_t *>(ps_malloc(kCanvasBytes));
    if (!_previousCanvas) {
      _previousCanvas = static_cast<uint16_t *>(malloc(kCanvasBytes));
    }
    if (!_previousCanvas) {
      Serial.println("[Display] Dirty buffer unavailable; full canvas flush");
    }
  }
}

void TextDisplay::setBrightness(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
  if (brightness > 0) {
    _hasPreviousCanvas = false;
  }
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

  if (state.alarmActive) {
    drawAlarm(state);
    if (_canvasReady) {
      flushCanvas();
    }
    return;
  }

  const bool hasHeader = !state.headerLeft.isEmpty() || !state.headerRight.isEmpty();
  const bool hasFooterText =
      !state.footerLeft.isEmpty() || !state.footerRight.isEmpty();
  if (hasHeader) {
    drawLine(0, mergeEdgeText(state.headerLeft, state.headerRight), COLOR_GRAY);
  }

  if (state.showMenu) {
    drawMenu(state);
  } else {
    const int bodyStart = hasHeader ? 1 : 0;
    const int bodyEnd = kLines - 1;
    const int bodyRows = bodyEnd - bodyStart;
    const bool imagePage = state.imagePresent && hasImage();
    String wrapped[32];
    const int wrappedCount = wrapBodyText(state.bodyText, wrapped, 32);
    const int textPageCount = max(1, (wrappedCount + bodyRows - 1) / bodyRows);
    const int totalPages =
        (imagePage ? 1 : 0) + (state.bodyText.isEmpty() ? 0 : textPageCount);
    const int pageCount = max(1, totalPages);
    const int safePageIndex =
        constrain(state.pageIndex, 0, max(0, pageCount - 1));
    const uint16_t bodyColor = state.bodyDim ? COLOR_GRAY : COLOR_WHITE;

    if (imagePage && safePageIndex == 0) {
      // The image dims with the same color the body text uses while
      // recording/thinking.
      drawStoredImage(bodyColor);
    } else {
      const int textPageIndex = imagePage ? safePageIndex - 1 : safePageIndex;
      for (int i = 0; i < bodyRows; i++) {
        const int lineIndex = textPageIndex * bodyRows + i;
        drawLine(bodyStart + i,
                 lineIndex < wrappedCount ? wrapped[lineIndex] : "", bodyColor);
      }
    }
    if (pageCount > 1) {
      drawPageIndicator(safePageIndex, pageCount);
    }
  }

  if (hasFooterText) {
    drawLine(kFooterRow, mergeEdgeText(state.footerLeft, state.footerRight),
             COLOR_GRAY);
  }

  if (_canvasReady) {
    flushCanvas();
  }
}

uint8_t *TextDisplay::allocFrame(size_t bytes) const {
  return static_cast<uint8_t *>(malloc(bytes));
}

bool TextDisplay::setImage(const uint8_t *packed, size_t packedLen, int width,
                           int height) {
  if (!packed || width != kImageW || height != kImageH) {
    Serial.printf("[Display] Image rejected: %dx%d (expected %dx%d)\n", width,
                  height, kImageW, kImageH);
    return false;
  }
  const size_t expectedBytes = static_cast<size_t>((width * height + 7) / 8);
  if (packedLen < expectedBytes) {
    Serial.printf("[Display] Image too short: %u bytes (expected %u)\n",
                  static_cast<unsigned>(packedLen),
                  static_cast<unsigned>(expectedBytes));
    return false;
  }

  uint8_t *frame = allocFrame(expectedBytes);
  if (!frame) {
    Serial.println("[Display] Failed to allocate image buffer");
    return false;
  }
  memcpy(frame, packed, expectedBytes);
  clearImage();
  _frames[0] = frame;
  _frameCount = 1;
  _activeFrame = 0;
  _imageWidth = width;
  _imageHeight = height;
  return true;
}

bool TextDisplay::addAnimationFrame(const uint8_t *packed, size_t packedLen,
                                    int width, int height) {
  if (_frameCount == 0) {
    Serial.println("[Display] Animation frame rejected: no base image");
    return false;
  }
  if (!packed || width != _imageWidth || height != _imageHeight) {
    Serial.printf("[Display] Animation frame rejected: %dx%d (expected %dx%d)\n",
                  width, height, _imageWidth, _imageHeight);
    return false;
  }
  if (_frameCount >= kMaxAnimationFrames) {
    Serial.println("[Display] Animation frame rejected: frame store full");
    return false;
  }
  const size_t expectedBytes = static_cast<size_t>((width * height + 7) / 8);
  if (packedLen < expectedBytes) {
    Serial.printf("[Display] Animation frame too short: %u bytes (expected %u)\n",
                  static_cast<unsigned>(packedLen),
                  static_cast<unsigned>(expectedBytes));
    return false;
  }
  uint8_t *frame = allocFrame(expectedBytes);
  if (!frame) {
    Serial.println("[Display] Failed to allocate animation frame buffer");
    return false;
  }
  memcpy(frame, packed, expectedBytes);
  _frames[_frameCount++] = frame;
  return true;
}

void TextDisplay::setActiveAnimationFrame(int index) {
  if (_frameCount == 0) {
    _activeFrame = 0;
    return;
  }
  _activeFrame = constrain(index, 0, _frameCount - 1);
}

void TextDisplay::clearImage() {
  for (int i = 0; i < kMaxAnimationFrames; i++) {
    if (_frames[i]) {
      free(_frames[i]);
      _frames[i] = nullptr;
    }
  }
  _frameCount = 0;
  _activeFrame = 0;
  _imageWidth = 0;
  _imageHeight = 0;
}

void TextDisplay::drawStoredImage(uint16_t color) const {
  if (_frameCount == 0) return;
  const uint8_t *frame = _frames[constrain(_activeFrame, 0, _frameCount - 1)];
  if (!frame) return;
  const int w = _imageWidth;
  const int h = _imageHeight;
  for (int y = 0; y < h; y++) {
    const int rowStart = y * w;
    for (int x = 0; x < w; x++) {
      const int bit = rowStart + x;
      const uint8_t byte = frame[bit >> 3];
      const bool on = (byte >> (7 - (bit & 7))) & 1;
      if (!on) continue; // background already black
      if (_canvasReady) {
        _canvas.drawPixel(kImageX + x, kImageY + y, color);
      } else {
        M5.Display.drawPixel(kImageX + x, kImageY + y, color);
      }
    }
  }
}

int TextDisplay::pageCountForText(const String &text) const {
  String wrapped[32];
  const int wrappedCount = wrapBodyText(text, wrapped, 32);
  return max(1, (wrappedCount + kBodyRows - 1) / kBodyRows);
}

String TextDisplay::layoutTextForReveal(const String &text) const {
  String wrapped[32];
  const int wrappedCount = wrapBodyText(text, wrapped, 32);
  String out;

  for (int i = 0; i < wrappedCount; i++) {
    if (i > 0) {
      out += '\n';
    }
    out += wrapped[i];
  }

  return out;
}

int TextDisplay::wrappedRowCount(const String &text) const {
  if (text.isEmpty()) {
    return 0;
  }
  String wrapped[32];
  return wrapBodyText(text, wrapped, 32);
}

String TextDisplay::fitLine(const String &text) const {
  String out;
  out.reserve(kCharsPerLine);

  for (int i = 0; i < static_cast<int>(text.length()) && out.length() < kCharsPerLine;
       i++) {
    const char c = text[i];
    if ((c >= 32 && c <= 126) || c == kGlyphTriangleDown ||
        c == kGlyphBulletFilled || c == kGlyphBulletHollow) {
      out += c;
    } else {
      out += ' ';
    }
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

    if ((c >= 32 && c <= 126) || c == kGlyphTriangleDown ||
        c == kGlyphBulletFilled || c == kGlyphBulletHollow) {
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

void TextDisplay::flushCanvas(bool forceFull) {
  if (!_canvasReady) {
    return;
  }

  uint16_t *current = static_cast<uint16_t *>(_canvas.getBuffer());
  if (!current) {
    return;
  }

  if (forceFull || !_previousCanvas || !_hasPreviousCanvas) {
    _canvas.pushSprite(&M5.Display, 0, 0);
    if (_previousCanvas) {
      memcpy(_previousCanvas, current, kCanvasBytes);
      _hasPreviousCanvas = true;
    }
    return;
  }

  constexpr size_t kRowBytes = SCREEN_WIDTH_PX * sizeof(uint16_t);
  int minY = SCREEN_HEIGHT_PX;
  int maxY = -1;

  for (int y = 0; y < SCREEN_HEIGHT_PX; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    if (memcmp(current + offset, _previousCanvas + offset, kRowBytes) == 0) {
      continue;
    }
    minY = min(minY, y);
    maxY = max(maxY, y);
  }

  if (maxY < minY) {
    return;
  }

  int minX = SCREEN_WIDTH_PX;
  int maxX = -1;
  for (int y = minY; y <= maxY; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX;
    const uint16_t *row = current + offset;
    const uint16_t *prev = _previousCanvas + offset;
    if (memcmp(row, prev, kRowBytes) == 0) {
      continue;
    }

    int left = 0;
    while (left < SCREEN_WIDTH_PX && row[left] == prev[left]) {
      left++;
    }

    int right = SCREEN_WIDTH_PX - 1;
    while (right >= left && row[right] == prev[right]) {
      right--;
    }

    minX = min(minX, left);
    maxX = max(maxX, right);
  }

  if (maxX < minX) {
    return;
  }

  const int dirtyW = maxX - minX + 1;
  const int dirtyH = maxY - minY + 1;
  const int dirtyPixels = dirtyW * dirtyH;
  const int screenPixels = SCREEN_WIDTH_PX * SCREEN_HEIGHT_PX;

  if (dirtyPixels > screenPixels / 3) {
    _canvas.pushSprite(&M5.Display, 0, 0);
    memcpy(_previousCanvas, current, kCanvasBytes);
    _hasPreviousCanvas = true;
    return;
  }

  for (int y = minY; y <= maxY; y++) {
    const uint16_t *row =
        current + static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    M5.Display.pushImage(minX, y, dirtyW, 1, row);
  }

  for (int y = minY; y <= maxY; y++) {
    const size_t offset = static_cast<size_t>(y) * SCREEN_WIDTH_PX + minX;
    memcpy(_previousCanvas + offset, current + offset,
           dirtyW * sizeof(uint16_t));
  }
}

void TextDisplay::drawLine(int row, const String &text, uint16_t color) const {
  const String fitted = fitLine(text);
  const int yTop = row * LINE_HEIGHT;
  int x = 4;
  for (int i = 0; i < static_cast<int>(fitted.length()); i++) {
    drawCharCell(x, yTop, fitted[i], color);
    x += 8;
  }
}

void TextDisplay::drawCharCell(int x, int yTop, char c, uint16_t color) const {
  if (c == kGlyphTriangleDown) {
    drawBitmapGlyph(x, yTop, kGlyphTriangleDownBits, color);
    return;
  }
  if (c == kGlyphBulletFilled) {
    drawBitmapGlyph(x, yTop, kGlyphBulletFilledBits, color);
    return;
  }
  if (c == kGlyphBulletHollow) {
    drawBitmapGlyph(x, yTop, kGlyphBulletHollowBits, color);
    return;
  }
  if (_canvasReady) {
    _canvas.setTextColor(color);
    _canvas.setCursor(x, yTop);
    _canvas.print(c);
    return;
  }
  M5.Display.setTextColor(color);
  M5.Display.setCursor(x, yTop);
  M5.Display.print(c);
}

void TextDisplay::drawBitmapGlyph(int x, int yTop, const uint8_t *bits,
                                  uint16_t color) const {
  for (int row = 0; row < LINE_HEIGHT; row++) {
    const uint8_t byte = bits[row];
    if (byte == 0) continue;
    for (int col = 0; col < 8; col++) {
      if (!((byte >> (7 - col)) & 1)) continue;
      if (_canvasReady) {
        _canvas.drawPixel(x + col, yTop + row, color);
      } else {
        M5.Display.drawPixel(x + col, yTop + row, color);
      }
    }
  }
}

void TextDisplay::drawGlyphAtRight(int row, char glyph, uint16_t color) const {
  const int x = 4 + (kCharsPerLine - 1) * 8;
  const int y = row * LINE_HEIGHT;
  drawCharCell(x, y, glyph, color);
}

void TextDisplay::drawPageIndicator(int pageIndex, int pageCount) const {
  if (pageCount <= 1) {
    return;
  }
  // Up to 8 dots — solid for current page, hollow for the rest. Past that,
  // fall back to a single down-triangle (more) or hollow bullet (last).
  constexpr int kMaxDots = 8;
  String indicator;
  if (pageCount <= kMaxDots) {
    for (int i = 0; i < pageCount; i++) {
      indicator += (i == pageIndex) ? kGlyphBulletFilled : kGlyphBulletHollow;
    }
  } else {
    indicator += (pageIndex >= pageCount - 1) ? kGlyphBulletHollow
                                              : kGlyphTriangleDown;
  }
  const int charCount = static_cast<int>(indicator.length());
  const int yTop = kFooterRow * LINE_HEIGHT;
  const int xStart = 4 + (kCharsPerLine - charCount) * 8;
  // Footer text was already drawn; clear behind the indicator so multi-dot
  // rows don't overlap with footerLeft / footerRight.
  if (_canvasReady) {
    _canvas.fillRect(xStart, yTop, charCount * 8, LINE_HEIGHT, COLOR_BLACK);
  } else {
    M5.Display.fillRect(xStart, yTop, charCount * 8, LINE_HEIGHT, COLOR_BLACK);
  }
  int x = xStart;
  for (int i = 0; i < charCount; i++) {
    drawCharCell(x, yTop, indicator[i], COLOR_GRAY);
    x += 8;
  }
}

void TextDisplay::drawBellIcon(int cx, int cy, uint16_t color) const {
  const int originX = cx - kBellW / 2;
  const int originY = cy - kBellH / 2;
  for (int y = 0; y < kBellH; y++) {
    const uint8_t *row = kBellBits + y * kBellRowBytes;
    for (int x = 0; x < kBellW; x++) {
      const uint8_t byte = row[x >> 3];
      if (!((byte >> (7 - (x & 7))) & 1)) continue;
      if (_canvasReady) {
        _canvas.drawPixel(originX + x, originY + y, color);
      } else {
        M5.Display.drawPixel(originX + x, originY + y, color);
      }
    }
  }
}

void TextDisplay::drawAlarm(const DisplayState &state) const {
  const String title = state.alarmTitle.isEmpty() ? String("ALARM") : state.alarmTitle;
  const String safeTitle = fitLine(title);
  const bool hasDetail = !state.alarmDetail.isEmpty();

  // Bell + title share a row, vertically centered. Detail line sits below when
  // present; when there's no detail, the whole composition sits dead-center.
  const int titlePixelWidth = static_cast<int>(safeTitle.length()) * 16;
  const int gap = 8;
  const int rowWidth = kBellW + gap + titlePixelWidth;
  const int rowX = max(0, (SCREEN_WIDTH_PX - rowWidth) / 2);
  const int rowTopY = hasDetail ? 44 : 60;
  const int bellCx = rowX + kBellW / 2;
  const int bellCy = rowTopY + kBellH / 2;
  drawBellIcon(bellCx, bellCy, COLOR_WHITE);

  const int titleX = rowX + kBellW + gap;
  const int titleY = rowTopY;
  if (_canvasReady) {
    _canvas.setTextColor(COLOR_WHITE);
    _canvas.setTextSize(2);
    _canvas.setCursor(titleX, titleY);
    _canvas.print(safeTitle);
    _canvas.setTextSize(1);
  } else {
    M5.Display.setTextColor(COLOR_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(titleX, titleY);
    M5.Display.print(safeTitle);
    M5.Display.setTextSize(1);
  }

  if (hasDetail) {
    const String safeDetail = fitLine(state.alarmDetail);
    const int detailX =
        max(0, (SCREEN_WIDTH_PX - static_cast<int>(safeDetail.length()) * 8) / 2);
    if (_canvasReady) {
      _canvas.setTextColor(COLOR_GRAY);
      _canvas.setCursor(detailX, 76);
      _canvas.print(safeDetail);
    } else {
      M5.Display.setTextColor(COLOR_GRAY);
      M5.Display.setCursor(detailX, 76);
      M5.Display.print(safeDetail);
    }
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
