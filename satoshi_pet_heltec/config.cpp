#include "config.h"
#include "ca_roots.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <time.h>

GanamosConfig ganamosConfig = {
  "",          // deviceId
  "Pet",       // petName
  "cat",       // petType
  "",          // userName
  0,           // balance
  0,           // coins
  0.0,         // btcPrice
  60,          // pollInterval (seconds)
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
int lastHttpCode = 0; // Track last HTTP response code

static const char GANAMOS_BASE_URL[] = "https://www.ganamos.earth";
static const uint16_t GANAMOS_HTTP_TIMEOUT_MS = 5000;
static WiFiClientSecure* ganamosClient = nullptr;
// One long-lived HTTPClient for this file's requests. HTTPClient's destructor
// stops its client, so a function-local HTTPClient would close the shared
// keep-alive connection every time it goes out of scope.
static HTTPClient ganamosHttp;
// Requests within one sync burst reuse the connection; after a longer idle
// gap the server has likely closed it, so start fresh instead of failing on
// a stale socket.
static const unsigned long GANAMOS_KEEPALIVE_IDLE_MS = 10000;
static unsigned long ganamosLastBegin = 0;

// Attach device identity headers to a request
void attachIdentityHeaders(HTTPClient& http) {
  // Send device identity via headers (primary)
  if (ganamosConfig.deviceId.length() > 0) {
    http.addHeader("X-Device-Id", ganamosConfig.deviceId);
  }
  if (pairingCode.length() > 0) {
    http.addHeader("X-Pairing-Code", pairingCode);
  }
}

bool ganamosHttpBegin(HTTPClient& http, const String& path) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (!ganamosClient) {
    // Created once and kept for the life of the firmware: avoids heap churn
    // and lets HTTPClient reuse an open TLS session (keep-alive).
    ganamosClient = new WiFiClientSecure();
    if (!ganamosClient) {
      return false;
    }
    ganamosClient->setCACert(GANAMOS_ROOT_CAS);
    ganamosClient->setHandshakeTimeout(GANAMOS_HTTP_TIMEOUT_MS / 1000);
  }

  unsigned long now = millis();
  if (ganamosLastBegin != 0 && now - ganamosLastBegin > GANAMOS_KEEPALIVE_IDLE_MS) {
    ganamosClient->stop();
  }
  ganamosLastBegin = now;

  if (!http.begin(*ganamosClient, String(GANAMOS_BASE_URL) + path)) {
    return false;
  }

  http.setReuse(true);
  http.setConnectTimeout(GANAMOS_HTTP_TIMEOUT_MS);
  http.setTimeout(GANAMOS_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  attachIdentityHeaders(http);
  http.addHeader("Content-Type", "application/json");
  return true;
}

void ganamosHttpReset() {
  if (ganamosClient) {
    ganamosClient->stop();
  }
}

// Redact secrets for logs: show first 4 characters only
String maskForLog(const String& value) {
  if (value.length() == 0) return "";
  int visible = value.length() < 4 ? value.length() : 4;
  return value.substring(0, visible) + "****";
}

// Assign only when the value changed, to avoid needless heap reallocations.
static void assignIfChanged(String& dst, JsonVariantConst value) {
  const char* s = value.as<const char*>();
  if (!s) s = "";
  if (dst != s) dst = s;
}

// Safe offline defaults match the live server configuration.
EconomyConfig economyConfig = {
  40.0,        // hungerDecayPer24h
  25.0         // happinessDecayPer24h
};

// Jobs data - cached from server
Job cachedJobs[MAX_JOBS];
int cachedJobCount = 0;

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

// Parse a JSON response body. Streams straight from the socket when the
// length is known; falls back to a String for chunked/unknown-length bodies
// (ArduinoJson cannot de-chunk) so keep-alive stays possible.
static DeserializationError parseJsonResponse(HTTPClient& http, JsonDocument& doc,
                                              JsonDocument& filter, int maxBytes) {
  int size = http.getSize();
  if (size > maxBytes) {
    return DeserializationError::InvalidInput;
  }
  if (size > 0) {
    return deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  }
  String body = http.getString();
  if ((int)body.length() > maxBytes) {
    return DeserializationError::InvalidInput;
  }
  return deserializeJson(doc, body, DeserializationOption::Filter(filter));
}

bool fetchJobs() {
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("fetchJobs: No deviceId");
    return false;
  }

  HTTPClient& http = ganamosHttp;
  if (!ganamosHttpBegin(http, "/api/device/jobs?deviceId=" + ganamosConfig.deviceId)) {
    Serial.println("fetchJobs: WiFi not connected");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.println("fetchJobs: HTTP error " + String(httpCode));
    http.end();
    if (httpCode < 0) ganamosHttpReset();
    return false;
  }

  JsonDocument filter;
  filter["success"] = true;
  JsonObject jobFilter = filter["jobs"].add<JsonObject>();
  jobFilter["id"] = true;
  jobFilter["title"] = true;
  jobFilter["reward"] = true;
  jobFilter["location"] = true;
  jobFilter["createdAt"] = true;
  jobFilter["groupName"] = true;

  JsonDocument doc;
  DeserializationError error = parseJsonResponse(http, doc, filter, 16384);
  http.end();

  if (error) {
    Serial.println("fetchJobs: JSON parse error");
    return false;
  }

  if (!(doc["success"] | false)) {
    Serial.println("fetchJobs: API returned success=false");
    return false;
  }

  cachedJobCount = 0;
  for (JsonVariantConst jobVar : doc["jobs"].as<JsonArrayConst>()) {
    if (cachedJobCount >= MAX_JOBS) break;
    JsonObjectConst job = jobVar.as<JsonObjectConst>();
    Job& slot = cachedJobs[cachedJobCount];
    assignIfChanged(slot.id, job["id"]);
    assignIfChanged(slot.title, job["title"]);
    slot.reward = job["reward"] | 0;
    assignIfChanged(slot.location, job["location"]);
    assignIfChanged(slot.createdAt, job["createdAt"]);
    assignIfChanged(slot.groupName, job["groupName"]);
    cachedJobCount++;
  }

  Serial.println("fetchJobs: Loaded " + String(cachedJobCount) + " jobs");
  return true;
}

