#ifndef PET_BLOB_H
#define PET_BLOB_H
#define RGB_LED 35  // Heltec V3 RGB LED pin
#define BUZZER_PIN 48  // Piezo buzzer pin

#include <Arduino.h>
#include "HT_SSD1306Wire.h"

// External functions for sounds (defined in main .ino file)
extern void playSatsEarnedSound();
extern void playNewJobChirp();
extern void playFixRejectedSound();

struct PetStats {
  int happiness;     // 0-100 (0 = sad, 100 = very happy)
  int fullness;      // 0-100 (0 = starving, 100 = full)
  int age;           // in hours
  bool sleeping;     // sleep cycle
  unsigned long lastFeed;
  unsigned long lastActivity;
  unsigned long lastUpdate;  // For entropy/decay calculations
  
  // Time-based decay tracking (milliseconds since boot)
  unsigned long lastFeedTimestamp;    // When last fed
  unsigned long lastPlayTimestamp;    // When last played game
  unsigned long lastDecayUpdate;      // When decay was last calculated
  
  // Real-world timestamps (Unix epoch) for persistence across reboots
  time_t lastFeedEpoch;
  time_t lastPlayEpoch;
};
extern PetStats petStats;
extern bool showCelebration;
extern bool showNewJobNotification;
extern unsigned long newJobNotificationStart;
extern const unsigned long NEW_JOB_NOTIFICATION_DURATION;
extern bool showRejection;
extern unsigned long rejectionStart;
extern String rejectionMessage;

void updatePetStats(int newBalance, int oldBalance);
String getPetAnimation();
const uint8_t* getCurrentPetSprite(String animation);

void initPet(String petType);
void updatePetMood(int price, int sats);
void applyTimeBasedDecay(); // Apply fullness/happiness decay based on elapsed time
void savePetStats(); // Save stats to NVS
void loadPetStats(); // Load stats from NVS
void renderPet(SSD1306Wire &display, int btcPrice, int satoshis, int batteryPercent);
void renderScreensaver(SSD1306Wire &display, int satoshis);
void renderMenu(SSD1306Wire &display, int menuOption); // 0=Home, 1=Play, 2=Feed
void renderFeedResult(SSD1306Wire &display, bool success, int foodIndex, bool hadEnoughCoins);
void renderFoodSelectionMenu(SSD1306Wire &display, int selectedFood); // 0=Lettuce, 1=Eggs, 2=Steak
bool handleFeedPet(int foodIndex);
int getFoodCostByIndex(int foodIndex);
int getFoodFullnessPercent(int foodIndex);
const char* getFoodNameByIndex(int foodIndex);
int getFoodOptionCount();
int handleLightningGame(SSD1306Wire &display); // Returns happiness increase (0 if failed)

// Jobs feature
void handleJobsMenu(SSD1306Wire &display);
void triggerNewJobNotification(String title, int reward);
void renderNewJobNotification(SSD1306Wire &display);
void triggerRejection(String message);
void renderRejection(SSD1306Wire &display);

// External battery functions (defined in main .ino file)
extern int getBatteryPercentage();
extern int getBatteryLevel();
extern bool isBatteryCharging();
extern float getBatteryVoltage();

#endif