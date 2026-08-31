#pragma once

#include "../Config.h"
#include "state/StateTypes.h"
#include <Arduino.h>

/**
 * @brief Converts a DisplayState snapshot into pixels for the AMOLED.
 *
 * TextDisplay is intentionally a renderer, not an app-state owner. The
 * controller decides what should be shown; this class handles fixed-cell text
 * layout, menu drawing, optional bitmap drawing, and framebuffer flushing.
 *
 * All body text uses SmartBrickFont as a monospace grid. That is why most text
 * layout is expressed as rows and character cells instead of pixel widths.
 */
class TextDisplay {
public:
  /// Fixed font cell width in pixels.
  static constexpr int kCellW = 16;

  /// Fixed font cell height in pixels.
  static constexpr int kCellH = 32;

  /// Horizontal content inset.
  static constexpr int kInsetX = 16;

  /// Vertical content inset.
  static constexpr int kInsetY = 32;

  /// Drawable content width after insets.
  static constexpr int kContentW = SCREEN_WIDTH_PX - 2 * kInsetX;

  /// Maximum monospace characters per text row.
  static constexpr int kCharsPerLine = kContentW / kCellW;

  /// Maximum chat rows visible per page.
  static constexpr int kChatRows = 11;

  /// Height of a single menu row.
  static constexpr int kMenuRowH = 64;

  /// Top Y coordinate of the menu body.
  static constexpr int kMenuBodyY = 128;

  /// Vertical text offset inside a menu row.
  static constexpr int kMenuRowTextYOffset = 8;

  /// Y coordinate of the page indicator dots.
  static constexpr int kDotY = SCREEN_HEIGHT_PX - 20;

  /// Horizontal spacing between page indicator dots.
  static constexpr int kDotSpacing = 12;

  /// Maximum number of page indicator dots drawn.
  static constexpr int kDotCountMax = 8;

  /// Maximum image bitmap width the device will accept (server-generated
  /// images fill the screen).
  static constexpr int kImageW = IMAGE_TARGET_WIDTH;

  /// Maximum image bitmap height the device will accept.
  static constexpr int kImageH = IMAGE_TARGET_HEIGHT;

  /// Initialize display buffers and hardware state.
  void init();

  /**
   * @brief Set display brightness.
   * @param brightness Backlight level.
   */
  void setBrightness(uint8_t brightness);

  /**
   * @brief Render a complete UI frame.
   * @param state UI state snapshot to draw.
   */
  void render(const DisplayState &state);

  /**
   * @brief Count body pages required to render a block of text.
   * @param text Body text to measure.
   * @return Number of pages required.
   */
  int pageCountForText(const String &text) const;

  /**
   * @brief Wrap text into the exact line breaks used by the reveal animation.
   * @param text Body text to wrap.
   * @return Text with explicit newline breaks matching display layout.
   */
  String layoutTextForReveal(const String &text) const;

  /**
   * @brief Count wrapped rows required to render a block of text.
   * @param text Body text to measure.
   * @return Number of wrapped rows.
   */
  int wrappedRowCount(const String &text) const;

  /**
   * @brief Store a 1-bit packed bitmap for the body image area.
   * @param packed Packed bitmap bytes, MSB first.
   * @param packedLen Number of bytes in packed.
   * @param width Bitmap width in pixels.
   * @param height Bitmap height in pixels.
   * @return True when the bitmap dimensions match the supported image area.
   */
  bool setImage(const uint8_t *packed, size_t packedLen, int width, int height);

  /**
   * @brief Append one flipbook animation frame after setImage stored frame 0.
   * @param packed Packed bitmap bytes, MSB first.
   * @param packedLen Number of bytes in packed.
   * @param width Bitmap width in pixels (must match the stored image).
   * @param height Bitmap height in pixels (must match the stored image).
   * @return True when the frame was stored (fails without a base image or
   *         when the frame store is full).
   */
  bool addAnimationFrame(const uint8_t *packed, size_t packedLen, int width,
                         int height);

  /// Number of stored frames (1 for a plain image, 2+ while animating).
  int animationFrameCount() const { return _frameCount; }

  /// Select which stored frame render() draws (clamped to the stored range).
  void setActiveAnimationFrame(int index);

  /// Index of the frame render() currently draws.
  int activeAnimationFrame() const { return _activeFrame; }

  /// Remove any stored body image (and all animation frames).
  void clearImage();

  /// Write the framebuffer to a stream as hex rows (debug diagnostics).
  void dumpFramebuffer(Stream &out) const;

  /// Push the entire framebuffer to the panel, bypassing the dirty-rect diff.
  void forceFullRepaint() const { flushFrame(true); }

