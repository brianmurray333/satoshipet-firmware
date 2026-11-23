#include "economy.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <stddef.h>
#include <string.h>
#include "config.h"

// Shared keep-alive HTTPS session (provided by config.cpp).
bool ganamosHttpBegin(HTTPClient& http, const String& path);

// ---------------------------------------------------------------------------
// Persisted queue format (V3)
//
// Each queue is stored as ONE variable-length NVS blob:
//   QueueHeader (12 bytes) + count * entry (24 bytes)
// The struct below is both the live RAM storage and the persisted image, so no
// scratch copy is needed. The coin balance lives under its own small int key
// so balance updates never rewrite the queue.
// ---------------------------------------------------------------------------

struct QueueHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t checksum;   // FNV-1a over header (checksum=0) + entries[0..count)
};

struct SpendQueueState {
  QueueHeader hdr;
  PendingSpend entries[MAX_PENDING_SPENDS];
};

struct ScoreQueueState {
  QueueHeader hdr;
  PendingGameScore entries[MAX_PENDING_SCORES];
};

static constexpr uint32_t SPEND_QUEUE_MAGIC = 0x45564D33; // "EVM3"
static constexpr uint32_t SCORE_QUEUE_MAGIC = 0x53435633; // "SCV3"
static constexpr uint16_t QUEUE_VERSION = 3;

static const char* const ECON_NS = "economy";
static const char* const SCORE_NS = "scores";
static const char* const KEY_SPENDS_V3 = "spendsV3";
static const char* const KEY_SCORES_V3 = "scoresV3";
static const char* const KEY_COINS = "localCoins";     // same key the legacy format used
static const char* const KEY_LEGACY_V2 = "stateV2";      // fixed-size blob (both namespaces)

// Upper bound on requests per sync call so a long offline backlog cannot block
// the main loop for a long time; the rest is sent on the next sync.
static const int MAX_SYNC_REQUESTS_PER_CALL = 10;

static Preferences economyPrefs;
static SpendQueueState spendState;
static ScoreQueueState scoreState;
static int localCoinBalance = 0;
static int persistedCoinBalance = -1;  // value currently in NVS (-1 = unknown)

static uint32_t fnv1a(uint32_t hash, const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}
static const uint32_t FNV_SEED = 2166136261UL;

template <typename State>
static size_t queueBytes(const State& s) {
  return offsetof(State, entries) + (size_t)s.hdr.count * sizeof(s.entries[0]);
}

template <typename State>
static uint32_t queueChecksum(const State& s) {
  QueueHeader h = s.hdr;
  h.checksum = 0;
  uint32_t hash = fnv1a(FNV_SEED, &h, sizeof(h));
  return fnv1a(hash, s.entries, (size_t)s.hdr.count * sizeof(s.entries[0]));
}

template <typename State>
static void sealQueue(State& s, uint32_t magic) {
  s.hdr.magic = magic;
  s.hdr.version = QUEUE_VERSION;
  s.hdr.checksum = queueChecksum(s);
}

// Reads a V3 queue blob. Returns true only for a complete, valid record.
template <typename State>
static bool loadQueueBlob(Preferences& prefs, const char* key, State& s, uint32_t magic) {
  const size_t maxEntries = sizeof(s.entries) / sizeof(s.entries[0]);
  memset(&s, 0, sizeof(s));
  const size_t len = prefs.getBytesLength(key);
  if (len < offsetof(State, entries) || len > sizeof(State)) return false;
  if (prefs.getBytes(key, &s, len) != len) { memset(&s, 0, sizeof(s)); return false; }
  const bool ok = s.hdr.magic == magic &&
                  s.hdr.version == QUEUE_VERSION &&
                  s.hdr.count <= maxEntries &&
                  queueBytes(s) == len &&
                  s.hdr.checksum == queueChecksum(s);
  if (!ok) memset(&s, 0, sizeof(s));
  return ok;
}

