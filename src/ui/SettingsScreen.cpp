#include "SettingsScreen.h"

#include <BoardConfig.h>

#include "../calendar/CalendarManager.h"
#include "../config/AppSettings.h"
#include "../config/StateStore.h"
#include "../config/Timezones.h"
#include "../net/WifiService.h"
#include "GfxTextDrawTarget.h"
#include "Screen.h"
#include "ScreenGeometry.h"

namespace ui = freeink::ui;
using wakeink::FONT_BODY;
using wakeink::FONT_BODY_B;
using wakeink::FONT_SMALL;
using wakeink::FONT_SMALL_B;
using wakeink::FONT_TINY;

namespace {
constexpr int W = wakeink::SCREEN_W;
constexpr int H = wakeink::SCREEN_H;
// Safe-area edge inset, shared with the idle screen (Screen.cpp) so both
// surfaces keep the same margin off the panel edges.
constexpr int MARGIN = wakeink::UI_MARGIN;

// FreeInkUI action ids used by this screen (0 = NO_ACTION).
enum : ui::ActionId {
  A_CLOSE = 1,
  A_TAB,     // value = tab index
  A_PAGE,    // value = new page
  A_TOGGLE,  // value = item id
  A_DEC,     // value = item id
  A_INC,     // value = item id
  A_DO,      // value = item id (action rows / pickers)
  A_TZ_ROW,  // value = timezone index
  A_TZ_PG,   // value = +visible / -visible rows
  A_DLG,     // value = dialog option
  A_DAY,     // value = ISO weekday 1..7
};

// Settings items (doubles as the ActionEvent value for row controls).
enum Item : int16_t {
  IT_LEAD,
  IT_EARLY_EN,
  IT_EARLY_LEAD,
  IT_EARLY_BEFORE,
  IT_AUTOSTOP,
  IT_TEST,
  IT_SOUND_EN,
  IT_VOLUME,
  IT_FLASH_EN,
  IT_FRONTLIGHT,
  IT_MAYBE,
  IT_UNACCEPTED,
  IT_LINKS_ONLY,
  IT_ANY_URL,
  IT_WORK_EN,
  IT_WORK_DAYS,
  IT_WORK_START,
  IT_WORK_END,
  IT_TZ,
  IT_24H,
  IT_LOOKAHEAD,
  IT_POLL,
  IT_PAUSE,
  IT_SYNC,
  IT_REBOOT,
  IT_INFO_NET,
  IT_INFO_SYNC,
  IT_INFO_VER,
};

enum class Kind { TOGGLE, STEPPER, ACTION, PICKER, DAYS, INFO };

struct Row {
  int16_t item;
  const char* label;
  Kind kind;
};

const char* TAB_NAMES[] = {"Alarm", "Sound", "Filter", "Clock", "System"};
constexpr int TAB_COUNT = 5;

const Row TAB_ALARM[] = {
    {IT_LEAD, "Lead time", Kind::STEPPER},
    {IT_EARLY_EN, "Early-meeting alert", Kind::TOGGLE},
    {IT_EARLY_LEAD, "Early lead time", Kind::STEPPER},
    {IT_EARLY_BEFORE, "Early = starts before", Kind::STEPPER},
    {IT_AUTOSTOP, "Auto-stop ringing", Kind::STEPPER},
    {IT_TEST, "Test alarm", Kind::ACTION},
};
const Row TAB_SOUND[] = {
    {IT_SOUND_EN, "Alarm sound", Kind::TOGGLE},
    {IT_VOLUME, "Volume", Kind::STEPPER},
    {IT_FLASH_EN, "Flash light on ring", Kind::TOGGLE},
    {IT_FRONTLIGHT, "Frontlight", Kind::STEPPER},
};
const Row TAB_FILTER[] = {
    {IT_MAYBE, "Show \"maybe\" events", Kind::TOGGLE},
    {IT_UNACCEPTED, "Unaccepted invites", Kind::TOGGLE},
    {IT_LINKS_ONLY, "Meeting links only", Kind::TOGGLE},
    {IT_ANY_URL, "Any URL counts", Kind::TOGGLE},
    {IT_WORK_EN, "Work hours only", Kind::TOGGLE},
    {IT_WORK_DAYS, "Work days", Kind::DAYS},
    {IT_WORK_START, "Work start", Kind::STEPPER},
    {IT_WORK_END, "Work end", Kind::STEPPER},
};
const Row TAB_CLOCK[] = {
    {IT_TZ, "Timezone", Kind::PICKER},
    {IT_24H, "24-hour clock", Kind::TOGGLE},
    {IT_LOOKAHEAD, "Look ahead", Kind::STEPPER},
    {IT_POLL, "Sync every", Kind::STEPPER},
};
const Row TAB_SYSTEM[] = {
    {IT_PAUSE, "Alarms", Kind::PICKER},
    {IT_SYNC, "Sync now", Kind::ACTION},
    {IT_REBOOT, "Reboot", Kind::ACTION},
    {IT_INFO_NET, "Network", Kind::INFO},
    {IT_INFO_SYNC, "Last sync", Kind::INFO},
    {IT_INFO_VER, "Firmware", Kind::INFO},
};

struct TabDef {
  const Row* rows;
  int count;
};
const TabDef TABS[TAB_COUNT] = {
    {TAB_ALARM, 6}, {TAB_SOUND, 4}, {TAB_FILTER, 8}, {TAB_CLOCK, 4}, {TAB_SYSTEM, 6},
};

// Chrome scale: this screen was laid out in Murphy pixels. The M5 binds ~1.4x
// fonts (same physical size at its higher PPI — see ScreenGeometry.h), so its
// chrome scales by the same factor or labels overflow their boxes. Hit
// paddings stay unscaled: touch bands only matter on Murphy (scale 1), and on
// the M5 the rects are only focus-rendering geometry.
#if FREEINK_DEVICE_M5 || FREEINK_DEVICE_PAPERMONO
constexpr int16_t S(int v) { return (int16_t)((v * 7 + 2) / 5); }  // x1.4
#else
constexpr int16_t S(int v) { return (int16_t)v; }
#endif

constexpr int CONTENT_Y = S(66);
constexpr int ROW_H = S(28);  // control/touch height of one row
// Vertical breathing room between rows: row components fill ROW_H, so without
// a gap stacked steppers/toggles butt right up against each other. Rows now
// advance by ROW_PITCH (control below, empty gap below that).
constexpr int ROW_GAP = S(7);
constexpr int ROW_PITCH = ROW_H + ROW_GAP;
// Rows that fully fit between CONTENT_Y and a small bottom margin; longer tabs
// paginate. Computed per device: the gap costs tight 240px-tall Murphy a row
// (6->5), but the 400px-tall M5 still fits 6.
constexpr int MAX_ROWS = 1 + (H - S(4) - CONTENT_Y - ROW_H) / ROW_PITCH;

const int POLL_PRESETS[] = {1, 2, 3, 5, 10, 15, 30, 60};
constexpr int POLL_PRESET_COUNT = 8;

ui::TextStyle txt(ui::FontId f, ui::Color c = ui::Color::Black,
                  ui::TextAlign a = ui::TextAlign::Left) {
  ui::TextStyle s;
  s.font = f;
  s.color = c;
  s.align = a;
  return s;
}

// Button style sets reused across the screen.
ui::StyleSet ghostButton() {
  ui::StyleSet st;
  st.normal.background = ui::Paint::solid(ui::Color::White);
  st.normal.border = ui::Paint::solid(ui::Color::Black);
  st.normal.borderWidth = 1;
  st.selected.background = ui::Paint::solid(ui::Color::Black);
  st.selected.foreground = ui::Paint::solid(ui::Color::White);
  // GPIO focus cue (buttons-only boards): heavy outline, label untouched —
  // a dither background reads as speckle on the 1-bit panel.
  st.focused.background = ui::Paint::solid(ui::Color::White);
  st.focused.border = ui::Paint::solid(ui::Color::Black);
  st.focused.borderWidth = 3;
  // Disabled: keep the box visible, mute the label (dither renders as
  // speckled "grey" text on the 1-bit panel).
  st.disabled.background = ui::Paint::solid(ui::Color::White);
  st.disabled.border = ui::Paint::solid(ui::Color::Black);
  st.disabled.borderWidth = 1;
  st.disabled.foreground = ui::Paint::dither(ui::Color::LightGray);
  return st;
}

ui::StyleSet onHeaderButton() {  // white-outline button on the black header
  ui::StyleSet st;
  st.normal.background = ui::Paint::solid(ui::Color::Black);
  st.normal.border = ui::Paint::solid(ui::Color::White);
  st.normal.borderWidth = 1;
  st.normal.foreground = ui::Paint::solid(ui::Color::White);
  st.focused = st.normal;
  st.focused.borderWidth = 3;  // GPIO focus cue
  return st;
}

// Settings rows (toggleRow/stepperRow/settingRow) carry no box of their own —
// just label + control. GPIO focus shows as a heavy outline; a dither
// background reads as speckle on the 1-bit panel (same lesson as ghostButton).
ui::StyleSet rowStyle() {
  ui::StyleSet st;
  st.normal.background = ui::Paint::none();
  st.selected = st.normal;
  st.disabled = st.normal;
  st.focused.background = ui::Paint::none();
  st.focused.border = ui::Paint::solid(ui::Color::Black);
  st.focused.borderWidth = 3;
  return st;
}

// Base props shared by every premade settings row: full-width rect, exact
// hit band (no minTouch expansion — rows are contiguous, and the M5 has no
// touch), focus-only outline.
ui::SettingRowProps rowBase(const char* label) {
  ui::SettingRowProps r;
  r.label = label;
  r.labelText = txt(FONT_SMALL);
  r.valueText = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Right);
  r.styles = rowStyle();
  // The row rect already carries the MARGIN edge inset; this is just internal
  // clearance so the focus outline doesn't crowd the label/control.
  r.sidePadding = S(4);
  r.minTouchSize = 0;
  return r;
}

