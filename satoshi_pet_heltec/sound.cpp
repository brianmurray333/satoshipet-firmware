#include "sound.h"
#include <esp_task_wdt.h>

#ifndef BUZZER_PIN
#define BUZZER_PIN 48  // Piezo buzzer pin (matches satoshi_pet_heltec.ino / pet_blob.h)
#endif

// Case v2 wiring: the buzzer is soldered to the first two holes at the board's
// keyring end, one driven with the tone and one held LOW as its ground, so
// neither lead runs to the GND pin at the USB-C end. Which GPIOs those holes
// are depends on the board:
//   WiFi Kit 32 V3.1:   GPIO 46 (tone) + GPIO 45 (low)
//   WiFi LoRa 32 V3.2:  GPIO 7  (tone) + GPIO 6  (low)
// Earlier pets wire it to GPIO 48 + GND. All tone outputs share one LEDC
// channel, so one build drives any of these wirings.
// (45 and 46 are strapping pins: both default LOW at reset, which is what they
// need for a normal boot, and the buzzer coil between them keeps them equal.
// 6 and 7 are plain GPIOs, unused on both boards.)
static const uint8_t BUZZER_TONE_PINS[] = {BUZZER_PIN, 46, 7};
static const uint8_t BUZZER_LOW_PINS[]  = {45, 6};
#define BUZZER_LEDC_CHANNEL 7
#define BUZZER_LEDC_BITS    10

static bool s_init = false;

void soundInit() {
  if (s_init) return;
  for (uint8_t pin : BUZZER_LOW_PINS) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
  for (uint8_t pin : BUZZER_TONE_PINS) {
    ledcAttachChannel(pin, 2000, BUZZER_LEDC_BITS, BUZZER_LEDC_CHANNEL);
  }
  ledcWrite(BUZZER_PIN, 0);  // silent: both outputs low
  s_init = true;
}

static void buzzerTone(uint16_t freq) {
  if (!s_init) soundInit();
  ledcWriteTone(BUZZER_PIN, freq);  // the channel drives both outputs
}

static void buzzerOff() {
  if (!s_init) return;
  ledcWrite(BUZZER_PIN, 0);
}

extern bool isQuietHours();  // defined in satoshi_pet_heltec.ino

static const ToneNote* s_notes = nullptr;
static uint8_t s_count = 0;
static uint8_t s_index = 0;
static unsigned long s_noteStart = 0;
static bool s_active = false;

static void startNote(uint8_t index) {
  s_index = index;
  s_noteStart = millis();
  uint16_t freq = s_notes[index].freq;
  if (freq > 0) {
    buzzerTone(freq);
  } else {
    buzzerOff();
  }
}

void soundStop() {
  if (s_active) buzzerOff();
  s_active = false;
  s_notes = nullptr;
  s_count = 0;
  s_index = 0;
}

void soundPlay(const ToneNote* notes, uint8_t count, bool respectQuietHours) {
  if (!notes || count == 0) return;
  if (respectQuietHours && isQuietHours()) return;
  if (s_active) buzzerOff();
  s_notes = notes;
  s_count = count;
  s_active = true;
  startNote(0);
}

void soundUpdate() {
  if (!s_active) return;
  unsigned long now = millis();
  if (now - s_noteStart < s_notes[s_index].ms) return;
  uint8_t next = s_index + 1;
  if (next >= s_count) {
    soundStop();
    return;
  }
  startNote(next);
}

bool soundBusy() {
  return s_active;
}

void soundPlayBlocking(const ToneNote* notes, uint8_t count, bool respectQuietHours) {
  soundPlay(notes, count, respectQuietHours);
  while (s_active) {
    soundUpdate();
    esp_task_wdt_reset();
    delay(2);
  }
}