// ===== Time zone =====
static const char DEFAULT_TZ[] = "PST8PDT,M3.2.0,M11.1.0";
static String currentTz;

// A POSIX TZ string ("PST8PDT,M3.2.0,M11.1.0", "CET-1CEST,...", "<+0530>-5:30")
// always has an offset digit and never contains '/' (IANA names do).
static bool isPosixTz(const char* tz) {
  if (!tz) return false;
  size_t len = strlen(tz);
  if (len < 4 || len > 60) return false;
  if (strchr(tz, '/') != nullptr) return false;
  for (size_t i = 0; i < len; i++) {
    if (isdigit((unsigned char)tz[i])) return true;
  }
  return false;
}

void startTimeSync() {
  if (currentTz.length() == 0) {
    Preferences tzPrefs;
    String saved;
    if (tzPrefs.begin("satoshi-pet", true)) {
      if (tzPrefs.isKey("tz")) saved = tzPrefs.getString("tz", "");
      tzPrefs.end();
    }
    currentTz = isPosixTz(saved.c_str()) ? saved : String(DEFAULT_TZ);
  }
  configTzTime(currentTz.c_str(), "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP time sync initiated (TZ " + currentTz + ")");
}

static void applyServerTimezone(const char* tz) {
  if (!isPosixTz(tz) || currentTz == tz) return;
  currentTz = tz;
  Preferences tzPrefs;
  if (tzPrefs.begin("satoshi-pet", false)) {
    tzPrefs.putString("tz", currentTz);
    tzPrefs.end();
  }
  setenv("TZ", currentTz.c_str(), 1);
  tzset();
  Serial.println("Timezone updated from server: " + currentTz);
}

