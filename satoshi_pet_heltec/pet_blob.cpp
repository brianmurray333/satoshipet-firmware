#ifndef BUTTON_PIN_PRG
#define BUTTON_PIN_PRG 0      // Heltec KEY button (PRG) on GPIO0
#endif
#ifndef BUTTON_PIN_EXTERNAL
#define BUTTON_PIN_EXTERNAL 2 // External button (active LOW with pull-up)
#endif

#include <Wire.h>
#include <esp_task_wdt.h>
#include "pet_blob.h"
#include "config.h"
#include <WiFi.h>
#include "economy.h"
extern void wakeForNotification();
extern bool lastSyncSucceeded;
#include "pet_sprites_simple.h"
#include "display_assets.h"
#include "food_bitmaps.h"
#include "sound.h"
#include "hold_press.h"

// Forward declarations
void renderJobsList(SSD1306Wire &display, int selectedJob, int scrollOffset);
void renderJobDetail(SSD1306Wire &display, int jobIndex);
void handleJobsMenu(SSD1306Wire &display);

extern GanamosConfig ganamosConfig;

PetKind petKindFromString(const String& petType) {
  if (petType.equalsIgnoreCase("dog")) return PET_DOG;
  if (petType.equalsIgnoreCase("squirrel")) return PET_SQUIRREL;
  if (petType.equalsIgnoreCase("turtle")) return PET_TURTLE;
  if (petType.equalsIgnoreCase("rabbit") || petType.equalsIgnoreCase("bunny")) return PET_BUNNY;
  if (petType.equalsIgnoreCase("owl")) return PET_OWL;
  return PET_CAT;  // "cat" and unknown types
}

// The pet type string only changes on a config fetch, so the enum is cached and
// recomputed when the string differs (one String compare per call).
static PetKind currentPetKind() {
  static String cachedType;
  static PetKind cachedKind = PET_CAT;
  static bool cachedValid = false;
  if (!cachedValid || cachedType != ganamosConfig.petType) {
    cachedType = ganamosConfig.petType;
    cachedKind = petKindFromString(cachedType);
    cachedValid = true;
  }
  return cachedKind;
}

// Horizontal position of the pet sprite on the home screen. Sprite width and
// height come from the per-pet animation table (getPetSpriteAnim).
static int petSpriteHomeX(PetKind kind) {
  return (kind == PET_BUNNY || kind == PET_OWL) ? 64 : 54;
}

bool showCelebration = false;
unsigned long celebrationStart = 0;
const unsigned long CELEBRATION_DURATION = 6000; // 6 seconds - longer celebration for earning sats!
int celebrationFrame = 0;
int satsEarned = 0;
String celebrationMessage = "";

// New job notification state
bool showNewJobNotification = false;
unsigned long newJobNotificationStart = 0;
const unsigned long NEW_JOB_NOTIFICATION_DURATION = 5000; // 5 seconds
String newJobTitle = "";
int newJobReward = 0;

// Fix rejection notification state
bool showRejection = false;
unsigned long rejectionStart = 0;
const unsigned long REJECTION_DURATION = 5000; // 5 seconds
String rejectionMessage = "";

// Animation state tracking
PetAnimationState currentAnimState = ANIM_DEFAULT;
int eatAnimLoopsCompleted = 0;
const int EAT_ANIM_TOTAL_LOOPS = 5;  // Number of eat animation cycles

// Last animation step drawn by each timed overlay; identical frames are not
// re-sent to the OLED. Reset by the matching trigger function.
static long lastCelebrationStepDrawn = -1;
static long lastNewJobStepDrawn = -1;
static long lastRejectionStepDrawn = -1;

struct FoodOption {
  const char* name;
  int coinCost;
  int fullnessPercent;   // Percent of hunger bar refilled (0-100)
  const unsigned char* bitmap;  // Pointer to food bitmap (all are 48x48)
};

// Cat food options (const without static allows linker to place in flash)
const FoodOption CAT_FOOD_OPTIONS[] PROGMEM = {
  {"Mouse", 100, 10, mouse_bitmap},
  {"Milk",  250, 25, milk_bitmap},
  {"Tuna", 1000, 100, tuna_bitmap}
};

// Dog food options
const FoodOption DOG_FOOD_OPTIONS[] PROGMEM = {
  {"Bone",   100, 10, bone_bitmap},
  {"Kibble", 250, 25, kibble_bitmap},
  {"Steak", 1000, 100, steak_bitmap}
};

// Rabbit food options
const FoodOption RABBIT_FOOD_OPTIONS[] PROGMEM = {
  {"Lettuce", 100, 10, lettuce_bitmap},
  {"Celery",  250, 25, celery_bitmap},
  {"Carrot", 1000, 100, carrot_bitmap}
};

// Squirrel food options
const FoodOption SQUIRREL_FOOD_OPTIONS[] PROGMEM = {
  {"Lettuce", 100, 10, lettuce_bitmap},
  {"Peanut",  250, 25, peanut_bitmap},
  {"Acorn",  1000, 100, acorn_bitmap}
};

// Turtle food options
const FoodOption TURTLE_FOOD_OPTIONS[] PROGMEM = {
  {"Lettuce", 100, 10, lettuce_bitmap},
  {"Celery",  250, 25, celery_bitmap},
  {"Snail",  1000, 100, snail_bitmap}
};

static const int FOOD_OPTION_COUNT = 3; // All pets have 3 food options

static int clampFoodIndex(int index) {
  if (index < 0) return 0;
  if (index >= FOOD_OPTION_COUNT) return FOOD_OPTION_COUNT - 1;
  return index;
}

// Get the appropriate food options array based on pet type.
// "bunny" and "rabbit" both get rabbit food (they share the bunny sprites).
// There is no owl-specific food art, so the owl uses the cat menu
// (mouse/milk/tuna); unknown pet types also fall back to cat food.
static const FoodOption* getPetFoodOptions() {
  switch (currentPetKind()) {
    case PET_DOG: return DOG_FOOD_OPTIONS;
    case PET_BUNNY: return RABBIT_FOOD_OPTIONS;
    case PET_SQUIRREL: return SQUIRREL_FOOD_OPTIONS;
    case PET_TURTLE: return TURTLE_FOOD_OPTIONS;
    case PET_OWL:
    case PET_CAT:
    default: return CAT_FOOD_OPTIONS;
  }
}

static const FoodOption& getFoodOption(int index) {
  const FoodOption* options = getPetFoodOptions();
  return options[clampFoodIndex(index)];
}

int getFoodCostByIndex(int foodIndex) {
  return getFoodOption(foodIndex).coinCost;
}

int getFoodFullnessPercent(int foodIndex) {
  return getFoodOption(foodIndex).fullnessPercent;
}

const char* getFoodNameByIndex(int foodIndex) {
  return getFoodOption(foodIndex).name;
}

int getFoodOptionCount() {
  return FOOD_OPTION_COUNT;
}

void triggerCelebration(int earnedSats, String message = "") {
  showCelebration = true;
  celebrationStart = millis();
  celebrationFrame = 0;
  lastCelebrationStepDrawn = -1;
  satsEarned = earnedSats;
  celebrationMessage = message;
  Serial.println("🎉 CELEBRATION! Earned " + String(earnedSats) + " sats!");
  if (message.length() > 0) {
    Serial.println("Message: " + message);
  }
  
  wakeForNotification();

  // Play the celebration sound!
  playSatsEarnedSound();
}

void triggerNewJobNotification(String title, int reward) {
  showNewJobNotification = true;
  newJobNotificationStart = millis();
  lastNewJobStepDrawn = -1;
  newJobTitle = title;
  newJobReward = reward;
  
  Serial.println("📋 NEW JOB! " + title + " (" + String(reward) + " sats)");
  
  wakeForNotification();

  // Play the new job chirp
  extern void playNewJobChirp();
  playNewJobChirp();
}

void renderNewJobNotification(SSD1306Wire &display) {
  unsigned long now = millis();
  
  // Blink LED
  if ((now - newJobNotificationStart) % 600 < 300) {
    digitalWrite(RGB_LED, HIGH);
  } else {
    digitalWrite(RGB_LED, LOW);
  }

  // Only the coin animation changes (every 150 ms); skip identical frames.
  long step = (long)((now - newJobNotificationStart) / 150);
  if (step == lastNewJobStepDrawn) return;
  lastNewJobStepDrawn = step;
  
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // "NEW JOB!" header
  display.drawString(64, 2, "NEW JOB!");
  
  // Reward
  display.setFont(ArialMT_Plain_10);
  extern String formatSatsShort(int sats);
  display.drawString(64, 22, formatSatsShort(newJobReward) + " sats");
  
  // Job title (may already be truncated from server)
  String title = newJobTitle;
  if (title.length() > 20) {
    title = title.substring(0, 18) + "..";
  }
  display.drawString(64, 38, title);
  
  // Spinning coins animation
  int frame = ((now - newJobNotificationStart) / 150) % 4;
  display.drawXbm(20, 52, 8, 8, bitcoin_spin_frames[frame]);
  display.drawXbm(100, 52, 8, 8, bitcoin_spin_frames[(frame + 2) % 4]);
  
  display.display();
}

