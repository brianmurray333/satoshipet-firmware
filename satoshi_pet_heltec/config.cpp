#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// Removed unused legacy DeviceConfig and loadConfig()

GanamosConfig ganamosConfig = {
  "",          // deviceId
  "Pet",       // petName
  "cat",       // petType
  "",          // userName
  0,           // balance
  0,           // coins
  0.0,         // btcPrice
  20000,       // pollInterval
  "",          // serverUrl
  "",          // lastMessage
  "",          // lastMessageType
  "",          // lastPostTitle
  "",          // lastSenderName
  100,         // gameCost (default: 100 coins per game)
  15,          // gameReward (default: 15 happiness per game)
  "",          // lastRejectionId
  "",          // rejectionMessage
  ""           // rejectionPostTitle
};
int consecutiveFailures = 0;
int lastHttpCode = 0; // Track last HTTP response code

// Economy configuration (0.05/min for both stats = 72 points/day)
EconomyConfig economyConfig = {
  72.0,        // hungerDecayPer24h (0.05/min × 1440 min)
  72.0         // happinessDecayPer24h (0.05/min × 1440 min)
};

// Jobs data - cached from server
Job cachedJobs[MAX_JOBS];
int cachedJobCount = 0;
String lastSeenJobId = "";

// Format sats with k/M suffix for compact display
String formatSatsShort(int sats) {
  if (sats >= 1000000) {
    return String(sats / 1000000.0, 1) + "M";
  } else if (sats >= 10000) {
    // 10k+ show without decimal: "15k"
    return String(sats / 1000) + "k";
  } else if (sats >= 1000) {
    // 1k-9.9k show with decimal: "1.5k"
    return String(sats / 1000.0, 1) + "k";
  }
  return String(sats);
}

bool fetchJobs() {
  // Check WiFi status first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("fetchJobs: WiFi not connected");
    return false;
  }
  
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("fetchJobs: No deviceId");
    return false;
  }
  
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) {
    return false;
  }
  
  HTTPClient http;
  
  client->setInsecure();
  client->setTimeout(5000);
  client->setHandshakeTimeout(5000);
  
  String url = "https://www.ganamos.earth/api/device/jobs?deviceId=" + ganamosConfig.deviceId;
  
  delay(100);
  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }
  
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.addHeader("Connection", "close");
  
  int httpCode = http.GET();
  
  if (httpCode != 200) {
    Serial.println("fetchJobs: HTTP error " + String(httpCode));
    http.end();
    client->stop();
    delete client;
    return false;
  }
  
  String payload = http.getString();
  
  // Use DynamicJsonDocument for larger response
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("fetchJobs: JSON parse error");
    http.end();
    client->stop();
    delete client;
    return false;
  }
  
  if (!doc["success"]) {
    Serial.println("fetchJobs: API returned success=false");
    http.end();
    client->stop();
    delete client;
    return false;
  }
  
  // Parse jobs array
  JsonArray jobs = doc["jobs"].as<JsonArray>();
  cachedJobCount = 0;
  
  for (JsonVariant jobVar : jobs) {
    if (cachedJobCount >= MAX_JOBS) break;
    
    JsonObject job = jobVar.as<JsonObject>();
    cachedJobs[cachedJobCount].id = job["id"].as<String>();
    cachedJobs[cachedJobCount].title = job["title"].as<String>();
    cachedJobs[cachedJobCount].reward = job["reward"] | 0;
    cachedJobs[cachedJobCount].location = job["location"].as<String>();
    cachedJobs[cachedJobCount].createdAt = job["createdAt"].as<String>();
    cachedJobs[cachedJobCount].groupName = job["groupName"].as<String>();
    cachedJobCount++;
  }
  
  Serial.println("fetchJobs: Loaded " + String(cachedJobCount) + " jobs");
  
  // Track newest job for notifications
  if (cachedJobCount > 0) {
    lastSeenJobId = cachedJobs[0].id;
  }
  
  http.end();
  client->stop();
  delete client;
  return true;
}