// ---------------------------------------------------------------------------
// Legacy V2 layouts (fixed-size blobs written by the previous firmware).
// Member order/types must stay identical so the compiler reproduces the old
// layout; they are only used during one-time migration.
// ---------------------------------------------------------------------------
static constexpr uint32_t LEGACY_ECONOMY_V2_MAGIC = 0x45564D32; // "EVM2"
static constexpr uint32_t LEGACY_SCORE_V2_MAGIC = 0x53435632;   // "SCV2"
static constexpr uint16_t LEGACY_V2_VERSION = 2;

struct LegacySpendV2 {
  char id[37];
  unsigned long timestamp;
  int amount;
  char action[32];
  bool synced;
};

struct LegacyEconomyStateV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  int32_t localCoins;
  LegacySpendV2 spends[50];
  uint32_t checksum;
};

struct LegacyScoreV2 {
  char id[37];
  unsigned long timestamp;
  int score;
  bool synced;
};

struct LegacyScoreStateV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  LegacyScoreV2 scores[10];
  uint32_t checksum;
};

static_assert(sizeof(PendingSpend) == 24, "PendingSpend is persisted; keep its size stable");
static_assert(sizeof(PendingGameScore) == 24, "PendingGameScore is persisted; keep its size stable");
static_assert(sizeof(QueueHeader) == 12, "QueueHeader is persisted; keep its size stable");
#if defined(ARDUINO_ARCH_ESP32)
// Sizes of the blobs written by the previous firmware (ILP32 targets).
static_assert(sizeof(LegacyEconomyStateV2) == 4216, "legacy economy V2 layout changed");
static_assert(sizeof(LegacyScoreStateV2) == 532, "legacy score V2 layout changed");
#endif

template <typename T>
static bool legacyV2Valid(const T& state, uint32_t magic, size_t maxCount) {
  return state.magic == magic &&
         state.version == LEGACY_V2_VERSION &&
         state.count <= maxCount &&
         state.checksum == fnv1a(FNV_SEED, &state, sizeof(T) - sizeof(state.checksum));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Random (version 4) UUID stored as 16 raw bytes.
static void generateUUID(uint8_t out[16]) {
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(out + i, &r, 4);
  }
  out[6] = (out[6] & 0x0F) | 0x40;  // version 4
  out[8] = (out[8] & 0x3F) | 0x80;  // RFC 4122 variant
}

static void formatUUID(const uint8_t id[16], char out[37]) {
  static const char hex[] = "0123456789abcdef";
  int p = 0;
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[p++] = '-';
    out[p++] = hex[id[i] >> 4];
    out[p++] = hex[id[i] & 0x0F];
  }
  out[p] = '\0';
}

// Parses a legacy 36-char UUID string ("8-4-4-4-12" hex) into raw bytes so
// queued spends keep their idempotency key across the format migration.
static bool parseUUID(const char* s, uint8_t out[16]) {
  int nibbles = 0;
  for (int i = 0; s[i] != '\0' && i < 40; ++i) {
    char c = s[i];
    if (c == '-') continue;
    uint8_t v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return false;
    if (nibbles >= 32) return false;
    if (nibbles % 2 == 0) out[nibbles / 2] = v << 4;
    else out[nibbles / 2] |= v;
    nibbles++;
  }
  return nibbles == 32;
}

static bool uuidIsZero(const uint8_t id[16]) {
  for (int i = 0; i < 16; ++i) if (id[i]) return false;
  return true;
}

static uint8_t spendActionFromString(const char* action) {
  if (action == nullptr) return SPEND_ACTION_OTHER;
  if (strcmp(action, "game") == 0) return SPEND_ACTION_GAME;
  if (strcmp(action, "feed") == 0 || strncmp(action, "food", 4) == 0) return SPEND_ACTION_FEED;
  if (strcmp(action, "pickleball") == 0) return SPEND_ACTION_PICKLEBALL;
  return SPEND_ACTION_OTHER;
}

