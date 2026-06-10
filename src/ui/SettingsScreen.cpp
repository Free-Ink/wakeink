#include "SettingsScreen.h"

#include "../calendar/CalendarManager.h"
#include "../config/AppSettings.h"
#include "../config/StateStore.h"
#include "../config/Timezones.h"
#include "../net/WifiService.h"
#include "GfxTextDrawTarget.h"
#include "Screen.h"

namespace ui = freeink::ui;
using wakeink::FONT_BODY;
using wakeink::FONT_BODY_B;
using wakeink::FONT_SMALL;
using wakeink::FONT_SMALL_B;
using wakeink::FONT_TINY;

namespace {
constexpr int W = 416;
constexpr int H = 240;

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

constexpr int CONTENT_Y = 66;
constexpr int ROW_H = 28;
constexpr int MAX_ROWS = 6;  // rows that fit; longer tabs paginate

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
  return st;
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

}  // namespace

// Shared render context (same pattern as Screen's Canvas).
class SettingsCanvas {
 public:
  wakeink::GfxTextDrawTarget target;
  ui::DeviceContext device;
  ui::InputSnapshot input;
  ui::Frame<48> frame;

  SettingsCanvas(uint8_t* fb, ui::InteractionBuffer<48>& buf)
      : target(fb, W, H),
        device{W, H, ui::Orientation::LandscapeCounterClockwise, true, true, {}, 0},
        frame(target, device, input, buf) {}
};

void SettingsScreen::open() {
  tab_ = 0;
  page_ = 0;
  modal_ = Modal::NONE;
  draw(EInkDisplay::FULL_REFRESH);
}

void SettingsScreen::redraw() { draw(EInkDisplay::FAST_REFRESH); }

void SettingsScreen::draw(EInkDisplay::RefreshMode mode) {
  display_.clearScreen(0xFF);
  SettingsCanvas c(display_.getFrameBuffer(), interactions_);

  if (modal_ == Modal::TZ_PICKER) {
    drawTzPicker(c);
  } else {
    drawNormal(c);
    if (modal_ == Modal::PAUSE) overlayPause(c);
    if (modal_ == Modal::REBOOT) overlayReboot(c);
  }

  display_.displayBuffer(mode);
}

