#pragma once

// The list of subscribed ICS calendars (Google "secret address" links pasted
// into the web UI), persisted as /calendars.json.

#include <Arduino.h>

#include <vector>

struct CalendarSource {
  String name;
  String url;      // https://calendar.google.com/calendar/ical/.../basic.ics
  bool enabled = true;
};

class CalendarStore {
 public:
  static constexpr size_t MAX_CALENDARS = 8;

  bool load();
  bool save() const;

  std::vector<CalendarSource>& list() { return calendars_; }
  const std::vector<CalendarSource>& list() const { return calendars_; }

 private:
  std::vector<CalendarSource> calendars_;
};

CalendarStore& calendarStore();
