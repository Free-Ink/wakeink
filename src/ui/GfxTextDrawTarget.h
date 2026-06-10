#pragma once

// Bridges FreeInkUI's DrawTarget interface onto our 1-bit framebuffer via the
// GfxText pixel/glyph engine. This is the seam the SDK is designed around:
// FreeInkUI does layout and ships components (statusBar, header, drawText,
// Stack) but no renderer — it calls back into this DrawTarget for every pixel.
// So all on-device drawing flows: Screen -> FreeInkUI components -> this -> GfxText.

#include <FreeInkUI.h>
#include <GfxText.h>

namespace wakeink {

// FontIds handed to FreeInkUI TextStyles; resolved back to glyph tables here.
enum Font : freeink::ui::FontId {
  FONT_TINY = 0,
  FONT_SMALL,
  FONT_SMALL_B,
  FONT_BODY,
  FONT_BODY_B,
  FONT_TITLE,
  FONT_HUGE,
};

class GfxTextDrawTarget final : public freeink::ui::DrawTarget {
 public:
  GfxTextDrawTarget(uint8_t* framebuffer, uint16_t width, uint16_t height)
      : gfx_(framebuffer, width, height), w_(width), h_(height) {}

  freeink::ui::Size measureText(freeink::ui::FontId font, const char* text,
                                freeink::ui::TextStyle style) const override;
  int16_t lineHeight(freeink::ui::FontId font) const override;
  void fill(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t radius,
            uint8_t corners) override;
  void stroke(freeink::ui::Rect rect, freeink::ui::Paint paint, uint8_t width, uint8_t radius,
              uint8_t corners) override;
  void line(freeink::ui::Point from, freeink::ui::Point to, uint8_t width,
            freeink::ui::Paint paint) override;
  void triangle(freeink::ui::Point a, freeink::ui::Point b, freeink::ui::Point c,
                freeink::ui::Paint paint) override;
  void text(freeink::ui::Rect rect, const char* text, freeink::ui::TextStyle style) override;
  void bitmap(freeink::ui::Rect rect, freeink::ui::BitmapRef bitmap, freeink::ui::BitmapMode mode,
              freeink::ui::Paint foreground, freeink::ui::Rotation rotation) override;

 private:
  static const FontDef* fontFor(freeink::ui::FontId font);
  // 1-bit panel: ink (black pixel) vs paper. DarkGray reads as ink, LightGray
  // as paper, with Dither paints rendered as ordered patterns in fill().
  static bool isInk(freeink::ui::Color c);

  mutable GfxText gfx_;
  uint16_t w_, h_;
};

}  // namespace wakeink
