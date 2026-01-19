#include "button_handler.h"

// These are defined in satoshi_pet_heltec.ino
extern const unsigned long SHORT_PRESS_MAX;
extern const unsigned long HOLD_PRESS_MIN;
extern const unsigned long VERY_LONG_PRESS;

void initButtons() {
  pinMode(BUTTON_PIN_PRG, INPUT_PULLUP);
  pinMode(BUTTON_PIN_EXTERNAL, INPUT_PULLUP);
  Serial.println("🔘 Buttons initialized (PRG=GPIO0, External=GPIO2)");
}

bool updateButtonState(ButtonState& state) {
  bool prgButtonState = digitalRead(BUTTON_PIN_PRG) == LOW;
  bool externalButtonState = digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
  bool currentButtonState = prgButtonState || externalButtonState;
  
  // Detect button press (transition from not pressed to pressed)
  if (currentButtonState && !state.pressed) {
    state.pressed = true;
    state.pressTime = millis();
    state.lastPress = millis();
    state.prgPressed = prgButtonState;
    state.externalPressed = externalButtonState;
    return true; // State changed
  }
  
  // Detect button release (transition from pressed to not pressed)
  if (!currentButtonState && state.pressed) {
    state.pressed = false;
    return true; // State changed
  }
  
  return false; // No state change
}

bool wasButtonJustPressed(const ButtonState& state) {
  return state.pressed && (millis() - state.pressTime < 50);
}

bool wasButtonJustReleased(const ButtonState& state) {
  return !state.pressed && (millis() - state.lastPress < 50);
}

unsigned long getPressDuration(const ButtonState& state) {
  if (state.pressed) {
    return millis() - state.pressTime;
  } else {
    return 0;
  }
}

bool isShortPress(unsigned long duration) {
  return duration <= SHORT_PRESS_MAX;
}

bool isHoldPress(unsigned long duration) {
  return duration >= HOLD_PRESS_MIN && duration < VERY_LONG_PRESS;
}

bool isVeryLongPress(unsigned long duration) {
  return duration >= VERY_LONG_PRESS;
}

bool wasPrgPressed(const ButtonState& state) {
  return state.prgPressed;
}

bool wasExternalPressed(const ButtonState& state) {
  return state.externalPressed;
}

