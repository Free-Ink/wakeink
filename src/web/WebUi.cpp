#include "WebUi.h"

#include <ArduinoJson.h>
#include <BatteryMonitor.h>
#include <WiFi.h>

#include <LittleFS.h>

#include "../audio/AlarmSound.h"
#include "../calendar/CalendarManager.h"
#include "../calendar/EventFilter.h"
#include "../config/AppSettings.h"
#include "../config/CalendarStore.h"
#include "../config/StateStore.h"
#include "../config/WifiStore.h"
#include "../net/WifiService.h"
#include "../ui/Screen.h"
#include "html/IndexHtml.generated.h"

WebUi& webUi() {
  static WebUi instance;
  return instance;
}

void WebUi::begin() {
  server_.reset(new WebServer(80));

  server_->on("/", HTTP_GET, [this] { handleRoot(); });
  server_->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server_->on("/api/events", HTTP_GET, [this] { handleEvents(); });
  server_->on("/api/calendars", HTTP_GET, [this] { handleGetCalendars(); });
  server_->on("/api/calendars", HTTP_POST, [this] { handlePostCalendars(); });
  server_->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server_->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  server_->on("/api/wifi", HTTP_GET, [this] { handleGetWifi(); });
  server_->on("/api/wifi/scan", HTTP_GET, [this] { handleWifiScan(); });
  server_->on("/api/wifi/add", HTTP_POST, [this] { handleWifiAdd(); });
  server_->on("/api/wifi/remove", HTTP_POST, [this] { handleWifiRemove(); });
  server_->on("/api/alarm-sound", HTTP_POST, [this] { handleAlarmSoundPost(); },
              [this] { handleAlarmSoundUpload(); });
  server_->on("/api/alarm-sound/reset", HTTP_POST, [this] { handleAlarmSoundReset(); });
  server_->on("/api/sync", HTTP_POST, [this] { handleSync(); });
  server_->on("/api/pause", HTTP_POST, [this] { handlePause(); });
  server_->on("/api/skip", HTTP_POST, [this] { handleSkip(); });
  server_->on("/api/unskip", HTTP_POST, [this] { handleUnskip(); });
  server_->on("/api/dismiss", HTTP_POST, [this] { handleAction(dismissRequested_); });
  server_->on("/api/test-alarm", HTTP_POST, [this] { handleAction(testAlarmRequested_); });
  server_->on("/api/reboot", HTTP_POST, [this] { handleReboot(); });
  server_->onNotFound([this] { handleNotFound(); });

  server_->begin();
}

void WebUi::loop() {
  if (server_) server_->handleClient();
}

void WebUi::handleRoot() {
  server_->sendHeader("Content-Encoding", "gzip");
  server_->send_P(200, "text/html", IndexHtml, IndexHtmlCompressedSize);
}

void WebUi::handleNotFound() {
  if (wifiService().mode() == WifiService::AP_PORTAL) {
    // Captive portal: bounce every unknown URL to the dashboard.
    server_->sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server_->send(302, "text/plain", "");
    return;
  }
  server_->send(404, "text/plain", "Not found");
}

void WebUi::sendJson(const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server_->send(200, "application/json", out);
}

bool WebUi::parseBody(JsonDocument& doc) {
  if (!server_->hasArg("plain")) {
    server_->send(400, "text/plain", "Missing body");
    return false;
  }
  if (deserializeJson(doc, server_->arg("plain")) != DeserializationError::Ok) {
    server_->send(400, "text/plain", "Invalid JSON");
    return false;
  }
  return true;
}

// --- status / events ----------------------------------------------------------

