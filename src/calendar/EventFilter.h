#pragma once

// Direct port of In Your Space's AlarmSyncService filter chain. An event must
// pass every gate to appear on the display and to ring an alarm:
//
//   1. all-day events are dropped
//   2. cancelled events are dropped
//   3. declined events are dropped
//   4. unaccepted (needs-action) events are dropped unless
//      alarm_on_unaccepted_events is on — events with no RSVP info at all
//      (your own / solo events) count as accepted
//   5. "maybe" events are dropped when show_maybe_events is off
//   6. manually skipped instances are dropped
//   7. any ignore_keyword found in the title drops the event
//   8. with only_alert_with_links on, events without a known meeting link
//      (zoom/meet/teams/webex/...) are dropped — unless alert_with_any_link
//      counts any URL not on ignore_link_domains
//   9. events outside work hours are dropped when work_hours_enabled, unless
//      an always_trigger_keyword matches title/description

#include "../config/AppSettings.h"
#include "../config/StateStore.h"
#include "Event.h"

namespace eventfilter {

// trustStoredLink: re-filtering after a settings change runs on events whose
// description was dropped at sync time — ev.hasLink carries the original
// meeting-link verdict.
bool passes(const Event& ev, const AppSettings& s, const StateStore& state,
            bool trustStoredLink = false);

// Lead time for this event's alarm, honoring the early-meeting override.
int leadTimeMinutes(const Event& ev, const AppSettings& s);

// Exposed for the web UI's status output.
bool hasJoinableLink(const Event& ev);

// Parse-time helpers (the parser scans FULL property text before truncation):
// known meeting-platform domain anywhere in lowercased text.
bool textHasMeetingDomain(const String& lowerText);
// Append up to maxUrls http(s) URLs (lowercased, length-capped) found in text.
void collectUrls(const String& lowerText, std::vector<String>& out, size_t maxUrls);

}  // namespace eventfilter
