#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <Arduino.h>

// Button pin definitions
#ifndef BUTTON_PIN_PRG
#define BUTTON_PIN_PRG 0      // Heltec PRG button (GPIO0)
#endif
#ifndef BUTTON_PIN_EXTERNAL
#define BUTTON_PIN_EXTERNAL 2 // External button (GPIO2)
#endif

// Press duration thresholds are defined in satoshi_pet_heltec.ino
// SHORT_PRESS_MAX = 600ms, HOLD_PRESS_MIN = 700ms, VERY_LONG_PRESS = 10000ms

// Initialize button pins
void initButtons();

// ---- Debounced button (active-low, INPUT_PULLUP) ----
// Plain aggregate on purpose (no default member initializers / methods):
// call debounceInit() before use.
#define DEBOUNCE_STABLE_MS 20

struct DebouncedButton {
  uint8_t pin;
  bool stableDown;          // debounced state: true = pressed
  bool lastRaw;             // last raw sample: true = pressed
  unsigned long changedAt;  // millis() of the last raw change
};

// Configures the pin as INPUT_PULLUP and adopts its current level as the
// stable state, so a button already held at init does not report a press.
void debounceInit(DebouncedButton& b, uint8_t pin);

// Sample the pin. Returns +1 on a debounced press edge, -1 on a debounced
// release edge, 0 otherwise. A level must be stable ~DEBOUNCE_STABLE_MS.
int debounceUpdate(DebouncedButton& b, unsigned long now);

// Current debounced state (true = pressed)
bool debounceIsDown(const DebouncedButton& b);

#endif
