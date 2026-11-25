#include "pickleball.h"
#include "config.h"
#include "button_handler.h"
#include "sound.h"
#include "hold_press.h"
#include "economy.h"
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================================================
// Pickleball Game Implementation
// ============================================================================

// Global game state. Only the loop() task reads or writes it: the ESP-NOW
// receive callback (WiFi task) just queues raw packets, which are applied by
// pbProcessRx() from the game loop.
static PickleballState pbState;

// This device's STA MAC (filled at game start)
static uint8_t pbMyMac[6] = {0, 0, 0, 0, 0, 0};

// Countdown beep tracker (reset at every match start)
static int pbLastBeep = -1;

// ---- Pending button presses (host only; loop task only) ----
struct PendingButtonPress { uint8_t playerIndex; uint32_t volleyId; };
#define PB_PRESS_QUEUE_LEN 8
static PendingButtonPress pbButtonQueue[PB_PRESS_QUEUE_LEN];
static uint8_t pbButtonQueueHead = 0;
static uint8_t pbButtonQueueTail = 0;

static void flushButtonQueue() {
  pbButtonQueueHead = 0;
  pbButtonQueueTail = 0;
}

static bool queueButtonPress(uint8_t playerIndex, uint32_t volleyId) {
  uint8_t next = (pbButtonQueueHead + 1) % PB_PRESS_QUEUE_LEN;
  if (next == pbButtonQueueTail) return false;
  pbButtonQueue[pbButtonQueueHead].playerIndex = playerIndex;
  pbButtonQueue[pbButtonQueueHead].volleyId = volleyId;
  pbButtonQueueHead = next;
  return true;
}

static bool dequeueButtonPress(PendingButtonPress &press) {
  if (pbButtonQueueTail == pbButtonQueueHead) return false;
  press = pbButtonQueue[pbButtonQueueTail];
  pbButtonQueueTail = (pbButtonQueueTail + 1) % PB_PRESS_QUEUE_LEN;
  return true;
}

static int playerIndexForMac(const uint8_t *mac) {
  for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++)
    if (memcmp(pbState.players[i].mac, mac, 6) == 0) return i;
  return -1;
}

// Clients accept host messages only from players[0] (the lobby creator).
static bool isFromHost(const uint8_t *mac) {
  return pbState.playerCount > 0 && memcmp(pbState.players[0].mac, mac, 6) == 0;
}

static bool macIsZero(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) if (mac[i]) return false;
  return true;
}

static uint8_t mySide() {
  return pbState.myPlayerIndex < MAX_GAME_PLAYERS
    ? pbState.players[pbState.myPlayerIndex].side : 0;
}

// Forward declarations
static void renderCourt(SSD1306Wire &display);
static void renderLobby(SSD1306Wire &display);
static void renderCountdown(SSD1306Wire &display, int secondsLeft);
static void renderBallTravel(SSD1306Wire &display);
static void renderBallLanded(SSD1306Wire &display);
static void renderPointResult(SSD1306Wire &display);
static void renderGameOver(SSD1306Wire &display);
static void renderScore(SSD1306Wire &display);
static bool pollLobbyState();
static bool createGameOnServer();
static bool joinGameOnServer(const String &gameId, const String &macAddr);
static bool startCountdownOnServer();
static void reportGameComplete();
static void updateGameWager(uint16_t wagerAmount);
static bool setWagerConsent(bool accepted);
static void sendGameStateToPlayers();
static void handleLocalButtonPress();
static void awardPoint(uint8_t scoringSide, uint8_t reason);

// Sound effects defined in main .ino (blocking, menu-only)
extern void playButtonChirp();
extern void playMenuSelectTone();
extern bool anyButtonDown();

// Flag to suppress duplicate sats celebration after wager game
bool suppressNextSatsCelebration = false;

#ifndef RGB_LED
#define RGB_LED 35
#endif

// Button pins
#ifndef BUTTON_PIN_PRG
#define BUTTON_PIN_PRG 0
#endif
#ifndef BUTTON_PIN_EXTERNAL
#define BUTTON_PIN_EXTERNAL 2
#endif

// ============================================================================
// Sound Effects (non-blocking; see sound.h). All respect quiet hours.
// ============================================================================

#define PB_NOTE_COUNT(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static const ToneNote PB_SND_HIT[]   = {{1200, 30}, {1600, 40}};
static const ToneNote PB_SND_POINT[] = {{880, 80}, {1100, 80}, {1320, 150}};
static const ToneNote PB_SND_MISS[]  = {{400, 120}, {300, 150}};
static const ToneNote PB_SND_BEEP[]  = {{800, 80}};
static const ToneNote PB_SND_GO[]    = {{1200, 150}};
// The plain win jingle is the first PB_SND_WIN_NOTES notes of the wager
// victory fanfare; the fanfare adds two high notes at the end.
static const ToneNote PB_SND_VICTORY[] = {
  {523, 100}, {659, 100}, {784, 100}, {1047, 150}, {784, 80}, {1047, 150},
  {1319, 200}, {1568, 400}
};
#define PB_SND_WIN_NOTES 6

static void playPickleballHit()       { soundPlay(PB_SND_HIT, PB_NOTE_COUNT(PB_SND_HIT), false); }
static void playPickleballPoint()     { soundPlay(PB_SND_POINT, PB_NOTE_COUNT(PB_SND_POINT), false); }
static void playPickleballMiss()      { soundPlay(PB_SND_MISS, PB_NOTE_COUNT(PB_SND_MISS), false); }
static void playCountdownBeep()       { soundPlay(PB_SND_BEEP, PB_NOTE_COUNT(PB_SND_BEEP), false); }
static void playCountdownGo()         { soundPlay(PB_SND_GO, PB_NOTE_COUNT(PB_SND_GO), false); }
static void playPickleballWin()       { soundPlay(PB_SND_VICTORY, PB_SND_WIN_NOTES, false); }
static void playWagerVictoryFanfare() { soundPlay(PB_SND_VICTORY, PB_NOTE_COUNT(PB_SND_VICTORY), false); }

// delay() replacement that keeps the sound sequencer and watchdog serviced
static void pbWait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    soundUpdate();
    esp_task_wdt_reset();
    delay(5);
  }
}

// ============================================================================
// Input: both buttons debounced and combined into one logical button
// ============================================================================

struct PbInput {
  DebouncedButton prg;
  DebouncedButton ext;
  bool down;
};

static void pbInputInit(PbInput &in) {
  debounceInit(in.prg, BUTTON_PIN_PRG);
  debounceInit(in.ext, BUTTON_PIN_EXTERNAL);
  in.down = debounceIsDown(in.prg) || debounceIsDown(in.ext);
}

// True once per debounced press of either button (both together = one press)
static bool pbInputPressed(PbInput &in, unsigned long now) {
  debounceUpdate(in.prg, now);
  debounceUpdate(in.ext, now);
  bool d = debounceIsDown(in.prg) || debounceIsDown(in.ext);
  bool edge = d && !in.down;
  in.down = d;
  return edge;
}

static bool pbRawButtonDown() {
  return digitalRead(BUTTON_PIN_PRG) == LOW || digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
}

// ---- Press latch while a blocking HTTP request runs ----
// A 10ms esp_timer samples the buttons so a press made during a lobby poll
// is not lost. (No GPIO interrupts: the .ino configures these pins as
// light-sleep wake sources and attach/detachInterrupt would clobber that.)
static esp_timer_handle_t pbLatchTimer = nullptr;
static volatile bool pbLatchedPress = false;
static volatile bool pbLatchHeld = false;
static volatile uint8_t pbLatchDownSamples = 0;

static void pbLatchTimerCb(void *) {
  if (pbRawButtonDown()) {
    if (pbLatchDownSamples < 2) pbLatchDownSamples = pbLatchDownSamples + 1;
    if (pbLatchDownSamples >= 2 && !pbLatchHeld) {   // stable ~20ms
      pbLatchHeld = true;
      pbLatchedPress = true;
    }
  } else {
    pbLatchDownSamples = 0;
    pbLatchHeld = false;
  }
}

static void pbLatchBegin() {
  pbLatchedPress = false;
  pbLatchDownSamples = 0;
  pbLatchHeld = pbRawButtonDown();   // a press already in progress doesn't count
  if (!pbLatchTimer) {
    esp_timer_create_args_t args = {};
    args.callback = &pbLatchTimerCb;
    args.arg = nullptr;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "pb_btn";
    if (esp_timer_create(&args, &pbLatchTimer) != ESP_OK) {
      pbLatchTimer = nullptr;
      return;
    }
  }
  esp_timer_start_periodic(pbLatchTimer, 10000);
}

static bool pbLatchEnd() {
  if (pbLatchTimer) esp_timer_stop(pbLatchTimer);
  bool pressed = pbLatchedPress;
  pbLatchedPress = false;
  return pressed;
}

// ============================================================================
// Reliable host broadcasts (POINT_SCORED / GAME_OVER): 3 copies, same sequence
// ============================================================================

#define PB_RELIABLE_COPIES 3
#define PB_RELIABLE_GAP_MS 30
static uint8_t pbReliableBuf[32];
static uint8_t pbReliableLen = 0;
static uint8_t pbReliableRemaining = 0;
static unsigned long pbReliableNextAt = 0;

static void pbSendReliable(const void *msg, size_t len) {
  espnowBroadcast((const uint8_t *)msg, len);
  if (len > sizeof(pbReliableBuf)) { pbReliableRemaining = 0; return; }
  memcpy(pbReliableBuf, msg, len);
  pbReliableLen = (uint8_t)len;
  pbReliableRemaining = PB_RELIABLE_COPIES - 1;
  pbReliableNextAt = millis() + PB_RELIABLE_GAP_MS;
}

