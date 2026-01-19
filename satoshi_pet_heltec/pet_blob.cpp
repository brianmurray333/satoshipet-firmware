#ifndef BUTTON_PIN_PRG
#define BUTTON_PIN_PRG 0      // Heltec KEY button (PRG) on GPIO0
#endif
#ifndef BUTTON_PIN_EXTERNAL
#define BUTTON_PIN_EXTERNAL 2 // External button (active LOW with pull-up)
#endif

#include <heltec.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include "pet_blob.h"
#include "config.h"
#include <WiFi.h>
#include "pet_sprites_simple.h"
#include "display_assets.h"
#include "food_bitmaps.h"
// Removed unused animation variables 

// Forward declarations
void renderJobsList(SSD1306Wire &display, int selectedJob, int scrollOffset);
void renderJobDetail(SSD1306Wire &display, int jobIndex);
void handleJobsMenu(SSD1306Wire &display);

extern GanamosConfig ganamosConfig;

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

   const uint8_t* getCurrentPetSprite(String animation) {
     return getPetSprite(ganamosConfig.petType, ANIM_DEFAULT, 0); // Use default animation frame 0 for static contexts
   }

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

// Get the appropriate food options array based on pet type
static const FoodOption* getPetFoodOptions() {
  extern GanamosConfig ganamosConfig;
  
  if (ganamosConfig.petType == "cat") {
    return CAT_FOOD_OPTIONS;
  } else if (ganamosConfig.petType == "dog") {
    return DOG_FOOD_OPTIONS;
  } else if (ganamosConfig.petType == "rabbit") {
    return RABBIT_FOOD_OPTIONS;
  } else if (ganamosConfig.petType == "squirrel") {
    return SQUIRREL_FOOD_OPTIONS;
  } else if (ganamosConfig.petType == "turtle") {
    return TURTLE_FOOD_OPTIONS;
  } else {
    // Default to cat food if pet type is unknown
    return CAT_FOOD_OPTIONS;
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
  satsEarned = earnedSats;
  celebrationMessage = message;
  Serial.println("🎉 CELEBRATION! Earned " + String(earnedSats) + " sats!");
  if (message.length() > 0) {
    Serial.println("Message: " + message);
  }
  
  // Reset screensaver timer on celebration (keep display bright during activity)
  extern unsigned long lastButtonPress;
  extern bool isScreensaverActive;
  extern uint8_t NORMAL_BRIGHTNESS;
  extern SSD1306Wire display;
  
  lastButtonPress = millis();
  extern bool isDisplayOff;
  if (isScreensaverActive) {
    isScreensaverActive = false;
    if (isDisplayOff) {
      // Display wake-up is handled in main loop (VextON + display.init)
      Serial.println("👁️ Waking from display OFF for celebration");
    } else {
      Serial.println("👁️ Waking from animated screensaver for celebration");
    }
    isDisplayOff = false;
  }
  
  // Play the celebration sound!
  playSatsEarnedSound();
}

void triggerNewJobNotification(String title, int reward) {
  showNewJobNotification = true;
  newJobNotificationStart = millis();
  newJobTitle = title;
  newJobReward = reward;
  
  Serial.println("📋 NEW JOB! " + title + " (" + String(reward) + " sats)");
  
  // Reset screensaver timer and wake display
  extern unsigned long lastButtonPress;
  extern bool isScreensaverActive;
  extern bool isDisplayOff;
  extern bool isBitcoinFactsActive;
  extern SSD1306Wire display;
  
  lastButtonPress = millis();
  
  if (isScreensaverActive || isDisplayOff || isBitcoinFactsActive) {
    isScreensaverActive = false;
    isBitcoinFactsActive = false;
    if (isDisplayOff) {
      extern void VextON(void);
      VextON();
      delay(100);
      display.init();
      Serial.println("👁️ Waking from display OFF for new job notification");
    }
    isDisplayOff = false;
  }
  
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
  rejectionMessage = message;
  Serial.println("❌ FIX REJECTED! " + message);
  
  // Reset screensaver timer
  extern unsigned long lastButtonPress;
  extern bool isScreensaverActive;
  extern uint8_t NORMAL_BRIGHTNESS;
  
  lastButtonPress = millis();
  if (isScreensaverActive) {
    isScreensaverActive = false;
    extern SSD1306Wire display;
    extern void setOLEDContrast(uint8_t);
    setOLEDContrast(NORMAL_BRIGHTNESS);
  }
  
  // Play the rejection sound
  playFixRejectedSound();
}

void renderRejection(SSD1306Wire &display) {
  unsigned long elapsed = millis() - rejectionStart;
  
  if (elapsed > REJECTION_DURATION) {
    showRejection = false;
    return;
  }
  
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Main rejection message
  display.drawString(64, 2, "FIX REJECTED");
  
  display.setFont(ArialMT_Plain_10);
  
  // Show X marks (opposite of spinning bitcoins)
  int frame = (elapsed / 150) % 2; // Blink effect
  if (frame == 0) {
    display.drawString(20, 36, "X");
    display.drawString(50, 36, "X");
    display.drawString(80, 36, "X");
    display.drawString(108, 36, "X");
  }
  
  // Show message or default
  String displayMsg = rejectionMessage;
  if (displayMsg.length() == 0) {
    displayMsg = "Try again!";
  }
  if (displayMsg.length() > 18) {
    displayMsg = displayMsg.substring(0, 18) + "...";
  }
  display.drawString(64, 50, displayMsg);
  
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
  
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Draw pet sprite centered and moved up
  int spriteFrame = (screensaverFrame / 2) % 3;
  const uint8_t* sprite = getPetSprite(ganamosConfig.petType, ANIM_DEFAULT, spriteFrame);
  
  // Handle bunny's different dimensions (default is 50x50)
  int spriteWidth = (ganamosConfig.petType == "bunny" || ganamosConfig.petType == "rabbit") ? BUNNY_DEFAULT_WIDTH : SPRITE_WIDTH;
  int spriteHeight = (ganamosConfig.petType == "bunny" || ganamosConfig.petType == "rabbit") ? BUNNY_DEFAULT_HEIGHT : SPRITE_HEIGHT;
  
  display.drawXbm(34, 6, spriteWidth, spriteHeight, sprite);
  
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
  
  // Update animation frame every 150ms for smoother spinning bitcoins
  if (now - celebrationStart > (celebrationFrame * 150)) {
    celebrationFrame++;
  }
  
  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Main celebration message - moved up from y=10 to y=2
  display.drawString(64, 2, "SATS EARNED!");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 22, "+" + String(satsEarned) + " sats");
  
  // Spinning Bitcoin logos - moved up from y=45 to y=36
  int frame = (celebrationFrame / 2) % 4;
  display.drawXbm(20, 36, 8, 8, bitcoin_spin_frames[frame]);
  display.drawXbm(50, 36, 8, 8, bitcoin_spin_frames[(frame + 1) % 4]);
  display.drawXbm(80, 36, 8, 8, bitcoin_spin_frames[(frame + 2) % 4]);
  display.drawXbm(110, 36, 8, 8, bitcoin_spin_frames[(frame + 3) % 4]);
  
  // Display sender's message if available, otherwise default message - moved up from y=55 to y=48
  String displayMsg = celebrationMessage;
  if (displayMsg.length() == 0 || displayMsg == "null") {
    displayMsg = ganamosConfig.petName + " is happy!";
  }
  // Truncate long messages to fit on screen
  if (displayMsg.length() > 21) {
    displayMsg = displayMsg.substring(0, 18) + "...";
  }
  display.drawString(64, 48, displayMsg);
  
  display.display();
}

