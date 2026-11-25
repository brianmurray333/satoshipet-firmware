#include "game_rooms.h"
#include "config.h"
#include "economy.h"
#include "hold_press.h"
#include "sound.h"
#include "pickleball.h"
#include "pet_blob.h"
#include "espnow_comm.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

// Defined in satoshi_pet_heltec.ino
extern bool anyButtonDown();
extern void playButtonChirp();
extern void playMenuSelectTone();
extern int runListMenu(const char* title, const char* const* options, int count);
bool wifiReady();
void wifiRadioOff();

// ============================================================================
// Room data (what the server's room view carries; see game_room_view in the
// Ganamos migration 20261126000000_game_rooms.sql)
// ============================================================================

#define ROOM_MAX_PLAYERS 6
#define ROOM_LIST_MAX 10
#define NAME_LEN 13

struct RoomPlayer {
  uint8_t seat = 0;
  char name[NAME_LEN] = "";
  bool isYou = false;
  bool isHost = false;
  bool played = false;
  int32_t score = 0;
  int32_t payout = 0;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  bool hasMac = false;
};

struct Room {
  char id[40] = "";
  char status[10] = "";  // open, playing, finished, void
  uint8_t game = GAME_FLAPPY;
  uint16_t bet = 0;
  uint32_t pot = 0;
  RoomPlayer players[ROOM_MAX_PLAYERS];
  uint8_t count = 0;
  bool youAreIn = false;
  bool youAreHost = false;
  bool yourTurn = false;
  int32_t yourPayout = 0;
  bool stakeReturned = false;
  int8_t currentSeat = -1;
  int32_t secondsLeft = 0;
  uint32_t seed = 0;
  uint32_t generation = 0;
  char voidReason[20] = "";
  bool left = false;        // response to "leave": we're out
  uint16_t returned = 0;    // sats returned on leave
};

struct RoomListing {
  char id[40] = "";
  uint16_t bet = 0;
  char host[NAME_LEN] = "";
  uint8_t players = 0;
  uint8_t maxPlayers = 0;
  bool canAfford = true;
};

// The last request's error, for the screen ("Not enough sats", "Room is full")
static char lastError[40] = "";

static bool isStatus(const Room &r, const char *s) { return strcmp(r.status, s) == 0; }

static uint8_t gameFromName(const char *name) {
  if (strcmp(name, "pickleball") == 0) return GAME_PICKLEBALL;
  if (strcmp(name, "satcatcher") == 0) return GAME_SATCATCHER;
  return GAME_FLAPPY;
}

static const char *gameTitle(uint8_t game) {
  switch (game) {
    case GAME_PICKLEBALL: return "PICKLEBALL";
    case GAME_SATCATCHER: return "SAT CATCHER";
    default: return "FLAPPY PET";
  }
}

static const char *gameLabel(uint8_t game) {
  switch (game) {
    case GAME_PICKLEBALL: return "Pickleball";
    case GAME_SATCATCHER: return "Sat Catcher";
    default: return "Flappy Pet";
  }
}

static void copyName(char *dst, const char *src) {
  strncpy(dst, src ? src : "", NAME_LEN - 1);
  dst[NAME_LEN - 1] = 0;
}

static void copyStr(char *dst, size_t len, const char *src) {
  strncpy(dst, src ? src : "", len - 1);
  dst[len - 1] = 0;
}