// ===== Device config =====
static bool requestConfigOnce(HTTPClient& http, const String& path) {
  lastHttpCode = 0;
  if (!ganamosHttpBegin(http, path)) {
    return false;
  }
  unsigned long started = millis();
  lastHttpCode = http.GET();
  if (lastHttpCode == 200) {
    return true;
  }
  Serial.printf("Config request failed after %lums: %d %s\n", millis() - started,
                lastHttpCode,
                lastHttpCode < 0 ? HTTPClient::errorToString(lastHttpCode).c_str() : "");
  if (lastHttpCode > 0) {
    http.getString(); // Drain the error body so the connection stays reusable
  }
  http.end();
  if (lastHttpCode < 0) {
    ganamosHttpReset(); // Transport error: don't reuse this socket
  }
  return false;
}

// A transport error (no HTTP response at all) is retried once on a fresh
// connection: the first request after the radio wakes can fail while the
// network path settles, and one retry is cheaper than showing a warning and
// waiting for the next poll.
static bool requestConfig(HTTPClient& http, const String& path) {
  if (requestConfigOnce(http, path)) return true;
  if (lastHttpCode >= 0 || WiFi.status() != WL_CONNECTED) return false;
  esp_task_wdt_reset();
  Serial.println(F("Retrying config request on a new connection"));
  return requestConfigOnce(http, path);
}

static bool parseConfigResponse(HTTPClient& http) {
  JsonDocument filter;
  filter["success"] = true;
  JsonObject f = filter["config"].to<JsonObject>();
  const char* const fields[] = {
    "deviceId", "petName", "petType", "userName", "balance", "coins", "btcPrice",
    "pollInterval", "lastMessage", "lastMessageType", "lastPostTitle",
    "lastSenderName", "gameCost", "gameReward", "lastRejectionId",
    "rejectionMessage", "rejectionPostTitle", "hungerDecayPer24h",
    "happinessDecayPer24h", "coinsEarnedSinceLastSync", "hasNewJob",
    "newJobTitle", "newJobReward", "timezone", "tz"
  };
  for (const char* field : fields) {
    f[field] = true;
  }

  JsonDocument doc;
  DeserializationError error = parseJsonResponse(http, doc, filter, 8192);
  if (error) {
    Serial.println("fetchGanamosConfig: JSON parse error: " + String(error.c_str()));
    return false;
  }
  if (!(doc["success"] | false)) {
    return false;
  }

  JsonObjectConst config = doc["config"].as<JsonObjectConst>();
  const char* deviceId = config["deviceId"].as<const char*>();
  if (deviceId && deviceId[0] && ganamosConfig.deviceId != deviceId) {
    ganamosConfig.deviceId = deviceId;
  }
  if (config["petName"].is<const char*>()) assignIfChanged(ganamosConfig.petName, config["petName"]);
  if (config["petType"].is<const char*>()) assignIfChanged(ganamosConfig.petType, config["petType"]);
  assignIfChanged(ganamosConfig.userName, config["userName"]);
  ganamosConfig.balance = config["balance"] | ganamosConfig.balance;
  ganamosConfig.coins = config["coins"] | 0; // Default to 0 if not present (deprecated)
  ganamosConfig.btcPrice = config["btcPrice"] | ganamosConfig.btcPrice;
  ganamosConfig.pollInterval = config["pollInterval"] | 60; // seconds
  assignIfChanged(ganamosConfig.lastMessage, config["lastMessage"]);
  assignIfChanged(ganamosConfig.lastMessageType, config["lastMessageType"]);
  assignIfChanged(ganamosConfig.lastPostTitle, config["lastPostTitle"]);
  assignIfChanged(ganamosConfig.lastSenderName, config["lastSenderName"]);
  // Pet care costs (with defaults)
  ganamosConfig.gameCost = config["gameCost"] | 100;
  ganamosConfig.gameReward = config["gameReward"] | 15;

  // Parse fix rejection data
  assignIfChanged(ganamosConfig.lastRejectionId, config["lastRejectionId"]);
  assignIfChanged(ganamosConfig.rejectionMessage, config["rejectionMessage"]);
  assignIfChanged(ganamosConfig.rejectionPostTitle, config["rejectionPostTitle"]);

  // Economy parameters (with defaults)
  if (config["hungerDecayPer24h"].is<float>()) {
    economyConfig.hungerDecayPer24h = config["hungerDecayPer24h"].as<float>();
  }
  if (config["happinessDecayPer24h"].is<float>()) {
    economyConfig.happinessDecayPer24h = config["happinessDecayPer24h"].as<float>();
  }

  // Optional POSIX time zone for quiet hours / pet clock
  const char* tz = config["timezone"].is<const char*>() ? config["timezone"].as<const char*>()
                                                        : config["tz"].as<const char*>();
  if (tz) applyServerTimezone(tz);

  // Apply coins earned from server
  int coinsEarned = config["coinsEarnedSinceLastSync"] | 0;
  if (coinsEarned > 0) {
    extern int getLocalCoins();
    extern void setLocalCoins(int coins);
    setLocalCoins(getLocalCoins() + coinsEarned);
  }

  // Check for new job notification (triggerNewJobNotification plays the chirp)
  if (config["hasNewJob"] | false) {
    const char* jobTitle = config["newJobTitle"] | "";
    int jobReward = config["newJobReward"] | 0;
    if (jobTitle[0] && jobReward > 0) {
      extern void triggerNewJobNotification(String title, int reward);
      triggerNewJobNotification(String(jobTitle), jobReward);
    }
  }
  return true;
}

