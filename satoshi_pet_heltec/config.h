// Test comment - delete me
#ifndef CONFIG_H
#include <Preferences.h>
#define CONFIG_H


#include <ArduinoJson.h>

struct DeviceConfig {
  String pet;
  String lnurl;
  int pollInterval;
};

extern DeviceConfig config;
extern String currentPet;
extern String pairingCode;
bool displayPairingMode();
String generatePairingCode();

bool loadConfig();
bool saveDeviceConfig();
bool loadDeviceConfig();
bool markJobComplete(String jobId);
void clearDeviceConfig();
int fetchBTCPrice();
int fetchSatoshiBalance(String lnurl);

struct GanamosConfig {
  String deviceId;
  String petName;
  String petType;
  String userName;
  int balance;
  int coins;           // Pet coins for pet care (separate from balance)
  float btcPrice;
  int pollInterval;
  String serverUrl;
  String lastMessage;  // Message from last transaction
  // Pet care costs (loaded from server but food uses FoodOption costs, heal unused)
  int gameCost;        // Coins per game attempt (default: 100)
  int gameReward;      // Happiness increase per successful game (default: 15)
  String lastRejectionId;    // ID of last rejected fix (to detect new rejections)
  String rejectionMessage;   // Message to show on rejection
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

// Pet care functions
bool spendCoins(int amount, String action); // Spend coins for feed/heal/game actions

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
extern String lastSeenJobId;  // Track newest job for notifications

// Fetch jobs from server - returns true if successful
bool fetchJobs();

// Format sats with k/M suffix (e.g., 1500 -> "1.5k", 2000000 -> "2M")
String formatSatsShort(int sats);

#endif