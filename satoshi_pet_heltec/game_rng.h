#ifndef GAME_RNG_H
#define GAME_RNG_H

#include <Arduino.h>

// Course randomness for Flappy Pet and Sat Catcher.
//
// In a room game every player gets the same course: the server hands out one
// seed and each pet seeds this generator with it before its turn. Arduino's
// random() can't be used for that: on the ESP32 it reads the hardware RNG and
// ignores randomSeed(). Solo games pass seed 0 and get a fresh course.
//
// Only course generation may draw from it (wall heights, item positions), and
// always in the same order, so the course doesn't depend on how a player plays.

void courseRngSeed(uint32_t seed);  // 0 = random course

// Uniform in [lo, hi). Returns lo when hi <= lo.
long courseRandom(long lo, long hi);

#endif
