#include "economy.h"
#include "config.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

static Preferences economyPrefs;
static PendingSpend pendingSpends[MAX_PENDING_SPENDS];
static int pendingSpendCount = 0;
static int localCoinBalance = 0;
static void savePendingSpends();

static constexpr uint32_t ECONOMY_STATE_MAGIC = 0x45564D32; // "EVM2"
static constexpr uint16_t ECONOMY_STATE_VERSION = 2;
static constexpr uint32_t SCORE_STATE_MAGIC = 0x53435632; // "SCV2"
static constexpr uint16_t SCORE_STATE_VERSION = 2;

struct EconomyStateV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  int32_t localCoins;
  PendingSpend spends[MAX_PENDING_SPENDS];
  uint32_t checksum;
};

struct ScoreStateV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  PendingGameScore scores[MAX_PENDING_SCORES];
  uint32_t checksum;
};

// Keep the large queue image out of the Arduino loop-task stack. A 4 KB local
// object here materially increases stack-overflow risk during NVS operations.
static EconomyStateV2 economyStateScratch;
static ScoreStateV2 scoreStateScratch;

static uint32_t checksumBytes(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

template <typename T>
static uint32_t stateChecksum(const T& state) {
  return checksumBytes(&state, sizeof(T) - sizeof(state.checksum));
}

// Generate a simple UUID (not cryptographically secure, but unique enough for our use)
static void generateUUID(char* uuidStr) {
  uint64_t tail = ((uint64_t)esp_random() << 16) | (esp_random() & 0xFFFFULL);
  snprintf(uuidStr, 37, "%08lx-%04x-%04x-%04x-%012llx",
    (unsigned long)esp_random(),
    (unsigned int)random(0xFFFF),
    (unsigned int)(0x4000 | random(0x0FFF)), // Version 4
    (unsigned int)(0x8000 | random(0x3FFF)), // Variant
    (unsigned long long)tail
  );
}

void initEconomy() {
  // Load pending spends from NVS
  economyPrefs.begin("economy", true); // read-only

  memset(&economyStateScratch, 0, sizeof(economyStateScratch));
  const size_t v2Length = economyPrefs.getBytesLength("stateV2");
  const bool hasV2 = v2Length == sizeof(economyStateScratch) &&
                     economyPrefs.getBytes("stateV2", &economyStateScratch, sizeof(economyStateScratch)) == sizeof(economyStateScratch) &&
                     economyStateScratch.magic == ECONOMY_STATE_MAGIC &&
                     economyStateScratch.version == ECONOMY_STATE_VERSION &&
                     economyStateScratch.count <= MAX_PENDING_SPENDS &&
                     economyStateScratch.checksum == stateChecksum(economyStateScratch);
  if (hasV2) {
    pendingSpendCount = economyStateScratch.count;
    localCoinBalance = max(0, (int)economyStateScratch.localCoins);
    memcpy(pendingSpends, economyStateScratch.spends, sizeof(pendingSpends));
    economyPrefs.end();
    Serial.printf("Economy: restored %d pending spends and %d coins\n",
                  pendingSpendCount, localCoinBalance);
    return;
  }
  if (v2Length > 0) {
    // Do not resurrect stale legacy entries after a damaged modern record.
    // The server will restore the authoritative coin balance on the next poll.
    pendingSpendCount = 0;
    localCoinBalance = 0;
    memset(pendingSpends, 0, sizeof(pendingSpends));
    economyPrefs.end();
    Serial.println("Economy: corrupt state rejected; awaiting server reconciliation");
    return;
  }
  
  pendingSpendCount = economyPrefs.getInt("spendCount", 0);
  if (pendingSpendCount > MAX_PENDING_SPENDS) {
    pendingSpendCount = MAX_PENDING_SPENDS;
  }
  
  Serial.println("💰 Economy: Loading " + String(pendingSpendCount) + " pending spends");
  
  for (int i = 0; i < pendingSpendCount; i++) {
    String key = "spend_" + String(i);
    String data = economyPrefs.getString(key.c_str(), "");
    
    if (data.length() > 0) {
      // Parse: id|timestamp|amount|action|synced
      int pipe1 = data.indexOf('|');
      int pipe2 = data.indexOf('|', pipe1 + 1);
      int pipe3 = data.indexOf('|', pipe2 + 1);
      int pipe4 = data.indexOf('|', pipe3 + 1);
      
      if (pipe1 > 0 && pipe2 > 0 && pipe3 > 0 && pipe4 > 0) {
        String id = data.substring(0, pipe1);
        unsigned long timestamp = data.substring(pipe1 + 1, pipe2).toInt();
        int amount = data.substring(pipe2 + 1, pipe3).toInt();
        String action = data.substring(pipe3 + 1, pipe4);
        bool synced = data.substring(pipe4 + 1).toInt();
        
        strncpy(pendingSpends[i].id, id.c_str(), 36);
        pendingSpends[i].id[36] = '\0';
        pendingSpends[i].timestamp = timestamp;
        pendingSpends[i].amount = amount;
        strncpy(pendingSpends[i].action, action.c_str(), 31);
        pendingSpends[i].action[31] = '\0';
        pendingSpends[i].synced = synced;
        
        Serial.println("  Loaded: " + String(pendingSpends[i].action) + " (" + String(amount) + " coins)");
      }
    }
  }
  
  localCoinBalance = economyPrefs.getInt("localCoins", 0);
  Serial.println("💰 Economy: Local balance = " + String(localCoinBalance) + " coins");
  
  economyPrefs.end();

  // Convert legacy per-key records to one checksummed NVS value. A single
  // record avoids partial queue rewrites and greatly reduces heap churn.
  savePendingSpends();
}

static void savePendingSpends() {
  Serial.println("💰 [REBOOT DEBUG] savePendingSpends() START - count=" + String(pendingSpendCount));
  
  // Safety: Validate pendingSpendCount before saving
  if (pendingSpendCount < 0 || pendingSpendCount > MAX_PENDING_SPENDS) {
    Serial.println("⚠️ [SAFETY] Invalid pendingSpendCount: " + String(pendingSpendCount) + " - clamping to safe range");
    pendingSpendCount = min(max(0, pendingSpendCount), MAX_PENDING_SPENDS);
  }
  
  memset(&economyStateScratch, 0, sizeof(economyStateScratch));
  economyStateScratch.magic = ECONOMY_STATE_MAGIC;
  economyStateScratch.version = ECONOMY_STATE_VERSION;
  economyStateScratch.count = pendingSpendCount;
  economyStateScratch.localCoins = max(0, localCoinBalance);
  memcpy(economyStateScratch.spends, pendingSpends, sizeof(pendingSpends));
  for (int i = 0; i < pendingSpendCount; ++i) {
    economyStateScratch.spends[i].id[36] = '\0';
    economyStateScratch.spends[i].action[31] = '\0';
  }
  economyStateScratch.checksum = stateChecksum(economyStateScratch);

  economyPrefs.begin("economy", false);
  const size_t written = economyPrefs.putBytes("stateV2", &economyStateScratch, sizeof(economyStateScratch));
  economyPrefs.end();
  if (written != sizeof(economyStateScratch)) {
    Serial.println("Economy: failed to persist complete state");
  }
}

bool spendCoinsLocal(int amount, const char* action) {
  if (amount <= 0 || action == nullptr || action[0] == '\0') {
    Serial.println("Economy: invalid local spend");
    return false;
  }

  // Never deduct a coin unless its idempotent sync record can be retained.
  if (pendingSpendCount >= MAX_PENDING_SPENDS) {
    Serial.println("Economy: pending queue full; spend rejected safely");
    return false;
  }

  // Check if we have enough coins
  if (localCoinBalance < amount) {
    Serial.println("❌ Economy: Insufficient coins (" + String(localCoinBalance) + " < " + String(amount) + ")");
    return false;
  }
  
  // Deduct coins immediately
  localCoinBalance -= amount;
  
  // Add to pending queue.
  {
    PendingSpend& spend = pendingSpends[pendingSpendCount];
    generateUUID(spend.id);
    spend.timestamp = millis();
    spend.amount = amount;
    strncpy(spend.action, action, 31);
    spend.action[31] = '\0';
    spend.synced = false;
    
    pendingSpendCount++;
    
    Serial.println("💰 Economy: Spent " + String(amount) + " coins on " + String(action) + 
                   " (balance: " + String(localCoinBalance) + ", pending: " + String(pendingSpendCount) + ")");
    
    // Save to NVS
    savePendingSpends();
    
    return true;
  }
}

int getLocalCoins() {
  return localCoinBalance;
}

void setLocalCoins(int coins) {
  localCoinBalance = coins;
  savePendingSpends();
}

int syncPendingSpends() {
  if (pendingSpendCount == 0) {
    return 0;
  }
  
  extern GanamosConfig ganamosConfig;
  
  // Check WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Economy: Cannot sync - no WiFi");
    return 0;
  }
  
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("⚠️ Economy: Cannot sync - no device ID");
    return 0;
  }
  
  int syncedCount = 0;
  
  // Create client ONCE and reuse (reduces heap fragmentation)
  WiFiClientSecure* client = new WiFiClientSecure;
  if (!client) {
    Serial.println("❌ Economy: Failed to create HTTP client");
    return 0;
  }
  
  client->setInsecure();
  client->setTimeout(5000);
  client->setHandshakeTimeout(5);
  
  for (int i = 0; i < pendingSpendCount; i++) {
    // Feed watchdog at start of each sync attempt
    esp_task_wdt_reset();
    
    if (pendingSpends[i].synced) {
      continue; // Already synced
    }
    
    // Validate spend data before syncing
    if (pendingSpends[i].amount <= 0 || strlen(pendingSpends[i].id) < 10) {
      Serial.println("⚠️ Economy: Skipping invalid spend at index " + String(i) + 
                    " (amount=" + String(pendingSpends[i].amount) + ", id=" + String(pendingSpends[i].id) + ")");
      pendingSpends[i].synced = true; // Mark as synced to remove it
      continue;
    }
    
    HTTPClient http;
    String url = "https://www.ganamos.earth/api/device/economy/sync?deviceId=" + ganamosConfig.deviceId;
    
    if (!http.begin(*client, url)) {
      Serial.println("❌ Economy: http.begin() failed for spend " + String(i));
      continue;
    }
    
    http.setTimeout(5000);
    // Prefer headers for identity; keep query param temporarily for compatibility
    attachIdentityHeaders(http);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");
    
    // Build JSON payload
    StaticJsonDocument<256> doc;
    doc["spendId"] = pendingSpends[i].id;
    doc["timestamp"] = pendingSpends[i].timestamp;
    doc["amount"] = pendingSpends[i].amount;
    doc["action"] = pendingSpends[i].action;
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.println("💰 Syncing spend: " + payload);
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
      String response = http.getString();
      
      StaticJsonDocument<512> responseDoc;
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (!error && responseDoc["success"]) {
        pendingSpends[i].synced = true;
        syncedCount++;
        
        // Update local balance from server response
        if (responseDoc.containsKey("newCoinBalance")) {
          int serverBalance = responseDoc["newCoinBalance"];
          Serial.println("✅ Economy: Synced spend " + String(pendingSpends[i].id) + 
                        ", server balance: " + String(serverBalance));
        }
      } else {
        Serial.println("❌ Economy: Server rejected spend " + String(pendingSpends[i].id));
      }
    } else {
      Serial.println("❌ Economy: Sync failed (HTTP " + String(httpCode) + ")");
    }
    
    http.end();
    
    // Small delay between requests to prevent overwhelming
    delay(50);
    
    // Feed watchdog after each sync
    esp_task_wdt_reset();
  }
  
  // Clean up client ONCE at the end
  client->stop();
  delete client;
  client = nullptr;
  
  if (syncedCount > 0) {
    Serial.println("✅ Economy: Synced " + String(syncedCount) + " spends");
    savePendingSpends();
  }
  
  return syncedCount;
}

