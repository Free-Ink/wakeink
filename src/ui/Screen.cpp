#include "Screen.h"

#include <BoardConfig.h>
#include <FreeInkUI.h>
#include <qrcode.h>

#include "../config/AppSettings.h"
#include "Icons.h"
#include "GfxTextDrawTarget.h"
#include "ScreenGeometry.h"

namespace ui = freeink::ui;
using wakeink::clearColor;
using wakeink::FONT_BODY;
using wakeink::FONT_BODY_B;
using wakeink::FONT_HUGE;
using wakeink::FONT_SMALL;
using wakeink::FONT_SMALL_B;
using wakeink::FONT_TINY;
using wakeink::FONT_TITLE;

namespace {
constexpr int W = wakeink::SCREEN_W;
constexpr int H = wakeink::SCREEN_H;
constexpr int MARGIN = wakeink::UI_MARGIN;
constexpr int HEADER_H = wakeink::UI_HEADER_H;
constexpr int BANNER_H = wakeink::UI_BANNER_H;

// Canonical panel context: per-device landscape framebuffer (ScreenGeometry.h).
// Touch mapping and rendering both key off this. The orientation only feeds
// touchToLogical, so it describes the touch panel's mounting — landscape CCW on
// Murphy, the only touch device; it is unused on touchless boards.
ui::DeviceContext makeDevice() {
  return ui::DeviceContext{W,
                           H,
                           ui::Orientation::LandscapeCounterClockwise,
                           BoardConfig::hasTouch(),
                           true,
                           // Safe area: the full-bleed header/banner owns the top
                           // edge on every screen, so top inset is 0; body content
                           // keeps a uniform MARGIN on the other three edges. Lay
                           // out against device.safeRect() and the edges stay
                           // consistent instead of being re-derived per draw call.
                           ui::Insets{0, MARGIN, MARGIN, MARGIN},
                           // minTouchSize stays 0: hit bands are sized explicitly
                           // per control (ButtonProps.hitPadding), not auto-grown.
                           0};
}

// A FreeInkUI render context bound to the device framebuffer. Each screen
// builds one over the Screen's persistent InteractionBuffer, draws through
// FreeInkUI components (which register their hit rects in the buffer), then
// pushes the framebuffer. A later routeTap() hit-tests that same buffer, so we
// never redraw just to handle a touch — important on slow e-ink.
struct Canvas {
  wakeink::GfxTextDrawTarget raw;
  ui::InvertedDrawTarget target;  // whole-UI dark mode; passthrough when off
  ui::DeviceContext device;
  ui::InputSnapshot input;  // empty during draw; routing uses a fresh snapshot
  ui::Frame<Screen::MAX_INTERACTIONS> frame;