static const char* spendActionToString(uint8_t action) {
  switch (action) {
    case SPEND_ACTION_GAME: return "game";
    case SPEND_ACTION_FEED: return "feed";
    case SPEND_ACTION_PICKLEBALL: return "pickleball";
    default: return "other";
  }
}

// Reads the response body and returns the JSON "success" flag.
static bool responseSucceeded(HTTPClient& http) {
  String body = http.getString();
  JsonDocument filter;
  filter["success"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) return false;
  return doc["success"] | false;
}

// Transient failures (no connection, rate limit, server error): stop this
// sync round and retry on the next one instead of hammering every entry.
static bool shouldAbortSync(int httpCode) {
  return httpCode <= 0 || httpCode == 429 || httpCode >= 500;
}

// Single NVS session: queue blob and (optionally) the coin balance.
static void persistEconomy(bool writeQueue, bool writeCoins) {
  if (!writeQueue && !writeCoins) return;
  if (!economyPrefs.begin(ECON_NS, false)) {
    Serial.println("Economy: NVS open failed");
    return;
  }
  if (writeQueue) {
    sealQueue(spendState, SPEND_QUEUE_MAGIC);
    const size_t len = queueBytes(spendState);
    if (economyPrefs.putBytes(KEY_SPENDS_V3, &spendState, len) != len) {
      Serial.println("Economy: failed to persist spend queue");
    }
  }
  if (writeCoins) {
    if (economyPrefs.putInt(KEY_COINS, localCoinBalance) == sizeof(int32_t)) {
      persistedCoinBalance = localCoinBalance;
    } else {
      Serial.println("Economy: failed to persist coin balance");
    }
  }
  economyPrefs.end();
}

static void persistScores() {
  if (!economyPrefs.begin(SCORE_NS, false)) {
    Serial.println("Scores: NVS open failed");
    return;
  }
  sealQueue(scoreState, SCORE_QUEUE_MAGIC);
  const size_t len = queueBytes(scoreState);
  if (economyPrefs.putBytes(KEY_SCORES_V3, &scoreState, len) != len) {
    Serial.println("Scores: failed to persist score queue");
  }
  economyPrefs.end();
}

// ---------------------------------------------------------------------------
// Loading / migration
// ---------------------------------------------------------------------------

// Appends a legacy spend (string id/action) to the V3 queue.
static void appendMigratedSpend(const char* id, int amount, const char* action, bool synced) {
  if (synced || amount <= 0) return;  // already on the server / invalid
  if (spendState.hdr.count >= MAX_PENDING_SPENDS) return;
  PendingSpend& e = spendState.entries[spendState.hdr.count];
  memset(&e, 0, sizeof(e));
  if (!parseUUID(id, e.id)) generateUUID(e.id);
  e.amount = amount;
  e.action = spendActionFromString(action);
  e.synced = 0;
  spendState.hdr.count++;
}

static void appendMigratedScore(const char* id, int score, bool synced) {
  if (synced || score < 0) return;
  if (scoreState.hdr.count >= MAX_PENDING_SCORES) return;
  PendingGameScore& e = scoreState.entries[scoreState.hdr.count];
  memset(&e, 0, sizeof(e));
  if (!parseUUID(id, e.id)) generateUUID(e.id);
  e.score = score;
  e.synced = 0;
  scoreState.hdr.count++;
}

