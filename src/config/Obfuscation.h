#pragma once

// WiFi passwords are XOR-obfuscated with the device MAC and base64-encoded
// before hitting flash — same scheme as crosspoint-reader. Not encryption,
// just keeps credentials out of casual flash dumps.

#include <Arduino.h>

namespace obfuscation {

String obfuscateToBase64(const String& plain);
String deobfuscateFromBase64(const String& encoded, bool* ok = nullptr);

}  // namespace obfuscation
