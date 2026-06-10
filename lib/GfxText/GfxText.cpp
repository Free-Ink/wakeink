#include "GfxText.h"

#include <cstring>

void GfxText::setPixel(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  uint8_t& byte = fb[y * wBytes + (x >> 3)];
  const uint8_t mask = 0x80 >> (x & 7);
  if (black) {
    byte &= ~mask;
  } else {
    byte |= mask;
  }
}

void GfxText::fillRect(int x, int y, int rw, int rh, bool black) {
  for (int yy = y; yy < y + rh; ++yy) {
    for (int xx = x; xx < x + rw; ++xx) {
      setPixel(xx, yy, black);
    }
  }
}

void GfxText::drawRect(int x, int y, int rw, int rh, bool black) {
  hLine(x, y, rw, black);
  hLine(x, y + rh - 1, rw, black);
  vLine(x, y, rh, black);
  vLine(x + rw - 1, y, rh, black);
}

const GlyphDef* GfxText::glyphFor(const FontDef* font, uint16_t cp) const {
  if (cp < font->first || cp > font->last) return nullptr;
  return &font->glyph[cp - font->first];
}

// Collapses common non-ASCII codepoints to printable ASCII stand-ins; the
// generated fonts only cover 0x20-0x7E. Unmappable codepoints (emoji, CJK)
// return 1 — below the font's range, so they render as nothing instead of
// turning "🏠 Standup" into "? Standup".
uint16_t GfxText::mapToAscii(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (uint16_t)cp;
  switch (cp) {
    case 0x2018:
    case 0x2019:
    case 0x02BC: return '\'';
    case 0x201C:
    case 0x201D: return '"';
    case 0x2013:
    case 0x2014:
    case 0x2212: return '-';
    case 0x2026: return '.';  // "…" degrades to a single dot
    case 0x00A0: return ' ';
    default: break;
  }
  // Latin-1 accents fold to their base letter so names stay readable.
  static const char* fold =
      "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";  // U+00C0..U+00FF
  if (cp >= 0xC0 && cp <= 0xFF) return (uint16_t)fold[cp - 0xC0];
  return 1;  // skip silently (no glyph)
}

uint16_t GfxText::nextCodepoint(const char*& p) {
  const uint8_t c = (uint8_t)*p;
  if (c == 0) return 0;
  uint32_t cp = 0;
  int extra = 0;
  if (c < 0x80) {
    cp = c;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07;
    extra = 3;
  } else {
    ++p;
    return '?';
  }
  ++p;
  for (int i = 0; i < extra; ++i) {
    if (((uint8_t)*p & 0xC0) != 0x80) break;
    cp = (cp << 6) | ((uint8_t)*p & 0x3F);
    ++p;
  }
  return mapToAscii(cp);
}

void GfxText::drawGlyph(const FontDef* font, uint16_t cp, int x, int baseline, bool black) {
  const GlyphDef* g = glyphFor(font, cp);
  if (!g || g->width == 0) return;
  const uint8_t* bits = font->bitmap + g->bitmapOffset;
  uint32_t bit = 0;
  for (int yy = 0; yy < g->height; ++yy) {
    for (int xx = 0; xx < g->width; ++xx, ++bit) {
      if (bits[bit >> 3] & (0x80 >> (bit & 7))) {
        setPixel(x + g->xOffset + xx, baseline + g->yOffset + yy, black);
      }
    }
  }
}

int GfxText::textWidth(const FontDef* font, const char* utf8) {
  int width = 0;
  const char* p = utf8;
  while (uint16_t cp = nextCodepoint(p)) {
    const GlyphDef* g = glyphFor(font, cp);
    if (g) width += g->xAdvance;
  }
  return width;
}

int GfxText::drawText(const FontDef* font, const char* utf8, int x, int y, bool black) {
  // Single line, no wrapping/clipping: callers (the FreeInkUI DrawTarget) hand
  // us pre-laid-out lines from ui::layoutText, so each already fits.
  const int baseline = y + font->ascent;
  int cursor = x;
  const char* p = utf8;
  while (uint16_t cp = nextCodepoint(p)) {
    const GlyphDef* g = glyphFor(font, cp);
    if (!g) continue;
    drawGlyph(font, cp, cursor, baseline, black);
    cursor += g->xAdvance;
  }
  return cursor;
}