// Returns true if the caller should write the V3 record (migration happened or
// the stored record was missing/corrupt). Sets removeV2/legacyCount for cleanup.
static bool loadSpendQueue(bool& removeV2, int& legacyCount) {
  removeV2 = false;
  legacyCount = 0;
  memset(&spendState, 0, sizeof(spendState));

  if (economyPrefs.isKey(KEY_SPENDS_V3)) {
    localCoinBalance = max(0, (int)economyPrefs.getInt(KEY_COINS, 0));
    if (!loadQueueBlob(economyPrefs, KEY_SPENDS_V3, spendState, SPEND_QUEUE_MAGIC)) {
      // Do not resurrect older entries after a damaged modern record; the
      // server restores the authoritative coin balance on the next poll.
      Serial.println("Economy: corrupt spend queue rejected");
      return true;
    }
    return false;
  }

  if (economyPrefs.isKey(KEY_LEGACY_V2)) {
    removeV2 = true;
    const size_t len = economyPrefs.getBytesLength(KEY_LEGACY_V2);
    LegacyEconomyStateV2* old = (LegacyEconomyStateV2*)malloc(sizeof(LegacyEconomyStateV2));
    if (old == nullptr) {
      // Keep the old record for the next boot rather than losing it.
      removeV2 = false;
      localCoinBalance = max(0, (int)economyPrefs.getInt(KEY_COINS, 0));
      Serial.println("Economy: no heap for V2 migration; retrying next boot");
      return false;
    }
    memset(old, 0, sizeof(*old));
    const bool valid = len == sizeof(*old) &&
                       economyPrefs.getBytes(KEY_LEGACY_V2, old, sizeof(*old)) == sizeof(*old) &&
                       legacyV2Valid(*old, LEGACY_ECONOMY_V2_MAGIC, 50);
    if (valid) {
      localCoinBalance = max(0, (int)old->localCoins);
      for (int i = 0; i < old->count; ++i) {
        old->spends[i].id[36] = '\0';
        old->spends[i].action[31] = '\0';
        appendMigratedSpend(old->spends[i].id, old->spends[i].amount,
                            old->spends[i].action, old->spends[i].synced);
      }
      Serial.printf("Economy: migrated %d pending spends from V2\n", spendState.hdr.count);
    } else {
      localCoinBalance = 0;
      Serial.println("Economy: corrupt V2 state rejected; awaiting server reconciliation");
    }
    free(old);
    return true;
  }

  // Oldest format: one "id|timestamp|amount|action|synced" string per spend.
  legacyCount = constrain((int)economyPrefs.getInt("spendCount", 0), 0, (int)MAX_PENDING_SPENDS);
  for (int i = 0; i < legacyCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "spend_%d", i);
    String data = economyPrefs.getString(key, "");
    int p1 = data.indexOf('|');
    int p2 = data.indexOf('|', p1 + 1);
    int p3 = data.indexOf('|', p2 + 1);
    int p4 = data.indexOf('|', p3 + 1);
    if (p1 > 0 && p2 > 0 && p3 > 0 && p4 > 0) {
      String id = data.substring(0, p1);
      int amount = data.substring(p2 + 1, p3).toInt();
      String action = data.substring(p3 + 1, p4);
      bool synced = data.substring(p4 + 1).toInt() != 0;
      appendMigratedSpend(id.c_str(), amount, action.c_str(), synced);
    }
  }
  localCoinBalance = max(0, (int)economyPrefs.getInt(KEY_COINS, 0));
  return true;
}

static bool loadScoreQueue(bool& removeV2, int& legacyCount) {
  removeV2 = false;
  legacyCount = 0;
  memset(&scoreState, 0, sizeof(scoreState));

  if (economyPrefs.isKey(KEY_SCORES_V3)) {
    if (!loadQueueBlob(economyPrefs, KEY_SCORES_V3, scoreState, SCORE_QUEUE_MAGIC)) {
      Serial.println("Scores: corrupt score queue rejected");
      return true;
    }
    return false;
  }

  if (economyPrefs.isKey(KEY_LEGACY_V2)) {
    removeV2 = true;
    const size_t len = economyPrefs.getBytesLength(KEY_LEGACY_V2);
    LegacyScoreStateV2* old = (LegacyScoreStateV2*)malloc(sizeof(LegacyScoreStateV2));
    if (old == nullptr) {
      removeV2 = false;
      return false;
    }
    memset(old, 0, sizeof(*old));
    const bool valid = len == sizeof(*old) &&
                       economyPrefs.getBytes(KEY_LEGACY_V2, old, sizeof(*old)) == sizeof(*old) &&
                       legacyV2Valid(*old, LEGACY_SCORE_V2_MAGIC, 10);
    if (valid) {
      for (int i = 0; i < old->count; ++i) {
        old->scores[i].id[36] = '\0';
        appendMigratedScore(old->scores[i].id, old->scores[i].score, old->scores[i].synced);
      }
    } else {
      Serial.println("Scores: corrupt V2 state rejected safely");
    }
    free(old);
    return true;
  }

  legacyCount = constrain((int)economyPrefs.getInt("scoreCount", 0), 0, (int)MAX_PENDING_SCORES);
  for (int i = 0; i < legacyCount; i++) {
    char key[16];
    snprintf(key, sizeof(key), "score_%d", i);
    String data = economyPrefs.getString(key, "");
    int p1 = data.indexOf('|');
    int p2 = data.indexOf('|', p1 + 1);
    int p3 = data.indexOf('|', p2 + 1);
    if (p1 > 0 && p2 > 0 && p3 > 0) {
      String id = data.substring(0, p1);
      int score = data.substring(p2 + 1, p3).toInt();
      bool synced = data.substring(p3 + 1).toInt() != 0;
      appendMigratedScore(id.c_str(), score, synced);
    }
  }
  return true;
}