void triggerRejection(String message) {
  showRejection = true;
  rejectionStart = millis();
  lastRejectionStepDrawn = -1;
  rejectionMessage = message;
  Serial.println("❌ FIX REJECTED! " + message);
  
  wakeForNotification();

  // Play the rejection sound
  playFixRejectedSound();
}

void renderRejection(SSD1306Wire &display) {
  unsigned long elapsed = millis() - rejectionStart;
  
  if (elapsed > REJECTION_DURATION) {
    showRejection = false;
    return;
  }

  // The X marks blink every 150 ms; skip identical frames.
  long step = (long)(elapsed / 150);
  if (step == lastRejectionStepDrawn) return;
  lastRejectionStepDrawn = step;
  
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Header: "Fix rejected"
  display.drawString(64, 2, "Fix rejected");
  
  display.setFont(ArialMT_Plain_10);
  
  // Context message: "for [post title]"
  String contextMsg;
  if (ganamosConfig.rejectionPostTitle.length() > 0) {
    contextMsg = "for " + ganamosConfig.rejectionPostTitle;
  } else if (rejectionMessage.length() > 0) {
    contextMsg = rejectionMessage;
  } else {
    contextMsg = "Try again!";
  }
  
  // Truncate if too long
  if (contextMsg.length() > 21) {
    contextMsg = contextMsg.substring(0, 18) + "...";
  }
  display.drawString(64, 22, contextMsg);
  
  // Show X marks (blinking)
  int frame = (elapsed / 150) % 2;
  if (frame == 0) {
    display.drawString(20, 40, "X");
    display.drawString(50, 40, "X");
    display.drawString(80, 40, "X");
    display.drawString(108, 40, "X");
  }
  
  // Encouragement message
  display.drawString(64, 54, "Keep trying!");
  
  display.display();
}

void turnOffLED() {
  digitalWrite(RGB_LED, LOW);
}

void renderScreensaver(SSD1306Wire &display, int satoshis) {
  static unsigned long lastScreensaverFrame = 0;
  static int screensaverFrame = 0;
  unsigned long now = millis();
  
  // Update screensaver animation frame every 500ms
  if (now - lastScreensaverFrame > 500) {
    screensaverFrame++;
    lastScreensaverFrame = now;
  }
  
  // Note: During animated screensaver, display stays at normal brightness
  // Display OFF happens later (after 1 more minute) for power saving

  // The frame only advances every 500 ms; don't resend an identical frame.
  // (Re-entering the screensaver after a pause always advances the frame.)
  static int lastDrawnScreensaverFrame = -1;
  if (screensaverFrame == lastDrawnScreensaverFrame) return;
  lastDrawnScreensaverFrame = screensaverFrame;
  
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Draw pet sprite centered and moved up, using the pet's real dimensions
  PetKind kind = currentPetKind();
  const PetSpriteAnim& anim = getPetSpriteAnim(kind, ANIM_DEFAULT);
  int spriteFrame = (screensaverFrame / 2) % 3;
  display.drawXbm(34, 6, anim.width, anim.height, getPetSprite(kind, ANIM_DEFAULT, spriteFrame));
  
  // Draw floating bitcoin coins at different positions
  int coinFrame = screensaverFrame % 4;
  
  // Coin 1 - top left, floating up and down
  int y1 = 10 + (screensaverFrame % 20) - 10;
  display.drawXbm(10, y1, 8, 8, bitcoin_spin_frames[coinFrame]);
  
  // Coin 2 - top right, different phase
  int y2 = 15 + ((screensaverFrame + 10) % 20) - 10;
  display.drawXbm(110, y2, 8, 8, bitcoin_spin_frames[(coinFrame + 1) % 4]);
  
  // Coin 3 - bottom left
  int y3 = 45 + ((screensaverFrame + 5) % 20) - 10;
  display.drawXbm(15, y3, 8, 8, bitcoin_spin_frames[(coinFrame + 2) % 4]);
  
  // Coin 4 - bottom right
  int y4 = 50 + ((screensaverFrame + 15) % 20) - 10;
  display.drawXbm(105, y4, 8, 8, bitcoin_spin_frames[(coinFrame + 3) % 4]);
  
  display.display();
}

void renderCelebration(SSD1306Wire &display) {
  esp_task_wdt_reset(); // Feed watchdog during celebration animation
  unsigned long now = millis();
  
  // Blink LED with nice rhythm - on/off every 250ms
  if ((now - celebrationStart) % 500 < 250) {
    digitalWrite(RGB_LED, HIGH);
  } else {
    digitalWrite(RGB_LED, LOW);
  }
  
  // Animation frame advances every 150ms; the coins change every 2 frames.
  celebrationFrame = (int)((now - celebrationStart) / 150);
  long step = celebrationFrame / 2;
  if (step == lastCelebrationStepDrawn) return;  // identical frame already shown
  lastCelebrationStepDrawn = step;
  
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Determine if this is a fix reward or internal transfer
  bool isFixReward = (ganamosConfig.lastMessageType == "fix");
  bool isTransfer = (ganamosConfig.lastMessageType == "transfer");
  
  // Format sats amount with k suffix
  String satsText;
  if (satsEarned >= 1000) {
    satsText = String(satsEarned / 1000) + "k";
  } else {
    satsText = String(satsEarned);
  }
  
  // Line 1: Amount and type header
  display.setFont(ArialMT_Plain_16);
  if (isTransfer) {
    display.drawString(64, 2, satsText + " Sats Received");
  } else {
    display.drawString(64, 2, satsText + " Sats Earned");
  }
  
  // Line 2: Context message
  display.setFont(ArialMT_Plain_10);
  String contextMsg;
  
  if (isFixReward && ganamosConfig.lastPostTitle.length() > 0) {
    // Fix reward: show "fix: [post title]"
    contextMsg = "fix: " + ganamosConfig.lastPostTitle;
  } else if (isTransfer && ganamosConfig.lastSenderName.length() > 0) {
    // Internal transfer: show "sent from [sender name]"
    contextMsg = "sent from " + ganamosConfig.lastSenderName;
  } else if (celebrationMessage.length() > 0 && celebrationMessage != "null") {
    // Fallback to celebration message
    contextMsg = celebrationMessage;
  } else {
    // Default fallback
    contextMsg = ganamosConfig.petName + " is happy!";
  }
  
  // Truncate long messages to fit on screen
  if (contextMsg.length() > 21) {
    contextMsg = contextMsg.substring(0, 18) + "...";
  }
  display.drawString(64, 22, contextMsg);
  
  // Spinning Bitcoin logos
  int frame = (celebrationFrame / 2) % 4;
  display.drawXbm(20, 36, 8, 8, bitcoin_spin_frames[frame]);
  display.drawXbm(50, 36, 8, 8, bitcoin_spin_frames[(frame + 1) % 4]);
  display.drawXbm(80, 36, 8, 8, bitcoin_spin_frames[(frame + 2) % 4]);
  display.drawXbm(110, 36, 8, 8, bitcoin_spin_frames[(frame + 3) % 4]);
  
  // Additional happy message at bottom
  display.drawString(64, 48, ganamosConfig.petName + " is happy!");
  
  display.display();
}

void initPet(String petType) {
  // Initialize pet stats based on type
  petStats.happiness = 50;  // Start at neutral happiness
  petStats.fullness = 50;   // Start at neutral fullness
  petStats.lastUpdate = millis();  // Initialize entropy tracking
  
  Serial.println("Initialized " + petType + " pet with default stats");
}

// Intentionally empty: mood is driven by updatePetStats(). Kept only because
// the .ino still calls it; safe to delete together with that call.
void updatePetMood(int price, int sats) {
  (void)price;
  (void)sats;
}

PetStats petStats = {50, 50, 0, 0}; // happiness, fullness, lastUpdate, lastActivity

static float hungerDecayRemainder = 0.0f;
static float happinessDecayRemainder = 0.0f;

static void applyDecayForMilliseconds(unsigned long elapsedMs) {
  if (elapsedMs == 0) return;
  const float days = (float)elapsedMs / 86400000.0f;
  hungerDecayRemainder += economyConfig.hungerDecayPer24h * days;
  happinessDecayRemainder += economyConfig.happinessDecayPer24h * days;

  const int hungerPoints = (int)hungerDecayRemainder;
  const int happinessPoints = (int)happinessDecayRemainder;
  hungerDecayRemainder -= hungerPoints;
  happinessDecayRemainder -= happinessPoints;
  petStats.fullness = max(0, petStats.fullness - hungerPoints);
  petStats.happiness = max(0, petStats.happiness - happinessPoints);
}

// --- Offline decay across reboots -----------------------------------------
// The device saves the epoch with its stats. At boot NTP is usually not synced
// yet, so the offline gap is applied the first time a valid wall-clock time is
// available (checked in the 60 s block of updatePetStats).
static const time_t MIN_VALID_EPOCH = 1600000000;  // Sep 2020
static uint32_t petBaseEpoch = 0;          // epoch the loaded stats were saved at (0 = unknown)
static unsigned long petBaseMillis = 0;    // millis() when the stats were loaded
static bool offlineDecayPending = false;   // offline gap not applied yet

