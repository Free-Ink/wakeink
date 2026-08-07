#pragma once

// Alarm sound selection + playback glue. Plays the user-uploaded
// /alarm.wav from LittleFS when present, otherwise the embedded default
// (assets/sounds/default_alarm.wav), looping until stopped.

#include <Arduino.h>
#include <AudioManager.h>

namespace alarmsound {

constexpr const char* CUSTOM_PATH = "/alarm.wav";

AudioManager& audio();

bool hasCustom();

// Starts looped playback of the active sound at the given volume. On boards
// with no output codec but a PWM buzzer (Paper Mono), falls back to a
// continuous alarm tone instead of WAV playback.
bool startLoop(uint8_t volumePercent);
void stop();

// Buzzer-path alarm beat: alternates the tone pitch so the alarm warbles
// instead of droning. Driven from the alarm strobe timer in loop(); no-op
// while WAV playback is active (or nothing is ringing).
void beat(bool phase);

// Checks that a candidate upload is a playable WAV (16-bit PCM, 1-2 ch,
// 8-48 kHz). Returns false with a human-readable reason.
bool validateWavFile(const char* path, String& error);

}  // namespace alarmsound
