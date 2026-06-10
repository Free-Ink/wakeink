#include "StateStore.h"

#include "FsJson.h"

static const char* STATE_PATH = "/state.json";

StateStore& stateStore() {
  static StateStore instance;
  return instance;
}

bool StateStore::load() {
  JsonDocument doc;
  if (!fsjson::loadDoc(STATE_PATH, doc)) return false;
  pauseUntil = doc["pause_until"] | 0;
  pauseChoiceMinutes = doc["pause_choice"] | 0;
  skipped_.clear();
  for (JsonVariantConst v : doc["skipped"].as<JsonArrayConst>()) {
    const char* s = v.as<const char*>();
    if (s && *s) skipped_.push_back(String(s));
  }
  return true;
}

bool StateStore::save() const {
  JsonDocument doc;
  doc["pause_until"] = (long long)pauseUntil;
  doc["pause_choice"] = pauseChoiceMinutes;
  JsonArray arr = doc["skipped"].to<JsonArray>();
  for (const auto& s : skipped_) arr.add(s);
  return fsjson::saveDoc(STATE_PATH, doc);
}

bool StateStore::isSkipped(const String& key) const {
  for (const auto& s : skipped_) {
    if (s == key) return true;
  }
  return false;
}

void StateStore::skip(const String& key) {
  if (isSkipped(key)) return;
  skipped_.push_back(key);
  save();
}

void StateStore::unskip(const String& key) {
  for (auto it = skipped_.begin(); it != skipped_.end(); ++it) {
    if (*it == key) {
      skipped_.erase(it);
      save();
      return;
    }
  }
}

void StateStore::pruneSkips(time_t now) {
  bool changed = false;
  for (auto it = skipped_.begin(); it != skipped_.end();) {
    const int sep = it->indexOf('|');
    const time_t start = (sep >= 0) ? (time_t)strtoll(it->c_str() + sep + 1, nullptr, 10) : 0;
    if (start != 0 && start < now - 3600) {
      it = skipped_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) save();
}