static bool currentEpoch(time_t& out) {
  time_t t = time(nullptr);
  if (t > MIN_VALID_EPOCH) {
    out = t;
    return true;
  }
  return false;
}

// Epoch up to which decay has already been applied to petStats. Before the
// offline gap is applied (or while the clock is unknown) this is the loaded
// epoch plus uptime, so a reboot before NTP sync never loses the saved epoch.
static uint32_t accountedEpoch() {
  time_t now;
  if (!offlineDecayPending && currentEpoch(now)) return (uint32_t)now;
  if (petBaseEpoch == 0) return 0;
  return petBaseEpoch + (uint32_t)((millis() - petBaseMillis) / 1000UL);
}

// Applies the offline gap once a valid time exists. Returns true if the stats
// should be saved (decay applied or the stored epoch needs correcting).
static bool tryApplyOfflineDecay() {
  if (!offlineDecayPending) return false;
  time_t now;
  if (!currentEpoch(now)) return false;

  const uint32_t accounted = accountedEpoch();
  offlineDecayPending = false;
  const long elapsedSeconds = (long)((int64_t)now - (int64_t)accounted);

  if (elapsedSeconds < 0) {
    Serial.println("Pet stats: saved time is in the future; skipping offline decay");
    return true;  // re-save with the real epoch
  }
  if (elapsedSeconds == 0) return false;

  const int oldFullness = petStats.fullness;
  const int oldHappiness = petStats.happiness;
  // More than 30 days already takes both meters to zero at current rates;
  // cap the conversion so seconds-to-milliseconds cannot overflow uint32_t.
  const unsigned long cappedSeconds = min((unsigned long)elapsedSeconds, 30UL * 86400UL);
  applyDecayForMilliseconds(cappedSeconds * 1000UL);
  Serial.printf("Pet stats: offline %ld min; fullness %d -> %d, happiness %d -> %d\n",
                elapsedSeconds / 60, oldFullness, petStats.fullness, oldHappiness, petStats.happiness);
  return true;
}

void updatePetStats(int newBalance, int oldBalance) {
  (void)newBalance;
  (void)oldBalance;
  unsigned long now = millis();
  
  // Note: Earning sats no longer automatically feeds pet
  // Pet feeding must be done manually via menu
  
  // Accumulate fractional decay. The old five-minute integer calculation always
  // truncated 0.05 * 5 to zero and then reset the timer, effectively disabling decay.
  if (petStats.lastUpdate == 0) petStats.lastUpdate = now;
  if (now - petStats.lastUpdate >= 60000) {
    unsigned long elapsedMs = now - petStats.lastUpdate;
    
    int oldFullness = petStats.fullness;
    int oldHappiness = petStats.happiness;

    bool dirty = tryApplyOfflineDecay();
    applyDecayForMilliseconds(elapsedMs);
    
    petStats.lastUpdate = now;
    
    // Save pet stats to flash if they changed due to entropy
    if (dirty || oldFullness != petStats.fullness || oldHappiness != petStats.happiness) {
      savePetStats();
    }
  }
}

// Writes the last known sats balance and coin count with one NVS session,
// skipping keys whose value did not change.
static int persistedLastBalance = -1;
static int persistedLastCoins = -1;
static void persistLastKnownBalances(int sats, int coins) {
  if (sats == persistedLastBalance && coins == persistedLastCoins) return;
  extern Preferences preferences;
  if (!preferences.begin("satoshi-pet", false)) return;
  if (sats != persistedLastBalance) preferences.putInt("lastBalance", sats);
  if (coins != persistedLastCoins) preferences.putInt("lastCoins", coins);
  preferences.end();
  persistedLastBalance = sats;
  persistedLastCoins = coins;
}

// Balance bookkeeping driven by renderPet(): detects earned sats (celebration),
// persists the last known balance, and advances pet decay.
static void trackBalanceChange(int satoshis) {
  static int oldBalance = -1; // Initialize to -1 to detect first balance
  static bool firstRun = true;
  static bool firstSyncAfterBoot = true; // Skip celebration on first sync after boot

  // On first run, load the last known balance from flash to prevent false celebrations
  if (firstRun) {
    oldBalance = loadLastKnownBalance();
    persistedLastBalance = oldBalance;
    Serial.println("Loaded last known balance from flash: " + String(oldBalance) + " sats");
    firstRun = false;
  }

  // On first sync after boot: silently update oldBalance to current balance without celebration
  // This handles the case where the device was off and balance changed - we don't want to celebrate that
  if (firstSyncAfterBoot) {
    // Only update if we actually got a new balance from API (satoshis > 0)
    // If satoshis is still 0, it means API hasn't fetched yet - don't update oldBalance
    if (satoshis > 0) {
      Serial.println("First sync after boot - balance " + String(satoshis) + " sats (was " + String(oldBalance) + ", no celebration)");
      int previousBalance = oldBalance;
      persistLastKnownBalances(satoshis, getLocalCoins());
      updatePetStats(satoshis, previousBalance);
      oldBalance = satoshis;
      firstSyncAfterBoot = false;
    }
    return;
  }

  // After first sync is complete, check for actual sats earned
  if (satoshis > oldBalance && oldBalance >= 0) {
    int earned = satoshis - oldBalance;
    Serial.println("BALANCE INCREASED! Old: " + String(oldBalance) + " New: " + String(satoshis) + " Earned: " + String(earned));
    triggerCelebration(earned, ganamosConfig.lastMessage);
  }

  // Persist the latest balance (and coins) whenever the balance changes so
  // the value survives a power-off.
  if (satoshis != oldBalance) {
    persistLastKnownBalances(satoshis, getLocalCoins());
  }

  updatePetStats(satoshis, oldBalance);
  oldBalance = satoshis;
}