void WebUi::handleStatus() {
  JsonDocument doc;
  const time_t now = time(nullptr);
  const SyncStatus sync = calendarManager().status();
  const auto events = calendarManager().snapshot();

  doc["version"] = WAKEINK_VERSION;
  doc["hostname"] = settings().hostname;
  doc["ip"] = wifiService().ip();
  doc["ssid"] = wifiService().currentSsid();
  doc["rssi"] = WiFi.RSSI();
  doc["ap_mode"] = wifiService().mode() == WifiService::AP_PORTAL;
  doc["wifi_connected"] = wifiService().mode() == WifiService::CONNECTED;
  doc["time"] = (long long)now;
  doc["time_valid"] = now > 1600000000;
  doc["timezone_name"] = settings().timezoneName;
  doc["heap_free"] = (long)ESP.getFreeHeap();
  doc["heap_min"] = (long)esp_get_minimum_free_heap_size();
  doc["psram_free"] = (long)ESP.getFreePsram();
  doc["psram_size"] = (long)ESP.getPsramSize();

  static BatteryMonitor battery;
  const BatteryMonitor::Status batteryStatus = battery.readStatus();
  doc["battery_supported"] = batteryStatus.supported;
  if (batteryStatus.percentageKnown) {
    doc["battery_percent"] = batteryStatus.percentage;
  } else {
    doc["battery_percent"] = nullptr;
  }
  if (batteryStatus.millivoltsKnown) {
    doc["battery_mv"] = batteryStatus.millivolts;
  } else {
    doc["battery_mv"] = nullptr;
  }
  if (batteryStatus.chargingKnown) {
    doc["battery_charging"] = batteryStatus.charging;
  } else {
    doc["battery_charging"] = nullptr;
  }
  if (batteryStatus.externalPowerKnown) {
    doc["battery_external_power"] = batteryStatus.externalPower;
  } else {
    doc["battery_external_power"] = nullptr;
  }
  // Raw PM1 rail telemetry (PaperColor only, -1 when unread) — diagnostics for
  // external-power detection; visible at /api/status, not rendered in the UI.
  doc["pm1_vin_mv"] = batteryStatus.pm1VinMv;
  doc["pm1_vinout_mv"] = batteryStatus.pm1VinOutMv;
  doc["pm1_power_source"] = batteryStatus.pm1PowerSource;

  doc["last_sync"] = (long long)sync.lastSyncTime;
  doc["last_sync_ok"] = sync.lastSyncOk;
  doc["last_error"] = sync.lastError;
  doc["syncing"] = sync.syncing;
  doc["calendars_ok"] = sync.calendarsOk;
  doc["calendars_total"] = sync.calendarsTotal;

  doc["audio_present"] = alarmsound::audio().present();
  doc["custom_alarm_sound"] = alarmsound::hasCustom();
  doc["paused"] = stateStore().isPaused(now);
  doc["pause_until"] = (long long)stateStore().pauseUntil;
  doc["pause_minutes"] = stateStore().pauseChoiceMinutes;
  doc["event_count"] = events.size();

  if (!events.empty()) {
    JsonObject next = doc["next_event"].to<JsonObject>();
    next["title"] = events.front().title;
    next["organizer"] = events.front().organizer;
    next["start"] = (long long)events.front().start;
    next["end"] = (long long)events.front().end;
  }
  sendJson(doc);
}

void WebUi::handleEvents() {
  JsonDocument doc;
  JsonArray arr = doc["events"].to<JsonArray>();
  const auto events = calendarManager().snapshot();
  const AppSettings& s = settings();
  for (const Event& ev : events) {
    JsonObject obj = arr.add<JsonObject>();
    obj["key"] = ev.skipKey();
    obj["title"] = ev.title;
    obj["organizer"] = ev.organizer;
    obj["location"] = ev.location;
    obj["start"] = (long long)ev.start;
    obj["end"] = (long long)ev.end;
    obj["cal"] = ev.calIndex;
    obj["has_link"] = ev.hasLink;
    obj["lead_minutes"] = eventfilter::leadTimeMinutes(ev, s);
  }
  JsonArray skips = doc["skipped"].to<JsonArray>();
  for (const auto& key : stateStore().skipped()) skips.add(key);
  sendJson(doc);
}

// --- calendars ------------------------------------------------------------------

void WebUi::handleGetCalendars() {
  JsonDocument doc;
  JsonArray arr = doc["calendars"].to<JsonArray>();
  for (const auto& cal : calendarStore().list()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = cal.name;
    obj["url"] = cal.url;
    obj["enabled"] = cal.enabled;
  }
  sendJson(doc);
}

