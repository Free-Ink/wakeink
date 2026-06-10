#pragma once

// Timezone choices for the on-device picker: IANA label + POSIX TZ string.
// Mirrors the web dashboard's TIMEZONES table (src/web/html/Index.html) —
// keep the two in sync when editing.

#include <Arduino.h>

struct TzEntry {
  const char* name;   // IANA-style label shown to the user
  const char* posix;  // POSIX TZ string applied to the clock
};

constexpr TzEntry TIMEZONES[] = {
    {"Pacific/Honolulu", "HST10"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Tijuana", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Detroit", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Bogota", "<-05>5"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"UTC", "UTC0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Taipei", "CST-8"},
    {"Australia/Perth", "AWST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr size_t TIMEZONE_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);
