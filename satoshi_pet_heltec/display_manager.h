#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "HT_SSD1306Wire.h"
#include "config.h"
#include "pet_blob.h"

// Display brightness levels
extern uint8_t NORMAL_BRIGHTNESS;
extern uint8_t DIM_BRIGHTNESS;

// Celebration state (managed by pet_blob, rendered here)
extern bool showCelebration;
extern unsigned long celebrationStart;
extern int celebrationFrame;
extern int satsEarned;
extern String celebrationMessage;

// Set OLED contrast
void setDisplayContrast(SSD1306Wire& display, uint8_t contrast);

// Main screens
void renderPet(SSD1306Wire& display, int btcPrice, int satoshis, int batteryPercent);
void renderScreensaver(SSD1306Wire& display, int satoshis);
void renderCelebration(SSD1306Wire& display);

// Menu and navigation
void renderMenu(SSD1306Wire& display, int menuOption);

// Food system
void renderFoodSelectionMenu(SSD1306Wire& display, int selectedFood);
void renderFeedResult(SSD1306Wire& display, bool success, int foodIndex, bool hadEnoughCoins);

// Game
void renderGameScreen(SSD1306Wire& display, int round, int score, unsigned long gameStartTime);
void renderLeaderboard(SSD1306Wire& display, const GameScoreResponse& response);

// Pairing
void renderPairingScreen(SSD1306Wire& display, const String& pairingCode);

// Factory reset
void renderFactoryResetConfirm(SSD1306Wire& display, int selectedOption);

// Helper: Get pet sprite (uses pet_sprites_simple.h)
#include "pet_sprites_simple.h"

#endif