static void pbServiceReliable(unsigned long now) {
  if (pbReliableRemaining && (long)(now - pbReliableNextAt) >= 0) {
    espnowBroadcast(pbReliableBuf, pbReliableLen);
    pbReliableRemaining--;
    pbReliableNextAt = now + PB_RELIABLE_GAP_MS;
  }
}

// Send any remaining copies now (before a blocking HTTP call)
static void pbFlushReliable() {
  while (pbReliableRemaining) {
    delay(PB_RELIABLE_GAP_MS);
    espnowBroadcast(pbReliableBuf, pbReliableLen);
    pbReliableRemaining--;
  }
}

// ============================================================================
// Match transitions
// ============================================================================

// Enter COUNTDOWN for a freshly confirmed match (host after all ACKs, client
// on accepted GAME_START). Clears all per-match volatile tracking.
static void pbStartMatch(unsigned long now) {
  pbState.phase = PB_PHASE_COUNTDOWN;
  pbState.phaseStartTime = now;
  pbState.volleyId = 0;
  pbState.buttonPressCount = 0;
  pbState.hitConfirmed = false;
  pbState.confirmDeadline = 0;
  pbState.volleyCount = 0;
  flushButtonQueue();
  for (int i = 0; i < MAX_GAME_PLAYERS; i++) pbState.playerLastSeen[i] = now;
  pbState.lastPresencePing = now;
  pbState.lastHostRx = now;
  pbLastBeep = -1;
}

// Enter GAME_OVER once (duplicate GAME_OVER packets are ignored) and play the
// result sound. The host must have settled wagerResult before calling.
static void pbEnterGameOver(uint8_t winnerSide) {
  if (pbState.phase == PB_PHASE_GAME_OVER) return;
  pbState.phase = PB_PHASE_GAME_OVER;
  pbState.phaseStartTime = millis();

  bool iWon = (winnerSide == mySide());
  // Client: estimate wager result for display (host settles actual transfer)
  if (!pbState.isHost && pbState.wagerActive && pbState.wagerAmount > 0) {
    pbState.wagerResult = iWon ? (int16_t)pbState.wagerAmount : -(int16_t)pbState.wagerAmount;
  }

  if (iWon) {
    if (pbState.wagerResult > 0) playWagerVictoryFanfare();
    else playPickleballWin();
  } else {
    playPickleballMiss();
  }
}

// ============================================================================
// ESP-NOW packet handling (loop task; packets queued by espnow_comm)
// ============================================================================

static void pbHandleHostPacket(const uint8_t *mac, const uint8_t *data, unsigned long now) {
  int sender = playerIndexForMac(mac);
  if (sender <= 0) return;  // unknown device, or our own MAC
  uint8_t msgType = data[0];

  if (msgType == MSG_BUTTON_PRESS) {
    ButtonPressMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation != pbState.matchGeneration ||
        msg.sequence <= pbState.playerButtonSequence[sender]) return;
    pbState.playerButtonSequence[sender] = msg.sequence;
    pbState.playerLastSeen[sender] = now;
    // Presses only count for the volley currently waiting on a hit
    if (pbState.phase == PB_PHASE_BALL_LANDED && msg.volleyId == pbState.volleyId) {
      queueButtonPress((uint8_t)sender, msg.volleyId);
    }
  }
  else if (msgType == MSG_LOBBY_READY) {
    LobbyReadyMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation != pbState.matchGeneration) return;
    if (!pbState.players[sender].ready) Serial.printf("Player %d ESP-NOW ready\n", sender);
    pbState.players[sender].ready = true;
    pbState.playerLastSeen[sender] = now;
  }
  else if (msgType == MSG_GAME_START_ACK) {
    GameStartAckMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation != pbState.matchGeneration) return;
    pbState.startAcknowledged[sender] = true;
    pbState.playerLastSeen[sender] = now;
  }
  else if (msgType == MSG_PONG) {
    PingMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation == pbState.matchGeneration) pbState.playerLastSeen[sender] = now;
  }
}

static void pbSendStartAck(const uint8_t *mac) {
  GameStartAckMessage ack;
  ack.type = MSG_GAME_START_ACK;
  ack.generation = pbState.matchGeneration;
  ack.playerIndex = pbState.myPlayerIndex;
  espnowSend(mac, (uint8_t *)&ack, sizeof(ack));
}

static void pbHandleClientPacket(const uint8_t *mac, const uint8_t *data, unsigned long now) {
  if (!isFromHost(mac)) return;
  uint8_t msgType = data[0];

  if (msgType == MSG_GAME_START) {
    GameStartMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.playerCount < 2 || msg.playerCount > MAX_GAME_PLAYERS) return;
    if (msg.yourIndex == 0 || msg.yourIndex >= msg.playerCount) return;
    for (int i = 0; i < msg.playerCount; i++) {
      if (msg.sides[i] > 1 || msg.positions[i] > 1) return;
    }
    if (pbState.matchGeneration != 0 && msg.generation < pbState.matchGeneration) return;
    if (pbState.startReceived && msg.generation == pbState.matchGeneration) {
      pbSendStartAck(mac);  // duplicate (our ACK was lost): re-ACK only
      return;
    }
    if (pbState.phase != PB_PHASE_LOBBY && pbState.phase != PB_PHASE_ESPNOW_SYNC) return;

    pbState.matchGeneration = msg.generation;
    pbState.lastReceivedSequence = msg.sequence;
    pbState.myPlayerIndex = msg.yourIndex;
    pbState.playerCount = msg.playerCount;
    for (int i = 0; i < msg.playerCount; i++) {
      pbState.players[i].petInitial = msg.initials[i];
      pbState.players[i].side = msg.sides[i];
      pbState.players[i].position = msg.positions[i];
    }
    pbState.startReceived = true;
    pbStartMatch(now);
    pbSendStartAck(mac);
    Serial.printf("Game starting! I'm player %d of %d\n", msg.yourIndex, msg.playerCount);
    return;
  }

  if (msgType == MSG_PING) {
    PingMessage ping;
    memcpy(&ping, data, sizeof(ping));
    if (ping.generation != pbState.matchGeneration) return;
    pbState.lastHostRx = now;
    PingMessage pong;
    pong.type = MSG_PONG;
    pong.generation = pbState.matchGeneration;
    pong.timestamp = now;
    espnowSend(mac, (uint8_t *)&pong, sizeof(pong));
    return;
  }

  // Everything below requires an accepted GAME_START for this generation
  if (!pbState.startReceived) return;

  if (msgType == MSG_GAME_STATE) {
    GameStateMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
    // Host only broadcasts state for COUNTDOWN..BALL_LANDED
    if (msg.phase < PB_PHASE_COUNTDOWN || msg.phase > PB_PHASE_BALL_LANDED) return;
    if (msg.targetPlayerIdx >= pbState.playerCount) return;
    if (msg.ballSide > 1 || msg.ballZone > 1 || msg.servingSide > 1) return;
    if (pbState.phase == PB_PHASE_GAME_OVER) return;
    pbState.lastReceivedSequence = msg.sequence;
    pbState.lastHostRx = now;

    PickleballPhase newPhase = (PickleballPhase)msg.phase;
    if (newPhase != pbState.phase) {
      pbState.phaseStartTime = now;
      pbState.ballAnimFrame = 0;
    }
    pbState.phase = newPhase;
    pbState.ballSide = msg.ballSide;
    pbState.ballZone = msg.ballZone;
    pbState.scoreLeft = msg.scoreLeft;
    pbState.scoreRight = msg.scoreRight;
    pbState.servingSide = msg.servingSide;
    pbState.targetPlayerIdx = msg.targetPlayerIdx;
    pbState.volleyId = msg.volleyId;
    pbState.reactionDeadline = now + msg.reactionTimeMs;
  }
  else if (msgType == MSG_POINT_SCORED) {
    PointScoredMessage msg;
    memcpy(&msg, data, sizeof(msg));
    // Host sends 3 copies with one sequence; the sequence check drops repeats
    if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
    if (msg.scoringSide > 1 || msg.reason > PB_MISS_DISCONNECTED) return;
    if (pbState.phase == PB_PHASE_GAME_OVER) return;
    pbState.lastReceivedSequence = msg.sequence;
    pbState.lastHostRx = now;
    pbState.scoreLeft = msg.scoreLeft;
    pbState.scoreRight = msg.scoreRight;
    pbState.lastPointWinner = msg.scoringSide;
    pbState.lastPointReason = msg.reason;
    pbState.phase = PB_PHASE_POINT_RESULT;
    pbState.phaseStartTime = now;

    if (msg.scoringSide == mySide()) playPickleballPoint();
    else playPickleballMiss();
  }
  else if (msgType == MSG_GAME_OVER) {
    GameOverMessage msg;
    memcpy(&msg, data, sizeof(msg));
    if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
    if (msg.winnerSide > 1) return;
    pbState.lastReceivedSequence = msg.sequence;
    pbState.lastHostRx = now;
    pbState.scoreLeft = msg.scoreLeft;
    pbState.scoreRight = msg.scoreRight;
    pbEnterGameOver(msg.winnerSide);
  }
}

// Drain the ESP-NOW receive queue (call from the game loop)
static void pbProcessRx(unsigned long now) {
  ESPNowRxPacket pkt;
  uint8_t budget = ESPNOW_RX_QUEUE_LEN;
  while (budget-- && espnowReceive(pkt)) {
    if (pkt.len < 1) continue;   // lengths were validated per type on receive
    if (pbState.isHost) pbHandleHostPacket(pkt.mac, pkt.data, now);
    else pbHandleClientPacket(pkt.mac, pkt.data, now);
  }
}

