#include "sound.h"
#include <esp_task_wdt.h>

#ifndef BUZZER_PIN
#define BUZZER_PIN 48  // Piezo buzzer pin (matches satoshi_pet_heltec.ino / pet_blob.h)
#endif

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
    tone(BUZZER_PIN, freq);
  } else {
    noTone(BUZZER_PIN);
  }
}

void soundStop() {
  if (s_active) noTone(BUZZER_PIN);
  s_active = false;
  s_notes = nullptr;
  s_count = 0;
  s_index = 0;
}

void soundPlay(const ToneNote* notes, uint8_t count, bool respectQuietHours) {
  if (!notes || count == 0) return;
  if (respectQuietHours && isQuietHours()) return;
  if (s_active) noTone(BUZZER_PIN);
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