bool fetchGanamosConfig() {
  // Check WiFi status first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("fetchGanamosConfig: WiFi not connected, skipping"));
    return false;
  }
  
  // Try DNS first, fall back to cached IP if DNS fails
  // (Common pattern for IoT devices on unreliable networks)
  IPAddress serverIP;
  
  if (!WiFi.hostByName("www.ganamos.earth", serverIP) || serverIP[0] == 0) {
    // DNS failed - use fallback IP
    serverIP = IPAddress(66, 33, 60, 35);
  }
  
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) {
    return false;
  }
  
  HTTPClient http;
  
  // Configure SSL client with shorter timeout to prevent watchdog resets
  client->setInsecure();  // Skip certificate validation
  client->setTimeout(5000); // 5 second timeout (reduced from 10s to prevent watchdog)
  client->setHandshakeTimeout(5000); // 5 second SSL handshake timeout
  
  // Build URL using hostname (DNS is now resolved)
  // Strategy: Try deviceId first (most reliable, never changes), then fallback to pairingCode
  // This ensures we can reconnect even if pairing code changed but deviceId is still valid
  
  bool triedDeviceId = false;
  bool triedPairingCode = false;
  
  // Try deviceId first if available
  if (ganamosConfig.deviceId.length() > 0) {
    triedDeviceId = true;
    String url = "https://www.ganamos.earth/api/device/config?deviceId=" + ganamosConfig.deviceId;
    
    delay(100);
    if (http.begin(*client, url)) {
      http.setTimeout(5000);
      http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
      http.setReuse(false);
      http.addHeader("Connection", "close");
      
      lastHttpCode = 0;
      int httpCode = http.GET();
      lastHttpCode = httpCode;
      
      if (httpCode == 200) {
        goto process_success;
      } else if (httpCode == 404) {
        http.end();
        // Continue to try pairing code
      } else {
        http.end();
        client->stop();
        delete client;
        return false;
      }
    }
  }
  
  // Fallback: Try pairingCode if deviceId failed or wasn't available
  if (pairingCode.length() > 0 && (!triedDeviceId || lastHttpCode == 404)) {
    triedPairingCode = true;
    String url = "https://www.ganamos.earth/api/device/config?pairingCode=" + pairingCode;
    
    http.end();
    client->stop();
    delete client;
    
    client = new WiFiClientSecure;
    if (!client) {
      return false;
    }
    client->setInsecure();
    client->setTimeout(5000);
    client->setHandshakeTimeout(5000);
    
    delay(100);
    if (!http.begin(*client, url)) {
      delete client;
      return false;
    }
    
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.addHeader("Connection", "close");
    
    lastHttpCode = 0;
    int httpCode = http.GET();
    lastHttpCode = httpCode;
    
    if (httpCode == 200) {
      goto process_success;
    } else {
      http.end();
      client->stop();
      delete client;
      return false;
    }
  }
  
  http.end();
  client->stop();
  delete client;
  return false;