// ============================================================================
// Practice (offline, single player)
// ============================================================================

// Solo: a rally streak against the CPU, which returns every ball a little
// faster each time. One missed return ends the game; the score (returns in a
// row) goes to the Pickleball leaderboard like a Flappy score.
static const int PB_SOLO_REACTION_MIN = 320;
int finishGameWithScore(SSD1306Wire &display, uint8_t game, int score);  // pet_blob.cpp  // solo keeps speeding up past the multiplayer floor

int handlePickleballPractice(SSD1306Wire &display) {
  Serial.println(F("Starting Pickleball solo (rally streak)"));

  PbInput input;
  pbInputInit(input);  // also configures both pins as INPUT_PULLUP

  int streak = 0;           // returns in a row: the score
  bool ballAtPlayer = true;

  auto drawPracticeCourt = [&](bool ballVisible, uint8_t requiredPresses,
                               const char *status, int ballX, int ballY) {
    char buf[20];
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Rally %d", streak);
    display.drawString(2, 0, buf);
    display.drawRect(PB_COURT_X, PB_COURT_Y, PB_COURT_W, PB_COURT_H);
    display.drawLine(PB_NET_X, PB_COURT_Y, PB_NET_X, PB_COURT_Y + PB_COURT_H);
    display.drawLine(PB_COURT_X, PB_COURT_MID_Y,
                     PB_COURT_X + PB_COURT_W, PB_COURT_MID_Y);
    if (ballVisible) {
      display.fillCircle(ballX, ballY, 2);
      if (ballAtPlayer && ballX == 29) {
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(29, ballY - 7, requiredPresses == 1 ? "1" : "2");
      }
    }
    if (status && status[0]) {
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 52, status);
    }
    display.display();
  };

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 8, "Rally!");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 31, "Top zone: press once");
  display.drawString(64, 44, "Bottom: press twice");
  display.drawString(64, 56, "Don't miss!");
  display.display();
  pbWait(1800);

  while (pbRawButtonDown()) {
    esp_task_wdt_reset();
    delay(10);
  }
  pbInputInit(input);

  bool missed = false;
  const char *missLabel = "MISSED";
  while (!missed) {
    uint8_t zone = random(0, 2);
    uint8_t requiredPresses = zone == 0 ? 1 : 2;

    // Fly from the hitter to the receiving court. The small parabolic arc
    // makes direction and timing legible without doing more than eight OLED
    // transfers per volley.
    const int startX = ballAtPlayer ? 99 : 29;
    const int endX = ballAtPlayer ? 29 : 99;
    const int startY = PB_COURT_MID_Y;
    const int endY = zone == 0 ? 23 : 48;
    for (int frame = 1; frame <= 8; frame++) {
      int ballX = startX + ((endX - startX) * frame) / 8;
      int ballY = startY + ((endY - startY) * frame) / 8;
      int arc = (8 * frame * (8 - frame)) / 16;
      ballY -= arc;
      drawPracticeCourt(true, requiredPresses, "", ballX, ballY);
      pbInputPressed(input, millis());  // keep debouncer current; early presses don't count
      pbWait(PB_BALL_TRAVEL_MS / 8);
    }

    if (!ballAtPlayer) {
      // The CPU always returns, a little sooner as the rally grows.
      pbWait(max(160, 520 - streak * 25));
      ballAtPlayer = true;
      continue;
    }

    unsigned long reactionWindow = max(PB_SOLO_REACTION_MIN,
      (int)PB_REACTION_TIME_START - streak * PB_REACTION_STEP);
    unsigned long reactStart = millis();
    uint8_t pressCount = 0;
    unsigned long confirmStart = 0;
    bool confirming = false;
    drawPracticeCourt(true, requiredPresses,
                      requiredPresses == 1 ? "PRESS 1" : "PRESS 2",
                      29, zone == 0 ? 23 : 48);

    while (true) {
      unsigned long now = millis();
      bool inWindow = now - reactStart < reactionWindow;
      bool inConfirm = confirming && now - confirmStart < PB_CONFIRM_WINDOW_MS;
      if (!inWindow && !inConfirm) break;
      if (pbInputPressed(input, now)) {
        pressCount++;
        playPickleballHit();
        if (pressCount == requiredPresses) {
          confirming = true;
          confirmStart = now;
        }
      }
      if (pressCount > requiredPresses) break;
      soundUpdate();
      esp_task_wdt_reset();
      delay(5);
    }

    if (pressCount == requiredPresses) {
      streak++;
      ballAtPlayer = false;
    } else {
      missed = true;
      missLabel = pressCount > requiredPresses ? "TOO MANY" : "MISSED";
    }
  }

  Serial.printf("Pickleball rally over: %d returns\n", streak);
  drawPracticeCourt(false, 1, missLabel, 0, 0);
  playPickleballMiss();
  pbWait(900);
  soundStop();
  while (pbRawButtonDown()) {  // don't carry a held press into the result screens
    esp_task_wdt_reset();
    delay(10);
  }
  return finishGameWithScore(display, GAME_PICKLEBALL, streak);
}

// ============================================================================
// Multiplayer
// ============================================================================

// Restores the radio on every exit path from handlePickleballGame():
// ESP-NOW off; WiFi off again if the game turned it on, otherwise modem
// sleep re-enabled (it is disabled during the game for ESP-NOW latency).
// Defined in satoshi_pet_heltec.ino
void wifiRadioOff();
bool wifiReady();

struct PbRadioSession {
  bool wifiStartedByGame = false;
  bool espnowActive = false;
  ~PbRadioSession() {
    soundStop();
    if (espnowActive) espnowDeinit();
    if (wifiStartedByGame) {
      wifiRadioOff();  // also resets the WiFi state machine + shared TLS client
    } else if (WiFi.getMode() != WIFI_OFF) {
      WiFi.setSleep(true);
    }
    digitalWrite(RGB_LED, LOW);
  }
};

static void pbShowMessage(SSD1306Wire &display, const char *line1, const char *line2) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  if (line1) display.drawString(64, line2 ? 18 : 25, line1);
  if (line2) display.drawString(64, 34, line2);
  display.display();
}

// Cheap fingerprint of everything static screens show; a change => redraw.
static uint32_t pbRenderSignature(unsigned long now) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
  mix(pbState.phase);
  mix(pbState.playerCount);
  mix(pbState.myPlayerIndex);
  mix(pbState.isHost);
  mix(pbState.scoreLeft);
  mix(pbState.scoreRight);
  mix(pbState.wagerAmount);
  mix(pbState.wagerActive);
  mix((uint16_t)pbState.wagerResult);
  mix(pbState.lastPointWinner);
  mix(pbState.lastPointReason);
  for (const char *c = pbState.roomCode.c_str(); c && *c; c++) mix((uint8_t)*c);
  for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++) {
    mix((uint8_t)pbState.players[i].petInitial);
    mix(pbState.players[i].side);
    mix(pbState.players[i].position);
  }
  if (pbState.phase == PB_PHASE_LOBBY) {
    mix((now - pbState.phaseStartTime) / 1000);  // "Waiting... Ns" countdown
  } else if (pbState.phase == PB_PHASE_COUNTDOWN) {
    mix((now - pbState.phaseStartTime) / 1000);
  }
  return h;
}

static void pbLogChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) == ESP_OK) {
    Serial.printf("ESP-NOW on WiFi channel %u (all players must share it)\n", (unsigned)primary);
  }
}

