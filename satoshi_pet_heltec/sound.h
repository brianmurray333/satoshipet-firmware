#ifndef SOUND_H
#define SOUND_H

#include <Arduino.h>

// ============================================================================
// Non-blocking buzzer sequencer
// ============================================================================

struct ToneNote {
  uint16_t freq;  // Hz; 0 = rest
  uint16_t ms;    // duration
};

// Start playing a note sequence, replacing whatever is playing. Non-blocking:
// call soundUpdate() regularly (every loop iteration) to advance it.
// The array is NOT copied: `notes` must stay valid until the sequence ends
// (pass static const arrays). When respectQuietHours is true and
// isQuietHours() is true, nothing plays (any current sequence is left alone).
void soundPlay(const ToneNote* notes, uint8_t count, bool respectQuietHours = true);

// Advance the sequencer based on millis(). Cheap; call every loop.
void soundUpdate();

// True while a sequence is playing.
bool soundBusy();

// Stop the current sequence immediately and silence the buzzer.
void soundStop();

// Play a sequence and wait for it to finish (feeds the task watchdog).
void soundPlayBlocking(const ToneNote* notes, uint8_t count, bool respectQuietHours = true);

#endif