bool fetchGanamosConfig() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("fetchGanamosConfig: WiFi not connected, skipping"));
    lastHttpCode = 0;
    return false;
  }

  extern bool isPaired;
  HTTPClient& http = ganamosHttp;
  bool haveDeviceId = ganamosConfig.deviceId.length() > 0;
  bool ok = false;

  // Strategy: try deviceId first (stable). The pairing code is only a
  // fallback while pairing is still in progress (no deviceId yet, or the
  // deviceId was rejected and the device is not paired).
  if (haveDeviceId) {
    ok = requestConfig(http, "/api/device/config?deviceId=" + ganamosConfig.deviceId);
  }
  if (!ok && pairingCode.length() > 0 &&
      (!haveDeviceId || (lastHttpCode == 404 && !isPaired))) {
    esp_task_wdt_reset();
    ok = requestConfig(http, "/api/device/config?pairingCode=" + pairingCode);
  }
  if (!ok) {
    return false;
  }

  bool parsed = parseConfigResponse(http);
  http.end();
  return parsed;
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
  Serial.println("  deviceId: " + maskForLog(storedDeviceId));
  Serial.println("  pairingCode: " + maskForLog(storedPairingCode));
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

// ===== WiFi fast-reconnect cache =====
// Stored in its own namespace and only rewritten when the AP changes, so a
// normal reconnect costs no flash writes.
bool loadWifiFastConnect(uint8_t bssid[6], int32_t& channel) {
  Preferences wifiPrefs;
  if (!wifiPrefs.begin("sp-wifi", true)) return false;
  bool ok = false;
  if (wifiPrefs.isKey("bssid") && wifiPrefs.isKey("chan")) {
    ok = wifiPrefs.getBytes("bssid", bssid, 6) == 6;
    channel = wifiPrefs.getInt("chan", 0);
    ok = ok && channel >= 1 && channel <= 14;
  }
  wifiPrefs.end();
  return ok;
}