void initPet(String petType) {
  // Initialize pet stats based on type
  petStats.happiness = 50;  // Start at neutral happiness
  petStats.fullness = 50;   // Start at neutral fullness
  petStats.age = 0;
  petStats.sleeping = false;
  petStats.lastFeed = millis();
  petStats.lastActivity = millis();
  petStats.lastUpdate = millis();  // Initialize entropy tracking
  
  Serial.println("Initialized " + petType + " pet with default stats");
}

void updatePetMood(int price, int sats) {
  // This function is now handled by updatePetStats()
  // Price changes don't affect pet mood anymore (removed happiness)
  static int lastPrice = price;
  lastPrice = price;
}

PetStats petStats = {50, 50, 0, false, 0, 0, 0}; // happiness, fullness, age, sleeping, lastFeed, lastActivity, lastUpdate (7 values total)

void updatePetStats(int newBalance, int oldBalance) {
  unsigned long now = millis();
  
  // Note: Earning sats no longer automatically feeds pet
  // Pet feeding must be done manually via menu
  
  // Entropy/decay over time: fullness decreases, happiness decreases
  // Update every 5 minutes (300000ms) for smooth decay
  if (now - petStats.lastUpdate > 300000) { // 5 minutes
    unsigned long elapsedMinutes = (now - petStats.lastUpdate) / 60000;
    
    int oldFullness = petStats.fullness;
    int oldHappiness = petStats.happiness;
    
    // Fullness decreases over time (0.05 per minute = 72 points/day)
    petStats.fullness = max(0, petStats.fullness - (int)(elapsedMinutes * 0.05));
    
    // Happiness decreases over time (0.05 per minute = 72 points/day)
    petStats.happiness = max(0, petStats.happiness - (int)(elapsedMinutes * 0.05));
    
    // If fullness is low, happiness decreases faster
    if (petStats.fullness < 30) {
      petStats.happiness = max(0, petStats.happiness - (int)(elapsedMinutes * 0.05));
    }
    
    petStats.lastUpdate = now;
    
    // Save pet stats to flash if they changed due to entropy
    if (oldFullness != petStats.fullness || oldHappiness != petStats.happiness) {
      savePetStats();
    }
  }
  
  // Sleep cycle (pet sleeps at night using real time)
  // Use 10ms timeout to prevent blocking when WiFi/NTP unavailable
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {  // 10ms timeout instead of default 5000ms!
    int hour = timeinfo.tm_hour;
    petStats.sleeping = (hour >= 22 || hour <= 6);
  } else {
    // If time not available, don't change sleep state
    petStats.sleeping = false;
  }
}

