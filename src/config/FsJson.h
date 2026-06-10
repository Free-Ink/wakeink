#pragma once

// Tiny helpers shared by every JSON config store: read/write whole files on
// LittleFS and atomically replace via a .tmp rename.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace fsjson {

inline String readFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return String();
  String out = f.readString();
  f.close();
  return out;
}

inline bool writeFile(const char* path, const String& data) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp.c_str(), "w");
  if (!f) return false;
  const size_t written = f.print(data);
  f.close();
  if (written != data.length()) {
    LittleFS.remove(tmp.c_str());
    return false;
  }
  LittleFS.remove(path);
  return LittleFS.rename(tmp.c_str(), path);
}

inline bool loadDoc(const char* path, JsonDocument& doc) {
  const String raw = readFile(path);
  if (raw.isEmpty()) return false;
  return deserializeJson(doc, raw) == DeserializationError::Ok;
}

inline bool saveDoc(const char* path, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return writeFile(path, out);
}

}  // namespace fsjson