static void parseRoom(JsonVariantConst r, Room &room) {
  room = Room{};
  copyStr(room.id, sizeof(room.id), r["roomId"] | "");
  copyStr(room.status, sizeof(room.status), r["status"] | "");
  room.game = gameFromName(r["game"] | "flappy");
  room.bet = r["bet"] | 0;
  room.pot = r["pot"] | 0;
  room.youAreIn = r["youAreIn"] | false;
  room.youAreHost = r["youAreHost"] | false;
  room.yourTurn = r["yourTurn"] | false;
  room.yourPayout = r["yourPayout"] | 0;
  room.stakeReturned = r["yourStakeReturned"] | false;
  room.currentSeat = r["currentSeat"].is<int>() ? (int8_t)(r["currentSeat"].as<int>()) : -1;
  room.secondsLeft = r["secondsLeft"] | 0;
  room.seed = r["seed"] | 0;
  room.generation = r["matchGeneration"] | 0;
  copyStr(room.voidReason, sizeof(room.voidReason), r["voidReason"] | "");
  room.left = r["left"] | false;
  room.returned = r["returned"] | 0;
  for (JsonVariantConst p : r["players"].as<JsonArrayConst>()) {
    if (room.count >= ROOM_MAX_PLAYERS) break;
    RoomPlayer &pl = room.players[room.count++];
    pl.seat = p["seat"] | 0;
    copyName(pl.name, p["petName"] | "Pet");
    pl.isYou = p["isYou"] | false;
    pl.isHost = p["isHost"] | false;
    pl.played = p["played"] | false;
    pl.score = p["score"] | 0;
    pl.payout = p["payout"] | 0;
    const char *mac = p["mac"] | "";
    pl.hasMac = mac[0] && espnowParseMac(mac, pl.mac);
  }
}

// ============================================================================
// HTTP (shared TLS connection; X-Device-Id comes from ganamosHttpBegin)
// ============================================================================

static HTTPClient &roomsHttp() {
  // Static: HTTPClient's destructor would close the shared keep-alive socket
  static HTTPClient http;
  return http;
}

// GET (body == nullptr) or POST JSON. Parses the reply (success or error) into
// out. Returns the HTTP code, negative on transport failure.
static int roomsRequest(const String &path, const JsonDocument *body, JsonDocument &out) {
  lastError[0] = 0;
  HTTPClient &http = roomsHttp();
  if (!ganamosHttpBegin(http, path)) {
    copyStr(lastError, sizeof(lastError), "No connection");
    return -1;
  }
  http.setTimeout(8000);
  http.addHeader("X-Device-Secret", gameSecret());
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    String payload;
    serializeJson(*body, payload);
    code = http.POST(payload);
  } else {
    code = http.GET();
  }
  if (code > 0) {
    String reply = http.getString();
    deserializeJson(out, reply);
  }
  http.end();
  if (code < 0) {
    ganamosHttpReset();
    copyStr(lastError, sizeof(lastError), "No connection");
  } else if (code != 200) {
    copyStr(lastError, sizeof(lastError), out["message"] | "Something went wrong");
  }
  Serial.printf("Rooms %s %s -> %d %s\n", body ? "POST" : "GET", path.c_str(), code, lastError);
  return code;
}

// POST an action to a room; on success the reply's room view replaces `room`.
static bool roomAction(Room &room, const char *action, JsonDocument *extra = nullptr) {
  JsonDocument body;
  if (extra) body = *extra;
  body["action"] = action;
  if (strcmp(action, "join") == 0) body["mac"] = espnowGetMacString();
  JsonDocument out;
  String path = String("/api/game/rooms/") + room.id;
  int code = roomsRequest(path, &body, out);
  if (out["room"].is<JsonObjectConst>()) {
    String id = room.id;
    parseRoom(out["room"], room);
    if (!room.id[0]) copyStr(room.id, sizeof(room.id), id.c_str());
  }
  return code == 200;
}

static bool roomPoll(Room &room) {
  JsonDocument out;
  int code = roomsRequest(String("/api/game/rooms/") + room.id, nullptr, out);
  if (code == 200 && out["room"].is<JsonObjectConst>()) {
    parseRoom(out["room"], room);
    return true;
  }
  return false;
}

// Open rooms for this game, the spendable balance, and any room we're in.
static bool roomsList(uint8_t game, RoomListing *rooms, int &n, int32_t &balance, Room &current, bool &hasCurrent) {
  n = 0;
  hasCurrent = false;
  JsonDocument out;
  int code = roomsRequest(String("/api/game/rooms?game=") + petGameApiName(game), nullptr, out);
  if (code != 200) return false;
  balance = out["balance"] | 0;
  for (JsonVariantConst r : out["rooms"].as<JsonArrayConst>()) {
    if (n >= ROOM_LIST_MAX) break;
    RoomListing &l = rooms[n++];
    copyStr(l.id, sizeof(l.id), r["roomId"] | "");
    l.bet = r["bet"] | 0;
    copyName(l.host, r["hostPetName"] | "Someone");
    l.players = r["players"] | 0;
    l.maxPlayers = r["maxPlayers"] | 0;
    l.canAfford = r["canAfford"] | true;
  }
  if (out["current"].is<JsonObjectConst>()) {
    parseRoom(out["current"], current);
    hasCurrent = current.id[0] != 0;
  }
  return true;
}

