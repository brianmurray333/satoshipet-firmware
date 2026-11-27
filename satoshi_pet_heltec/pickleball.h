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

// With 3 players the CPU takes the 4th quadrant, addressed as this index
#define PB_CPU_IDX       3
#define PB_CPU_HIT_PCT_START  92   // CPU return rate on the first volley...
#define PB_CPU_HIT_PCT_STEP   3    // ...dropping this much per volley in the rally
#define PB_CPU_HIT_PCT_MIN    60

// Game phases
enum PickleballPhase : uint8_t {
  PB_PHASE_LOBBY,        // Not in a match yet (the room screens handle the lobby)
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
  unsigned long reactionDeadline = 0;  // When reaction window closes

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

  // CPU partner: with 3 players it fills the empty quadrant. Every pet derives
  // it from the same roster, so the wire protocol is unchanged; it is addressed
  // as player index PB_CPU_IDX (== playerCount). Only the host plays its shots.
  bool cpuActive = false;
  uint8_t cpuSide = 0;
  uint8_t cpuPosition = 0;
  bool cpuWillHit = false;            // Host: decided when the ball lands
  unsigned long cpuHitAt = 0;         // Host: when the CPU swings

};

// ---- Public API ----

// One player in a room match, in seat order (seat 0 = host).
struct PbRosterEntry {
  uint8_t mac[6];
  char initial;
  uint8_t side;      // 0 = left, 1 = right
  uint8_t position;  // 0 = top, 1 = bottom
};

struct PbMatchOutcome {
  bool finished = false;   // a winner was decided on this pet
  bool iWon = false;
  uint8_t winnerSide = 0;  // 0 = left, 1 = right
  uint8_t scoreLeft = 0;
  uint8_t scoreRight = 0;
};

// Live match for a started game room: links the pets over ESP-NOW (the room
// already holds the bets and decided the roster), plays to 6, and fills `out`.
// The caller reports the result to the room. Blocks until the match ends.
// Returns the happiness bonus (0 if the match didn't finish).
int handlePickleballRoomMatch(SSD1306Wire &display, const PbRosterEntry *roster, uint8_t count,
                              uint8_t myIndex, uint32_t matchGeneration, PbMatchOutcome &out);

// Solo rally streak using the same reaction-window and 1/2-press rules as
// multiplayer; one miss ends it and the streak goes to the Pickleball
// leaderboard. No room, ESP-NOW or bets. Applies its own happiness.
int handlePickleballPractice(SSD1306Wire &display);

#endif
