#include "IcsParser.h"

#include <cstdlib>

#include "EventFilter.h"
#include "TimeUtil.h"

namespace {

constexpr size_t MAX_LOGICAL_LINE = 4096;
// Field caps keep a busy calendar from exhausting the heap; description and
// location only need enough text for keyword/meeting-link matching.
constexpr size_t MAX_DESCRIPTION = 400;
constexpr size_t MAX_TITLE = 120;
constexpr size_t MAX_LOCATION = 240;
constexpr size_t MAX_ORGANIZER = 60;
constexpr size_t MAX_UID = 80;  // truncated consistently, so override matching still works
// RECURRENCE-ID overrides held for finish(); a years-old Google calendar can
// carry thousands, so only window-relevant ones are kept (see endEvent).
constexpr size_t MAX_OVERRIDES = 200;
constexpr size_t MAX_VTIMEZONES = 12;
constexpr int MAX_DAY_ITERATIONS = 40000;   // ~110 years of daily stepping
constexpr int MAX_MONTH_ITERATIONS = 1200;  // 100 years
constexpr int MAX_YEAR_ITERATIONS = 100;

struct Rrule {
  enum Freq { DAILY, WEEKLY, MONTHLY, YEARLY };
  Freq freq = DAILY;
  int interval = 1;
  long count = -1;       // -1 = unbounded
  time_t until = 0;      // 0 = none
  uint8_t bydayMask = 0; // bit 0 = Monday .. bit 6 = Sunday
  int bydayOrd = 0;      // monthly ordinal (2 = 2nd, -1 = last); 0 = plain list
  int bymonthday = 0;
  bool valid = false;
};

int weekdayBit(const char* s) {
  static const char* names[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  for (int i = 0; i < 7; ++i) {
    if (strncmp(s, names[i], 2) == 0) return i;
  }
  return -1;
}

int daysInMonth(int year, int month) {  // month 1-12
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return days[month - 1];
}

// Inverse of timeutil::daysFromCivil (Howard Hinnant's civil_from_days).
void civilFromDays(long z, int& y, int& m, int& d) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long yr = (long)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp + (mp < 10 ? 3 : -9));
  y = (int)(yr + (m <= 2));
}

// Monday-based weekday (0 = Monday) for an epoch day number (day 0 = Thursday).
int weekdayOfDayNum(long dayNum) { return (int)(((dayNum % 7) + 7 + 3) % 7); }

// Epoch day number of a VTIMEZONE transition (nth/last weekday of month) in a year.
long transitionDayNum(int year, int month, int week, int wday) {
  const long first = timeutil::daysFromCivil(year, month, 1);
  const int firstWd = weekdayOfDayNum(first);
  const int dim = daysInMonth(year, month);
  if (week > 0) {
    int day = 1 + ((wday - firstWd + 7) % 7) + (week - 1) * 7;
    while (day > dim) day -= 7;  // 5th weekday may not exist; clamp to last
    return first + day - 1;
  }
  const int lastWd = (firstWd + dim - 1) % 7;
  const int day = dim - ((lastWd - wday + 7) % 7);
  return first + day - 1;
}

long parseUtcOffset(const String& value) {  // "+0530" / "-0800" [/ "+053030"]
  if (value.length() < 5) return 0;
  const int sign = value[0] == '-' ? -1 : 1;
  const long h = (value[1] - '0') * 10 + (value[2] - '0');
  const long m = (value[3] - '0') * 10 + (value[4] - '0');
  long s = 0;
  if (value.length() >= 7) s = (value[5] - '0') * 10 + (value[6] - '0');
  return sign * (h * 3600 + m * 60 + s);
}

