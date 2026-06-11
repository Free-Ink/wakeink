// WakeInk — In Your Space ported to FreeInk e-ink devices (Murphy M3 and
// M5Stack PaperColor; one binary per device, selected by the build env).
//
// Calendars come in as Google Calendar ICS links pasted into the local web
// dashboard (http://wakeink.local), get polled in a background task, filtered
// with the same rules as the Android app, rendered widget-style on the e-ink
// panel, and ring a full-screen alarm (with looping sound, plus a frontlight
// flash where the board has a frontlight) before meetings. Boards without
// touch (PaperColor) are configured entirely on the web dashboard; their
// buttons dismiss alarms and force a sync.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkUIInputManager.h>  // ui::snapshotFrom (orientation-aware touch)
#include <FrontlightManager.h>
#include <InputManager.h>
#include <LedManager.h>
#include <LittleFS.h>

#include "audio/AlarmSound.h"
#include "calendar/CalendarManager.h"
#include "config/AppSettings.h"
#include "config/CalendarStore.h"
#include "config/StateStore.h"
#include "config/WifiStore.h"
#include "net/WifiService.h"
#include "ui/Screen.h"
#include "ui/ScreenGeometry.h"
#include "ui/SettingsScreen.h"
#include "web/WebUi.h"

namespace ui = freeink::ui;