void WebUi::handlePostCalendars() {
  JsonDocument doc;
  if (!parseBody(doc)) return;

  std::vector<CalendarSource> list;
  for (JsonObjectConst obj : doc["calendars"].as<JsonArrayConst>()) {
    if (list.size() >= CalendarStore::MAX_CALENDARS) break;
    CalendarSource cal;
    cal.name = obj["name"] | "";
    cal.url = obj["url"] | "";
    cal.enabled = obj["enabled"] | true;
    cal.url.trim();
    // Google hands out webcal:// links in some flows; normalize to https.
    if (cal.url.startsWith("webcal://")) cal.url = "https://" + cal.url.substring(9);
    if (cal.url.isEmpty()) continue;
    if (!cal.url.startsWith("http://") && !cal.url.startsWith("https://")) continue;
    if (cal.name.isEmpty()) cal.name = "Calendar " + String(list.size() + 1);
    list.push_back(cal);
  }

  calendarManager().lockConfig();
  calendarStore().list() = list;
  const bool ok = calendarStore().save();
  calendarManager().unlockConfig();

  if (!ok) {
    server_->send(500, "text/plain", "Save failed");
    return;
  }
  calendarManager().requestSync();
  server_->send(200, "text/plain", "OK");
}

// --- settings -------------------------------------------------------------------

void WebUi::handleGetSettings() {
  JsonDocument doc;
  settings().toJson(doc);
  sendJson(doc);
}

void WebUi::handlePostSettings() {
  JsonDocument doc;
  if (!parseBody(doc)) return;

  calendarManager().lockConfig();
  const String oldTz = settings().timezone;
  const String oldHostname = settings().hostname;
  settings().fromJson(doc);
  const bool ok = settings().save();
  const bool tzChanged = settings().timezone != oldTz;
  const bool hostnameChanged = settings().hostname != oldHostname;
  calendarManager().unlockConfig();

  if (!ok) {
    server_->send(500, "text/plain", "Save failed");
    return;
  }
  if (tzChanged) wifiService().applyTimeConfig();
  if (hostnameChanged) wifiService().applyHostname();  // new .local URL, no reboot needed
  // Tightened filters apply to the display immediately; the sync that follows
  // brings back anything a loosened filter re-admits.
  calendarManager().refilterNow();
  calendarManager().requestSync();
  server_->send(200, "text/plain", "OK");
}

// --- wifi -----------------------------------------------------------------------

void WebUi::handleGetWifi() {
  JsonDocument doc;
  doc["connected_ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  doc["ap_mode"] = wifiService().mode() == WifiService::AP_PORTAL;
  doc["last_failed_ssid"] = wifiService().lastFailedSsid();
  doc["last_fail_reason"] = wifiService().lastFailReason();
  JsonArray arr = doc["saved"].to<JsonArray>();
  for (const auto& cred : wifiStore().list()) arr.add(cred.ssid);
  sendJson(doc);
}

// Async scan: a blocking scanNetworks() inside a handler stalls (and in AP
// mode disrupts) the very connection the browser is on. First call kicks off
// a background scan and returns {scanning:true}; the page polls until the
// results land.
void WebUi::handleWifiScan() {
  JsonDocument doc;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
  } else if (n < 0) {  // WIFI_SCAN_FAILED = idle/no results yet
    WiFi.scanNetworks(/*async=*/true);
    doc["scanning"] = true;
  } else {
    doc["scanning"] = false;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; ++i) {
      JsonObject obj = arr.add<JsonObject>();
      obj["ssid"] = WiFi.SSID(i);
      obj["rssi"] = WiFi.RSSI(i);
      obj["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
  }
  sendJson(doc);
}

void WebUi::handleWifiAdd() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const String ssid = doc["ssid"] | "";
  const String password = doc["password"] | "";
  if (ssid.isEmpty()) {
    server_->send(400, "text/plain", "Missing ssid");
    return;
  }
  if (!wifiStore().find(ssid) && wifiStore().list().size() >= WifiStore::MAX_NETWORKS) {
    server_->send(400, "text/plain", "Too many saved networks (max 8)");
    return;
  }
  if (!wifiStore().add(ssid, password)) {
    server_->send(500, "text/plain", "Saving to flash failed");
    return;
  }
  server_->send(200, "text/plain", "OK");
  // From the setup portal a reboot is the clean path to STA mode.
  if (wifiService().mode() == WifiService::AP_PORTAL) {
    delay(500);
    ESP.restart();
  }
}

void WebUi::handleWifiRemove() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const String ssid = doc["ssid"] | "";
  wifiStore().remove(ssid);
  server_->send(200, "text/plain", "OK");
}

// --- alarm sound upload -----------------------------------------------------------