process_success:
  // Process successful response (200 OK)
  int httpCode = lastHttpCode;
    String payload = http.getString();
    
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      http.end();
      delete client;
      return false;
    }
    
    if (!doc["success"]) {
      http.end();
      delete client;
      return false;
    }
    
    JsonObject config = doc["config"];
    ganamosConfig.deviceId = config["deviceId"].as<String>();
    ganamosConfig.petName = config["petName"].as<String>();
    ganamosConfig.petType = config["petType"].as<String>();
    ganamosConfig.userName = config["userName"].as<String>();
    ganamosConfig.balance = config["balance"];
    ganamosConfig.coins = config["coins"] | 0; // Default to 0 if not present (deprecated)
    ganamosConfig.btcPrice = config["btcPrice"];
    ganamosConfig.pollInterval = config["pollInterval"];
    ganamosConfig.serverUrl = config["serverUrl"].as<String>();
    ganamosConfig.lastMessage = config["lastMessage"].as<String>();
    ganamosConfig.lastMessageType = config["lastMessageType"].as<String>();
    ganamosConfig.lastPostTitle = config["lastPostTitle"].as<String>();
    ganamosConfig.lastSenderName = config["lastSenderName"].as<String>();
    // Pet care costs (with defaults)
    ganamosConfig.gameCost = config["gameCost"] | 100;
    ganamosConfig.gameReward = config["gameReward"] | 15;
    
    // Parse fix rejection data
    ganamosConfig.lastRejectionId = config["lastRejectionId"].as<String>();
    ganamosConfig.rejectionMessage = config["rejectionMessage"].as<String>();
    ganamosConfig.rejectionPostTitle = config["rejectionPostTitle"].as<String>();

    // Economy parameters (with defaults)
    if (config.containsKey("hungerDecayPer24h")) {
      economyConfig.hungerDecayPer24h = config["hungerDecayPer24h"];
      economyConfig.happinessDecayPer24h = config["happinessDecayPer24h"];
    }
    
    // Apply coins earned from server
    int coinsEarned = config["coinsEarnedSinceLastSync"] | 0;
    if (coinsEarned > 0) {
      extern int getLocalCoins();
      extern void setLocalCoins(int coins);
      
      int currentCoins = getLocalCoins();
      setLocalCoins(currentCoins + coinsEarned);
    }
    
    // Check for new job notification
    bool hasNewJob = config["hasNewJob"] | false;
    if (hasNewJob) {
      String jobTitle = config["newJobTitle"].as<String>();
      int jobReward = config["newJobReward"] | 0;
      
      if (jobTitle.length() > 0 && jobReward > 0) {
      extern void triggerNewJobNotification(String title, int reward);
      extern void playNewJobChirp();  // Add this
      triggerNewJobNotification(jobTitle, jobReward);
      playNewJobChirp();
      }
    }
    
    consecutiveFailures = 0;
    
    http.end();
    client->stop();
    delete client;
    return true;
}

String pairingCode = "";

String generatePairingCode() {
  // Generate a 6-character alphanumeric code
  String code = "";
  const char chars[] = "ABCDEFGHIJKLMNPQRSTUVWXYZ123456789"; // Removed confusing chars like 0, O, I, 1
  
  for (int i = 0; i < 6; i++) {
    code += chars[random(0, sizeof(chars) - 1)];
  }
  
  pairingCode = code;
  return code;
}

// Removed unused displayPairingMode() function

Preferences preferences;

bool saveDeviceConfig() {
  preferences.begin("satoshi-pet", false);
  preferences.putString("deviceId", ganamosConfig.deviceId);
  preferences.putString("petName", ganamosConfig.petName);
  preferences.putString("petType", ganamosConfig.petType);
  preferences.putString("pairingCode", pairingCode);
  preferences.putBool("isPaired", true);
  // Save last known balance, coins, and BTC price to prevent false celebrations and show $0
  preferences.putInt("lastBalance", ganamosConfig.balance);
  preferences.putInt("lastCoins", ganamosConfig.coins);
  preferences.putFloat("lastBtcPrice", ganamosConfig.btcPrice);  // Save BTC price
  preferences.end();
  return true;
}

bool loadDeviceConfig() {
  preferences.begin("satoshi-pet", true);
  bool isPaired = preferences.getBool("isPaired", false);
  String storedDeviceId = preferences.getString("deviceId", "");
  String storedPairingCode = preferences.getString("pairingCode", "");
  int storedBalance = preferences.getInt("lastBalance", 0);
  int storedCoins = preferences.getInt("lastCoins", 0);
  float storedBtcPrice = preferences.getFloat("lastBtcPrice", 0);

  Serial.println("🔐 loadDeviceConfig() - raw values:");
  Serial.println("  isPaired: " + String(isPaired ? "true" : "false"));
  Serial.println("  deviceId: " + storedDeviceId);
  Serial.println("  pairingCode: " + storedPairingCode);
  Serial.println("  lastBalance: " + String(storedBalance));
  Serial.println("  lastCoins: " + String(storedCoins));
  Serial.println("  lastBtcPrice: " + String(storedBtcPrice, 2));

  if (isPaired && storedDeviceId.length() > 0) {
    ganamosConfig.deviceId = storedDeviceId;
    ganamosConfig.petName = preferences.getString("petName", "");
    ganamosConfig.petType = preferences.getString("petType", "");
    pairingCode = storedPairingCode;
    // Load last known balance, coins, and BTC price to prevent false celebrations and show $0
    ganamosConfig.balance = storedBalance;
    ganamosConfig.coins = storedCoins;
    ganamosConfig.btcPrice = storedBtcPrice;
    preferences.end();
    Serial.println("✅ loadDeviceConfig succeeded - deviceId restored.");
    return true;
  }

  preferences.end();
  Serial.println("⚠️ loadDeviceConfig failed - no valid pairing found.");
  return false;
}

