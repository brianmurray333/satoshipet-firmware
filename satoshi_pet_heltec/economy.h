#ifndef ECONOMY_H
#define ECONOMY_H

#include <Arduino.h>

// Maximum pending spends to queue (spends are rejected when the queue is full)
#define MAX_PENDING_SPENDS 50
#define MAX_PENDING_SCORES 10

// Compact action codes for queued spends. Mapped back to the server's action
// strings ("game", "feed", ...) only when the sync payload is built.
enum SpendAction : uint8_t {
  SPEND_ACTION_OTHER = 0,
  SPEND_ACTION_GAME = 1,
  SPEND_ACTION_FEED = 2,
  SPEND_ACTION_PICKLEBALL = 3
};

// 24 bytes per entry (was 84). Persisted as-is in NVS (variable-length blob).
struct PendingSpend {
  uint8_t id[16];        // UUIDv4 raw bytes; formatted as 8-4-4-4-12 hex when sent
  int32_t amount;        // coins spent
  uint8_t action;        // SpendAction
  uint8_t synced;        // 1 if successfully synced to backend
  uint8_t reserved[2];
};

// 24 bytes per entry (was 52).
struct PendingGameScore {
  uint8_t id[16];        // UUIDv4 raw bytes
  int32_t score;         // game score
  uint8_t synced;        // 1 if successfully synced to backend
  uint8_t reserved[3];
};

// Initialize economy system (load pending spends, coin balance and pending
// game scores from NVS; migrates older storage formats).
void initEconomy();

// Spend coins locally (immediate deduction, queued for sync)
// Returns true if spend succeeded (had enough coins)
bool spendCoinsLocal(int amount, const char* action);

// Get current local coin balance
int getLocalCoins();

// Set local coin balance (called after server sync). No NVS write if unchanged.
void setLocalCoins(int coins);

// Sync pending spends to backend (call when WiFi available). Marks entries as
// synced in RAM only; call clearSyncedSpends() afterwards to persist once.
// Returns number of spends successfully synced
int syncPendingSpends();

// Get count of pending (unsynced) spends
int getPendingSpendCount();

// Remove synced spends from the queue and persist (single NVS write)
void clearSyncedSpends();

// Clear all economy data (use for debugging/reset)
void clearEconomyData();

// === Game Score Queueing (offline-first) ===

// Queue a game score locally (for sync when online)
// Returns true if queued successfully
bool queueGameScoreLocal(int score);

// Sync pending game scores to backend (call when WiFi available). Marks
// entries as synced in RAM only; call clearSyncedGameScores() afterwards.
// Returns number of scores successfully synced
int syncPendingGameScores();

// Get count of pending (unsynced) game scores
int getPendingGameScoreCount();

// Remove synced game scores from the queue and persist (single NVS write)
void clearSyncedGameScores();

#endif
