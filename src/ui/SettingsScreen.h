#pragma once

// On-device settings UI: opened from the idle screen's cog, composed entirely
// of FreeInkUI components (header, button tabs/steppers/toggles/day-chips,
// list for the timezone picker, optionDialog for pause + reboot confirm).
// Everything that doesn't need a keyboard lives here; free-text settings
// (keywords, calendars, WiFi) stay on the web dashboard.
//
// Interaction model matches the rest of the firmware: draw registers hit
// rects in a persistent InteractionBuffer, a later tap is routed against the
// last draw (no redraw needed to hit-test), and every state change redraws
// with a FAST refresh.

#include <Arduino.h>
#include <EInkDisplay.h>
#include <FreeInkUI.h>
#include <FrontlightManager.h>

class SettingsScreen {
 public:
  SettingsScreen(EInkDisplay& display, FrontlightManager& frontlight)
      : display_(display), frontlight_(frontlight) {}

  // What the caller (main loop) must do after handleInput.
  enum class Result {
    NONE,        // handled internally (already redrawn)
    CLOSED,      // user left settings — return to idle
    TEST_ALARM,  // fire the test alarm (also closes settings)
  };

  // Reset to the first tab and render with a FULL refresh.
  void open();

  // Route a mapped input snapshot (tap and/or Back) from ui::snapshotFrom.
  Result handleInput(const freeink::ui::InputSnapshot& snap);

  // Touch gestures (redraws internally): horizontal swipes switch tabs,
  // vertical swipes page the current tab's rows / the timezone list. No-op
  // over the pause/reboot dialogs.
  void handleSwipe(bool up, bool down, bool left, bool right);

  // Re-render in place (e.g. after a sync finishes, to update info rows).
  void redraw();

 private:
  enum class Modal { NONE, TZ_PICKER, PAUSE, REBOOT };

  // Refresh tiering: structural changes (tab/page/modal swaps) move big black
  // regions and need a ghost-clearing pass or the previous frame's residue
  // reads as "dithered" trash; small value tweaks can stay FAST.
  void draw(EInkDisplay::RefreshMode mode);
  void drawNormal(class SettingsCanvas& c);
  void drawTzPicker(class SettingsCanvas& c);
  void overlayPause(class SettingsCanvas& c);
  void overlayReboot(class SettingsCanvas& c);

  void applyStep(int16_t item, int dir);
  void applyToggle(int16_t item);
  void toggleWorkDay(int day);
  void persist(int16_t item);

  // GPIO focus helpers (buttons-only boards). The PAUSE/REBOOT dialogs draw
  // OVER the normal view, whose controls stay registered — so dialog focus
  // must be scoped to the dialog's own options instead of the router's
  // whole-table moveFocus.
  void focusFirst(freeink::ui::ActionId action);
  void moveFocusWithin(freeink::ui::ActionId action, int dir);

  EInkDisplay& display_;
  FrontlightManager& frontlight_;
  freeink::ui::InteractionBuffer<48> interactions_;
  int tab_ = 0;
  int page_ = 0;         // buttons-only boards: page within the current tab
  int16_t scroll_ = 0;   // touch boards: first visible row of the current tab
  Modal modal_ = Modal::NONE;
  uint16_t tzTop_ = 0;  // timezone picker scroll position
};