String minutesLabel(int minutes) { return String(minutes) + " min"; }

String timeOfDayLabel(int hour, int minute, bool use24) {
  char buf[16];
  if (use24) {
    snprintf(buf, sizeof(buf), "%d:%02d", hour, minute);
  } else {
    int h = hour % 12;
    if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h, minute, hour < 12 ? "AM" : "PM");
  }
  return String(buf);
}

int currentTzIndex() {
  for (size_t i = 0; i < TIMEZONE_COUNT; ++i) {
    if (settings().timezoneName == TIMEZONES[i].name) return (int)i;
  }
  return -1;
}

String valueFor(int16_t item) {
  AppSettings& s = settings();
  switch (item) {
    case IT_LEAD: return minutesLabel(s.alarmLeadTimeMinutes);
    case IT_EARLY_LEAD: return minutesLabel(s.earlyMeetingLeadTimeMinutes);
    case IT_EARLY_BEFORE:
      return timeOfDayLabel(s.earlyMeetingBeforeHour, s.earlyMeetingBeforeMinute, s.use24HourTime);
    case IT_AUTOSTOP: return minutesLabel(s.alarmMaxMinutes);
    case IT_VOLUME: return String(s.alarmVolume) + "%";
    case IT_FRONTLIGHT:
      return s.frontlightBrightness ? String(s.frontlightBrightness) + "%" : String("off");
    case IT_WORK_START: return timeOfDayLabel(s.workHoursStartHour, s.workHoursStartMinute, s.use24HourTime);
    case IT_WORK_END: return timeOfDayLabel(s.workHoursEndHour, s.workHoursEndMinute, s.use24HourTime);
    case IT_TZ: return s.timezoneName;
    case IT_LOOKAHEAD: return String(s.lookaheadDays) + (s.lookaheadDays == 1 ? " day" : " days");
    case IT_POLL: return minutesLabel(s.pollIntervalMinutes);
    case IT_PAUSE: {
      const time_t now = time(nullptr);
      if (!stateStore().isPaused(now)) return "Active";
      if (stateStore().pauseChoiceMinutes < 0) return "Paused";
      return "Until " + Screen::formatClock(stateStore().pauseUntil, settings().use24HourTime);
    }
    case IT_INFO_NET:
      return wifiService().mode() == WifiService::CONNECTED
                 ? settings().hostname + ".local · " + wifiService().ip()
                 : String("disconnected");
    case IT_INFO_SYNC: {
      const SyncStatus st = calendarManager().status();
      if (!st.lastSyncTime) return "never";
      String v = Screen::formatClock(st.lastSyncTime, settings().use24HourTime);
      v += st.lastSyncOk ? "" : " (errors)";
      return v;
    }
    case IT_INFO_VER: return "v" WAKEINK_VERSION;
    default: return String();
  }
}

