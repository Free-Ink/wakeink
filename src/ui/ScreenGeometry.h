#pragma once

// Logical framebuffer geometry, fixed per build: each PlatformIO env targets
// exactly one FREEINK_DEVICE_*, and these must match the logical
// (post-rotation) geometry that device's panel driver reports — setup()
// verifies the match after display.begin(). Murphy M3 is a landscape 416x240
// framebuffer; the M5 PaperColor's physical 400x600 portrait panel is driven
// through a 600x400 landscape framebuffer (the ED2208 driver rotates it).

#include <BoardConfig.h>

namespace wakeink {

// Alongside the framebuffer size, each device binds its own UI scale: glyph
// tables (GfxTextDrawTarget::fontFor) and the chrome metrics below. This is
// the FreeInkUI scaling model — layout adapts through per-device sizes and
// font bindings, never through fractional glyph scaling. Heights that track a
// text line are derived from lineHeight() at draw time and aren't listed here.
#if FREEINK_DEVICE_MURPHY
constexpr int SCREEN_W = 416;
constexpr int SCREEN_H = 240;
constexpr int UI_MARGIN = 8;
constexpr int UI_HEADER_H = 22;       // idle status bar band
constexpr int UI_BANNER_H = 30;       // full-width header band (alarm/portal/message)
constexpr int UI_FOOTER_BAND_H = 26;  // alarm footer band
constexpr int UI_QR_SCALE = 4;        // setup-portal QR module size
constexpr int UI_DIALOG_BTN_H = 34;
constexpr int UI_TIME_COL_W = 78;     // upcoming-list time column
#elif FREEINK_DEVICE_M5
// 4.0" 600x400 (~180 PPI): ~1.4x the Murphy's linear scale, like the fonts.
constexpr int SCREEN_W = 600;
constexpr int SCREEN_H = 400;
constexpr int UI_MARGIN = 12;
constexpr int UI_HEADER_H = 30;
constexpr int UI_BANNER_H = 42;
constexpr int UI_FOOTER_BAND_H = 36;
constexpr int UI_QR_SCALE = 6;
constexpr int UI_DIALOG_BTN_H = 48;
constexpr int UI_TIME_COL_W = 112;
#else
#error "WakeInk: no screen geometry for the selected FREEINK_DEVICE_* (add it to ScreenGeometry.h)"
#endif

}  // namespace wakeink