// ============================================================================
// Screens and sounds
// ============================================================================

static const ToneNote SND_TURN[]  = {{988, 90}, {0, 40}, {1319, 160}};
static const ToneNote SND_SCORE[] = {{1568, 50}};
static const ToneNote SND_WIN[]   = {{523, 100}, {659, 100}, {784, 100}, {1047, 150}, {784, 80}, {1047, 150}, {1319, 200}, {1568, 400}};
static const ToneNote SND_LOSE[]  = {{392, 200}, {0, 60}, {330, 220}, {0, 60}, {262, 300}};
#define NOTES(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static void serviceWait(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    soundUpdate();
    delay(10);
  }
}

// Wait up to ms, or until a press (released). Returns true if pressed.
static bool waitOrPress(unsigned long ms) {
  holdPressSwallow();
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    soundUpdate();
    if (holdPressUpdate(anyButtonDown()) != PRESS_NONE) return true;
    delay(15);
  }
  return false;
}

// Title with a rule under it, up to three lines, and a footer.
static void drawScreen(SSD1306Wire &d, const char *title, const char *l1, const char *l2,
                       const char *l3, const char *foot) {
  d.clear();
  d.setFont(ArialMT_Plain_10);
  d.setTextAlignment(TEXT_ALIGN_LEFT);
  d.drawString(0, 0, title);
  d.drawLine(0, 12, 127, 12);
  if (l1) d.drawString(0, 14, l1);
  if (l2) d.drawString(0, 26, l2);
  if (l3) d.drawString(0, 38, l3);
  if (foot) d.drawString(0, 50, foot);
}

static void showMessage(SSD1306Wire &d, const char *l1, const char *l2, unsigned long ms = 2200) {
  d.clear();
  d.setFont(ArialMT_Plain_10);
  d.setTextAlignment(TEXT_ALIGN_CENTER);
  if (l1) d.drawString(64, l2 ? 18 : 25, l1);
  if (l2) d.drawString(64, 34, l2);
  d.display();
  waitOrPress(ms);
}

static String satsText(uint32_t sats) {
  return String(sats) + (sats == 1 ? " sat" : " sats");
}

static String betText(uint16_t bet) {
  return bet == 0 ? String("Free") : satsText(bet);
}

// Why a room ended without a winner, in the pet's words.
static void showVoid(SSD1306Wire &d, const Room &room) {
  const char *why = "Game didn't finish";
  if (strcmp(room.voidReason, "expired") == 0) why = "Nobody started in time";
  else if (strcmp(room.voidReason, "host_left") == 0) why = "The host left";
  else if (strcmp(room.voidReason, "results_disagree") == 0) why = "Results didn't match";
  drawScreen(d, "GAME DIDN'T FINISH", why,
             room.bet > 0 ? "Everyone got their" : nullptr,
             room.bet > 0 ? "sats back." : nullptr, "Tap: OK");
  d.display();
  waitOrPress(6000);
}

// A money decision: a long hold (1.5 s) with the bar filling, so a habitual
// hold can't spend sats by accident. A tap (or 20 s) says no.
static bool confirmBet(SSD1306Wire &d, uint16_t bet, int32_t balance) {
  const unsigned long HOLD_MS = 1500;
  char l1[24], l3[24];
  snprintf(l1, sizeof(l1), "Win: get the pot");
  snprintf(l3, sizeof(l3), "You have %ld", (long)balance);
  String lose = "Lose: " + satsText(bet) + " gone";
  String title = "BET " + satsText(bet) + "?";
  // The hold that picked the room must end first: only a fresh press can bet
  while (anyButtonDown()) {
    esp_task_wdt_reset();
    soundUpdate();
    delay(10);
  }
  holdPressSwallow();
  unsigned long start = millis();
  unsigned long pressStart = 0;
  bool wasDown = false;
  while (millis() - start < 20000) {
    esp_task_wdt_reset();
    soundUpdate();
    bool down = anyButtonDown();
    unsigned long now = millis();
    if (down && !wasDown) pressStart = now;
    if (!down && wasDown) {
      if (now - pressStart >= 40) return false;  // a tap: no
    }
    wasDown = down;
    drawScreen(d, title.c_str(), l1, lose.c_str(), l3, "Hold to bet  Tap: no");
    if (down) {
      unsigned long held = now - pressStart;
      if (held >= HOLD_MS) {
        drawHoldBar(d, HOLD_SELECT_MS, false);
        d.display();
        holdPressSwallow();
        playMenuSelectTone();
        return true;
      }
      drawHoldBar(d, held * HOLD_SELECT_MS / HOLD_MS, false);
      start = now;  // holding counts as attention
    }
    d.display();
    delay(20);
  }
  return false;
}

