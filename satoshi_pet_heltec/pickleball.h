#ifndef PICKLEBALL_H
#define PICKLEBALL_H

#include <Arduino.h>
#include "HT_SSD1306Wire.h"
#include "espnow_comm.h"

// ============================================================================
// Pickleball Multiplayer Game
// ============================================================================

// Game constants
#define PB_WINNING_SCORE     6
#define PB_REACTION_TIME_START 900   // Initial reaction window (ms)
#define PB_REACTION_TIME_MIN   450   // Fastest reaction window (ms)
#define PB_REACTION_STEP       50    // Shrinks by this much per volley
#define PB_CONFIRM_WINDOW_MS   250   // After required presses, wait this long for over-press
#define PB_BALL_TRAVEL_MS      350   // Ball animation across net (ms)
#define PB_POINT_DISPLAY_MS    1500  // Show point result (ms)
#define PB_COUNTDOWN_SECS      3     // 3-2-1 countdown before game
#define PB_SERVE_DELAY_MS      800   // Pause before next serve
#define PB_LOBBY_POLL_MS       2500  // Poll server for lobby state every 2.5s
#define PB_LOBBY_TIMEOUT_MS    100000UL // Host gives up if nobody joins in 100s
#define PB_HOST_LOST_MS        5000  // Client ends game after 5s without host packets
#define PB_ESPNOW_STATE_MS     50    // Send game state every 50ms via ESP-NOW

// Court layout coordinates for 128x64 display
// The court is divided into 4 quadrants with a net in the middle
#define PB_COURT_X       4       // Court left edge
#define PB_COURT_Y       12      // Court top edge (below score)
#define PB_COURT_W       120     // Court width
#define PB_COURT_H       48      // Court height
#define PB_NET_X         64      // Center net X position
#define PB_COURT_MID_Y   (PB_COURT_Y + PB_COURT_H / 2)  // Horizontal split Y

// Game phases
enum PickleballPhase : uint8_t {
  PB_PHASE_LOBBY,        // Waiting for players (server polling)
  PB_PHASE_ESPNOW_SYNC,  // Transitioning to ESP-NOW, confirming peers
  PB_PHASE_COUNTDOWN,     // 3-2-1 countdown
  PB_PHASE_SERVING,       // Brief pause before ball launch
  PB_PHASE_BALL_TRAVEL,   // Ball animating across net
  PB_PHASE_BALL_LANDED,   // Ball in target zone, waiting for reaction
  PB_PHASE_POINT_RESULT,  // Showing who scored
  PB_PHASE_GAME_OVER,     // Final score display
};

// Point loss reasons
enum PointLossReason : uint8_t {
  PB_MISS_TIMEOUT = 0,    // Player didn't press in time
  PB_MISS_WRONG_PLAYER,   // Wrong player on the side pressed
  PB_MISS_DOUBLE_FAULT,   // Solo player pressed wrong count
  PB_MISS_DISCONNECTED,   // Player stopped responding to presence checks
};

// Game state (shared across host and clients). Owned by the loop() task only:
// the ESP-NOW callback never touches it (packets are queued, see espnow_comm).
// Every member has a default initializer so `pbState = PickleballState{};`
// resets it safely (the String members must not be memset).
struct PickleballState {
  PickleballPhase phase = PB_PHASE_LOBBY;

  // Players
  ESPNowPlayer players[MAX_GAME_PLAYERS];
  uint8_t playerCount = 0;
  uint8_t myPlayerIndex = 0;    // This device's index in the players array
  bool isHost = false;
  bool startReceived = false;   // Client: accepted GAME_START for matchGeneration
  uint32_t matchGeneration = 0;
  uint32_t sendSequence = 0;
  uint32_t lastReceivedSequence = 0;
  uint32_t playerButtonSequence[MAX_GAME_PLAYERS] = {0, 0, 0, 0};
  bool startAcknowledged[MAX_GAME_PLAYERS] = {false, false, false, false};
  unsigned long playerLastSeen[MAX_GAME_PLAYERS] = {0, 0, 0, 0};
  unsigned long lastPresencePing = 0;
  unsigned long lastHostRx = 0;  // Client: last valid packet from the host

  // Ball state
  uint32_t volleyId = 0;        // Host increments per serve; presses carry it
  uint8_t ballSide = 0;         // 0=left, 1=right
  uint8_t ballZone = 0;         // 0=top, 1=bottom
  uint8_t targetPlayerIdx = 0;  // Who needs to hit

  // Score
  uint8_t scoreLeft = 0;
  uint8_t scoreRight = 0;
  uint8_t servingSide = 0;      // Who serves next (0=left, 1=right)

  // Timing
  unsigned long phaseStartTime = 0;
  unsigned long lastStateSend = 0;     // Last ESP-NOW state broadcast (host only)
  unsigned long lastLobbyPoll = 0;     // Last server poll during lobby
  unsigned long reactionDeadline = 0;  // When reaction window closes

  // Lobby
  String gameId;            // Server-side game ID
  String roomCode;          // Short code for optional cross-group joining
  bool lobbyCancelled = false;  // Server reported the lobby as cancelled

  // Animation
  uint8_t ballAnimFrame = 0;

  // Point result
  uint8_t lastPointWinner = 0;  // 0=left, 1=right
  uint8_t lastPointReason = 0;  // PointLossReason

  // Button tracking for press-count validation
  uint8_t buttonPressCount = 0;       // Presses received this volley
  uint8_t requiredPresses = 1;        // 1 for top zone, 2 for bottom zone
  bool hitConfirmed = false;          // Required presses reached, in confirm window
  unsigned long confirmDeadline = 0;  // End of over-press detection window

  // Rally difficulty
  uint8_t volleyCount = 0;            // Volleys in current rally (resets on point)

  // Wagering
  uint16_t wagerAmount = 0;           // 0 = free, 100/500/1000 sats
  bool wagerActive = false;           // All players accepted the wager
  int16_t wagerResult = 0;            // Sats won (+) or lost (-), set after game over
};

// ---- Public API ----

// Main entry point: runs the entire pickleball game flow
// Called from menu selection. Blocks until game ends.
// Returns happiness bonus (0 if no game played)
int handlePickleballGame(SSD1306Wire &display);

// Solo rally streak using the same reaction-window and 1/2-press rules as
// multiplayer; one miss ends it and the streak goes to the Pickleball
// leaderboard. No lobby, ESP-NOW or wagers. Applies its own happiness.
int handlePickleballPractice(SSD1306Wire &display);

#endif