  /// Whether a stored image is currently available for rendering.
  bool hasImage() const { return _frameCount > 0; }

  /// Maximum stored flipbook animation frames (matches the show_animation
  /// tool's 5-frame cap; a plain image is one frame).
  static constexpr int kMaxAnimationFrames = 5;

private:
  /// Footer baseline Y coordinate.
  static constexpr int kFooterY = SCREEN_HEIGHT_PX - kCellH;

  /// Current framebuffer written during render.
  uint16_t *_framebuffer = nullptr;

  /// Previous framebuffer used for diff-based flushes.
  mutable uint16_t *_previousFramebuffer = nullptr;

  /// Whether _previousFramebuffer contains valid frame data.
  mutable bool _hasPreviousFrame = false;

  /// Row span written by draw calls since the last clearFrame. Changed pixels
  /// can only exist where this frame drew content or where the previous frame
  /// had content that is now erased to background, so flushFrame's diff scan
  /// is limited to the union of this span and _prevDrawn*. Sentinel values
  /// (min > max) mean "nothing drawn".
  mutable int _drawnMinY = SCREEN_HEIGHT_PX;
  mutable int _drawnMaxY = -1;

  /// Drawn-row span of the frame held in _previousFramebuffer.
  mutable int _prevDrawnMinY = 0;
  mutable int _prevDrawnMaxY = SCREEN_HEIGHT_PX - 1;

  /// Background color used by the most recent clearFrame.
  mutable uint16_t _lastClearColor = 0x0000;

  /// Packed 1-bit frame buffers: slot 0 is the body image, slots 1+ are
  /// extra flipbook animation frames.
  uint8_t *_frames[kMaxAnimationFrames] = {nullptr};

  /// Number of stored frames.
  int _frameCount = 0;

  /// Frame index render() draws.
  int _activeFrame = 0;

  /// Stored image width in pixels (shared by all frames).
  int _imageWidth = 0;

  /// Stored image height in pixels (shared by all frames).
  int _imageHeight = 0;

  /// Allocate one packed frame buffer, preferring PSRAM.
  uint8_t *allocFrame(size_t bytes) const;

  /// Record rows [y0, y1] as drawn for flushFrame's diff-scan hint.
  void markDrawnRows(int y0, int y1) const;

  /// Fill the current framebuffer with a solid color.
  void clearFrame(uint16_t color) const;

  /// Flush the framebuffer to the display, optionally forcing a full refresh.
  void flushFrame(bool forceFull = false) const;

  /// Write a single pixel into the framebuffer.
  void putPixel(int x, int y, uint16_t color) const;

  /// Fill a rectangle in the framebuffer.
  void fillRect(int x, int y, int w, int h, uint16_t color) const;

  /// Draw a rectangle outline in the framebuffer.
  void drawRect(int x, int y, int w, int h, uint16_t color) const;

  /// Fill a circle in the framebuffer.
  void fillCircle(int cx, int cy, int radius, uint16_t color) const;

  /// Draw plain text starting at pixel coordinates.
  void drawText(int x, int y, const String &text, uint16_t color,
                int maxChars = kCharsPerLine) const;

  /// Measure rendered text width in pixels.
  int textPixelWidth(const String &text) const;

  /// Trim a string so it fits on one rendered line.
  String fitLine(const String &text) const;

  /// Merge left and right edge text into a single line.
  String mergeEdgeText(const String &left, const String &right) const;

  /// Build a string containing a fixed number of spaces.
  String spaces(int count) const;

  /// Wrap body text into display rows.
  int wrapBodyText(const String &text, String out[], int maxRows) const;

  /// Draw one text line at a row index.
  void drawLine(int row, const String &text, uint16_t color) const;

  /**
   * @brief Draw a row whose newest characters are still fading in.
   * @param row Row index within the chat body.
   * @param text Row text.
   * @param baseColor Color a fully faded-in character settles at.
   * @param charsToEnd Rendered characters from this row's start to the newest.
   * @param fadeChars How many trailing characters are still ramping up.
   */
  void drawFadingLine(int row, const String &text, uint16_t baseColor,
                      int charsToEnd, int fadeChars) const;

  /// Draw left and right aligned text on the same row.
  void drawEdgeLine(int row, const String &left, const String &right,
                    uint16_t color) const;

  /// Draw the page indicator dots.
  void drawPageIndicator(int pageIndex, int pageCount) const;

  /// Draw the menu overlay.
  void drawMenu(const DisplayState &state) const;

  /// Draw the currently stored body image in the given color.
  void drawStoredImage(uint16_t color) const;

  /// Draw the timer alarm screen.
  void drawAlarm(const DisplayState &state) const;
};