long parseIcsDuration(const String& value) {
  // P[n]W | P[n]D[T[n]H[n]M[n]S] — returns seconds, 0 on parse failure.
  long total = 0;
  const char* p = value.c_str();
  bool negative = false;
  if (*p == '-') {
    negative = true;
    ++p;
  }
  if (*p != 'P') return 0;
  ++p;
  bool inTime = false;
  while (*p) {
    if (*p == 'T') {
      inTime = true;
      ++p;
      continue;
    }
    char* end;
    const long n = strtol(p, &end, 10);
    if (end == p) break;
    p = end;
    switch (*p) {
      case 'W': total += n * 7 * 86400; break;
      case 'D': total += n * 86400; break;
      case 'H': total += n * 3600; break;
      case 'M': total += inTime ? n * 60 : 0; break;
      case 'S': total += n; break;
      default: return negative ? -total : total;
    }
    ++p;
  }
  return negative ? -total : total;
}

Rrule parseRrule(const String& text) {
  Rrule r;
  int pos = 0;
  while (pos < (int)text.length()) {
    int semi = text.indexOf(';', pos);
    if (semi < 0) semi = text.length();
    const String part = text.substring(pos, semi);
    pos = semi + 1;
    const int eq = part.indexOf('=');
    if (eq < 0) continue;
    const String key = part.substring(0, eq);
    const String val = part.substring(eq + 1);

    if (key == "FREQ") {
      if (val == "DAILY") {
        r.freq = Rrule::DAILY;
      } else if (val == "WEEKLY") {
        r.freq = Rrule::WEEKLY;
      } else if (val == "MONTHLY") {
        r.freq = Rrule::MONTHLY;
      } else if (val == "YEARLY") {
        r.freq = Rrule::YEARLY;
      } else {
        return r;  // unsupported FREQ -> invalid
      }
      r.valid = true;
    } else if (key == "INTERVAL") {
      r.interval = max(1, (int)val.toInt());
    } else if (key == "COUNT") {
      r.count = val.toInt();
    } else if (key == "BYDAY") {
      int p2 = 0;
      while (p2 < (int)val.length()) {
        int comma = val.indexOf(',', p2);
        if (comma < 0) comma = val.length();
        String day = val.substring(p2, comma);
        p2 = comma + 1;
        day.trim();
        int ord = 0;
        int i = 0;
        if (i < (int)day.length() && (day[i] == '+' || day[i] == '-' || isdigit(day[i]))) {
          ord = atoi(day.c_str());
          while (i < (int)day.length() && (day[i] == '+' || day[i] == '-' || isdigit(day[i]))) ++i;
        }
        const int bit = weekdayBit(day.c_str() + i);
        if (bit >= 0) {
          r.bydayMask |= (1 << bit);
          if (ord != 0) r.bydayOrd = ord;
        }
      }
    } else if (key == "BYMONTHDAY") {
      r.bymonthday = val.toInt();
    }
  }
  return r;
}

String extractRruleValue(const String& rrule, const char* key) {
  String needle = String(key) + "=";
  int idx = rrule.indexOf(needle);
  if (idx < 0) return String();
  if (idx > 0 && rrule[idx - 1] != ';') return String();
  const int start = idx + needle.length();
  int end = rrule.indexOf(';', start);
  if (end < 0) end = rrule.length();
  return rrule.substring(start, end);
}

}  // namespace

IcsParser::IcsParser(std::vector<Event>& out, uint8_t calIndex, time_t windowStart,
                     time_t windowEnd, const std::vector<String>& myEmailsLower, size_t maxEvents)
    : out_(out),
      calIndex_(calIndex),
      windowStart_(windowStart),
      windowEnd_(windowEnd),
      myEmails_(myEmailsLower),
      maxEvents_(maxEvents) {
  lineBuf_.reserve(256);
  logical_.reserve(512);
  // Pre-size the hot vectors: growth reallocations move Event objects (five
  // Strings each) and were implicated in a heap-pressure crash.
  out_.reserve(out_.size() + std::min(maxEvents_, (size_t)64));
  overrides_.reserve(32);
}