void renderPet(SSD1306Wire &display, int btcPrice, int satoshis, int batteryPercent) {
  unsigned long funcStart = millis();
  static unsigned long lastFrameUpdate = 0;
  static int currentAnimFrame = 0;

  trackBalanceChange(satoshis);
  
  // Check if we should show celebration
  if (showCelebration) {
    if (millis() - celebrationStart < CELEBRATION_DURATION) {
      renderCelebration(display);
      return; // Skip normal display
    } else {
      showCelebration = false; // End celebration
      turnOffLED(); // Turn off the rainbow LED
    }
  }
  
  // Determine animation state based on pet stats
  PetAnimationState targetAnimState;
  if (petStats.fullness == 0 && petStats.happiness == 0) {
    targetAnimState = ANIM_DIE;
  } else if (petStats.fullness < 20 || petStats.happiness < 20) {
    targetAnimState = ANIM_SAD;
  } else if (currentAnimState == ANIM_EAT) {
    // Keep eating animation running if active
    targetAnimState = ANIM_EAT;
  } else {
    targetAnimState = ANIM_DEFAULT;
  }
  
  // If animation state changed (and not from eat), reset frame counter
  if (targetAnimState != currentAnimState && currentAnimState != ANIM_EAT) {
    currentAnimFrame = 0;
    currentAnimState = targetAnimState;
    eatAnimLoopsCompleted = 0;
  }
  
  // Update animation frame every 400ms for smoother animations
  unsigned long now = millis();
  if (now - lastFrameUpdate > 400) {
    currentAnimFrame++;
    
    // Handle eat animation looping (3 complete loops then back to default/sad)
    if (currentAnimState == ANIM_EAT) {
      // Get frame count for current pet's eat animation
      int maxFrames = getPetAnimFrameCount(currentPetKind(), ANIM_EAT);
      
      if (currentAnimFrame >= maxFrames) {
        eatAnimLoopsCompleted++;
        currentAnimFrame = 0;
        
        if (eatAnimLoopsCompleted >= EAT_ANIM_TOTAL_LOOPS) {
          // Eat animation complete, transition back to appropriate state
          eatAnimLoopsCompleted = 0;
          if (petStats.fullness < 20 || petStats.happiness < 20) {
            currentAnimState = ANIM_SAD;
          } else {
            currentAnimState = ANIM_DEFAULT;
          }
          currentAnimFrame = 0;
        }
      }
    }
    
    lastFrameUpdate = now;
  }
  
  unsigned long preDrawTime = millis() - funcStart;
  if (preDrawTime > 50) {
    Serial.print(F("Pre-draw logic took "));
    Serial.print(preDrawTime);
    Serial.println(F("ms"));
  }
  
  // Normal display
  unsigned long drawStart = millis();
  display.clear();
  display.setFont(ArialMT_Plain_10);

  // Left side: Stats stacked vertically
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  
  // Top left: Pet name
  String shortName = ganamosConfig.petName;
  while (shortName.length() && display.getStringWidth(shortName) > 62) shortName.remove(shortName.length() - 1);
  display.drawString(0, 0, shortName);
  // Sync status just left of the battery: nothing when synced, animated dots
  // while connecting/syncing, an icon only once something actually failed.
  static const uint8_t ICON_WIFI_OFF[] = {  // WiFi with a slash: no WiFi
    0x7A, 0x00, 0x86, 0x00, 0x29, 0x01, 0x54, 0x00, 0x30, 0x00, 0x40, 0x00, 0x80, 0x00, 0x00, 0x01 };
  static const uint8_t ICON_WARNING[] = {   // warning triangle: last sync failed
    0x10, 0x00, 0x28, 0x00, 0x28, 0x00, 0x54, 0x00, 0x54, 0x00, 0x82, 0x00, 0x92, 0x00, 0xFF, 0x01 };
  static const uint8_t ICON_UPLOAD[] = {    // up arrow: feedings/scores waiting to upload
    0x10, 0x00, 0x38, 0x00, 0x54, 0x00, 0x92, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0xFE, 0x00 };
  extern bool wifiConnecting;
  extern bool syncPending;
  extern bool syncAttempted;
  const uint8_t* statusIcon = nullptr;
  if (syncPending || wifiConnecting) {
    // Syncing (e.g. just woke up): animated ". .. ..." rather than a warning
    int dots = (millis() / 400) % 3 + 1;
    for (int i = 0; i < dots; i++) display.fillRect(128 - 18 - 11 + i * 3, 5, 2, 2);
  } else if (WiFi.status() != WL_CONNECTED) {
    statusIcon = ICON_WIFI_OFF;                 // couldn't connect
  } else if (getPendingSpendCount() || getPendingGameScoreCount()) {
    statusIcon = ICON_UPLOAD;
  } else if (syncAttempted && !lastSyncSucceeded) {
    statusIcon = ICON_WARNING;                  // a sync was tried and failed
  }
  if (statusIcon) display.drawXbm(128 - 18 - 12, 0, 9, 8, statusIcon);
  
    // Top right: Battery icon (16x8 pixels, right-aligned)
  extern int getBatteryLevel();
  extern bool isBatteryCharging();
  int batteryLvl = getBatteryLevel();
  bool charging = isBatteryCharging();

  int battX = 128 - 18;
  int battY = 1;

  // Draw battery outline (12x6 body + 2x3 nub)
  display.drawRect(battX, battY, 12, 6);
  display.fillRect(battX + 12, battY + 1, 2, 4);

  // Draw fill bars - animate if charging
  int displayLevel;
  if (charging) {
    // Cycle through 1-2-3 every 500ms
    displayLevel = ((millis() / 500) % 3) + 1;
  } else {
    displayLevel = batteryLvl;
  }

  for (int i = 0; i < displayLevel; i++) {
    int barX = battX + 1 + (i * 3);
    display.fillRect(barX, battY + 1, 2, 4);
  }
  
  // Coins with "C:" prefix - use LOCAL balance (works offline, reflects spending)
  extern int getLocalCoins();
  int coins = getLocalCoins();
  String coinsText = "C: ";
  if (coins >= 100000) {
    // 100k and above: round to nearest k without decimal (max 3 digits)
    int roundedK = (coins + 500) / 1000; // Round to nearest thousand
    coinsText += String(roundedK) + "k";
  } else if (coins > 999) {
    // 1k to 99.9k: show one decimal place
    coinsText += String(coins/1000.0, 1) + "k";
  } else {
    coinsText += String(coins);
  }
  display.drawString(0, 13, coinsText);
  
  // Happiness with "H:" prefix
  display.drawString(0, 26, "H: " + String(petStats.happiness));
  
  // Fullness with "F:" prefix
  display.drawString(0, 39, "F: " + String(petStats.fullness));
  
  // Bottom left: Sats balance with "B:" prefix and USD equivalent
  String satsText = "B: ";
  int sats = ganamosConfig.balance;
  if (sats >= 1000000) {
    satsText += String(sats / 1000000.0, 1) + "M";
  } else if (sats >= 10000) {
    satsText += String(sats / 1000) + "k";
  } else if (sats >= 1000) {
    satsText += String(sats / 1000.0, 1) + "k";
  } else {
    satsText += String(sats);
  }

  // Add USD equivalent if we have a valid BTC price
  if (ganamosConfig.btcPrice > 0) {
    float usdValue = (sats / 100000000.0) * ganamosConfig.btcPrice;
    int roundedUsd = (int)(usdValue + 0.5);  // Round to nearest dollar
    satsText += " \xB7 $" + String(roundedUsd);  // \xB7 is the interpunct (·)
  }

  display.drawString(0, 54, satsText);

  // Right side: Draw pet sprite - position and size vary by pet type/animation
  PetKind kind = currentPetKind();
  const PetSpriteAnim& anim = getPetSpriteAnim(kind, currentAnimState);
  const uint8_t* sprite = getPetSprite(kind, currentAnimState, currentAnimFrame);
  display.drawXbm(petSpriteHomeX(kind), 12, anim.width, anim.height, sprite);

  unsigned long drawTime = millis() - drawStart;
  if (drawTime > 50) {
    Serial.print(F("Draw commands took "));
    Serial.print(drawTime);
    Serial.println(F("ms"));
  }
  
  unsigned long dispStart = millis();
  display.display();
  unsigned long dispTime = millis() - dispStart;
  if (dispTime > 50) {
    Serial.print(F("display.display() took "));
    Serial.print(dispTime);
    Serial.println(F("ms"));
  }
}

void renderMenu(SSD1306Wire &display, int menuOption) {
  // menuOption: 0=Home, 1=Play, 2=Feed, 3=Jobs
  // 2x2 grid layout for better use of horizontal space
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  
  String options[] = {"Home", "Play", "Feed", "Jobs"};
  
  // Grid positions: [index] = {x, y}
  // Row 0: y=12, Row 1: y=36 (vertically centered)
  // Col 0: x=8,  Col 1: x=68
  int positions[][2] = {
    {8, 9},    // 0: Home (top-left)
    {68, 9},   // 1: Play (top-right)
    {8, 30},   // 2: Feed (bottom-left)
    {68, 30}   // 3: Jobs (bottom-right)
  };
  
  for (int i = 0; i < 4; i++) {
    String line = (i == menuOption) ? "> " : "  ";
    line += options[i];
    display.drawString(positions[i][0], positions[i][1], line);
  }
  
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  if (holdHintsVisible()) display.drawString(64, 50, "Tap: next  Hold: pick");
  display.display();
}

// ============================================================================
// Jobs List Feature - Browse open jobs from user's private groups
// ============================================================================

// Marquee scrolling state
static int jobsMarqueeOffset = 0;
static unsigned long lastMarqueeUpdate = 0;
static int lastSelectedJob = -1;
const int MARQUEE_SPEED_MS = 150;  // Scroll speed (lower = faster)
const int MARQUEE_PAUSE_MS = 1500; // Pause at start before scrolling
const int JOB_TITLE_VISIBLE_CHARS = 15; // Title chars shown in the list before truncating/scrolling

void renderJobsList(SSD1306Wire &display, int selectedJob, int scrollOffset) {
  extern Job cachedJobs[];
  extern int cachedJobCount;
  extern String formatSatsShort(int sats);
  
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  
  if (cachedJobCount == 0) {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 25, "No open jobs");
    display.display();
    return;
  }
  
  // Single-row layout: 4 items visible (jobs + back option)
  // selectedJob == cachedJobCount means "Back" is selected
  const int rowHeight = 15;      // Height per row (10pt font + padding)
  const int visibleItems = 4;
  const int totalItems = cachedJobCount + 1;  // +1 for Back option
  
  int startIdx = scrollOffset;
  int endIdx = min(totalItems, startIdx + visibleItems);
  
  for (int i = startIdx; i < endIdx; i++) {
    int displayIdx = i - startIdx;  // 0, 1, 2, or 3
    int y = displayIdx * rowHeight;
    
    bool isSelected = (i == selectedJob);
    String selector = isSelected ? "> " : "  ";
    
    // Check if this is the Back option
    if (i == cachedJobCount) {
      // Draw Back option
      display.drawString(0, y, selector + "<- Back");
      continue;
    }
    
    Job& job = cachedJobs[i];
    String title = job.title;
    
    // Marquee scrolling for selected job if title is too long
    if (isSelected && title.length() > JOB_TITLE_VISIBLE_CHARS) {
      unsigned long now = millis();
      
      // Reset marquee when selection changes
      if (lastSelectedJob != selectedJob) {
        jobsMarqueeOffset = 0;
        lastMarqueeUpdate = now;
        lastSelectedJob = selectedJob;
      }
      
      // Calculate visible portion with scrolling
      int maxScroll = title.length() - JOB_TITLE_VISIBLE_CHARS + 3; // +3 for "..." effect
      
      // Pause at start, then scroll
      if (now - lastMarqueeUpdate > MARQUEE_PAUSE_MS || jobsMarqueeOffset > 0) {
        if (now - lastMarqueeUpdate > MARQUEE_SPEED_MS) {
          jobsMarqueeOffset++;
          if (jobsMarqueeOffset > maxScroll) {
            jobsMarqueeOffset = 0;  // Reset to start
          }
          lastMarqueeUpdate = now;
        }
      }
      
      // Extract visible portion
      int startChar = jobsMarqueeOffset;
      title = title.substring(startChar, startChar + JOB_TITLE_VISIBLE_CHARS);
    } else if (title.length() > JOB_TITLE_VISIBLE_CHARS) {
      // Non-selected long title: truncate with ellipsis
      title = title.substring(0, JOB_TITLE_VISIBLE_CHARS - 2) + "..";
    }
    
    // Draw title (left aligned)
    display.drawString(0, y, selector + title);
    
    // Draw reward (right aligned, no "sats" label)
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, y, formatSatsShort(job.reward));
    display.setTextAlignment(TEXT_ALIGN_LEFT);  // Reset for next row
  }
  
  // Scroll indicator if more items than visible
  if (totalItems > visibleItems) {
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 54, String(selectedJob + 1) + "/" + String(cachedJobCount));
  }
  
  display.display();
}

