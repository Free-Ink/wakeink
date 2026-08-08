#pragma once

// Renders the WakeInk screens into the FreeInk 1-bit framebuffer (per-device
// landscape geometry, see ScreenGeometry.h) using FreeInkUI components
// (statusBar, header, drawText, button) drawn through a GfxText-backed
// DrawTarget. Layout follows the Android widget: bold event
// title, time line, "No upcoming events" placeholder — plus a status header,
// an upcoming list, full-screen alarm/setup views, and a tappable event popup.

#include <Arduino.h>
#include <EInkDisplay.h>
#include <FreeInkUI.h>

#include <vector>

#include "../calendar/CalendarManager.h"
#include "../calendar/Event.h"

class Screen {
 public:
  explicit Screen(EInkDisplay& display) : display_(display) {}

  // Tap routing result. action is one of the TAP_* constants; for TAP_EVENT,
  // value is the index into the event list passed to the last drawIdle().
  static constexpr int TAP_NONE = 0;    // must equal FreeInkUI NO_ACTION
  static constexpr int TAP_EVENT = 1;
  static constexpr int TAP_SKIP = 2;
  static constexpr int TAP_CANCEL = 3;
  static constexpr int TAP_SETTINGS = 4;  // the idle screen's cog button
  struct Tap {
    int action = TAP_NONE;
    int value = 0;
  };

  void drawIdle(const std::vector<Event>& events, const SyncStatus& sync, const String& ssid,
                const String& ip, const String& hostname, bool wifiUp, bool paused, time_t now);
  void drawAlarm(const Event& ev, time_t now);
  void drawEventPopup(const Event& ev, time_t now);
  void drawSetupPortal(const String& apSsid, const String& ip, const String& failNote = String());
  void drawMessage(const char* title, const char* line1, const char* line2 = nullptr);
  void drawHibernate(time_t now);  // "hibernating" banner + big live clock

  // Route an input snapshot (touch tap and/or physical buttons, already mapped
  // to logical coordinates by ui::snapshotFrom) against the interactive regions
  // registered by the last draw — no redraw needed (correct for slow e-ink).
  Tap route(const freeink::ui::InputSnapshot& input);

  // The canonical device context for this panel (size + orientation); shared by
  // the renderer and by the caller's ui::snapshotFrom touch mapping.
  freeink::ui::DeviceContext deviceContext() const;

  // GPIO focus (buttons-only boards). Focus indices live in the interaction
  // buffer and persist across redraws; the focused control renders highlighted
  // (inverted list row / outlined card / outlined dialog button) and a
  // confirm press activates it via route().
  void focusAction(int action);  // focus the first control carrying `action`
  void clearFocus();
  bool hasFocus() const;

  // Page the idle upcoming list (touch boards: vertical swipes; the list is a
  // virtualized ui::list with a scroll indicator). dir +1 = show later events.
  // Returns true when the window moved — the caller redraws.
  bool scrollUpcoming(int dir);

  // Pushes the framebuffer to the panel.
  void show(bool full);

  static String formatClock(time_t t, bool use24);
  static String formatDate(time_t t);

  // Capacity covers the upcoming list (≤32 rows) plus popup buttons.
  static constexpr size_t MAX_INTERACTIONS = 40;
  freeink::ui::InteractionBuffer<MAX_INTERACTIONS>& interactions() { return interactions_; }

 private:
  EInkDisplay& display_;
  freeink::ui::InteractionBuffer<MAX_INTERACTIONS> interactions_;
  // Upcoming-list scroll window, maintained across drawIdle() calls and
  // re-clamped there against the current event count / fitting rows.
  uint16_t upcomingTop_ = 0;
  uint16_t upcomingVisible_ = 0;
  uint16_t upcomingCount_ = 0;
};
