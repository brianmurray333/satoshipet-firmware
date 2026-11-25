#include "button_handler.h"

void initButtons() {
  pinMode(BUTTON_PIN_PRG, INPUT_PULLUP);
  pinMode(BUTTON_PIN_EXTERNAL, INPUT_PULLUP);
  Serial.println("Buttons initialized (PRG=GPIO0, External=GPIO2)");
}

void debounceInit(DebouncedButton& b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  bool down = digitalRead(pin) == LOW;
  b.stableDown = down;
  b.lastRaw = down;
  b.changedAt = millis();
}

int debounceUpdate(DebouncedButton& b, unsigned long now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = now;
    return 0;
  }
  if (raw != b.stableDown && now - b.changedAt >= DEBOUNCE_STABLE_MS) {
    b.stableDown = raw;
    return raw ? 1 : -1;
  }
  return 0;
}

bool debounceIsDown(const DebouncedButton& b) {
  return b.stableDown;
}
