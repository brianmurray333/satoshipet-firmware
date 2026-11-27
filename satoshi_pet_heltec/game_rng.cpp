#include "game_rng.h"
#include <esp_random.h>

static uint32_t rngState = 0x9E3779B9u;

void courseRngSeed(uint32_t seed) {
  if (seed == 0) seed = esp_random();
  // splitmix32 so nearby seeds give unrelated courses; never leave xorshift at 0
  seed += 0x9E3779B9u;
  seed = (seed ^ (seed >> 16)) * 0x85EBCA6Bu;
  seed = (seed ^ (seed >> 13)) * 0xC2B2AE35u;
  seed ^= seed >> 16;
  rngState = seed ? seed : 0x9E3779B9u;
}

static uint32_t next32() {
  // xorshift32
  uint32_t x = rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rngState = x;
  return x;
}

long courseRandom(long lo, long hi) {
  if (hi <= lo) return lo;
  uint32_t span = (uint32_t)(hi - lo);
  return lo + (long)(next32() % span);
}
