#pragma once

#include <Arduino.h>

// One concrete event occurrence (recurring events are expanded into
// individual occurrences inside the lookahead window).

// Self-RSVP states, matching Android's SELF_ATTENDEE_STATUS values where the
// filter logic was ported from. UNKNOWN means the ICS had no ATTENDEE line for
// the configured email (solo events, or no email configured) and is treated as
// accepted.
enum class SelfStatus : int8_t {
  UNKNOWN = -1,
  NONE = 0,       // PARTSTAT=NEEDS-ACTION
  ACCEPTED = 1,
  DECLINED = 2,
  INVITED = 3,
  TENTATIVE = 4,  // "maybe"
};

enum class EventStatus : int8_t {
  TENTATIVE = 0,
  CONFIRMED = 1,
  CANCELLED = 2,
};

struct Event {
  String uid;
  String title;
  String organizer;    // CN= display name, or email
  String location;     // used for meeting-link detection + display
  String description;  // truncated copy, used for keyword/link checks only
  time_t start = 0;
  time_t end = 0;
  bool allDay = false;
  // Computed at sync time (description is dropped after filtering to save RAM).
  bool hasLink = false;
  EventStatus status = EventStatus::CONFIRMED;
  SelfStatus selfStatus = SelfStatus::UNKNOWN;
  uint8_t calIndex = 0;

  // RECURRENCE-ID handling: an override replaces the base occurrence whose
  // start equals recurrenceId (same uid).
  bool isOverride = false;
  time_t recurrenceId = 0;

  String skipKey() const { return uid + "|" + String((long long)start); }
};
