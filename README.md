# WakeInk

A calendar-alarm clock for FreeInk e-ink devices — the **Murphy M3** (416×240,
touch, frontlight, audio) and the **M5Stack PaperColor** (600×400, buttons,
built-in speaker). WakeInk subscribes to Google Calendar **ICS links** (the
"secret address in iCal format") pasted into a **web dashboard hosted on the
device itself** — no Google account, OAuth, or app required. It keeps a
persistent WiFi connection, polls the feeds, renders the next event widget-style
on the panel, and rings a full-screen alarm with a looping sound (plus a
frontlight strobe on the Murphy) before your meetings.

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/<you>/wakeink.git
cd wakeink
pio run -e murphy -t upload        # Murphy M3: build + flash over USB
pio run -e m5papercolor -t upload  # M5Stack PaperColor
pio device monitor                 # serial logs @ 115200
```

Already cloned without `--recurse-submodules`? Run:

```bash
git submodule update --init
```

The FreeInk SDK is pinned as a git submodule at `vendor/freeink-sdk`, so the
project compiles as-is — no sibling checkout needed. Requires
[PlatformIO](https://platformio.org/).

### First-time device setup

1. On first boot (no saved WiFi) the device starts an open access point
   **`WakeInk-Setup`** and shows setup instructions on screen with a QR code.
2. Join that network from your phone/laptop (scan the QR or pick it manually).
   The captive portal opens the dashboard automatically; if not, browse to
   **http://192.168.4.1**.
3. In the **WiFi** tab, add your home network. The device reboots and joins it.
4. From then on the dashboard lives at **http://wakeink.local** (the IP is shown
   on screen too). Open the **Calendars** tab and paste your Google Calendar ICS
   links — get them from Google Calendar → *Settings → Settings for my calendars
   → Integrate calendar → Secret address in iCal format*.

---

## Features

- **Local web dashboard** (`http://wakeink.local`): live status, upcoming events,
  calendar management, every filter setting, WiFi management, and alarm-sound
  upload. The single-page app is gzipped and embedded in flash.
- **Filtering**, ported 1:1 from the Android app:
  - ignore keywords (title match)
  - work-hours window + always-trigger keyword bypass
  - meeting-link gating (known platforms — Zoom/Meet/Teams/Webex/… — or any URL
    minus ignored domains)
  - RSVP handling: declined dropped, "maybe" optional, unaccepted optional — your
    status is detected from `ATTENDEE` lines matching your email(s)
  - all-day and cancelled events excluded
  - per-instance skips and pause (timed or indefinite)
- **Alarms**: configurable lead time (with a separate lead time for early
  meetings), full-screen alert, frontlight strobe, and a **looping WAV** through
  the Murphy's ES8388 codec. Tap to dismiss; auto-stops after a timeout. Ships a
  default sound; upload your own from **Settings → Alarms** (16-bit PCM WAV,
  mono/stereo, 8–48 kHz, ≤4 MB).
- **Day-aware display**: shows the next event, or "No more events today" with
  tomorrow's first event labeled. Tap an event for a skip dialog.
- **On-device settings** (cog at bottom-right): tabbed groups — Alarm (lead
  times, early-meeting alert, auto-stop, test alarm), Sound (alarm sound,
  volume, ring flash, steady frontlight), Filter (maybe/unaccepted/link rules,
  work hours + day chips), Clock (timezone picker, 24-hour, look-ahead, poll
  interval), System (pause alarms, sync now, reboot, network/sync/version
  info). Everything keyboard-free is adjustable on the panel; free-text
  settings (keywords, calendars, WiFi) stay on the web dashboard.
- **ICS engine**: streaming parser with line unfolding, **gzip transfer**
  (an 8.7 MB feed downloads as ~1.4 MB and inflates on-device), full RRULE
  expansion (DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL, BYDAY incl.
  monthly ordinals, BYMONTHDAY), EXDATE, RECURRENCE-ID overrides, and proper
  timezone resolution from the feed's own VTIMEZONE blocks (offset + DST rules).

---

## Architecture

The Arduino `loop()` (core 1) owns the display, web server, input, and alarm
state machine. Calendar sync runs in a dedicated FreeRTOS task (core 0):
fetch → gzip-inflate → parse → filter → sort → swap under a mutex, so network
I/O never blocks the UI.

