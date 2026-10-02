#pragma once

#include <Arduino.h>

/**
 * @brief Bitmap font tables and helpers for the SmartBrick display font.
 */
namespace SmartBrickFont {

/**
 * @brief Glyph metadata describing where a character lives in the bitmap table.
 */
struct Glyph {
  /// Offset into kBitmap where this glyph begins.
  uint16_t bitmapIndex;

  /// Glyph bounding-box width in pixels.
  uint8_t boxW;

  /// Glyph bounding-box height in pixels.
  uint8_t boxH;

  /// Horizontal drawing offset from the cell origin.
  int8_t ofsX;

  /// Vertical drawing offset from the baseline.
  int8_t ofsY;
};

/// First supported printable ASCII character.
constexpr int kFirstChar = 32;

/// Last supported printable ASCII character.
constexpr int kLastChar = 126;

/// Fixed character cell width in pixels.
constexpr int kCellW = 16;

/// Fixed character cell height in pixels.
constexpr int kCellH = 32;

/// Vertical distance between rendered text rows.
constexpr int kLineHeight = 30;

/// Baseline offset within a cell.
constexpr int kBaseline = 6;

/// Number of glyphs stored in the font table.
constexpr int kGlyphCount = 96;

/// Packed glyph bitmap data stored in program memory.
extern const uint8_t kBitmap[] PROGMEM;

/// Glyph metadata table stored in program memory.
extern const Glyph kGlyphs[] PROGMEM;

/**
 * @brief Look up glyph metadata for a printable ASCII character.
 * @param c Character to look up.
 * @return Pointer to glyph metadata, or null when unsupported.
 */
const Glyph *glyphFor(char c);

/**
 * @brief Test whether a glyph pixel is on at local glyph coordinates.
 * @param glyph Glyph metadata.
 * @param x Horizontal coordinate inside the glyph box.
 * @param y Vertical coordinate inside the glyph box.
 * @return True when the glyph pixel should be lit.
 */
bool glyphPixelOn(const Glyph &glyph, int x, int y);

} // namespace SmartBrickFont