// ============================================================================
// Choosing: start or join
// ============================================================================

// Bet for a new room. Returns -1 for Back (or nothing picked in 30 s).
static int pickBet(SSD1306Wire &d, int32_t balance) {
  static const uint16_t BETS[] = {0, 10, 50, 100, 500};
  const int N = 5;  // plus Back
  int idx = 0;
  unsigned long lastInput = millis();
  holdPressSwallow();
  while (millis() - lastInput < 30000) {
    esp_task_wdt_reset();
    soundUpdate();
    d.clear();
    d.setFont(ArialMT_Plain_10);
    d.setTextAlignment(TEXT_ALIGN_CENTER);
    d.drawString(64, 0, "BET PER PLAYER");
    d.setFont(ArialMT_Plain_16);
    String label = idx == N ? String("Back") : (BETS[idx] == 0 ? String("Free") : satsText(BETS[idx]));
    d.drawString(64, 15, String("< ") + label + " >");
    d.setFont(ArialMT_Plain_10);
    if (idx < N && BETS[idx] > balance) {
      d.drawString(64, 36, "Not enough sats");
    } else if (idx < N && BETS[idx] > 0) {
      d.drawString(64, 36, "Winner takes the pot");
    }
    d.drawString(64, 48, "You have " + satsText(balance > 0 ? balance : 0));
    PressEvent ev = holdPressUpdate(anyButtonDown());
    if (holdPressActive()) lastInput = millis();
    holdPressDrawBar(d, false);
    d.display();
    if (ev == PRESS_TAP) {
      lastInput = millis();
      idx = (idx + 1) % (N + 1);
      playButtonChirp();
    } else if (ev == PRESS_HOLD) {
      if (idx == N) return -1;
      if (BETS[idx] > balance) {
        showMessage(d, "Not enough sats", "Pick a smaller bet", 1600);
        lastInput = millis();
        continue;
      }
      playMenuSelectTone();
      return BETS[idx];
    }
    delay(25);
  }
  return -1;
}

// Pick one of the open rooms. Returns its index, or -1 for Back.
static int browseRooms(SSD1306Wire &d, uint8_t game, const RoomListing *rooms, int n) {
  int idx = 0;
  unsigned long lastInput = millis();
  holdPressSwallow();
  while (millis() - lastInput < 30000) {
    esp_task_wdt_reset();
    soundUpdate();
    char title[24];
    if (idx < n) {
      snprintf(title, sizeof(title), "OPEN ROOMS  %d of %d", idx + 1, n);
      const RoomListing &r = rooms[idx];
      String who = String(r.host) + "'s room";
      String bet = r.bet == 0 ? String("Free") : satsText(r.bet) + " each";
      bet += " - " + String(r.players) + " in";
      drawScreen(d, title, who.c_str(), gameLabel(game), bet.c_str(),
                 r.canAfford ? "Tap: next  Hold: look" : "Not enough sats");
    } else {
      drawScreen(d, "OPEN ROOMS", "Back", nullptr, nullptr, "Hold: back");
    }
    PressEvent ev = holdPressUpdate(anyButtonDown());
    if (holdPressActive()) lastInput = millis();
    holdPressDrawBar(d, false);
    d.display();
    if (ev == PRESS_TAP) {
      lastInput = millis();
      idx = (idx + 1) % (n + 1);
      playButtonChirp();
    } else if (ev == PRESS_HOLD) {
      if (idx == n) return -1;
      if (!rooms[idx].canAfford) {
        showMessage(d, "Not enough sats", "for this room", 1600);
        lastInput = millis();
        continue;
      }
      playMenuSelectTone();
      return idx;
    }
    delay(25);
  }
  return -1;
}

