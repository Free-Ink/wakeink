#pragma once

// Minimal text/primitive renderer for FreeInk's 1-bit framebuffer.
//
// Fonts are 1-bit glyph tables generated from Noto Sans (SIL OFL 1.1 — see
// fonts/OFL.txt) by scripts/gen_fonts.py. ASCII 0x20-0x7E; common non-ASCII
// punctuation is collapsed to ASCII stand-ins.
//
// Framebuffer layout (FreeInkDisplay): row-major, 8 pixels per byte,
// MSB = leftmost pixel, 1 = white, 0 = black.

#include <Arduino.h>

struct GlyphDef {
  uint32_t bitmapOffset;  // into FontDef::bitmap
  uint8_t width;          // bitmap size in pixels
  uint8_t height;
  uint8_t xAdvance;       // cursor advance
  int8_t xOffset;         // bitmap left relative to cursor
  int8_t yOffset;         // bitmap top relative to baseline (negative = above)
};

struct FontDef {
  const uint8_t* bitmap;  // bit-packed glyph rows, MSB first
  const GlyphDef* glyph;  // [last - first + 1]
  uint16_t first, last;
  uint8_t yAdvance;       // line height
  int8_t ascent;          // baseline offset from top of line box
};

class GfxText {
 public:
  GfxText(uint8_t* framebuffer, uint16_t width, uint16_t height)
      : fb(framebuffer), w(width), h(height), wBytes(width / 8) {}

  void setPixel(int x, int y, bool black);
  void fillRect(int x, int y, int rw, int rh, bool black);
  void hLine(int x, int y, int len, bool black) { fillRect(x, y, len, 1, black); }
  void vLine(int x, int y, int len, bool black) { fillRect(x, y, 1, len, black); }
  void drawRect(int x, int y, int rw, int rh, bool black);

  // Draws one pre-laid-out line: top-left of the line box at (x, y) — i.e. y is
  // the top of the ascent, not the baseline. Returns the x after the last
  // glyph. `black == false` draws white-on-black (inverted banners). Wrapping,
  // alignment, and ellipsis live above this, in ui::layoutText.
  int drawText(const FontDef* font, const char* utf8, int x, int y, bool black = true);

  // Pixel width of the rendered string (no clipping).
  int textWidth(const FontDef* font, const char* utf8);

  static int lineHeight(const FontDef* font) { return font->yAdvance; }

 private:
  uint8_t* fb;
  uint16_t w, h, wBytes;

  void drawGlyph(const FontDef* font, uint16_t cp, int x, int baseline, bool black);
  const GlyphDef* glyphFor(const FontDef* font, uint16_t cp) const;
  static uint16_t nextCodepoint(const char*& p);
  static uint16_t mapToAscii(uint32_t cp);
};
