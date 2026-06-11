#include "AppSettings.h"

#include "FsJson.h"

static const char* SETTINGS_PATH = "/settings.json";

AppSettings& settings() {
  static AppSettings instance;
  return instance;
}

// Validate a "#rrggbb" hex color; anything malformed falls back to `fallback`
// so a bad web payload can't leave the LEDs unparsable.
static String sanitizeHexColor(String v, const char* fallback) {
  v.trim();
  v.toLowerCase();
  if (v.length() == 7 && v[0] == '#') {
    bool ok = true;
    for (int i = 1; i < 7; ++i) {
      if (!isxdigit((unsigned char)v[i])) {
        ok = false;
        break;
      }
    }
    if (ok) return v;
  }
  return String(fallback);
}

static void stringsToJson(JsonDocument& doc, const char* key, const std::vector<String>& list) {
  JsonArray arr = doc[key].to<JsonArray>();
  for (const auto& s : list) arr.add(s);
}

static void stringsFromJson(const JsonDocument& doc, const char* key, std::vector<String>& list) {
  if (!doc[key].is<JsonArrayConst>()) return;
  list.clear();
  for (JsonVariantConst v : doc[key].as<JsonArrayConst>()) {
    const char* s = v.as<const char*>();
    if (s && *s) list.push_back(String(s));
  }
}

void AppSettings::toJson(JsonDocument& doc) const {
  doc["hostname"] = hostname;
  doc["timezone"] = timezone;
  doc["timezone_name"] = timezoneName;
  doc["poll_interval_minutes"] = pollIntervalMinutes;
  doc["wifi_sleep_between_syncs"] = wifiSleepBetweenSyncs;
  doc["lookahead_days"] = lookaheadDays;
  doc["use_24_hour_time"] = use24HourTime;
  doc["dark_mode"] = darkMode;
  stringsToJson(doc, "my_emails", myEmails);

  doc["alarm_lead_time_minutes"] = alarmLeadTimeMinutes;
  doc["early_meeting_alert_enabled"] = earlyMeetingAlertEnabled;
  doc["early_meeting_before_hour"] = earlyMeetingBeforeHour;
  doc["early_meeting_before_minute"] = earlyMeetingBeforeMinute;
  doc["early_meeting_lead_time_minutes"] = earlyMeetingLeadTimeMinutes;
  doc["alarm_flash_frontlight"] = alarmFlashFrontlight;
  doc["alarm_sound_enabled"] = alarmSoundEnabled;
  doc["alarm_volume"] = alarmVolume;
  doc["alarm_max_minutes"] = alarmMaxMinutes;
  doc["frontlight_brightness"] = frontlightBrightness;
  doc["alarm_led_color_a"] = alarmLedColorA;
  doc["alarm_led_color_b"] = alarmLedColorB;
  doc["alarm_led_brightness"] = alarmLedBrightness;

  stringsToJson(doc, "ignore_keywords", ignoreKeywords);
  stringsToJson(doc, "always_trigger_keywords", alwaysTriggerKeywords);
  doc["only_alert_with_links"] = onlyAlertWithLinks;
  doc["alert_with_any_link"] = alertWithAnyLink;
  stringsToJson(doc, "ignore_link_domains", ignoreLinkDomains);
  doc["show_maybe_events"] = showMaybeEvents;
  doc["alarm_on_unaccepted_events"] = alarmOnUnacceptedEvents;

  doc["work_hours_enabled"] = workHoursEnabled;
  doc["work_hours_start_hour"] = workHoursStartHour;
  doc["work_hours_start_minute"] = workHoursStartMinute;
  doc["work_hours_end_hour"] = workHoursEndHour;
  doc["work_hours_end_minute"] = workHoursEndMinute;
  JsonArray days = doc["work_days"].to<JsonArray>();
  for (int d : workDays) days.add(d);
}

