#include "GfxTextDrawTarget.h"

#include <fonts/NotoSans10.h>
#include <fonts/NotoSans12.h>
#include <fonts/NotoSans12B.h>
#include <fonts/NotoSans15.h>
#include <fonts/NotoSans15B.h>
#include <fonts/NotoSans21B.h>
#include <fonts/NotoSans28B.h>

namespace wakeink {

using freeink::ui::Color;
using freeink::ui::Paint;
using freeink::ui::PaintKind;
using freeink::ui::Point;
using freeink::ui::Rect;
using freeink::ui::Size;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

const FontDef* GfxTextDrawTarget::fontFor(freeink::ui::FontId font) {
  switch (font) {
    case FONT_TINY: return &NotoSans10;
    case FONT_SMALL: return &NotoSans12;
    case FONT_SMALL_B: return &NotoSans12B;
    case FONT_BODY: return &NotoSans15;
    case FONT_BODY_B: return &NotoSans15B;
    case FONT_TITLE: return &NotoSans21B;
    case FONT_HUGE: return &NotoSans28B;
    default: return &NotoSans15;
  }
}

bool GfxTextDrawTarget::isInk(Color c) {
  return c == Color::Black || c == Color::DarkGray;
}

Size GfxTextDrawTarget::measureText(freeink::ui::FontId font, const char* text, TextStyle) const {
  // style.bold is intentionally ignored: weight is encoded in the FontId
  // (FONT_SMALL vs FONT_SMALL_B), so the glyph table already reflects it.
  const FontDef* f = fontFor(font);
  return Size{static_cast<int16_t>(text ? gfx_.textWidth(f, text) : 0),
              static_cast<int16_t>(f->yAdvance)};
}

int16_t GfxTextDrawTarget::lineHeight(freeink::ui::FontId font) const {
  return static_cast<int16_t>(fontFor(font)->yAdvance);
}

// radius/corners are ignored: this 1-bit panel renders rounded styles as
// square. No firmware screen requests rounding, so it's a silent no-op by
// design rather than an oversight.
void GfxTextDrawTarget::fill(Rect rect, Paint paint, uint8_t /*radius*/, uint8_t /*corners*/) {
  if (rect.empty() || paint.kind == PaintKind::None || paint.color == Color::Transparent) return;

  if (paint.kind == PaintKind::Dither &&
      (paint.color == Color::LightGray || paint.color == Color::DarkGray)) {
    // 2x2 ordered dither: LightGray ~25% ink, DarkGray ~75% ink.
    const bool dark = paint.color == Color::DarkGray;
    for (int y = rect.y; y < rect.bottom(); ++y) {
      for (int x = rect.x; x < rect.right(); ++x) {
        const bool on = dark ? !((x ^ y) & 1) || ((x + y) & 1) : (((x & 1) == 0) && ((y & 1) == 0));
        if (on) gfx_.setPixel(x, y, true);
      }
    }
    return;
  }
  gfx_.fillRect(rect.x, rect.y, rect.width, rect.height, isInk(paint.color));
}

// radius/corners ignored — square outlines only (see fill()).
void GfxTextDrawTarget::stroke(Rect rect, Paint paint, uint8_t width, uint8_t /*radius*/,
                               uint8_t /*corners*/) {
  if (rect.empty() || paint.kind == PaintKind::None || width == 0) return;
  const bool ink = isInk(paint.color);
  for (uint8_t i = 0; i < width; ++i) {
    gfx_.drawRect(rect.x + i, rect.y + i, rect.width - 2 * i, rect.height - 2 * i, ink);
  }
}

void GfxTextDrawTarget::line(Point from, Point to, uint8_t width, Paint paint) {
  if (paint.kind == PaintKind::None) return;
  const bool ink = isInk(paint.color);
  // Bresenham, thickened perpendicular to the dominant axis.
  int x0 = from.x, y0 = from.y, x1 = to.x, y1 = to.y;
  const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  const bool steep = dy < -dx;  // |dy| > |dx|
  int err = dx + dy;
  const int half = width / 2;
  while (true) {
    for (int t = -half; t <= half - (width % 2 == 0 ? 1 : 0); ++t) {
      if (steep) {
        gfx_.setPixel(x0 + t, y0, ink);
      } else {
        gfx_.setPixel(x0, y0 + t, ink);
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void GfxTextDrawTarget::triangle(Point a, Point b, Point c, Paint paint) {
  if (paint.kind == PaintKind::None) return;
  const bool ink = isInk(paint.color);
  const int minY = min(a.y, min(b.y, c.y));
  const int maxY = max(a.y, max(b.y, c.y));
  auto edge = [](Point p, Point q, int y, float& x) -> bool {
    if (p.y == q.y) return false;
    if (y < min(p.y, q.y) || y >= max(p.y, q.y)) return false;
    x = p.x + (float)(q.x - p.x) * (y - p.y) / (q.y - p.y);
    return true;
  };
  for (int y = minY; y <= maxY; ++y) {
    float xs[3];
    int n = 0;
    float x;
    if (edge(a, b, y, x)) xs[n++] = x;
    if (edge(b, c, y, x)) xs[n++] = x;
    if (edge(c, a, y, x)) xs[n++] = x;
    if (n < 2) continue;
    int lo = (int)min(xs[0], xs[1]);
    int hi = (int)max(xs[0], xs[1]);
    for (int px = lo; px <= hi; ++px) gfx_.setPixel(px, y, ink);
  }
}

void GfxTextDrawTarget::text(Rect rect, const char* text, TextStyle style) {
  if (!text || rect.empty()) return;
  const FontDef* f = fontFor(style.font);
  const bool ink = isInk(style.color);
  // Delegate wrapping/alignment/ellipsis/vertical-centering to the SDK's
  // renderer-agnostic layout (built only on measureText/lineHeight). It emits
  // each finished line already positioned, so we just stamp the glyphs.
  freeink::ui::layoutText(*this, rect, text, style,
                          [&](const char* line, freeink::ui::Rect r) {
                            gfx_.drawText(f, line, r.x, r.y, ink);
                          });
}

void GfxTextDrawTarget::bitmap(Rect rect, freeink::ui::BitmapRef bmp, freeink::ui::BitmapMode mode,
                               Paint foreground, freeink::ui::Rotation rotation) {
  if (!bmp || rect.empty()) return;
  const bool ink = isInk(foreground.color);
  freeink::ui::forEachBitmapPixel(
      rect, bmp, mode, [&](int x, int y) { gfx_.setPixel(x, y, ink); }, rotation);
}

}  // namespace wakeink