| Piece | Path | Notes |
|---|---|---|
| Entry / alarm state machine | `src/main.cpp` | minute redraws, alarm ring/dismiss |
| Sync task | `src/calendar/CalendarManager.*` | fetch → parse → filter → publish |
| HTTPS + gzip fetch | `src/net/IcsFetch.*` | `esp_http_client` + CA bundle, streaming inflate (uzlib) |
| ICS parser | `src/calendar/IcsParser.*` | streaming, RRULE/EXDATE/VTIMEZONE, window-bounded |
| Filters | `src/calendar/EventFilter.*` | port of the app's gate chain |
| WiFi | `src/net/WifiService.*` | saved-network cycling, AP setup portal, SNTP + mDNS |
| Web UI | `src/web/WebUi.*` + `src/web/html/Index.html` | `WebServer`, JSON APIs, gzipped assets |
| Display | `src/ui/Screen.*` | screens built from FreeInkUI components (statusBar/header/drawText) |
| Settings UI | `src/ui/SettingsScreen.*` | tabbed on-device settings (FreeInkUI buttons/list/optionDialog) |
| UI bridge | `src/ui/GfxTextDrawTarget.*` | implements FreeInkUI's `DrawTarget` over `lib/GfxText` (Noto Sans glyph tables) |
| Config / state | `src/config/*` | JSON files on LittleFS |
| Audio glue | `src/audio/AlarmSound.*` | default + uploaded WAV, drives SDK `AudioManager` |

### Storage

JSON files on LittleFS (internal flash): `settings.json`, `calendars.json`,
`wifi.json` (passwords MAC-XOR obfuscated), `state.json`, plus an optional
uploaded `alarm.wav`. There is no SD card dependency.

### Timezone model

Set your timezone in Settings (a "Detect" button reads your browser's). ICS times
ending in `Z` are exact UTC; times with a `TZID` are resolved against the feed's
own VTIMEZONE definition (offset + yearly DST transition), falling back to the
device timezone only for an unknown zone.

---

## Build details

`platformio.ini` defines one environment per device: `murphy` (ESP32-S3, UC8253
panel) and `m5papercolor` (ESP32-S3, ED2208 Spectra-6 color panel). Each env
selects its device with a `-DFREEINK_DEVICE_*` flag; the UI's compile-time
geometry lives in `src/ui/ScreenGeometry.h` and is verified against the panel
driver at boot.

- **M5Stack PaperColor** has no touch or frontlight: any button press dismisses
  an alarm, a button press on the clock forces a sync, and all configuration
  happens on the web dashboard (the on-device settings cog is hidden). Alarm
  sound plays through the built-in 1W speaker (ES8311 codec + AW8737A amp,
  driven by the SDK's AudioManager). Refreshes use the FreeInk SDK's
  interrupted-waveform driver (~340 ms monochrome instead of the panel's native
  ~15 s six-color refresh).
- **PSRAM is intentionally disabled on the Murphy** (plain `esp32-s3-devkitc-1`
  board + `dio_qspi`). The Murphy module has octal PSRAM, but it is not needed —
  transfers are gzipped and parsing is memory-bounded — and the obvious
  `…-n16r8` board profile silently force-enables it. (That was a marginal-chip
  quirk of the Murphy unit; the PaperColor env keeps PSRAM on.)
- Pre-build scripts embed the dashboard (`scripts/build_html.py`) and the default
  alarm sound (`scripts/embed_assets.py`); a post-build script archives each ELF
  by SHA (`scripts/archive_elf.py`) for crash-backtrace decoding.

### Fonts

`lib/GfxText/fonts/*.h` are 1-bit glyph tables generated from **Noto Sans**
(SIL Open Font License 1.1 — see `lib/GfxText/fonts/OFL.txt`) by
`scripts/gen_fonts.py`:

```bash
python3 scripts/gen_fonts.py NotoSans-Regular.ttf NotoSans-Bold.ttf
```

### Debugging crashes

Every build archives its ELF to `.elf-archive/<sha9>.elf`. A panic prints
`ELF file SHA256: <sha>`; decode it with:

```bash
xtensa-esp32s3-elf-addr2line -pfiaC -e .elf-archive/<sha9>.elf <addresses…>
```

The boot line `[wakeink] v… reset_reason=N …` reports why the device last reset
(`6` = task watchdog, i.e. a long unyielded loop; `4` = panic/fault).

---

## License

Firmware: see `LICENSE`. Bundled third-party code retains its own license —
Noto Sans (`lib/GfxText/fonts/OFL.txt`) and uzlib (`lib/uzlib`).
