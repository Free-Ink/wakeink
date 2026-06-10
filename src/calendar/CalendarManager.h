#pragma once

// Owns the event pipeline: a background FreeRTOS task polls every enabled ICS
// calendar on the configured interval, parses + filters + sorts, then swaps
// the result in under a mutex so the main loop (display) and web handlers can
// snapshot it without blocking on network I/O.
//
// Alarm scheduling is evaluated by the main loop via nextAlarm(): an event
// rings when now >= start - leadTime (early-meeting aware), unless paused,
// skipped, already fired, or the event started more than GRACE ago.

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <vector>

#include "Event.h"

struct SyncStatus {
  time_t lastSyncTime = 0;
  bool lastSyncOk = false;
  String lastError;
  int calendarsOk = 0;
  int calendarsTotal = 0;
  bool syncing = false;
};

class CalendarManager {
 public:
  void begin();                 // spawns the sync task
  void requestSync() { syncRequested_ = true; }

  // Re-applies the filter chain to the cached events immediately (no network).
  // Tightened filters take effect on the next redraw; loosened ones are
  // completed by the background sync that callers should also request.
  void refilterNow();

  // Snapshot of upcoming (filtered, sorted) events.
  std::vector<Event> snapshot() const;
  SyncStatus status() const;

  // True once a sync produced data and the display should redraw.
  bool consumeDirtyFlag();

  // The event that should be ringing right now, if any.
  bool nextAlarm(time_t now, Event& out) const;
  void markFired(const Event& ev);
  bool wasFired(const Event& ev) const;

  // Config lock: web handlers must hold this while mutating settings(),
  // calendarStore() or stateStore(), because the sync task copies them on
  // the other core.
  void lockConfig() const { xSemaphoreTake(mutex_, portMAX_DELAY); }
  void unlockConfig() const { xSemaphoreGive(mutex_); }

 private:
  static void taskEntry(void* self);
  void taskLoop();
  void runSync();

  mutable SemaphoreHandle_t mutex_ = nullptr;
  std::vector<Event> events_;
  std::vector<String> firedKeys_;  // "uid|start" of alarms already rung
  SyncStatus status_;
  volatile bool syncRequested_ = false;
  volatile bool dirty_ = false;
  time_t lastSyncAttempt_ = 0;
};

CalendarManager& calendarManager();
