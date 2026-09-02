#include "pickleball.h"
#include "config.h"
#include "button_handler.h"
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ============================================================================
// Pickleball Game Implementation
// ============================================================================

// Global game state (accessible from ESP-NOW callback)
static PickleballState pbState;
struct PendingButtonPress { uint8_t playerIndex; uint32_t sequence; };
static PendingButtonPress pbButtonQueue[8];
static volatile uint8_t pbButtonQueueHead = 0;
static volatile uint8_t pbButtonQueueTail = 0;
static portMUX_TYPE pbButtonQueueMux = portMUX_INITIALIZER_UNLOCKED;

static int playerIndexForMac(const uint8_t *mac) {
  for (int i = 0; i < pbState.playerCount; i++)
    if (memcmp(pbState.players[i].mac, mac, 6) == 0) return i;
  return -1;
}

static bool queueButtonPress(uint8_t playerIndex, uint32_t sequence) {
  bool queued = false;
  portENTER_CRITICAL(&pbButtonQueueMux);
  uint8_t next = (pbButtonQueueHead + 1) % 8;
  if (next != pbButtonQueueTail) {
    pbButtonQueue[pbButtonQueueHead] = {playerIndex, sequence};
    pbButtonQueueHead = next;
    queued = true;
  }
  portEXIT_CRITICAL(&pbButtonQueueMux);
  return queued;
}

static bool dequeueButtonPress(PendingButtonPress &press) {
  bool found = false;
  portENTER_CRITICAL(&pbButtonQueueMux);
  if (pbButtonQueueTail != pbButtonQueueHead) {
    press = pbButtonQueue[pbButtonQueueTail];
    pbButtonQueueTail = (pbButtonQueueTail + 1) % 8;
    found = true;
  }
  portEXIT_CRITICAL(&pbButtonQueueMux);
  return found;
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
static void hostAdvanceGameState();
static void sendGameStateToPlayers();
static void handleLocalButtonPress();
static void assignSidesForPlayerCount();
static void awardPoint(uint8_t scoringSide, uint8_t reason);

// Sound effects (defined in main .ino)
extern void playButtonChirp();
extern void playMenuSelectTone();
extern bool isQuietHours();

// Flag to suppress duplicate sats celebration after wager game
bool suppressNextSatsCelebration = false;

// Buzzer pin (defined in main .ino)
#ifndef BUZZER_PIN
#define BUZZER_PIN 48
#endif

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

// ---- Sound Effects ----

static void playPickleballHit() {
  if (isQuietHours()) return;
  tone(BUZZER_PIN, 1200, 40);
  delay(30);
  tone(BUZZER_PIN, 1600, 60);
  delay(40);
  noTone(BUZZER_PIN);
}

static void playPickleballPoint() {
  if (isQuietHours()) return;
  tone(BUZZER_PIN, 880, 100);
  delay(80);
  tone(BUZZER_PIN, 1100, 100);
  delay(80);
  tone(BUZZER_PIN, 1320, 200);
  delay(150);
  noTone(BUZZER_PIN);
}

static void playPickleballMiss() {
  if (isQuietHours()) return;
  tone(BUZZER_PIN, 400, 150);
  delay(120);
  tone(BUZZER_PIN, 300, 200);
  delay(150);
  noTone(BUZZER_PIN);
}

static void playPickleballWin() {
  int melody[] = {523, 659, 784, 1047, 784, 1047};
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, melody[i], 120);
    delay(100);
    noTone(BUZZER_PIN);
  }
}

static void playCountdownBeep() {
  if (isQuietHours()) return;
  tone(BUZZER_PIN, 800, 100);
  delay(80);
  noTone(BUZZER_PIN);
}

static void playCountdownGo() {
  if (isQuietHours()) return;
  tone(BUZZER_PIN, 1200, 200);
  delay(150);
  noTone(BUZZER_PIN);
}

static void playWagerVictoryFanfare() {
  int melody[] = {523, 659, 784, 1047, 784, 1047, 1319, 1568};
  int durations[] = {100, 100, 100, 150, 80, 150, 200, 400};
  for (int i = 0; i < 8; i++) {
    tone(BUZZER_PIN, melody[i], durations[i]);
    delay(durations[i] - 20);
    noTone(BUZZER_PIN);
    delay(20);
  }
}

// ============================================================================
// ESP-NOW Receive Callback
// ============================================================================