void clearDeviceConfig() {
  preferences.begin("satoshi-pet", false);
  preferences.clear();
  preferences.end();
}

int loadLastKnownBalance() {
  preferences.begin("satoshi-pet", true);
  int balance = preferences.getInt("lastBalance", 0);
  preferences.end();
  return balance;
}

void saveLastKnownBalance(int balance) {
  preferences.begin("satoshi-pet", false);
  preferences.putInt("lastBalance", balance);
  preferences.end();
}

void saveLastKnownCoins(int coins) {
  preferences.begin("satoshi-pet", false);
  preferences.putInt("lastCoins", coins);
  preferences.end();
}

bool spendCoins(int amount, String action) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  IPAddress serverIP;
  if (!WiFi.hostByName("www.ganamos.earth", serverIP) || serverIP[0] == 0) {
    serverIP = IPAddress(66, 33, 60, 35);
  }
  
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) {
    return false;
  }
  
  HTTPClient http;
  
  // Configure SSL client
  client->setInsecure();  // Skip certificate validation
  client->setTimeout(5000); // 5 second timeout (reduced from 10s)
  client->setHandshakeTimeout(5000);  // 5 second handshake timeout
  
  String url = "https://www.ganamos.earth/api/device/spend-coins?deviceId=" + ganamosConfig.deviceId;
  
  lastHttpCode = 0;
  delay(100);
  
  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }
  
  http.setTimeout(5000); // 5 second timeout (reduced from 10s to prevent watchdog)
  
  // Create JSON payload
  StaticJsonDocument<200> doc;
  doc["amount"] = amount;
  doc["action"] = action;
  
  String payload;
  serializeJson(doc, payload);
  
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(payload);
  
  if (httpCode == 200) {
    String payload = http.getString();
    
    StaticJsonDocument<200> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, payload);
    
    if (!error && responseDoc["success"]) {
      ganamosConfig.coins = responseDoc["newCoinBalance"];
      http.end();
      client->stop();
      delete client;
      return true;
    }
  }
  
  http.end();
  client->stop();
  delete client;
  return false;
}

// Removed spendCoinsAsync() - replaced by offline economy system (spendCoinsLocal)