// Removes superseded legacy keys after the V3 record has been written.
static void removeLegacyKeys(const char* ns, bool removeV2, const char* countKey,
                             const char* itemPrefix, int legacyCount) {
  if (!removeV2 && legacyCount == 0) return;
  if (!economyPrefs.begin(ns, false)) return;
  if (removeV2) economyPrefs.remove(KEY_LEGACY_V2);
  if (legacyCount > 0) {
    for (int i = 0; i < legacyCount; i++) {
      char key[16];
      snprintf(key, sizeof(key), "%s%d", itemPrefix, i);
      economyPrefs.remove(key);
    }
    economyPrefs.remove(countKey);
  }
  economyPrefs.end();
}

void initEconomy() {
  // --- Spends + coin balance ---
  bool removeV2 = false;
  int legacyCount = 0;
  bool rewrite = false;
  localCoinBalance = 0;
  if (economyPrefs.begin(ECON_NS, true)) {  // read-only; fails on first boot (no namespace yet)
    rewrite = loadSpendQueue(removeV2, legacyCount);  // also loads localCoinBalance
    economyPrefs.end();
  } else {
    memset(&spendState, 0, sizeof(spendState));
    rewrite = true;
  }
  persistedCoinBalance = rewrite ? -1 : localCoinBalance;
  if (rewrite) {
    persistEconomy(true, true);
    removeLegacyKeys(ECON_NS, removeV2, "spendCount", "spend_", legacyCount);
  }
  Serial.printf("Economy: %d pending spends, %d coins\n", spendState.hdr.count, localCoinBalance);

  // --- Game scores (loaded eagerly so getPendingGameScoreCount() is right after reboot) ---
  removeV2 = false;
  legacyCount = 0;
  rewrite = false;
  if (economyPrefs.begin(SCORE_NS, true)) {
    rewrite = loadScoreQueue(removeV2, legacyCount);
    economyPrefs.end();
  } else {
    memset(&scoreState, 0, sizeof(scoreState));
    // Nothing stored yet; the first queued score creates the record.
  }
  if (rewrite) {
    persistScores();
    removeLegacyKeys(SCORE_NS, removeV2, "scoreCount", "score_", legacyCount);
  }
  Serial.printf("Scores: %d pending scores\n", scoreState.hdr.count);
}

// ---------------------------------------------------------------------------
// Spends
// ---------------------------------------------------------------------------