void IcsParser::feed(const char* data, size_t len) {
  if (canaryHead_ != 0xC0FFEE01 || canaryTail_ != 0xC0FFEE02) {
    Serial.printf("[parser] CANARY SMASHED head=%08lx tail=%08lx this=%p line=%u logical=%u\n",
                  (unsigned long)canaryHead_, (unsigned long)canaryTail_, this,
                  (unsigned)lineBuf_.length(), (unsigned)logical_.length());
    abort();  // die here, close to the corruption, with a useful trace
  }
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\n') {
      handlePhysicalLine();
      lineBuf_ = "";
    } else if (c != '\r') {
      if (lineBuf_.length() < MAX_LOGICAL_LINE) lineBuf_ += c;
    }
  }
}

void IcsParser::handlePhysicalLine() {
  // RFC 5545 folding: a line starting with space/tab continues the previous.
  if (!lineBuf_.isEmpty() && (lineBuf_[0] == ' ' || lineBuf_[0] == '\t')) {
    if (havePending_ && logical_.length() < MAX_LOGICAL_LINE) {
      logical_ += lineBuf_.substring(1);
    }
    return;
  }
  if (havePending_) processLogicalLine(logical_);
  logical_ = lineBuf_;
  havePending_ = true;
}

void IcsParser::finish() {
  handlePhysicalLine();
  if (havePending_) {
    processLogicalLine(logical_);
    havePending_ = false;
  }

  // Reconcile RECURRENCE-ID overrides: each override replaces the base
  // occurrence with the same uid + original start. Cancelled overrides are
  // pure tombstones.
  for (const Event& ov : overrides_) {
    for (auto it = out_.begin(); it != out_.end(); ++it) {
      if (!it->isOverride && it->uid == ov.uid && it->start == ov.recurrenceId) {
        out_.erase(it);
        break;
      }
    }
    if (ov.status == EventStatus::CANCELLED) continue;
    if (ov.end > windowStart_ && ov.start < windowEnd_) {
      if (out_.size() < maxEvents_) {
        out_.push_back(ov);
      } else {
        truncated_ = true;
      }
    }
  }
  overrides_.clear();
}

void IcsParser::processLogicalLine(const String& line) {
  if (line.isEmpty()) return;

  // Split NAME[;PARAMS]:VALUE at the first ':' outside double quotes.
  int colon = -1;
  bool inQuotes = false;
  for (int i = 0; i < (int)line.length(); ++i) {
    const char c = line[i];
    if (c == '"') inQuotes = !inQuotes;
    if (c == ':' && !inQuotes) {
      colon = i;
      break;
    }
  }
  if (colon < 0) return;

  String head = line.substring(0, colon);
  const String value = line.substring(colon + 1);
  String name = head;
  String params;
  const int semi = head.indexOf(';');
  if (semi >= 0) {
    name = head.substring(0, semi);
    params = head.substring(semi);  // keep leading ';' for param scanning
  }
  name.toUpperCase();

  if (name == "BEGIN") {
    if (value == "VEVENT") {
      inEvent_ = true;
      skipDepth_ = 0;
      raw_.clear();
    } else if (inEvent_) {
      ++skipDepth_;  // VALARM etc. inside the event
    } else if (value == "VTIMEZONE") {
      inVtz_ = true;
      vtzSection_ = 0;
      curVtz_ = Vtz();
    } else if (inVtz_ && value == "STANDARD") {
      vtzSection_ = 1;
    } else if (inVtz_ && value == "DAYLIGHT") {
      vtzSection_ = 2;
    }
    return;
  }
  if (name == "END") {
    if (inEvent_ && skipDepth_ > 0) {
      --skipDepth_;
    } else if (value == "VEVENT" && inEvent_) {
      endEvent();
      inEvent_ = false;
    } else if (inVtz_ && (value == "STANDARD" || value == "DAYLIGHT")) {
      vtzSection_ = 0;
    } else if (value == "VTIMEZONE" && inVtz_) {
      inVtz_ = false;
      if (!curVtz_.tzid.isEmpty() && curVtz_.std.valid && vtimezones_.size() < MAX_VTIMEZONES) {
        curVtz_.hasDst = curVtz_.dst.valid && curVtz_.dst.month != 0 && curVtz_.std.month != 0;
        vtimezones_.push_back(curVtz_);
      }
    }
    return;
  }

  if (inEvent_ && skipDepth_ == 0) {
    handleEventProp(name, params, value);
  } else if (inVtz_) {
    handleVtzProp(name, params, value);
  }
}