int handlePickleballGame(SSD1306Wire &display) {
  // Initialize game state (assignment, not memset: the struct holds Strings)
  pbState = PickleballState{};
  pbState.phase = PB_PHASE_LOBBY;
  pbState.isHost = true;  // Assume host until we discover we're joining
  pbState.myPlayerIndex = 0;
  pbState.phaseStartTime = millis();
  flushButtonQueue();
  pbReliableRemaining = 0;
  pbLastBeep = -1;

  PbRadioSession radio;  // restores WiFi/ESP-NOW/LED on every return below

  // Join-by-invite path: NOT WIRED. pendingPickleballGameId is never assigned
  // anywhere in the firmware, so this branch (and joinGameOnServer) is
  // currently unreachable. Kept in place pending a product decision.
  extern String pendingPickleballGameId;
  if (pendingPickleballGameId.length() > 0) {
    pbState.isHost = false;
    pbState.gameId = pendingPickleballGameId;
    Serial.println("Joining existing game: " + pbState.gameId);
  }

  Serial.println(F("Starting Pickleball game..."));

  // Ensure WiFi is available for lobby phase (through the main WiFi state
  // machine, so it doesn't race a connect already in progress)
  if (!wifiReady()) {
    pbShowMessage(display, "Connecting WiFi...", nullptr);
    radio.wifiStartedByGame = true;
    // Long enough for the fast reconnect (4 s) plus a full scan (10 s): after
    // waking, the background connect may have just fallen back to a scan.
    if (!wifiEnsureConnected(15000)) {
      pbShowMessage(display, "WiFi failed", "Try again later");
      pbWait(2000);
      return 0;
    }
    Serial.println(F("WiFi connected for Pickleball"));
  }

  WiFi.macAddress(pbMyMac);

  // Initialize ESP-NOW (works alongside WiFi STA)
  if (!espnowInit()) {
    pbShowMessage(display, "ESP-NOW init failed", nullptr);
    pbWait(2000);
    return 0;
  }
  radio.espnowActive = true;
  WiFi.setSleep(false);  // modem sleep adds 100ms+ latency to ESP-NOW
  pbLogChannel();

  String myMac = espnowGetMacString();

  // --- JOIN MODE: Join existing game from invite (not wired, see above) ---
  if (!pbState.isHost) {
    pbShowMessage(display, "Joining game...", nullptr);

    if (!joinGameOnServer(pbState.gameId, myMac)) {
      pbShowMessage(display, "Failed to join", "game");
      pbWait(2000);
      return 0;
    }

    Serial.println("Joined game: " + pbState.gameId);
    pbState.phase = PB_PHASE_LOBBY;
    pbState.phaseStartTime = millis();
  }

  // --- No invite: try to find an existing game first, then create ---
  if (pbState.isHost) {
    pbShowMessage(display, "Finding game...", nullptr);

    // Try to find and join an existing lobby (wagerAmount=0 for discovery)
    pbState.wagerAmount = 0;
    if (!createGameOnServer()) {
      pbShowMessage(display, "Failed to connect", nullptr);
      pbWait(2000);
      return 0;
    }

    if (!pbState.isHost) {
      // Found and joined an existing game
      Serial.println("Joined existing game: " + pbState.gameId);
      char buf[16];
      snprintf(buf, sizeof(buf), "%u players", (unsigned)pbState.playerCount);
      pbShowMessage(display, "Found game!", buf);
      pbWait(800);
    } else {
      // No game found — we're hosting. Now ask about wager.
      Serial.println("No existing game found, hosting new one: " + pbState.gameId);

      const uint16_t wagerOptions[] = {0, 100, 500, 1000};
      int wagerIdx = 0;
      bool wagerSelected = false;
      unsigned long wagerTimeout = millis() + 15000;

      while (!wagerSelected && millis() < wagerTimeout) {
        esp_task_wdt_reset();
        display.clear();
        display.setFont(ArialMT_Plain_16);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 0, "WAGER?");

        display.setFont(ArialMT_Plain_24);
        if (wagerOptions[wagerIdx] == 0) {
          display.drawString(64, 20, "FREE");
        } else {
          char buf[8];
          snprintf(buf, sizeof(buf), "%u", (unsigned)wagerOptions[wagerIdx]);
          display.drawString(64, 20, buf);
        }

        display.setFont(ArialMT_Plain_10);
        if (holdHintsVisible()) display.drawString(64, 50, "Tap: next  Hold: pick");

        // Tap = next amount, hold = pick (as soon as the line fills)
        PressEvent ev = holdPressUpdate(anyButtonDown());
        if (holdPressActive()) wagerTimeout = millis() + 15000;
        holdPressDrawBar(display, false);
        display.display();

        if (ev == PRESS_HOLD) {
          playMenuSelectTone();
          wagerSelected = true;
        } else if (ev == PRESS_TAP) {
          playButtonChirp();
          wagerIdx = (wagerIdx + 1) % 4;
        }
        soundUpdate();
        delay(30);
      }

      uint16_t selectedWager = wagerOptions[wagerIdx];
      Serial.printf("Wager selected: %d sats\n", selectedWager);

      // Transition game from "setup" to "lobby" (also sets wager)
      pbState.wagerAmount = selectedWager;
      updateGameWager(selectedWager);
    }
  }

  // --- Wager acceptance screen for joiners ---
  if (!pbState.isHost && pbState.wagerAmount > 0) {
    bool wagerDecision = false;
    bool wagerAccepted = false;
    unsigned long acceptTimeout = millis() + 15000;
    char amountBuf[8];
    snprintf(amountBuf, sizeof(amountBuf), "%u", (unsigned)pbState.wagerAmount);

    while (!wagerDecision && millis() < acceptTimeout) {
      esp_task_wdt_reset();
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 0, "WAGER");

      display.setFont(ArialMT_Plain_24);
      display.drawString(64, 18, amountBuf);

      display.setFont(ArialMT_Plain_10);
      // A money decision, so the hint always shows
      display.drawString(64, 46, "Hold: Accept  Tap: Decline");

      // Hold = accept (as soon as the line fills), tap = decline
      PressEvent ev = holdPressUpdate(anyButtonDown());
      if (holdPressActive()) acceptTimeout = millis() + 15000;
      holdPressDrawBar(display, false);
      display.display();
      if (ev != PRESS_NONE) {
        wagerDecision = true;
        wagerAccepted = (ev == PRESS_HOLD);
      }
      soundUpdate();
      delay(30);
    }

    if (!wagerAccepted) {
      Serial.println(F("Wager declined, exiting game"));
      setWagerConsent(false); // Removes this device from the lobby atomically.
      pbShowMessage(display, "Wager declined", nullptr);
      pbWait(1500);
      return 0;
    }

    playMenuSelectTone();
    if (!setWagerConsent(true)) {
      pbShowMessage(display, "Could not accept wager", nullptr);
      pbWait(1500);
      return 0;
    }
    Serial.printf("Wager of %d sats accepted\n", pbState.wagerAmount);
  }

  // ====================================================================
  // Main game loop (blocks until game ends or cancelled)
  // ====================================================================

  bool gameRunning = true;
  const char *exitLine1 = nullptr;  // shown briefly when the game aborts
  const char *exitLine2 = nullptr;
  unsigned long lastRender = 0;
  const unsigned long RENDER_INTERVAL = 50; // 20 FPS while the ball moves
  uint32_t lastSignature = 0;
  bool forceRedraw = true;

  PbInput input;
  pbInputInit(input);

  while (gameRunning) {
    unsigned long now = millis();
    esp_task_wdt_reset(); // Feed watchdog
    soundUpdate();

    // --- Apply ESP-NOW packets queued by the WiFi-task callback ---
    pbProcessRx(now);

    // --- Check for local button press (debounced) ---
    if (pbInputPressed(input, now)) {
      handleLocalButtonPress();
    }

    // --- Phase-specific logic ---
    switch (pbState.phase) {
      case PB_PHASE_LOBBY: {
        // Poll server for player list. Presses made during the blocking
        // request are latched and applied afterwards.
        if (pbState.lastLobbyPoll == 0 || now - pbState.lastLobbyPoll >= PB_LOBBY_POLL_MS) {
          pbLatchBegin();
          pollLobbyState();
          bool latched = pbLatchEnd();
          pbState.lastLobbyPoll = millis();
          if (pbState.lobbyCancelled) {
            exitLine1 = "Game cancelled";
            gameRunning = false;
            break;
          }
          if (latched) {
            pbInputInit(input);  // adopt current level so a held press isn't counted twice
            handleLocalButtonPress();
          }
        }

        // Lobby timeout (matches the "Waiting... Ns" countdown on screen)
        if (pbState.phase == PB_PHASE_LOBBY && pbState.playerCount < 2 &&
            millis() - pbState.phaseStartTime > PB_LOBBY_TIMEOUT_MS) {
          Serial.println(F("Lobby timed out"));
          exitLine1 = "No players joined";
          gameRunning = false;
        }
        break;
      }

      case PB_PHASE_ESPNOW_SYNC: {
        if (pbState.isHost) {
          // Host: wait for all players to send LOBBY_READY via ESP-NOW
          bool allReady = true;
          bool allStartAcknowledged = true;
          for (int i = 1; i < pbState.playerCount; i++) { // Skip host (idx 0)
            if (!pbState.players[i].ready) allReady = false;
            if (!pbState.startAcknowledged[i]) allStartAcknowledged = false;
          }

          // Send ping to all peers to prompt ready response
          if (now - pbState.lastStateSend > 500) {
            pbState.lastStateSend = now;
            PingMessage ping;
            ping.type = MSG_PING;
            ping.generation = pbState.matchGeneration;
            ping.timestamp = now;
            espnowBroadcast((uint8_t*)&ping, sizeof(ping));
          }

          // Never begin with missing players. A dropped start packet is retried
          // until every client acknowledges the current match generation.
          if (pbState.playerCount >= 2 && allReady && allStartAcknowledged) {
            Serial.println(F("ESP-NOW sync complete, starting countdown"));
            pbStartMatch(now);
          } else if (allReady && now - pbState.lastStateSend > 250) {
            pbState.lastStateSend = now;
            // Send GAME_START to each player with their index
            GameStartMessage startMsg;
            memset(&startMsg, 0, sizeof(startMsg));
            startMsg.type = MSG_GAME_START;
            startMsg.generation = pbState.matchGeneration;
            startMsg.sequence = ++pbState.sendSequence;
            startMsg.playerCount = pbState.playerCount;
            for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++) {
              startMsg.initials[i] = pbState.players[i].petInitial;
              startMsg.sides[i] = pbState.players[i].side;
              startMsg.positions[i] = pbState.players[i].position;
            }

            // Send to each non-host player with their specific index
            for (int i = 1; i < pbState.playerCount; i++) {
              if (!pbState.startAcknowledged[i]) {
                startMsg.yourIndex = i;
                espnowSend(pbState.players[i].mac, (uint8_t*)&startMsg, sizeof(startMsg));
              }
            }
          }
          if (pbState.phase == PB_PHASE_ESPNOW_SYNC && now - pbState.phaseStartTime > 15000) {
            Serial.println(F("ESP-NOW sync failed: missing ready/start acknowledgement"));
            pbLogChannel();
            exitLine1 = "Can't reach other player";
            exitLine2 = "(different WiFi channel?)";
            gameRunning = false;
          }
        } else {
          // Client: send LOBBY_READY to the host and wait for GAME_START
          if (now - pbState.lastStateSend > 500 && pbState.playerCount > 0) {
            pbState.lastStateSend = now;
            LobbyReadyMessage ready;
            ready.type = MSG_LOBBY_READY;
            ready.generation = pbState.matchGeneration;
            ready.playerIndex = pbState.myPlayerIndex;
            ready.petInitial = ganamosConfig.petName.charAt(0);
            espnowSend(pbState.players[0].mac, (uint8_t*)&ready, sizeof(ready));
          }

          // Timeout waiting for game start
          if (now - pbState.phaseStartTime > 15000) {
            Serial.println(F("ESP-NOW sync timeout (no GAME_START from host)"));
            pbLogChannel();
            exitLine1 = "Can't reach other player";
            exitLine2 = "(different WiFi channel?)";
            gameRunning = false;
          }
        }
        break;
      }

      case PB_PHASE_COUNTDOWN: {
        int elapsed = now - pbState.phaseStartTime;
        int secondsLeft = PB_COUNTDOWN_SECS - (elapsed / 1000);

        if (secondsLeft <= 0) {
          // Start the game!
          pbState.phase = PB_PHASE_SERVING;
          pbState.phaseStartTime = now;
          pbState.servingSide = 0; // Left serves first
          playCountdownGo();
        } else if (secondsLeft != pbLastBeep) {
          pbLastBeep = secondsLeft;
          playCountdownBeep();
        }
        break;
      }

      case PB_PHASE_SERVING: {
        if (pbState.isHost && (now - pbState.phaseStartTime > PB_SERVE_DELAY_MS)) {
          // Choose random target on the receiving side
          uint8_t receiveSide = 1 - pbState.servingSide;
          pbState.ballSide = receiveSide;
          pbState.ballZone = random(0, 2); // 0=top, 1=bottom

          // Find the target player
          pbState.targetPlayerIdx = 255; // Invalid
          for (int i = 0; i < pbState.playerCount; i++) {
            if (pbState.players[i].side == receiveSide &&
                pbState.players[i].position == pbState.ballZone) {
              pbState.targetPlayerIdx = i;
              break;
            }
          }

          // If only 1 player on receiving side (1v2 or 1v1), they're the target regardless of zone
          if (pbState.targetPlayerIdx == 255) {
            for (int i = 0; i < pbState.playerCount; i++) {
              if (pbState.players[i].side == receiveSide) {
                pbState.targetPlayerIdx = i;
                break;
              }
            }
          }

          // Nobody on the receiving side (bad lobby assignment): can't play
          if (pbState.targetPlayerIdx == 255) {
            Serial.println(F("No player on receiving side; aborting game"));
            exitLine1 = "Team setup error";
            gameRunning = false;
            break;
          }

          // New volley: presses tagged with an older volleyId are dropped
          pbState.volleyId++;
          pbState.buttonPressCount = 0;
          pbState.hitConfirmed = false;
          pbState.confirmDeadline = 0;

          // Determine required presses: 1 for top zone, 2 for bottom zone
          bool isSoloOnSide = true;
          for (int i = 0; i < pbState.playerCount; i++) {
            if (i != pbState.targetPlayerIdx &&
                pbState.players[i].side == receiveSide) {
              isSoloOnSide = false;
              break;
            }
          }
          if (isSoloOnSide) {
            pbState.requiredPresses = (pbState.ballZone == 0) ? 1 : 2;
          } else {
            pbState.requiredPresses = 1;
          }

          pbState.phase = PB_PHASE_BALL_TRAVEL;
          pbState.phaseStartTime = now;
          pbState.ballAnimFrame = 0;

          sendGameStateToPlayers();
        }
        break;
      }

      case PB_PHASE_BALL_TRAVEL: {
        if (pbState.isHost && (now - pbState.phaseStartTime > PB_BALL_TRAVEL_MS)) {
          pbState.phase = PB_PHASE_BALL_LANDED;
          pbState.phaseStartTime = now;
          flushButtonQueue();  // only presses made while the ball is down count

          // Progressive difficulty: reaction window shrinks each volley
          int reactionMs = PB_REACTION_TIME_START - (pbState.volleyCount * PB_REACTION_STEP);
          if (reactionMs < PB_REACTION_TIME_MIN) reactionMs = PB_REACTION_TIME_MIN;
          pbState.reactionDeadline = now + reactionMs;

          sendGameStateToPlayers();
          playPickleballHit();
        }

        // Update animation frame
        pbState.ballAnimFrame = (now - pbState.phaseStartTime) * 4 / PB_BALL_TRAVEL_MS;
        break;
      }

      case PB_PHASE_BALL_LANDED: {
        if (pbState.isHost) {
          if (pbState.targetPlayerIdx >= pbState.playerCount) {
            // Should not happen (SERVING validates); re-serve safely
            pbState.phase = PB_PHASE_SERVING;
            pbState.phaseStartTime = now;
            break;
          }
          uint8_t targetSide = pbState.players[pbState.targetPlayerIdx].side;

          // --- Handle incoming button presses for this volley ---
          PendingButtonPress pendingPress;
          while (pbState.phase == PB_PHASE_BALL_LANDED && dequeueButtonPress(pendingPress)) {
            if (pendingPress.volleyId != pbState.volleyId) continue;   // stale
            uint8_t pressedIdx = pendingPress.playerIndex;
            if (pressedIdx >= pbState.playerCount) continue;
            uint8_t presserSide = pbState.players[pressedIdx].side;

            if (presserSide != targetSide) {
              // Wrong side pressed — fault
              awardPoint(1 - presserSide, PB_MISS_WRONG_PLAYER);
            } else if (pbState.playerCount > 2 && pressedIdx != pbState.targetPlayerIdx) {
              // Doubles: wrong player on correct side
              awardPoint(1 - targetSide, PB_MISS_WRONG_PLAYER);
            } else if (pbState.hitConfirmed) {
              // Already hit the required count — extra press is a fault
              pbState.hitConfirmed = false;
              awardPoint(1 - targetSide, PB_MISS_DOUBLE_FAULT);
            } else {
              pbState.buttonPressCount++;

              if (pbState.buttonPressCount == pbState.requiredPresses) {
                // Correct count reached — start confirm window
                pbState.hitConfirmed = true;
                pbState.confirmDeadline = now + PB_CONFIRM_WINDOW_MS;
                playPickleballHit();
              } else if (pbState.buttonPressCount > pbState.requiredPresses) {
                // Over-pressed before confirm even started (safety net)
                awardPoint(1 - targetSide, PB_MISS_DOUBLE_FAULT);
              }
            }
          }
          if (pbState.phase != PB_PHASE_BALL_LANDED) break;

          // --- Check confirm window expiry (successful hit!) ---
          if (pbState.hitConfirmed && (long)(now - pbState.confirmDeadline) >= 0) {
            pbState.hitConfirmed = false;
            pbState.volleyCount++;
            pbState.servingSide = targetSide;
            pbState.phase = PB_PHASE_SERVING;
            pbState.phaseStartTime = now;
            sendGameStateToPlayers();
            break;
          }

          // --- Check reaction timeout (missed the ball) ---
          if (!pbState.hitConfirmed && (long)(now - pbState.reactionDeadline) > 0) {
            awardPoint(1 - targetSide, PB_MISS_TIMEOUT);
          }
        }
        break;
      }

      case PB_PHASE_POINT_RESULT: {
        if (now - pbState.phaseStartTime > PB_POINT_DISPLAY_MS) {
          // Check for game over
          if (pbState.scoreLeft >= PB_WINNING_SCORE || pbState.scoreRight >= PB_WINNING_SCORE) {
            uint8_t winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
            if (pbState.isHost) {
              GameOverMessage gameOver;
              gameOver.type = MSG_GAME_OVER;
              gameOver.generation = pbState.matchGeneration;
              gameOver.sequence = ++pbState.sendSequence;
              gameOver.winnerSide = winnerSide;
              gameOver.scoreLeft = pbState.scoreLeft;
              gameOver.scoreRight = pbState.scoreRight;
              pbSendReliable(&gameOver, sizeof(gameOver));
              pbFlushReliable();  // all 3 copies out before the blocking HTTP call

              // Report to server (also parses wager settlement)
              reportGameComplete();
            }
            // Clients also get here on their own if every GAME_OVER copy is lost
            pbEnterGameOver(winnerSide);
          } else {
            // Next serve
            pbState.phase = PB_PHASE_SERVING;
            pbState.phaseStartTime = now;
          }
        }
        break;
      }

      case PB_PHASE_GAME_OVER: {
        unsigned long gameOverDuration = (pbState.wagerResult != 0) ? 8000 : 5000;
        if (now - pbState.phaseStartTime > gameOverDuration) {
          gameRunning = false;
        }

        // Blink LED for winner during game-over display
        uint8_t winSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
        if (winSide == mySide()) {
          digitalWrite(RGB_LED, ((now - pbState.phaseStartTime) % 500 < 250) ? HIGH : LOW);
        }
        break;
      }
    }

    // --- Host: periodically broadcast game state during active play ---
    if (pbState.isHost && pbState.phase >= PB_PHASE_COUNTDOWN && pbState.phase <= PB_PHASE_BALL_LANDED) {
      if (now - pbState.lastStateSend > PB_ESPNOW_STATE_MS) {
        sendGameStateToPlayers();
        pbState.lastStateSend = now;
      }
    }

    // --- Host: presence pings + disconnect detection ---
    if (gameRunning && pbState.isHost && pbState.phase >= PB_PHASE_COUNTDOWN &&
        pbState.phase < PB_PHASE_GAME_OVER && now - pbState.lastPresencePing > 2000) {
      pbState.lastPresencePing = now;
      PingMessage ping;
      ping.type = MSG_PING;
      ping.generation = pbState.matchGeneration;
      ping.timestamp = now;
      espnowBroadcast((uint8_t*)&ping, sizeof(ping));
      for (int i = 1; i < pbState.playerCount; i++) {
        if (pbState.playerLastSeen[i] != 0 && now - pbState.playerLastSeen[i] > 8000) {
          Serial.printf("Player %d disconnected\n", i);
          uint8_t disconnectedSide = pbState.players[i].side;
          // Other side wins: set it one short, then award the final point
          if (disconnectedSide == 0) pbState.scoreRight = PB_WINNING_SCORE - 1;
          else pbState.scoreLeft = PB_WINNING_SCORE - 1;
          awardPoint(1 - disconnectedSide, PB_MISS_DISCONNECTED);
          break;
        }
      }
    }

    // --- Client: host-loss detection ---
    if (gameRunning && !pbState.isHost && pbState.startReceived &&
        pbState.phase >= PB_PHASE_COUNTDOWN && pbState.phase < PB_PHASE_GAME_OVER &&
        now - pbState.lastHostRx > PB_HOST_LOST_MS) {
      Serial.println(F("Host lost: no packets for 5s"));
      exitLine1 = "Host lost";
      exitLine2 = "Game ended";
      gameRunning = false;
    }

    pbServiceReliable(now);

    // --- Render: 20fps only while the ball moves; static screens on change ---
    if (gameRunning) {
      now = millis();
      bool animated = (pbState.phase == PB_PHASE_BALL_TRAVEL ||
                       pbState.phase == PB_PHASE_BALL_LANDED);
      bool draw = false;
      uint32_t sig = pbRenderSignature(now);
      if (forceRedraw || sig != lastSignature) {
        draw = true;
      } else if (animated && now - lastRender >= RENDER_INTERVAL) {
        draw = true;
      }

      if (draw) {
        lastRender = now;
        lastSignature = sig;
        forceRedraw = false;

        display.clear();

        switch (pbState.phase) {
          case PB_PHASE_LOBBY:
            renderLobby(display);
            break;
          case PB_PHASE_ESPNOW_SYNC: {
            char buf[16];
            display.setFont(ArialMT_Plain_10);
            display.setTextAlignment(TEXT_ALIGN_CENTER);
            display.drawString(64, 20, "Connecting...");
            snprintf(buf, sizeof(buf), "%u players", (unsigned)pbState.playerCount);
            display.drawString(64, 35, buf);
            break;
          }
          case PB_PHASE_COUNTDOWN:
            renderCountdown(display, PB_COUNTDOWN_SECS - (int)((now - pbState.phaseStartTime) / 1000));
            break;
          case PB_PHASE_SERVING:
          case PB_PHASE_BALL_TRAVEL:
          case PB_PHASE_BALL_LANDED:
            renderCourt(display);
            renderScore(display);
            if (pbState.phase == PB_PHASE_BALL_TRAVEL) renderBallTravel(display);
            if (pbState.phase == PB_PHASE_BALL_LANDED) renderBallLanded(display);
            break;
          case PB_PHASE_POINT_RESULT:
            renderCourt(display);
            renderScore(display);
            renderPointResult(display);
            break;
          case PB_PHASE_GAME_OVER:
            renderGameOver(display);
            break;
        }

        display.display();
      }
    }

    delay(5); // Small yield
  }

  if (exitLine1) {
    pbShowMessage(display, exitLine1, exitLine2);
    pbWait(1500);
  }

  // Suppress duplicate sats celebration if wager was involved
  if (pbState.wagerResult != 0) {
    suppressNextSatsCelebration = true;
  }

  // Return happiness bonus. Wager pet coins arrive from the authoritative server
  // ledger on the next config sync; crediting locally here would double-mint them.
  // (Radio/ESP-NOW/LED cleanup happens in PbRadioSession's destructor.)
  if (pbState.phase == PB_PHASE_GAME_OVER) {
    uint8_t winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
    return (winnerSide == mySide()) ? 30 : 15;
  }

  return 0;
}

