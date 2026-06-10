#include "WifiStore.h"

#include "FsJson.h"
#include "Obfuscation.h"

static const char* WIFI_PATH = "/wifi.json";

WifiStore& wifiStore() {
  static WifiStore instance;
  return instance;
}

bool WifiStore::load() {
  JsonDocument doc;
  if (!fsjson::loadDoc(WIFI_PATH, doc)) return false;

  lastConnectedSsid = doc["lastConnectedSsid"] | "";
  credentials_.clear();
  for (JsonObjectConst obj : doc["credentials"].as<JsonArrayConst>()) {
    if (credentials_.size() >= MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | "";
    if (cred.ssid.isEmpty()) continue;
    bool ok = false;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok) cred.password = obj["password"] | "";  // unobfuscated fallback
    credentials_.push_back(cred);
  }
  return true;
}

bool WifiStore::save() const {
  JsonDocument doc;
  doc["lastConnectedSsid"] = lastConnectedSsid;
  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : credentials_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }
  return fsjson::saveDoc(WIFI_PATH, doc);
}

bool WifiStore::add(const String& ssid, const String& password) {
  for (auto& cred : credentials_) {
    if (cred.ssid == ssid) {
      cred.password = password;
      return save();
    }
  }
  if (credentials_.size() >= MAX_NETWORKS) return false;
  credentials_.push_back({ssid, password});
  return save();
}

bool WifiStore::remove(const String& ssid) {
  for (auto it = credentials_.begin(); it != credentials_.end(); ++it) {
    if (it->ssid == ssid) {
      credentials_.erase(it);
      return save();
    }
  }
  return false;
}

const WifiCredential* WifiStore::find(const String& ssid) const {
  for (const auto& cred : credentials_) {
    if (cred.ssid == ssid) return &cred;
  }
  return nullptr;
}
