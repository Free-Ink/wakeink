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
Screen* screen = nullptr;
SettingsScreen* settingsScreen = nullptr;

enum class UiState { BOOTING, PORTAL, CONNECTING, IDLE, ALARM, POPUP, SETTINGS };
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

// Touch→logical mapping is SDK-owned (ui::snapshotFrom + touchToLogical, keyed
// off the device orientation). These flips compensate only for mirrored panel
// mounting — if taps land mirrored on the bench, flip the matching constant.
constexpr bool TOUCH_FLIP_X = false;
constexpr bool TOUCH_FLIP_Y = false;

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
}

void stopAlarm(bool fired) {
  alarmsound::stop();
  if (fired) calendarManager().markFired(ringingEvent);
  enterIdle();
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

  // WiFi state transitions drive the boot screens.
  if (wifiService().modeChanged()) {
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

      // Frontlight strobe while ringing.
      if (settings().alarmFlashFrontlight && ms - lastFlashToggleMs >= FLASH_PERIOD_MS) {
        lastFlashToggleMs = ms;
        flashOn = !flashOn;
        frontlight.setBrightness(flashOn ? 100 : 10);
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

      // No touch on this board: settings/skip live on the web dashboard, so
      // the physical keys drive everything. Holding up+down together toggles
      // dark mode; on a single key release, confirm (GPIO1 on the PaperColor)
      // forces a complete-waveform deep clean (~15 s on the M5 — clears
      // accumulated ghosting) and any other key forces a sync + redraw (the
      // same as tapping empty space below). Single-key actions fire on
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

        const bool anyKeyReleased = input.wasReleased(InputManager::BTN_UP) ||
                                    input.wasReleased(InputManager::BTN_DOWN) ||
                                    input.wasReleased(InputManager::BTN_CONFIRM) ||
                                    input.wasReleased(InputManager::BTN_BACK);
        if (anyKeyReleased && ms - lastChordMs > CHORD_COOLDOWN_MS) {
          if (snap.confirm) display.requestCompleteWaveformNextRefresh();
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
        drawIdleScreen(fastRefreshCount % FULL_REFRESH_EVERY == 0);
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

    case UiState::PORTAL:
    case UiState::CONNECTING:
      if (webUi().consumeTestAlarm() || webUi().consumeDismiss()) {
        // ignore while not running
      }
      break;

    case UiState::BOOTING:
      break;
  }

  delay(20);
}
