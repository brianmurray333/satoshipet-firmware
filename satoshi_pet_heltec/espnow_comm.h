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

// Wire protocol version. It lives in the high nibble of every packet's type
// byte: type = (version << 4) | kind. Protocol v1 firmware used bare kinds
// (0x01-0x09) and ignores any other type byte, so v1 and v2 devices silently
// reject each other's packets instead of misreading changed layouts.
// v2: GameStateMessage and ButtonPressMessage gained a trailing volleyId.
#define ESPNOW_PROTOCOL_VERSION 2
#define ESPNOW_MSG_TYPE(kind) ((uint8_t)((ESPNOW_PROTOCOL_VERSION << 4) | (kind)))
#define ESPNOW_MSG_VERSION(type) ((uint8_t)((type) >> 4))

// Message types sent over ESP-NOW
enum ESPNowMessageType : uint8_t {
  MSG_GAME_STATE     = ESPNOW_MSG_TYPE(0x01),  // Host -> All: current game state
  MSG_BUTTON_PRESS   = ESPNOW_MSG_TYPE(0x02),  // Player -> Host: player pressed their button
  MSG_GAME_START     = ESPNOW_MSG_TYPE(0x03),  // Host -> All: game is starting (player assignments)
  MSG_POINT_SCORED   = ESPNOW_MSG_TYPE(0x04),  // Host -> All: point result
  MSG_GAME_OVER      = ESPNOW_MSG_TYPE(0x05),  // Host -> All: game finished
  MSG_PING           = ESPNOW_MSG_TYPE(0x06),  // Host -> All: keepalive
  MSG_PONG           = ESPNOW_MSG_TYPE(0x07),  // Player -> Host: response to ping
  MSG_LOBBY_READY    = ESPNOW_MSG_TYPE(0x08),  // Player -> Host: player is connected via ESP-NOW
  MSG_GAME_START_ACK = ESPNOW_MSG_TYPE(0x09),  // Player -> Host: GAME_START generation received
};

// Player info for ESP-NOW game
struct ESPNowPlayer {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  char petInitial = 0;       // First letter of pet name
  uint8_t side = 0;          // 0 = left, 1 = right
  uint8_t position = 0;      // 0 = top, 1 = bottom
  bool connected = false;    // ESP-NOW peer confirmed
  bool ready = false;        // Player sent LOBBY_READY
};

// Game state message (Host -> All players, sent every ~50ms during gameplay)
struct __attribute__((packed)) GameStateMessage {
  uint8_t type;            // MSG_GAME_STATE
  uint32_t generation;     // Match nonce; rejects packets from prior games
  uint32_t sequence;       // Monotonic host sequence; rejects stale/duplicates
  uint8_t phase;           // PickleballPhase
  uint8_t ballSide;        // 0=left, 1=right
  uint8_t ballZone;        // 0=top, 1=bottom
  uint8_t scoreLeft;
  uint8_t scoreRight;
  uint8_t servingSide;     // 0=left, 1=right
  uint8_t targetPlayerIdx; // Which player needs to hit (0-3)
  uint16_t reactionTimeMs; // How long player has to react
  uint32_t timestamp;      // millis() on host for sync
  uint32_t volleyId;       // v2: host volley counter; clients echo it in presses
};

// Button press message (Player -> Host)
struct __attribute__((packed)) ButtonPressMessage {
  uint8_t type;            // MSG_BUTTON_PRESS
  uint32_t generation;
  uint32_t sequence;
  uint8_t playerIndex;     // Sender's player index (0-3)
  uint32_t timestamp;      // millis() when pressed
  uint32_t volleyId;       // v2: volley this press belongs to; host drops stale ones
};

// Game start message (Host -> All)
struct __attribute__((packed)) GameStartMessage {
  uint8_t type;            // MSG_GAME_START
  uint32_t generation;
  uint32_t sequence;
  uint8_t playerCount;
  uint8_t yourIndex;       // Filled per-recipient before sending
  char    initials[MAX_GAME_PLAYERS];
  uint8_t sides[MAX_GAME_PLAYERS];
  uint8_t positions[MAX_GAME_PLAYERS];
};

// Point scored message (Host -> All). Sent 3x with the same sequence.
struct __attribute__((packed)) PointScoredMessage {
  uint8_t type;           // MSG_POINT_SCORED
  uint32_t generation;
  uint32_t sequence;
  uint8_t scoringSide;    // Which side scored (0=left, 1=right)
  uint8_t scoreLeft;
  uint8_t scoreRight;
  uint8_t reason;         // PointLossReason
};

// Game over message (Host -> All). Sent 3x with the same sequence.
struct __attribute__((packed)) GameOverMessage {
  uint8_t type;           // MSG_GAME_OVER
  uint32_t generation;
  uint32_t sequence;
  uint8_t winnerSide;     // 0=left, 1=right
  uint8_t scoreLeft;
  uint8_t scoreRight;
};

// Simple ping/pong
struct __attribute__((packed)) PingMessage {
  uint8_t type;           // MSG_PING or MSG_PONG
  uint32_t generation;
  uint32_t timestamp;
};

// Lobby ready message (Player -> Host)
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

// ---- Receive queue ----
// The ESP-NOW receive callback runs in the WiFi task (core 0). It only checks
// the packet's type/length and copies it into a FreeRTOS queue; game code
// drains the queue from loop() with espnowReceive().
#define ESPNOW_RX_MAX_LEN   48
#define ESPNOW_RX_QUEUE_LEN 16

struct ESPNowRxPacket {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[ESPNOW_RX_MAX_LEN];
};

// ---- Public API ----

// Initialize ESP-NOW (call once). WiFi must be in STA mode.
// Returns true on success.
bool espnowInit();

// Shut down ESP-NOW, clean up peers and discard queued packets
void espnowDeinit();

// Add a peer by raw MAC bytes (uses the current WiFi channel)
bool espnowAddPeer(const uint8_t mac[6]);

// Remove all peers
void espnowRemoveAllPeers();

// Send data to a specific peer
bool espnowSend(const uint8_t *peerMac, const uint8_t *data, size_t len);

// Send data to all registered peers
bool espnowBroadcast(const uint8_t *data, size_t len);

// Pop one received packet (non-blocking). Returns false if the queue is empty.
bool espnowReceive(ESPNowRxPacket &out);

// Diagnostics counters (incremented from WiFi-task callbacks)
uint32_t espnowRxDroppedCount();   // queue full / malformed packets
uint32_t espnowTxFailedCount();    // send callback reported failure

// Get this device's MAC address as a string "AA:BB:CC:DD:EE:FF"
String espnowGetMacString();

// Parse MAC string "AA:BB:CC:DD:EE:FF" into 6-byte array
bool espnowParseMac(const char *macStr, uint8_t mac[6]);
bool espnowParseMac(const String &macStr, uint8_t mac[6]);

#endif