  Canvas(uint8_t* fb, ui::InteractionBuffer<Screen::MAX_INTERACTIONS>& buf)
      : raw(fb, W, H),
        target(raw, settings().darkMode),
        device(makeDevice()),
        frame(target, device, input, buf) {}
};

ui::TextStyle style(ui::FontId font, ui::Color color = ui::Color::Black,
                    ui::TextAlign align = ui::TextAlign::Left, uint8_t maxLines = 1) {
  ui::TextStyle s;
  s.font = font;
  s.color = color;
  s.align = align;
  s.maxLines = maxLines;
  return s;
}

ui::StyleSet invertedBanner() {
  ui::StyleSet st;
  st.normal.background = ui::Paint::solid(ui::Color::Black);
  return st;
}

ui::StyleSet dialogPanel() {
  ui::StyleSet st;
  st.normal.background = ui::Paint::solid(ui::Color::White);
  st.normal.border = ui::Paint::solid(ui::Color::Black);
  st.normal.borderWidth = 2;
  return st;
}

int16_t lh(ui::DrawTarget& t, ui::FontId font) { return t.lineHeight(font); }

String countdownLabel(time_t now, const Event& ev) {
  if (now >= ev.start && now < ev.end) return "now";
  long mins = (long)(ev.start - now) / 60;
  if (mins < 1) return "now";
  // Far-out countdowns step coarsely (5-minute steps inside the hour, quarter
  // hours beyond it, floored so the time left is never overstated). The label
  // then changes only on step boundaries, so most minute ticks dirty nothing
  // but the status-bar clock — keeping the panel's partial-refresh window
  // (one bounding rect over every changed pixel) to a few glyphs instead of a
  // clock-to-countdown stripe. Inside 15 minutes it stays exact.
  if (mins < 60) {
    if (mins >= 15) mins -= mins % 5;
    return "in " + String(mins) + " min";
  }
  const long hours = mins / 60;
  mins %= 60;
  mins -= mins % 15;
  if (hours < 24) {
    return mins ? "in " + String(hours) + "h " + String(mins) + "m" : "in " + String(hours) + "h";
  }
  return "in " + String(hours / 24) + "d";
}

// Whole local days between now and t: 0 = today, 1 = tomorrow, ...
int daysAhead(time_t t, time_t now) {
  struct tm a, b;
  localtime_r(&now, &a);
  localtime_r(&t, &b);
  a.tm_hour = a.tm_min = a.tm_sec = 0;
  b.tm_hour = b.tm_min = b.tm_sec = 0;
  a.tm_isdst = -1;
  b.tm_isdst = -1;
  return (int)((mktime(&b) - mktime(&a)) / 86400);
}

// "" for today, "Tomorrow" for the next day, weekday name beyond that.
String dayLabel(time_t t, time_t now) {
  const int days = daysAhead(t, now);
  if (days <= 0) return String();
  if (days == 1) return String("Tomorrow");
  static const char* names[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday"};
  struct tm tmv;
  localtime_r(&t, &tmv);
  return String(names[tmv.tm_wday]);
}

// "3:30 PM" for today, "Tue 3:30 PM" for later days.
String listTimeLabel(time_t t, time_t now, bool use24) {
  struct tm evTm, nowTm;
  localtime_r(&t, &evTm);
  localtime_r(&now, &nowTm);
  String clock = Screen::formatClock(t, use24);
  if (evTm.tm_yday == nowTm.tm_yday && evTm.tm_year == nowTm.tm_year) return clock;
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return String(days[evTm.tm_wday]) + " " + clock;
}

// Draws a QR code through the DrawTarget (each module a filled square).
void drawQr(ui::DrawTarget& target, const char* text, int x, int y, int scale) {
  QRCode qr;
  uint8_t data[qrcode_getBufferSize(3)];
  if (qrcode_initText(&qr, data, 3, ECC_MEDIUM, text) != 0) return;
  for (int my = 0; my < qr.size; ++my) {
    for (int mx = 0; mx < qr.size; ++mx) {
      if (qrcode_getModule(&qr, mx, my)) {
        target.fill(ui::Rect{(int16_t)(x + mx * scale), (int16_t)(y + my * scale), (int16_t)scale,
                             (int16_t)scale},
                    ui::Paint::solid(ui::Color::Black), 0, ui::CornersAll);
      }
    }
  }
}

}  // namespace

String Screen::formatClock(time_t t, bool use24) {
  if (t < 1600000000) return "--:--";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[16];
  if (use24) {
    snprintf(buf, sizeof(buf), "%d:%02d", tmv.tm_hour, tmv.tm_min);
  } else {
    int h = tmv.tm_hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
  }
  return String(buf);
}

String Screen::formatDate(time_t t) {
  if (t < 1600000000) return "WakeInk";
  struct tm tmv;
  localtime_r(&t, &tmv);
  static const char* days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                               "Thursday", "Friday", "Saturday"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%s, %s %d", days[tmv.tm_wday], months[tmv.tm_mon], tmv.tm_mday);
  return String(buf);
}

void Screen::show(bool full) {
  // Every draw repaints from scratch, so flipping the buffer in place never
  // corrupts later drawing.
  wakeink::flipFrameForMount(display_.getFrameBuffer(), display_.getBufferSize());
  display_.displayBuffer(full ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
}

void Screen::drawIdle(const std::vector<Event>& events, const SyncStatus& sync, const String& ssid,
                      const String& ip, const String& hostname, bool wifiUp, bool paused,
                      time_t now) {
  (void)ssid;
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  ui::DrawTarget& t = c.target;
  const bool use24 = settings().use24HourTime;

  // Page skeleton on the FreeInkUI grid, laid out inside the safe area: a
  // reserved header band (the status bar itself draws full-bleed below),
  // flexible content, and a footer band. safeRect() carries the left/right/
  // bottom margins, so every band inherits them — no per-draw MARGIN math.
  const int16_t footerH = (int16_t)(lh(t, FONT_SMALL_B) + lh(t, FONT_TINY) + 2);
  ui::Stack<3> grid(c.device.safeRect(), ui::Axis::Column, 0);
  grid.fixed(HEADER_H);
  grid.flex(1);
  grid.fixed(footerH);
  grid.layout();
  const ui::Rect content = grid.rect(1);
  const ui::Rect footer = grid.rect(2);

  // --- header: FreeInkUI status bar (inverted), date left / clock right -------
  const String dateStr = formatDate(now);
  const String clockStr = formatClock(now, use24);
  String badge;
  if (!wifiUp) {
    badge = "no wifi";
  } else if (paused) {
    badge = "paused";
  } else if (sync.syncing) {
    badge = "syncing";
  } else if (!sync.lastSyncOk && sync.calendarsTotal > 0) {
    badge = "sync err";
  }
  ui::StatusBarProps sb;
  sb.fillBackground = true;
  sb.background = ui::Paint::solid(ui::Color::Black);
  sb.text = style(FONT_SMALL_B, ui::Color::White);
  sb.leading = dateStr.c_str();
  sb.trailing = clockStr.c_str();
  if (!badge.isEmpty()) sb.title = badge.c_str();
  ui::statusBar(c.frame, ui::Rect{0, 0, W, HEADER_H}, sb);  // full-bleed across the top

  // --- footer: wakeink.local + IP, with the settings cog at bottom-right ------
  const int16_t footerW = (int16_t)(footer.width - 40);  // keep clear of the cog
  String footerDetail;
  if (wifiUp) {
    ui::drawText(t, ui::Rect{footer.x, footer.y, footerW, lh(t, FONT_SMALL_B)},
                 ("http://" + hostname + ".local").c_str(), style(FONT_SMALL_B));
    footerDetail = ip;
    if (sync.lastSyncTime) footerDetail += "  ·  synced " + formatClock(sync.lastSyncTime, use24);
    ui::drawText(t,
                 ui::Rect{footer.x, (int16_t)(footer.y + lh(t, FONT_SMALL_B) - 1), footerW,
                          lh(t, FONT_TINY)},
                 footerDetail.c_str(), style(FONT_TINY));
  } else {
    ui::drawText(t, ui::Rect{footer.x, footer.y, footerW, lh(t, FONT_SMALL_B)},
                 "WiFi disconnected", style(FONT_SMALL_B));
  }

  // Settings cog (FreeInkUI button with an icon; white background so the
  // default button box doesn't draw a frame around it). Edge/corner reach is
  // SDK-handled: ensureMinTouchRect snaps near-edge hit rects to the bezel.
  // On touchless boards (M5 PaperColor) the cog is reached by GPIO focus:
  // up/down cycle events then the cog; confirm opens settings. The focused
  // style outlines it as the selection cue.
  {
    ui::ButtonProps cog;
    // SDK Lucide "settings" icon (freeink::Icon, Mask1: bit 0 = draw).
    cog.icon = ui::BitmapRef{icon_settings_24.bits, icon_settings_24.w, icon_settings_24.h,
                             ui::BitmapFormat::Mask1, false};
    cog.action = TAP_SETTINGS;
    cog.styles.normal.background = ui::Paint::solid(ui::Color::White);
    cog.styles.focused.background = ui::Paint::solid(ui::Color::White);
    cog.styles.focused.border = ui::Paint::solid(ui::Color::Black);
    cog.styles.focused.borderWidth = 2;
    ui::button(c.frame,
               ui::Rect{(int16_t)(footer.right() - 26), (int16_t)(footer.bottom() - 26), 26, 26}, cog);
  }

  if (events.empty()) {
    // Centered in the content slot, nudged up a third for visual balance.
    const int16_t emptyY = (int16_t)(content.y + content.height / 3);
    ui::drawText(t, ui::Rect{content.x, emptyY, content.width, lh(t, FONT_BODY)}, "No upcoming events",
                 style(FONT_BODY, ui::Color::Black, ui::TextAlign::Center));
    if (sync.calendarsTotal == 0) {
      const String hint = "Add calendars at http://" + (wifiUp ? ip : String("192.168.4.1"));
      ui::drawText(t,
                   ui::Rect{content.x, (int16_t)(emptyY + lh(t, FONT_BODY) + 10), content.width,
                            lh(t, FONT_SMALL)},
                   hint.c_str(), style(FONT_SMALL, ui::Color::Black, ui::TextAlign::Center));
    }
    return;
  }

  // --- next event card (the "widget") ----------------------------------------
  const Event& next = events.front();
  const bool nextIsToday = (now >= next.start && now < next.end) || daysAhead(next.start, now) <= 0;
  int16_t y = (int16_t)(content.y + MARGIN - 1);
  const int16_t contentX = content.x;
  const int16_t contentW = content.width;

  if (!nextIsToday) {
    ui::drawText(t, ui::Rect{contentX, y, contentW, lh(t, FONT_SMALL_B)}, "No more events today",
                 style(FONT_SMALL_B));
    y += lh(t, FONT_SMALL_B) + 4;
  }

  // Title: word-wrapped up to 2 lines. Reserve exactly the height it needs so
  // the time row sits tight beneath it.
  const int16_t cardTop = y;
  const ui::TextStyle titleStyle = style(FONT_TITLE, ui::Color::Black, ui::TextAlign::Left, 2);
  const int16_t titleH = ui::measureWrappedText(t, next.title.c_str(), titleStyle, contentW).height;
  ui::drawText(t, ui::Rect{contentX, y, contentW, titleH}, next.title.c_str(), titleStyle);
  y += titleH + 2;

  // Time row: time left, countdown right (same rect, opposite alignment).
  String when = formatClock(next.start, use24) + " - " + formatClock(next.end, use24);
  const String day = nextIsToday ? String() : dayLabel(next.start, now);
  if (!day.isEmpty()) when = day + "  ·  " + when;
  const String count = countdownLabel(now, next);
  const ui::Rect timeRow{contentX, y, contentW, lh(t, FONT_BODY)};
  ui::drawText(t, timeRow, when.c_str(), style(FONT_BODY));
  ui::drawText(t, timeRow, count.c_str(), style(FONT_BODY_B, ui::Color::Black, ui::TextAlign::Right));
  y += lh(t, FONT_BODY) + 5;

  // The whole next-event card is tappable (value 0 = events.front()). With GPIO
  // focus on it (buttons-only boards), outline the card as the selection cue.
  c.frame.hit(ui::Rect{0, cardTop, W, (int16_t)(y - cardTop)}, TAP_EVENT, 0);
  if (ui::hasState(c.frame.stateFor(TAP_EVENT, 0), ui::StateFocused)) {
    t.stroke(ui::Rect{2, (int16_t)(cardTop - 3), W - 4, (int16_t)(y - cardTop + 4)},
             ui::Paint::solid(ui::Color::Black), 2, 0, ui::CornersAll);
  }

  t.line(ui::Point{contentX, y}, ui::Point{(int16_t)(contentX + contentW), y}, 1,
         ui::Paint::solid(ui::Color::Black));
  y += 6;

  // --- upcoming list (each row tappable; value = event index) -----------------
  const int16_t rowH = lh(t, FONT_SMALL) + 4;
  const int16_t timeColW = wakeink::UI_TIME_COL_W;
  for (size_t i = 1; i < events.size(); ++i) {
    if (y + rowH > footer.y - 4) break;
    const Event& ev = events[i];
    // Register the hit before drawing so stateFor() can see this row's focus
    // (the same order FreeInkUI components use); a GPIO-focused row renders
    // inverted as the selection cue.
    c.frame.hit(ui::Rect{0, y, W, rowH}, TAP_EVENT, (int16_t)i);
    const bool focused = ui::hasState(c.frame.stateFor(TAP_EVENT, (int16_t)i), ui::StateFocused);
    if (focused) t.fill(ui::Rect{0, y, W, rowH}, ui::Paint::solid(ui::Color::Black), 0, ui::CornersAll);
    const ui::Color ink = focused ? ui::Color::White : ui::Color::Black;
    ui::drawText(t, ui::Rect{contentX, y, timeColW, rowH}, listTimeLabel(ev.start, now, use24).c_str(),
                 style(FONT_SMALL_B, ink));
    ui::drawText(t,
                 ui::Rect{(int16_t)(contentX + timeColW + 6), y,
                          (int16_t)(contentW - timeColW - 6), rowH},
                 ev.title.c_str(), style(FONT_SMALL, ink));
    y += rowH;
  }

  // Breadcrumb if the list ever out-grows the interaction buffer (rows past the
  // cap would silently become un-tappable). Raise MAX_INTERACTIONS if seen.
  if (interactions_.overflowed()) Serial.println("[ui] interaction buffer overflow");
}

void Screen::drawEventPopup(const Event& ev, time_t now) {
  (void)now;
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  const bool use24 = settings().use24HourTime;
  const String when = formatClock(ev.start, use24) + " - " + formatClock(ev.end, use24);

  // Cancel is listed first so a physical Back press (InputBack) resolves to it
  // rather than Skip — route() returns the first control carrying the mask.
  const ui::DialogOption options[] = {
      {"Cancel", TAP_CANCEL},
      {"Skip", TAP_SKIP},
  };
  ui::OptionDialogProps d;
  d.title = "Skip this event?";
  d.headline = ev.title.c_str();
  d.message = when.c_str();
  d.options = options;
  d.optionCount = 2;
  d.titleText = style(FONT_SMALL_B);
  d.headlineText = style(FONT_TITLE, ui::Color::Black, ui::TextAlign::Left, 2);
  d.messageText = style(FONT_BODY);
  d.buttonText = style(FONT_BODY_B, ui::Color::Black, ui::TextAlign::Center);
  d.styles = dialogPanel();
  // Buttons-only boards drive the dialog by GPIO focus (up/down move it,
  // confirm activates): the focused option gets a heavy outline so the
  // selection is visible without touch.
  d.buttonStyles.normal.border = ui::Paint::solid(ui::Color::Black);
  d.buttonStyles.normal.borderWidth = 1;
  d.buttonStyles.focused.border = ui::Paint::solid(ui::Color::Black);
  d.buttonStyles.focused.borderWidth = 3;
  d.buttonHeight = wakeink::UI_DIALOG_BTN_H;
  d.gap = MARGIN - 2;
  d.inputMask = ui::InputDefault | ui::InputBack;  // touch, focus+confirm, or Back

  // Size the panel to its content (caption + wrapped headline + time + buttons)
  // and center it, so the time can never collide with the button row.
  const int16_t w = W - 4 * MARGIN;
  int16_t h = ui::optionDialogHeight(c.target, d, w);
  if (h > H - 8) h = H - 8;
  ui::optionDialog(c.frame, ui::Rect{2 * MARGIN, (int16_t)((H - h) / 2), w, h}, d);
}

ui::DeviceContext Screen::deviceContext() const { return makeDevice(); }

Screen::Tap Screen::route(const ui::InputSnapshot& input) {
  const ui::ActionEvent ev = interactions_.route(input);
  return Tap{(int)ev.action, (int)ev.value};
}

void Screen::focusAction(int action) {
  const ui::Interaction* data = interactions_.data();
  for (int16_t i = 0; i < (int16_t)interactions_.count(); ++i) {
    if (data[i].action == action) {
      interactions_.setFocusedIndex(i);
      return;
    }
  }
  interactions_.setFocusedIndex(-1);
}

void Screen::clearFocus() { interactions_.setFocusedIndex(-1); }

bool Screen::hasFocus() const { return interactions_.focusedIndex() >= 0; }

void Screen::drawAlarm(const Event& ev, time_t now) {
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  ui::DrawTarget& t = c.target;
  const bool use24 = settings().use24HourTime;

  // Inverted header banner.
  const String head = "MEETING  ·  " + countdownLabel(now, ev);
  ui::HeaderProps hp;
  hp.styles = invertedBanner();
  hp.centered = true;
  hp.title = head.c_str();
  hp.titleText = style(FONT_BODY_B, ui::Color::White, ui::TextAlign::Center);
  ui::header(c.frame, ui::Rect{0, 0, W, BANNER_H}, hp);

  const ui::Rect safe = c.device.safeRect();
  int16_t y = (int16_t)(BANNER_H + MARGIN + 4);
  const ui::TextStyle titleStyle = style(FONT_HUGE, ui::Color::Black, ui::TextAlign::Left, 3);
  const int16_t titleH =
      ui::measureWrappedText(t, ev.title.c_str(), titleStyle, safe.width).height;
  ui::drawText(t, ui::Rect{safe.x, y, safe.width, titleH}, ev.title.c_str(), titleStyle);
  y += titleH + 6;

  const String body = "Starts at " + formatClock(ev.start, use24);
  ui::drawText(t, ui::Rect{safe.x, y, safe.width, lh(t, FONT_BODY)}, body.c_str(),
               style(FONT_BODY));

  ui::HeaderProps foot;
  foot.styles = invertedBanner();
  foot.centered = true;
  foot.title = BoardConfig::hasTouch() ? "Tap to dismiss" : "Press any button to dismiss";
  foot.titleText = style(FONT_SMALL_B, ui::Color::White, ui::TextAlign::Center);
  ui::header(c.frame,
             ui::Rect{0, (int16_t)(H - wakeink::UI_FOOTER_BAND_H), W, wakeink::UI_FOOTER_BAND_H},
             foot);
}

void Screen::drawSetupPortal(const String& apSsid, const String& ip, const String& failNote) {
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  ui::DrawTarget& t = c.target;

  ui::HeaderProps hp;
  hp.styles = invertedBanner();
  hp.title = "WakeInk Setup";
  hp.titleText = style(FONT_BODY_B, ui::Color::White);
  ui::header(c.frame, ui::Rect{0, 0, W, BANNER_H}, hp);

  const ui::Rect safe = c.device.safeRect();
  // QR (right) joins the open AP in one scan (WIFI: URI format).
  constexpr int QR_SCALE = wakeink::UI_QR_SCALE;
  constexpr int QR_SIZE = 29 * QR_SCALE;  // version 3
  const int16_t qrX = (int16_t)(safe.right() - QR_SIZE - 8);
  const int16_t qrY = (int16_t)(BANNER_H + MARGIN + 10);
  const String wifiQr = "WIFI:T:nopass;S:" + apSsid + ";;";
  drawQr(t, wifiQr.c_str(), qrX, qrY, QR_SCALE);
  ui::drawText(t, ui::Rect{qrX, (int16_t)(qrY + QR_SIZE + 4), QR_SIZE, lh(t, FONT_TINY)},
               "Scan to join", style(FONT_TINY, ui::Color::Black, ui::TextAlign::Center));

  const int16_t textW = (int16_t)(qrX - safe.x - 12);
  int16_t y = qrY;
  auto row = [&](ui::FontId f, const String& s, int extra = 0) {
    ui::drawText(t, ui::Rect{safe.x, y, textW, lh(t, f)}, s.c_str(), style(f));
    y += lh(t, f) + extra;
  };
  row(FONT_BODY, "1. Join the WiFi network");
  row(FONT_BODY_B, "    " + apSsid, 10);
  row(FONT_BODY, "2. Setup opens by itself,");
  row(FONT_BODY, "    or browse to");
  row(FONT_BODY_B, "    http://" + ip, 10);
  row(FONT_SMALL, "Add your home WiFi there, then paste");
  row(FONT_SMALL, "Google Calendar ICS links.", 6);
  if (!failNote.isEmpty()) row(FONT_SMALL_B, failNote);
}

void Screen::drawHibernate(time_t now) {
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  ui::DrawTarget& t = c.target;

  ui::HeaderProps hp;
  hp.styles = invertedBanner();
  hp.centered = true;
  hp.title = "WakeInk is hibernating...";
  hp.titleText = style(FONT_BODY_B, ui::Color::White, ui::TextAlign::Center);
  ui::header(c.frame, ui::Rect{0, 0, W, BANNER_H}, hp);

  // Big clock (with the date beneath) centered in the space below the banner.
  const String clock = formatClock(now, settings().use24HourTime);
  const String date = formatDate(now);
  const int16_t clockH = lh(t, FONT_HUGE);
  const int16_t dateH = lh(t, FONT_SMALL);
  int16_t y = (int16_t)(BANNER_H + (H - BANNER_H - clockH - dateH - 6) / 2);
  ui::drawText(t, ui::Rect{0, y, W, clockH}, clock.c_str(),
               style(FONT_HUGE, ui::Color::Black, ui::TextAlign::Center));
  y += clockH + 6;
  ui::drawText(t, ui::Rect{0, y, W, dateH}, date.c_str(),
               style(FONT_SMALL, ui::Color::Black, ui::TextAlign::Center));
}

void Screen::drawMessage(const char* title, const char* line1, const char* line2) {
  display_.clearScreen(clearColor());
  Canvas c(display_.getFrameBuffer(), interactions_);
  ui::DrawTarget& t = c.target;

  ui::HeaderProps hp;
  hp.styles = invertedBanner();
  hp.title = title;
  hp.titleText = style(FONT_BODY_B, ui::Color::White);
  ui::header(c.frame, ui::Rect{0, 0, W, BANNER_H}, hp);

  const ui::Rect safe = c.device.safeRect();
  int16_t y = (int16_t)(BANNER_H * 2);
  if (line1) {
    ui::drawText(t, ui::Rect{safe.x, y, safe.width, lh(t, FONT_BODY)}, line1, style(FONT_BODY));
    y += lh(t, FONT_BODY) + 6;
  }
  if (line2) {
    ui::drawText(t, ui::Rect{safe.x, y, safe.width, lh(t, FONT_SMALL)}, line2, style(FONT_SMALL));
  }
}