void renderJobDetail(SSD1306Wire &display, int jobIndex) {
  extern Job cachedJobs[];
  extern int cachedJobCount;
  extern String formatSatsShort(int sats);
  
  if (jobIndex < 0 || jobIndex >= cachedJobCount) return;
  
  Job& job = cachedJobs[jobIndex];
  
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  
  // Title - word wrap up to 3 lines
  String title = job.title;
  int titleY = 0;
  int maxCharsPerLine = 21;  // ~128px / 6px per char
  int linesUsed = 0;
  
  while (title.length() > 0 && linesUsed < 3) {
    String line;
    if (title.length() <= maxCharsPerLine) {
      line = title;
      title = "";
    } else {
      // Find last space within limit
      int breakPoint = maxCharsPerLine;
      int lastSpace = title.lastIndexOf(' ', maxCharsPerLine);
      if (lastSpace > 5) {
        breakPoint = lastSpace;
      }
      line = title.substring(0, breakPoint);
      title = title.substring(breakPoint);
      title.trim();
    }
    display.drawString(0, titleY + (linesUsed * 11), line);
    linesUsed++;
  }
  
  // Reward (larger, prominent)
  int rewardY = 36;
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, rewardY, String(job.reward) + " sats");
  
  // Bottom line: instructions
  display.setFont(ArialMT_Plain_10);
  if (holdHintsVisible()) display.drawString(0, 54, "Tap: back  Hold: done");
  display.display();
}

// Main Jobs menu handler - called when Jobs is selected from main menu
void handleJobsMenu(SSD1306Wire &display) {
  extern Job cachedJobs[];
  extern int cachedJobCount;
  extern bool fetchJobs();
  
  // Show loading screen
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 25, "Loading jobs...");
  display.display();
  
  // Fetch jobs from server
  bool success = fetchJobs();
  
  if (!success || cachedJobCount == 0) {
    display.clear();
    display.drawString(64, 20, success ? "No open jobs" : "Failed to load");
    display.drawString(64, 35, "in your groups");
    display.display();
    delay(2000);
    return;
  }
  
  // Jobs list navigation
  int selectedJob = 0;
  int scrollOffset = 0;  // Which job is at the top of the visible list
  bool inJobsMenu = true;
  unsigned long lastJobCycle = millis();
  
  // Reset marquee state
  jobsMarqueeOffset = 0;
  lastMarqueeUpdate = millis();
  lastSelectedJob = -1;
  
  renderJobsList(display, selectedJob, scrollOffset);
  
  extern bool anyButtonDown();

  while (inJobsMenu) {
    esp_task_wdt_reset();
    soundUpdate();
    unsigned long now = millis();
    
    // Timeout after 30 seconds
    if (now - lastJobCycle > 30000) {
      inJobsMenu = false;
      break;
    }
    
    // Tap = next, hold = open (counts as soon as the line fills)
    PressEvent ev = holdPressUpdate(anyButtonDown());
    if (holdPressActive()) lastJobCycle = millis();
    holdPressDrawBar(display);

    if (ev != PRESS_NONE) {
      lastJobCycle = millis();

      bool isShort = ev == PRESS_TAP;
      bool isHold = ev == PRESS_HOLD;
      
            if (isShort) {
        // Cycle to next item (jobs + back option)
        int totalItems = cachedJobCount + 1;  // +1 for Back
        selectedJob = (selectedJob + 1) % totalItems;
        
        // Adjust scroll offset to keep selected item visible
        if (selectedJob < scrollOffset) {
          scrollOffset = selectedJob;
        } else if (selectedJob >= scrollOffset + 4) {
          scrollOffset = selectedJob - 3;
        }
        
        // Reset marquee for new selection
        jobsMarqueeOffset = 0;
        lastMarqueeUpdate = millis();
        
        // Play button chirp
        extern void playButtonChirp();
        playButtonChirp();
        
        renderJobsList(display, selectedJob, scrollOffset);
      } else if (isHold) {
        // Check if Back is selected
        if (selectedJob == cachedJobCount) {
          // Back selected - exit jobs menu
          extern void playMenuSelectTone();
          playMenuSelectTone();
          inJobsMenu = false;
          break;
        }
        
        // Show job detail
        extern void playMenuSelectTone();
        playMenuSelectTone();
        
        renderJobDetail(display, selectedJob);
        
        // Detail view: tap = back to list, hold = mark as complete.
        // The hold that opened this screen is still down; holdPressUpdate
        // ignores it until release.
        unsigned long detailStart = millis();
        bool inDetailView = true;
        
        while (inDetailView && (millis() - detailStart < 30000)) {  // 30s timeout
          esp_task_wdt_reset();
          soundUpdate();
          PressEvent detailEv = holdPressUpdate(anyButtonDown());
          if (holdPressActive()) detailStart = millis();
          holdPressDrawBar(display);
          
          if (detailEv != PRESS_NONE) {
            if (detailEv == PRESS_HOLD) {
              // Long press - show confirmation for marking complete
              playMenuSelectTone();
              
              // Show confirmation screen
              display.clear();
              display.setFont(ArialMT_Plain_16);
              display.setTextAlignment(TEXT_ALIGN_CENTER);
              display.drawString(64, 5, "Mark Done?");
              
              display.setFont(ArialMT_Plain_10);
              display.drawString(64, 28, "Tap: Yes");
              display.drawString(64, 42, "Hold: Cancel");
              display.display();
              
              // Wait for confirmation (the hold that got here is ignored
              // until it is released)
              unsigned long confirmStart = millis();
              
              while (millis() - confirmStart < 10000) {  // 10s timeout for confirm
                esp_task_wdt_reset();
                soundUpdate();
                PressEvent confirmEv = holdPressUpdate(anyButtonDown());
                if (holdPressActive()) confirmStart = millis();
                holdPressDrawBar(display);
                
                if (confirmEv != PRESS_NONE) {
                  if (confirmEv == PRESS_TAP) {
                    // Short press = Confirm! Finish the tone before the
                    // network call: nothing services the sound sequencer
                    // while the request blocks, so a non-blocking tone would
                    // hold its first note until the server answers.
                    extern void playMenuSelectToneBlocking();
                    playMenuSelectToneBlocking();
                    soundStop();
                    
                    // Show "Sending..." screen
                    display.clear();
                    display.setFont(ArialMT_Plain_10);
                    display.setTextAlignment(TEXT_ALIGN_CENTER);
                    display.drawString(64, 25, "Notifying poster...");
                    display.display();
                    
                    esp_task_wdt_reset();
                    soundUpdate();

                    // Call the API
                    extern bool markJobComplete(String jobId);
                    bool success = markJobComplete(cachedJobs[selectedJob].id);
                    
                    // Show result
                    display.clear();
                    display.setFont(ArialMT_Plain_16);
                    if (success) {
                      display.drawString(64, 15, "Sent!");
                      display.setFont(ArialMT_Plain_10);
                      display.drawString(64, 38, "Poster will verify");
                    } else {
                      display.drawString(64, 15, "Failed");
                      display.setFont(ArialMT_Plain_10);
                      display.drawString(64, 38, "Try again later");
                    }
                    display.display();
                    delay(2500);

                    if (success) {
                      // The job now waits for the poster's review, and the
                      // server stops listing it: drop it from the cached list
                      // too instead of showing it until the next fetch.
                      for (int i = selectedJob; i < cachedJobCount - 1; i++) {
                        cachedJobs[i] = cachedJobs[i + 1];
                      }
                      cachedJobCount--;
                      if (cachedJobCount == 0) {
                        inDetailView = false;
                        inJobsMenu = false;
                        break;
                      }
                      if (selectedJob >= cachedJobCount) selectedJob = cachedJobCount - 1;
                      if (scrollOffset > selectedJob) scrollOffset = selectedJob;
                      lastSelectedJob = -1;  // reset the title marquee
                    }

                    inDetailView = false;
                  } else {
                    // Long press = Cancel
                    playMenuSelectTone();
                    inDetailView = false;
                  }
                  break;
                }
                delay(30);
              }
              
              // Timeout = cancel
              if (millis() - confirmStart >= 10000) {
                inDetailView = false;
              }
              
            } else {
              // Short press - exit detail view
              playMenuSelectTone();
              inDetailView = false;
            }
          }
          delay(30);
        }
        
        // Return to list
        renderJobsList(display, selectedJob, scrollOffset);
      }
    }
    
    // Update marquee animation even when no button pressed
    // selectedJob == cachedJobCount is the "Back" row (no job behind it)
    // (not while the button is down: the redraw would wipe the hold line)
    if (!holdPressActive() && selectedJob >= 0 && selectedJob < cachedJobCount &&
        cachedJobs[selectedJob].title.length() > JOB_TITLE_VISIBLE_CHARS) {
      static unsigned long lastMarqueeRender = 0;
      if (now - lastMarqueeRender > MARQUEE_SPEED_MS) {
        renderJobsList(display, selectedJob, scrollOffset);
        lastMarqueeRender = now;
      }
    }
    
    delay(30);
  }
}