namespace {

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager input;
FrontlightManager frontlight;
LedManager leds;  // PaperColor's two RGB LEDs; no-op stub on boards without LEDs
Screen* screen = nullptr;
SettingsScreen* settingsScreen = nullptr;

enum class UiState { BOOTING, PORTAL, CONNECTING, IDLE, ALARM, POPUP, SETTINGS, HIBERNATING };
UiState uiState = UiState::BOOTING;
uint32_t settingsLastInputMs = 0;  // auto-exit settings after idle timeout

constexpr uint32_t SETTINGS_IDLE_EXIT_MS = 120000;  // 2 min untouched -> back to clock

Event ringingEvent;
Event popupEvent;  // the event whose skip/cancel popup is showing
uint32_t alarmStartedMs = 0;
uint32_t lastFlashToggleMs = 0;
bool flashOn = false;

int lastDrawnMinute = -1;
uint32_t fastRefreshCount = 0;

constexpr uint32_t FLASH_PERIOD_MS = 600;
constexpr uint32_t FULL_REFRESH_EVERY = 30;  // minute ticks between deep cleans

// Up+down key chord (buttons-only boards) toggles dark mode, mirroring
// CrossPoint's PaperColor invert combo: one toggle per chord press, and a
// cooldown so the chord's own key releases don't fire the single-key actions.
bool darkChordArmed = true;
uint32_t lastChordMs = 0;
constexpr uint32_t CHORD_COOLDOWN_MS = 600;

// Idle key actions are suppressed until this time — set when an alarm is
// dismissed so the dismissing key's release can't double as an idle action.
uint32_t keyQuietUntilMs = 0;

// LED watchdog: one of the PaperColor's LEDs rides 3V3_L2 (per the board
// silkscreen) — the main power layer every panel refresh loads down. A dip
// re-PORs the LED into latched garbage (a stuck green pixel) at some point
// AFTER the refresh returns, so per-refresh re-asserts race it and lose.
// Instead the loop re-pushes the LED buffer twice a second in every UI state;
// garbage survives at most half a second. No-op on boards without LEDs.
uint32_t lastLedAssertMs = 0;
constexpr uint32_t LED_ASSERT_PERIOD_MS = 500;

// Panel DC-balance maintenance (M5 PaperColor): interrupted refreshes cut the
// OTP waveform short, so each one leaves a little net charge on the pixels —
// over hours the panel darkens and colors fade. The complete waveform is the
// only DC-balanced cycle; promote one redraw per hour to it (~15 s blackout).
// requestCompleteWaveformNextRefresh() is a no-op on other panels.
uint32_t lastCompleteWaveformMs = 0;
constexpr uint32_t COMPLETE_WAVEFORM_INTERVAL_MS = 60UL * 60UL * 1000UL;
// Don't start a ~15 s blocking refresh if an alarm would ring during/near it.
constexpr time_t MAINTENANCE_ALARM_GUARD_S = 180;

bool panelMaintenanceDue(uint32_t ms) {
  return ms - lastCompleteWaveformMs >= COMPLETE_WAVEFORM_INTERVAL_MS;
}

// Touch→logical mapping is SDK-owned (ui::snapshotFrom + touchToLogical, keyed
// off the device orientation). These flips compensate only for mirrored panel
// mounting — if taps land mirrored on the bench, flip the matching constant.
constexpr bool TOUCH_FLIP_X = false;
constexpr bool TOUCH_FLIP_Y = false;

// "#rrggbb" (validated by AppSettings) -> LedColor.
LedColor ledColorFromHex(const String& hex) {
  const long v = strtol(hex.c_str() + 1, nullptr, 16);
  return LedColor::rgb((uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                       (uint8_t)(v & 0xFF));
}

void drawIdleScreen(bool full) {
  const time_t now = time(nullptr);
  screen->drawIdle(calendarManager().snapshot(), calendarManager().status(),
                   wifiService().currentSsid(), wifiService().ip(), settings().hostname,
                   wifiService().mode() == WifiService::CONNECTED,
                   stateStore().isPaused(now), now);
  screen->show(full);
  struct tm tmv;
  localtime_r(&now, &tmv);
  lastDrawnMinute = tmv.tm_min;
}

void enterIdle() {
  uiState = UiState::IDLE;
  // Restore the user's steady frontlight level (0 = off) after alarms/menus.
  frontlight.setBrightness((uint8_t)settings().frontlightBrightness);
  fastRefreshCount = 0;
  drawIdleScreen(true);
}

void drawHibernateScreen(bool full) {
  const time_t now = time(nullptr);
  screen->drawHibernate(now);
  screen->show(full);
  struct tm tmv;
  localtime_r(&now, &tmv);
  lastDrawnMinute = tmv.tm_min;
}

// Hibernate: WiFi goes off entirely — which inherently stops syncing (the
// sync task gates on WL_CONNECTED) and alarms stay quiet because nextAlarm is
// only polled in the active states. The frontlight goes dark and the screen
// parks on a banner + big clock that keeps ticking. Any key release (or tap)
// wakes: WiFi reconnects and a sync catches up.
void enterHibernate() {
  uiState = UiState::HIBERNATING;
  wifiService().suspend();
  frontlight.setBrightness(0);
  fastRefreshCount = 0;
  drawHibernateScreen(true);
}

void wakeFromHibernate() {
  wifiService().resume();  // restarts the connect flow (or the setup portal)
  calendarManager().requestSync();
  // resume() lands in CONNECTING (or AP_PORTAL with no saved networks); the
  // normal modeChanged() handling takes over from idle once it resolves.
  if (wifiService().mode() == WifiService::AP_PORTAL) {
    uiState = UiState::PORTAL;
    screen->drawSetupPortal(WifiService::AP_SSID, wifiService().ip());
    screen->show(true);
    return;
  }
  enterIdle();
}

void startTestAlarm(time_t now);  // defined below startAlarm

void startAlarm(const Event& ev) {
  ringingEvent = ev;
  uiState = UiState::ALARM;
  alarmStartedMs = millis();
  lastFlashToggleMs = millis();
  flashOn = true;
  if (settings().alarmFlashFrontlight) frontlight.setBrightness(100);
  if (settings().alarmSoundEnabled) {
    // Loops until stopAlarm(); falls back to the embedded default sound when
    // no custom /alarm.wav is uploaded.
    alarmsound::startLoop((uint8_t)settings().alarmVolume);
  }
  screen->drawAlarm(ev, time(nullptr));
  screen->show(true);
  // LEDs go last, AFTER the panel refresh: the refresh's current draw can sag
  // the PM1 LED rail and re-POR the LEDs, wiping anything written earlier.
  // One "flash light" setting drives whatever light the board has — the
  // frontlight strobe on Murphy, the two RGB LEDs (the configured color pair,
  // swapped between the LEDs each beat) on the PaperColor.
  if (settings().alarmFlashFrontlight) {
    leds.setBrightness((uint8_t)(settings().alarmLedBrightness * 255 / 100));
    leds.setColor(0, ledColorFromHex(settings().alarmLedColorA));
    leds.setColor(1, ledColorFromHex(settings().alarmLedColorB));
    leds.show();
  }
}

void stopAlarm(bool fired) {
  alarmsound::stop();
  if (fired) calendarManager().markFired(ringingEvent);
  // The dismissing key's release lands after we're back in IDLE — keep it
  // from triggering the idle key actions (notably hibernate on Murphy's UP).
  keyQuietUntilMs = millis() + 600;
  enterIdle();
  // Clear the LEDs AFTER enterIdle()'s full panel refresh — clearing first
  // left them vulnerable to a rail-sag re-POR during the refresh, which is
  // how a stuck (typically green) LED survived dismissal.
  leds.clear();
}

void startTestAlarm(time_t now) {
  Event test;
  test.uid = "test";
  test.title = "Test alarm";
  test.start = now + settings().alarmLeadTimeMinutes * 60;
  test.end = test.start + 1800;
  startAlarm(test);
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // Boot forensics: why did we restart, and what memory do we have?
  Serial.printf("[wakeink] v%s reset_reason=%d heap=%lu psram=%lu\n", WAKEINK_VERSION,
                (int)esp_reset_reason(), (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getPsramSize());

  // PSRAM integrity self-test: heap-corruption crashes during heavy PSRAM
  // traffic look exactly like marginal PSRAM timing. 3 passes x 512 KB of
  // xorshift pattern; a FAIL here means disable PSRAM (platformio.ini) or
  // the module needs a slower PSRAM clock.
  if (ESP.getPsramSize() > 0) {
    constexpr size_t TEST_BYTES = 512 * 1024;
    uint32_t* buf = (uint32_t*)heap_caps_malloc(TEST_BYTES, MALLOC_CAP_SPIRAM);
    bool pass = buf != nullptr;
    for (int round = 0; pass && round < 3; ++round) {
      uint32_t seed = 0x9E3779B9u + round;
      uint32_t x = seed;
      for (size_t i = 0; i < TEST_BYTES / 4; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = x;
      }
      x = seed;
      for (size_t i = 0; i < TEST_BYTES / 4; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (buf[i] != x) {
          Serial.printf("[wakeink] PSRAM TEST FAIL round=%d offset=%u got=%08lx want=%08lx\n",
                        round, (unsigned)(i * 4), (unsigned long)buf[i], (unsigned long)x);
          pass = false;
          break;
        }
      }
    }
    free(buf);
    Serial.printf("[wakeink] psram self-test: %s\n", pass ? "PASS" : "FAIL");
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[wakeink] LittleFS mount failed");
  }

  settings().load();
  setenv("TZ", settings().timezone.c_str(), 1);
  tzset();
  wifiStore().load();
  calendarStore().load();
  stateStore().load();

  display.begin();
  // The UI's compile-time geometry (ScreenGeometry.h) must match the logical
  // framebuffer the active panel driver reports; a mismatch here means a new
  // device env shipped without its ScreenGeometry.h entry.
  if (display.getDisplayWidth() != wakeink::SCREEN_W ||
      display.getDisplayHeight() != wakeink::SCREEN_H) {
    Serial.printf("[wakeink] GEOMETRY MISMATCH: driver %ux%u, UI built for %dx%d\n",
                  display.getDisplayWidth(), display.getDisplayHeight(), wakeink::SCREEN_W,
                  wakeink::SCREEN_H);
  }
  input.begin();
  frontlight.begin();
  leds.begin();
  if (!alarmsound::audio().begin()) {
    Serial.println("[wakeink] audio unavailable (no codec?)");
  }

  screen = new Screen(display);
  settingsScreen = new SettingsScreen(display, frontlight);
  frontlight.setBrightness((uint8_t)settings().frontlightBrightness);
  // The PaperColor's interrupted refreshes never run the panel's complete OTP
  // waveform, so residue accumulates across reboots. Run it once on the boot
  // screen for a true-white baseline (~15 s on the M5; no-op on other panels).
  display.requestCompleteWaveformNextRefresh();
  screen->drawMessage("WakeInk", "Starting up...", "v" WAKEINK_VERSION);
  screen->show(true);
  lastCompleteWaveformMs = millis();  // the boot refresh starts the hourly clock
  webUi().notePanelMaintenance();

  // Boot refreshes (the complete waveform above, the portal screen below) may
  // have re-POR'd the 3V3_L2 LED into garbage — settle it before loop() takes
  // over with the periodic re-assert.
  leds.clear();

  wifiService().begin();
  webUi().begin();
  calendarManager().begin();

  if (wifiService().mode() == WifiService::AP_PORTAL) {
    uiState = UiState::PORTAL;
    screen->drawSetupPortal(WifiService::AP_SSID, wifiService().ip());
    screen->show(true);
  } else {
    uiState = UiState::CONNECTING;
    screen->drawMessage("WakeInk", "Connecting to WiFi...",
                        wifiStore().lastConnectedSsid.c_str());
    screen->show(true);
  }
}

void loop() {
  wifiService().loop();
  webUi().loop();
  input.update();

  const time_t now = time(nullptr);
  const uint32_t ms = millis();

  // One snapshot per loop: physical buttons + a touch tap already mapped to
  // logical coordinates by the SDK's orientation-aware transform.
  const ui::InputSnapshot snap =
      ui::snapshotFrom(input, screen->deviceContext(), TOUCH_FLIP_X, TOUCH_FLIP_Y);
  const bool gotTap = snap.touchReleased;
  // Any input dismisses an alarm (touch tap or a physical key).
  const bool anyInput = gotTap || input.wasAnyPressed();
  // Release edges drive the idle/hibernate key actions: a synthesized CONFIRM
  // (M5's shared key) arrives as press+release in one update, and acting on
  // release keeps chords and wake presses from double-firing.
  const bool anyKeyReleased =
      input.wasReleased(InputManager::BTN_UP) || input.wasReleased(InputManager::BTN_DOWN) ||
      input.wasReleased(InputManager::BTN_CONFIRM) || input.wasReleased(InputManager::BTN_BACK);

  // WiFi state transitions drive the boot screens. While hibernating they are
  // ignored entirely (the device is asleep); wakeFromHibernate() catches up.
  if (wifiService().modeChanged() && uiState != UiState::HIBERNATING) {
    switch (wifiService().mode()) {
      case WifiService::AP_PORTAL: {
        uiState = UiState::PORTAL;
        String note;
        if (!wifiService().lastFailedSsid().isEmpty()) {
          note = "Couldn't join \"" + wifiService().lastFailedSsid() + "\" — " +
                 wifiService().lastFailReason();
        }
        screen->drawSetupPortal(WifiService::AP_SSID, wifiService().ip(), note);
        screen->show(true);
        break;
      }
      case WifiService::CONNECTED:
        if (uiState != UiState::ALARM) {
          calendarManager().requestSync();
          enterIdle();
        }
        break;
      default:
        break;
    }
  }

  switch (uiState) {
    case UiState::ALARM: {
      webUi().consumeTestAlarm();  // ignore while already ringing

      // Light strobe while ringing: frontlight pulse + LEDs swapping colors.
      if (settings().alarmFlashFrontlight && ms - lastFlashToggleMs >= FLASH_PERIOD_MS) {
        lastFlashToggleMs = ms;
        flashOn = !flashOn;
        frontlight.setBrightness(flashOn ? 100 : 10);
        const LedColor a = ledColorFromHex(settings().alarmLedColorA);
        const LedColor b = ledColorFromHex(settings().alarmLedColorB);
        leds.setColor(0, flashOn ? a : b);
        leds.setColor(1, flashOn ? b : a);
        leds.show();
      }
      const bool timedOut = ms - alarmStartedMs > (uint32_t)settings().alarmMaxMinutes * 60000UL;
      const bool eventOver = now > ringingEvent.end;
      if (anyInput || webUi().consumeDismiss() || timedOut || eventOver) {
        stopAlarm(true);
      }
      break;
    }

    case UiState::IDLE: {
      // Swallow a dismiss clicked while nothing was ringing so it can't
      // insta-dismiss the next alarm.
      webUi().consumeDismiss();

      // Alarm trigger check.
      Event candidate;
      if (calendarManager().nextAlarm(now, candidate)) {
        startAlarm(candidate);
        break;
      }
      if (webUi().consumeTestAlarm()) {
        startTestAlarm(now);
        break;
      }

      // Touch boards (Murphy): the top side key (GPIO1 / BTN_UP) hibernates;
      // everything else is touch-driven.
      if (BoardConfig::hasTouch() && input.wasReleased(InputManager::BTN_UP) &&
          ms >= keyQuietUntilMs) {
        enterHibernate();
        break;
      }

      // No touch on this board: settings/skip live on the web dashboard, so
      // the physical keys drive everything. Holding up+down together toggles
      // dark mode. On a single key release: a short confirm press (GPIO1 on
      // the PaperColor) hibernates, a press-and-hold of the same key (the
      // synthesized BACK) forces a complete-waveform deep clean (~15 s on the
      // M5 — clears accumulated ghosting), and up/down force a sync + redraw
      // (the same as tapping empty space below). Single-key actions fire on
      // RELEASE, after a chord cooldown, so a forming or ending chord can't
      // trigger them.
      if (!BoardConfig::hasTouch()) {
        if (input.isPressed(InputManager::BTN_UP) && input.isPressed(InputManager::BTN_DOWN)) {
          lastChordMs = ms;
          if (darkChordArmed) {
            darkChordArmed = false;
            settings().darkMode = !settings().darkMode;
            settings().save();
            drawIdleScreen(true);
          }
          break;
        }
        darkChordArmed = true;

        if (anyKeyReleased && ms - lastChordMs > CHORD_COOLDOWN_MS && ms >= keyQuietUntilMs) {
          if (snap.confirm) {
            enterHibernate();
            break;
          }
          if (input.wasReleased(InputManager::BTN_BACK)) {
            display.requestCompleteWaveformNextRefresh();
            lastCompleteWaveformMs = ms;  // manual clean resets the hourly pass
            webUi().notePanelMaintenance();
            calendarManager().requestSync();
            drawIdleScreen(true);  // complete waveform (~15 s)
            drawIdleScreen(true);  // uniform interrupted baseline
            break;
          }
          calendarManager().requestSync();
          drawIdleScreen(true);
          break;
        }
      }

      // Touch: tap an event for its skip popup, the cog for settings, or empty
      // space to force a sync + redraw.
      if (gotTap) {
        const Screen::Tap r = screen->route(snap);
        if (r.action == Screen::TAP_EVENT) {
          const auto events = calendarManager().snapshot();
          if (r.value >= 0 && r.value < (int)events.size()) {
            popupEvent = events[r.value];
            uiState = UiState::POPUP;
            screen->drawEventPopup(popupEvent, now);
            screen->show(true);
          }
        } else if (r.action == Screen::TAP_SETTINGS) {
          uiState = UiState::SETTINGS;
          settingsLastInputMs = ms;
          settingsScreen->open();
        } else {
          calendarManager().requestSync();
          drawIdleScreen(true);
        }
        break;
      }

      // Redraw when new data landed or settings changed.
      if (calendarManager().consumeDirtyFlag() || webUi().consumeDisplayRefresh()) {
        drawIdleScreen(false);
        break;
      }

      // Minute tick keeps the clock + countdowns fresh; occasionally deep-clean.
      struct tm tmv;
      localtime_r(&now, &tmv);
      if (tmv.tm_min != lastDrawnMinute && now > 1600000000) {
        ++fastRefreshCount;
        const bool full = fastRefreshCount % FULL_REFRESH_EVERY == 0;
        // Hourly DC-balance pass, skipped when an alarm is about to ring (the
        // complete waveform blocks the loop for ~15 s). The complete waveform
        // settles true white but every later partial update is interrupted-
        // yellow, which reads as a stain — so follow it immediately with a
        // full interrupted pass to put the whole panel on one uniform
        // baseline. (The balance benefit comes from the complete cycle having
        // run, not from its white staying on screen.)
        Event soon;
        if (panelMaintenanceDue(ms) &&
            !calendarManager().nextAlarm(now + MAINTENANCE_ALARM_GUARD_S, soon)) {
          display.requestCompleteWaveformNextRefresh();
          lastCompleteWaveformMs = ms;
          webUi().notePanelMaintenance();
          drawIdleScreen(true);  // complete waveform (~15 s)
          drawIdleScreen(true);  // uniform interrupted baseline
        } else {
          drawIdleScreen(full);
        }
      }
      break;
    }

    case UiState::SETTINGS: {
      // An imminent alarm preempts the menu.
      Event candidate;
      if (calendarManager().nextAlarm(now, candidate)) {
        startAlarm(candidate);
        break;
      }
      // Keep info rows (last sync, pause countdown) fresh when a sync lands.
      if (calendarManager().consumeDirtyFlag()) settingsScreen->redraw();

      if (gotTap || snap.back) {
        settingsLastInputMs = ms;
        const SettingsScreen::Result r = settingsScreen->handleInput(snap);
        if (r == SettingsScreen::Result::CLOSED) {
          enterIdle();
        } else if (r == SettingsScreen::Result::TEST_ALARM) {
          startTestAlarm(now);
        }
      } else if (ms - settingsLastInputMs > SETTINGS_IDLE_EXIT_MS) {
        enterIdle();  // forgotten menu returns to the clock
      }
      break;
    }

    case UiState::POPUP: {
      // An imminent alarm preempts the popup.
      Event candidate;
      if (calendarManager().nextAlarm(now, candidate)) {
        startAlarm(candidate);
        break;
      }
      if (gotTap || snap.back) {
        // One route() handles both: a tap resolves by position, a Back press
        // resolves to the Cancel button (registered with InputBack).
        const Screen::Tap r = screen->route(snap);
        if (r.action == Screen::TAP_SKIP) {
          calendarManager().lockConfig();
          stateStore().skip(popupEvent.skipKey());
          calendarManager().unlockConfig();
          calendarManager().refilterNow();  // drop it from the list now
        }
        // SKIP, CANCEL, or a tap outside the buttons all close the popup.
        enterIdle();
      }
      break;
    }

    case UiState::HIBERNATING: {
      // Asleep: WiFi is off, so no syncing and no alarms — but the big clock
      // keeps ticking. Drain web triggers latched before suspend so they
      // can't fire on wake; any key release (or tap) wakes.
      webUi().consumeTestAlarm();
      webUi().consumeDismiss();
      webUi().consumeDisplayRefresh();
      if (gotTap || anyKeyReleased) {
        wakeFromHibernate();
        break;
      }
      struct tm tmv;
      localtime_r(&now, &tmv);
      if (tmv.tm_min != lastDrawnMinute && now > 1600000000) {
        ++fastRefreshCount;
        const bool full = fastRefreshCount % FULL_REFRESH_EVERY == 0;
        // Hourly DC-balance pass (no alarm guard needed — nothing rings
        // here). Same complete-then-interrupted pair as idle, so the parked
        // screen stays on one uniform background.
        if (panelMaintenanceDue(ms)) {
          display.requestCompleteWaveformNextRefresh();
          lastCompleteWaveformMs = ms;
          webUi().notePanelMaintenance();
          drawHibernateScreen(true);  // complete waveform (~15 s)
          drawHibernateScreen(true);  // uniform interrupted baseline
        } else {
          drawHibernateScreen(full);
        }
      }
      break;
    }

    case UiState::PORTAL:
    case UiState::CONNECTING:
      if (webUi().consumeTestAlarm() || webUi().consumeDismiss()) {
        // ignore while not running
      }
      break;

    case UiState::BOOTING:
      break;
  }

  // LED watchdog (see lastLedAssertMs): re-push the buffer so a rail-dip
  // re-POR can't leave an LED latched with garbage for more than ~500 ms.
  if (ms - lastLedAssertMs >= LED_ASSERT_PERIOD_MS) {
    lastLedAssertMs = ms;
    leds.show();
  }

  delay(20);
}