bool toggleValue(int16_t item) {
  AppSettings& s = settings();
  switch (item) {
    case IT_EARLY_EN: return s.earlyMeetingAlertEnabled;
    case IT_SOUND_EN: return s.alarmSoundEnabled;
    case IT_FLASH_EN: return s.alarmFlashFrontlight;
    case IT_MAYBE: return s.showMaybeEvents;
    case IT_UNACCEPTED: return s.alarmOnUnacceptedEvents;
    case IT_LINKS_ONLY: return s.onlyAlertWithLinks;
    case IT_ANY_URL: return s.alertWithAnyLink;
    case IT_WORK_EN: return s.workHoursEnabled;
    case IT_24H: return s.use24HourTime;
    default: return false;
  }
}

bool isFilterItem(int16_t item) {
  switch (item) {
    case IT_MAYBE:
    case IT_UNACCEPTED:
    case IT_LINKS_ONLY:
    case IT_ANY_URL:
    case IT_WORK_EN:
    case IT_WORK_DAYS:
    case IT_WORK_START:
    case IT_WORK_END:
      return true;
    default:
      return false;
  }
}

void stepTimeOfDay(int& hour, int& minute, int dir) {
  int total = hour * 60 + minute + dir * 30;
  if (total < 0) total += 24 * 60;
  if (total >= 24 * 60) total -= 24 * 60;
  hour = total / 60;
  minute = total % 60;
}

// Timezone-picker list geometry, shared by drawTzPicker and the swipe handler
// so button paging and gesture paging agree on the window size.
constexpr int16_t TZ_ROW_H = S(27);
ui::Rect tzListRect() {
  return ui::Rect{MARGIN, S(34), (int16_t)(W - 2 * MARGIN), (int16_t)(H - S(34) - S(36))};
}
uint16_t tzVisibleRows() { return ui::listVisibleRows(tzListRect(), TZ_ROW_H, 0); }

}  // namespace

// Shared render context (same pattern as Screen's Canvas).
class SettingsCanvas {
 public:
  wakeink::GfxTextDrawTarget raw;
  ui::InvertedDrawTarget target;  // whole-UI dark mode; passthrough when off
  ui::DeviceContext device;
  ui::InputSnapshot input;
  ui::Frame<48> frame;

  SettingsCanvas(uint8_t* fb, ui::InteractionBuffer<48>& buf)
      : raw(fb, W, H),
        target(raw, settings().darkMode),
        device{.width = W,
               .height = H,
               .orientation = ui::Orientation::LandscapeCounterClockwise,
               // Same per-device transform as Screen.cpp's makeDevice():
               // identity on the Paper Mono (its TouchConfig already maps the
               // FT6336 into the displayed frame), landscape CCW on Murphy.
#if FREEINK_DEVICE_PAPERMONO
               .touchOrientation = ui::Orientation::Portrait,
#else
               .touchOrientation = ui::Orientation::LandscapeCounterClockwise,
#endif
               .hasTouch = BoardConfig::hasTouch(),
               .hasButtons = true,
               // Full-bleed header owns the top edge; uniform MARGIN elsewhere.
               .safeArea = ui::Insets{0, MARGIN, MARGIN, MARGIN},
               .minTouchSize = 0},
        frame(target, device, input, buf) {}
};

void SettingsScreen::open() {
  tab_ = 0;
  page_ = 0;
  scroll_ = 0;
  modal_ = Modal::NONE;
#if FREEINK_DEVICE_M5
  // The menu is transient UI: FULL on the M5 now runs the ~15 s complete
  // waveform (standing-image policy), so open with the interrupted HALF —
  // idle's complete render returns on close.
  draw(EInkDisplay::HALF_REFRESH);
#else
  draw(EInkDisplay::FULL_REFRESH);
#endif
}

void SettingsScreen::redraw() { draw(EInkDisplay::FAST_REFRESH); }

void SettingsScreen::draw(EInkDisplay::RefreshMode mode) {
  display_.clearScreen(wakeink::clearColor());
  SettingsCanvas c(display_.getFrameBuffer(), interactions_);

  if (modal_ == Modal::TZ_PICKER) {
    drawTzPicker(c);
  } else {
    drawNormal(c);
    if (modal_ == Modal::PAUSE) overlayPause(c);
    if (modal_ == Modal::REBOOT) overlayReboot(c);
  }

  // Guard: rows past the buffer cap would silently become un-tappable.
  if (interactions_.overflowed()) Serial.println("[ui] settings interaction buffer overflow");

  wakeink::flipFrameForMount(display_.getFrameBuffer(), display_.getBufferSize());
  display_.displayBuffer(mode);
}

