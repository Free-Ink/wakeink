#include "CalendarManager.h"

#include <WiFi.h>

#include <algorithm>

#include "../config/AppSettings.h"
#include "../config/CalendarStore.h"
#include "../config/StateStore.h"
#include "../net/IcsFetch.h"
#include "EventFilter.h"
#include "IcsParser.h"

namespace {
constexpr size_t MAX_EVENTS_PER_CALENDAR = 100;
constexpr size_t MAX_DISPLAY_EVENTS = 32;
constexpr long ALARM_GRACE_SEC = 5 * 60;  // ring up to 5 min after start
// TLS needs ~50 KB contiguous; starting a fetch below this risks taking the
// whole firmware down (malloc failures abort in library code).
constexpr uint32_t MIN_HEAP_FOR_FETCH = 70 * 1024;
// Sanity bound only. Google orders basic.ics oldest-first, so upcoming events
// sit at the END of the feed — truncating early throws away exactly the events
// we want (a real 8.7 MB calendar proved this). The streaming parser holds
// O(window) memory regardless of feed size, so big feeds just take time.
constexpr size_t MAX_ICS_BYTES = 20 * 1024 * 1024;
}  // namespace

CalendarManager& calendarManager() {
  static CalendarManager instance;
  return instance;
}

void CalendarManager::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // Pin to core 0; Arduino loop() (display + web server) runs on core 1.
  // 28K stack: the TLS handshake runs on this task's stack, and the gzip path
  // nests esp_http_client_read inside uzlib's refill callback. 16K overflowed
  // under TLS + inflate + parser depth — task stacks are heap blocks, so the
  // overflow surfaced as heap corruption aborts in unrelated allocations.
  xTaskCreatePinnedToCore(taskEntry, "cal_sync", 28672, this, 1, nullptr, 0);
}

void CalendarManager::taskEntry(void* self) { static_cast<CalendarManager*>(self)->taskLoop(); }