int getPendingSpendCount() {
  int unsynced = 0;
  for (int i = 0; i < pendingSpendCount; i++) {
    if (!pendingSpends[i].synced) {
      unsynced++;
    }
  }
  return unsynced;
}

void clearSyncedSpends() {
  // Feed watchdog before potentially slow operation
  esp_task_wdt_reset();
  
  Serial.println("💰 clearSyncedSpends() START - count=" + String(pendingSpendCount));
  
  // Safety: Validate pendingSpendCount before clearing
  if (pendingSpendCount < 0 || pendingSpendCount > MAX_PENDING_SPENDS) {
    Serial.println("⚠️ Invalid pendingSpendCount in clearSyncedSpends: " + String(pendingSpendCount));
    pendingSpendCount = min(max(0, pendingSpendCount), MAX_PENDING_SPENDS);
  }
  
  // Remove synced spends from queue
  int writeIdx = 0;
  for (int readIdx = 0; readIdx < pendingSpendCount && readIdx < MAX_PENDING_SPENDS; readIdx++) {
    if (!pendingSpends[readIdx].synced) {
      if (writeIdx != readIdx) {
        // Safety: Ensure null termination before copying
        pendingSpends[readIdx].id[36] = '\0';
        pendingSpends[readIdx].action[31] = '\0';
        pendingSpends[writeIdx] = pendingSpends[readIdx];
      }
      writeIdx++;
    }
  }
  
  int removed = pendingSpendCount - writeIdx;
  pendingSpendCount = writeIdx;
  
  if (removed > 0) {
    Serial.println("💰 Economy: Cleared " + String(removed) + " synced spends");
    
    // Feed watchdog before NVS write
    esp_task_wdt_reset();
    
    savePendingSpends();
  }
  
  Serial.println("💰 clearSyncedSpends() COMPLETE");
}