// ============================================================================
// Helper: Award a point (host only)
// ============================================================================

static void awardPoint(uint8_t scoringSide, uint8_t reason) {
  if (scoringSide == 0) {
    pbState.scoreLeft++;
  } else {
    pbState.scoreRight++;
  }

  pbState.lastPointWinner = scoringSide;
  pbState.lastPointReason = reason;
  pbState.phase = PB_PHASE_POINT_RESULT;
  pbState.phaseStartTime = millis();
  pbState.hitConfirmed = false;
  flushButtonQueue();

  // Reset rally volley count (difficulty resets each point)
  pbState.volleyCount = 0;

  // Alternate serve
  pbState.servingSide = 1 - scoringSide;

  // Send point scored to all players (3 copies, same sequence)
  PointScoredMessage msg;
  msg.type = MSG_POINT_SCORED;
  msg.generation = pbState.matchGeneration;
  msg.sequence = ++pbState.sendSequence;
  msg.scoringSide = scoringSide;
  msg.scoreLeft = pbState.scoreLeft;
  msg.scoreRight = pbState.scoreRight;
  msg.reason = reason;
  pbSendReliable(&msg, sizeof(msg));

  // Sound on host
  if (scoringSide == mySide()) {
    playPickleballPoint();
  } else {
    playPickleballMiss();
  }

  Serial.printf("Point! L:%d R:%d (reason:%d)\n", pbState.scoreLeft, pbState.scoreRight, reason);
}