void saveWifiFastConnect(const uint8_t* bssid, int32_t channel) {
  if (!bssid || channel < 1 || channel > 14) return;
  uint8_t oldBssid[6];
  int32_t oldChannel = 0;
  if (loadWifiFastConnect(oldBssid, oldChannel) && oldChannel == channel &&
      memcmp(oldBssid, bssid, 6) == 0) {
    return;
  }
  Preferences wifiPrefs;
  if (!wifiPrefs.begin("sp-wifi", false)) return;
  wifiPrefs.putBytes("bssid", bssid, 6);
  wifiPrefs.putInt("chan", channel);
  wifiPrefs.end();
}

void clearWifiFastConnect() {
  Preferences wifiPrefs;
  if (!wifiPrefs.begin("sp-wifi", false)) return;
  wifiPrefs.clear();
  wifiPrefs.end();
}

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

  HTTPClient& http = ganamosHttp;
  if (!ganamosHttpBegin(http, "/api/device/game-score?deviceId=" + ganamosConfig.deviceId)) {
    return false;
  }

  JsonDocument payloadDoc;
  payloadDoc["score"] = score;
  String payload;
  serializeJson(payloadDoc, payload);

  int httpCode = http.POST(payload);
  if (httpCode != 200) {
    http.end();
    if (httpCode < 0) ganamosHttpReset();
    return false;
  }

  String body = http.getString();
  http.end();

  // Safety checks
  if (body.length() > 3000 || !body.startsWith("{")) {
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    return false;
  }

  response.success = doc["success"] | false;
  response.isNewHighScore = doc["isNewHighScore"] | false;
  response.isPersonalBest = doc["isPersonalBest"] | false;
  response.personalBest = doc["personalBest"] | score;
  response.yourRank = doc["yourRank"] | 0;
  response.currentScoreRank = doc["currentScoreRank"] | 0;

  JsonArrayConst leaderboard = doc["leaderboard"].as<JsonArrayConst>();
  if (!leaderboard.isNull()) {
    int idx = 0;
    for (JsonVariantConst entry : leaderboard) {
      if (idx >= 4) break;

      if (!entry.is<JsonObjectConst>()) {
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

  if (doc["yourEntry"].is<JsonObjectConst>()) {
    JsonObjectConst personal = doc["yourEntry"].as<JsonObjectConst>();
    response.hasPersonalEntry = true;
    response.personalEntry.rank = personal["rank"] | response.yourRank;
    const char* personalName = personal["petName"].as<const char*>();
    response.personalEntry.petName = personalName ? String(personalName) : ganamosConfig.petName;
    response.personalEntry.score = personal["score"] | response.personalBest;
    response.personalEntry.isYou = true;
  }

  return response.success;
}

int getLastHttpCode() {
  return lastHttpCode;
}

// Mark a job as complete - sends request to server which emails the poster
bool markJobComplete(String jobId) {
  if (ganamosConfig.deviceId.length() == 0) {
    Serial.println("markJobComplete: No deviceId");
    return false;
  }

  if (jobId.length() == 0) {
    Serial.println("markJobComplete: No jobId");
    return false;
  }

  HTTPClient& http = ganamosHttp;
  if (!ganamosHttpBegin(http, "/api/device/job-complete?deviceId=" + ganamosConfig.deviceId)) {
    Serial.println("markJobComplete: WiFi not connected");
    return false;
  }

  JsonDocument doc;
  doc["jobId"] = jobId;
  String payload;
  serializeJson(doc, payload);

  Serial.println("📋 Marking job complete: " + jobId);

  int httpCode = http.POST(payload);
  bool success = false;

  if (httpCode == 200) {
    JsonDocument filter;
    filter["success"] = true;
    JsonDocument responseDoc;
    DeserializationError error = parseJsonResponse(http, responseDoc, filter, 2048);

    if (!error && (responseDoc["success"] | false)) {
      success = true;
      Serial.println("✅ Job marked complete - notification sent to poster");
    } else {
      Serial.println("❌ Server returned error for job completion");
    }
  } else {
    Serial.println("❌ Job complete request failed (HTTP " + String(httpCode) + ")");
  }

  http.end();
  if (httpCode < 0) ganamosHttpReset();
  return success;
}