void pickleballOnESPNowReceive(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 1) return;
  uint8_t msgType = data[0];

  if (pbState.isHost) {
    // HOST receives button presses from players
    if (msgType == MSG_BUTTON_PRESS && len >= sizeof(ButtonPressMessage)) {
      ButtonPressMessage msg;
      memcpy(&msg, data, sizeof(msg));
      int sender = playerIndexForMac(mac);
      if (sender < 0 || msg.generation != pbState.matchGeneration ||
          msg.sequence <= pbState.playerButtonSequence[sender]) return;
      pbState.playerButtonSequence[sender] = msg.sequence;
      pbState.playerLastSeen[sender] = millis();
      queueButtonPress((uint8_t)sender, msg.sequence);
      Serial.printf("🏓 Button press from verified player %d\n", sender);
    }
    else if (msgType == MSG_LOBBY_READY && len >= sizeof(LobbyReadyMessage)) {
      LobbyReadyMessage msg;
      memcpy(&msg, data, sizeof(msg));
      if (msg.generation != pbState.matchGeneration) return;
      // Mark player as connected
      for (int i = 0; i < pbState.playerCount; i++) {
        if (memcmp(pbState.players[i].mac, mac, 6) == 0) {
          pbState.players[i].ready = true;
          pbState.playerLastSeen[i] = millis();
          Serial.printf("✅ Player %d ESP-NOW ready\n", i);
          break;
        }
      }
    }
    else if (msgType == MSG_GAME_START_ACK && len >= sizeof(GameStartAckMessage)) {
      GameStartAckMessage msg;
      memcpy(&msg, data, sizeof(msg));
      int sender = playerIndexForMac(mac);
      if (sender >= 0 && msg.generation == pbState.matchGeneration) {
        pbState.startAcknowledged[sender] = true;
        pbState.playerLastSeen[sender] = millis();
      }
    }
    else if (msgType == MSG_PONG && len >= sizeof(PingMessage)) {
      PingMessage msg;
      memcpy(&msg, data, sizeof(msg));
      int sender = playerIndexForMac(mac);
      if (sender >= 0 && msg.generation == pbState.matchGeneration)
        pbState.playerLastSeen[sender] = millis();
    }
  } else {
    // CLIENT receives game state from host
    if (msgType == MSG_GAME_STATE && len >= sizeof(GameStateMessage)) {
      GameStateMessage msg;
      memcpy(&msg, data, sizeof(msg));
      if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
      pbState.lastReceivedSequence = msg.sequence;
      PickleballPhase newPhase = (PickleballPhase)msg.phase;
      if (newPhase != pbState.phase) {
        pbState.phaseStartTime = millis();
        pbState.ballAnimFrame = 0;
      }
      pbState.phase = newPhase;
      pbState.ballSide = msg.ballSide;
      pbState.ballZone = msg.ballZone;
      pbState.scoreLeft = msg.scoreLeft;
      pbState.scoreRight = msg.scoreRight;
      pbState.servingSide = msg.servingSide;
      pbState.targetPlayerIdx = msg.targetPlayerIdx;
      pbState.reactionDeadline = millis() + msg.reactionTimeMs;
    }
    else if (msgType == MSG_GAME_START && len >= sizeof(GameStartMessage)) {
      GameStartMessage msg;
      memcpy(&msg, data, sizeof(msg));
      if (pbState.matchGeneration != 0 && msg.generation < pbState.matchGeneration) return;
      pbState.matchGeneration = msg.generation;
      pbState.lastReceivedSequence = msg.sequence;
      pbState.myPlayerIndex = msg.yourIndex;
      pbState.playerCount = msg.playerCount;
      for (int i = 0; i < msg.playerCount && i < MAX_GAME_PLAYERS; i++) {
        pbState.players[i].petInitial = msg.initials[i];
        pbState.players[i].side = msg.sides[i];
        pbState.players[i].position = msg.positions[i];
      }
      pbState.phase = PB_PHASE_COUNTDOWN;
      pbState.phaseStartTime = millis();
      GameStartAckMessage ack{MSG_GAME_START_ACK, pbState.matchGeneration, pbState.myPlayerIndex};
      espnowSend(mac, (uint8_t*)&ack, sizeof(ack));
      Serial.printf("🎮 Game starting! I'm player %d\n", msg.yourIndex);
    }
    else if (msgType == MSG_POINT_SCORED && len >= sizeof(PointScoredMessage)) {
      PointScoredMessage msg;
      memcpy(&msg, data, sizeof(msg));
      if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
      pbState.lastReceivedSequence = msg.sequence;
      pbState.scoreLeft = msg.scoreLeft;
      pbState.scoreRight = msg.scoreRight;
      pbState.lastPointWinner = msg.scoringSide;
      pbState.lastPointReason = msg.reason;
      pbState.phase = PB_PHASE_POINT_RESULT;
      pbState.phaseStartTime = millis();

      // Play sound based on whether our side scored
      uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
      if (msg.scoringSide == mySide) {
        playPickleballPoint();
      } else {
        playPickleballMiss();
      }
    }
    else if (msgType == MSG_GAME_OVER && len >= sizeof(GameOverMessage)) {
      GameOverMessage msg;
      memcpy(&msg, data, sizeof(msg));
      if (msg.generation != pbState.matchGeneration || msg.sequence <= pbState.lastReceivedSequence) return;
      pbState.lastReceivedSequence = msg.sequence;
      pbState.scoreLeft = msg.scoreLeft;
      pbState.scoreRight = msg.scoreRight;
      pbState.phase = PB_PHASE_GAME_OVER;
      pbState.phaseStartTime = millis();

      uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
      bool iWon = (msg.winnerSide == mySide);

      // Estimate wager result for display (host settles actual transfer)
      if (pbState.wagerActive && pbState.wagerAmount > 0) {
        pbState.wagerResult = iWon ? (int16_t)pbState.wagerAmount : -(int16_t)pbState.wagerAmount;
      }

      if (iWon) {
        if (pbState.wagerActive && pbState.wagerAmount > 0) {
          playWagerVictoryFanfare();
        } else {
          playPickleballWin();
        }
      } else {
        playPickleballMiss();
      }
    }
    else if (msgType == MSG_PING) {
      // Respond with pong
      PingMessage pong;
      pong.type = MSG_PONG;
      pong.generation = pbState.matchGeneration;
      pong.timestamp = millis();
      espnowSend(mac, (uint8_t*)&pong, sizeof(pong));
    }
  }
}

// ============================================================================
// Main Game Entry Point
// ============================================================================