// ============================================================================
// Button handling
// ============================================================================

static void handleLocalButtonPress() {
  if (pbState.phase == PB_PHASE_LOBBY) {
    // In lobby: host can start game if 2+ players
    if (pbState.isHost && pbState.playerCount >= 2) {
      Serial.println(F("Host starting game..."));
      if (startCountdownOnServer()) {
        pbState.phase = PB_PHASE_ESPNOW_SYNC;
        pbState.phaseStartTime = millis();
        pbState.lastStateSend = 0;
      } else {
        Serial.println(F("Host start rejected; remaining in lobby"));
      }
    }
    return;
  }

  if (pbState.phase == PB_PHASE_BALL_LANDED) {
    if (pbState.isHost) {
      // Host handles own button press directly
      queueButtonPress(pbState.myPlayerIndex, pbState.volleyId);
    } else if (pbState.startReceived && pbState.playerCount > 0) {
      // Client: send button press to host, tagged with the current volley
      ButtonPressMessage msg;
      msg.type = MSG_BUTTON_PRESS;
      msg.generation = pbState.matchGeneration;
      msg.sequence = ++pbState.playerButtonSequence[pbState.myPlayerIndex];
      msg.playerIndex = pbState.myPlayerIndex;
      msg.timestamp = millis();
      msg.volleyId = pbState.volleyId;
      espnowSend(pbState.players[0].mac, (uint8_t*)&msg, sizeof(msg));
    }
    return;
  }

  if (pbState.phase == PB_PHASE_GAME_OVER) {
    // Any press during game over → exit early
    pbState.phaseStartTime -= 10000; // Force timeout
    return;
  }
}

// ============================================================================
// Send game state to all players (host only)
// ============================================================================

static void sendGameStateToPlayers() {
  if (!pbState.isHost) return;

  unsigned long now = millis();
  GameStateMessage msg;
  msg.type = MSG_GAME_STATE;
  msg.generation = pbState.matchGeneration;
  msg.sequence = ++pbState.sendSequence;
  msg.phase = (uint8_t)pbState.phase;
  msg.ballSide = pbState.ballSide;
  msg.ballZone = pbState.ballZone;
  msg.scoreLeft = pbState.scoreLeft;
  msg.scoreRight = pbState.scoreRight;
  msg.servingSide = pbState.servingSide;
  msg.targetPlayerIdx = pbState.targetPlayerIdx;
  long remaining = (long)(pbState.reactionDeadline - now);
  if (remaining < 0) remaining = 0;
  if (remaining > 65535) remaining = 65535;
  msg.reactionTimeMs = (uint16_t)remaining;
  msg.timestamp = now;
  msg.volleyId = pbState.volleyId;

  espnowBroadcast((uint8_t*)&msg, sizeof(msg));
}

// ============================================================================
// Rendering
// ============================================================================

static void renderScore(SSD1306Wire &display) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%u - %u", (unsigned)pbState.scoreLeft, (unsigned)pbState.scoreRight);
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, buf);
}

static void renderCourt(SSD1306Wire &display) {
  // Outer court border
  display.drawRect(PB_COURT_X, PB_COURT_Y, PB_COURT_W, PB_COURT_H);

  // Net (center vertical line, dashed)
  for (int y = PB_COURT_Y; y < PB_COURT_Y + PB_COURT_H; y += 4) {
    display.drawLine(PB_NET_X, y, PB_NET_X, min(y + 2, PB_COURT_Y + PB_COURT_H - 1));
  }

  // Horizontal splits on each side (top/bottom zones)
  display.drawLine(PB_COURT_X, PB_COURT_MID_Y, PB_NET_X - 1, PB_COURT_MID_Y);
  display.drawLine(PB_NET_X + 1, PB_COURT_MID_Y, PB_COURT_X + PB_COURT_W - 1, PB_COURT_MID_Y);

  // Player initials in their zones
  display.setFont(ArialMT_Plain_10);

  for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++) {
    ESPNowPlayer &p = pbState.players[i];
    int x = (p.side == 0)
      ? PB_COURT_X + (PB_NET_X - PB_COURT_X) / 2
      : PB_NET_X + (PB_COURT_X + PB_COURT_W - PB_NET_X) / 2;
    int y = (p.position == 0) ? PB_COURT_Y + 4 : PB_COURT_MID_Y + 4;

    char initial[2] = {p.petInitial ? p.petInitial : '?', '\0'};
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(x, y, initial);
  }

  // For solo player on a side: show "1" and "2" indicators
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  for (uint8_t side = 0; side <= 1; side++) {
    int playersOnSide = 0;
    int soloIdx = -1;
    for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++) {
      if (pbState.players[i].side == side) {
        playersOnSide++;
        soloIdx = i;
      }
    }

    if (playersOnSide == 1) {
      int baseX = (side == 0) ? PB_COURT_X + 5 : PB_COURT_X + PB_COURT_W - 12;
      uint8_t playerPos = pbState.players[soloIdx].position;

      // The zone matching the player's position = 1 press, the other = 2
      if (playerPos == 0) {
        display.drawString(baseX, PB_COURT_Y + 2, "1");
        display.drawString(baseX, PB_COURT_MID_Y + 2, "2");
      } else {
        display.drawString(baseX, PB_COURT_Y + 2, "2");
        display.drawString(baseX, PB_COURT_MID_Y + 2, "1");
      }
    }
  }
}