bool spendCoinsLocal(int amount, const char* action) {
  if (amount <= 0 || action == nullptr || action[0] == '\0') {
    Serial.println("Economy: invalid local spend");
    return false;
  }

  // Never deduct a coin unless its idempotent sync record can be retained.
  if (spendState.hdr.count >= MAX_PENDING_SPENDS) {
    Serial.println("Economy: pending queue full; spend rejected safely");
    return false;
  }

  if (localCoinBalance < amount) {
    Serial.printf("Economy: insufficient coins (%d < %d)\n", localCoinBalance, amount);
    return false;
  }

  localCoinBalance -= amount;

  PendingSpend& spend = spendState.entries[spendState.hdr.count];
  memset(&spend, 0, sizeof(spend));
  generateUUID(spend.id);
  spend.amount = amount;
  spend.action = spendActionFromString(action);
  spend.synced = 0;
  spendState.hdr.count++;

  Serial.printf("Economy: spent %d coins on %s (balance %d, queued %d)\n",
                amount, action, localCoinBalance, spendState.hdr.count);

  persistEconomy(true, localCoinBalance != persistedCoinBalance);
  return true;
}

int getLocalCoins() {
  return localCoinBalance;
}

void setLocalCoins(int coins) {
  if (coins < 0) coins = 0;
  localCoinBalance = coins;
  if (localCoinBalance != persistedCoinBalance) {
    persistEconomy(false, true);
  }
}

int syncPendingSpends() {
  if (getPendingSpendCount() == 0) return 0;

  extern GanamosConfig ganamosConfig;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Economy: cannot sync - no WiFi");
    return 0;
  }
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("Economy: cannot sync - no device ID");
    return 0;
  }

  int syncedCount = 0;
  int requests = 0;

  for (int i = 0; i < spendState.hdr.count && requests < MAX_SYNC_REQUESTS_PER_CALL; i++) {
    esp_task_wdt_reset();
    PendingSpend& spend = spendState.entries[i];
    if (spend.synced) continue;

    if (spend.amount <= 0 || uuidIsZero(spend.id)) {
      Serial.printf("Economy: dropping invalid spend at index %d\n", i);
      spend.synced = 1;  // removed by clearSyncedSpends()
      continue;
    }

    HTTPClient http;
    if (!ganamosHttpBegin(http, "/api/device/economy/sync")) {
      Serial.println("Economy: HTTP begin failed");
      break;
    }
    requests++;

    char id[37];
    formatUUID(spend.id, id);
    char payload[128];
    int len = snprintf(payload, sizeof(payload),
                       "{\"spendId\":\"%s\",\"amount\":%ld,\"action\":\"%s\"}",
                       id, (long)spend.amount, spendActionToString(spend.action));
    if (len < 0 || len >= (int)sizeof(payload)) {
      http.end();
      continue;
    }

    int httpCode = http.POST((uint8_t*)payload, (size_t)len);
    bool ok = httpCode == 200 && responseSucceeded(http);
    http.end();

    if (ok) {
      spend.synced = 1;
      syncedCount++;
      Serial.printf("Economy: synced spend %s\n", id);
    } else {
      Serial.printf("Economy: spend sync failed (HTTP %d)\n", httpCode);
      if (shouldAbortSync(httpCode)) break;
    }
  }

  if (syncedCount > 0) {
    Serial.printf("Economy: synced %d spends\n", syncedCount);
  }
  // No NVS write here: clearSyncedSpends() persists the compacted queue once.
  return syncedCount;
}

int getPendingSpendCount() {
  int unsynced = 0;
  for (int i = 0; i < spendState.hdr.count; i++) {
    if (!spendState.entries[i].synced) unsynced++;
  }
  return unsynced;
}

void clearSyncedSpends() {
  esp_task_wdt_reset();
  if (spendState.hdr.count > MAX_PENDING_SPENDS) spendState.hdr.count = MAX_PENDING_SPENDS;

  int writeIdx = 0;
  for (int readIdx = 0; readIdx < spendState.hdr.count; readIdx++) {
    if (!spendState.entries[readIdx].synced) {
      if (writeIdx != readIdx) spendState.entries[writeIdx] = spendState.entries[readIdx];
      writeIdx++;
    }
  }

  int removed = spendState.hdr.count - writeIdx;
  spendState.hdr.count = writeIdx;

  if (removed > 0) {
    Serial.printf("Economy: cleared %d synced spends\n", removed);
    esp_task_wdt_reset();
    persistEconomy(true, false);
  }
}