void SettingsScreen::drawNormal(SettingsCanvas& c) {
  ui::DrawTarget& t = c.target;

  // Tap bands: the visual buttons are 22px tall in a 28px row with small gaps,
  // leaving dead pixels above/below/between them that made taps feel random.
  // ButtonProps.hitPadding extends each control's hit rect per edge to a
  // contiguous, non-overlapping full-row band (the old hand-rolled hitBand
  // geometry, now declarative — one interaction per control). Bezel reach is
  // SDK-owned: hit rects within 12px of a screen edge snap to it.

  // Header: premade component owns the whole band, Close as its trailing
  // action button. A physical Back press still leaves settings through
  // handleInput's fallback (Back with no routed hit closes).
  ui::HeaderProps hp;
  hp.title = "Settings";
  hp.titleText = txt(FONT_BODY_B, ui::Color::White);
  ui::StyleSet headerStyle;
  headerStyle.normal.background = ui::Paint::solid(ui::Color::Black);
  hp.styles = headerStyle;
  hp.trailingLabel = "Close";
  hp.trailingAction = A_CLOSE;
  hp.trailingText = txt(FONT_SMALL_B, ui::Color::White, ui::TextAlign::Center);
  hp.trailingStyles = onHeaderButton();
  ui::header(c.frame, ui::Rect{0, 0, W, S(30)}, hp);

  // Tab bar: content-width tabs from the leading edge (the component falls
  // back to equal slots by itself if the labels overflow the band — which is
  // what happens on the narrow Murphy, wide papermono gets natural widths).
  // Horizontal swipes also switch tabs (handleSwipe).
  ui::TabItem tabs[TAB_COUNT];
  for (int i = 0; i < TAB_COUNT; ++i) {
    tabs[i] = ui::tabItem(i, i == tab_, true, TAB_NAMES[i]);
  }
  ui::TabBarProps tb;
  tb.tabs = tabs;
  tb.count = TAB_COUNT;
  tb.action = A_TAB;
  tb.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  tb.tabStyles = ghostButton();  // selected = black fill, focused = heavy outline
  tb.layout = ui::TabBarLayout::ContentWidth;
  tb.gap = S(4);
  tb.contentInset = ui::Insets{S(2), S(10), S(2), S(10)};
  tb.tabInset = ui::Insets{S(2), S(2), S(2), S(2)};  // equal-width fallback inset
  tb.minTouchSize = 0;  // rows below sit close; hit bands stay the drawn pills
  tb.divider = true;
  ui::tabBar(c.frame, ui::Rect{MARGIN, S(34), (int16_t)(W - 2 * MARGIN), S(28)}, tb);

  // Content rows. Touch boards scroll the tab (vertical swipes page it, a
  // scroll indicator marks the window); buttons-only boards keep the explicit
  // pager row, which GPIO focus can reach.
  const TabDef& tab = TABS[tab_];
  const bool touchScrolls = BoardConfig::hasTouch();
  bool paged = false;
  int pageCount = 1;
  int first, last;
  if (touchScrolls) {
    const int maxTop = tab.count > MAX_ROWS ? tab.count - MAX_ROWS : 0;
    if (scroll_ > maxTop) scroll_ = (int16_t)maxTop;
    if (scroll_ < 0) scroll_ = 0;
    first = scroll_;
    last = min(tab.count, first + MAX_ROWS);
  } else {
    paged = tab.count > MAX_ROWS;
    const int rowsPerPage = paged ? MAX_ROWS - 1 : MAX_ROWS;  // reserve a pager row
    pageCount = paged ? (tab.count + rowsPerPage - 1) / rowsPerPage : 1;
    if (page_ >= pageCount) page_ = 0;
    first = page_ * rowsPerPage;
    last = min(tab.count, first + rowsPerPage);
  }

  int16_t y = CONTENT_Y;
  for (int i = first; i < last; ++i, y += ROW_PITCH) {
    const Row& row = tab.rows[i];
    // Inset to the safe margin: content AND the GPIO focus outline respect the
    // edge. The hit rect still reaches the bezel — ensureMinTouchRect snaps any
    // edge within 12px of the screen, and MARGIN is inside that.
    const ui::Rect rowRect{MARGIN, y, (int16_t)(W - 2 * MARGIN), ROW_H};

    switch (row.kind) {
      case Kind::TOGGLE: {
        ui::ToggleRowProps tog;
        tog.row = rowBase(row.label);
        tog.checked = toggleValue(row.item);
        tog.toggleAction = A_TOGGLE;
        tog.toggleValue = row.item;
        tog.toggleWidth = S(40);
        tog.toggleHeight = S(20);
        ui::toggleRow(c.frame, rowRect, tog);
        break;
      }
      case Kind::STEPPER: {
        const String v = valueFor(row.item);
        ui::StepperRowProps st;
        st.row = rowBase(row.label);
        st.value = v.c_str();
        st.decrement = A_DEC;
        st.decrementValue = row.item;  // value carries the item, not -1
        st.increment = A_INC;
        st.incrementValue = row.item;
        // Wide ± targets (~11mm): small steppers plus finger scatter made
        // repeated taps unreliable. buttonStyles inherit the ghost look.
        st.buttonStyles = ghostButton();
        st.buttonWidth = S(48);
        st.valueWidth = S(72);
        st.controlSize = S(14);
        st.gap = S(4);
        ui::stepperRow(c.frame, rowRect, st);
        break;
      }
      case Kind::ACTION: {
        // Auto-width button, centered: an action ("Test alarm", "Sync now")
        // reads as a control, not a full-width banner. Sized to its label plus
        // padding, clamped to the safe width.
        ui::TextStyle bt = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
        int16_t btnW = (int16_t)(t.measureText(FONT_SMALL_B, row.label, bt).width + S(44));
        const int16_t maxW = (int16_t)(W - 2 * MARGIN);
        if (btnW > maxW) btnW = maxW;
        const int16_t btnX = (int16_t)((W - btnW) / 2);
        ui::ButtonProps b;
        b.label = row.label;
        b.action = A_DO;
        b.value = row.item;
        b.text = bt;
        b.styles = ghostButton();
        b.minTouchSize = 0;
        b.hitPadding = ui::Insets{2, S(6), 4, S(6)};  // a little tap slack around the button
        ui::button(c.frame, ui::Rect{btnX, (int16_t)(y + S(2)), btnW, (int16_t)(ROW_H - S(6))}, b);
        break;
      }
      case Kind::PICKER: {
        const String v = valueFor(row.item);
        ui::SettingRowProps pick = rowBase(row.label);
        pick.value = v.c_str();
        pick.drawChevron = true;
        pick.action = A_DO;
        pick.valueId = row.item;
        ui::settingRow(c.frame, rowRect, pick);
        break;
      }
      case Kind::DAYS: {
        static const char* dayLetters[] = {"M", "T", "W", "T", "F", "S", "S"};
        // Label via settingRow (no action); chips remain hand-placed buttons —
        // there is no premade multi-select control.
        ui::settingRow(c.frame, rowRect, rowBase(row.label));
        const int16_t chipW = S(28);
        for (int d = 1; d <= 7; ++d) {
          bool on = false;
          for (int wd : settings().workDays) {
            if (wd == d) on = true;
          }
          ui::ButtonProps chip;
          chip.label = dayLetters[d - 1];
          chip.action = A_DAY;
          chip.value = d;
          chip.state = on ? ui::StateChecked : ui::StateNormal;
          chip.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
          chip.styles = ghostButton();
          chip.minTouchSize = 0;
          chip.hitPadding = ui::Insets{2, 1, 4, 1};  // full row height, chips stay disjoint
          const int16_t chipX = (int16_t)(W - MARGIN - (8 - d) * (chipW + S(2)));
          ui::button(c.frame, ui::Rect{chipX, (int16_t)(y + S(2)), chipW, (int16_t)(ROW_H - S(6))}, chip);
        }
        break;
      }
      case Kind::INFO: {
        const String v = valueFor(row.item);
        ui::SettingRowProps info = rowBase(row.label);
        info.value = v.c_str();
        info.valueText = txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Right);
        ui::settingRow(c.frame, rowRect, info);
        break;
      }
    }
  }

  if (touchScrolls && tab.count > MAX_ROWS) {
    // Scroll indicator on the content band's right edge (the SDK list helper,
    // reused standalone since these rows are premade row components, not a
    // ui::list — steppers/day chips have no list-item form).
    ui::drawListScrollIndicator(t,
                                ui::Rect{MARGIN, CONTENT_Y, (int16_t)(W - 2 * MARGIN),
                                         (int16_t)(MAX_ROWS * ROW_PITCH - ROW_GAP)},
                                (uint32_t)tab.count, (uint32_t)MAX_ROWS, (uint32_t)scroll_);
  }

  if (paged) {
    // Explicit pager: Prev / "Page x of y" / Next (a lone cycling "More"
    // button read as a status line, not a control). Anchored to the last row
    // slot so it doesn't ride up when a page has fewer rows.
    y = CONTENT_Y + (MAX_ROWS - 1) * ROW_PITCH;
    ui::ButtonProps prev;
    prev.label = "< Prev";
    prev.action = A_PAGE;
    prev.value = (int16_t)(page_ - 1);
    prev.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
    prev.styles = ghostButton();
    prev.minTouchSize = 0;
    prev.enabled = page_ > 0;  // disabled buttons register no hit rect
    prev.hitPadding = ui::Insets{2, 6, 4, 8};  // full row, left edge inward
    ui::button(c.frame, ui::Rect{MARGIN, (int16_t)(y + S(2)), S(110), (int16_t)(ROW_H - S(6))}, prev);

    ui::ButtonProps nextB = prev;
    nextB.label = "Next >";
    nextB.value = (int16_t)(page_ + 1);
    nextB.enabled = page_ + 1 < pageCount;
    nextB.hitPadding = ui::Insets{2, 8, 4, 6};  // full row, right edge inward
    ui::button(c.frame,
               ui::Rect{(int16_t)(W - MARGIN - S(110)), (int16_t)(y + S(2)), S(110), (int16_t)(ROW_H - S(6))}, nextB);

    const String pos = "Page " + String(page_ + 1) + " of " + String(pageCount);
    const int16_t posX = (int16_t)(MARGIN + S(110));
    ui::drawText(t, ui::Rect{posX, y, (int16_t)(W - 2 * posX), ROW_H}, pos.c_str(),
                 txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Center));
  }
}