void clearEconomyData() {
  economyPrefs.begin("economy", false);
  economyPrefs.clear();
  economyPrefs.end();
  
  pendingSpendCount = 0;
  localCoinBalance = 0;
  
  Serial.println("🗑️ Economy: All data cleared");
}

// === Game Score Queueing Implementation ===

static PendingGameScore pendingScores[MAX_PENDING_SCORES];
static int pendingScoreCount = 0;
static Preferences scorePrefs;

static void savePendingScores() {
  if (pendingScoreCount < 0 || pendingScoreCount > MAX_PENDING_SCORES) {
    pendingScoreCount = min(max(0, pendingScoreCount), MAX_PENDING_SCORES);
  }

  memset(&scoreStateScratch, 0, sizeof(scoreStateScratch));
  scoreStateScratch.magic = SCORE_STATE_MAGIC;
  scoreStateScratch.version = SCORE_STATE_VERSION;
  scoreStateScratch.count = pendingScoreCount;
  memcpy(scoreStateScratch.scores, pendingScores, sizeof(pendingScores));
  for (int i = 0; i < pendingScoreCount; ++i) scoreStateScratch.scores[i].id[36] = '\0';
  scoreStateScratch.checksum = stateChecksum(scoreStateScratch);

  scorePrefs.begin("scores", false);
  const size_t written = scorePrefs.putBytes("stateV2", &scoreStateScratch, sizeof(scoreStateScratch));
  scorePrefs.end();
  if (written != sizeof(scoreStateScratch)) Serial.println("Scores: failed to persist complete state");
}