void SettingsScreen::drawNormal(SettingsCanvas& c) {
  ui::DrawTarget& t = c.target;

  // Fingers register low near the bezel, so controls near the bottom edge get
  // their hit zone extended DOWN to the screen edge (downward only — centered
  // minTouchSize expansion bleeds up into the row above and steals its taps).
  auto extendDown = [&](const ui::Rect& r, ui::ActionId a, int16_t v) {
    if (r.bottom() >= 200 && r.bottom() < H) {
      c.frame.hit(ui::Rect{r.x, r.bottom(), r.width, (int16_t)(H - r.bottom())}, a, v);
    }
  };

  // Header: title + Close (Back button routes here too).
  ui::HeaderProps hp;
  hp.title = "Settings";
  hp.titleText = txt(FONT_BODY_B, ui::Color::White);
  ui::StyleSet headerStyle;
  headerStyle.normal.background = ui::Paint::solid(ui::Color::Black);
  hp.styles = headerStyle;
  ui::header(c.frame, ui::Rect{0, 0, W, 30}, hp);

  ui::ButtonProps closeBtn;
  closeBtn.label = "Close";
  closeBtn.action = A_CLOSE;
  closeBtn.inputMask = ui::InputDefault | ui::InputBack;
  closeBtn.text = txt(FONT_SMALL_B, ui::Color::White, ui::TextAlign::Center);
  closeBtn.styles = onHeaderButton();
  closeBtn.minTouchSize = 0;
  ui::button(c.frame, ui::Rect{W - 70, 3, 64, 24}, closeBtn);

  // Tab bar.
  const int16_t tabW = (W - 8) / TAB_COUNT;
  for (int i = 0; i < TAB_COUNT; ++i) {
    ui::ButtonProps tabBtn;
    tabBtn.label = TAB_NAMES[i];
    tabBtn.action = A_TAB;
    tabBtn.value = i;
    tabBtn.state = i == tab_ ? ui::StateSelected : ui::StateNormal;
    tabBtn.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
    tabBtn.styles = ghostButton();
    tabBtn.minTouchSize = 0;
    ui::button(c.frame, ui::Rect{(int16_t)(4 + i * tabW), 34, (int16_t)(tabW - 2), 28}, tabBtn);
  }

  // Content rows (with simple pagination when a tab overflows).
  const TabDef& tab = TABS[tab_];
  const bool paged = tab.count > MAX_ROWS;
  const int rowsPerPage = paged ? MAX_ROWS - 1 : MAX_ROWS;  // reserve a pager row
  const int pageCount = paged ? (tab.count + rowsPerPage - 1) / rowsPerPage : 1;
  if (page_ >= pageCount) page_ = 0;
  const int first = page_ * rowsPerPage;
  const int last = min(tab.count, first + rowsPerPage);

  int16_t y = CONTENT_Y;
  for (int i = first; i < last; ++i, y += ROW_H) {
    const Row& row = tab.rows[i];
    const int16_t midY = y;

    // Label (no dimmed look on a 1-bit panel; INFO rows use a tiny-font value).
    ui::drawText(t, ui::Rect{10, midY, 190, ROW_H}, row.label, txt(FONT_SMALL));

    switch (row.kind) {
      case Kind::TOGGLE: {
        const bool on = toggleValue(row.item);
        ui::ButtonProps b;
        b.label = on ? "On" : "Off";
        b.action = A_TOGGLE;
        b.value = row.item;
        b.state = on ? ui::StateChecked : ui::StateNormal;
        b.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
        b.styles = ghostButton();
        b.minTouchSize = 0;
        const ui::Rect r{W - 70, (int16_t)(y + 2), 60, ROW_H - 6};
        ui::button(c.frame, r, b);
        extendDown(r, A_TOGGLE, row.item);
        break;
      }
      case Kind::STEPPER: {
        const String v = valueFor(row.item);
        ui::drawText(t, ui::Rect{W - 196, midY, 100, ROW_H}, v.c_str(),
                     txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Right));
        ui::ButtonProps minus;
        minus.label = "-";
        minus.action = A_DEC;
        minus.value = row.item;
        minus.text = txt(FONT_BODY_B, ui::Color::Black, ui::TextAlign::Center);
        minus.styles = ghostButton();
        minus.minTouchSize = 0;
        const ui::Rect rMinus{W - 88, (int16_t)(y + 2), 38, ROW_H - 6};
        ui::button(c.frame, rMinus, minus);
        extendDown(rMinus, A_DEC, row.item);
        ui::ButtonProps plus = minus;
        plus.label = "+";
        plus.action = A_INC;
        const ui::Rect rPlus{W - 46, (int16_t)(y + 2), 38, ROW_H - 6};
        ui::button(c.frame, rPlus, plus);
        extendDown(rPlus, A_INC, row.item);
        break;
      }
      case Kind::ACTION: {
        ui::ButtonProps b;
        b.label = row.label;
        b.action = A_DO;
        b.value = row.item;
        b.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
        b.styles = ghostButton();
        b.minTouchSize = 0;
        // Full-width action button replaces the plain label.
        t.fill(ui::Rect{8, midY, 200, ROW_H}, ui::Paint::solid(ui::Color::White), 0,
               ui::CornersAll);  // erase the label drawn above
        const ui::Rect r{8, (int16_t)(y + 2), W - 16, ROW_H - 6};
        ui::button(c.frame, r, b);
        extendDown(r, A_DO, row.item);
        break;
      }
      case Kind::PICKER: {
        const String v = valueFor(row.item) + "  >";
        ui::drawText(t, ui::Rect{150, midY, W - 160, ROW_H}, v.c_str(),
                     txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Right));
        c.frame.hit(ui::Rect{0, y, W, ROW_H}, A_DO, row.item);
        break;
      }
      case Kind::DAYS: {
        static const char* dayLetters[] = {"M", "T", "W", "T", "F", "S", "S"};
        const int16_t chipW = 28;
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
          const ui::Rect rChip{(int16_t)(W - 10 - (8 - d) * (chipW + 2)), (int16_t)(y + 2), chipW,
                               ROW_H - 6};
          ui::button(c.frame, rChip, chip);
          extendDown(rChip, A_DAY, (int16_t)d);
        }
        break;
      }
      case Kind::INFO: {
        const String v = valueFor(row.item);
        ui::drawText(t, ui::Rect{110, midY, W - 120, ROW_H}, v.c_str(),
                     txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Right));
        break;
      }
    }
  }

  if (paged) {
    // Explicit pager: Prev / "Page x of y" / Next (a lone cycling "More"
    // button read as a status line, not a control). Anchored to the last row
    // slot so it doesn't ride up when a page has fewer rows.
    y = CONTENT_Y + (MAX_ROWS - 1) * ROW_H;
    ui::ButtonProps prev;
    prev.label = "< Prev";
    prev.action = A_PAGE;
    prev.value = (int16_t)(page_ - 1);
    prev.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
    prev.styles = ghostButton();
    prev.minTouchSize = 0;
    prev.enabled = page_ > 0;
    const ui::Rect rPrev{8, (int16_t)(y + 2), 110, ROW_H - 6};
    ui::button(c.frame, rPrev, prev);
    if (prev.enabled) extendDown(rPrev, A_PAGE, prev.value);

    ui::ButtonProps nextB = prev;
    nextB.label = "Next >";
    nextB.value = (int16_t)(page_ + 1);
    nextB.enabled = page_ + 1 < pageCount;
    const ui::Rect rNext{W - 118, (int16_t)(y + 2), 110, ROW_H - 6};
    ui::button(c.frame, rNext, nextB);
    if (nextB.enabled) extendDown(rNext, A_PAGE, nextB.value);

    const String pos = "Page " + String(page_ + 1) + " of " + String(pageCount);
    ui::drawText(t, ui::Rect{126, y, W - 252, ROW_H}, pos.c_str(),
                 txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Center));
  }
}