void CalendarManager::taskLoop() {
  while (true) {
    const time_t now = time(nullptr);
    int interval = 5;
    {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      interval = settings().pollIntervalMinutes;
      xSemaphoreGive(mutex_);
    }

    const bool timeValid = now > 1600000000;  // SNTP has synced
    const bool due = timeValid && (lastSyncAttempt_ == 0 || now - lastSyncAttempt_ >= interval * 60);

    if (WiFi.status() == WL_CONNECTED && timeValid && (due || syncRequested_)) {
      syncRequested_ = false;
      lastSyncAttempt_ = now;
      runSync();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void CalendarManager::runSync() {
  // Copy config under the lock; the network phase runs unlocked.
  AppSettings cfg;
  std::vector<CalendarSource> sources;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  cfg = settings();
  sources = calendarStore().list();
  status_.syncing = true;
  xSemaphoreGive(mutex_);

  Serial.printf("[sync] start heap=%lu min=%lu\n", (unsigned long)ESP.getFreeHeap(),
                (unsigned long)esp_get_minimum_free_heap_size());

  const time_t now = time(nullptr);
  const time_t windowEnd = now + (time_t)cfg.lookaheadDays * 86400;

  std::vector<Event> filtered;
  int okCount = 0;
  int total = 0;
  String firstError;
  // Per-calendar outcome, so one calendar's transient failure can't wipe its
  // previously synced events while another calendar succeeds.
  bool calOk[CalendarStore::MAX_CALENDARS] = {};

  for (size_t i = 0; i < sources.size(); ++i) {
    if (!sources[i].enabled || sources[i].url.isEmpty()) continue;
    ++total;

    if (ESP.getFreeHeap() < MIN_HEAP_FOR_FETCH) {
      if (firstError.isEmpty()) firstError = "low memory, sync truncated";
      break;
    }

    std::vector<Event> calEvents;
    IcsParser parser(calEvents, (uint8_t)i, now, windowEnd, cfg.myEmails,
                     MAX_EVENTS_PER_CALENDAR);
    int httpStatus = 0;
    size_t fed = 0;
    const icsfetch::Result rc = icsfetch::fetch(
        sources[i].url,
        [&parser, &fed](const char* data, size_t len) {
          fed += len;
          if (fed > MAX_ICS_BYTES) return false;  // ABORTED: keep what we parsed
          parser.feed(data, len);
          return true;
        },
        &httpStatus);

    if (rc != icsfetch::OK && rc != icsfetch::ABORTED) {
      if (firstError.isEmpty()) {
        firstError = sources[i].name + ": " +
                     (rc == icsfetch::HTTP_ERROR ? "HTTP " + String(httpStatus) : "connection failed");
      }
      continue;
    }
    if (rc == icsfetch::ABORTED && firstError.isEmpty()) {
      firstError = sources[i].name + ": feed over 20 MB, truncated";
    }
    parser.finish();
    ++okCount;
    if (i < CalendarStore::MAX_CALENDARS) calOk[i] = true;

    // Filter immediately per calendar: rejected events (and every survivor's
    // description, which only exists for keyword/link matching) are freed
    // right away instead of riding along until the end of the sync.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (Event& ev : calEvents) {
      if (eventfilter::passes(ev, cfg, stateStore())) {
        ev.hasLink = eventfilter::hasJoinableLink(ev);
        ev.description = String();
        ev.urls.clear();
        ev.urls.shrink_to_fit();
        filtered.push_back(std::move(ev));
      }
    }
    xSemaphoreGive(mutex_);
  }

  // Carry over the last-good events of any calendar that failed THIS round
  // (still enabled, just unreachable), so a transient ICS/DNS/TLS blip never
  // blanks that calendar. Ended events are aged out; disabled/removed
  // calendars drop naturally because they're neither fetched nor carried.
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (const Event& old : events_) {
    const uint8_t ci = old.calIndex;
    const bool wasAttempted = ci < sources.size() && sources[ci].enabled &&
                              !sources[ci].url.isEmpty();
    const bool failed = ci < CalendarStore::MAX_CALENDARS && !calOk[ci];
    if (wasAttempted && failed && old.end > now) filtered.push_back(old);
  }
  xSemaphoreGive(mutex_);

  std::sort(filtered.begin(), filtered.end(),
            [](const Event& a, const Event& b) { return a.start < b.start; });
  if (filtered.size() > MAX_DISPLAY_EVENTS) filtered.resize(MAX_DISPLAY_EVENTS);
  filtered.shrink_to_fit();

  xSemaphoreTake(mutex_, portMAX_DELAY);
  stateStore().pruneSkips(now);
  // Dirty only when the displayed content actually changed — a no-change poll
  // must not redraw the panel (on the M5 a content redraw is a ~15 s complete
  // waveform, so per-sync churn would blackout the screen every poll).
  const bool changed =
      filtered.size() != events_.size() ||
      !std::equal(filtered.begin(), filtered.end(), events_.begin(), [](const Event& a, const Event& b) {
        return a.start == b.start && a.end == b.end && a.calIndex == b.calIndex && a.title == b.title;
      });
  events_ = std::move(filtered);
  status_.lastSyncTime = time(nullptr);
  status_.lastSyncOk = (okCount == total);
  status_.lastError = firstError;
  status_.calendarsOk = okCount;
  status_.calendarsTotal = total;
  status_.syncing = false;

  // Trim fired-alarm keys for events no longer tracked.
  const time_t cutoff = time(nullptr) - 86400;
  firedKeys_.erase(std::remove_if(firedKeys_.begin(), firedKeys_.end(),
                                  [cutoff](const String& key) {
                                    const int sep = key.indexOf('|');
                                    const time_t start =
                                        sep >= 0 ? (time_t)strtoll(key.c_str() + sep + 1, nullptr, 10) : 0;
                                    return start != 0 && start < cutoff;
                                  }),
                   firedKeys_.end());
  xSemaphoreGive(mutex_);

  // stack_low is this task's high-water mark in bytes-remaining: if it ever
  // approaches 0, the stack budget above needs raising again.
  Serial.printf("[sync] done ok=%d/%d events=%u changed=%d heap=%lu min=%lu stack_low=%u\n", okCount,
                total, (unsigned)events_.size(), (int)changed, (unsigned long)ESP.getFreeHeap(),
                (unsigned long)esp_get_minimum_free_heap_size(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (changed) dirty_ = true;
}

void CalendarManager::refilterNow() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const AppSettings& cfg = settings();
  StateStore& state = stateStore();
  events_.erase(std::remove_if(events_.begin(), events_.end(),
                               [&](const Event& ev) {
                                 return !eventfilter::passes(ev, cfg, state,
                                                             /*trustStoredLink=*/true);
                               }),
                events_.end());
  xSemaphoreGive(mutex_);
  dirty_ = true;
}

std::vector<Event> CalendarManager::snapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  std::vector<Event> copy = events_;
  xSemaphoreGive(mutex_);
  return copy;
}

SyncStatus CalendarManager::status() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  SyncStatus copy = status_;
  xSemaphoreGive(mutex_);
  return copy;
}

bool CalendarManager::consumeDirtyFlag() {
  if (!dirty_) return false;
  dirty_ = false;
  return true;
}

bool CalendarManager::nextAlarm(time_t now, Event& out) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (stateStore().isPaused(now)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  bool found = false;
  for (const Event& ev : events_) {
    const int lead = eventfilter::leadTimeMinutes(ev, settings());
    const time_t alarmAt = ev.start - (time_t)lead * 60;
    if (now >= alarmAt && now < ev.start + ALARM_GRACE_SEC) {
      bool fired = false;
      const String key = ev.skipKey();
      for (const auto& k : firedKeys_) {
        if (k == key) fired = true;
      }
      if (!fired) {
        out = ev;
        found = true;
        break;
      }
    }
  }
  xSemaphoreGive(mutex_);
  return found;
}

void CalendarManager::markFired(const Event& ev) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  firedKeys_.push_back(ev.skipKey());
  xSemaphoreGive(mutex_);
}

bool CalendarManager::wasFired(const Event& ev) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  bool fired = false;
  const String key = ev.skipKey();
  for (const auto& k : firedKeys_) {
    if (k == key) fired = true;
  }
  xSemaphoreGive(mutex_);
  return fired;
}
