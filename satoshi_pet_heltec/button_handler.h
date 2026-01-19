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

// Button state tracking
struct ButtonState {
  bool pressed;
  unsigned long pressTime;
  unsigned long lastPress;
  bool prgPressed;
  bool externalPressed;
};

// Initialize button pins
void initButtons();

// Update button state (call every loop iteration)
// Returns true if button state changed
bool updateButtonState(ButtonState& state);

// Check if button was just pressed (for logging)
bool wasButtonJustPressed(const ButtonState& state);

// Check if button was just released
bool wasButtonJustReleased(const ButtonState& state);

// Get press duration in milliseconds
unsigned long getPressDuration(const ButtonState& state);

// Check press type
bool isShortPress(unsigned long duration);
bool isHoldPress(unsigned long duration);
bool isVeryLongPress(unsigned long duration);

// Check which button was pressed (for logging)
bool wasPrgPressed(const ButtonState& state);
bool wasExternalPressed(const ButtonState& state);

#endif

