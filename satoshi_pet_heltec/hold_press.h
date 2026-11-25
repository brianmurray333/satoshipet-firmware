#ifndef HOLD_PRESS_H
#define HOLD_PRESS_H

#include <Arduino.h>
#include "HT_SSD1306Wire.h"

// Tap / hold input for the pick-from-a-list screens (menu, games, feed,
// jobs, pickleball wager).
//
// - Tap: released before HOLD_SELECT_MS. Reported on release.
// - Hold: reported the moment the press reaches HOLD_SELECT_MS, without
//   waiting for release. A line fills across the bottom of the screen while
//   the button is down so the user can see when it will count. The rest of
//   that press is ignored, so it does not also count on the next screen.

#define HOLD_SELECT_MS 700

enum PressEvent : uint8_t { PRESS_NONE = 0, PRESS_TAP, PRESS_HOLD };

// Feed the current (debounced) button level every loop pass.
PressEvent holdPressUpdate(bool down);

// Ignore the press that is down now, until it is released. Call when a hold
// was acted on outside holdPressUpdate (the main menu does this).
void holdPressSwallow();

// A counted press is down (not swallowed); elapsed is its age in ms.
bool holdPressActive();
unsigned long holdPressElapsed();

// Draw the hold line for `elapsedMs` along the bottom two rows. With flush,
// pushes the frame (throttled); without, the caller calls display().
void drawHoldBar(SSD1306Wire &display, unsigned long elapsedMs, bool flush = true);

// Draws the bar for the press tracked by holdPressUpdate, if one is down.
void holdPressDrawBar(SSD1306Wire &display, bool flush = true);

// "Tap: next  Hold: pick" style hints show until the user has completed a
// few holds; the count is kept in NVS so it survives reboots.
bool holdHintsVisible();
void holdHintsNoteHold();

#endif