void renderFeedResult(SSD1306Wire &display, bool success, int foodIndex, bool hadEnoughCoins) {
  extern int getLocalCoins();
  const FoodOption& option = getFoodOption(foodIndex);
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  if (success) {
    // Large "Yum!" message
    display.drawString(64, 10, "Yum!");
    
    // Pet name loves food message
    display.setFont(ArialMT_Plain_10);
    String loveMessage = ganamosConfig.petName + " loves " + String(option.name);
    display.drawString(64, 30, loveMessage);
    
    // Cost and fullness on one line with interpunct
    String stats = "-" + String(option.coinCost) + "c · +" + String(option.fullnessPercent) + "f";
    display.drawString(64, 44, stats);
  } else {
    display.drawString(64, 10, "Feed Failed");
    display.setFont(ArialMT_Plain_10);
    if (!hadEnoughCoins) {
      int localCoins = getLocalCoins();
      display.drawString(64, 28, "Not enough coins!");
      display.drawString(64, 40, "Need: " + String(option.coinCost) + " coins");
      display.drawString(64, 52, "Have: " + String(localCoins) + " coins");
    } else {
      display.drawString(64, 30, "Connection issue");
      display.drawString(64, 44, "Try again shortly");
    }
  }
  
  display.display();
}

static void drawFoodIllustration(SSD1306Wire &display, int foodIndex) {
  const int iconY = 2;  // Start at y=2, icon ends at y=50 (2+48), leaves room for text below
  const int iconX = 72; // All food items are 48x48, positioned at x=72 (moved from 80)
  const int iconWidth = 48;
  const int iconHeight = 48;

  const FoodOption& option = getFoodOption(foodIndex);
  display.drawXbm(iconX, iconY, iconWidth, iconHeight, option.bitmap);
}

void renderFoodSelectionMenu(SSD1306Wire &display, int selectedFood) {
  // selectedFood can now be 0 to FOOD_OPTION_COUNT (where FOOD_OPTION_COUNT = Back)
  bool isBackSelected = (selectedFood == FOOD_OPTION_COUNT);
  int clampedFood = isBackSelected ? 0 : clampFoodIndex(selectedFood);

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  // Draw food list on the left with smaller text (3 foods + Back = 4 items)
  const int blockHeight = 15;  // Reduced to fit 4 items
  for (int i = 0; i < FOOD_OPTION_COUNT; i++) {
    const FoodOption& option = getFoodOption(i);
    int y = 1 + (i * blockHeight);

    display.setFont(ArialMT_Plain_10);
    String line = (i == selectedFood ? "> " : "  ");
    line += option.name;
    display.drawString(2, y, line);
  }
  
  // Draw Back option as last item
  int backY = 1 + (FOOD_OPTION_COUNT * blockHeight);
  display.setFont(ArialMT_Plain_10);
  String backLine = isBackSelected ? "> <- Back" : "  <- Back";
  display.drawString(2, backY, backLine);

  // Draw the 48x48 food icon on the right (only if a food is selected, not Back)
  if (!isBackSelected) {
    drawFoodIllustration(display, clampedFood);

    // Draw stats BELOW the food icon (48x48 icon at y=2 ends at y=50)
    const FoodOption& selectedOption = getFoodOption(clampedFood);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    // Single line with interpunct separator: "500c · +25%F"
    // Icon at x=72, width=48, so center is at x=96 (72 + 24)
    display.drawString(96, 52, String(selectedOption.coinCost) + "c · +" + String(selectedOption.fullnessPercent) + "%F");
  }
  
  display.display();
}

static void flashRgbLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(RGB_LED, HIGH);
    delay(onMs);
    digitalWrite(RGB_LED, LOW);
    delay(offMs);
  }
  digitalWrite(RGB_LED, LOW);
}

// Same notes and gaps as the old tone()+delay() loops (freq 0 = rest).
static const ToneNote HIGH_SCORE_MELODY[] = {
  {988, 120}, {0, 40}, {1175, 120}, {0, 40}, {1319, 150}, {0, 40}, {1568, 220}, {0, 40}
};
static const ToneNote GAME_OVER_WOMP[] = {
  {392, 200}, {0, 60}, {330, 220}, {0, 60}, {262, 300}, {0, 60}
};

// The game-over sequence is a blocking UI flow (delay()-based LED flashes and
// result screens), so the loop-driven sequencer would not advance during it;
// play these to completion like before. Now respects quiet hours.
static void playHighScoreCelebrationTone() {
  soundPlayBlocking(HIGH_SCORE_MELODY, (uint8_t)(sizeof(HIGH_SCORE_MELODY) / sizeof(HIGH_SCORE_MELODY[0])));
}

static void playGameOverWomp() {
  soundPlayBlocking(GAME_OVER_WOMP, (uint8_t)(sizeof(GAME_OVER_WOMP) / sizeof(GAME_OVER_WOMP[0])));
}

static void renderLeaderboard(SSD1306Wire &display, const GameScoreResponse &response) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, "Top Scores");
  display.drawLine(0, 14, 127, 14);

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  int y = 18;
  for (int i = 0; i < response.leaderboardCount && i < 5; i++) {
    const LeaderboardEntry &entry = response.leaderboard[i];
    String line = String(entry.rank) + ". ";
    if (entry.isYou) {
      line += "You";
    } else {
      line += entry.petName.length() > 0 ? entry.petName : "Pet";
    }
    if (line.length() > 18) {
      line = line.substring(0, 18);
    }
    display.drawString(4, y, line);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(124, y, String(entry.score));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    y += 10;
  }

  if (response.leaderboardCount == 0 && !response.hasPersonalEntry) {
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 36, "No scores yet");
    display.display();
    return;
  }

  if (response.hasPersonalEntry) {
    display.drawLine(0, y, 127, y);
    y += 4;
    const LeaderboardEntry &me = response.personalEntry;
    String line = "#" + String(me.rank) + " You";
    display.drawString(4, y, line);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(124, y, String(me.score));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
  }

  display.display();
}

bool handleFeedPet(int foodIndex) {
  const FoodOption& option = getFoodOption(foodIndex);
  int foodCost = option.coinCost;
  
  // Check if we have enough coins (use local balance for offline support)
  extern int getLocalCoins();
  int localCoins = getLocalCoins();
  if (localCoins < foodCost) {
    Serial.println("Not enough coins to feed pet! Need " + String(foodCost) + ", have " + String(localCoins));
    return false;
  }
  
  // Spend coins locally (works offline, syncs when online)
  extern bool spendCoinsLocal(int amount, const char* action);
  bool success = spendCoinsLocal(foodCost, "feed");
  
  if (success) {
    int oldFullness = petStats.fullness;
    int fullnessIncrease = option.fullnessPercent;
    petStats.fullness = min(100, petStats.fullness + fullnessIncrease);
    int actualIncrease = petStats.fullness - oldFullness;
    petStats.lastUpdate = millis(); // Reset entropy timer after feeding
    
    Serial.println("Pet fed with " + String(option.name) + " (" + String(foodCost) + " coins)");
    Serial.println("Fullness increased by " + String(actualIncrease) + " (target +" + String(option.fullnessPercent) + "%), now " + String(petStats.fullness));
    
    // Trigger eat animation (will loop 3 times)
    currentAnimState = ANIM_EAT;
    eatAnimLoopsCompleted = 0;
    Serial.println("🍽️ Starting eat animation");
    
    // Save pet stats to flash after feeding
    savePetStats();
    
    return true;
  } else {
    Serial.println("Failed to spend coins for feeding");
    return false;
  }
}

struct FlapDebouncedButton {
  int pin;
  bool lastStable = true;       // HIGH = not pressed (because of pull-up)
  bool lastEdgeState = true;
  unsigned long lastChange = 0;
  unsigned long graceUntil = 0; // press buffer window (ms)
  const unsigned long debounceMs = 25;
  const unsigned long graceMs = 120;

  void begin() {
    pinMode(pin, INPUT_PULLUP);   // IMPORTANT for GPIO0
    lastStable   = digitalRead(pin);
    lastEdgeState = lastStable;
  }

  // Call every frame. Returns true exactly once per press (on the falling edge).
  bool justPressed() {
    bool reading = digitalRead(pin);
    unsigned long now = millis();

    if (reading != lastStable) {          // input changed -> debounce timer
      lastChange = now;
      lastStable = reading;
    }

    // Accept change only if stable for debounceMs
    bool edgeFired = false;
    if (now - lastChange > debounceMs) {
      // Detect falling edge (HIGH -> LOW) = press
      if (lastEdgeState == HIGH && lastStable == LOW) {
        edgeFired = true;
        graceUntil = now + graceMs;       // make it “hard” to miss while rendering
      }
      lastEdgeState = lastStable;
    }

    // Edge fires immediately; additionally, allow a late consumer to “see” it within grace window
    if (edgeFired) return true;
    if (graceUntil && now <= graceUntil) {
      graceUntil = 0;                     // consume buffered press once
      return true;
    }
    return false;
  }
};