bool submitGameScore(int score, GameScoreResponse &response) {
  response.success = false;
  response.isNewHighScore = false;
  response.isPersonalBest = false;
  response.personalBest = 0;
  response.yourRank = 0;
  response.currentScoreRank = 0;
  response.leaderboardCount = 0;
  response.hasPersonalEntry = false;
  response.personalEntry = {};

  if (score < 0) {
    return false;
  }

  if (ganamosConfig.deviceId.length() == 0) {
    return false;
  }

  // If WiFi not connected, queue score for later sync
  if (WiFi.status() != WL_CONNECTED) {
    extern bool queueGameScoreLocal(int score);
    queueGameScoreLocal(score);
    return false;
  }

  IPAddress serverIP;
  if (!WiFi.hostByName("www.ganamos.earth", serverIP) || serverIP[0] == 0) {
    serverIP = IPAddress(66, 33, 60, 35);
  }

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) {
    return false;
  }
  client->setInsecure();
  client->setTimeout(5000);
  client->setHandshakeTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/device/game-score?deviceId=" + ganamosConfig.deviceId;

  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> payloadDoc;
  payloadDoc["score"] = score;
  String payload;
  serializeJson(payloadDoc, payload);

  int httpCode = http.POST(payload);

  if (httpCode != 200) {
    http.end();
    client->stop();
    delete client;
    return false;
  }

  String body = http.getString();

  // Safety checks
  if (body.length() > 3000 || !body.startsWith("{")) {
    http.end();
    client->stop();
    delete client;
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    http.end();
    client->stop();
    delete client;
    return false;
  }

  response.success = doc["success"] | false;
  response.isNewHighScore = doc["isNewHighScore"] | false;
  response.isPersonalBest = doc["isPersonalBest"] | false;
  response.personalBest = doc["personalBest"] | score;
  response.yourRank = doc["yourRank"] | 0;
  response.currentScoreRank = doc["currentScoreRank"] | 0;

  JsonArray leaderboard = doc["leaderboard"].as<JsonArray>();
  if (!leaderboard.isNull()) {
    int idx = 0;
    for (JsonVariant entry : leaderboard) {
      if (idx >= 4) break;
      
      if (!entry.is<JsonObject>()) {
        continue;
      }
      
      LeaderboardEntry &slot = response.leaderboard[idx];
      slot.rank = entry["rank"] | (idx + 1);
      
      // Safety: Limit pet name length to prevent buffer issues
      const char* name = entry["petName"].as<const char*>();
      if (name && strlen(name) > 0) {
        slot.petName = String(name).substring(0, 20); // Max 20 chars
      } else {
        slot.petName = "Pet";
      }
      
      slot.score = entry["score"] | 0;
      slot.isYou = entry["isYou"] | false;
      response.leaderboardCount = idx + 1;
      idx++;
    }
  }

  if (doc.containsKey("yourEntry")) {
    JsonObject personal = doc["yourEntry"].as<JsonObject>();
    if (!personal.isNull()) {
      response.hasPersonalEntry = true;
      response.personalEntry.rank = personal["rank"] | response.yourRank;
      response.personalEntry.petName = personal["petName"].as<const char*>() ? personal["petName"].as<const char*>() : ganamosConfig.petName;
      response.personalEntry.score = personal["score"] | response.personalBest;
      response.personalEntry.isYou = true;
    }
  }

  http.end();
  client->stop();
  delete client;
  return response.success;
}

int getLastHttpCode() {
  return lastHttpCode;
}

// Mark a job as complete - sends request to server which emails the poster
bool markJobComplete(String jobId) {
  // Check WiFi status first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("markJobComplete: WiFi not connected");
    return false;
  }
  
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("markJobComplete: No deviceId");
    return false;
  }
  
  if (jobId.length() == 0) {
    Serial.println("markJobComplete: No jobId");
    return false;
  }
  
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) {
    return false;
  }
  
  HTTPClient http;
  
  client->setInsecure();
  client->setTimeout(5000);
  client->setHandshakeTimeout(5000);
  
  String url = "https://www.ganamos.earth/api/device/job-complete?deviceId=" + ganamosConfig.deviceId;
  
  delay(100);
  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }
  
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  
  // Build JSON payload
  StaticJsonDocument<128> doc;
  doc["jobId"] = jobId;
  
  String payload;
  serializeJson(doc, payload);
  
  Serial.println("📋 Marking job complete: " + jobId);
  
  int httpCode = http.POST(payload);
  
  bool success = false;
  
  if (httpCode == 200) {
    String response = http.getString();
    
    StaticJsonDocument<256> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    
    if (!error && responseDoc["success"]) {
      success = true;
      Serial.println("✅ Job marked complete - notification sent to poster");
    } else {
      Serial.println("❌ Server returned error for job completion");
    }
  } else {
    Serial.println("❌ Job complete request failed (HTTP " + String(httpCode) + ")");
  }
  
  http.end();
  client->stop();
  delete client;
  
  return success;
}