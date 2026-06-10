#include "IcsFetch.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstdlib>
#include <cstring>

#include "uzlib.h"

namespace icsfetch {

namespace {
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Big Google feeds stream slowly; 30s per socket op killed them with EAGAIN.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;
constexpr int MAX_REDIRECT_HOPS = 5;
constexpr size_t GZIP_WINDOW = 32768;  // gzip's max back-reference distance
constexpr size_t GZIP_OUT_CHUNK = 4096;

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Google serves basic.ics gzip-compressed when asked (8.7 MB -> ~1.4 MB on a
// real calendar), and the endpoint supports neither Range nor conditional
// requests — so on-device streaming inflate is the only way to shrink the
// transfer. The stream is sniffed by magic bytes rather than trusting the
// Content-Encoding header.
struct GzipStream {
  struct uzlib_uncomp d;  // must stay first: the refill callback casts back
  esp_http_client_handle_t client;
  uint8_t magic[2];
  uint8_t inBuf[READ_CHUNK];
  bool readError;
};

// RFC 1952 gzip header walk (the vendored uzlib omits upstream's tinfgzip.c).
// The two magic bytes were already consumed by the sniffer; the deflate stream
// follows the optional fields. CRC trailer is not verified (checksum NONE) —
// the ICS parser tolerates a torn tail and the transport is already TLS.
bool parseGzipHeader(struct uzlib_uncomp* d) {
  enum { FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16 };
  const uint8_t cm = uzlib_get_byte(d);
  const uint8_t flg = uzlib_get_byte(d);
  if (cm != 8 || (flg & 0xE0)) return false;  // deflate only; reserved bits clear
  for (int i = 0; i < 6; ++i) uzlib_get_byte(d);  // MTIME, XFL, OS
  if (flg & FEXTRA) {
    uint16_t len = uzlib_get_byte(d);
    len |= (uint16_t)uzlib_get_byte(d) << 8;
    while (len--) uzlib_get_byte(d);
  }
  if (flg & FNAME) {
    while (uzlib_get_byte(d) != 0) {}
  }
  if (flg & FCOMMENT) {
    while (uzlib_get_byte(d) != 0) {}
  }
  if (flg & FHCRC) {
    uzlib_get_byte(d);
    uzlib_get_byte(d);
  }
  d->checksum_type = TINF_CHKSUM_NONE;
  return !d->eof;  // EOF mid-header (uzlib_get_byte is sticky-EOF) = bad stream
}

int gzipRefill(struct uzlib_uncomp* uc) {
  GzipStream* s = (GzipStream*)uc;
  // esp_http_client_read returns 0 on a read *timeout*, not only at end of
  // stream — treating that as EOF feeds uzlib a truncated stream (the cause of
  // the inflate-desync corruption crashes). Retry 0-returns until the client
  // confirms the body is complete; only then is it a real EOF.
  for (int attempt = 0; attempt < 200; ++attempt) {
    const int n = esp_http_client_read(s->client, (char*)s->inBuf, sizeof(s->inBuf));
    if (n > 0) {
      uc->source = s->inBuf + 1;
      uc->source_limit = s->inBuf + n;
      return s->inBuf[0];
    }
    if (n < 0) {
      s->readError = true;
      return -1;
    }
    // n == 0: real EOF if the transfer is done, else a transient timeout.
    if (esp_http_client_is_complete_data_received(s->client)) return -1;  // genuine EOF
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  s->readError = true;  // stuck too long; abort cleanly rather than spin
  return -1;
}

Result readGzipBody(esp_http_client_handle_t client, const Sink& sink) {
  static bool uzlibInited = false;
  if (!uzlibInited) {
    uzlib_init();
    uzlibInited = true;
  }

  GzipStream* s = (GzipStream*)calloc(1, sizeof(GzipStream));
  // The inflate dictionary is read/written for every output byte — keep it in
  // internal RAM (PSRAM showed corruption under sustained traffic; the boot
  // self-test in main.cpp tracks that). Falls back to default heap if internal
  // is tight.
  uint8_t* window = (uint8_t*)heap_caps_malloc(GZIP_WINDOW, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!window) window = (uint8_t*)malloc(GZIP_WINDOW);
  uint8_t* outBuf = (uint8_t*)malloc(GZIP_OUT_CHUNK);
  if (!s || !window || !outBuf) {
    free(s);
    free(window);
    free(outBuf);
    return CONNECT_FAILED;
  }

  s->client = client;
  uzlib_uncompress_init(&s->d, window, GZIP_WINDOW);
  // Magic bytes were consumed by the sniffer; the header walk starts at CM.
  s->d.source = nullptr;
  s->d.source_limit = nullptr;
  s->d.source_read_cb = gzipRefill;
  (void)s->magic;

  Result result = OK;
  if (!parseGzipHeader(&s->d)) {
    result = CONNECT_FAILED;
  } else {
    // Inflating + parsing 8+ MB is a multi-second CPU-bound burst on one core.
    // Decompressing a small gzip means few blocking socket reads, so nothing
    // else yields — the idle task starves and the task watchdog resets the
    // chip (reset_reason=6). Yield every few chunks to keep it fed.
    int chunks = 0;
    while (true) {
      s->d.dest = outBuf;
      s->d.dest_limit = outBuf + GZIP_OUT_CHUNK;
      const int res = uzlib_uncompress(&s->d);
      const size_t produced = (size_t)(s->d.dest - outBuf);
      if (produced && !sink((const char*)outBuf, produced)) {
        result = ABORTED;
        break;
      }
      if (res == TINF_DONE) break;
      if (res != TINF_OK || s->readError) {
        result = CONNECT_FAILED;  // truncated stream or corrupt deflate data
        break;
      }
      if ((++chunks & 7) == 0) vTaskDelay(1);  // ~every 32 KB inflated
    }
  }

  free(s);
  free(window);
  free(outBuf);
  return result;
}

Result readRawBody(esp_http_client_handle_t client, const uint8_t* head, size_t headLen,
                   const Sink& sink) {
  if (headLen && !sink((const char*)head, headLen)) return ABORTED;
  char buf[READ_CHUNK];
  while (true) {
    const int n = esp_http_client_read(client, buf, sizeof(buf));
    if (n < 0) return CONNECT_FAILED;
    if (n == 0) return OK;
    if (!sink(buf, (size_t)n)) return ABORTED;
  }
}

}  // namespace

Result fetch(const String& url, const Sink& sink, int* httpStatus) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return CONNECT_FAILED;

  esp_http_client_set_header(client, "User-Agent", "WakeInk-ESP32/" WAKEINK_VERSION);
  esp_http_client_set_header(client, "Accept", "text/calendar");
  esp_http_client_set_header(client, "Accept-Encoding", "gzip");

  // open()/read() does not auto-follow redirects; step 30x manually (Google's
  // ICS endpoints redirect).
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return CONNECT_FAILED;
  }
  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECT_HOPS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      esp_http_client_cleanup(client);
      return CONNECT_FAILED;
    }
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }
  if (httpStatus) *httpStatus = status;

  Result result;
  if (status < 200 || status >= 300) {
    result = HTTP_ERROR;
  } else {
    // Sniff the first two body bytes for the gzip magic.
    uint8_t magic[2];
    size_t got = 0;
    while (got < 2) {
      const int n = esp_http_client_read(client, (char*)magic + got, 2 - got);
      if (n <= 0) break;
      got += (size_t)n;
    }
    if (got == 2 && magic[0] == 0x1f && magic[1] == 0x8b) {
      result = readGzipBody(client, sink);
    } else {
      result = readRawBody(client, magic, got, sink);
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result;
}

}  // namespace icsfetch
