#pragma once

#include "../Config.h"
#include "state/StateTypes.h"
#include <Arduino.h>
#include <M5Unified.h>

class TextDisplay {
public:
  static constexpr int kCharsPerLine = 29;
  static constexpr int kLines = 8;
  static constexpr int kChatRows = 7;
  // Body image bounding box, matching designs.md "Images" / Layout sections.
  static constexpr int kImageX = 4;
  static constexpr int kImageY = 4;
  static constexpr int kImageW = 232;
  static constexpr int kImageH = 112;

  // Custom 8x16 glyphs grafted onto the ASCII font. These codepoints are
  // unused control characters in normal text, so we repurpose them as
  // printable iconography and pass them through fitLine / wrapBodyText.
  static constexpr char kGlyphTriangleDown = '\x01';
  static constexpr char kGlyphBulletFilled = '\x02';
  static constexpr char kGlyphBulletHollow = '\x03';

  void init();
  void setBrightness(uint8_t brightness);
  void render(const DisplayState &state);
  int pageCountForText(const String &text) const;
  String layoutTextForReveal(const String &text) const;
  int wrappedRowCount(const String &text) const;

  // Maximum stored flipbook animation frames (matches the show_animation
  // tool's 5-frame cap; a plain image is one frame).
  static constexpr int kMaxAnimationFrames = 5;

  // Store a 1-bit packed bitmap (MSB first) for the body image area. Pixel
  // dimensions must match kImageW x kImageH; mismatches return false.
  // Replaces any stored image or animation (this becomes frame 0).
  bool setImage(const uint8_t *packed, size_t packedLen, int width, int height);
  // Append one flipbook animation frame after setImage stored frame 0.
  // Dimensions must match the stored image; fails when full or without a
  // base image.
  bool addAnimationFrame(const uint8_t *packed, size_t packedLen, int width,
                         int height);
  int animationFrameCount() const { return _frameCount; }
  // Select which stored frame render() draws (clamped to the stored range).
  void setActiveAnimationFrame(int index);
  int activeAnimationFrame() const { return _activeFrame; }
  void clearImage();
  bool hasImage() const { return _frameCount > 0; }

private:
  static constexpr int kBodyRows = 7;
  static constexpr int kFooterRow = 7;
  static constexpr size_t kCanvasBytes =
      static_cast<size_t>(SCREEN_WIDTH_PX) * SCREEN_HEIGHT_PX * sizeof(uint16_t);

  mutable M5Canvas _canvas;
  bool _canvasReady = false;
  uint16_t *_previousCanvas = nullptr;
  bool _hasPreviousCanvas = false;

  // Packed 1-bit frame buffers: slot 0 is the image, slots 1+ are extra
  // flipbook animation frames.
  uint8_t *_frames[kMaxAnimationFrames] = {nullptr};
  int _frameCount = 0;
  int _activeFrame = 0;
  int _imageWidth = 0;
  int _imageHeight = 0;

  uint8_t *allocFrame(size_t bytes) const;

  String fitLine(const String &text) const;
  String mergeEdgeText(const String &left, const String &right) const;
  String spaces(int count) const;
  int wrapBodyText(const String &text, String out[], int maxRows) const;
  void flushCanvas(bool forceFull = false);
  void drawLine(int row, const String &text, uint16_t color) const;
  void drawCharCell(int x, int yTop, char c, uint16_t color) const;
  void drawBitmapGlyph(int x, int yTop, const uint8_t *bits,
                       uint16_t color) const;
  void drawGlyphAtRight(int row, char glyph, uint16_t color) const;
  void drawPageIndicator(int pageIndex, int pageCount) const;
  void drawMenu(const DisplayState &state) const;
  void drawStoredImage(uint16_t color) const;
  void drawAlarm(const DisplayState &state) const;
  void drawBellIcon(int cx, int cy, uint16_t color) const;
};