void IcsParser::handleVtzProp(const String& name, const String& params, const String& value) {
  (void)params;
  if (vtzSection_ == 0) {
    if (name == "TZID") curVtz_.tzid = value;
    return;
  }
  VtzRule& rule = vtzSection_ == 1 ? curVtz_.std : curVtz_.dst;
  if (name == "TZOFFSETTO") {
    rule.offsetSec = parseUtcOffset(value);
    rule.valid = true;
  } else if (name == "DTSTART") {
    // Only the local transition time-of-day matters ("19700308T020000").
    if (value.length() >= 15 && value[8] == 'T') {
      rule.timeSec = (long)((value[9] - '0') * 10 + (value[10] - '0')) * 3600 +
                     (long)((value[11] - '0') * 10 + (value[12] - '0')) * 60;
    }
  } else if (name == "RRULE") {
    // FREQ=YEARLY;BYMONTH=3;BYDAY=2SU (Google's standard shape).
    const String bymonth = extractRruleValue(value, "BYMONTH");
    const String byday = extractRruleValue(value, "BYDAY");
    if (!bymonth.isEmpty() && !byday.isEmpty()) {
      rule.month = bymonth.toInt();
      int ord = atoi(byday.c_str());
      int i = 0;
      while (i < (int)byday.length() && (byday[i] == '+' || byday[i] == '-' || isdigit(byday[i]))) {
        ++i;
      }
      const int bit = weekdayBit(byday.c_str() + i);
      if (bit >= 0 && rule.month >= 1 && rule.month <= 12) {
        rule.week = ord == 0 ? 1 : ord;
        rule.wday = bit;
      } else {
        rule.month = 0;
      }
    }
  }
}

String IcsParser::paramValue(const String& params, const char* key) {
  const String needle = String(";") + key + "=";
  const int idx = params.indexOf(needle);
  if (idx < 0) return String();
  int start = idx + needle.length();
  String out;
  bool inQuotes = false;
  for (int i = start; i < (int)params.length(); ++i) {
    const char c = params[i];
    if (c == '"') {
      inQuotes = !inQuotes;
      continue;
    }
    if ((c == ';' || c == ',') && !inQuotes) break;
    out += c;
  }
  return out;
}