void renderInsufficientCoinsForGame(SSD1306Wire &display, int required, int available) {
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  display.drawString(64, 8, "Not Enough");
  display.drawString(64, 26, "Coins!");
  
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 46, "Need: " + String(required) + " coins");
  display.drawString(64, 56, "Have: " + String(available) + " coins");
  
  display.display();
}

int handleLightningGame(SSD1306Wire &display) {
  extern GanamosConfig ganamosConfig;
  extern int getLocalCoins();
  extern void setLocalCoins(int coins);
  
  // Check if we have enough coins for game (use local balance)
  int localCoins = getLocalCoins();
  if (localCoins < ganamosConfig.gameCost) {
    Serial.println("Not enough coins to play game! Need " + String(ganamosConfig.gameCost) + ", have " + String(localCoins));
    
    // Show error screen to user
    renderInsufficientCoinsForGame(display, ganamosConfig.gameCost, localCoins);
    delay(2500); // Show error for 2.5 seconds
    
    return 0;
  }
  
  // Spend coins locally (offline-first)
  extern bool spendCoinsLocal(int amount, const char* action);
  Serial.println("Spending " + String(ganamosConfig.gameCost) + " coins to play game (current coins: " + String(localCoins) + ")");
  if (!spendCoinsLocal(ganamosConfig.gameCost, "game")) {
    Serial.println("Failed to spend coins locally");
    return 0;
  }
  
  Serial.println("Starting Flappy Pet Game!");
  
  // ===== Game constants (tuned for easier start, better progression) =====
  const int PET_X = 25;
  const int PET_SPRITE_HEIGHT = 16;
  const int PET_SPRITE_WIDTH  = 16;
  const float GRAVITY         = 0.25f;   // Reduced from 0.35f - slower falling for better control
  const float FLAP_VELOCITY   = -3.5f;   // Reduced from -6.0f - smaller jump, need multiple clicks to reach top (like Flappy Bird)
  const float MAX_FALL_SPEED  = 5.0f;    // Reduced from 6.5f - slower terminal velocity
  const int   WALL_WIDTH      = 8;
  const int   WALL_SPACING    = 120;     // Good spacing between walls
  const float INITIAL_SPEED   = 0.9f;    // Increased from 0.5f for faster start
  const float SPEED_INCREASE  = 0.025f;  // Gradual difficulty increase
  const int   SINGLE_WALL_START_SCORE = 15;  // Start single upper or lower walls
  const int   COMBINED_WALL_START_SCORE = 30; // Start combined upper+lower walls
  
// ===== Button init =====
  FlapDebouncedButton flapBtnExternal;
  flapBtnExternal.pin = BUTTON_PIN_EXTERNAL;
  flapBtnExternal.begin();

  FlapDebouncedButton flapBtnPrg;
  flapBtnPrg.pin = BUTTON_PIN_PRG;
  flapBtnPrg.begin();

  // ===== Game state =====
  float petY = 32.0f;
  float petVelocity = 0.0f;
  int   score = 0;
  float wallSpeed = INITIAL_SPEED;
  unsigned long lastFrameTime = millis();
  bool gameOver = false;
  
    // Ready screen (non-blocking release; don’t hold PRG to auto-flap)
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 20, "Ready?");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 40, "Press to flap!");
  display.display();
  delay(800);

  // Ensure the first press after ready is counted once
  // (call once here to clear any residual edge)
  (void)flapBtnExternal.justPressed();
  (void)flapBtnPrg.justPressed();

  // ===== Walls init - Single lower wall only (top walls added after score >= 20) =====
  struct Wall { 
    float x; 
    int wallHeight;  // Height of lower wall (from bottom)
    int topWallHeight;  // Height of top wall (from top) - 0 means no top wall
    bool passed; 
  };
  Wall walls[4];
  for (int i = 0; i < 4; i++) {
    walls[i].x = 128 + (i * WALL_SPACING);
    // Wall height varies - start very easy, get gradually harder
    // Pet starts at y=32, sprite height=16, so pet bottom is at y=48
    // Screen is 64 pixels tall
    // Wall height determines how much clearance: lower wall = easier to clear
    // Wall top Y = 64 - wallHeight, so wallHeight=12 means wall top at y=52 (pet clears by 4px)
    if (i == 0) {
      walls[i].wallHeight = 12;  // First wall: very low (12px) - wall top at y=52, pet at y=48 clears easily
      walls[i].topWallHeight = 0;  // No top wall initially
    } else if (i == 1) {
      walls[i].wallHeight = random(12, 18);  // Second wall: 12-17px (still easy)
      walls[i].topWallHeight = 0;  // No top wall initially
    } else if (i == 2) {
      walls[i].wallHeight = random(15, 22);  // Third wall: 15-21px (medium)
      walls[i].topWallHeight = 0;  // No top wall initially
    } else {
      walls[i].wallHeight = random(18, 28);  // Later walls: 18-27px (harder but jumpable)
      walls[i].topWallHeight = 0;  // No top wall initially
    }
    walls[i].passed = false;
  }
  
  // ===== Main loop =====
  while (!gameOver) {
    unsigned long now = millis();
    float deltaTime = (now - lastFrameTime) / 16.6667f;   // ~frames
    if (deltaTime < 0.25f) deltaTime = 0.25f;             // guard slow clocks
    if (deltaTime > 2.0f)  deltaTime = 2.0f;              // guard hiccups
    lastFrameTime = now;

    // ---- INPUT FIRST (edge + grace) ----
    if (flapBtnExternal.justPressed() || flapBtnPrg.justPressed()) {
      petVelocity = FLAP_VELOCITY;                        // immediate impulse
    }

    // ---- PHYSICS ----
    petVelocity += GRAVITY * deltaTime;
    if (petVelocity >  MAX_FALL_SPEED) petVelocity =  MAX_FALL_SPEED;
    if (petVelocity < -MAX_FALL_SPEED) petVelocity = -MAX_FALL_SPEED;

    petY += petVelocity * deltaTime;

    // Clamp for drawing (not a death at edges)
    if (petY < -8) petY = -8;                             // allow small overshoot
    if (petY > 64 - PET_SPRITE_HEIGHT + 8) petY = 64 - PET_SPRITE_HEIGHT + 8;

    // ---- WALLS / SCORING (same as yours) ----
    for (int i = 0; i < 4; i++) {
      walls[i].x -= wallSpeed * deltaTime;

      if (!walls[i].passed && walls[i].x + WALL_WIDTH < PET_X) {
        walls[i].passed = true;
        score++;
        wallSpeed += SPEED_INCREASE;
      }

      if (walls[i].x + WALL_WIDTH < 0) {
        float rightmostX = walls[0].x;
        for (int j = 1; j < 4; j++) rightmostX = max(rightmostX, walls[j].x);
        walls[i].x = rightmostX + WALL_SPACING;
        // Progressive difficulty: walls get taller as score increases
        int maxHeight = min(30, 18 + (score / 2));
        int minHeight = max(10, 12 - (score / 8));
        
        if (score < SINGLE_WALL_START_SCORE) {
          // Early game: lower walls only
          walls[i].wallHeight = random(minHeight, maxHeight + 1);
          walls[i].topWallHeight = 0;
        }
        else if (score < COMBINED_WALL_START_SCORE) {
          // Mid game (15-29): mix of upper-only and lower-only walls
          bool upperOnly = random(0, 2) == 0; // 50% chance
          if (upperOnly) {
            walls[i].wallHeight = 0; // No lower wall
            walls[i].topWallHeight = random(12, 24); // Upper wall only
          } else {
            walls[i].wallHeight = random(minHeight, maxHeight + 1); // Lower wall only
            walls[i].topWallHeight = 0; // No upper wall
          }
        }
        else {
          // Late game (30+): combined upper+lower walls
          // Screen is 64px, pet is 16px, so gap needs to be at least 20px for playability
          const int MIN_GAP = 40;  // Minimum gap size (comfortable for 16px pet)
          const int MAX_WALL_HEIGHT = 18;  // Max height for each wall
          
          // First, decide the gap size (28-36 pixels)
          int gapSize = random(MIN_GAP, MIN_GAP + 8);
          
          // Then allocate remaining space to walls
          int availableForWalls = 64 - gapSize;  // e.g., 64 - 30 = 34 pixels for both walls
          
          // Split available space between top and bottom walls
          int bottomWallMax = min(MAX_WALL_HEIGHT, availableForWalls / 2);
          int topWallMax = min(MAX_WALL_HEIGHT, availableForWalls / 2);
          
          walls[i].wallHeight = random(8, bottomWallMax + 1);  // Bottom wall
          walls[i].topWallHeight = random(6, topWallMax + 1);  // Top wall
        }
        
        walls[i].passed = false;
      }
    }

    // ---- COLLISION - Check both lower wall and top wall (if present) ----
    int petLeft = PET_X, petRight = PET_X + PET_SPRITE_WIDTH;
    int petTop = (int)petY, petBottom = (int)petY + PET_SPRITE_HEIGHT;

    for (int i = 0; i < 4; i++) {
      int wallLeft = (int)walls[i].x;
      int wallRight = wallLeft + WALL_WIDTH;
      
      // Check horizontal overlap
      if (petRight > wallLeft && petLeft < wallRight) {
        // Check lower wall collision (if lower wall exists)
        if (walls[i].wallHeight > 0) {
          int lowerWallTop = 64 - walls[i].wallHeight;  // Wall starts from bottom, extends upward
          if (petBottom > lowerWallTop) {  // Pet's bottom overlaps with lower wall top
            gameOver = true;
            break;
          }
        }
        
        // Check top wall collision (if top wall exists)
        if (walls[i].topWallHeight > 0) {
          int topWallBottom = walls[i].topWallHeight;  // Top wall extends from top (y=0) downward
          if (petTop < topWallBottom) {  // Pet's top overlaps with top wall bottom
            gameOver = true;
            break;
          }
        }
      }
    }
    if (gameOver) break;

    // ---- RENDER - Draw lower walls and top walls (if present) ----
    display.clear();
    for (int i = 0; i < 4; i++) {
      int wallX = (int)walls[i].x;
      if (wallX >= -WALL_WIDTH && wallX < 128) {
        // Draw lower wall from bottom upward (wall extends from y = 64 - wallHeight to y = 64)
        int wallTopY = 64 - walls[i].wallHeight;
        display.fillRect(wallX, wallTopY, WALL_WIDTH, walls[i].wallHeight);
        
        // Draw top wall if present (wall extends from y = 0 to y = topWallHeight)
        if (walls[i].topWallHeight > 0) {
          display.fillRect(wallX, 0, WALL_WIDTH, walls[i].topWallHeight);
        }
      }
    }

    // Always use cat sprite in game regardless of pet type
    // Use dedicated 16x16 game character (same for all pet types)
    const uint8_t* petSprite = game_character_bitmap;
    int drawY = (int)petY;
    if (drawY < 0) drawY = 0;
    if (drawY + PET_SPRITE_HEIGHT > 64) drawY = 64 - PET_SPRITE_HEIGHT;
    display.drawXbm(PET_X, drawY, PET_SPRITE_WIDTH, PET_SPRITE_HEIGHT, petSprite);

    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Score: " + String(score));
    display.display();

    esp_task_wdt_reset(); // Feed watchdog during game loop
    delay(16); // ~60 FPS
  }
  
  // Base happiness reward for playing the game (always get +10 for playing)
  int happinessIncrease = 10;
  Serial.println("Game played! Score: " + String(score) + " Base happiness: +10");
  
  // Apply base happiness increase
  int oldHappiness = petStats.happiness;
  petStats.happiness = min(100, petStats.happiness + happinessIncrease);
  Serial.println("Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness));
  // Stats are saved once below, after any leaderboard bonus is applied.
  
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 15, "Game Over!");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 35, "Score: " + String(score));
  if (happinessIncrease > 0) {
    display.drawString(64, 48, "Happiness +" + String(happinessIncrease));
  } else {
    display.drawString(64, 48, "Try again!");
  }
  display.display();

  playGameOverWomp();
  flashRgbLed(4, 90, 55);

  GameScoreResponse leaderboardResponse;
  bool leaderboardSuccess = false;

  #ifdef WOKWI_SIMULATOR
  // Simulator runs must be deterministic and never write test scores to the
  // production service.
  leaderboardSuccess = false;
  #else
  if (ganamosConfig.deviceId.length() > 0) {
    leaderboardSuccess = submitGameScore(score, leaderboardResponse);
  } else {
    Serial.println("Skipping leaderboard submission - missing deviceId");
  }
  #endif

  // Check if score made it into top 5 global leaderboard
  if (leaderboardSuccess && leaderboardResponse.isNewHighScore) {
    Serial.println("🎯 Made it into top 5! Rank #" + String(leaderboardResponse.currentScoreRank));
    
    // Bonus happiness for making top 5 (+20, total +30 with base)
    oldHappiness = petStats.happiness;
    petStats.happiness = min(100, petStats.happiness + 20);
    Serial.println("🎉 Top 5 bonus! Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness) + " (+20 bonus)");
    
    playHighScoreCelebrationTone();
    flashRgbLed(6, 120, 60);

    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 10, "New High Score!");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 32, "Score: " + String(score));
    if (leaderboardResponse.currentScoreRank > 0) {
      display.drawString(64, 46, "Rank #" + String(leaderboardResponse.currentScoreRank));
    }
    display.display();
    delay(2000);

    renderLeaderboard(display, leaderboardResponse);
    delay(4000);
  } 
  // Check if it's a new personal best (but not in top 5)
  else if (leaderboardSuccess && leaderboardResponse.isPersonalBest) {
    Serial.println("🎯 New personal best!");
    
    // Bonus happiness for personal best (+20, total +30 with base)
    oldHappiness = petStats.happiness;
    petStats.happiness = min(100, petStats.happiness + 20);
    Serial.println("🎉 Personal best bonus! Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness) + " (+20 bonus)");
    
    playHighScoreCelebrationTone();
    flashRgbLed(3, 120, 60);

    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 10, "Personal Best!");
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 32, "Score: " + String(score));
    display.display();
    delay(2000);
  } 
  else if (!leaderboardSuccess) {
    Serial.println("Leaderboard submission failed after game over.");
    delay(1200);
  } else {
    delay(1500);
  }

  // Single NVS write for the base reward plus any leaderboard bonus.
  savePetStats();

  int batteryPct = getBatteryPercentage();
  renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
  digitalWrite(RGB_LED, LOW);

  return happinessIncrease;
}

