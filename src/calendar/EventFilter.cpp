#include "EventFilter.h"

#include "TimeUtil.h"

namespace eventfilter {

static const char* KNOWN_MEETING_DOMAINS[] = {
    "zoom.us",          "meet.google.com", "teams.microsoft.com", "teams.live.com",
    "webex.com",        "gotomeeting.com", "bluejeans.com",
};

static String lower(const String& s) {
  String out = s;
  out.toLowerCase();
  return out;
}

static bool containsKeyword(const String& haystackLower, const std::vector<String>& keywords) {
  for (const auto& kw : keywords) {
    String k = kw;
    k.trim();
    k.toLowerCase();
    if (!k.isEmpty() && haystackLower.indexOf(k) >= 0) return true;
  }
  return false;
}

bool hasJoinableLink(const Event& ev) {
  const String loc = lower(ev.location);
  const String desc = lower(ev.description);
  for (const char* domain : KNOWN_MEETING_DOMAINS) {
    if (loc.indexOf(domain) >= 0 || desc.indexOf(domain) >= 0) return true;
  }
  return false;
}

// Scans for http(s):// URLs and returns true if any of them is NOT on the
// ignore list (mirrors the app's hasAnyLink regex walk).
static bool hasAnyCountedLink(const Event& ev, const std::vector<String>& ignoreDomains) {
  const String texts[2] = {lower(ev.location), lower(ev.description)};
  for (const String& text : texts) {
    int pos = 0;
    while (true) {
      int idx = text.indexOf("http://", pos);
      int idxS = text.indexOf("https://", pos);
      if (idx < 0 || (idxS >= 0 && idxS < idx)) idx = idxS;
      if (idx < 0) break;
      int end = idx;
      while (end < (int)text.length()) {
        const char c = text[end];
        if (c == ' ' || c == '\t' || c == '<' || c == '>' || c == '"' || c == ')' || c == ']' ||
            c == '\n') {
          break;
        }
        ++end;
      }
      const String url = text.substring(idx, end);
      bool ignored = false;
      for (const auto& domRaw : ignoreDomains) {
        String dom = domRaw;
        dom.trim();
        dom.toLowerCase();
        if (!dom.isEmpty() && url.indexOf(dom) >= 0) {
          ignored = true;
          break;
        }
      }
      if (!ignored) return true;
      pos = end;
    }
  }
  return false;
}

static bool isOutsideWorkHours(const AppSettings& s, time_t eventStart) {
  if (!s.workHoursEnabled) return false;

  struct tm local;
  localtime_r(&eventStart, &local);

  const int day = timeutil::isoWeekday(local);
  bool workDay = false;
  for (int d : s.workDays) {
    if (d == day) workDay = true;
  }
  if (!workDay) return true;

  const int eventMinutes = local.tm_hour * 60 + local.tm_min;
  const int startMinutes = s.workHoursStartHour * 60 + s.workHoursStartMinute;
  const int endMinutes = s.workHoursEndHour * 60 + s.workHoursEndMinute;
  return eventMinutes < startMinutes || eventMinutes >= endMinutes;
}

static bool matchesAlwaysTriggerKeyword(const AppSettings& s, const Event& ev) {
  if (s.alwaysTriggerKeywords.empty()) return false;
  const String title = lower(ev.title);
  const String desc = lower(ev.description);
  for (const auto& kwRaw : s.alwaysTriggerKeywords) {
    String kw = kwRaw;
    kw.trim();
    kw.toLowerCase();
    if (!kw.isEmpty() && (title.indexOf(kw) >= 0 || desc.indexOf(kw) >= 0)) return true;
  }
  return false;
}

bool passes(const Event& ev, const AppSettings& s, const StateStore& state,
            bool trustStoredLink) {
  // 1. all-day
  if (ev.allDay) return false;

  // 2. cancelled
  if (ev.status == EventStatus::CANCELLED) return false;

  // 3. declined
  if (ev.selfStatus == SelfStatus::DECLINED) return false;

  // 4. unaccepted (UNKNOWN = no attendee info = your own event = accepted)
  if (!s.alarmOnUnacceptedEvents &&
      (ev.selfStatus == SelfStatus::NONE || ev.selfStatus == SelfStatus::INVITED)) {
    return false;
  }

  // 5. maybe
  if (ev.selfStatus == SelfStatus::TENTATIVE && !s.showMaybeEvents) return false;

  // 6. manually skipped
  if (state.isSkipped(ev.skipKey())) return false;

  // 7. ignore keywords (title only, like the app)
  if (containsKeyword(lower(ev.title), s.ignoreKeywords)) return false;

  // 8. link filtering
  if (s.onlyAlertWithLinks) {
    const bool joinable = trustStoredLink ? ev.hasLink : hasJoinableLink(ev);
    const bool anyCounted = s.alertWithAnyLink && hasAnyCountedLink(ev, s.ignoreLinkDomains);
    if (!joinable && !anyCounted) return false;
  }

  // 9. work hours (always-trigger keywords bypass)
  if (isOutsideWorkHours(s, ev.start) && !matchesAlwaysTriggerKeyword(s, ev)) return false;

  return true;
}

int leadTimeMinutes(const Event& ev, const AppSettings& s) {
  if (!s.earlyMeetingAlertEnabled) return s.alarmLeadTimeMinutes;

  struct tm local;
  localtime_r(&ev.start, &local);
  const int eventMinutes = local.tm_hour * 60 + local.tm_min;
  const int threshold = s.earlyMeetingBeforeHour * 60 + s.earlyMeetingBeforeMinute;
  return eventMinutes < threshold ? s.earlyMeetingLeadTimeMinutes : s.alarmLeadTimeMinutes;
}

}  // namespace eventfilter
