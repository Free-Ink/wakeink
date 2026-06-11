#pragma once

// WiFi lifecycle: tries saved credentials in order (last-connected first); if
// none work (or none exist) it opens the "WakeInk-Setup" AP with a captive
// portal so the dashboard is reachable at http://192.168.4.1 for first-time
// setup. Once connected it brings up SNTP (with the configured POSIX TZ) and
// mDNS (http://<hostname>.local).

#include <Arduino.h>
#include <DNSServer.h>

#include <vector>

class WifiService {
 public:
  enum Mode { IDLE, CONNECTING, CONNECTED, RECONNECTING, AP_PORTAL };

  static constexpr const char* AP_SSID = "WakeInk-Setup";

  void begin();
  void loop();

  Mode mode() const { return mode_; }
  String ip() const;
  String currentSsid() const;
  // One-shot: true on the loop() pass where the connection came up.
  bool justConnected();
  // One-shot: true when the mode changed (drives screen redraws).
  bool modeChanged();

  // Re-apply TZ + restart SNTP (used after timezone settings change).
  void applyTimeConfig();

  // Re-register mDNS under the current settings().hostname (used after a
  // rename from the web UI, so the new http://<name>.local works without a
  // reboot). No-op unless connected.
  void applyHostname();

  // Hibernate: tear WiFi down entirely (radio off, AP/mDNS/DNS stopped) —
  // which inherently stops syncing, since the sync task gates on
  // WL_CONNECTED. resume() re-runs the normal connect flow from the top.
  void suspend();
  void resume() { begin(); }

  // Why the last STA attempt failed (empty if none) — shown on the setup
  // screen and in the web UI so a bad password isn't a silent dead end.
  String lastFailedSsid() const { return lastFailedSsid_; }
  String lastFailReason() const { return lastFailReason_; }

 private:
  void startPortal();
  void beginAttempt(size_t index);
  void onConnected();
  void recordFailure();

  DNSServer dns_;
  Mode mode_ = IDLE;
  Mode lastReportedMode_ = IDLE;
  size_t credIndex_ = 0;
  uint32_t attemptStartMs_ = 0;
  uint32_t lastReconnectMs_ = 0;
  bool everConnected_ = false;
  bool justConnected_ = false;
  std::vector<String> attemptOrder_;
  String lastFailedSsid_;
  String lastFailReason_;
};

WifiService& wifiService();
