#pragma once

// Streaming iCalendar (RFC 5545) parser tuned for Google Calendar exports.
//
// Feed it raw HTTP body bytes; it assembles logical lines (unfolding), tracks
// VEVENT blocks, expands recurrence rules into concrete occurrences inside
// [windowStart, windowEnd), applies EXDATE, and reconciles RECURRENCE-ID
// overrides (including cancelled single instances) in finish().
//
// Supported RRULE subset (covers what Google's UI can produce):
//   FREQ=DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL,
//   BYDAY (weekly lists + monthly ordinals like 2TU / -1FR), BYMONTHDAY.
//
// Timezone model: times with a trailing Z are exact UTC. Times with a TZID
// are resolved against the feed's own VTIMEZONE definitions (TZOFFSETTO +
// yearly DST transition rules), so cross-timezone invites land at the right
// moment; only an unknown TZID falls back to the device's local timezone.
// VALUE=DATE entries are all-day events at device-local midnight.

#include <Arduino.h>

#include <vector>

#include "Event.h"

class IcsParser {
 public:
  IcsParser(std::vector<Event>& out, uint8_t calIndex, time_t windowStart, time_t windowEnd,
            const std::vector<String>& myEmailsLower, size_t maxEvents = 150);

  void feed(const char* data, size_t len);
  void finish();

  bool truncated() const { return truncated_; }

 private:
  struct Civil {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  };

  // One VTIMEZONE STANDARD/DAYLIGHT section: target offset + the yearly
  // transition that activates it (nth weekday of month at a local time).
  struct VtzRule {
    long offsetSec = 0;
    int month = 0;     // 1-12; 0 = no transition rule (fixed-offset zone)
    int week = 0;      // 1-5 = nth, -1 = last
    int wday = 0;      // 0 = Monday .. 6 = Sunday
    long timeSec = 7200;
    bool valid = false;
  };
  struct Vtz {
    String tzid;
    VtzRule std, dst;
    bool hasDst = false;
  };

  // Parsed datetime with enough context to convert any derived occurrence.
  struct DtParts {
    Civil civil;
    int vtzIndex = -1;  // into vtimezones_, -1 = none
    bool utc = false;
    bool isDate = false;
  };

  struct RawEvent {
    String uid, summary, location, description, organizer;
    String dtStart, dtStartParams;
    String dtEnd, dtEndParams;
    String duration;
    String rrule;
    String recurrenceId, recurrenceIdParams;
    std::vector<time_t> exdates;
    EventStatus status = EventStatus::CONFIRMED;
    SelfStatus selfStatus = SelfStatus::UNKNOWN;
    void clear() { *this = RawEvent(); }
  };

  void handlePhysicalLine();
  void processLogicalLine(const String& line);
  void handleEventProp(const String& name, const String& params, const String& value);
  void handleVtzProp(const String& name, const String& params, const String& value);
  void endEvent();

  void emitOccurrence(const RawEvent& raw, time_t start, long durationSec, bool allDay);
  void expandRecurrence(const RawEvent& raw, time_t dtStart, const DtParts& parts,
                        long durationSec, bool allDay);

  time_t parseDateTime(const String& value, const String& params, bool* isDate = nullptr,
                       DtParts* parts = nullptr) const;
  time_t civilToEpoch(const Civil& c, int vtzIndex, bool utc) const;
  long tzOffsetFor(const Vtz& z, const Civil& c) const;

  static String unescapeText(const String& value);
  static String paramValue(const String& params, const char* key);

  // Corruption fence: this object lives on the sync task's stack, and crash
  // forensics show something scribbling that region. Checked every feed().
  uint32_t canaryHead_ = 0xC0FFEE01;

  std::vector<Event>& out_;
  std::vector<Event> overrides_;
  std::vector<Vtz> vtimezones_;
  uint8_t calIndex_;
  time_t windowStart_, windowEnd_;
  std::vector<String> myEmails_;
  size_t maxEvents_;
  bool truncated_ = false;

  String lineBuf_;     // physical line being assembled
  String logical_;     // pending logical line (may still receive folds)
  bool havePending_ = false;
  bool inEvent_ = false;
  int skipDepth_ = 0;  // inside VALARM or other nested component
  RawEvent raw_;

  // VTIMEZONE block state.
  bool inVtz_ = false;
  int vtzSection_ = 0;  // 0 = none, 1 = STANDARD, 2 = DAYLIGHT
  Vtz curVtz_;

  uint32_t canaryTail_ = 0xC0FFEE02;  // see canaryHead_
};
