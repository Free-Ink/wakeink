#pragma once

// Mutable runtime state (pause + per-event skips), persisted as /state.json so
// it survives reboots without dirtying settings.json.
//
// Skip keys are "uid|startEpoch" so a single instance of a recurring event can
// be skipped, mirroring the Android app's skipped_events list.

#include <Arduino.h>

#include <vector>

class StateStore {
 public:
  // 0 = not paused. Matches the app's pause_until; "indefinite" is just a
  // far-future timestamp (year 2999).
  time_t pauseUntil = 0;
  // The duration button that was chosen (-1 indefinite, else minutes); lets
  // the web UI highlight the active pause option.
  int pauseChoiceMinutes = 0;

  bool load();
  bool save() const;

  bool isPaused(time_t now) const { return pauseUntil != 0 && now < pauseUntil; }

  bool isSkipped(const String& key) const;
  void skip(const String& key);
  void unskip(const String& key);
  // Drop skip entries whose event start is in the past.
  void pruneSkips(time_t now);

  const std::vector<String>& skipped() const { return skipped_; }

 private:
  std::vector<String> skipped_;
};

StateStore& stateStore();