void AppSettings::fromJson(const JsonDocument& doc) {
  hostname = doc["hostname"] | hostname;
  // Sanitize to a valid mDNS label: lowercase alphanumerics and dashes, no
  // leading/trailing/double dash, max 32 chars; an empty result falls back to
  // the default so a device can't rename itself unreachable.
  {
    String clean;
    hostname.toLowerCase();
    for (size_t i = 0; i < hostname.length() && clean.length() < 32; ++i) {
      const char ch = hostname[i];
      if (isalnum((unsigned char)ch)) {
        clean += ch;
      } else if ((ch == '-' || ch == ' ' || ch == '_') && !clean.isEmpty() &&
                 clean[clean.length() - 1] != '-') {
        clean += '-';
      }
    }
    while (!clean.isEmpty() && clean[clean.length() - 1] == '-') {
      clean.remove(clean.length() - 1);
    }
    hostname = clean.isEmpty() ? String("wakeink") : clean;
  }
  timezone = doc["timezone"] | timezone;
  timezoneName = doc["timezone_name"] | timezoneName;
  pollIntervalMinutes = doc["poll_interval_minutes"] | pollIntervalMinutes;
  if (pollIntervalMinutes < 1) pollIntervalMinutes = 1;
  wifiSleepBetweenSyncs = doc["wifi_sleep_between_syncs"] | wifiSleepBetweenSyncs;
  lookaheadDays = doc["lookahead_days"] | lookaheadDays;
  if (lookaheadDays < 1) lookaheadDays = 1;
  if (lookaheadDays > 31) lookaheadDays = 31;
  use24HourTime = doc["use_24_hour_time"] | use24HourTime;
  darkMode = doc["dark_mode"] | darkMode;
  stringsFromJson(doc, "my_emails", myEmails);
  // Migration: older builds stored a single "my_email" string.
  const String legacyEmail = doc["my_email"] | "";
  if (myEmails.empty() && !legacyEmail.isEmpty()) myEmails.push_back(legacyEmail);
  for (auto& email : myEmails) {
    email.trim();
    email.toLowerCase();
  }

  alarmLeadTimeMinutes = doc["alarm_lead_time_minutes"] | alarmLeadTimeMinutes;
  if (alarmLeadTimeMinutes < 1) alarmLeadTimeMinutes = 1;
  earlyMeetingAlertEnabled = doc["early_meeting_alert_enabled"] | earlyMeetingAlertEnabled;
  earlyMeetingBeforeHour = doc["early_meeting_before_hour"] | earlyMeetingBeforeHour;
  earlyMeetingBeforeMinute = doc["early_meeting_before_minute"] | earlyMeetingBeforeMinute;
  earlyMeetingLeadTimeMinutes = doc["early_meeting_lead_time_minutes"] | earlyMeetingLeadTimeMinutes;
  alarmFlashFrontlight = doc["alarm_flash_frontlight"] | alarmFlashFrontlight;
  alarmSoundEnabled = doc["alarm_sound_enabled"] | alarmSoundEnabled;
  alarmVolume = doc["alarm_volume"] | alarmVolume;
  if (alarmVolume < 0) alarmVolume = 0;
  if (alarmVolume > 100) alarmVolume = 100;
  alarmMaxMinutes = doc["alarm_max_minutes"] | alarmMaxMinutes;
  frontlightBrightness = doc["frontlight_brightness"] | frontlightBrightness;
  if (frontlightBrightness < 0) frontlightBrightness = 0;
  if (frontlightBrightness > 100) frontlightBrightness = 100;
  alarmLedColorA = sanitizeHexColor(doc["alarm_led_color_a"] | alarmLedColorA, "#ff0000");
  alarmLedColorB = sanitizeHexColor(doc["alarm_led_color_b"] | alarmLedColorB, "#0000ff");
  alarmLedBrightness = doc["alarm_led_brightness"] | alarmLedBrightness;
  if (alarmLedBrightness < 0) alarmLedBrightness = 0;
  if (alarmLedBrightness > 100) alarmLedBrightness = 100;

  stringsFromJson(doc, "ignore_keywords", ignoreKeywords);
  stringsFromJson(doc, "always_trigger_keywords", alwaysTriggerKeywords);
  onlyAlertWithLinks = doc["only_alert_with_links"] | onlyAlertWithLinks;
  alertWithAnyLink = doc["alert_with_any_link"] | alertWithAnyLink;
  stringsFromJson(doc, "ignore_link_domains", ignoreLinkDomains);
  showMaybeEvents = doc["show_maybe_events"] | showMaybeEvents;
  alarmOnUnacceptedEvents = doc["alarm_on_unaccepted_events"] | alarmOnUnacceptedEvents;

  workHoursEnabled = doc["work_hours_enabled"] | workHoursEnabled;
  workHoursStartHour = doc["work_hours_start_hour"] | workHoursStartHour;
  workHoursStartMinute = doc["work_hours_start_minute"] | workHoursStartMinute;
  workHoursEndHour = doc["work_hours_end_hour"] | workHoursEndHour;
  workHoursEndMinute = doc["work_hours_end_minute"] | workHoursEndMinute;
  if (doc["work_days"].is<JsonArrayConst>()) {
    workDays.clear();
    for (JsonVariantConst v : doc["work_days"].as<JsonArrayConst>()) {
      const int d = v.as<int>();
      if (d >= 1 && d <= 7) workDays.push_back(d);
    }
  }
}

bool AppSettings::load() {
  JsonDocument doc;
  if (!fsjson::loadDoc(SETTINGS_PATH, doc)) return false;
  fromJson(doc);
  return true;
}

bool AppSettings::save() const {
  JsonDocument doc;
  toJson(doc);
  return fsjson::saveDoc(SETTINGS_PATH, doc);
}