int handlePickleballPractice(SSD1306Wire &display) {
  Serial.println(F("Starting Pickleball Practice!"));
  pinMode(BUTTON_PIN_PRG, INPUT_PULLUP);
  pinMode(BUTTON_PIN_EXTERNAL, INPUT_PULLUP);

  uint8_t playerScore = 0;
  uint8_t cpuScore = 0;
  uint8_t volleyCount = 0;
  bool ballAtPlayer = true;
  unsigned long practiceStart = millis();

  auto buttonDown = []() {
    return digitalRead(BUTTON_PIN_PRG) == LOW ||
           digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
  };

  auto drawPracticeCourt = [&](bool ballVisible, uint8_t zone,
                               uint8_t requiredPresses, const String &status,
                               int ballX, int ballY) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(2, 0, "YOU " + String(playerScore));
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(126, 0, "CPU " + String(cpuScore));
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
    if (status.length() > 0) {
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 52, status);
    }
    display.display();
  };

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 8, "Pickle Practice");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 31, "Top zone: press once");
  display.drawString(64, 44, "Bottom: press twice");
  display.drawString(64, 56, "First to 6");
  display.display();
  delay(1800);

  while (buttonDown()) delay(10);

  while (playerScore < PB_WINNING_SCORE && cpuScore < PB_WINNING_SCORE &&
         millis() - practiceStart < 90000UL) {
    uint8_t zone = random(0, 2);
    uint8_t requiredPresses = zone == 0 ? 1 : 2;

    // Fly from the hitter to the receiving court. The small parabolic arc
    // makes direction and timing legible without increasing the reaction
    // window or doing more than eight OLED transfers per volley.
    const int startX = ballAtPlayer ? 99 : 29;
    const int endX = ballAtPlayer ? 29 : 99;
    const int startY = PB_COURT_MID_Y;
    const int endY = zone == 0 ? 23 : 48;
    for (int frame = 1; frame <= 8; frame++) {
      int ballX = startX + ((endX - startX) * frame) / 8;
      int ballY = startY + ((endY - startY) * frame) / 8;
      int arc = (8 * frame * (8 - frame)) / 16;
      ballY -= arc;
      drawPracticeCourt(true, zone, requiredPresses, "", ballX, ballY);
      esp_task_wdt_reset();
      delay(PB_BALL_TRAVEL_MS / 8);
    }

    if (ballAtPlayer) {
      unsigned long reactionWindow = max((int)PB_REACTION_TIME_MIN,
        (int)PB_REACTION_TIME_START - (int)volleyCount * PB_REACTION_STEP);
      unsigned long deadline = millis() + reactionWindow;
      uint8_t pressCount = 0;
      bool wasDown = false;
      unsigned long confirmDeadline = 0;
      drawPracticeCourt(true, zone, requiredPresses,
                        requiredPresses == 1 ? "PRESS 1" : "PRESS 2",
                        29, zone == 0 ? 23 : 48);

      while (millis() < deadline || (confirmDeadline && millis() < confirmDeadline)) {
        bool down = buttonDown();
        if (down && !wasDown) {
          pressCount++;
          playPickleballHit();
          if (pressCount == requiredPresses) {
            confirmDeadline = millis() + PB_CONFIRM_WINDOW_MS;
          }
        }
        wasDown = down;
        if (pressCount > requiredPresses) break;
        esp_task_wdt_reset();
        delay(10);
      }

      if (pressCount == requiredPresses) {
        volleyCount++;
        ballAtPlayer = false;
      } else {
        cpuScore++;
        volleyCount = 0;
        drawPracticeCourt(false, zone, requiredPresses,
                          pressCount > requiredPresses ? "TOO MANY" : "MISSED",
                          0, 0);
        playPickleballMiss();
        delay(PB_POINT_DISPLAY_MS);
        ballAtPlayer = true;
      }
    } else {
      // CPU returns most balls, but occasionally misses so a practiced player
      // can win. Longer rallies slightly increase its miss chance.
      int missChance = min(38, 14 + (int)volleyCount * 3);
      delay(max(180, 520 - (int)volleyCount * 25));
      if (random(0, 100) < missChance) {
        playerScore++;
        volleyCount = 0;
        drawPracticeCourt(false, zone, requiredPresses, "CPU MISSED", 0, 0);
        playPickleballPoint();
        delay(PB_POINT_DISPLAY_MS);
        ballAtPlayer = true;
      } else {
        volleyCount++;
        ballAtPlayer = true;
      }
    }
  }

  bool won = playerScore > cpuScore;
  Serial.println("Pickleball practice complete! Score: " +
                 String(playerScore) + "-" + String(cpuScore));
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 12, won ? "You win!" : "Practice over");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 36, "YOU " + String(playerScore) + " - " +
                     String(cpuScore) + " CPU");
  display.drawString(64, 52, "Offline / no wager");
  display.display();
  if (won) playPickleballWin();
  else playPickleballMiss();
  delay(1800);
  return won ? 10 : 5;
}