void SettingsScreen::drawTzPicker(SettingsCanvas& c) {
  // Header with Back as its trailing action. A physical Back press still
  // closes via the (!ev && snap.back) fallback in handleInput.
  ui::HeaderProps hp;
  hp.title = "Timezone";
  hp.titleText = txt(FONT_BODY_B, ui::Color::White);
  ui::StyleSet headerStyle;
  headerStyle.normal.background = ui::Paint::solid(ui::Color::Black);
  hp.styles = headerStyle;
  hp.trailingLabel = "Back";
  hp.trailingAction = A_CLOSE;
  hp.trailingText = txt(FONT_SMALL_B, ui::Color::White, ui::TextAlign::Center);
  hp.trailingStyles = onHeaderButton();
  ui::header(c.frame, ui::Rect{0, 0, W, S(30)}, hp);

  // Zone list via the FreeInkUI list component (virtualized by topIndex;
  // vertical swipes page it through handleSwipe, sharing tzListRect()).
  static ui::ListItem items[TIMEZONE_COUNT];
  for (size_t i = 0; i < TIMEZONE_COUNT; ++i) {
    items[i] = ui::ListItem{};
    items[i].label = TIMEZONES[i].name;
    items[i].actionValue = (int16_t)i;
  }
  const ui::Rect listRect = tzListRect();
  ui::ListProps lp;
  lp.items = items;
  lp.count = TIMEZONE_COUNT;
  lp.topIndex = tzTop_;
  lp.selectedIndex = (int16_t)currentTzIndex();
  lp.action = A_TZ_ROW;
  lp.labelText = txt(FONT_SMALL);
  lp.rowHeight = TZ_ROW_H;
  lp.rowGap = 0;
  lp.selectionMarker = ui::SelectionMarker::Triangle;
  lp.scrollIndicator = true;
  // Solid-invert focus/press cue: the component's default LightGray dither
  // reads as broken speckle on the 1-bit panels (see ghostButton).
  {
    ui::StyleSet rows;
    rows.explicitlySet = true;
    rows.normal.background = ui::Paint::solid(ui::Color::White);
    rows.normal.foreground = ui::Paint::solid(ui::Color::Black);
    rows.focused.background = ui::Paint::solid(ui::Color::Black);
    rows.focused.foreground = ui::Paint::solid(ui::Color::White);
    rows.active = rows.focused;
    // Selected keeps the triangle marker as its cue (a solid fill on top of
    // the marker would hide which row is the current zone while focus moves).
    rows.selected = rows.normal;
    rows.disabled = rows.normal;
    lp.rowStyles = rows;
  }
  ui::list(c.frame, listRect, lp);

  const uint16_t visible = tzVisibleRows();

  ui::ButtonProps prev;
  prev.label = "Prev";
  prev.action = A_TZ_PG;
  prev.value = (int16_t)-visible;
  prev.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  prev.styles = ghostButton();
  prev.minTouchSize = 0;
  prev.enabled = tzTop_ > 0;
  // Bottom-edge reach is SDK-owned: the buttons end 4px from the bezel, inside
  // the 12px edge-snap threshold, so their hit rects already extend to it.
  ui::button(c.frame, ui::Rect{MARGIN, (int16_t)(H - S(32)), S(120), S(28)}, prev);

  ui::ButtonProps next = prev;
  next.label = "Next";
  next.value = (int16_t)visible;
  next.enabled = tzTop_ + visible < TIMEZONE_COUNT;
  ui::button(c.frame, ui::Rect{(int16_t)(W - MARGIN - S(120)), (int16_t)(H - S(32)), S(120), S(28)}, next);

  const uint16_t lastShown =
      tzTop_ + visible < TIMEZONE_COUNT ? (uint16_t)(tzTop_ + visible) : (uint16_t)TIMEZONE_COUNT;
  const String pos = String(tzTop_ + 1) + "-" + String(lastShown) + " of " + String((int)TIMEZONE_COUNT);
  const int16_t posX = (int16_t)(MARGIN + S(120) + S(8));
  ui::drawText(c.target, ui::Rect{posX, (int16_t)(H - S(32)), (int16_t)(W - 2 * posX), S(28)}, pos.c_str(),
               txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Center));
}