String getPetAnimation() {
  if (petStats.sleeping) return "sleeping";
  if (petStats.fullness < 20) return "hungry"; // Low fullness (starving)
  if (petStats.happiness > 70 && petStats.fullness > 70) return "happy"; // Very happy and well-fed
  if (petStats.happiness < 30 || petStats.fullness < 30) return "sick";  // Unhappy or very empty
  return "idle";
}

void renderPet(SSD1306Wire &display, int btcPrice, int satoshis, int batteryPercent) {
  unsigned long funcStart = millis();
  static int oldBalance = -1; // Initialize to -1 to detect first balance
  static bool firstRun = true;
  static bool firstSyncAfterBoot = true; // Skip celebration on first sync after boot
  static unsigned long lastFrameUpdate = 0;
  static int currentAnimFrame = 0;
  
  // On first run, load the last known balance from flash to prevent false celebrations
  if (firstRun) {
    extern int loadLastKnownBalance();
    oldBalance = loadLastKnownBalance();
    Serial.println("Loaded last known balance from flash: " + String(oldBalance) + " sats");
    firstRun = false;
  }
  
  // On first sync after boot: silently update oldBalance to current balance without celebration
  // This handles the case where the device was off and balance changed - we don't want to celebrate that
  if (firstSyncAfterBoot) {
    // Only update if we actually got a new balance from API (satoshis > 0)
    // If satoshis is still 0, it means API hasn't fetched yet - don't update oldBalance
    if (satoshis > 0) {
      Serial.println("📊 First sync after boot - Balance: " + String(satoshis) + " sats (no celebration)");
      Serial.println("📊 Previous balance was: " + String(oldBalance) + " sats");
      int previousBalance = oldBalance; // Save previous balance before updating
      
      // Only save to flash if we have a valid balance from API
      extern void saveLastKnownBalance(int balance);
      extern void saveLastKnownCoins(int coins);
      saveLastKnownBalance(satoshis);
      extern int getLocalCoins();
      saveLastKnownCoins(getLocalCoins());
      Serial.println("💾 Saved balance and coins to flash: " + String(satoshis) + " sats, " + String(getLocalCoins()) + " coins");
      
      // Update pet stats with the actual previous balance (but don't celebrate)
      // We pass the previous balance so stats update correctly, but no celebration happens
      updatePetStats(satoshis, previousBalance);
      
      // Set oldBalance to current balance and mark first sync as complete
      oldBalance = satoshis;
      firstSyncAfterBoot = false;
    }
    // If satoshis is 0, keep firstSyncAfterBoot = true and use oldBalance from flash
    // Skip celebration check for first sync - just render the display
    // Continue to render below
  } else {
    // After first sync is complete, check for actual sats earned
    if (satoshis > oldBalance && oldBalance >= 0) {
      int earned = satoshis - oldBalance;
      // Normal operation - celebrate the earnings!
      Serial.println("🎉 BALANCE INCREASED! Old: " + String(oldBalance) + " New: " + String(satoshis) + " Earned: " + String(earned));
      triggerCelebration(earned, ganamosConfig.lastMessage);
    }
    
    // ALWAYS save the current balance and coins to flash (not just on increases)
    // This ensures we have the latest values even if device powers off
    if (satoshis != oldBalance) {
      extern void saveLastKnownBalance(int balance);
      extern void saveLastKnownCoins(int coins);
      extern int getLocalCoins();
      saveLastKnownBalance(satoshis);
      saveLastKnownCoins(getLocalCoins());
      Serial.println("💾 Saved balance and coins to flash: " + String(satoshis) + " sats, " + String(getLocalCoins()) + " coins");
    }
    
    updatePetStats(satoshis, oldBalance);
    oldBalance = satoshis;
  }
  
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
      int maxFrames = (ganamosConfig.petType == "cat") ? CAT_EAT_FRAMES :
                      (ganamosConfig.petType == "dog") ? DOG_EAT_FRAMES :
                      (ganamosConfig.petType == "squirrel") ? SQUIRREL_EAT_FRAMES :
                      (ganamosConfig.petType == "turtle") ? TURTLE_EAT_FRAMES :
                      (ganamosConfig.petType == "rabbit" || ganamosConfig.petType == "bunny") ? BUNNY_EAT_FRAMES : 8;
      
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
  display.drawString(0, 0, ganamosConfig.petName);
  
  // // Top right: Lightning bolt icon + Battery percentage (above pet sprite which starts at y=12)
  // // Calculate position: text is right-aligned, icon is to the left of text
  // String batteryText = String(batteryPercent) + "%";
  // int textWidth = batteryText.length() * 6;  // Approximate width (6px per char in ArialMT_Plain_10)
  // int iconWidth = 8;
  // int gap = 2;  // 2px gap between icon and text
  // int totalWidth = iconWidth + gap + textWidth;
  // int startX = 128 - totalWidth;

  //   // Draw lightning bolt icon (8x10 pixels)
  // display.drawXbm(startX, 0, 8, 10, epd_bitmap_bolt);
  
  // // Draw battery percentage after icon
  // display.setTextAlignment(TEXT_ALIGN_LEFT);
  // display.drawString(startX + iconWidth + gap, 0, batteryText);

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

  // Right side: Draw pet sprite - position varies by pet type (60x51px, bunny varies)
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Get the appropriate sprite for the pet type with animation frame
  const uint8_t* sprite = getPetSprite(ganamosConfig.petType, currentAnimState, currentAnimFrame);
  
    // Determine sprite dimensions based on pet type and animation state
  int spriteWidth, spriteHeight, spriteX;
  if (ganamosConfig.petType == "bunny" || ganamosConfig.petType == "rabbit") {
    // Bunny: default is 50x50, eat/sad/die are 55x55
    if (currentAnimState == ANIM_DEFAULT) {
      spriteWidth = BUNNY_DEFAULT_WIDTH;   // 50
      spriteHeight = BUNNY_DEFAULT_HEIGHT; // 50
    } else {
      spriteWidth = BUNNY_SPRITE_WIDTH;   // 55
      spriteHeight = BUNNY_SPRITE_HEIGHT; // 55
    }
    spriteX = 64; // Bunny position (moved 8px left from 72)
  } else if (ganamosConfig.petType == "owl") {
    // Owl: all animations are 50x50
    spriteWidth = 50;
    spriteHeight = 50;
    spriteX = 64; // Same position as bunny (fits similar size)
  } else {
    // Cat, dog, squirrel, turtle are all 60x51
    spriteWidth = SPRITE_WIDTH;   // 60
    spriteHeight = SPRITE_HEIGHT; // 51
    spriteX = 54; // Other pets position (moved 8px left from 62)
  }
  
  display.drawXbm(spriteX, 12, spriteWidth, spriteHeight, sprite);

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
    {8, 12},   // 0: Home (top-left)
    {68, 12},  // 1: Play (top-right)
    {8, 36},   // 2: Feed (bottom-left)
    {68, 36}   // 3: Jobs (bottom-right)
  };
  
  for (int i = 0; i < 4; i++) {
    String line = (i == menuOption) ? "> " : "  ";
    line += options[i];
    display.drawString(positions[i][0], positions[i][1], line);
  }
  
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
const int MAX_TITLE_CHARS = 20;    // Characters that fit without scrolling