int handlePickleballGame(SSD1306Wire &display) {
  extern GanamosConfig ganamosConfig;

  // Initialize game state
  memset(&pbState, 0, sizeof(pbState));
  pbState.phase = PB_PHASE_LOBBY;
  pbState.isHost = true;  // Assume host until we discover we're joining
  pbState.myPlayerIndex = 0;
  pbState.phaseStartTime = millis();

  // Check if we're joining an existing game (invite received via config)
  extern String pendingPickleballGameId;
  extern String pendingPickleballHostMac;

  if (pendingPickleballGameId.length() > 0) {
    pbState.isHost = false;
    pbState.gameId = pendingPickleballGameId;
    Serial.println("🏓 Joining existing game: " + pbState.gameId);
  }

  Serial.println(F("🏓 Starting Pickleball game..."));

  // Ensure WiFi is available for lobby phase
  if (WiFi.status() != WL_CONNECTED) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, "Connecting WiFi...");
    display.display();

    WiFi.mode(WIFI_STA);
    WiFi.begin();

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
      esp_task_wdt_reset();
      delay(250);
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 20, "Connecting WiFi...");
      int dots = ((millis() - wifiStart) / 500) % 4;
      String dotStr = "";
      for (int i = 0; i < dots; i++) dotStr += ".";
      display.drawString(64, 35, dotStr);
      display.display();
    }

    if (WiFi.status() != WL_CONNECTED) {
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 25, "WiFi failed");
      display.drawString(64, 40, "Try again later");
      display.display();
      delay(2000);
      return 0;
    }
    Serial.println(F("✅ WiFi connected for Pickleball"));
  }

  // Initialize ESP-NOW (works alongside WiFi STA)
  if (!espnowInit()) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 25, "ESP-NOW init failed");
    display.display();
    delay(2000);
    return 0;
  }

  // Set our receive callback
  espnowSetReceiveCallback(pickleballOnESPNowReceive);

  String myMac = espnowGetMacString();
  Serial.println("My MAC: " + myMac);

  // --- JOIN MODE: Join existing game from invite ---
  if (!pbState.isHost) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, "Joining game...");
    display.display();

    if (!joinGameOnServer(pbState.gameId, myMac)) {
      display.clear();
      display.drawString(64, 25, "Failed to join");
      display.drawString(64, 40, "game");
      display.display();
      delay(2000);
      espnowDeinit();
      return 0;
    }

    Serial.println("Joined game: " + pbState.gameId);
    pbState.phase = PB_PHASE_LOBBY;
    pbState.phaseStartTime = millis();
  }

  // --- No invite: try to find an existing game first, then create ---
  if (pbState.isHost) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 20, "Finding game...");
    display.display();

    // Try to find and join an existing lobby (wagerAmount=0 for discovery)
    pbState.wagerAmount = 0;
    if (!createGameOnServer()) {
      display.clear();
      display.drawString(64, 25, "Failed to connect");
      display.display();
      delay(2000);
      espnowDeinit();
      return 0;
    }

    if (!pbState.isHost) {
      // Found and joined an existing game
      Serial.println("🏓 Joined existing game: " + pbState.gameId);
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 20, "Found game!");
      display.drawString(64, 35, String(pbState.playerCount) + " players");
      display.display();
      delay(800);
    } else {
      // No game found — we're hosting. Now ask about wager.
      Serial.println("🏓 No existing game found, hosting new one: " + pbState.gameId);

      const uint16_t wagerOptions[] = {0, 100, 500, 1000};
      int wagerIdx = 0;
      bool wagerSelected = false;
      bool lastWagerBtn = false;
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
          display.drawString(64, 20, String(wagerOptions[wagerIdx]));
        }

        display.setFont(ArialMT_Plain_10);
        display.drawString(64, 50, "Tap=cycle  Hold=pick");
        display.display();

        bool btn = digitalRead(BUTTON_PIN_PRG) == LOW || digitalRead(BUTTON_PIN_EXTERNAL) == LOW;

        if (btn && !lastWagerBtn) {
          unsigned long pressStart = millis();
          while ((digitalRead(BUTTON_PIN_PRG) == LOW || digitalRead(BUTTON_PIN_EXTERNAL) == LOW)
                 && millis() - pressStart < 2000) {
            esp_task_wdt_reset();
            delay(20);
          }
          unsigned long dur = millis() - pressStart;

          if (dur >= 500) {
            playMenuSelectTone();
            wagerSelected = true;
          } else {
            playButtonChirp();
            wagerIdx = (wagerIdx + 1) % 4;
          }
        }
        lastWagerBtn = btn;
        delay(30);
      }

      uint16_t selectedWager = wagerOptions[wagerIdx];
      Serial.printf("🏓 Wager selected: %d sats\n", selectedWager);

      // Transition game from "setup" to "lobby" (also sets wager)
      pbState.wagerAmount = selectedWager;
      updateGameWager(selectedWager);
    }
  }

  // --- Wager acceptance screen for joiners ---
  if (!pbState.isHost && pbState.wagerAmount > 0) {
    bool wagerDecision = false;
    bool wagerAccepted = false;
    bool lastAccBtn = false;
    unsigned long acceptTimeout = millis() + 15000;

    while (!wagerDecision && millis() < acceptTimeout) {
      esp_task_wdt_reset();
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 0, "WAGER");

      display.setFont(ArialMT_Plain_24);
      display.drawString(64, 18, String(pbState.wagerAmount));

      display.setFont(ArialMT_Plain_10);
      display.drawString(64, 46, "Hold=Accept  Tap=Decline");
      display.display();

      bool btn = digitalRead(BUTTON_PIN_PRG) == LOW || digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
      if (btn && !lastAccBtn) {
        unsigned long pressStart = millis();
        while ((digitalRead(BUTTON_PIN_PRG) == LOW || digitalRead(BUTTON_PIN_EXTERNAL) == LOW)
               && millis() - pressStart < 2000) {
          esp_task_wdt_reset();
          delay(20);
        }
        unsigned long dur = millis() - pressStart;
        wagerDecision = true;
        wagerAccepted = (dur >= 500);
      }
      lastAccBtn = btn;
      delay(30);
    }

    if (!wagerAccepted) {
      Serial.println(F("🏓 Wager declined, exiting game"));
      setWagerConsent(false); // Removes this device from the lobby atomically.
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 25, "Wager declined");
      display.display();
      delay(1500);
      espnowDeinit();
      return 0;
    }

    playMenuSelectTone();
    if (!setWagerConsent(true)) {
      display.clear();
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 25, "Could not accept wager");
      display.display();
      delay(1500);
      espnowDeinit();
      return 0;
    }
    Serial.printf("🏓 Wager of %d sats accepted\n", pbState.wagerAmount);
  }

  // ====================================================================
  // Main game loop (blocks until game ends or cancelled)
  // ====================================================================

  bool gameRunning = true;
  unsigned long lastRender = 0;
  const unsigned long RENDER_INTERVAL = 50; // 20 FPS

  while (gameRunning) {
    unsigned long now = millis();
    esp_task_wdt_reset(); // Feed watchdog

    // --- Check for local button press ---
    bool prgBtn = digitalRead(BUTTON_PIN_PRG) == LOW;
    bool extBtn = digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
    static bool lastBtnState = false;
    bool currentBtn = prgBtn || extBtn;

    if (currentBtn && !lastBtnState) {
      // Button just pressed
      handleLocalButtonPress();
    }
    lastBtnState = currentBtn;

    // --- Phase-specific logic ---
    switch (pbState.phase) {
      case PB_PHASE_LOBBY: {
        // Poll server for player list
        if (now - pbState.lastLobbyPoll > PB_LOBBY_POLL_MS) {
          pbState.lastLobbyPoll = now;
          pollLobbyState();
        }

        // Check for lobby timeout (100 seconds)
        if (now - pbState.phaseStartTime > 100000 && pbState.playerCount < 2) {
          Serial.println(F("Lobby timed out"));
          gameRunning = false;
        }
        break;
      }

      case PB_PHASE_ESPNOW_SYNC: {
        // Host: wait for all players to send LOBBY_READY via ESP-NOW
        if (pbState.isHost) {
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
          if (allReady && allStartAcknowledged) {
            Serial.println(F("ESP-NOW sync complete, starting countdown"));
            pbState.phase = PB_PHASE_COUNTDOWN;
            pbState.phaseStartTime = now;
          } else if (allReady && now - pbState.lastStateSend > 250) {
            pbState.lastStateSend = now;
            // Send GAME_START to each player with their index
            GameStartMessage startMsg;
            startMsg.type = MSG_GAME_START;
            startMsg.generation = pbState.matchGeneration;
            startMsg.sequence = ++pbState.sendSequence;
            startMsg.playerCount = pbState.playerCount;
            for (int i = 0; i < pbState.playerCount; i++) {
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
          if (now - pbState.phaseStartTime > 15000 && !(allReady && allStartAcknowledged)) {
            Serial.println(F("ESP-NOW sync failed: missing ready/start acknowledgement"));
            gameRunning = false;
          }
        } else {
          // Client: send LOBBY_READY and wait for GAME_START
          if (now - pbState.lastStateSend > 500) {
            pbState.lastStateSend = now;
            LobbyReadyMessage ready;
            ready.type = MSG_LOBBY_READY;
            ready.generation = pbState.matchGeneration;
            ready.playerIndex = pbState.myPlayerIndex;
            ready.petInitial = ganamosConfig.petName.charAt(0);
            espnowBroadcast((uint8_t*)&ready, sizeof(ready));
          }

          // Timeout waiting for game start
          if (now - pbState.phaseStartTime > 15000) {
            Serial.println(F("ESP-NOW sync timeout"));
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
        } else {
          static int lastBeep = -1;
          if (secondsLeft != lastBeep) {
            lastBeep = secondsLeft;
            playCountdownBeep();
          }
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

          // Reset button tracking for new volley
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
          uint8_t targetSide = pbState.players[pbState.targetPlayerIdx].side;

          // --- Handle incoming button press ---
          PendingButtonPress pendingPress;
          if (dequeueButtonPress(pendingPress)) {
            uint8_t pressedIdx = pendingPress.playerIndex;
            uint8_t presserSide = pbState.players[pressedIdx].side;

            if (presserSide != targetSide) {
              // Wrong side pressed — fault
              awardPoint(1 - presserSide, PB_MISS_WRONG_PLAYER);
            } else if (pbState.playerCount > 2 && pressedIdx != pbState.targetPlayerIdx) {
              // Doubles: wrong player on correct side
              uint8_t scoringSide = 1 - targetSide;
              awardPoint(scoringSide, PB_MISS_WRONG_PLAYER);
            } else if (pbState.hitConfirmed) {
              // Already hit the required count — extra press is a fault
              pbState.hitConfirmed = false;
              uint8_t scoringSide = 1 - targetSide;
              awardPoint(scoringSide, PB_MISS_DOUBLE_FAULT);
            } else {
              pbState.buttonPressCount++;

              if (pbState.buttonPressCount == pbState.requiredPresses) {
                // Correct count reached — start confirm window
                pbState.hitConfirmed = true;
                pbState.confirmDeadline = now + PB_CONFIRM_WINDOW_MS;
                playPickleballHit();
              } else if (pbState.buttonPressCount > pbState.requiredPresses) {
                // Over-pressed before confirm even started (shouldn't happen, safety net)
                uint8_t scoringSide = 1 - targetSide;
                awardPoint(scoringSide, PB_MISS_DOUBLE_FAULT);
              }
            }
          }

          // --- Check confirm window expiry (successful hit!) ---
          if (pbState.hitConfirmed && now >= pbState.confirmDeadline) {
            pbState.hitConfirmed = false;
            pbState.volleyCount++;
            pbState.servingSide = targetSide;
            pbState.phase = PB_PHASE_SERVING;
            pbState.phaseStartTime = now;
            sendGameStateToPlayers();
          }

          // --- Check reaction timeout (missed the ball) ---
          if (!pbState.hitConfirmed && now > pbState.reactionDeadline) {
            uint8_t scoringSide = 1 - targetSide;
            awardPoint(scoringSide, PB_MISS_TIMEOUT);
          }
        }
        break;
      }

      case PB_PHASE_POINT_RESULT: {
        if (now - pbState.phaseStartTime > PB_POINT_DISPLAY_MS) {
          // Check for game over
          if (pbState.scoreLeft >= PB_WINNING_SCORE || pbState.scoreRight >= PB_WINNING_SCORE) {
            pbState.phase = PB_PHASE_GAME_OVER;
            pbState.phaseStartTime = now;

            if (pbState.isHost) {
              // Send game over to all players
              GameOverMessage gameOver;
              gameOver.type = MSG_GAME_OVER;
              gameOver.generation = pbState.matchGeneration;
              gameOver.sequence = ++pbState.sendSequence;
              gameOver.winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
              gameOver.scoreLeft = pbState.scoreLeft;
              gameOver.scoreRight = pbState.scoreRight;
              espnowBroadcast((uint8_t*)&gameOver, sizeof(gameOver));

              // Report to server (also parses wager settlement)
              reportGameComplete();

              uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
              if (gameOver.winnerSide == mySide) {
                if (pbState.wagerResult > 0) {
                  playWagerVictoryFanfare();
                } else {
                  playPickleballWin();
                }
              } else {
                playPickleballMiss();
              }
            }
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
        uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
        uint8_t winSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
        if (winSide == mySide) {
          if ((now - pbState.phaseStartTime) % 500 < 250) {
            digitalWrite(RGB_LED, HIGH);
          } else {
            digitalWrite(RGB_LED, LOW);
          }
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

    if (pbState.isHost && pbState.phase >= PB_PHASE_COUNTDOWN &&
        pbState.phase < PB_PHASE_GAME_OVER && now - pbState.lastPresencePing > 2000) {
      pbState.lastPresencePing = now;
      PingMessage ping{MSG_PING, pbState.matchGeneration, now};
      espnowBroadcast((uint8_t*)&ping, sizeof(ping));
      for (int i = 1; i < pbState.playerCount; i++) {
        if (pbState.playerLastSeen[i] != 0 && now - pbState.playerLastSeen[i] > 8000) {
          Serial.printf("🏓 Player %d disconnected\n", i);
          uint8_t disconnectedSide = pbState.players[i].side;
          if (disconnectedSide == 0) pbState.scoreRight = PB_WINNING_SCORE;
          else pbState.scoreLeft = PB_WINNING_SCORE;
          awardPoint(1 - disconnectedSide, PB_MISS_DISCONNECTED);
          break;
        }
      }
    }

    // --- Render ---
    if (now - lastRender > RENDER_INTERVAL) {
      lastRender = now;

      display.clear();

      switch (pbState.phase) {
        case PB_PHASE_LOBBY:
          renderLobby(display);
          break;
        case PB_PHASE_ESPNOW_SYNC:
          display.setFont(ArialMT_Plain_10);
          display.setTextAlignment(TEXT_ALIGN_CENTER);
          display.drawString(64, 20, "Connecting...");
          display.drawString(64, 35, String(pbState.playerCount) + " players");
          break;
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

    delay(5); // Small yield
    yield();
  }

  // Clean up
  espnowDeinit();
  digitalWrite(RGB_LED, LOW);

  // Suppress duplicate sats celebration if wager was involved
  if (pbState.wagerResult != 0) {
    suppressNextSatsCelebration = true;
  }

  // Return happiness bonus. Wager pet coins arrive from the authoritative server
  // ledger on the next config sync; crediting locally here would double-mint them.
  if (pbState.phase == PB_PHASE_GAME_OVER) {
    uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
    uint8_t winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;
    if (winnerSide == mySide) {
      return 30;
    } else {
      return 15;
    }
  }

  return 0;
}

// ============================================================================
// Helper: Award a point
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

  // Reset rally volley count (difficulty resets each point)
  pbState.volleyCount = 0;

  // Alternate serve
  pbState.servingSide = 1 - scoringSide;

  // Send point scored to all players
  PointScoredMessage msg;
  msg.type = MSG_POINT_SCORED;
  msg.generation = pbState.matchGeneration;
  msg.sequence = ++pbState.sendSequence;
  msg.scoringSide = scoringSide;
  msg.scoreLeft = pbState.scoreLeft;
  msg.scoreRight = pbState.scoreRight;
  msg.reason = reason;
  espnowBroadcast((uint8_t*)&msg, sizeof(msg));

  // Sound on host
  uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
  if (scoringSide == mySide) {
    playPickleballPoint();
  } else {
    playPickleballMiss();
  }

  Serial.printf("🏓 Point! L:%d R:%d (reason:%d)\n", pbState.scoreLeft, pbState.scoreRight, reason);
}

// ============================================================================
// Button handling
// ============================================================================

static void handleLocalButtonPress() {
  if (pbState.phase == PB_PHASE_LOBBY) {
    // In lobby: host can start game if 2+ players
    if (pbState.isHost && pbState.playerCount >= 2) {
      pbState.lobbyStartRequested = true;
      Serial.println(F("Host starting game..."));
      if (startCountdownOnServer()) {
        pbState.phase = PB_PHASE_ESPNOW_SYNC;
        pbState.phaseStartTime = millis();
      } else {
        Serial.println(F("Host start rejected; remaining in lobby"));
      }
    }
    return;
  }

  if (pbState.phase == PB_PHASE_BALL_LANDED) {
    if (pbState.isHost) {
      // Host handles own button press directly
      queueButtonPress(pbState.myPlayerIndex, ++pbState.playerButtonSequence[pbState.myPlayerIndex]);
    } else {
      // Client: send button press to host
      ButtonPressMessage msg;
      msg.type = MSG_BUTTON_PRESS;
      msg.generation = pbState.matchGeneration;
      msg.sequence = ++pbState.playerButtonSequence[pbState.myPlayerIndex];
      msg.playerIndex = pbState.myPlayerIndex;
      msg.timestamp = millis();
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
  msg.reactionTimeMs = (pbState.reactionDeadline > millis())
    ? (pbState.reactionDeadline - millis()) : 0;
  msg.timestamp = millis();

  espnowBroadcast((uint8_t*)&msg, sizeof(msg));
}

// ============================================================================
// Rendering
// ============================================================================

static void renderScore(SSD1306Wire &display) {
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, String(pbState.scoreLeft) + " - " + String(pbState.scoreRight));
}

static void renderCourt(SSD1306Wire &display) {
  // Outer court border
  display.drawRect(PB_COURT_X, PB_COURT_Y, PB_COURT_W, PB_COURT_H);

  // Net (center vertical line, dashed)
  for (int y = PB_COURT_Y; y < PB_COURT_Y + PB_COURT_H; y += 4) {
    display.drawLine(PB_NET_X, y, PB_NET_X, min(y + 2, PB_COURT_Y + PB_COURT_H - 1));
  }

  // Horizontal splits on each side (top/bottom zones)
  // Left side split
  display.drawLine(PB_COURT_X, PB_COURT_MID_Y, PB_NET_X - 1, PB_COURT_MID_Y);
  // Right side split
  display.drawLine(PB_NET_X + 1, PB_COURT_MID_Y, PB_COURT_X + PB_COURT_W - 1, PB_COURT_MID_Y);

  // Player initials in their zones
  display.setFont(ArialMT_Plain_10);

  for (int i = 0; i < pbState.playerCount; i++) {
    ESPNowPlayer &p = pbState.players[i];
    int x, y;

    if (p.side == 0) { // Left
      x = PB_COURT_X + (PB_NET_X - PB_COURT_X) / 2;
    } else { // Right
      x = PB_NET_X + (PB_COURT_X + PB_COURT_W - PB_NET_X) / 2;
    }

    if (p.position == 0) { // Top
      y = PB_COURT_Y + 4;
    } else { // Bottom
      y = PB_COURT_MID_Y + 4;
    }

    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(x, y, String(p.petInitial));
  }

  // For solo player on a side: show "1" and "2" indicators
  for (uint8_t side = 0; side <= 1; side++) {
    int playersOnSide = 0;
    int soloIdx = -1;
    for (int i = 0; i < pbState.playerCount; i++) {
      if (pbState.players[i].side == side) {
        playersOnSide++;
        soloIdx = i;
      }
    }

    if (playersOnSide == 1) {
      // Show press count indicators
      int baseX = (side == 0) ? PB_COURT_X + 5 : PB_COURT_X + PB_COURT_W - 12;
      uint8_t playerPos = pbState.players[soloIdx].position;

      // The zone matching the player's position = 1 press
      // The other zone = 2 presses
      display.setFont(ArialMT_Plain_10);
      if (playerPos == 0) {
        // Player is in top: top=1 press, bottom=2 presses
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
  // Animate ball from serving side's edge to the net, then from net to target zone
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

  // Draw ball (filled circle)
  display.fillCircle(ballX, ballY, 3);
}

static void renderBallLanded(SSD1306Wire &display) {
  // Ball is in the target zone - flash it to indicate "HIT NOW!"
  unsigned long elapsed = millis() - pbState.phaseStartTime;
  bool visible = (elapsed / 150) % 2 == 0; // Blink every 150ms

  if (visible) {
    int zoneX;
    if (pbState.ballSide == 0) {
      zoneX = PB_COURT_X + (PB_NET_X - PB_COURT_X) / 2;
    } else {
      zoneX = PB_NET_X + (PB_COURT_X + PB_COURT_W - PB_NET_X) / 2;
    }

    int zoneY;
    if (pbState.ballZone == 0) {
      zoneY = PB_COURT_Y + (PB_COURT_MID_Y - PB_COURT_Y) / 2;
    } else {
      zoneY = PB_COURT_MID_Y + (PB_COURT_Y + PB_COURT_H - PB_COURT_MID_Y) / 2;
    }

    display.fillCircle(zoneX, zoneY, 4);

    // Show remaining time bar at bottom
    unsigned long remaining = 0;
    if (pbState.reactionDeadline > millis()) {
      remaining = pbState.reactionDeadline - millis();
    }
    int currentReaction = PB_REACTION_TIME_START - (pbState.volleyCount * PB_REACTION_STEP);
    if (currentReaction < PB_REACTION_TIME_MIN) currentReaction = PB_REACTION_TIME_MIN;
    int barWidth = (int)((float)remaining / currentReaction * PB_COURT_W);
    display.fillRect(PB_COURT_X, PB_COURT_Y + PB_COURT_H + 1, barWidth, 2);
  }
}

static void renderLobby(SSD1306Wire &display) {
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "PICKLEBALL");

  display.setFont(ArialMT_Plain_10);

  // Show wager info on same line as player count
  String infoLine = "Players: " + String(pbState.playerCount) + "/4";
  if (pbState.wagerAmount > 0) {
    infoLine += "  " + String(pbState.wagerAmount) + " sats";
  }
  display.drawString(64, 17, infoLine);
  if (pbState.isHost && pbState.roomCode.length() > 0 && pbState.wagerAmount == 0) {
    display.drawString(64, 28, "Room " + pbState.roomCode);
  }

  // Show wager declined warning
  if (pbState.wagerAmount > 0 && !pbState.wagerActive) {
    display.drawString(64, 28, "Wager declined - FREE");
  }

  // Show player names
  int nameY = (pbState.wagerAmount > 0 && !pbState.wagerActive) ? 39 : 38;
  for (int i = 0; i < pbState.playerCount && i < 4; i++) {
    String name = String(pbState.players[i].petInitial);
    int x = 20 + (i * 30);
    display.drawString(x, nameY, name);
  }

  // Show instruction
  if (pbState.isHost) {
    if (pbState.playerCount >= 2) {
      display.drawString(64, 52, "Hold to START");
    } else {
      int elapsed = (millis() - pbState.phaseStartTime) / 1000;
      display.drawString(64, 52, "Waiting... " + String(60 - elapsed) + "s");
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
    display.drawString(64, 20, String(secondsLeft));
  } else {
    display.drawString(64, 20, "GO!");
  }
}

static void renderPointResult(SSD1306Wire &display) {
  // Flash the zone that was missed/hit
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);

  String msg;
  if (pbState.lastPointReason == PB_MISS_TIMEOUT) {
    msg = "Miss!";
  } else if (pbState.lastPointReason == PB_MISS_WRONG_PLAYER) {
    msg = "Wrong player!";
  } else {
    msg = "Fault!";
  }

  // Show which side scored
  String sideStr = (pbState.lastPointWinner == 0) ? "LEFT" : "RIGHT";
  display.drawString(64, PB_COURT_Y + PB_COURT_H / 2 - 10, msg);
  display.drawString(64, PB_COURT_Y + PB_COURT_H / 2 + 2, sideStr + " scores!");
}

static void renderGameOver(SSD1306Wire &display) {
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 2, "GAME OVER");

  display.setFont(ArialMT_Plain_24);
  display.drawString(64, 18, String(pbState.scoreLeft) + " - " + String(pbState.scoreRight));

  display.setFont(ArialMT_Plain_10);
  uint8_t mySide = pbState.players[pbState.myPlayerIndex].side;
  uint8_t winnerSide = (pbState.scoreLeft >= PB_WINNING_SCORE) ? 0 : 1;

  if (winnerSide == mySide) {
    if (pbState.wagerResult > 0) {
      display.drawString(64, 46, "YOU WON " + String(pbState.wagerResult) + " SATS!");
    } else {
      display.drawString(64, 50, "YOU WIN!");
    }
  } else {
    if (pbState.wagerResult < 0) {
      display.drawString(64, 46, "Lost " + String(-pbState.wagerResult) + " sats");
    } else {
      display.drawString(64, 50, "You lose");
    }
  }
}

// ============================================================================
// Server Communication (HTTP during lobby phase only)
// ============================================================================

static bool createGameOnServer() {
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/create";

  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["macAddress"] = espnowGetMacString();
  doc["wagerAmount"] = pbState.wagerAmount;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  bool success = false;

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> resp;
    if (deserializeJson(resp, payload) == DeserializationError::Ok) {
      if (resp["success"]) {
        pbState.gameId = resp["gameId"].as<String>();
        pbState.roomCode = resp["roomCode"].as<String>();
        String action = resp["action"].as<String>();

        // Parse wager fields from response
        uint16_t serverWager = resp["wagerAmount"] | 0;
        String wagerStatus = resp["wagerStatus"].as<String>();
        pbState.wagerAmount = serverWager;
        pbState.wagerActive = (wagerStatus == "active" && serverWager > 0);

        if (action == "join") {
          // Server found an existing lobby — auto-joined
          pbState.isHost = false;
          pbState.myPlayerIndex = resp["playerIndex"] | 0;
          pbState.phase = PB_PHASE_LOBBY;
          pbState.phaseStartTime = millis();

          bool wagerAccepted = resp["wagerAccepted"] | true;
          if (!wagerAccepted) pbState.wagerActive = false;

          JsonArray players = resp["players"];
          pbState.playerCount = min((int)players.size(), MAX_GAME_PLAYERS);

          for (int i = 0; i < pbState.playerCount; i++) {
            JsonObject p = players[i];
            String mac = p["macAddress"].as<String>();
            espnowParseMac(mac, pbState.players[i].mac);

            String initial = p["petInitial"].as<String>();
            if (initial.length() > 0) {
              pbState.players[i].petInitial = initial.charAt(0);
            }

            String pSide = p["side"].as<String>();
            pbState.players[i].side = (pSide == "right") ? 1 : 0;

            String pPos = p["position"].as<String>();
            pbState.players[i].position = (pPos == "bottom") ? 1 : 0;

            if (i != pbState.myPlayerIndex) {
              espnowAddPeer(pbState.players[i].mac);
            }
          }

          Serial.printf("🏓 Auto-joined existing game as player %d (wager: %d, active: %d)\n",
            pbState.myPlayerIndex, pbState.wagerAmount, pbState.wagerActive);
          success = true;
        } else {
          // We are the host — new game created
          String myMac = espnowGetMacString();
          espnowParseMac(myMac, pbState.players[0].mac);
          pbState.players[0].petInitial = ganamosConfig.petName.charAt(0);
          pbState.players[0].side = 0;     // Left
          pbState.players[0].position = 0; // Top
          pbState.players[0].connected = true;
          pbState.players[0].ready = true;
          pbState.playerCount = 1;
          Serial.printf("🏓 Hosting new game (wager: %d, status: %s)\n",
            pbState.wagerAmount, wagerStatus.c_str());
          success = true;
        }
      }
    }
  }

  http.end();
  client->stop();
  delete client;
  return success;
}

static bool pollLobbyState() {
  if (pbState.gameId.length() == 0) return false;
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/state?gameId=" + pbState.gameId + "&deviceId=" + ganamosConfig.deviceId;

  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }

  http.setTimeout(5000);
  int httpCode = http.GET();
  bool updated = false;

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> resp;
    if (deserializeJson(resp, payload) == DeserializationError::Ok && resp["success"]) {

      String status = resp["status"].as<String>();
      if (status == "cancelled") {
        pbState.phase = PB_PHASE_GAME_OVER; // Exit
        http.end(); client->stop(); delete client;
        return false;
      }
      pbState.matchGeneration = resp["matchGeneration"] | pbState.matchGeneration;
      if (!pbState.isHost && status == "countdown" && pbState.phase == PB_PHASE_LOBBY) {
        pbState.phase = PB_PHASE_ESPNOW_SYNC;
        pbState.phaseStartTime = millis();
        pbState.lastStateSend = 0;
        Serial.println(F("🏓 Host started; switching to ESP-NOW sync"));
      }

      JsonArray players = resp["players"];
      pbState.playerCount = min((int)players.size(), MAX_GAME_PLAYERS);

      for (int i = 0; i < pbState.playerCount; i++) {
        JsonObject p = players[i];
        String mac = p["macAddress"].as<String>();
        espnowParseMac(mac, pbState.players[i].mac);

        String initial = p["petInitial"].as<String>();
        if (initial.length() > 0) {
          pbState.players[i].petInitial = initial.charAt(0);
        }

        String side = p["side"].as<String>();
        pbState.players[i].side = (side == "right") ? 1 : 0;

        String position = p["position"].as<String>();
        pbState.players[i].position = (position == "bottom") ? 1 : 0;

        // Add as ESP-NOW peer
        espnowAddPeer(pbState.players[i].mac);
      }

      // Parse wager fields
      uint16_t serverWager = resp["wagerAmount"] | 0;
      String wagerStatus = resp["wagerStatus"].as<String>();
      pbState.wagerAmount = serverWager;
      pbState.wagerActive = (wagerStatus == "active" && serverWager > 0);

      updated = true;
    }
  }

  http.end();
  client->stop();
  delete client;
  return updated;
}

static bool joinGameOnServer(const String &gameId, const String &macAddr) {
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/join";

  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["gameId"] = gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["macAddress"] = macAddr;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  bool success = false;

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> resp;
    if (deserializeJson(resp, payload) == DeserializationError::Ok && resp["success"]) {
      // Get our player assignment
      pbState.myPlayerIndex = resp["playerIndex"] | 0;

      String side = resp["side"].as<String>();
      String position = resp["position"].as<String>();

      // Parse wager fields
      uint16_t serverWager = resp["wagerAmount"] | 0;
      String wagerStatus = resp["wagerStatus"].as<String>();
      bool wagerAccepted = resp["wagerAccepted"] | true;
      pbState.wagerAmount = serverWager;
      pbState.wagerActive = (wagerStatus == "active" && serverWager > 0 && wagerAccepted);

      success = true;
      Serial.printf("🏓 Joined as player %d (%s/%s) wager: %d accepted: %d\n",
        pbState.myPlayerIndex, side.c_str(), position.c_str(), serverWager, wagerAccepted);

      // Parse all players from response
      JsonArray players = resp["players"];
      pbState.playerCount = min((int)players.size(), MAX_GAME_PLAYERS);

      for (int i = 0; i < pbState.playerCount; i++) {
        JsonObject p = players[i];
        String mac = p["macAddress"].as<String>();
        espnowParseMac(mac, pbState.players[i].mac);

        String initial = p["petInitial"].as<String>();
        if (initial.length() > 0) {
          pbState.players[i].petInitial = initial.charAt(0);
        }

        String pSide = p["side"].as<String>();
        pbState.players[i].side = (pSide == "right") ? 1 : 0;

        String pPos = p["position"].as<String>();
        pbState.players[i].position = (pPos == "bottom") ? 1 : 0;

        // Add all other players as ESP-NOW peers
        if (i != pbState.myPlayerIndex) {
          espnowAddPeer(pbState.players[i].mac);
        }
      }
    }
  }

  http.end();
  client->stop();
  delete client;
  return success;
}

static bool startCountdownOnServer() {
  if (pbState.gameId.length() == 0) return false;
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/state";

  if (!http.begin(*client, url)) {
    delete client;
    return false;
  }

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "start_countdown";

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    StaticJsonDocument<256> resp;
    if (deserializeJson(resp, http.getString()) == DeserializationError::Ok) {
      pbState.matchGeneration = resp["matchGeneration"] | pbState.matchGeneration;
    }
  }
  http.end();
  client->stop();
  delete client;
  return httpCode == 200 && pbState.matchGeneration > 0;
}

static bool setWagerConsent(bool accepted) {
  if (pbState.gameId.length() == 0) return false;
  extern GanamosConfig ganamosConfig;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient http;
  if (!http.begin(client, "https://www.ganamos.earth/api/game/pickleball/state")) return false;
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<256> doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "wager_consent";
  doc["accepted"] = accepted;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  http.end();
  return code == 200;
}

static void updateGameWager(uint16_t wagerAmount) {
  if (pbState.gameId.length() == 0) return;
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/state";

  if (!http.begin(*client, url)) {
    delete client;
    return;
  }

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["action"] = "set_wager";
  doc["wagerAmount"] = wagerAmount;

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    pbState.wagerActive = true;
    Serial.printf("🏓 Wager updated to %d sats on server\n", wagerAmount);
  }

  http.end();
  client->stop();
  delete client;
}

static void reportGameComplete() {
  if (pbState.gameId.length() == 0) return;
  extern GanamosConfig ganamosConfig;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return;
  client->setInsecure();
  client->setTimeout(5000);

  HTTPClient http;
  String url = "https://www.ganamos.earth/api/game/pickleball/complete";

  if (!http.begin(*client, url)) {
    delete client;
    return;
  }

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> doc;
  doc["gameId"] = pbState.gameId;
  doc["deviceId"] = ganamosConfig.deviceId;
  doc["scoreLeft"] = pbState.scoreLeft;
  doc["scoreRight"] = pbState.scoreRight;
  doc["winnerSide"] = (pbState.scoreLeft >= PB_WINNING_SCORE) ? "left" : "right";

  String body;
  serializeJson(doc, body);

  int httpCode = http.POST(body);

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> resp;
    if (deserializeJson(resp, payload) == DeserializationError::Ok && resp["success"]) {
      bool wagerSettled = resp["wagerSettled"] | false;
      if (wagerSettled) {
        // Find this device's user payout
        extern GanamosConfig ganamosConfig;
        JsonArray payoutsArr = resp["payouts"];
        for (JsonObject payout : payoutsArr) {
          // Match by payout's userId — we need to find our own userId
          // The host's userId is in the players array
          int amount = payout["amount"] | 0;
          String petName = payout["petName"].as<String>();
          // Check if this payout matches our device's pet name
          if (petName == ganamosConfig.petName) {
            pbState.wagerResult = amount;
            Serial.printf("🏓 Wager result for us: %d sats\n", amount);
            break;
          }
        }
      }
    }
  }

  http.end();
  client->stop();
  delete client;
}

static void assignSidesForPlayerCount() {
  // Assign players to sides based on count:
  // 2 players: 1v1 (left top vs right top)
  // 3 players: 1v2 (left top vs right top + right bottom)
  // 4 players: 2v2 (left top + left bottom vs right top + right bottom)

  switch (pbState.playerCount) {
    case 2:
      pbState.players[0].side = 0; pbState.players[0].position = 0; // Left top
      pbState.players[1].side = 1; pbState.players[1].position = 0; // Right top
      break;
    case 3:
      pbState.players[0].side = 0; pbState.players[0].position = 0; // Left top (solo)
      pbState.players[1].side = 1; pbState.players[1].position = 0; // Right top
      pbState.players[2].side = 1; pbState.players[2].position = 1; // Right bottom
      break;
    case 4:
      pbState.players[0].side = 0; pbState.players[0].position = 0; // Left top
      pbState.players[1].side = 0; pbState.players[1].position = 1; // Left bottom
      pbState.players[2].side = 1; pbState.players[2].position = 0; // Right top
      pbState.players[3].side = 1; pbState.players[3].position = 1; // Right bottom
      break;
  }
}