void SettingsScreen::drawTzPicker(SettingsCanvas& c) {
  ui::HeaderProps hp;
  hp.title = "Timezone";
  hp.titleText = txt(FONT_BODY_B, ui::Color::White);
  ui::StyleSet headerStyle;
  headerStyle.normal.background = ui::Paint::solid(ui::Color::Black);
  hp.styles = headerStyle;
  ui::header(c.frame, ui::Rect{0, 0, W, 30}, hp);

  ui::ButtonProps backBtn;
  backBtn.label = "Back";
  backBtn.action = A_CLOSE;
  backBtn.inputMask = ui::InputDefault | ui::InputBack;
  backBtn.text = txt(FONT_SMALL_B, ui::Color::White, ui::TextAlign::Center);
  backBtn.styles = onHeaderButton();
  backBtn.minTouchSize = 0;
  ui::button(c.frame, ui::Rect{W - 70, 3, 64, 24}, backBtn);

  // Zone list via the FreeInkUI list component (virtualized by topIndex).
  static ui::ListItem items[TIMEZONE_COUNT];
  for (size_t i = 0; i < TIMEZONE_COUNT; ++i) {
    items[i] = ui::ListItem{};
    items[i].label = TIMEZONES[i].name;
    items[i].actionValue = (int16_t)i;
  }
  const ui::Rect listRect{8, 34, W - 16, H - 34 - 36};
  ui::ListProps lp;
  lp.items = items;
  lp.count = TIMEZONE_COUNT;
  lp.topIndex = tzTop_;
  lp.selectedIndex = (int16_t)currentTzIndex();
  lp.action = A_TZ_ROW;
  lp.labelText = txt(FONT_SMALL);
  lp.rowHeight = 27;
  lp.selectionMarker = ui::SelectionMarker::Triangle;
  ui::list(c.frame, listRect, lp);

  const uint16_t visible = ui::listVisibleRows(listRect, lp.rowHeight, lp.rowGap);

  ui::ButtonProps prev;
  prev.label = "Prev";
  prev.action = A_TZ_PG;
  prev.value = (int16_t)-visible;
  prev.text = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  prev.styles = ghostButton();
  prev.minTouchSize = 0;
  prev.enabled = tzTop_ > 0;
  ui::button(c.frame, ui::Rect{8, H - 32, 120, 28}, prev);
  // Bottom-edge: extend the hit zones to the bezel (downward only).
  if (prev.enabled) c.frame.hit(ui::Rect{8, H - 4, 120, 4}, A_TZ_PG, prev.value);

  ui::ButtonProps next = prev;
  next.label = "Next";
  next.value = (int16_t)visible;
  next.enabled = tzTop_ + visible < TIMEZONE_COUNT;
  ui::button(c.frame, ui::Rect{W - 128, H - 32, 120, 28}, next);
  if (next.enabled) c.frame.hit(ui::Rect{W - 128, H - 4, 120, 4}, A_TZ_PG, next.value);

  const uint16_t lastShown =
      tzTop_ + visible < TIMEZONE_COUNT ? (uint16_t)(tzTop_ + visible) : (uint16_t)TIMEZONE_COUNT;
  const String pos = String(tzTop_ + 1) + "-" + String(lastShown) + " of " + String((int)TIMEZONE_COUNT);
  ui::drawText(c.target, ui::Rect{136, H - 32, W - 272, 28}, pos.c_str(),
               txt(FONT_TINY, ui::Color::Black, ui::TextAlign::Center));
}