void SettingsScreen::overlayPause(SettingsCanvas& c) {
  // The active pause choice renders checked (solid), like the web dashboard's
  // segmented control, so the current state is visible before picking.
  const bool paused = stateStore().isPaused(time(nullptr));
  auto mark = [&](int v) {
    return (paused && stateStore().pauseChoiceMinutes == v) ? ui::StateChecked : ui::StateNormal;
  };
  const ui::DialogOption options[] = {
      {paused ? "Resume" : "Active", A_DLG, 0, ui::StateNormal, paused},
      {"30 minutes", A_DLG, 30, mark(30)},
      {"1 hour", A_DLG, 60, mark(60)},
      {"4 hours", A_DLG, 240, mark(240)},
      {"Until resumed", A_DLG, -1, mark(-1)},
  };
  ui::OptionDialogProps d;
  d.title = "Pause alarms";
  d.options = options;
  d.optionCount = 5;
  d.titleText = txt(FONT_SMALL_B);
  d.buttonText = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  d.buttonStyles = ghostButton();  // visible GPIO focus + checked fill
  ui::StyleSet panel;
  panel.normal.background = ui::Paint::solid(ui::Color::White);
  panel.normal.border = ui::Paint::solid(ui::Color::Black);
  panel.normal.borderWidth = 2;
  d.styles = panel;
  d.buttonHeight = S(30);
  d.gap = S(5);
  d.verticalOptions = true;
  // No dim scrim: the LightGray dither reads as broken speckle on a 1-bit
  // panel; the bordered panel separates fine on its own.
  d.dimBackground = false;
  const int16_t w = S(220);
  const int16_t h = ui::optionDialogHeight(c.target, d, w);
  ui::optionDialog(c.frame, ui::Rect{(int16_t)((W - w) / 2), (int16_t)((H - h) / 2), w, h}, d);
}

void SettingsScreen::overlayReboot(SettingsCanvas& c) {
  const ui::DialogOption options[] = {
      {"Cancel", A_DLG, 0},
      {"Reboot", A_DLG, 1},
  };
  ui::OptionDialogProps d;
  d.title = "Reboot device?";
  d.options = options;
  d.optionCount = 2;
  d.titleText = txt(FONT_SMALL_B);
  d.buttonText = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  d.buttonStyles = ghostButton();  // visible GPIO focus outline
  ui::StyleSet panel;
  panel.normal.background = ui::Paint::solid(ui::Color::White);
  panel.normal.border = ui::Paint::solid(ui::Color::Black);
  panel.normal.borderWidth = 2;
  d.styles = panel;
  d.buttonHeight = S(34);
  d.inputMask = ui::InputDefault | ui::InputBack;
  d.dimBackground = false;  // see overlayPause: dither scrim looks broken on 1-bit
  const int16_t w = S(260);
  const int16_t h = ui::optionDialogHeight(c.target, d, w);
  ui::optionDialog(c.frame, ui::Rect{(int16_t)((W - w) / 2), (int16_t)((H - h) / 2), w, h}, d);
}

void SettingsScreen::applyToggle(int16_t item) {
  AppSettings& s = settings();
  switch (item) {
    case IT_EARLY_EN: s.earlyMeetingAlertEnabled = !s.earlyMeetingAlertEnabled; break;
    case IT_SOUND_EN: s.alarmSoundEnabled = !s.alarmSoundEnabled; break;
    case IT_FLASH_EN: s.alarmFlashFrontlight = !s.alarmFlashFrontlight; break;
    case IT_MAYBE: s.showMaybeEvents = !s.showMaybeEvents; break;
    case IT_UNACCEPTED: s.alarmOnUnacceptedEvents = !s.alarmOnUnacceptedEvents; break;
    case IT_LINKS_ONLY: s.onlyAlertWithLinks = !s.onlyAlertWithLinks; break;
    case IT_ANY_URL: s.alertWithAnyLink = !s.alertWithAnyLink; break;
    case IT_WORK_EN: s.workHoursEnabled = !s.workHoursEnabled; break;
    case IT_24H: s.use24HourTime = !s.use24HourTime; break;
    default: return;
  }
  persist(item);
}