static void loadPendingScores() {
  scorePrefs.begin("scores", true); // read-only

  memset(&scoreStateScratch, 0, sizeof(scoreStateScratch));
  const size_t v2Length = scorePrefs.getBytesLength("stateV2");
  const bool hasV2 = v2Length == sizeof(scoreStateScratch) &&
                     scorePrefs.getBytes("stateV2", &scoreStateScratch, sizeof(scoreStateScratch)) == sizeof(scoreStateScratch) &&
                     scoreStateScratch.magic == SCORE_STATE_MAGIC &&
                     scoreStateScratch.version == SCORE_STATE_VERSION &&
                     scoreStateScratch.count <= MAX_PENDING_SCORES &&
                     scoreStateScratch.checksum == stateChecksum(scoreStateScratch);
  if (hasV2) {
    pendingScoreCount = scoreStateScratch.count;
    memcpy(pendingScores, scoreStateScratch.scores, sizeof(pendingScores));
    scorePrefs.end();
    Serial.printf("Scores: restored %d pending scores\n", pendingScoreCount);
    return;
  }
  if (v2Length > 0) {
    pendingScoreCount = 0;
    memset(pendingScores, 0, sizeof(pendingScores));
    scorePrefs.end();
    Serial.println("Scores: corrupt state rejected safely");
    return;
  }
  
  pendingScoreCount = scorePrefs.getInt("scoreCount", 0);
  if (pendingScoreCount > MAX_PENDING_SCORES) {
    pendingScoreCount = MAX_PENDING_SCORES;
  }
  
  Serial.println("🎮 Scores: Loading " + String(pendingScoreCount) + " pending scores");
  
  for (int i = 0; i < pendingScoreCount; i++) {
    String key = "score_" + String(i);
    String data = scorePrefs.getString(key.c_str(), "");
    
    if (data.length() > 0) {
      int pipe1 = data.indexOf('|');
      int pipe2 = data.indexOf('|', pipe1 + 1);
      int pipe3 = data.indexOf('|', pipe2 + 1);
      
      if (pipe1 > 0 && pipe2 > 0 && pipe3 > 0) {
        String id = data.substring(0, pipe1);
        unsigned long timestamp = data.substring(pipe1 + 1, pipe2).toInt();
        int score = data.substring(pipe2 + 1, pipe3).toInt();
        bool synced = data.substring(pipe3 + 1).toInt();
        
        strncpy(pendingScores[i].id, id.c_str(), 36);
        pendingScores[i].id[36] = '\0';
        pendingScores[i].timestamp = timestamp;
        pendingScores[i].score = score;
        pendingScores[i].synced = synced;
        
        if (!synced) {
          Serial.println("  Pending score: " + String(score));
        }
      }
    }
  }
  
  scorePrefs.end();
  savePendingScores();
}

