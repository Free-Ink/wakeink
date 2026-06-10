#include "Obfuscation.h"

#include <base64.h>
#include <esp_mac.h>

#include "mbedtls/base64.h"

namespace obfuscation {

static void xorWithMac(uint8_t* data, size_t len) {
  // esp_read_mac works before WiFi is initialized (unlike WiFi.macAddress),
  // so load-at-boot and save-from-web always use the same key.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  for (size_t i = 0; i < len; ++i) {
    data[i] ^= mac[i % 6];
  }
}

String obfuscateToBase64(const String& plain) {
  if (plain.isEmpty()) return String();
  String buf = plain;
  xorWithMac((uint8_t*)buf.begin(), buf.length());
  return base64::encode((const uint8_t*)buf.c_str(), buf.length());
}

String deobfuscateFromBase64(const String& encoded, bool* ok) {
  if (ok) *ok = false;
  if (encoded.isEmpty()) return String();

  size_t outLen = 0;
  const size_t bufSize = encoded.length();  // decoded is always shorter
  uint8_t* buf = (uint8_t*)malloc(bufSize + 1);
  if (!buf) return String();

  const int rc = mbedtls_base64_decode(buf, bufSize, &outLen, (const uint8_t*)encoded.c_str(),
                                       encoded.length());
  if (rc != 0) {
    free(buf);
    return String();
  }
  xorWithMac(buf, outLen);
  buf[outLen] = 0;
  String result((const char*)buf);
  free(buf);
  if (ok) *ok = true;
  return result;
}

}  // namespace obfuscation