void SettingsScreen::applyStep(int16_t item, int dir) {
  AppSettings& s = settings();
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  // Lead times: floor of 1 minute (0 = alarm at meeting start, pointless);
  // fine steps below 5 so the low values are actually reachable.
  auto stepLead = [&](int v) {
    const int step = (v < 5 || (v == 5 && dir < 0)) ? 1 : 5;
    return clampi(v + dir * step, 1, 120);
  };
  switch (item) {
    case IT_LEAD: s.alarmLeadTimeMinutes = stepLead(s.alarmLeadTimeMinutes); break;
    case IT_EARLY_LEAD:
      s.earlyMeetingLeadTimeMinutes = stepLead(s.earlyMeetingLeadTimeMinutes);
      break;
    case IT_EARLY_BEFORE:
      stepTimeOfDay(s.earlyMeetingBeforeHour, s.earlyMeetingBeforeMinute, dir);
      break;
    case IT_AUTOSTOP: s.alarmMaxMinutes = clampi(s.alarmMaxMinutes + dir, 1, 30); break;
    case IT_VOLUME: s.alarmVolume = clampi(s.alarmVolume + dir * 10, 0, 100); break;
    case IT_FRONTLIGHT:
      s.frontlightBrightness = clampi(s.frontlightBrightness + dir * 10, 0, 100);
      frontlight_.setBrightness((uint8_t)s.frontlightBrightness);  // live preview
      break;
    case IT_WORK_START: stepTimeOfDay(s.workHoursStartHour, s.workHoursStartMinute, dir); break;
    case IT_WORK_END: stepTimeOfDay(s.workHoursEndHour, s.workHoursEndMinute, dir); break;
    case IT_LOOKAHEAD: s.lookaheadDays = clampi(s.lookaheadDays + dir, 1, 14); break;
    case IT_POLL: {
      int idx = 0;
      for (int i = 0; i < POLL_PRESET_COUNT; ++i) {
        if (POLL_PRESETS[i] <= s.pollIntervalMinutes) idx = i;
      }
      idx = clampi(idx + dir, 0, POLL_PRESET_COUNT - 1);
      s.pollIntervalMinutes = POLL_PRESETS[idx];
      break;
    }
    default: return;
  }
  persist(item);
}

void SettingsScreen::toggleWorkDay(int day) {
  auto& days = settings().workDays;
  for (auto it = days.begin(); it != days.end(); ++it) {
    if (*it == day) {
      days.erase(it);
      persist(IT_WORK_DAYS);
      return;
    }
  }
  days.push_back(day);
  persist(IT_WORK_DAYS);
}

void SettingsScreen::persist(int16_t item) {
  calendarManager().lockConfig();
  settings().save();
  calendarManager().unlockConfig();
  if (isFilterItem(item)) {
    calendarManager().refilterNow();
    calendarManager().requestSync();
  }
  if (item == IT_LOOKAHEAD) calendarManager().requestSync();
}

void SettingsScreen::focusFirst(ui::ActionId action) {
  const ui::Interaction* data = interactions_.data();
  for (int16_t i = 0; i < (int16_t)interactions_.count(); ++i) {
    if (data[i].action == action && !ui::hasState(data[i].state, ui::StateDisabled)) {
      interactions_.setFocusedIndex(i);
      return;
    }
  }
  interactions_.setFocusedIndex(-1);
}

void SettingsScreen::moveFocusWithin(ui::ActionId action, int dir) {
  const ui::Interaction* data = interactions_.data();
  const int n = (int)interactions_.count();
  int16_t idxs[16];
  int m = 0;
  for (int16_t i = 0; i < n && m < 16; ++i) {
    if (data[i].action == action && !ui::hasState(data[i].state, ui::StateDisabled)) idxs[m++] = i;
  }
  if (m == 0) return;
  int pos = -1;
  for (int k = 0; k < m; ++k) {
    if (idxs[k] == interactions_.focusedIndex()) pos = k;
  }
  pos = pos < 0 ? (dir > 0 ? 0 : m - 1) : (pos + dir + m) % m;
  interactions_.setFocusedIndex(idxs[pos]);
}

void SettingsScreen::handleSwipe(bool up, bool down, bool left, bool right) {
  // Dialogs stay tap/key-driven; a swipe over one is most likely a mis-tap.
  if (modal_ == Modal::PAUSE || modal_ == Modal::REBOOT) return;

  if (modal_ == Modal::TZ_PICKER) {
    if (!up && !down) return;
    // Same page step as the Prev/Next buttons (swipe up = later zones).
    const int visible = (int)tzVisibleRows();
    const int maxTop = (int)TIMEZONE_COUNT > visible ? (int)TIMEZONE_COUNT - visible : 0;
    int next = (int)tzTop_ + (up ? visible : -visible);
    if (next < 0) next = 0;
    if (next > maxTop) next = maxTop;
    if (next == (int)tzTop_) return;
    tzTop_ = (uint16_t)next;
    draw(EInkDisplay::HALF_REFRESH);
    return;
  }

  if (left || right) {
    // Swipe left pulls the next tab in, right the previous. No wrap — hitting
    // the end is a natural stop, not a jump back to the start.
    const int next = tab_ + (left ? 1 : -1);
    if (next < 0 || next >= TAB_COUNT) return;
    tab_ = next;
    page_ = 0;
    scroll_ = 0;
    draw(EInkDisplay::HALF_REFRESH);
    return;
  }

  // Vertical: page the current tab's rows (touch boards render a scroll
  // window; buttons-only boards never get here — no touch, no swipes).
  const TabDef& tab = TABS[tab_];
  if (tab.count <= MAX_ROWS) return;
  const int step = MAX_ROWS > 1 ? MAX_ROWS - 1 : 1;
  const int maxTop = tab.count - MAX_ROWS;
  int next = scroll_ + (up ? step : -step);
  if (next < 0) next = 0;
  if (next > maxTop) next = maxTop;
  if (next == scroll_) return;
  scroll_ = (int16_t)next;
  draw(EInkDisplay::HALF_REFRESH);
}