void WebUi::handleAlarmSoundUpload() {
  constexpr size_t MAX_UPLOAD = 4 * 1024 * 1024;
  static const char* TMP_PATH = "/alarm.wav.tmp";
  HTTPUpload& up = server_->upload();

  switch (up.status) {
    case UPLOAD_FILE_START:
      uploadError_ = "";
      uploadSize_ = 0;
      // Don't clobber a sound that's ringing right now.
      alarmsound::stop();
      LittleFS.remove(TMP_PATH);
      uploadFile_ = LittleFS.open(TMP_PATH, "w");
      if (!uploadFile_) uploadError_ = "cannot write to flash";
      break;
    case UPLOAD_FILE_WRITE:
      if (uploadFile_ && uploadError_.isEmpty()) {
        uploadSize_ += up.currentSize;
        if (uploadSize_ > MAX_UPLOAD) {
          uploadError_ = "file too large (max 4 MB)";
          uploadFile_.close();
          LittleFS.remove(TMP_PATH);
        } else if (uploadFile_.write(up.buf, up.currentSize) != up.currentSize) {
          uploadError_ = "flash write failed (full?)";
          uploadFile_.close();
          LittleFS.remove(TMP_PATH);
        }
      }
      break;
    case UPLOAD_FILE_END:
      if (uploadFile_) uploadFile_.close();
      break;
    case UPLOAD_FILE_ABORTED:
      if (uploadFile_) uploadFile_.close();
      LittleFS.remove(TMP_PATH);
      uploadError_ = "upload aborted";
      break;
  }
}

void WebUi::handleAlarmSoundPost() {
  static const char* TMP_PATH = "/alarm.wav.tmp";
  if (!uploadError_.isEmpty()) {
    server_->send(400, "text/plain", uploadError_);
    return;
  }
  String why;
  if (!alarmsound::validateWavFile(TMP_PATH, why)) {
    LittleFS.remove(TMP_PATH);
    server_->send(400, "text/plain", "Rejected: " + why);
    return;
  }
  LittleFS.remove(alarmsound::CUSTOM_PATH);
  if (!LittleFS.rename(TMP_PATH, alarmsound::CUSTOM_PATH)) {
    server_->send(500, "text/plain", "Saving failed");
    return;
  }
  server_->send(200, "text/plain", "OK");
}

void WebUi::handleAlarmSoundReset() {
  alarmsound::stop();
  LittleFS.remove(alarmsound::CUSTOM_PATH);
  server_->send(200, "text/plain", "OK");
}

// --- actions ----------------------------------------------------------------------

void WebUi::handleSync() {
  calendarManager().requestSync();
  server_->send(200, "text/plain", "OK");
}

void WebUi::handlePause() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const long minutes = doc["minutes"] | 0L;

  calendarManager().lockConfig();
  if (minutes < 0) {
    // Indefinite pause, like the app: year 2999.
    stateStore().pauseUntil = 32472144000LL;
  } else if (minutes == 0) {
    stateStore().pauseUntil = 0;
  } else {
    stateStore().pauseUntil = time(nullptr) + minutes * 60;
  }
  stateStore().pauseChoiceMinutes = (int)minutes;
  stateStore().save();
  calendarManager().unlockConfig();

  displayRefreshRequested_ = true;
  server_->send(200, "text/plain", "OK");
}

void WebUi::handleSkip() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const String key = doc["key"] | "";
  calendarManager().lockConfig();
  stateStore().skip(key);
  calendarManager().unlockConfig();
  calendarManager().refilterNow();  // drops it from the display right away
  server_->send(200, "text/plain", "OK");
}

void WebUi::handleUnskip() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  const String key = doc["key"] | "";
  calendarManager().lockConfig();
  stateStore().unskip(key);
  calendarManager().unlockConfig();
  calendarManager().requestSync();
  server_->send(200, "text/plain", "OK");
}

void WebUi::handleAction(volatile bool& flag) {
  flag = true;
  server_->send(200, "text/plain", "OK");
}

void WebUi::handleReboot() {
  server_->send(200, "text/plain", "Rebooting");
  delay(300);
  ESP.restart();
}

// --- one-shot flags -----------------------------------------------------------------

bool WebUi::consumeDismiss() {
  if (!dismissRequested_) return false;
  dismissRequested_ = false;
  return true;
}

bool WebUi::consumeTestAlarm() {
  if (!testAlarmRequested_) return false;
  testAlarmRequested_ = false;
  return true;
}

bool WebUi::consumeDisplayRefresh() {
  if (!displayRefreshRequested_) return false;
  displayRefreshRequested_ = false;
  return true;
}