void SettingsScreen::overlayPause(SettingsCanvas& c) {
  const ui::DialogOption options[] = {
      {"Resume", A_DLG, 0, ui::StateNormal, stateStore().isPaused(time(nullptr))},
      {"30 minutes", A_DLG, 30},
      {"1 hour", A_DLG, 60},
      {"4 hours", A_DLG, 240},
      {"Until resumed", A_DLG, -1},
  };
  ui::OptionDialogProps d;
  d.title = "Pause alarms";
  d.options = options;
  d.optionCount = 5;
  d.titleText = txt(FONT_SMALL_B);
  d.buttonText = txt(FONT_SMALL_B, ui::Color::Black, ui::TextAlign::Center);
  ui::StyleSet panel;
  panel.normal.background = ui::Paint::solid(ui::Color::White);
  panel.normal.border = ui::Paint::solid(ui::Color::Black);
  panel.normal.borderWidth = 2;
  d.styles = panel;
  d.buttonHeight = 30;
  d.gap = 5;
  d.verticalOptions = true;
  // No dim scrim: the LightGray dither reads as broken speckle on a 1-bit
  // panel; the bordered panel separates fine on its own.
  d.dimBackground = false;
  const int16_t w = 220;
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
  ui::StyleSet panel;
  panel.normal.background = ui::Paint::solid(ui::Color::White);
  panel.normal.border = ui::Paint::solid(ui::Color::Black);
  panel.normal.borderWidth = 2;
  d.styles = panel;
  d.buttonHeight = 34;
  d.inputMask = ui::InputDefault | ui::InputBack;
  d.dimBackground = false;  // see overlayPause: dither scrim looks broken on 1-bit
  const int16_t w = 260;
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
  switch (item) {
    case IT_LEAD: s.alarmLeadTimeMinutes = clampi(s.alarmLeadTimeMinutes + dir * 5, 0, 120); break;
    case IT_EARLY_LEAD:
      s.earlyMeetingLeadTimeMinutes = clampi(s.earlyMeetingLeadTimeMinutes + dir * 5, 0, 120);
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

SettingsScreen::Result SettingsScreen::handleInput(const ui::InputSnapshot& snap) {
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
    }
    modal_ = Modal::NONE;  // any tap (option or outside) closes the dialog
    draw(EInkDisplay::HALF_REFRESH);
    return Result::NONE;
  }

  if (modal_ == Modal::REBOOT) {
    if (ev.action == A_DLG && ev.value == 1) {
      Serial.println("[settings] reboot requested");
      delay(100);
      ESP.restart();
    }
    modal_ = Modal::NONE;
    draw(EInkDisplay::HALF_REFRESH);
    return Result::NONE;
  }

  // Normal (tabbed) view. Tab/page/dialog transitions repaint large regions,
  // so they take the ghost-clearing HALF refresh; value tweaks stay FAST.
  bool structural = false;
  switch (ev.action) {
    case A_CLOSE:
      return Result::CLOSED;
    case A_TAB:
      tab_ = ev.value;
      page_ = 0;
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
  return Result::NONE;
}
