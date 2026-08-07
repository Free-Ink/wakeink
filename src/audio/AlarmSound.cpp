#include "AlarmSound.h"

#include <BoardConfig.h>
#include <Buzzer.h>
#include <LittleFS.h>

#include "DefaultAlarmWav.generated.h"

namespace alarmsound {

namespace {
File g_file;  // kept open for the duration of file-backed playback

// Buzzer fallback (no codec on board): a two-pitch warble, advanced by beat().
Buzzer g_buzzer;
bool g_buzzing = false;
constexpr uint32_t BUZZ_HZ_A = 880;
constexpr uint32_t BUZZ_HZ_B = 1320;
}  // namespace

AudioManager& audio() {
  static AudioManager instance;
  return instance;
}

bool hasCustom() { return LittleFS.exists(CUSTOM_PATH); }

bool startLoop(uint8_t volumePercent) {
  AudioManager& am = audio();
  if (!am.begin()) {
    // No codec: ring the PWM buzzer instead (LEDC square wave; a passive
    // beeper has no volume control, so only volume 0 silences it).
    if (volumePercent > 0 && BoardConfig::hasBuzzer() && g_buzzer.begin()) {
      g_buzzer.tone(BUZZ_HZ_A, 0);
      g_buzzing = true;
      return true;
    }
    return false;
  }
  am.setVolume(volumePercent);

  if (hasCustom()) {
    if (g_file) g_file.close();
    g_file = LittleFS.open(CUSTOM_PATH, "r");
    if (g_file) {
      AudioManager::WavSource src;
      src.read = [](uint8_t* dst, size_t len) -> int {
        return g_file ? (int)g_file.read(dst, len) : -1;
      };
      src.seek = [](size_t pos) { return g_file && g_file.seek(pos); };
      if (am.play(src, /*loop=*/true)) return true;
      g_file.close();
      // Bad custom file: fall through to the default sound.
    }
  }
  return am.playBuffer(DefaultAlarmWav, DefaultAlarmWavSize, /*loop=*/true);
}

void stop() {
  if (g_buzzing) {
    g_buzzer.noTone();
    g_buzzing = false;
  }
  audio().stop();
  if (g_file) g_file.close();
}

void beat(bool phase) {
  if (!g_buzzing) return;
  g_buzzer.tone(phase ? BUZZ_HZ_A : BUZZ_HZ_B, 0);
}

bool validateWavFile(const char* path, String& error) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    error = "cannot open upload";
    return false;
  }

  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    f.close();
    error = "not a WAV file";
    return false;
  }

  bool ok = false;
  size_t pos = 12;
  for (int guard = 0; guard < 32; ++guard) {
    uint8_t chunk[8];
    if (!f.seek(pos) || f.read(chunk, 8) != 8) break;
    const uint32_t size = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) |
                          ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
    pos += 8;
    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || f.read(fmt, 16) != 16) break;
      const uint16_t audioFormat = fmt[0] | (fmt[1] << 8);
      const uint16_t channels = fmt[2] | (fmt[3] << 8);
      const uint32_t rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) |
                            ((uint32_t)fmt[7] << 24);
      const uint16_t bits = fmt[14] | (fmt[15] << 8);
      if (audioFormat != 1) {
        error = "compressed WAV not supported (need 16-bit PCM)";
        break;
      }
      if (bits != 16) {
        error = "need 16-bit samples (got " + String(bits) + "-bit)";
        break;
      }
      if (channels != 1 && channels != 2) {
        error = "need mono or stereo";
        break;
      }
      if (rate < 8000 || rate > 48000) {
        error = "sample rate must be 8-48 kHz";
        break;
      }
      ok = true;
      break;
    }
    pos += size + (size & 1);
  }
  f.close();
  if (!ok && error.isEmpty()) error = "missing fmt chunk";
  return ok;
}

}  // namespace alarmsound
