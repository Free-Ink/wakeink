#include "WifiService.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "../config/AppSettings.h"
#include "../config/WifiStore.h"

namespace {
constexpr uint32_t ATTEMPT_TIMEOUT_MS = 15000;
constexpr uint32_t RECONNECT_INTERVAL_MS = 30000;
}  // namespace

WifiService& wifiService() {
  static WifiService instance;
  return instance;
}

void WifiService::begin() {
  WiFi.persistent(false);

  auto& store = wifiStore();
  if (store.list().empty()) {
    startPortal();
    return;
  }

  // Last-connected network first, then the rest in saved order.
  attemptOrder_.clear();
  if (!store.lastConnectedSsid.isEmpty() && store.find(store.lastConnectedSsid)) {
    attemptOrder_.push_back(store.lastConnectedSsid);
  }
  for (const auto& cred : store.list()) {
    if (cred.ssid != store.lastConnectedSsid) attemptOrder_.push_back(cred.ssid);
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // keep the web server responsive
  beginAttempt(0);
}

void WifiService::beginAttempt(size_t index) {
  if (index >= attemptOrder_.size()) {
    startPortal();
    return;
  }
  credIndex_ = index;
  const WifiCredential* cred = wifiStore().find(attemptOrder_[index]);
  if (!cred) {
    beginAttempt(index + 1);
    return;
  }
  mode_ = everConnected_ ? RECONNECTING : CONNECTING;
  attemptStartMs_ = millis();
  Serial.printf("[wifi] connecting to \"%s\" (pw %d chars)\n", cred->ssid.c_str(),
                cred->password.length());
  WiFi.disconnect();
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
}

void WifiService::recordFailure() {
  lastFailedSsid_ = attemptOrder_.empty() ? String() : attemptOrder_[credIndex_];
  const wl_status_t st = WiFi.status();
  if (st == WL_NO_SSID_AVAIL) {
    lastFailReason_ = "network not found (device is 2.4 GHz only)";
  } else {
    lastFailReason_ = "check the password";
  }
  Serial.printf("[wifi] failed to join \"%s\": status=%d (%s)\n", lastFailedSsid_.c_str(), st,
                lastFailReason_.c_str());
}

void WifiService::startPortal() {
  mode_ = AP_PORTAL;
  WiFi.mode(WIFI_AP_STA);  // STA side stays idle but lets /api/wifi/scan work
  WiFi.softAP(AP_SSID);
  delay(100);
  dns_.start(53, "*", WiFi.softAPIP());
}

void WifiService::onConnected() {
  mode_ = CONNECTED;
  everConnected_ = true;
  justConnected_ = true;
  lastFailedSsid_ = "";
  lastFailReason_ = "";
  Serial.printf("[wifi] connected to \"%s\", ip %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());

  auto& store = wifiStore();
  if (store.lastConnectedSsid != WiFi.SSID()) {
    store.lastConnectedSsid = WiFi.SSID();
    store.save();
  }

  applyTimeConfig();
  MDNS.end();
  if (MDNS.begin(settings().hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
}

void WifiService::applyHostname() {
  if (mode_ != CONNECTED) return;  // mDNS comes up in onConnected()
  MDNS.end();
  if (MDNS.begin(settings().hostname.c_str())) {
    MDNS.addService("http", "tcp", 80);
  }
  Serial.printf("[wifi] mDNS renamed to http://%s.local\n", settings().hostname.c_str());
}

void WifiService::suspend() {
  dns_.stop();
  MDNS.end();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true /* turn radio off */);
  WiFi.mode(WIFI_OFF);
  mode_ = IDLE;
  Serial.println("[wifi] suspended (hibernate)");
}

void WifiService::applyTimeConfig() {
  setenv("TZ", settings().timezone.c_str(), 1);
  tzset();
  configTzTime(settings().timezone.c_str(), "pool.ntp.org", "time.google.com",
               "time.cloudflare.com");
}

void WifiService::loop() {
  switch (mode_) {
    case AP_PORTAL:
      dns_.processNextRequest();
      break;

    case CONNECTING:
    case RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        onConnected();
      } else if (millis() - attemptStartMs_ > ATTEMPT_TIMEOUT_MS) {
        recordFailure();
        if (everConnected_) {
          // Router blip: keep cycling saved networks forever, never portal.
          beginAttempt((credIndex_ + 1) % attemptOrder_.size());
        } else {
          beginAttempt(credIndex_ + 1);  // falls into portal after the last one
        }
      }
      break;

    case CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastReconnectMs_ > RECONNECT_INTERVAL_MS) {
          lastReconnectMs_ = millis();
          beginAttempt(0);
        }
      }
      break;

    case IDLE:
      break;
  }
}

String WifiService::ip() const {
  if (mode_ == AP_PORTAL) return WiFi.softAPIP().toString();
  return WiFi.localIP().toString();
}

String WifiService::currentSsid() const {
  if (mode_ == AP_PORTAL) return String(AP_SSID);
  return WiFi.SSID();
}

bool WifiService::justConnected() {
  if (!justConnected_) return false;
  justConnected_ = false;
  return true;
}

bool WifiService::modeChanged() {
  if (mode_ == lastReportedMode_) return false;
  lastReportedMode_ = mode_;
  return true;
}
