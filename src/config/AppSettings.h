#pragma once

// All user-facing settings, persisted as /settings.json on LittleFS.
//
// The keys and defaults mirror the In Your Space Android app's preferences
// (alarm lead times, ignore keywords, work hours, link filtering, RSVP
// gating), plus device-specific additions (timezone, polling, display).

#include <Arduino.h>
#include <ArduinoJson.h>

#include <vector>

struct AppSettings {
  // --- device / sync ---------------------------------------------------------
  String hostname = "wakeink";          // mDNS name -> http://wakeink.local
  String timezone = "PST8PDT,M3.2.0,M11.1.0";  // POSIX TZ string
  String timezoneName = "America/Los_Angeles"; // display label for the web UI
  int pollIntervalMinutes = 5;          // ICS poll cadence (Android sync = 5 min)
  int lookaheadDays = 2;                // event query window (default: today + tomorrow)
  bool use24HourTime = false;
  bool darkMode = false;                // invert the on-device UI (white-on-black);
                                        // toggled by the up+down key chord on
                                        // buttons-only boards, or via the web API
  std::vector<String> myEmails;         // your accounts, matched against ICS ATTENDEE
                                        // lines to find your PARTSTAT (any-of)

  // --- alarms ----------------------------------------------------------------
  int alarmLeadTimeMinutes = 15;        // flutter.alarm_lead_time_minutes
  bool earlyMeetingAlertEnabled = false;
  int earlyMeetingBeforeHour = 10;
  int earlyMeetingBeforeMinute = 0;
  int earlyMeetingLeadTimeMinutes = 30;
  bool alarmFlashFrontlight = true;     // pulse the frontlight while ringing
  bool alarmSoundEnabled = true;        // loop the alarm WAV while ringing
  int alarmVolume = 70;                 // codec output volume, 0-100
  int alarmMaxMinutes = 5;              // auto-stop a ringing alarm after this
  int frontlightBrightness = 0;         // steady frontlight level outside alarms, 0-100
  String alarmLedColorA = "#ff0000";    // alarm LED strobe pair ("#rrggbb"), used on
  String alarmLedColorB = "#0000ff";    // boards with RGB LEDs (M5 PaperColor)
  int alarmLedBrightness = 60;          // LED strobe brightness, 0-100

  // --- filtering -------------------------------------------------------------
  std::vector<String> ignoreKeywords;        // flutter.ignore_keywords
  std::vector<String> alwaysTriggerKeywords; // flutter.always_trigger_keywords
  bool onlyAlertWithLinks = false;           // flutter.only_alert_with_links
  bool alertWithAnyLink = false;             // flutter.alert_with_any_link
  std::vector<String> ignoreLinkDomains;     // flutter.ignore_link_domains
  bool showMaybeEvents = true;               // flutter.show_maybe_events
  bool alarmOnUnacceptedEvents = false;      // flutter.alarm_on_unaccepted_events

  // --- work hours ------------------------------------------------------------
  bool workHoursEnabled = false;
  int workHoursStartHour = 9;
  int workHoursStartMinute = 0;
  int workHoursEndHour = 17;
  int workHoursEndMinute = 0;
  std::vector<int> workDays = {1, 2, 3, 4, 5};  // 1=Mon .. 7=Sun

  bool load();
  bool save() const;

  // (De)serialize to/from a JSON object — shared by file IO and the web API.
  void toJson(JsonDocument& doc) const;
  void fromJson(const JsonDocument& doc);
};

AppSettings& settings();