static void renderBallTravel(SSD1306Wire &display) {
  // Animate ball from serving side's edge to the target zone
  float progress = (float)(millis() - pbState.phaseStartTime) / PB_BALL_TRAVEL_MS;
  if (progress > 1.0f) progress = 1.0f;

  int startX, endX;
  if (pbState.servingSide == 0) {
    startX = PB_COURT_X + 10;  // Left side
    endX = PB_NET_X + (PB_COURT_X + PB_COURT_W - PB_NET_X) / 2; // Right zone center
  } else {
    startX = PB_COURT_X + PB_COURT_W - 10; // Right side
    endX = PB_COURT_X + (PB_NET_X - PB_COURT_X) / 2; // Left zone center
  }

  int targetY;
  if (pbState.ballZone == 0) {
    targetY = PB_COURT_Y + (PB_COURT_MID_Y - PB_COURT_Y) / 2;
  } else {
    targetY = PB_COURT_MID_Y + (PB_COURT_Y + PB_COURT_H - PB_COURT_MID_Y) / 2;
  }

  int ballX = startX + (int)((endX - startX) * progress);
  int startY = PB_COURT_Y + PB_COURT_H / 2; // Start from center height
  // Keep top/bottom ambiguous for 60% of flight, then curve toward the zone.
  float reveal = progress <= 0.6f ? 0.0f : (progress - 0.6f) / 0.4f;
  int ballY = startY + (int)((targetY - startY) * reveal);
  ballY -= (int)(5.0f * 4.0f * progress * (1.0f - progress));

  display.fillCircle(ballX, ballY, 3);
}

static void renderBallLanded(SSD1306Wire &display) {
  // Ball is in the target zone - flash it to indicate "HIT NOW!"
  unsigned long now = millis();
  unsigned long elapsed = now - pbState.phaseStartTime;
  bool visible = (elapsed / 150) % 2 == 0; // Blink every 150ms

  if (visible) {
    int zoneX = (pbState.ballSide == 0)
      ? PB_COURT_X + (PB_NET_X - PB_COURT_X) / 2
      : PB_NET_X + (PB_COURT_X + PB_COURT_W - PB_NET_X) / 2;
    int zoneY = (pbState.ballZone == 0)
      ? PB_COURT_Y + (PB_COURT_MID_Y - PB_COURT_Y) / 2
      : PB_COURT_MID_Y + (PB_COURT_Y + PB_COURT_H - PB_COURT_MID_Y) / 2;

    display.fillCircle(zoneX, zoneY, 4);

    // Show remaining time bar at bottom
    long remaining = (long)(pbState.reactionDeadline - now);
    if (remaining < 0) remaining = 0;
    int currentReaction = PB_REACTION_TIME_START - (pbState.volleyCount * PB_REACTION_STEP);
    if (currentReaction < PB_REACTION_TIME_MIN) currentReaction = PB_REACTION_TIME_MIN;
    int barWidth = (int)((float)remaining / currentReaction * PB_COURT_W);
    if (barWidth > PB_COURT_W) barWidth = PB_COURT_W;
    display.fillRect(PB_COURT_X, PB_COURT_Y + PB_COURT_H + 1, barWidth, 2);
  }
}

static void renderLobby(SSD1306Wire &display) {
  char buf[32];
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "PICKLEBALL");

  display.setFont(ArialMT_Plain_10);

  // Show wager info on same line as player count
  if (pbState.wagerAmount > 0) {
    snprintf(buf, sizeof(buf), "Players: %u/4  %u sats",
             (unsigned)pbState.playerCount, (unsigned)pbState.wagerAmount);
  } else {
    snprintf(buf, sizeof(buf), "Players: %u/4", (unsigned)pbState.playerCount);
  }
  display.drawString(64, 17, buf);
  if (pbState.isHost && pbState.roomCode.length() > 0 && pbState.wagerAmount == 0) {
    snprintf(buf, sizeof(buf), "Room %s", pbState.roomCode.c_str());
    display.drawString(64, 28, buf);
  }

  // Show wager declined warning
  if (pbState.wagerAmount > 0 && !pbState.wagerActive) {
    display.drawString(64, 28, "Wager declined - FREE");
  }

  // Show player initials
  int nameY = (pbState.wagerAmount > 0 && !pbState.wagerActive) ? 39 : 38;
  for (int i = 0; i < pbState.playerCount && i < MAX_GAME_PLAYERS; i++) {
    char initial[2] = {pbState.players[i].petInitial ? pbState.players[i].petInitial : '?', '\0'};
    display.drawString(20 + (i * 30), nameY, initial);
  }

  // Show instruction
  if (pbState.isHost) {
    if (pbState.playerCount >= 2) {
      display.drawString(64, 52, "Press to START");
    } else {
      long elapsed = (long)((millis() - pbState.phaseStartTime) / 1000);
      long left = (long)(PB_LOBBY_TIMEOUT_MS / 1000) - elapsed;
      if (left < 0) left = 0;
      snprintf(buf, sizeof(buf), "Waiting... %lds", left);
      display.drawString(64, 52, buf);
    }
  } else {
    display.drawString(64, 52, "Waiting for host...");
  }
}

static void renderCountdown(SSD1306Wire &display, int secondsLeft) {
  renderCourt(display);
  renderScore(display);

  // Big countdown number in center
  display.setFont(ArialMT_Plain_24);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  if (secondsLeft > 0) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", secondsLeft);
    display.drawString(64, 20, buf);
  } else {
    display.drawString(64, 20, "GO!");
  }
}

static void renderPointResult(SSD1306Wire &display) {
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);

  const char *msg;
  if (pbState.lastPointReason == PB_MISS_TIMEOUT) {
    msg = "Miss!";
  } else if (pbState.lastPointReason == PB_MISS_WRONG_PLAYER) {
    msg = "Wrong player!";
  } else if (pbState.lastPointReason == PB_MISS_DISCONNECTED) {
    msg = "Player left!";
  } else {
    msg = "Fault!";
  }

  // Show which side scored
  const char *side = (pbState.lastPointWinner == 0) ? "LEFT scores!" : "RIGHT scores!";
  display.drawString(64, PB_COURT_Y + PB_COURT_H / 2 - 10, msg);
  display.drawString(64, PB_COURT_Y + PB_COURT_H / 2 + 2, side);
}

static void renderGameOver(SSD1306Wire &display) {
  char buf[28];
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 2, "GAME OVER");

  display.setFont(ArialMT_Plain_24);
  snprintf(buf, sizeof(buf), "%u - %u", (unsigned)pbState.scoreLeft, (unsigned)pbState.scoreRight);
  display.drawString(64, 18, buf);

  display.setFont(ArialMT_Plain_10);
  uint8_t winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;

  if (winnerSide == mySide()) {
    if (pbState.wagerResult > 0) {
      snprintf(buf, sizeof(buf), "YOU WON %d SATS!", (int)pbState.wagerResult);
      display.drawString(64, 46, buf);
    } else {
      display.drawString(64, 50, "YOU WIN!");
    }
  } else {
    if (pbState.wagerResult < 0) {
      snprintf(buf, sizeof(buf), "Lost %d sats", -(int)pbState.wagerResult);
      display.drawString(64, 46, buf);
    } else {
      display.drawString(64, 50, "You lose");
    }
  }
}

// ============================================================================
// Server Communication (HTTP during lobby phase only)
// ============================================================================

// One HTTPClient for all pickleball requests, deliberately static: the
// HTTPClient destructor stop()s its underlying client, which would close the
// shared keep-alive TLS connection from ganamosHttpBegin() after every call.
static HTTPClient &pbHttp() {
  static HTTPClient http;
  return http;
}

// GET (body == nullptr) or POST JSON to a server path. Returns the HTTP code
// (negative on transport failure); on 200 the payload is stored in response.
static int pbHttpRequest(const String &path, const JsonDocument *body, String &response) {
  HTTPClient &http = pbHttp();
  if (!ganamosHttpBegin(http, path)) return -1;
  http.setTimeout(5000);
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    String payload;
    serializeJson(*body, payload);
    code = http.POST(payload);
  } else {
    code = http.GET();
  }
  if (code == 200) response = http.getString();
  http.end();
  return code;
}

// Only the lobby fields we use are kept when parsing server responses.
static void pbBuildLobbyFilter(JsonDocument &filter) {
  filter["success"] = true;
  filter["status"] = true;
  filter["matchGeneration"] = true;
  filter["gameId"] = true;
  filter["roomCode"] = true;
  filter["action"] = true;
  filter["playerIndex"] = true;
  filter["wagerAmount"] = true;
  filter["wagerStatus"] = true;
  filter["wagerAccepted"] = true;
  filter["players"][0]["macAddress"] = true;
  filter["players"][0]["petInitial"] = true;
  filter["players"][0]["side"] = true;
  filter["players"][0]["position"] = true;
}