// Pet stats persistence (single checksummed NVS record)
static constexpr uint32_t PET_STATE_MAGIC = 0x50545632; // "PTV2"
static constexpr uint16_t PET_STATE_VERSION = 2;

struct PetStateV2 {
  uint32_t magic;
  uint16_t version;
  int16_t happiness;
  int16_t fullness;
  uint32_t savedEpoch;
  uint32_t checksum;
};

static uint32_t petStateChecksum(const PetStateV2& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < sizeof(state) - sizeof(state.checksum); ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

void loadPetStats() {
  extern Preferences preferences;
  preferences.begin("satoshi-pet", true);

  PetStateV2 state = {};
  const size_t stateLength = preferences.getBytesLength("petStateV2");
  const bool validState = stateLength == sizeof(state) &&
                          preferences.getBytes("petStateV2", &state, sizeof(state)) == sizeof(state) &&
                          state.magic == PET_STATE_MAGIC &&
                          state.version == PET_STATE_VERSION &&
                          state.happiness >= 0 && state.happiness <= 100 &&
                          state.fullness >= 0 && state.fullness <= 100 &&
                          state.checksum == petStateChecksum(state);

  uint32_t lastSaveEpoch = 0;
  if (validState) {
    petStats.happiness = state.happiness;
    petStats.fullness = state.fullness;
    lastSaveEpoch = state.savedEpoch;
  } else if (stateLength == 0) {
    // One-time migration from the original independent keys.
    petStats.happiness = constrain(preferences.getInt("happiness", 50), 0, 100);
    petStats.fullness = constrain(preferences.getInt("fullness", 50), 0, 100);
    lastSaveEpoch = (uint32_t)preferences.getULong("lastSaveEpoch", 0);
  } else {
    // A partial/corrupt state must not create impossible pet values.
    petStats.happiness = 50;
    petStats.fullness = 50;
    Serial.println("Pet stats: corrupt state rejected; using safe defaults");
  }

  preferences.end();

  // Remember the saved epoch; the offline gap is applied once the wall clock
  // is valid (usually shortly after NTP sync, see updatePetStats()).
  petBaseEpoch = (lastSaveEpoch > (uint32_t)MIN_VALID_EPOCH) ? lastSaveEpoch : 0;
  petBaseMillis = millis();
  offlineDecayPending = petBaseEpoch != 0;

  Serial.println("Loaded pet stats: happiness " + String(petStats.happiness) +
                 ", fullness " + String(petStats.fullness) +
                 ", saved at " + String((unsigned long)lastSaveEpoch));

  bool needSave = !validState;
  if (tryApplyOfflineDecay()) needSave = true;
  if (offlineDecayPending) {
    Serial.println("Pet stats: clock not synced yet - offline decay deferred");
  }
  if (needSave) savePetStats();
}

void savePetStats() {
  extern Preferences preferences;

  // Real time when known; otherwise the loaded epoch advanced by uptime, so an
  // unsynced clock never erases the saved timestamp.
  const uint32_t epoch = accountedEpoch();

  PetStateV2 state = {};
  state.magic = PET_STATE_MAGIC;
  state.version = PET_STATE_VERSION;
  state.happiness = constrain(petStats.happiness, 0, 100);
  state.fullness = constrain(petStats.fullness, 0, 100);
  state.savedEpoch = epoch;
  state.checksum = petStateChecksum(state);

  preferences.begin("satoshi-pet", false);
  const size_t written = preferences.putBytes("petStateV2", &state, sizeof(state));
  preferences.end();

  if (written != sizeof(state)) Serial.println("Pet stats: incomplete NVS write");
  
  Serial.println("Saved pet stats: happiness " + String(petStats.happiness) +
                 ", fullness " + String(petStats.fullness) +
                 ", epoch " + String((unsigned long)epoch));
}

void applyTimeBasedDecay() {
  updatePetStats(0, 0);
}
