#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// ============================================================================
// ESP-NOW Communication Layer for Multiplayer Games
// ============================================================================

// Maximum players in a game (pickleball doubles = 4)
#define MAX_GAME_PLAYERS 4

// Message types sent over ESP-NOW
enum ESPNowMessageType : uint8_t {
  MSG_GAME_STATE    = 0x01,  // Host → All: current game state (ball pos, score, phase)
  MSG_BUTTON_PRESS  = 0x02,  // Player → Host: player pressed their button
  MSG_GAME_START    = 0x03,  // Host → All: game is starting (includes player assignments)
  MSG_POINT_SCORED  = 0x04,  // Host → All: point result
  MSG_GAME_OVER     = 0x05,  // Host → All: game finished
  MSG_PING          = 0x06,  // Bidirectional: keepalive / latency check
  MSG_PONG          = 0x07,  // Response to ping
  MSG_LOBBY_READY   = 0x08,  // Player → Host: player is connected via ESP-NOW
  MSG_GAME_START_ACK = 0x09, // Player → Host: GAME_START generation received
};

// Player info for ESP-NOW game
struct ESPNowPlayer {
  uint8_t mac[6];
  char petInitial;       // First letter of pet name
  uint8_t side;          // 0 = left, 1 = right
  uint8_t position;      // 0 = top, 1 = bottom
  bool connected;        // ESP-NOW peer confirmed
  bool ready;            // Player sent LOBBY_READY
};

// Game state message (Host → All players, sent every ~50ms during gameplay)
// Keep this compact for fast transmission
struct __attribute__((packed)) GameStateMessage {
  uint8_t type;            // MSG_GAME_STATE
  uint32_t generation;     // Match nonce; rejects packets from prior games
  uint32_t sequence;       // Monotonic host sequence; rejects stale/duplicates
  uint8_t phase;           // 0=waiting, 1=ball_travel, 2=ball_landed, 3=point_scored
  uint8_t ballSide;        // 0=left, 1=right
  uint8_t ballZone;        // 0=top, 1=bottom
  uint8_t scoreLeft;
  uint8_t scoreRight;
  uint8_t servingSide;     // 0=left, 1=right
  uint8_t targetPlayerIdx; // Which player needs to hit (0-3)
  uint16_t reactionTimeMs; // How long player has to react
  uint32_t timestamp;      // millis() on host for sync
};

// Button press message (Player → Host)
struct __attribute__((packed)) ButtonPressMessage {
  uint8_t type;            // MSG_BUTTON_PRESS
  uint32_t generation;
  uint32_t sequence;
  uint8_t playerIndex;     // Sender's player index (0-3)
  uint32_t timestamp;      // millis() when pressed
};

// Game start message (Host → All)
struct __attribute__((packed)) GameStartMessage {
  uint8_t type;            // MSG_GAME_START
  uint32_t generation;
  uint32_t sequence;
  uint8_t playerCount;
  uint8_t yourIndex;       // Filled per-recipient before sending
  // Player assignments (petInitial, side, position for each)
  char    initials[MAX_GAME_PLAYERS];
  uint8_t sides[MAX_GAME_PLAYERS];
  uint8_t positions[MAX_GAME_PLAYERS];
};

// Point scored message (Host → All)
struct __attribute__((packed)) PointScoredMessage {
  uint8_t type;           // MSG_POINT_SCORED
  uint32_t generation;
  uint32_t sequence;
  uint8_t scoringSide;    // Which side scored (0=left, 1=right)
  uint8_t scoreLeft;
  uint8_t scoreRight;
  uint8_t reason;         // 0=miss, 1=wrong_player, 2=timeout
};

// Game over message (Host → All)
struct __attribute__((packed)) GameOverMessage {
  uint8_t type;           // MSG_GAME_OVER
  uint32_t generation;
  uint32_t sequence;
  uint8_t winnerSide;     // 0=left, 1=right
  uint8_t scoreLeft;
  uint8_t scoreRight;
};

// Simple ping/pong (bidirectional)
struct __attribute__((packed)) PingMessage {
  uint8_t type;           // MSG_PING or MSG_PONG
  uint32_t generation;
  uint32_t timestamp;
};

// Lobby ready message (Player → Host)
struct __attribute__((packed)) LobbyReadyMessage {
  uint8_t type;           // MSG_LOBBY_READY
  uint32_t generation;
  uint8_t playerIndex;
  char petInitial;
};

struct __attribute__((packed)) GameStartAckMessage {
  uint8_t type;
  uint32_t generation;
  uint8_t playerIndex;
};

// ---- Callback signature for received messages ----
typedef void (*ESPNowReceiveCallback)(const uint8_t *mac, const uint8_t *data, int len);

// ---- Public API ----

// Initialize ESP-NOW (call once). WiFi must be in STA mode.
// Returns true on success.
bool espnowInit();

// Shut down ESP-NOW and clean up peers
void espnowDeinit();

// Add a peer by MAC address (hex string "AA:BB:CC:DD:EE:FF" or raw bytes)
bool espnowAddPeer(const uint8_t mac[6]);
bool espnowAddPeerFromString(const String &macStr);

// Remove all peers
void espnowRemoveAllPeers();

// Send data to a specific peer (NULL = broadcast)
bool espnowSend(const uint8_t *peerMac, const uint8_t *data, size_t len);

// Send data to all registered peers
bool espnowBroadcast(const uint8_t *data, size_t len);

// Register callback for incoming messages
void espnowSetReceiveCallback(ESPNowReceiveCallback cb);

// Get this device's MAC address as a string "AA:BB:CC:DD:EE:FF"
String espnowGetMacString();

// Parse MAC string "AA:BB:CC:DD:EE:FF" into 6-byte array
bool espnowParseMac(const String &macStr, uint8_t mac[6]);

#endif