bool queueGameScoreLocal(int score) {
  // Load scores on first call
  static bool scoresLoaded = false;
  if (!scoresLoaded) {
    loadPendingScores();
    scoresLoaded = true;
  }
  
  if (score < 0) {
    Serial.println("❌ Scores: Invalid score " + String(score));
    return false;
  }
  
  // Add to pending queue if we have room
  if (pendingScoreCount < MAX_PENDING_SCORES) {
    PendingGameScore& entry = pendingScores[pendingScoreCount];
    
    // Generate UUID
    generateUUID(entry.id);
    
    entry.timestamp = millis();
    entry.score = score;
    entry.synced = false;
    
    pendingScoreCount++;
    
    Serial.println("🎮 Scores: Queued score " + String(score) + " for sync (pending: " + String(pendingScoreCount) + ")");
    
    savePendingScores();
    return true;
  } else {
    Serial.println("Scores: queue full; score retained in gameplay only");
    return false;
  }
}

int syncPendingGameScores() {
  // Load scores on first call
  static bool scoresLoaded = false;
  if (!scoresLoaded) {
    loadPendingScores();
    scoresLoaded = true;
  }
  
  if (pendingScoreCount == 0) {
    return 0;
  }
  
  extern GanamosConfig ganamosConfig;
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ Scores: Cannot sync - no WiFi");
    return 0;
  }
  
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("⚠️ Scores: Cannot sync - no device ID");
    return 0;
  }
  
  int syncedCount = 0;
  
  for (int i = 0; i < pendingScoreCount; i++) {
    if (pendingScores[i].synced) {
      continue;
    }
    
    if (pendingScores[i].score < 0) {
      pendingScores[i].synced = true;
      continue;
    }
    
    WiFiClientSecure* client = new WiFiClientSecure;
    if (!client) {
      Serial.println("❌ Scores: Failed to create HTTP client");
      break;
    }
    
    client->setInsecure();
    client->setTimeout(5000);
    client->setHandshakeTimeout(5);
    
    HTTPClient http;
    String url = "https://www.ganamos.earth/api/device/game-score?deviceId=" + ganamosConfig.deviceId;
    
    if (!http.begin(*client, url)) {
      delete client;
      Serial.println("❌ Scores: http.begin() failed");
      continue;
    }
    
    http.setTimeout(5000);
    // Prefer headers for identity
    attachIdentityHeaders(http);
    http.addHeader("Content-Type", "application/json");
    
    StaticJsonDocument<128> doc;
    doc["score"] = pendingScores[i].score;
    
    String payload;
    serializeJson(doc, payload);
    
    Serial.println("🎮 Syncing score: " + String(pendingScores[i].score));
    
    int httpCode = http.POST(payload);
    
    if (httpCode == 200) {
      String response = http.getString();
      
      StaticJsonDocument<512> responseDoc;
      DeserializationError error = deserializeJson(responseDoc, response);
      
      if (!error && responseDoc["success"]) {
        pendingScores[i].synced = true;
        syncedCount++;
        Serial.println("✅ Scores: Synced score " + String(pendingScores[i].score));
      } else {
        Serial.println("❌ Scores: Server rejected score");
      }
    } else {
      Serial.println("❌ Scores: Sync failed (HTTP " + String(httpCode) + ")");
    }
    
    http.end();
    client->stop();
    delete client;
    
    delay(100);
  }
  
  if (syncedCount > 0) {
    Serial.println("✅ Scores: Synced " + String(syncedCount) + " scores");
    savePendingScores();
  }
  
  return syncedCount;
}

int getPendingGameScoreCount() {
  int unsynced = 0;
  for (int i = 0; i < pendingScoreCount; i++) {
    if (!pendingScores[i].synced) {
      unsynced++;
    }
  }
  return unsynced;
}

void clearSyncedGameScores() {
  int writeIdx = 0;
  for (int readIdx = 0; readIdx < pendingScoreCount && readIdx < MAX_PENDING_SCORES; readIdx++) {
    if (!pendingScores[readIdx].synced) {
      if (writeIdx != readIdx) {
        pendingScores[readIdx].id[36] = '\0';
        pendingScores[writeIdx] = pendingScores[readIdx];
      }
      writeIdx++;
    }
  }
  
  int removed = pendingScoreCount - writeIdx;
  pendingScoreCount = writeIdx;
  
  if (removed > 0) {
    Serial.println("🎮 Scores: Cleared " + String(removed) + " synced scores");
    savePendingScores();
  }
}
