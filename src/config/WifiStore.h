#pragma once

// Saved WiFi networks, persisted as /wifi.json — same schema as
// crosspoint-reader ({lastConnectedSsid, credentials:[{ssid,password_obf}]}).

#include <Arduino.h>

#include <vector>

struct WifiCredential {
  String ssid;
  String password;  // plaintext in RAM; obfuscated on disk
};

class WifiStore {
 public:
  static constexpr size_t MAX_NETWORKS = 8;

  bool load();
  bool save() const;

  bool add(const String& ssid, const String& password);
  bool remove(const String& ssid);
  const WifiCredential* find(const String& ssid) const;

  std::vector<WifiCredential>& list() { return credentials_; }
  const std::vector<WifiCredential>& list() const { return credentials_; }

  String lastConnectedSsid;

 private:
  std::vector<WifiCredential> credentials_;
};

WifiStore& wifiStore();