// Format date string from ISO to "Dec 7" format
String formatJobDate(String isoDate) {
  if (isoDate.length() < 10) return isoDate;
  
  // Parse month and day from "2025-12-07T..."
  int month = isoDate.substring(5, 7).toInt();
  int day = isoDate.substring(8, 10).toInt();
  
  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  
  if (month >= 1 && month <= 12) {
    return String(months[month - 1]) + " " + String(day);
  }
  return isoDate.substring(5, 10);
}

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
  const int maxTitleChars = 15;  // Chars before truncation
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
    if (isSelected && title.length() > maxTitleChars) {
      unsigned long now = millis();
      
      // Reset marquee when selection changes
      if (lastSelectedJob != selectedJob) {
        jobsMarqueeOffset = 0;
        lastMarqueeUpdate = now;
        lastSelectedJob = selectedJob;
      }
      
      // Calculate visible portion with scrolling
      int maxScroll = title.length() - maxTitleChars + 3; // +3 for "..." effect
      
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
      title = title.substring(startChar, startChar + maxTitleChars);
    } else if (title.length() > maxTitleChars) {
      // Non-selected long title: truncate with ellipsis
      title = title.substring(0, maxTitleChars - 2) + "..";
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
  
  // Location and date on bottom line
  display.setFont(ArialMT_Plain_10);
  String location = job.location;
  if (location.length() > 14) {
    location = location.substring(0, 12) + "..";
  }
  String dateStr = formatJobDate(job.createdAt);
  // Show location/date OR instruction (alternate or just show instruction)
  display.drawString(0, 54, "Press:Back  Hold:Done");  
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
  bool buttonPressed = false;
  unsigned long buttonPressTime = 0;
  
  // Reset marquee state
  jobsMarqueeOffset = 0;
  lastMarqueeUpdate = millis();
  lastSelectedJob = -1;
  
  renderJobsList(display, selectedJob, scrollOffset);
  
  // Button handling constants (from main sketch)
  const unsigned long SHORT_PRESS_MAX = 600;
  const unsigned long HOLD_PRESS_MIN = 700;
  
  while (inJobsMenu) {
    esp_task_wdt_reset();  
    unsigned long now = millis();
    
    // Timeout after 30 seconds
    if (now - lastJobCycle > 30000) {
      inJobsMenu = false;
      break;
    }
    
    // Read buttons
    bool prgState = digitalRead(0) == LOW;      // BUTTON_PIN_PRG
    bool externalState = digitalRead(2) == LOW;  // BUTTON_PIN_EXTERNAL
    bool currentState = prgState || externalState;
    
    if (currentState && !buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
    }
    
    if (!currentState && buttonPressed) {
      unsigned long pressDuration = millis() - buttonPressTime;
      buttonPressed = false;
      lastJobCycle = millis();
      
      bool isShort = pressDuration <= SHORT_PRESS_MAX;
      bool isHold = pressDuration >= HOLD_PRESS_MIN;
      
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
        
        // Wait for button press in detail view
        // Short press = back to list
        // Long press = mark as complete
        delay(300);  // Debounce
        bool detailButtonPressed = false;
        unsigned long detailButtonPressTime = 0;
        unsigned long detailStart = millis();
        bool inDetailView = true;
        
        while (inDetailView && (millis() - detailStart < 30000)) {  // 30s timeout
          esp_task_wdt_reset();
          bool detailPrgState = digitalRead(0) == LOW;
          bool detailExtState = digitalRead(2) == LOW;
          bool detailState = detailPrgState || detailExtState;
          
          if (detailState && !detailButtonPressed) {
            detailButtonPressed = true;
            detailButtonPressTime = millis();
          }
          
          if (!detailState && detailButtonPressed) {
            unsigned long detailPressDuration = millis() - detailButtonPressTime;
            detailButtonPressed = false;
            
            if (detailPressDuration >= HOLD_PRESS_MIN) {
              // Long press - show confirmation for marking complete
              playMenuSelectTone();
              
              // Show confirmation screen
              display.clear();
              display.setFont(ArialMT_Plain_16);
              display.setTextAlignment(TEXT_ALIGN_CENTER);
              display.drawString(64, 5, "Mark Done?");
              
              display.setFont(ArialMT_Plain_10);
              display.drawString(64, 28, "Press: Yes");
              display.drawString(64, 42, "Hold: Cancel");
              display.display();
              
              // Wait for confirmation
              delay(300);
              bool confirmPressed = false;
              unsigned long confirmPressTime = 0;
              unsigned long confirmStart = millis();
              
              while (millis() - confirmStart < 10000) {  // 10s timeout for confirm
                esp_task_wdt_reset(); 
                bool confirmPrgState = digitalRead(0) == LOW;
                bool confirmExtState = digitalRead(2) == LOW;
                bool confirmState = confirmPrgState || confirmExtState;
                
                if (confirmState && !confirmPressed) {
                  confirmPressed = true;
                  confirmPressTime = millis();
                }
                
                if (!confirmState && confirmPressed) {
                  unsigned long confirmDuration = millis() - confirmPressTime;
                  confirmPressed = false;
                  
                  if (confirmDuration < SHORT_PRESS_MAX) {
                    // Short press = Confirm!
                    playMenuSelectTone();
                    
                    // Show "Sending..." screen
                    display.clear();
                    display.setFont(ArialMT_Plain_10);
                    display.setTextAlignment(TEXT_ALIGN_CENTER);
                    display.drawString(64, 25, "Notifying poster...");
                    display.display();
                    
                    esp_task_wdt_reset();

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
    if (cachedJobCount > 0 && cachedJobs[selectedJob].title.length() > MAX_TITLE_CHARS) {
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
      int deficit = option.coinCost - localCoins;
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

static void playHighScoreCelebrationTone() {
  int melody[] = { 988, 1175, 1319, 1568 };
  int durations[] = { 120, 120, 150, 220 };

  for (int i = 0; i < 4; i++) {
    tone(BUZZER_PIN, melody[i], durations[i]);
    delay(durations[i] + 40);
    noTone(BUZZER_PIN);
  }
}

static void playGameOverWomp() {
  int melody[] = { 392, 330, 262 };
  int durations[] = { 200, 220, 300 };

  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, melody[i], durations[i]);
    delay(durations[i] + 60);
    noTone(BUZZER_PIN);
  }
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
  extern GanamosConfig ganamosConfig;
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
    petStats.lastFeed = millis();
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

void renderGameScreen(SSD1306Wire &display, int round, int score, unsigned long gameStartTime) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  
  // Game title
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 0, "Lightning!");
  
  display.setFont(ArialMT_Plain_10);
  
  // Show round info
  display.drawString(64, 20, "Round " + String(round) + "/3");
  display.drawString(64, 30, "Score: " + String(score));
  
  // Show lightning bolt (random position based on seed) - only if boltVisible
  // Note: boltVisible state is handled in game loop, this function just renders
  // The bolt position will be calculated consistently based on round
  
  // Instructions
  display.drawString(64, 58, "Press when bolt!");
  
  display.display();
}

struct DebouncedButton {
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
  DebouncedButton flapBtnExternal;
  flapBtnExternal.pin = BUTTON_PIN_EXTERNAL;
  flapBtnExternal.begin();

  DebouncedButton flapBtnPrg;
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
  petStats.lastActivity = millis();
  Serial.println("Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness));
  
  // Save pet stats to flash after game
  savePetStats();
  
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

  if (ganamosConfig.deviceId.length() > 0) {
    leaderboardSuccess = submitGameScore(score, leaderboardResponse);
  } else {
    Serial.println("Skipping leaderboard submission - missing deviceId");
  }

  // Check if score made it into top 5 global leaderboard
  if (leaderboardSuccess && leaderboardResponse.isNewHighScore) {
    Serial.println("🎯 Made it into top 5! Rank #" + String(leaderboardResponse.currentScoreRank));
    
    // Bonus happiness for making top 5 (+20, total +30 with base)
    oldHappiness = petStats.happiness;
    petStats.happiness = min(100, petStats.happiness + 20);
    Serial.println("🎉 Top 5 bonus! Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness) + " (+20 bonus)");
    savePetStats();
    
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
    savePetStats();
    
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

  int batteryPct = getBatteryPercentage();
  renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
  digitalWrite(RGB_LED, LOW);

  return happinessIncrease;
}

// Pet stats persistence functions (stubs - stats already managed in updatePetStats)
void loadPetStats() {
  extern Preferences preferences;
  preferences.begin("satoshi-pet", false);
  
  // Load happiness and fullness from NVS (default to 50 if not found)
  petStats.happiness = preferences.getInt("happiness", 50);
  petStats.fullness = preferences.getInt("fullness", 50);
  unsigned long lastSaveEpoch = preferences.getULong("lastSaveEpoch", 0);
  
  preferences.end();
  
  Serial.println("📊 Loaded pet stats from flash:");
  Serial.println("  Happiness: " + String(petStats.happiness));
  Serial.println("  Fullness: " + String(petStats.fullness));
  Serial.println("  Last save: " + String(lastSaveEpoch));
  
  // Apply offline decay if we have a valid timestamp and current time
  if (lastSaveEpoch > 0) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {  // 10ms timeout to prevent blocking
      time_t currentEpoch = mktime(&timeinfo);
      long elapsedSeconds = (long)(currentEpoch - lastSaveEpoch);
      
      if (elapsedSeconds > 0) {
        long elapsedMinutes = elapsedSeconds / 60;
        
        Serial.println("⏰ Device was offline for " + String(elapsedMinutes) + " minutes");
        
        // Apply fullness decay (0.05 per minute)
        int fullnessDecay = (int)(elapsedMinutes * 0.05);
        int oldFullness = petStats.fullness;
        petStats.fullness = max(0, petStats.fullness - fullnessDecay);
        
        // Apply happiness decay (0.05 per minute)
        int happinessDecay = (int)(elapsedMinutes * 0.05);
        int oldHappiness = petStats.happiness;
        petStats.happiness = max(0, petStats.happiness - happinessDecay);
        
        // If fullness is low, apply additional happiness decay
        if (petStats.fullness < 30) {
          int extraDecay = (int)(elapsedMinutes * 0.05);
          petStats.happiness = max(0, petStats.happiness - extraDecay);
          Serial.println("⚠️ Low fullness - extra happiness decay applied");
        }
        
        Serial.println("📉 Offline decay applied:");
        Serial.println("  Fullness: " + String(oldFullness) + " → " + String(petStats.fullness) + " (-" + String(fullnessDecay) + ")");
        Serial.println("  Happiness: " + String(oldHappiness) + " → " + String(petStats.happiness) + " (-" + String(happinessDecay) + ")");
        
        // Save the updated stats with current timestamp
        savePetStats();
      } else if (elapsedSeconds < 0) {
        Serial.println("⚠️ Clock went backwards! Skipping offline decay.");
      }
    } else {
      Serial.println("⚠️ Could not get current time - skipping offline decay");
    }
  } else {
    Serial.println("ℹ️ No previous timestamp found - first boot or fresh device");
  }
}

void savePetStats() {
  extern Preferences preferences;
  preferences.begin("satoshi-pet", false);
  
  // Get current epoch time (10ms timeout to prevent blocking when offline)
  struct tm timeinfo;
  time_t now = 0;
  if (getLocalTime(&timeinfo, 10)) {
    now = mktime(&timeinfo);
  }
  
  // Save happiness, fullness, and current epoch timestamp to NVS
  preferences.putInt("happiness", petStats.happiness);
  preferences.putInt("fullness", petStats.fullness);
  preferences.putULong("lastSaveEpoch", (unsigned long)now);
  
  preferences.end();
  
  Serial.println("💾 Saved pet stats to flash:");
  Serial.println("  Happiness: " + String(petStats.happiness));
  Serial.println("  Fullness: " + String(petStats.fullness));
  Serial.println("  Timestamp: " + String((unsigned long)now));
}

void applyTimeBasedDecay() {
  // Time-based decay is handled by updatePetStats when rendering
  // This stub exists for compatibility
  Serial.println("applyTimeBasedDecay() called (decay handled by updatePetStats)");
}