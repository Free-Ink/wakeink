#pragma once

// Streams an ICS feed over HTTP(S) into a sink callback, following redirects.
// Uses esp_http_client with the ESP-IDF CA root bundle for TLS verification —
// the same pattern as crosspoint-reader's HttpDownloader (Arduino's
// WiFiClientSecure is avoided; the SDK's precompiled mbedTLS ships TLS 1.3 as
// stubs, but esp_http_client + the cert bundle negotiates TLS 1.2 fine with
// calendar.google.com).

#include <Arduino.h>

#include <functional>

namespace icsfetch {

// sink returns false to abort the transfer.
using Sink = std::function<bool(const char* data, size_t len)>;

enum Result {
  OK = 0,
  CONNECT_FAILED,
  HTTP_ERROR,       // non-2xx status
  ABORTED,
};

Result fetch(const String& url, const Sink& sink, int* httpStatus = nullptr);

}  // namespace icsfetch