String IcsParser::unescapeText(const String& value) {
  String out;
  out.reserve(value.length());
  for (int i = 0; i < (int)value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' && i + 1 < (int)value.length()) {
      const char n = value[i + 1];
      if (n == 'n' || n == 'N') {
        out += ' ';  // single-line display; newlines become spaces
      } else {
        out += n;
      }
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

long IcsParser::tzOffsetFor(const Vtz& z, const Civil& c) const {
  if (!z.hasDst) return z.std.offsetSec;
  const long dstDay = transitionDayNum(c.y, z.dst.month, z.dst.week, z.dst.wday);
  const long stdDay = transitionDayNum(c.y, z.std.month, z.std.week, z.std.wday);
  const long cSec = timeutil::daysFromCivil(c.y, c.mo, c.d) * 86400L + c.h * 3600L + c.mi * 60L;
  const long dstStart = dstDay * 86400L + z.dst.timeSec;
  const long stdStart = stdDay * 86400L + z.std.timeSec;
  // Northern hemisphere: DST window sits inside the year. Southern: it wraps.
  const bool inDst = dstStart < stdStart ? (cSec >= dstStart && cSec < stdStart)
                                         : (cSec >= dstStart || cSec < stdStart);
  return inDst ? z.dst.offsetSec : z.std.offsetSec;
}

time_t IcsParser::civilToEpoch(const Civil& c, int vtzIndex, bool utc) const {
  if (utc) return timeutil::epochFromUtc(c.y, c.mo, c.d, c.h, c.mi, c.s);
  if (vtzIndex >= 0 && vtzIndex < (int)vtimezones_.size()) {
    const time_t asUtc = timeutil::epochFromUtc(c.y, c.mo, c.d, c.h, c.mi, c.s);
    return asUtc - tzOffsetFor(vtimezones_[vtzIndex], c);
  }
  return timeutil::epochFromLocal(c.y, c.mo, c.d, c.h, c.mi, c.s);
}

time_t IcsParser::parseDateTime(const String& value, const String& params, bool* isDate,
                                DtParts* parts) const {
  if (isDate) *isDate = false;
  const char* v = value.c_str();
  if (strlen(v) < 8) return 0;

  Civil c;
  c.y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0');
  c.mo = (v[4] - '0') * 10 + (v[5] - '0');
  c.d = (v[6] - '0') * 10 + (v[7] - '0');

  const bool dateOnly = paramValue(params, "VALUE") == "DATE" || strlen(v) == 8 || v[8] != 'T';
  if (dateOnly) {
    if (isDate) *isDate = true;
    if (parts) {
      parts->civil = c;
      parts->vtzIndex = -1;
      parts->utc = false;
      parts->isDate = true;
    }
    return timeutil::epochFromLocal(c.y, c.mo, c.d, 0, 0, 0);
  }

  if (strlen(v) < 15) return 0;
  c.h = (v[9] - '0') * 10 + (v[10] - '0');
  c.mi = (v[11] - '0') * 10 + (v[12] - '0');
  c.s = (v[13] - '0') * 10 + (v[14] - '0');

  const String tzid = paramValue(params, "TZID");
  const bool utc = (v[strlen(v) - 1] == 'Z') || tzid == "UTC";
  int vtzIndex = -1;
  if (!utc && !tzid.isEmpty()) {
    for (size_t i = 0; i < vtimezones_.size(); ++i) {
      if (vtimezones_[i].tzid == tzid) {
        vtzIndex = (int)i;
        break;
      }
    }
    // Unknown TZID (no VTIMEZONE in feed): falls through to device-local.
  }

  if (parts) {
    parts->civil = c;
    parts->vtzIndex = vtzIndex;
    parts->utc = utc;
    parts->isDate = false;
  }
  return civilToEpoch(c, vtzIndex, utc);
}

void IcsParser::handleEventProp(const String& name, const String& params, const String& value) {
  if (name == "UID") {
    raw_.uid = value.length() > MAX_UID ? value.substring(0, MAX_UID) : value;
  } else if (name == "SUMMARY") {
    raw_.summary = unescapeText(value);
    if (raw_.summary.length() > MAX_TITLE) raw_.summary = raw_.summary.substring(0, MAX_TITLE);
  } else if (name == "LOCATION") {
    raw_.location = unescapeText(value);
    // Link-scan the FULL text before capping for storage.
    String low = raw_.location;
    low.toLowerCase();
    if (eventfilter::textHasMeetingDomain(low)) raw_.linkJoinable = true;
    eventfilter::collectUrls(low, raw_.urls, 4);
    if (raw_.location.length() > MAX_LOCATION) raw_.location = raw_.location.substring(0, MAX_LOCATION);
  } else if (name == "DESCRIPTION") {
    String text = unescapeText(value);
    // Link-scan the FULL description: Google's Meet link sits ~1KB into its
    // boilerplate, far past the storage cap, and the links-only filter was
    // silently dropping those events.
    String low = text;
    low.toLowerCase();
    if (eventfilter::textHasMeetingDomain(low)) raw_.linkJoinable = true;
    eventfilter::collectUrls(low, raw_.urls, 4);
    if (text.length() > MAX_DESCRIPTION) text = text.substring(0, MAX_DESCRIPTION);
    raw_.description = text;
  } else if (name == "X-GOOGLE-CONFERENCE") {
    // Dedicated conference property — a joinable link by definition.
    if (value.startsWith("http")) {
      raw_.linkJoinable = true;
      if (raw_.urls.size() < 4) {
        String low = value;
        low.toLowerCase();
        raw_.urls.push_back(low.length() > 96 ? low.substring(0, 96) : low);
      }
    }
  } else if (name == "DTSTART") {
    raw_.dtStart = value;
    raw_.dtStartParams = params;
  } else if (name == "DTEND") {
    raw_.dtEnd = value;
    raw_.dtEndParams = params;
  } else if (name == "DURATION") {
    raw_.duration = value;
  } else if (name == "RRULE") {
    raw_.rrule = value;
  } else if (name == "EXDATE") {
    int pos = 0;
    while (pos < (int)value.length()) {
      int comma = value.indexOf(',', pos);
      if (comma < 0) comma = value.length();
      const time_t t = parseDateTime(value.substring(pos, comma), params);
      if (t) raw_.exdates.push_back(t);
      pos = comma + 1;
    }
  } else if (name == "RECURRENCE-ID") {
    raw_.recurrenceId = value;
    raw_.recurrenceIdParams = params;
  } else if (name == "STATUS") {
    if (value == "CANCELLED") {
      raw_.status = EventStatus::CANCELLED;
    } else if (value == "TENTATIVE") {
      raw_.status = EventStatus::TENTATIVE;
    } else {
      raw_.status = EventStatus::CONFIRMED;
    }
  } else if (name == "ORGANIZER") {
    String cn = paramValue(params, "CN");
    if (!cn.isEmpty()) {
      raw_.organizer = cn;
    } else {
      String email = value;
      if (email.startsWith("mailto:")) email = email.substring(7);
      raw_.organizer = email;
    }
    if (raw_.organizer.length() > MAX_ORGANIZER) {
      raw_.organizer = raw_.organizer.substring(0, MAX_ORGANIZER);
    }
  } else if (name == "ATTENDEE") {
    if (myEmails_.empty()) return;
    String v = value;
    v.toLowerCase();
    bool mine = false;
    for (const auto& email : myEmails_) {
      if (!email.isEmpty() && v.indexOf("mailto:" + email) >= 0) {
        mine = true;
        break;
      }
    }
    if (!mine) return;
    String partstat = paramValue(params, "PARTSTAT");
    partstat.toUpperCase();
    if (partstat == "ACCEPTED") {
      raw_.selfStatus = SelfStatus::ACCEPTED;
    } else if (partstat == "DECLINED") {
      raw_.selfStatus = SelfStatus::DECLINED;
    } else if (partstat == "TENTATIVE") {
      raw_.selfStatus = SelfStatus::TENTATIVE;
    } else {
      raw_.selfStatus = SelfStatus::NONE;  // NEEDS-ACTION / unknown
    }
  }
}

void IcsParser::emitOccurrence(const RawEvent& raw, time_t start, long durationSec, bool allDay) {
  for (time_t ex : raw.exdates) {
    if (ex == start) return;
  }
  const time_t end = start + durationSec;
  if (!(end > windowStart_ && start < windowEnd_)) return;
  if (out_.size() >= maxEvents_) {
    truncated_ = true;
    return;
  }

  Event ev;
  ev.uid = raw.uid;
  ev.title = raw.summary.isEmpty() ? String("(untitled)") : raw.summary;
  ev.organizer = raw.organizer;
  ev.location = raw.location;
  ev.description = raw.description;
  ev.linkJoinable = raw.linkJoinable;
  ev.urls = raw.urls;
  ev.start = start;
  ev.end = end;
  ev.allDay = allDay;
  ev.status = raw.status;
  ev.selfStatus = raw.selfStatus;
  ev.calIndex = calIndex_;
  out_.push_back(ev);
}

void IcsParser::endEvent() {
  if (raw_.dtStart.isEmpty()) return;

  bool isDate = false;
  DtParts parts;
  const time_t dtStart = parseDateTime(raw_.dtStart, raw_.dtStartParams, &isDate, &parts);
  if (dtStart == 0) return;

  long durationSec;
  if (!raw_.dtEnd.isEmpty()) {
    const time_t dtEnd = parseDateTime(raw_.dtEnd, raw_.dtEndParams);
    durationSec = (dtEnd > dtStart) ? (long)(dtEnd - dtStart) : (isDate ? 86400 : 3600);
  } else if (!raw_.duration.isEmpty()) {
    durationSec = parseIcsDuration(raw_.duration);
    if (durationSec <= 0) durationSec = isDate ? 86400 : 3600;
  } else {
    durationSec = isDate ? 86400 : 3600;
  }

  if (raw_.uid.isEmpty()) raw_.uid = raw_.summary + "@" + String((long long)dtStart);

  // Single-instance override of a recurring event.
  if (!raw_.recurrenceId.isEmpty()) {
    const time_t recId = parseDateTime(raw_.recurrenceId, raw_.recurrenceIdParams);
    if (recId == 0) return;
    // Keep only overrides that can matter: the moved instance lands in the
    // window, or it tombstones a base occurrence inside the window. Everything
    // else (historic edits) is dead weight that exhausts the heap on big feeds.
    const time_t ovEnd = dtStart + durationSec;
    const bool selfRelevant = ovEnd > windowStart_ && dtStart < windowEnd_;
    const bool baseRelevant = recId > windowStart_ - 86400 && recId < windowEnd_ + 86400;
    if (!selfRelevant && !baseRelevant) return;
    if (overrides_.size() >= MAX_OVERRIDES) {
      truncated_ = true;
      return;
    }
    Event ov;
    ov.uid = raw_.uid;
    ov.title = raw_.summary.isEmpty() ? String("(untitled)") : raw_.summary;
    ov.organizer = raw_.organizer;
    ov.location = raw_.location;
    ov.description = raw_.description;
    ov.linkJoinable = raw_.linkJoinable;
    ov.urls = raw_.urls;
    ov.start = dtStart;
    ov.end = dtStart + durationSec;
    ov.allDay = isDate;
    ov.status = raw_.status;
    ov.selfStatus = raw_.selfStatus;
    ov.calIndex = calIndex_;
    ov.isOverride = true;
    ov.recurrenceId = recId;
    overrides_.push_back(ov);
    return;
  }

  if (raw_.status == EventStatus::CANCELLED) return;

  if (raw_.rrule.isEmpty()) {
    emitOccurrence(raw_, dtStart, durationSec, isDate);
    return;
  }

  expandRecurrence(raw_, dtStart, parts, durationSec, isDate);
}

void IcsParser::expandRecurrence(const RawEvent& raw, time_t dtStart, const DtParts& parts,
                                 long durationSec, bool allDay) {
  Rrule rule = parseRrule(raw.rrule);
  if (!rule.valid) {
    emitOccurrence(raw, dtStart, durationSec, allDay);
    return;
  }

  const String untilRaw = extractRruleValue(raw.rrule, "UNTIL");
  if (!untilRaw.isEmpty()) {
    bool untilIsDate = false;
    rule.until = parseDateTime(untilRaw, "", &untilIsDate);
    if (untilIsDate && rule.until) rule.until += 86399;  // inclusive end of day
  }

  // All stepping is pure civil-date arithmetic in the event's own timezone;
  // each occurrence converts to epoch with that date's correct DST offset.
  const Civil& base = parts.civil;
  const long day0 = timeutil::daysFromCivil(base.y, base.mo, base.d);
  long remaining = rule.count;  // -1 = unbounded

  auto occurrenceAt = [&](long dayNum) -> time_t {
    Civil c = base;
    civilFromDays(dayNum, c.y, c.mo, c.d);
    return civilToEpoch(c, parts.vtzIndex, parts.utc);
  };

  auto emitIfDue = [&](time_t occStart) {
    if (remaining == 0) return false;
    if (rule.until && occStart > rule.until) return false;
    emitOccurrence(raw, occStart, durationSec, allDay);
    if (remaining > 0) --remaining;
    return true;
  };

  if (rule.freq == Rrule::DAILY || rule.freq == Rrule::WEEKLY) {
    uint8_t mask = rule.bydayMask;
    const int startDow = weekdayOfDayNum(day0);  // 0 = Monday
    if (rule.freq == Rrule::WEEKLY && mask == 0) mask = (1 << startDow);

    // Fast-forward toward the window when no COUNT bookkeeping is needed.
    long n = 0;
    if (rule.count < 0 && windowStart_ > dtStart) {
      const long daysAhead = (long)((windowStart_ - dtStart) / 86400) - 2;
      if (daysAhead > 0) {
        const long step = (rule.freq == Rrule::DAILY) ? rule.interval : 7L * rule.interval;
        n = (daysAhead / step) * step;
      }
    }

    for (int iter = 0; iter < MAX_DAY_ITERATIONS; ++iter, ++n) {
      const time_t occ = occurrenceAt(day0 + n);
      if (occ > windowEnd_ + 2 * 86400) break;
      if (rule.until && occ > rule.until) break;

      bool due;
      if (rule.freq == Rrule::DAILY) {
        due = (n % rule.interval) == 0;
      } else {
        const long weekIndex = (n + startDow) / 7;
        const int dow = (int)((startDow + n) % 7);
        due = (mask & (1 << dow)) && (weekIndex % rule.interval) == 0;
      }
      if (due && !emitIfDue(occ)) break;
      if (remaining == 0) break;
    }
    return;
  }

  if (rule.freq == Rrule::MONTHLY) {
    int targetWd = -1;  // 0 = Monday
    if (rule.bydayMask) {
      for (int i = 0; i < 7; ++i) {
        if (rule.bydayMask & (1 << i)) targetWd = i;
      }
    }
    for (int m = 0; m < MAX_MONTH_ITERATIONS; m += rule.interval) {
      int year = base.y;
      int month = base.mo + m;
      year += (month - 1) / 12;
      month = ((month - 1) % 12) + 1;

      int day;
      const int dim = daysInMonth(year, month);
      if (targetWd >= 0) {
        const long firstDayNum = timeutil::daysFromCivil(year, month, 1);
        const int firstWd = weekdayOfDayNum(firstDayNum);
        if (rule.bydayOrd >= 0) {
          const int ord = rule.bydayOrd > 0 ? rule.bydayOrd : 1;
          day = 1 + ((targetWd - firstWd + 7) % 7) + (ord - 1) * 7;
        } else {
          const int lastWd = (firstWd + dim - 1) % 7;
          day = dim - ((lastWd - targetWd + 7) % 7) + (rule.bydayOrd + 1) * 7;
        }
      } else if (rule.bymonthday != 0) {
        day = rule.bymonthday > 0 ? rule.bymonthday : dim + 1 + rule.bymonthday;
      } else {
        day = base.d;
      }
      if (day < 1 || day > dim) continue;

      Civil c = base;
      c.y = year;
      c.mo = month;
      c.d = day;
      const time_t occ = civilToEpoch(c, parts.vtzIndex, parts.utc);
      if (occ < dtStart) continue;
      if (occ > windowEnd_ + 2 * 86400) break;
      if (rule.until && occ > rule.until) break;
      if (!emitIfDue(occ)) break;
      if (remaining == 0) break;
    }
    return;
  }

  // YEARLY
  for (int yOff = 0; yOff < MAX_YEAR_ITERATIONS; yOff += rule.interval) {
    const int year = base.y + yOff;
    if (base.d > daysInMonth(year, base.mo)) continue;  // Feb 29 on non-leap years
    Civil c = base;
    c.y = year;
    const time_t occ = civilToEpoch(c, parts.vtzIndex, parts.utc);
    if (occ > windowEnd_ + 2 * 86400) break;
    if (rule.until && occ > rule.until) break;
    if (!emitIfDue(occ)) break;
    if (remaining == 0) break;
  }
}