SettingsScreen::Result SettingsScreen::handleInput(const ui::InputSnapshot& snap) {
  // GPIO focus moves (buttons-only boards). The PAUSE/REBOOT dialogs overlay
  // the normal view, whose controls are still registered — scope their focus
  // to the dialog options so up/down can't land behind the dialog. Everywhere
  // else (tabbed view, full-screen TZ picker) the router's own focus walk is
  // correct. Redraw FAST to show the moved outline.
  if ((snap.focusNext || snap.focusPrev) && !snap.confirm && !snap.back && !snap.touchReleased) {
    if (modal_ == Modal::PAUSE || modal_ == Modal::REBOOT) {
      moveFocusWithin(A_DLG, snap.focusNext ? 1 : -1);
    } else {
      interactions_.route(snap);
    }
    draw(EInkDisplay::FAST_REFRESH);
    return Result::NONE;
  }

  const ui::ActionEvent ev = interactions_.route(snap);
  Serial.printf("[settings] route action=%u value=%d tap=(%d,%d) tab=%d page=%d modal=%d\n",
                (unsigned)ev.action, (int)ev.value, (int)snap.touchX, (int)snap.touchY, tab_,
                page_, (int)modal_);

  if (modal_ == Modal::TZ_PICKER) {
    bool structural = false;
    if (ev.action == A_TZ_ROW && ev.value >= 0 && ev.value < (int)TIMEZONE_COUNT) {
      calendarManager().lockConfig();
      settings().timezoneName = TIMEZONES[ev.value].name;
      settings().timezone = TIMEZONES[ev.value].posix;
      settings().save();
      calendarManager().unlockConfig();
      wifiService().applyTimeConfig();
      calendarManager().requestSync();
      modal_ = Modal::NONE;
      structural = true;
    } else if (ev.action == A_TZ_PG) {
      int next = (int)tzTop_ + ev.value;
      if (next < 0) next = 0;
      if (next >= (int)TIMEZONE_COUNT) next = tzTop_;  // shouldn't happen (button disabled)
      tzTop_ = (uint16_t)next;
      structural = true;  // a whole page of list text moves
    } else if (ev.action == A_CLOSE || (!ev && snap.back)) {
      modal_ = Modal::NONE;
      structural = true;
    }
    draw(structural ? EInkDisplay::HALF_REFRESH : EInkDisplay::FAST_REFRESH);
    return Result::NONE;
  }

  if (modal_ == Modal::PAUSE) {
    if (ev.action == A_DLG) {
      calendarManager().lockConfig();
      if (ev.value < 0) {
        stateStore().pauseUntil = 32472144000LL;  // "indefinite" = year 2999
      } else if (ev.value == 0) {
        stateStore().pauseUntil = 0;
      } else {
        stateStore().pauseUntil = time(nullptr) + (time_t)ev.value * 60;
      }
      stateStore().pauseChoiceMinutes = ev.value;
      stateStore().save();
      calendarManager().unlockConfig();
    } else if (!snap.touchReleased && !snap.back) {
      // GPIO confirm with nothing focused (or other non-input): keep the
      // dialog open. Only a chosen option, an outside tap, or Back closes.
      draw(EInkDisplay::FAST_REFRESH);
      return Result::NONE;
    }
    modal_ = Modal::NONE;
    interactions_.setFocusedIndex(-1);  // drop dialog-scoped focus
    draw(EInkDisplay::HALF_REFRESH);
    return Result::NONE;
  }

  if (modal_ == Modal::REBOOT) {
    if (ev.action == A_DLG && ev.value == 1) {
      Serial.println("[settings] reboot requested");
      delay(100);
      ESP.restart();
    }
    if (ev.action != A_DLG && !snap.touchReleased && !snap.back) {
      draw(EInkDisplay::FAST_REFRESH);
      return Result::NONE;  // see PAUSE: GPIO no-ops keep the dialog open
    }
    modal_ = Modal::NONE;
    interactions_.setFocusedIndex(-1);
    draw(EInkDisplay::HALF_REFRESH);
    return Result::NONE;
  }

  // Normal (tabbed) view. Take the ghost-clearing HALF refresh only for
  // structural changes (tab/page/dialog transitions repaint large regions and
  // need a full drive or the previous layout's residue reads as trash).
  // In-place value/state changes (stepper value, toggle, day chip) take the FAST
  // differential refresh: with the facade double-buffered, the driver diffs the
  // new frame against the previously displayed one and only drives the handful of
  // changed pixels — flash-free, and the per-refresh controller reset keeps the
  // new glyph crisp instead of ghosting over the old value.
  bool structural = false;
  switch (ev.action) {
    case A_CLOSE:
      return Result::CLOSED;
    case A_TAB:
      tab_ = ev.value;
      page_ = 0;
      scroll_ = 0;
      structural = true;
      break;
    case A_PAGE:
      page_ = ev.value < 0 ? 0 : ev.value;  // draw() re-clamps against pageCount
      structural = true;
      break;
    case A_TOGGLE:
      applyToggle(ev.value);
      break;
    case A_DEC:
      applyStep(ev.value, -1);
      break;
    case A_INC:
      applyStep(ev.value, +1);
      break;
    case A_DAY:
      toggleWorkDay(ev.value);
      break;
    case A_DO:
      switch (ev.value) {
        case IT_TEST: return Result::TEST_ALARM;
        case IT_SYNC: calendarManager().requestSync(); break;
        case IT_REBOOT: modal_ = Modal::REBOOT; structural = true; break;
        case IT_PAUSE: modal_ = Modal::PAUSE; structural = true; break;
        case IT_TZ: {
          modal_ = Modal::TZ_PICKER;
          const int cur = currentTzIndex();
          tzTop_ = cur > 2 ? (uint16_t)(cur - 2) : 0;
          structural = true;
          break;
        }
        default: break;
      }
      break;
    default:
      if (snap.back) return Result::CLOSED;  // back with no hit = leave settings
      break;
  }

  draw(structural ? EInkDisplay::HALF_REFRESH : EInkDisplay::FAST_REFRESH);
  // A dialog just opened on a buttons-only board: land GPIO focus on its first
  // option so confirm works immediately (the draw above registered the dialog's
  // interactions; one more FAST pass renders the focus outline).
  if (!BoardConfig::hasTouch() && structural &&
      (modal_ == Modal::PAUSE || modal_ == Modal::REBOOT)) {
    focusFirst(A_DLG);
    draw(EInkDisplay::FAST_REFRESH);
  }
  return Result::NONE;
}
