#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// Forward declaration to avoid pulling HTTPClient into all includers
class HTTPClient;

extern String pairingCode;
String generatePairingCode();

bool saveDeviceConfig();
bool loadDeviceConfig();
bool markJobComplete(String jobId);
void clearDeviceConfig();

struct GanamosConfig {
  String deviceId;
  String petName;
  String petType;
  String userName;
  int balance;
  int coins;           // Pet coins for pet care (separate from balance)
  float btcPrice;
  int pollInterval;    // Seconds (server sends seconds); clamped by the sketch
  String lastMessage;  // Message from last transaction
  String lastMessageType;    // "fix" for fix rewards, "transfer" for internal transfers
  String lastPostTitle;      // Post title for fix rewards
  String lastSenderName;     // Sender name for internal transfers
  // Pet care costs (loaded from server but food uses FoodOption costs, heal unused)
  int gameCost;        // Coins per game attempt (default: 100)
  int gameReward;      // Happiness increase per successful game (default: 15)
  String lastRejectionId;    // ID of last rejected fix (to detect new rejections)
  String rejectionMessage;   // Message to show on rejection
  String rejectionPostTitle; // Post title for rejected fix
};

extern GanamosConfig ganamosConfig;
bool fetchGanamosConfig(); // Returns false on 404 (device not found)
int getLastHttpCode(); // Get last HTTP response code (0 = connection error, 200 = success, 404 = not found, etc)

struct LeaderboardEntry {
  String petName;
  int score;
  bool isYou;
  int rank;
};

struct GameScoreResponse {
  bool success;
  bool isNewHighScore;      // True if score made it into top 5
  bool isPersonalBest;       // True if score beat your personal best
  int personalBest;
  int yourRank;              // Rank based on your personal best score
  int currentScoreRank;      // Rank of the score just submitted
  int leaderboardCount;
  LeaderboardEntry leaderboard[5];
  bool hasPersonalEntry;
  LeaderboardEntry personalEntry;
};

bool submitGameScore(int score, GameScoreResponse &response);

// Balance and coins persistence to prevent false celebrations on restart
int loadLastKnownBalance();
void saveLastKnownBalance(int balance);
void saveLastKnownCoins(int coins);

// Economy config parameters
struct EconomyConfig {
  // Decay rates (points per 24 hours)
  float hungerDecayPer24h;
  float happinessDecayPer24h;
};

extern EconomyConfig economyConfig;

// Jobs feature - browse open jobs from user's private groups
#define MAX_JOBS 10

struct Job {
  String id;
  String title;       // Full title for detail view
  int reward;         // Reward in sats
  String location;    // Location string
  String createdAt;   // ISO date string
  String groupName;   // Group name for context
};

extern Job cachedJobs[MAX_JOBS];
extern int cachedJobCount;

// Fetch jobs from server - returns true if successful
bool fetchJobs();

// Format sats with k/M suffix (e.g., 1500 -> "1.5k", 2000000 -> "2M")
String formatSatsShort(int sats);

// === Shared HTTPS client for the Ganamos API ===
// Prepares `http` for a request to https://www.ganamos.earth<path> using one
// persistent, CA-verified WiFiClientSecure (ca_roots.h), keep-alive enabled,
// ~5 s connect/read timeouts, and the standard device headers
// (X-Device-Id, X-Pairing-Code, Content-Type: application/json).
// Returns false if WiFi is not connected or begin() fails.
// Callers then do http.GET()/http.POST(), read the response, and call http.end().
bool ganamosHttpBegin(HTTPClient& http, const String& path);
// Closes the shared TLS connection (call when WiFi goes down/off).
void ganamosHttpReset();

// === HTTP identity handling ===
// Attaches device identity headers to every HTTP request.
// - X-Device-Id: current device id, when available
// - X-Pairing-Code: current pairing code, when available
// Note: deviceId remains in the query string temporarily for one release as a compatibility fallback.
void attachIdentityHeaders(HTTPClient& http);

// Returns a redacted form of a secret for logs (first 4 chars + "****")
String maskForLog(const String& value);

// === Time zone / NTP ===
// Starts SNTP with the saved POSIX TZ (NVS) or the default US Pacific rule.
void startTimeSync();

// === WiFi fast-reconnect cache (BSSID + channel of the last good AP) ===
bool loadWifiFastConnect(uint8_t bssid[6], int32_t& channel);
void saveWifiFastConnect(const uint8_t* bssid, int32_t channel);
void clearWifiFastConnect();

// Defined in satoshi_pet_heltec.ino: starts (if needed) a non-blocking WiFi
// connect and waits up to timeoutMs for an IP, feeding the watchdog. Never
// calls WiFi.begin() while a connect is already pending. For user-initiated
// network actions only.
bool wifiEnsureConnected(unsigned long timeoutMs);

#endif
