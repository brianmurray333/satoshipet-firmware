#ifndef ECONOMY_H
#define ECONOMY_H

#include <Arduino.h>

// Maximum pending spends to queue (older ones auto-sync or drop)
#define MAX_PENDING_SPENDS 50
#define MAX_PENDING_SCORES 10

struct PendingSpend {
  char id[37];           // UUID string (36 chars + null terminator)
  unsigned long timestamp; // millis() when spend occurred
  int amount;            // coins spent
  char action[32];       // "game", "food_lettuce", "food_eggs", "food_steak"
  bool synced;           // true if successfully synced to backend
};

struct PendingGameScore {
  char id[37];           // UUID string (36 chars + null terminator)
  unsigned long timestamp; // millis() when game ended
  int score;             // game score
  bool synced;           // true if successfully synced to backend
};

// Initialize economy system (load pending spends from NVS)
void initEconomy();

// Spend coins locally (immediate deduction, queued for sync)
// Returns true if spend succeeded (had enough coins)
bool spendCoinsLocal(int amount, const char* action);

// Get current local coin balance
int getLocalCoins();

// Set local coin balance (called after server sync)
void setLocalCoins(int coins);

// Sync pending spends to backend (call when WiFi available)
// Returns number of spends successfully synced
int syncPendingSpends();

// Get count of pending (unsynced) spends
int getPendingSpendCount();

// Clear all synced spends from queue
void clearSyncedSpends();

// Clear all economy data (use for debugging/reset)
void clearEconomyData();

// === Game Score Queueing (offline-first) ===

// Queue a game score locally (for sync when online)
// Returns true if queued successfully
bool queueGameScoreLocal(int score);

// Sync pending game scores to backend (call when WiFi available)
// Returns number of scores successfully synced
int syncPendingGameScores();

// Get count of pending (unsynced) game scores
int getPendingGameScoreCount();

// Clear all synced game scores from queue
void clearSyncedGameScores();

#endif

