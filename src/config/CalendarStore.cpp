#include "CalendarStore.h"

#include "FsJson.h"

static const char* CALENDARS_PATH = "/calendars.json";

CalendarStore& calendarStore() {
  static CalendarStore instance;
  return instance;
}

bool CalendarStore::load() {
  JsonDocument doc;
  if (!fsjson::loadDoc(CALENDARS_PATH, doc)) return false;
  calendars_.clear();
  for (JsonObjectConst obj : doc["calendars"].as<JsonArrayConst>()) {
    if (calendars_.size() >= MAX_CALENDARS) break;
    CalendarSource cal;
    cal.name = obj["name"] | "";
    cal.url = obj["url"] | "";
    cal.enabled = obj["enabled"] | true;
    if (!cal.url.isEmpty()) calendars_.push_back(cal);
  }
  return true;
}

bool CalendarStore::save() const {
  JsonDocument doc;
  JsonArray arr = doc["calendars"].to<JsonArray>();
  for (const auto& cal : calendars_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = cal.name;
    obj["url"] = cal.url;
    obj["enabled"] = cal.enabled;
  }
  return fsjson::saveDoc(CALENDARS_PATH, doc);
}