// ============================================================================
// Lobby: wait for friends; the host starts
// ============================================================================

enum LobbyResult { LOBBY_STARTED, LOBBY_LEFT, LOBBY_ENDED };

static LobbyResult runLobby(SSD1306Wire &d, Room &room) {
  const unsigned long POLL_MS = 2500;
  unsigned long lastPoll = millis();
  int choice = 0;  // host: 0 = Start, 1 = Leave
  holdPressSwallow();
  while (true) {
    esp_task_wdt_reset();
    soundUpdate();
    unsigned long now = millis();

    if (now - lastPoll >= POLL_MS) {
      roomPoll(room);
      lastPoll = millis();
    }
    if (isStatus(room, "playing")) return LOBBY_STARTED;
    if (isStatus(room, "void") || isStatus(room, "finished")) {
      showVoid(d, room);
      return LOBBY_ENDED;
    }

    bool canStart = room.youAreHost && room.count >= 2;
    if (!canStart) choice = 1;

    // Screen
    char title[28];
    snprintf(title, sizeof(title), "%s  %s", gameTitle(room.game),
             room.bet == 0 ? "FREE" : (String(room.bet) + " ea").c_str());
    String names[3];
    for (int i = 0; i < 3; i++) names[i] = "";
    int shown = 0;
    for (int i = 0; i < room.count && shown < 2; i++) {
      names[shown++] = String(room.players[i].name) + (room.players[i].isYou ? " (you)" : "");
    }
    if (room.count > 2) names[1] += "  +" + String(room.count - 2);
    String status;
    if (room.youAreHost) {
      status = room.count < 2 ? String("Waiting for friends") : (room.bet ? "Pot: " + satsText(room.pot) : String("Ready!"));
    } else {
      status = room.bet ? "Pot: " + satsText(room.pot) : String("Waiting for host");
    }
    int secs = room.secondsLeft > 0 ? room.secondsLeft : 0;
    String foot;
    if (room.youAreHost) {
      foot = canStart ? (choice == 0 ? String("> Start    Leave") : String("  Start  > Leave"))
                      : String("Hold: leave ") + String(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + String(secs % 60);
    } else {
      foot = room.bet ? String("Hold: leave, sats back") : String("Hold: leave");
    }
    drawScreen(d, title, names[0].c_str(), names[1].length() ? names[1].c_str() : nullptr, status.c_str(), foot.c_str());

    PressEvent ev = holdPressUpdate(anyButtonDown());
    holdPressDrawBar(d, false);
    d.display();

    if (ev == PRESS_TAP && canStart) {
      choice = 1 - choice;
      playButtonChirp();
    } else if (ev == PRESS_HOLD) {
      if (canStart && choice == 0) {
        drawScreen(d, title, "Starting...", nullptr, nullptr, nullptr);
        d.display();
        if (roomAction(room, "start") && isStatus(room, "playing")) {
          playMenuSelectTone();
          return LOBBY_STARTED;
        }
        showMessage(d, "Couldn't start", lastError, 1800);
      } else {
        drawScreen(d, title, "Leaving...", nullptr, nullptr, nullptr);
        d.display();
        uint16_t bet = room.bet;
        if (roomAction(room, "leave")) {
          if (bet > 0) {
            showMessage(d, "You left the room", (satsText(bet) + " back").c_str(), 1800);
          } else {
            showMessage(d, "You left the room", nullptr, 1400);
          }
          return LOBBY_LEFT;
        }
        showMessage(d, "Couldn't leave", lastError, 1800);
      }
      lastPoll = 0;  // refresh right away
    }
    delay(25);
  }
}

// ============================================================================
// Results
// ============================================================================

static int showTurnResults(SSD1306Wire &d, const Room &room) {
  // Rank by score (seat order breaks ties for display)
  int order[ROOM_MAX_PLAYERS];
  for (int i = 0; i < room.count; i++) order[i] = i;
  for (int i = 0; i < room.count; i++)
    for (int j = i + 1; j < room.count; j++)
      if (room.players[order[j]].score > room.players[order[i]].score) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  String lines[3];
  for (int k = 0; k < 3; k++) {
    if (k >= room.count) break;
    const RoomPlayer &p = room.players[order[k]];
    char buf[28];
    snprintf(buf, sizeof(buf), "%d %-10s %ld", k + 1, p.isYou ? "You" : p.name, (long)p.score);
    lines[k] = buf;
  }
  bool won = room.yourPayout > 0 || (room.bet == 0 && room.count > 0 && room.players[order[0]].isYou);
  String foot;
  if (room.yourPayout > 0) foot = "+" + satsText(room.yourPayout) + "!";
  else if (room.bet > 0) foot = String(room.players[order[0]].isYou ? "You" : room.players[order[0]].name) + " won the pot";
  else foot = won ? String("You win!") : String("Good game!");
  drawScreen(d, "RESULTS", lines[0].c_str(), lines[1].length() ? lines[1].c_str() : nullptr,
             lines[2].length() ? lines[2].c_str() : nullptr, foot.c_str());
  d.display();
  if (won) soundPlayBlocking(SND_WIN, NOTES(SND_WIN), false);
  else soundPlayBlocking(SND_LOSE, NOTES(SND_LOSE), false);
  waitOrPress(8000);
  return won ? 30 : 15;
}

static void showPickleballResult(SSD1306Wire &d, const Room &room, const PbMatchOutcome &m) {
  char score[16];
  snprintf(score, sizeof(score), "%u - %u", (unsigned)m.scoreLeft, (unsigned)m.scoreRight);
  String line;
  if (room.yourPayout > 0) line = "+" + satsText(room.yourPayout) + "!";
  else if (room.bet > 0) line = "Lost " + satsText(room.bet);
  else line = m.iWon ? "You win!" : "Good game!";
  drawScreen(d, "PICKLEBALL", score, m.iWon ? "You win!" : "You lose", line.c_str(), "Tap: OK");
  d.display();
  waitOrPress(8000);
}

// ============================================================================
// Playing
// ============================================================================

static int playTurns(SSD1306Wire &d, Room &room) {
  const unsigned long POLL_MS = 2000;
  unsigned long lastPoll = 0;
  int lastPlayed = -1;
  bool turnAnnounced = false;
  holdPressSwallow();
  while (true) {
    esp_task_wdt_reset();
    soundUpdate();
    if (millis() - lastPoll >= POLL_MS) {
      roomPoll(room);
      lastPoll = millis();
    }
    if (isStatus(room, "finished")) return showTurnResults(d, room);
    if (isStatus(room, "void")) { showVoid(d, room); return 0; }

    int played = 0, best = -1, bestIdx = -1, turnIdx = -1;
    for (int i = 0; i < room.count; i++) {
      const RoomPlayer &p = room.players[i];
      if (p.played) {
        played++;
        if (p.score > best) { best = p.score; bestIdx = i; }
      }
      if (p.seat == room.currentSeat) turnIdx = i;
    }
    if (lastPlayed >= 0 && played > lastPlayed) soundPlay(SND_SCORE, NOTES(SND_SCORE), false);
    lastPlayed = played;

    if (room.yourTurn) {
      if (!turnAnnounced) {
        soundPlay(SND_TURN, NOTES(SND_TURN), false);
        turnAnnounced = true;
      }
      String beat = best >= 0 ? "Beat " + String(best) : String("Set the score");
      String pot = room.bet ? "Pot: " + satsText(room.pot) : String("Free game");
      String foot = "Hold: play (" + String(room.secondsLeft) + "s)";
      d.clear();
      d.setFont(ArialMT_Plain_10);
      d.setTextAlignment(TEXT_ALIGN_LEFT);
      d.drawString(0, 0, "YOUR TURN!");
      d.drawLine(0, 12, 127, 12);
      d.setFont(ArialMT_Plain_16);
      d.drawString(0, 14, beat);
      d.setFont(ArialMT_Plain_10);
      d.drawString(0, 34, pot);
      d.drawString(0, 50, foot);
      PressEvent ev = holdPressUpdate(anyButtonDown());
      holdPressDrawBar(d, false);
      d.display();
      if (ev == PRESS_HOLD) {
        playMenuSelectTone();
        if (!roomAction(room, "turn_begin")) {
          showMessage(d, "Couldn't start turn", lastError, 1800);
          lastPoll = 0;
          continue;
        }
        uint32_t seed = room.seed;
        int score = room.game == GAME_SATCATCHER ? playSatCatcherRoomTurn(d, seed) : playFlappyRoomTurn(d, seed);
        // Send the score; retry through a dropped connection
        bool sent = false;
        for (int attempt = 0; attempt < 4 && !sent; attempt++) {
          drawScreen(d, "YOUR SCORE", String(score).c_str(), "Sending...", nullptr, nullptr);
          d.display();
          JsonDocument extra;
          extra["score"] = score;
          sent = roomAction(room, "turn_submit", &extra);
          if (!sent && strcmp(lastError, "No connection") != 0) break;  // the server said no
          if (!sent) serviceWait(1500);
        }
        if (!sent) showMessage(d, "Score not sent", lastError, 2200);
        else showMessage(d, "Score sent!", (String(score) + " - nice run").c_str(), 1400);
        turnAnnounced = false;
        lastPoll = 0;
        holdPressSwallow();
      }
    } else {
      turnAnnounced = false;
      String who = turnIdx >= 0 ? String(room.players[turnIdx].name) : String("Someone");
      who.toUpperCase();
      String title = who + " IS PLAYING";
      String turnLine = "Turn " + String(played + 1) + " of " + String(room.count);
      String bestLine = bestIdx >= 0
          ? "Best: " + String(room.players[bestIdx].isYou ? "You" : room.players[bestIdx].name) + " " + String(best)
          : String("No scores yet");
      String pot = room.bet ? "Pot: " + satsText(room.pot) : String("Free game");
      drawScreen(d, title.c_str(), turnLine.c_str(), bestLine.c_str(), pot.c_str(), nullptr);
      d.display();
      holdPressUpdate(anyButtonDown());  // presses while waiting do nothing
    }
    delay(25);
  }
}

static int playPickleball(SSD1306Wire &d, Room &room) {
  // Roster in seat order; sides by seat: 0 left-top, 1 right-top, 2 right-bottom, 3 left-bottom
  static const uint8_t SIDE[4] = {0, 1, 1, 0};
  static const uint8_t POS[4] = {0, 0, 1, 1};
  PbRosterEntry roster[MAX_GAME_PLAYERS];
  uint8_t n = 0;
  int myIndex = -1;
  bool macsOk = true;
  for (int i = 0; i < room.count && n < MAX_GAME_PLAYERS; i++) {
    const RoomPlayer &p = room.players[i];
    if (p.seat > 3) { macsOk = false; break; }
    PbRosterEntry &e = roster[n];
    memcpy(e.mac, p.mac, 6);
    e.initial = p.name[0];
    e.side = SIDE[p.seat];
    e.position = POS[p.seat];
    if (!p.hasMac) macsOk = false;
    if (p.isYou) myIndex = n;
    n++;
  }
  if (!macsOk || myIndex < 0 || n < 2 || !room.players[0].isHost) {
    roomAction(room, "abandon");
    showVoid(d, room);
    return 0;
  }

  PbMatchOutcome m;
  int bonus = handlePickleballRoomMatch(d, roster, n, (uint8_t)myIndex, room.generation, m);
  if (!m.finished) {
    roomAction(room, "abandon");  // every bet goes back
    if (!isStatus(room, "void")) roomPoll(room);
    showVoid(d, room);
    return 0;
  }

  // Every pet reports what it saw; the server pays once they all agree
  JsonDocument extra;
  extra["winner"] = m.winnerSide == 0 ? "left" : "right";
  extra["left"] = m.scoreLeft;
  extra["right"] = m.scoreRight;
  bool sent = false;
  for (int attempt = 0; attempt < 5 && !sent; attempt++) {
    drawScreen(d, "PICKLEBALL", "Checking result...", nullptr, nullptr, nullptr);
    d.display();
    sent = roomAction(room, "report", &extra);
    if (!sent && strcmp(lastError, "No connection") != 0) break;
    if (!sent) serviceWait(1500);
  }
  // Wait for the other pets' reports (the server gives them 2 minutes)
  unsigned long waitStart = millis();
  while (isStatus(room, "playing") && millis() - waitStart < 150000UL) {
    drawScreen(d, "PICKLEBALL", "Checking result...", "Waiting for the", "other pets", nullptr);
    d.display();
    serviceWait(2500);
    roomPoll(room);
  }
  if (isStatus(room, "void")) {
    showVoid(d, room);
    return 0;
  }
  showPickleballResult(d, room, m);
  return bonus;
}

// ============================================================================
// Entry point
// ============================================================================

int runFriendsGame(SSD1306Wire &d, uint8_t game) {
  bool startedWifi = false;
  if (!wifiReady()) {
    showMessage(d, "Connecting WiFi...", nullptr, 10);
    if (!wifiEnsureConnected(15000)) {
      showMessage(d, "WiFi failed", "Try again later");
      return 0;
    }
    startedWifi = true;
  }
  struct WifiRestore {
    bool off;
    ~WifiRestore() { if (off) wifiRadioOff(); }
  } wifiRestore{startedWifi};

  // The game secret is claimed through the config request
  if (!hasGameSecret()) {
    showMessage(d, "Getting ready...", nullptr, 10);
    bool ok = fetchGanamosConfig();
    if (!hasGameSecret()) {
      if (ok) showMessage(d, "Pair your pet again", "to play with friends", 3000);
      else showMessage(d, "Can't reach Ganamos", "Try again later");
      return 0;
    }
  }

  int bonus = 0;
  for (int round = 0; round < 20; round++) {
    showMessage(d, "Looking for rooms...", nullptr, 10);
    static RoomListing rooms[ROOM_LIST_MAX];
    int n = 0;
    int32_t balance = 0;
    Room room;
    bool inRoom = false;
    if (!roomsList(game, rooms, n, balance, room, inRoom)) {
      showMessage(d, "Can't reach Ganamos", lastError[0] ? lastError : "Try again later");
      return bonus;
    }

    if (!inRoom) {
      char joinLabel[24];
      snprintf(joinLabel, sizeof(joinLabel), n > 0 ? "Join a room (%d)" : "Join a room", n);
      const char *options[] = {"Start a room", joinLabel, "Back"};
      int pick = runListMenu("WITH FRIENDS", options, 3);
      if (pick < 0) return bonus;

      if (pick == 0) {
        int bet = pickBet(d, balance);
        if (bet < 0) continue;
        showMessage(d, "Opening room...", nullptr, 10);
        JsonDocument body;
        body["game"] = petGameApiName(game);
        body["bet"] = bet;
        body["mac"] = espnowGetMacString();
        JsonDocument out;
        if (roomsRequest("/api/game/rooms", &body, out) != 200) {
          showMessage(d, "Couldn't open room", lastError);
          continue;
        }
        parseRoom(out["room"], room);
      } else {
        if (n == 0) {
          showMessage(d, "No open rooms", "Start one!", 1800);
          continue;
        }
        int idx = browseRooms(d, game, rooms, n);
        if (idx < 0) continue;
        uint16_t bet = rooms[idx].bet;
        if (bet > 0 && !confirmBet(d, bet, balance)) continue;
        showMessage(d, "Joining...", nullptr, 10);
        copyStr(room.id, sizeof(room.id), rooms[idx].id);
        if (!roomAction(room, "join") || !room.youAreIn) {
          showMessage(d, "Couldn't join", lastError);
          continue;
        }
      }
    } else {
      // Back in a room we were already in (after a reboot or a menu exit)
      showMessage(d, "Back to your room", gameLabel(room.game), 1200);
    }

    if (isStatus(room, "open")) {
      LobbyResult lr = runLobby(d, room);
      if (lr == LOBBY_LEFT) continue;
      if (lr == LOBBY_ENDED) return bonus;
    }
    if (isStatus(room, "playing")) {
      bonus += room.game == GAME_PICKLEBALL ? playPickleball(d, room) : playTurns(d, room);
    } else if (isStatus(room, "finished")) {
      if (room.game != GAME_PICKLEBALL) bonus += showTurnResults(d, room);
    } else if (isStatus(room, "void")) {
      showVoid(d, room);
    }
    return bonus;
  }
  return bonus;
}
