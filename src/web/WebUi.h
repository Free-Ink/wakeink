#pragma once

// The local web dashboard: serves the gzipped single-page app from flash and
// exposes JSON APIs for status, events, calendars (paste ICS links), settings,
// WiFi management and actions (sync / pause / skip / dismiss / test alarm).
//
// Patterned after crosspoint-reader's CrossPointWebServer: stock WebServer,
// lambda routes, Content-Encoding: gzip assets, ArduinoJson bodies.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <WebServer.h>

#include <memory>

class WebUi {
 public:
  void begin();
  void loop();

  // One-shot action flags consumed by the main loop.
  bool consumeDismiss();
  bool consumeTestAlarm();
  bool consumeDisplayRefresh();

 private:
  void handleRoot();
  void handleStatus();
  void handleEvents();
  void handleGetCalendars();
  void handlePostCalendars();
  void handleGetSettings();
  void handlePostSettings();
  void handleGetWifi();
  void handleWifiScan();
  void handleWifiAdd();
  void handleWifiRemove();
  void handleAlarmSoundPost();
  void handleAlarmSoundUpload();
  void handleAlarmSoundReset();
  void handleSync();
  void handlePause();
  void handleSkip();
  void handleUnskip();
  void handleAction(volatile bool& flag);
  void handleReboot();
  void handleNotFound();

  void sendJson(const JsonDocument& doc);
  bool parseBody(JsonDocument& doc);

  std::unique_ptr<WebServer> server_;
  volatile bool dismissRequested_ = false;
  volatile bool testAlarmRequested_ = false;
  volatile bool displayRefreshRequested_ = false;

  // Alarm-sound upload state (multipart streaming to a temp file).
  fs::File uploadFile_;
  size_t uploadSize_ = 0;
  String uploadError_;
};

WebUi& webUi();