static bool pbParseLobbyResponse(const String &payload, JsonDocument &resp) {
  JsonDocument filter;
  pbBuildLobbyFilter(filter);
  return deserializeJson(resp, payload, DeserializationOption::Filter(filter)) ==
         DeserializationError::Ok;
}

// Copy the server's player list into pbState and register ESP-NOW peers for
// everyone except this device. Returns our index in the list, or -1.
static int pbApplyServerPlayers(JsonArrayConst players) {
  int count = 0;
  int myIdx = -1;
  for (JsonVariantConst p : players) {
    if (count >= MAX_GAME_PLAYERS) break;
    ESPNowPlayer &pl = pbState.players[count];
    const char *mac = p["macAddress"] | "";
    if (!espnowParseMac(mac, pl.mac)) memset(pl.mac, 0, sizeof(pl.mac));
    const char *initial = p["petInitial"] | "";
    if (initial[0]) pl.petInitial = initial[0];
    const char *side = p["side"] | "";
    pl.side = (strcmp(side, "right") == 0) ? 1 : 0;
    const char *position = p["position"] | "";
    pl.position = (strcmp(position, "bottom") == 0) ? 1 : 0;

    if (memcmp(pl.mac, pbMyMac, 6) == 0) {
      myIdx = count;
    } else if (!macIsZero(pl.mac)) {
      espnowAddPeer(pl.mac);
    }
    count++;
  }
  pbState.playerCount = (uint8_t)count;
  return myIdx;
}

// Resolve this device's index: prefer our MAC in the list, else the server's
// playerIndex if in range. Returns -1 if neither is usable.
static int pbResolveMyIndex(int macIdx, int serverIdx) {
  if (macIdx >= 0) return macIdx;
  if (serverIdx >= 0 && serverIdx < pbState.playerCount && serverIdx < MAX_GAME_PLAYERS) return serverIdx;
  return -1;
}

static void pbApplyWager(JsonDocument &resp) {
  uint16_t serverWager = resp["wagerAmount"] | 0;
  const char *wagerStatus = resp["wagerStatus"] | "";
  pbState.wagerAmount = serverWager;
  pbState.wagerActive = (strcmp(wagerStatus, "active") == 0 && serverWager > 0);
}

static bool createGameOnServer() {
  JsonDocument doc;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["macAddress"] = espnowGetMacString();
  doc["wagerAmount"] = pbState.wagerAmount;

  String payload;
  int httpCode = pbHttpRequest("/api/game/pickleball/create", &doc, payload);
  if (httpCode != 200) {
    Serial.printf("Pickleball create failed: HTTP %d\n", httpCode);
    return false;
  }

  JsonDocument resp;
  if (!pbParseLobbyResponse(payload, resp) || !(resp["success"] | false)) return false;

  pbState.gameId = (const char *)(resp["gameId"] | "");
  pbState.roomCode = (const char *)(resp["roomCode"] | "");
  const char *action = resp["action"] | "";
  pbApplyWager(resp);

  if (strcmp(action, "join") == 0) {
    // Server found an existing lobby — auto-joined
    pbState.isHost = false;
    pbState.phase = PB_PHASE_LOBBY;
    pbState.phaseStartTime = millis();

    bool wagerAccepted = resp["wagerAccepted"] | true;
    if (!wagerAccepted) pbState.wagerActive = false;

    int macIdx = pbApplyServerPlayers(resp["players"].as<JsonArrayConst>());
    int myIdx = pbResolveMyIndex(macIdx, resp["playerIndex"] | -1);
    if (myIdx <= 0) {
      // Index 0 is the host; anything else invalid means we can't play
      Serial.printf("Pickleball join: invalid player index (mac %d)\n", macIdx);
      return false;
    }
    pbState.myPlayerIndex = (uint8_t)myIdx;

    Serial.printf("Auto-joined existing game as player %d (wager: %d, active: %d)\n",
      pbState.myPlayerIndex, pbState.wagerAmount, pbState.wagerActive);
    return true;
  }

  // We are the host — new game created
  memcpy(pbState.players[0].mac, pbMyMac, 6);
  pbState.players[0].petInitial = ganamosConfig.petName.charAt(0);
  pbState.players[0].side = 0;     // Left
  pbState.players[0].position = 0; // Top
  pbState.players[0].connected = true;
  pbState.players[0].ready = true;
  pbState.playerCount = 1;
  Serial.printf("Hosting new game (wager: %d)\n", pbState.wagerAmount);
  return true;
}

static bool pollLobbyState() {
  if (pbState.gameId.length() == 0) return false;

  String path = "/api/game/pickleball/state?gameId=" + pbState.gameId +
                "&deviceId=" + ganamosConfig.deviceId;
  String payload;
  if (pbHttpRequest(path, nullptr, payload) != 200) return false;

  JsonDocument resp;
  if (!pbParseLobbyResponse(payload, resp) || !(resp["success"] | false)) return false;

  const char *status = resp["status"] | "";
  if (strcmp(status, "cancelled") == 0) {
    pbState.lobbyCancelled = true;
    return false;
  }
  pbState.matchGeneration = resp["matchGeneration"] | pbState.matchGeneration;
  if (!pbState.isHost && strcmp(status, "countdown") == 0 && pbState.phase == PB_PHASE_LOBBY) {
    pbState.phase = PB_PHASE_ESPNOW_SYNC;
    pbState.phaseStartTime = millis();
    pbState.lastStateSend = 0;
    Serial.println(F("Host started; switching to ESP-NOW sync"));
  }

  int macIdx = pbApplyServerPlayers(resp["players"].as<JsonArrayConst>());
  if (!pbState.isHost && macIdx > 0) {
    // Indices shift if someone leaves the lobby; follow our MAC
    pbState.myPlayerIndex = (uint8_t)macIdx;
  }

  pbApplyWager(resp);
  return true;
}

// NOTE: only reachable via the invite path, which is not wired (see
// handlePickleballGame). Kept pending a product decision.
static bool joinGameOnServer(const String &gameId, const String &macAddr) {
  JsonDocument doc;
  doc["gameId"] = gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["macAddress"] = macAddr;

  String payload;
  if (pbHttpRequest("/api/game/pickleball/join", &doc, payload) != 200) return false;

  JsonDocument resp;
  if (!pbParseLobbyResponse(payload, resp) || !(resp["success"] | false)) return false;

  uint16_t serverWager = resp["wagerAmount"] | 0;
  const char *wagerStatus = resp["wagerStatus"] | "";
  bool wagerAccepted = resp["wagerAccepted"] | true;
  pbState.wagerAmount = serverWager;
  pbState.wagerActive = (strcmp(wagerStatus, "active") == 0 && serverWager > 0 && wagerAccepted);

  int macIdx = pbApplyServerPlayers(resp["players"].as<JsonArrayConst>());
  int myIdx = pbResolveMyIndex(macIdx, resp["playerIndex"] | -1);
  if (myIdx <= 0) return false;
  pbState.myPlayerIndex = (uint8_t)myIdx;

  Serial.printf("Joined as player %d wager: %d accepted: %d\n",
    pbState.myPlayerIndex, serverWager, wagerAccepted);
  return true;
}

static bool startCountdownOnServer() {
  if (pbState.gameId.length() == 0) return false;

  JsonDocument doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "start_countdown";

  String payload;
  int httpCode = pbHttpRequest("/api/game/pickleball/state", &doc, payload);
  if (httpCode == 200) {
    JsonDocument filter;
    filter["matchGeneration"] = true;
    JsonDocument resp;
    if (deserializeJson(resp, payload, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
      pbState.matchGeneration = resp["matchGeneration"] | pbState.matchGeneration;
    }
  }
  return httpCode == 200 && pbState.matchGeneration > 0;
}

static bool setWagerConsent(bool accepted) {
  if (pbState.gameId.length() == 0) return false;
  JsonDocument doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "wager_consent";
  doc["accepted"] = accepted;
  String payload;
  return pbHttpRequest("/api/game/pickleball/state", &doc, payload) == 200;
}

static void updateGameWager(uint16_t wagerAmount) {
  if (pbState.gameId.length() == 0) return;

  JsonDocument doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "set_wager";
  doc["wagerAmount"] = wagerAmount;

  String payload;
  if (pbHttpRequest("/api/game/pickleball/state", &doc, payload) == 200) {
    pbState.wagerActive = true;
    Serial.printf("Wager updated to %d sats on server\n", wagerAmount);
  }
}

static void reportGameComplete() {
  if (pbState.gameId.length() == 0) return;

  JsonDocument doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["scoreLeft"] = pbState.scoreLeft;
  doc["scoreRight"] = pbState.scoreRight;
  doc["winnerSide"] = (pbState.scoreLeft >= PB_WINNING_SCORE) ? "left" : "right";

  String payload;
  if (pbHttpRequest("/api/game/pickleball/complete", &doc, payload) != 200) return;

  JsonDocument filter;
  filter["success"] = true;
  filter["wagerSettled"] = true;
  filter["payouts"][0]["amount"] = true;
  filter["payouts"][0]["petName"] = true;
  JsonDocument resp;
  if (deserializeJson(resp, payload, DeserializationOption::Filter(filter)) != DeserializationError::Ok ||
      !(resp["success"] | false)) return;

  bool wagerSettled = resp["wagerSettled"] | false;
  if (!wagerSettled) return;

  // Find this device's payout by pet name
  for (JsonVariantConst payout : resp["payouts"].as<JsonArrayConst>()) {
    const char *petName = payout["petName"] | "";
    if (ganamosConfig.petName == petName) {
      int amount = payout["amount"] | 0;
      pbState.wagerResult = (int16_t)amount;
      Serial.printf("Wager result for us: %d sats\n", amount);
      break;
    }
  }
}
