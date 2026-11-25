#include "hold_press.h"
#include <Preferences.h>

namespace {
  bool pressDown = false;        // a counted press is in progress
  bool swallow = false;          // ignore the current press until release
  unsigned long pressStart = 0;

  const uint8_t HINT_HOLDS = 3;  // hide hints after this many completed holds
  int8_t holdsDone = -1;         // -1 = not loaded from NVS yet

  void loadHolds() {
    if (holdsDone >= 0) return;
    holdsDone = 0;
    Preferences p;
    if (p.begin("sp-ui", true)) {
      holdsDone = (int8_t)min<uint8_t>(p.getUChar("holdsDone", 0), HINT_HOLDS);
      p.end();
    }
  }
}

PressEvent holdPressUpdate(bool down) {
  if (swallow) {
    if (!down) swallow = false;
    return PRESS_NONE;
  }
  if (down && !pressDown) {
    pressDown = true;
    pressStart = millis();
    return PRESS_NONE;
  }
  if (!pressDown) return PRESS_NONE;
  if (!down) {
    pressDown = false;
    return PRESS_TAP;
  }
  if (millis() - pressStart >= HOLD_SELECT_MS) {
    pressDown = false;
    swallow = true;
    holdHintsNoteHold();
    return PRESS_HOLD;
  }
  return PRESS_NONE;
}

void holdPressSwallow() {
  pressDown = false;
  swallow = true;
}

bool holdPressActive() { return pressDown; }

unsigned long holdPressElapsed() { return pressDown ? millis() - pressStart : 0; }

void drawHoldBar(SSD1306Wire &display, unsigned long elapsedMs, bool flush) {
  static unsigned long lastFlush = 0;
  unsigned long now = millis();
  if (flush && now - lastFlush < 30) return;
  int w = (int)min<unsigned long>(128UL, elapsedMs * 128UL / HOLD_SELECT_MS);
  display.setColor(BLACK);
  display.fillRect(0, 62, 128, 2);
  display.setColor(WHITE);
  display.fillRect(0, 62, w, 2);
  if (flush) {
    display.display();
    lastFlush = now;
  }
}

void holdPressDrawBar(SSD1306Wire &display, bool flush) {
  if (pressDown) drawHoldBar(display, millis() - pressStart, flush);
}

bool holdHintsVisible() {
  loadHolds();
  return holdsDone < HINT_HOLDS;
}

void holdHintsNoteHold() {
  loadHolds();
  if (holdsDone >= HINT_HOLDS) return;   // at most HINT_HOLDS flash writes, ever
  holdsDone++;
  Preferences p;
  if (p.begin("sp-ui", false)) {
    p.putUChar("holdsDone", (uint8_t)holdsDone);
    p.end();
  }
}
