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
  unsigned long lastUpdate;  // millis() of the last in-session decay step
  unsigned long lastActivity; // millis() of the last game played (written by the .ino; not read)
};

// Pet species, resolved once from ganamosConfig.petType ("bunny" and "rabbit"
// share PET_BUNNY; unknown types fall back to PET_CAT).
enum PetKind : uint8_t {
  PET_CAT = 0,
  PET_DOG,
  PET_SQUIRREL,
  PET_TURTLE,
  PET_BUNNY,
  PET_OWL,
  PET_KIND_COUNT
};
PetKind petKindFromString(const String& petType);
extern PetStats petStats;
extern bool showCelebration;
extern bool showNewJobNotification;
extern unsigned long newJobNotificationStart;
extern const unsigned long NEW_JOB_NOTIFICATION_DURATION;
extern bool showRejection;
extern unsigned long rejectionStart;
extern String rejectionMessage;

void updatePetStats(int newBalance, int oldBalance);

void initPet(String petType);
void updatePetMood(int price, int sats); // no-op, kept for the .ino call site
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