void clearEconomyData() {
  if (economyPrefs.begin(ECON_NS, false)) {
    economyPrefs.clear();
    economyPrefs.end();
  }

  memset(&spendState, 0, sizeof(spendState));
  localCoinBalance = 0;
  persistedCoinBalance = -1;

  Serial.println("Economy: all data cleared");
}

// ---------------------------------------------------------------------------
// Game scores
// ---------------------------------------------------------------------------

bool queueGameScoreLocal(int score) {
  if (score < 0) {
    Serial.printf("Scores: invalid score %d\n", score);
    return false;
  }
  if (scoreState.hdr.count >= MAX_PENDING_SCORES) {
    Serial.println("Scores: queue full; score retained in gameplay only");
    return false;
  }

  PendingGameScore& entry = scoreState.entries[scoreState.hdr.count];
  memset(&entry, 0, sizeof(entry));
  generateUUID(entry.id);
  entry.score = score;
  entry.synced = 0;
  scoreState.hdr.count++;

  Serial.printf("Scores: queued score %d (pending %d)\n", score, scoreState.hdr.count);
  persistScores();
  return true;
}

int syncPendingGameScores() {
  if (getPendingGameScoreCount() == 0) return 0;

  extern GanamosConfig ganamosConfig;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Scores: cannot sync - no WiFi");
    return 0;
  }
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("Scores: cannot sync - no device ID");
    return 0;
  }

  int syncedCount = 0;
  int requests = 0;

  for (int i = 0; i < scoreState.hdr.count && requests < MAX_SYNC_REQUESTS_PER_CALL; i++) {
    esp_task_wdt_reset();
    PendingGameScore& entry = scoreState.entries[i];
    if (entry.synced) continue;

    if (entry.score < 0) {
      entry.synced = 1;
      continue;
    }

    HTTPClient http;
    if (!ganamosHttpBegin(http, "/api/device/game-score")) {
      Serial.println("Scores: HTTP begin failed");
      break;
    }
    requests++;

    // "scoreId" lets the server deduplicate retries; servers that do not
    // know the field ignore it.
    char id[37];
    formatUUID(entry.id, id);
    char payload[96];
    int len = snprintf(payload, sizeof(payload), "{\"score\":%ld,\"scoreId\":\"%s\"}",
                       (long)entry.score, id);
    if (len < 0 || len >= (int)sizeof(payload)) {
      http.end();
      continue;
    }

    int httpCode = http.POST((uint8_t*)payload, (size_t)len);
    bool ok = httpCode == 200 && responseSucceeded(http);
    http.end();

    if (ok) {
      entry.synced = 1;
      syncedCount++;
      Serial.printf("Scores: synced score %ld\n", (long)entry.score);
    } else {
      Serial.printf("Scores: sync failed (HTTP %d)\n", httpCode);
      if (shouldAbortSync(httpCode)) break;
    }
  }

  // No NVS write here: clearSyncedGameScores() persists once.
  return syncedCount;
}

int getPendingGameScoreCount() {
  int unsynced = 0;
  for (int i = 0; i < scoreState.hdr.count; i++) {
    if (!scoreState.entries[i].synced) unsynced++;
  }
  return unsynced;
}

void clearSyncedGameScores() {
  if (scoreState.hdr.count > MAX_PENDING_SCORES) scoreState.hdr.count = MAX_PENDING_SCORES;

  int writeIdx = 0;
  for (int readIdx = 0; readIdx < scoreState.hdr.count; readIdx++) {
    if (!scoreState.entries[readIdx].synced) {
      if (writeIdx != readIdx) scoreState.entries[writeIdx] = scoreState.entries[readIdx];
      writeIdx++;
    }
  }

  int removed = scoreState.hdr.count - writeIdx;
  scoreState.hdr.count = writeIdx;

  if (removed > 0) {
    Serial.printf("Scores: cleared %d synced scores\n", removed);
    persistScores();
  }
}
