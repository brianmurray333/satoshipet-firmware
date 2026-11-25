#ifndef GAME_ROOMS_H
#define GAME_ROOMS_H

#include <Arduino.h>
#include "HT_SSD1306Wire.h"

// "With friends" for Flappy Pet, Pickleball and Sat Catcher: one flow for all
// three games.
//
//   Start a room: pick a bet (Free, 10, 50, 100, 500 sats). The bet is held
//   from the balance by the server right away; friends join, the host starts.
//   Join a room: rooms opened by family or Ganamos group members; joining with
//   a bet takes a long hold ("Hold to bet") and holds the bet.
//   Leaving before the start returns the bet; the host leaving closes the room.
//
//   Pickleball plays live over ESP-NOW; every pet reports the result and the
//   server pays the pot when the reports agree.
//   Flappy Pet and Sat Catcher take turns on the same course (shared seed);
//   the high score takes the pot, ties split.
//   A game that doesn't finish returns every bet.
//
// All money moves on the server (api/game/rooms); the pet only shows it.
// Blocks until the player is done. Returns the happiness bonus.
int runFriendsGame(SSD1306Wire &display, uint8_t game);  // game: PetGame

#endif
