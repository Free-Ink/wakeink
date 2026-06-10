#pragma once

#include <Arduino.h>

#include <ctime>

namespace timeutil {

// Howard Hinnant's days-from-civil: epoch days for a y/m/d, no TZ involved.
inline long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (long)era * 146097 + (long)doe - 719468;
}

// UTC civil time -> epoch (timegm replacement; newlib has no timegm).
inline time_t epochFromUtc(int y, int mo, int d, int h, int mi, int s) {
  return (time_t)daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

// Local civil time -> epoch via mktime (honors the configured TZ + DST).
inline time_t epochFromLocal(int y, int mo, int d, int h, int mi, int s) {
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min = mi;
  tmv.tm_sec = s;
  tmv.tm_isdst = -1;
  return mktime(&tmv);
}

// ISO weekday 1=Mon..7=Sun from a tm (matches the app's work_days values).
inline int isoWeekday(const struct tm& tmv) { return tmv.tm_wday == 0 ? 7 : tmv.tm_wday; }

}  // namespace timeutil
