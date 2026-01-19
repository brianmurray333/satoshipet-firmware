  #include <Wire.h>
  #include "HT_SSD1306Wire.h"
  #include <WiFiManager.h>
  #include <HTTPClient.h>
  #include <ArduinoJson.h>
  #include <time.h>
  #include "config.h"
  #include "pet_blob.h"
  #include "economy.h"
  #include "button_handler.h"
  #include "display_assets.h"
  #include <esp_task_wdt.h>  // Watchdog timer support (framework auto-initializes)

  // Debug logging - comment out to disable verbose logs and save memory
  // #define DEBUG_LOGGING

  #define Vext 21
  #define BUTTON_PIN_PRG 0      // PRG button
  #define BUTTON_PIN_EXTERNAL 2 // External button
  #define RGB_LED 35            // Heltec V3 RGB LED pin
  #define BUZZER_PIN 48         // Piezo buzzer pin
  #define ADC_CTRL 37           // GPIO37 - Control pin to enable battery voltage divider
  #define VBAT_PIN 1            // GPIO1 - Battery voltage ADC reading pin (ADC1_CH0)
  // Heltec WiFi Kit 32 V3 (ESP32-S3) voltage divider: 100kΩ/390kΩ = multiply by 4.9

  SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

  // Function to dim OLED display (SSD1306 commands)
  // Based on Heltec forum: contrast control may not work well on V2.0+ boards
  // Using multiple SSD1306 commands for better dimming effect
  void setOLEDContrast(uint8_t contrast) {
    Wire.beginTransmission(0x3c);
    Wire.write(0x00); // Command mode
    
    // Set contrast (0x81)
    Wire.write(0x81);
    Wire.write(contrast);
    
    // Set precharge period (0xD9) - affects brightness
    // Lower values = dimmer, but too low causes flicker
    if (contrast < 128) {
      Wire.write(0xD9);
      Wire.write(0x11); // Dim: phase 1 = 1, phase 2 = 1 (minimum)
    } else {
      Wire.write(0xD9);
      Wire.write(0xF1); // Bright: phase 1 = 15, phase 2 = 1 (default)
    }
    
    // Set VCOMH deselect level (0xDB) - affects brightness
    if (contrast < 128) {
      Wire.write(0xDB);
      Wire.write(0x20); // Dim: 0.77 * VCC (lower voltage = dimmer)
    } else {
      Wire.write(0xDB);
      Wire.write(0x40); // Bright: 0.83 * VCC (default)
    }
    
    Wire.endTransmission();
    
    // Also try library method as backup
    display.setContrast(contrast);
  }

  bool isPaired = false;  
  unsigned long lastUpdate = 0;
  const unsigned long UPDATE_INTERVAL = 20000; // 20 seconds for better responsiveness
  bool buttonPressed = false;
  unsigned long buttonPressTime = 0;
  unsigned long lastButtonPress = 0;
  bool hadSavedConfigOnBoot = false; // Track if we loaded saved config on boot
  int consecutiveConfig404s = 0;
  const int CONFIG_404_THRESHOLD = 3;
  // WiFi reconnection with exponential backoff
  unsigned long lastWifiReconnectAttempt = 0;
  int wifiReconnectAttempts = 0;
  const int WIFI_MAX_QUICK_RETRIES = 5;  // After 5 quick failures, slow down significantly
  const unsigned long WIFI_MIN_BACKOFF_MS = 5000;  // Start with 5 seconds between attempts
  const unsigned long WIFI_MAX_BACKOFF_MS = 300000;  // Max 5 minutes between attempts
  bool wifiConfigPortalRequested = false;  // Flag to enter WiFi setup mode
  enum ButtonSource {
    BUTTON_SOURCE_NONE,
    BUTTON_SOURCE_EXTERNAL,
    BUTTON_SOURCE_PRG,
    BUTTON_SOURCE_BOTH
  };
  ButtonSource lastButtonSource = BUTTON_SOURCE_NONE;
  void logCurrentPairingState();
  bool confirmFactoryReset();
  void renderFactoryResetPrompt(int selectedOption);

  // Menu system state
  bool inMenuMode = false;
  int currentMenuOption = 0; // 0=Feed, 1=Play, 2=Home
  unsigned long lastMenuCycle = 0;

  // Screensaver settings for power saving
  const unsigned long SCREENSAVER_TIMEOUT = 180000; // 3 minutes of inactivity → animated screensaver
  const unsigned long BITCOIN_FACTS_TIMEOUT = 220000; // 3.67 minutes → Bitcoin facts screen
  const unsigned long SCREENSAVER_DISPLAY_OFF_TIMEOUT = 280000; // 4.67 minutes total → display OFF
  uint8_t NORMAL_BRIGHTNESS = 255;  // Full brightness (non-const so it can be extern)
  uint8_t DIM_BRIGHTNESS = 10;      // Dimmed brightness (much lower for V2.0+ boards that don't respond well to contrast)
  bool isScreensaverActive = false;
  bool isBitcoinFactsActive = false;
  bool isDisplayOff = false;
  int bitcoinFactsForSession[3] = {0, 1, 2}; // 3 facts to show in this session
  int onboardingStep = 0;
  unsigned long bitcoinFactsStartTime = 0; // When facts mode started  // Track if display is completely off (power saving mode)

  // Low battery warning state
  bool isLowBatteryWarningActive = false;
  unsigned long lowBatteryWarningStartTime = 0;
  bool lowBatteryAlertPlayed = false; // Prevent repeated alerts

  const unsigned long SHORT_PRESS_MAX = 600;
  const unsigned long HOLD_PRESS_MIN = 700;
  const unsigned long VERY_LONG_PRESS = 10000; // 10 seconds to prevent accidental factory reset

  int getBatteryPercentage() {
    float voltage = getBatteryVoltage();
    
    // Non-linear LiPo discharge curve (more accurate than simple linear mapping)
    // Based on typical single-cell LiPo discharge characteristics
    int percentage;
    
    if (voltage >= 4.15) {
      percentage = 100;  // 4.15V+ = 100%
    } else if (voltage >= 4.0) {
      // 4.0-4.15V = 85-100% (top 15% is rapid)
      percentage = 85 + (int)((voltage - 4.0) / 0.15 * 15);
    } else if (voltage >= 3.85) {
      // 3.85-4.0V = 60-85% (gradual decline)
      percentage = 60 + (int)((voltage - 3.85) / 0.15 * 25);
    } else if (voltage >= 3.7) {
      // 3.7-3.85V = 40-60% (mid range, flatter curve)
      percentage = 40 + (int)((voltage - 3.7) / 0.15 * 20);
    } else if (voltage >= 3.5) {
      // 3.5-3.7V = 15-40% (steeper drop)
      percentage = 15 + (int)((voltage - 3.5) / 0.2 * 25);
    } else if (voltage >= 3.2) {
      // 3.2-3.5V = 5-15% (getting low)
      percentage = 5 + (int)((voltage - 3.2) / 0.3 * 10);
    } else if (voltage >= 3.0) {
      // 3.0-3.2V = 0-5% (critical low)
      percentage = (int)((voltage - 3.0) / 0.2 * 5);
    } else {
      percentage = 0;  // Below 3.0V = dead
    }
    
    // Clamp between 0-100
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;
    
    // Log if voltage seems inconsistent
    if (voltage > 4.3) {
      Serial.println("⚠️  Warning: Voltage > 4.3V - may be charging or ADC issue");
    }
    if (voltage < 3.0 && voltage > 0.1) {
      Serial.println("⚠️  Warning: Voltage < 3.0V - battery critically low");
    }
    
    return percentage;
  }

  // Returns battery level 0-3 for icon display
  int getBatteryLevel() {
    float voltage = getBatteryVoltage();
    if (voltage >= 3.9) return 3;      // Full
    else if (voltage >= 3.7) return 2; // Medium  
    else if (voltage >= 3.5) return 1; // Low
    else return 0;                     // Critical
  }

  // Returns true if battery appears to be charging (USB connected)
  bool isBatteryCharging() {
    float voltage = getBatteryVoltage();
    // If voltage >= 4.1V, likely charging or nearly full on USB power
    return voltage >= 4.1;
  }

  // Helper function to read battery once
  float readVbatOnce() {
    // Use calibrated analogReadMilliVolts for better accuracy
    float v_meas = analogReadMilliVolts(VBAT_PIN) / 1000.0;
    return v_meas * 4.9;  // 100kΩ/(100kΩ+390kΩ) divider = multiply by 4.9
  }

  float getBatteryVoltage() {
    // Brief wait for voltage to settle after any WiFi transmission
    delay(10);
    
    // Re-initialize ADC config (can reset after WiFi init or deep sleep)
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    
    // Use ADC_CTRL HIGH consistently (tested to work reliably)
    pinMode(ADC_CTRL, OUTPUT);
    digitalWrite(ADC_CTRL, LOW);
    delay(5);
    
    // Debug: Read raw ADC values
    int rawADC = analogRead(VBAT_PIN);
    int rawMillivolts = analogReadMilliVolts(VBAT_PIN);
    
    Serial.println("🔋 ADC Debug: pin=" + String(VBAT_PIN) + " ctrl=" + String(ADC_CTRL));
    Serial.println("   Raw ADC value: " + String(rawADC) + " (0-4095 range)");
    Serial.println("   Raw millivolts: " + String(rawMillivolts) + " mV");
    Serial.println("   After 4.9x multiplier: " + String(rawMillivolts / 1000.0 * 4.9, 2) + " V");
    
    // Average 4 samples for stable reading (reduced from 8 for faster response)
    float sum = 0;
    for (int i = 0; i < 4; i++) {
      sum += readVbatOnce();
      delay(1);
    }
    float rawVoltage = sum / 4.0;
    
    // Apply Exponential Moving Average (EMA) filter for stable readings
    static float smoothedVoltage = 0;
    if (smoothedVoltage == 0) {
      // First reading - initialize with current value
      smoothedVoltage = rawVoltage;
    } else {
      // EMA: 85% old value + 15% new value (smooth but responsive)
      smoothedVoltage = (0.85 * smoothedVoltage) + (0.15 * rawVoltage);
    }
    
    float voltage = smoothedVoltage;
    
    // Sanity check for disconnected battery or bad reading
    if (voltage < 0.1) {
      Serial.println("⚠️  Warning: Battery read near 0 V – check cable or battery health.");
    } else if (voltage < 3.0 && voltage > 0.1) {
      Serial.println("⚠️  Warning: Voltage < 3.0V - battery critically low");
    }
    
    Serial.println("Battery: Raw=" + String(rawVoltage, 2) + "V Smoothed=" + String(voltage, 2) + "V");
    
    if (voltage > 4.15) {
      Serial.println("ℹ️  Battery appears nearly full - charging LED may be off due to charge complete");
    }
    
    return voltage;
  }

  void playSatsEarnedSound() {
    // Uplifting 8-bit style melody for earning sats
    int melody[] = {523, 587, 659, 784, 880, 1047};
    int durations[] = {100, 100, 100, 150, 150, 300};
    
    for (int i = 0; i < 6; i++) {
      tone(BUZZER_PIN, melody[i], durations[i]);
      delay(durations[i] + 20);
      noTone(BUZZER_PIN);
    }
  }

  void playFixRejectedSound() {
    if (isQuietHours()) return;  // Silent during quiet hours
    // Sad descending tone - opposite of the celebration melody
    int melody[] = {880, 784, 659, 587, 523, 392};
    int durations[] = {150, 150, 150, 200, 200, 400};
    
    for (int i = 0; i < 6; i++) {
      tone(BUZZER_PIN, melody[i], durations[i]);
      delay(durations[i] + 30);
      noTone(BUZZER_PIN);
    }
  }

  void displayPairingCode() {
  #ifdef DEBUG_LOGGING
    Serial.print(F("Pairing code: "));
    Serial.println(pairingCode);
  #endif
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 5, "Pairing Code:");
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 25, pairingCode);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 50, "Enter in Ganamos app");
    display.display();
  }

  void renderBitcoinFact(SSD1306Wire &display, int factIndex) {
    
    // Cycle through facts
    factIndex = factIndex % BITCOIN_FACTS_COUNT;
    
    display.clear();
    
    // Draw Bitcoin logo on the left (32x32px at position 0, 16 to center vertically)
    display.drawXbm(0, 16, 32, 32, epd_bitmap_bitcoin_32);
    
    // Draw fact text to the right of logo with padding (starts at x=36, wraps across 4 lines)
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    
    // Get the fact from PROGMEM
    char factBuffer[128];
    strcpy_P(factBuffer, (char*)pgm_read_ptr(&(bitcoin_facts[factIndex])));
    
    // Word wrap the fact text (max ~18 chars per line for 92px width)
    String fact = String(factBuffer);
    int textX = 36;
    int lineHeight = 11;
    int startY = 6; // Moved up to make room for 4 lines
    
    // Simple word wrapping - break at spaces near line limit
    int currentLine = 0;
    int currentPos = 0;
    int maxCharsPerLine = 18;
    
    while (currentPos < fact.length() && currentLine < 4) {
      String line = "";
      int lineStart = currentPos;
      
      // Find good break point (word boundary)
      while (currentPos < fact.length() && line.length() < maxCharsPerLine) {
        line += fact.charAt(currentPos);
        currentPos++;
      }
      
      // Backtrack to last space if we're mid-word
      if (currentPos < fact.length() && fact.charAt(currentPos) != ' ') {
        int lastSpace = line.lastIndexOf(' ');
        if (lastSpace > 0) {
          currentPos = lineStart + lastSpace + 1;
          line = line.substring(0, lastSpace);
        }
      }
      
      line.trim(); // Trim in place (returns void)
      display.drawString(textX, startY + (currentLine * lineHeight), line);
      currentLine++;
    }
    
    display.display();
  }

  void VextON(void) {
  #ifdef DEBUG_LOGGING
    Serial.println(F("VextON - display power on"));
  #endif
    // First restore power via Vext pin
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);  // LOW = display ON (Vext enabled)
    
    delay(10); // Small delay for power to stabilize
    
    // Then wake up SSD1306 controller
    Wire.beginTransmission(0x3c);
    Wire.write(0x00); // Command mode
    Wire.write(0xAF); // Display ON command
    Wire.endTransmission();
  }

  void VextOFF(void) {
    // First, send display sleep command to SSD1306 controller
    Wire.beginTransmission(0x3c);
    Wire.write(0x00); // Command mode
    Wire.write(0xAE); // Display OFF command (0xAE = display off, 0xAF = display on)
    Wire.endTransmission();
    
    // Then cut power to display via Vext pin
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, HIGH);  // HIGH = display OFF (Vext disabled, saves power)
    
  #ifdef DEBUG_LOGGING
    Serial.println(F("VextOFF - display powered off"));
  #endif
  }

  // Calculate exponential backoff delay for WiFi reconnection
  unsigned long getWifiBackoffDelay() {
    if (wifiReconnectAttempts == 0) return WIFI_MIN_BACKOFF_MS;
    
    // Exponential backoff: 5s, 10s, 20s, 40s, 80s, then cap at 5 min
    unsigned long delay = WIFI_MIN_BACKOFF_MS << min(wifiReconnectAttempts, 6);
    return min(delay, WIFI_MAX_BACKOFF_MS);
  }

  // Enter WiFi config portal to connect to a new network
  void enterWifiConfigPortal() {
    Serial.println(F("📶 Entering WiFi config portal..."));
    
    // Disable watchdog during portal (it blocks for up to 3 minutes)
    esp_task_wdt_delete(NULL);
    Serial.println(F("🐕 Watchdog disabled for config portal"));
    
    // Turn on display if off
    if (isDisplayOff) {
      VextON();
      delay(100);
      display.init();
    }
    
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 10, "WiFi Setup Mode");
    display.drawString(64, 28, "Connect to:");
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 42, "SatoshiPet-Setup");
    display.display();
    
    // Disconnect from any existing connection
    WiFi.disconnect(true);
    delay(100);
    
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);  // 3 minute timeout
    wm.setConnectTimeout(30);
    
    // This will block until connected or timeout
    bool connected = wm.startConfigPortal("SatoshiPet-Setup");
    
    if (connected) {
      Serial.println(F("✅ WiFi connected via config portal!"));
      wifiReconnectAttempts = 0;
      lastWifiReconnectAttempt = 0;
      
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 25, "Connected!");
      display.display();
      delay(1500);
    } else {
      Serial.println(F("❌ Config portal timed out"));
      WiFi.mode(WIFI_OFF);  // Turn off to save power
    }
    
    wifiConfigPortalRequested = false;
    
    // Re-enable watchdog
    esp_task_wdt_add(NULL);
    Serial.println(F("🐕 Watchdog re-enabled"));
  }

  void setup() {
    Serial.begin(115200);
    
    // ESP32 Arduino framework already initializes watchdog timer (5s default)
    // DISABLED: Watchdog registration causing boot loop
    // esp_task_wdt_add(NULL); // Add loop() task to watchdog
    
    VextON();
    delay(100);

    display.init();
  #ifdef DEBUG_LOGGING
    Serial.println(F("Display initialized"));
  #endif
    
    // Show splash screen logo (128x64 full screen)
    display.clear();
    display.drawXbm(0, 0, 128, 64, epd_bitmap_g_logo);
    display.display();
    delay(2500); // Show splash for 2.5 seconds
    
    // Show tagline screen: "Fix your community / Earn [Bitcoin]"
    display.clear();
    display.setFont(ArialMT_Plain_10);
    // Line 1: "Fix your community" - centered horizontally (x=64), moved down (y=16)
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 16, "Fix your community");
    // Line 2: "Earn" + Bitcoin logo (72x15px) - centered horizontally
    // "Earn" ~24px + logo 72px = ~96px total, centered: (128-96)/2 = 16px left margin
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(16, 38, "Earn");
    display.drawXbm(42, 36, 72, 17, epd_bitmap_bitcoin_72);
    display.display();
    delay(2500); // Show tagline for 2.5 seconds
    
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    pinMode(RGB_LED, OUTPUT);  // Enable RGB LED
    digitalWrite(RGB_LED, LOW);  // Start with LED off
    pinMode(BUZZER_PIN, OUTPUT);  // Enable buzzer
    digitalWrite(BUZZER_PIN, LOW);  // Start with buzzer off
    
    // Configure ADC for battery reading
    analogReadResolution(12);  // 12-bit resolution (0-4095)
    analogSetAttenuation(ADC_11db);  // Full scale ~3.3V

      // FIRST: Check if device is already paired (before WiFi setup)
    // This determines whether we need mandatory WiFi setup or can try quick reconnect
    hadSavedConfigOnBoot = loadDeviceConfig();
    logCurrentPairingState();
    
    WiFiManager wm;
    wm.setCustomHeadElement("<style>"
      "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
      "background: #f5f5f5; margin: 0; padding: 20px; }"
      ".c { background: white; border-radius: 16px; padding: 40px; "
      "box-shadow: 0 2px 20px rgba(0,0,0,0.08); max-width: 400px; margin: 40px auto; }"
      "h1 { color: #1a1a1a; font-size: 32px; margin-bottom: 8px; font-weight: 600; }"
      "h3 { color: #666; font-weight: 400; margin-bottom: 24px; font-size: 16px; }"
      ".btn { background: #10b981; "
      "border: none; color: white; padding: 14px 28px; border-radius: 10px; "
      "font-size: 16px; font-weight: 500; cursor: pointer; transition: all 0.2s; "
      "width: 100%; margin: 8px 0; }"
      ".btn:hover { background: #059669; transform: translateY(-1px); "
      "box-shadow: 0 4px 12px rgba(16, 185, 129, 0.3); }"
      "input { border: 2px solid #e5e7eb; border-radius: 8px; padding: 12px; "
      "font-size: 15px; width: 100%; margin: 8px 0; transition: border 0.2s; }"
      "input:focus { outline: none; border-color: #10b981; }"
      "label { color: #374151; font-weight: 500; font-size: 14px; }"
      "</style>");
    
    wm.setTitle("Satoshi Pet Setup");
    wm.setConnectTimeout(10);        // 10 seconds to connect to saved network
    wm.setWiFiAutoReconnect(true);   // Auto-reconnect if connection drops

    if (!hadSavedConfigOnBoot) {
      // ===== UNPAIRED DEVICE: WiFi setup is REQUIRED before pairing =====
      Serial.println(F("No saved config - WiFi setup required for pairing"));
      
      // Show "Connect to WiFi" message on display
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER);
      display.drawString(64, 10, "WiFi Setup Required");
      display.drawString(64, 28, "Connect to WiFi:");
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 44, "SatoshiPet-Setup");
      display.display();
      
      // Start config portal - blocks until WiFi configured or timeout
      wm.setConfigPortalTimeout(0);  // No timeout - wait forever for WiFi setup
      bool wifiConfigured = wm.autoConnect("SatoshiPet-Setup");
      
      if (wifiConfigured && WiFi.status() == WL_CONNECTED) {
        Serial.print(F("WiFi configured and connected: "));
        Serial.println(WiFi.localIP());
      } else {
        // WiFi setup failed or timed out - show error and restart
        Serial.println(F("WiFi setup failed - restarting..."));
        display.clear();
        display.setFont(ArialMT_Plain_10);
        display.setTextAlignment(TEXT_ALIGN_CENTER);
        display.drawString(64, 20, "WiFi Setup Failed");
        display.drawString(64, 40, "Restarting...");
        display.display();
        delay(3000);
        ESP.restart();
      }
    } else {
      // ===== PAIRED DEVICE: Try quick reconnect, offline is OK =====
      Serial.println(F("Device paired - trying quick WiFi reconnect..."));
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(false);
      WiFi.begin();
      
      // Wait up to 10 seconds for connection
      unsigned long wifiStart = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart) < 10000) {
        delay(250);
        yield();
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("WiFi connected: "));
        Serial.println(WiFi.localIP());
      } else {
        Serial.println(F("WiFi unavailable - continuing offline"));
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
    }

    // Initialize NTP for real time (for sleep cycle)
    configTime(-8 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // PST timezone, adjust as needed
    Serial.println("NTP time sync initiated...");

    initPet("default");
    
    // Initialize economy system
    extern void initEconomy();
    extern void loadPetStats();
    extern void setLocalCoins(int coins);
    extern int getPendingSpendCount();
    extern void clearEconomyData();
    
    initEconomy();
    
    // Check for corrupted economy data and clear if needed
    int pendingCount = getPendingSpendCount();
    if (pendingCount > 10) {
      Serial.println("⚠️ Suspicious pending spend count (" + String(pendingCount) + ") - clearing economy data");
      clearEconomyData();
    }
    
    loadPetStats();
    
    // Now handle pairing - WiFi is guaranteed to be connected for unpaired devices
    if (hadSavedConfigOnBoot) {
      Serial.println("Found saved config! Device already paired.");
      Serial.println("Device ID: " + ganamosConfig.deviceId);
      Serial.println("Pet name: " + ganamosConfig.petName);
      Serial.println("Pairing code: " + pairingCode);
      Serial.println("Last known balance from config: " + String(ganamosConfig.balance) + " sats");
      Serial.println("Last known coins from config (server): " + String(ganamosConfig.coins) + " coins");
      
      // DON'T overwrite local economy balance with server config!
      extern int getLocalCoins();
      Serial.println("Actual local coin balance: " + String(getLocalCoins()) + " coins");
      
      isPaired = true;
      Serial.println("Will fetch data in main loop...");
    } else {
      // WiFi is now connected - safe to generate and display pairing code
      Serial.println(F("WiFi connected - generating pairing code"));
      generatePairingCode();
      Serial.print(F("Pairing code: "));
      Serial.println(pairingCode);
      displayPairingCode();
      isPaired = false;
    }
    // Initialize buttons using button_handler module
    initButtons();
    
    // Quick battery test before main loop starts
    Serial.println("\n=== Battery Quick Test ===");
    float testVoltage = getBatteryVoltage();
    Serial.printf("Voltage: %.2f V\n", testVoltage);
    Serial.println("==========================\n");
    
    // Initialize lastButtonPress to prevent immediate screensaver activation
    lastButtonPress = millis();
    setOLEDContrast(NORMAL_BRIGHTNESS); // Start at full brightness (direct I2C command)
    display.setContrast(NORMAL_BRIGHTNESS); // Also try library method
    
      // Register main loop task with watchdog (15 second timeout)
    // Configure watchdog with 15 second timeout
    esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 15000,           // 15 second timeout
      .idle_core_mask = 0,           // Don't watch idle tasks
      .trigger_panic = true          // Panic (reset) on timeout
    };
    esp_task_wdt_reconfigure(&wdt_config);  // Reconfigure existing watchdog
    esp_task_wdt_add(NULL);                  // Add current task (main loop)
    Serial.println("🐕 Watchdog initialized (15s timeout)");

    Serial.println("Setup complete!");
  }

  void logCurrentPairingState() {
    Serial.println("🔍 Current pairing snapshot:");
    Serial.println("  deviceId: " + ganamosConfig.deviceId);
    Serial.println("  pairingCode: " + pairingCode);
    Serial.println("  petName: " + ganamosConfig.petName);
    Serial.println("  petType: " + ganamosConfig.petType);
    Serial.println("  balance: " + String(ganamosConfig.balance));
    Serial.println("  coins: " + String(ganamosConfig.coins));
  }

  void loop() {
    // DISABLED: Watchdog feed causing boot loop
    // esp_task_wdt_reset();
      esp_task_wdt_reset();  // Feed watchdog at start of every loop iteration
    // Ensure WiFi stays off if we're in offline mode (prevents background blocking)
    static bool wifiDisabled = false;
    if (!wifiDisabled && WiFi.status() != WL_CONNECTED) {
      if (WiFi.getMode() != WIFI_OFF) {
        Serial.println(F("Forcing WiFi OFF to prevent blocking"));
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
      wifiDisabled = true;
    }
    
    static unsigned long lastLoopDebug = 0;
    static unsigned long lastLoopTime = 0;
    unsigned long now = millis();
    
    // Warn if loop is running slowly (taking more than 500ms between iterations)
    if (lastLoopTime > 0 && (now - lastLoopTime) > 500) {
      Serial.print(F("SLOW LOOP: "));
      Serial.print(now - lastLoopTime);
      Serial.println(F("ms since last iteration"));
    }
    lastLoopTime = now;
    
    // Debug: track time through loop sections
    unsigned long sectionStart = millis();
    
    // Log loop alive status every 30 seconds to help debug reboots
  #ifdef DEBUG_LOGGING
    if (now - lastLoopDebug > 30000) {
      Serial.print(F("Loop alive, uptime: "));
      Serial.print(now/1000);
      Serial.println(F("s"));
      lastLoopDebug = now;
    }
  #endif
    static int cachedBatteryPct = -1; // Cache battery percentage (-1 = not initialized)
    static unsigned long lastBatteryCheck = 0;
    
    // Initialize battery on first loop OR update every 60 seconds
    if (cachedBatteryPct < 0 || (now - lastBatteryCheck > 60000)) { 
      cachedBatteryPct = getBatteryPercentage();
      lastBatteryCheck = now;
      
      // Trigger low battery warning at 10% (only once per charge cycle)
      if (cachedBatteryPct <= 10 && !lowBatteryAlertPlayed && !isLowBatteryWarningActive) {
        Serial.println("🔋 LOW BATTERY WARNING - waking display to show warning");
        
        // Wake display if needed
        if (isDisplayOff) {
          VextON();
          delay(100);
          display.init();
          display.setFont(ArialMT_Plain_10);
          display.setTextAlignment(TEXT_ALIGN_LEFT);
        }
        
        // Enter low battery warning mode
        isLowBatteryWarningActive = true;
        lowBatteryWarningStartTime = now;
        isScreensaverActive = false;
        isBitcoinFactsActive = false;
        isDisplayOff = false;
        
        // Play warning sound
        playLowBatterySound();
        lowBatteryAlertPlayed = true;
        
        // Show the warning screen
        renderLowBatteryWarning(display, cachedBatteryPct);
      }
    }
    
    // Handle low battery warning timeout (60 seconds) - return to normal mode
    if (isLowBatteryWarningActive && (now - lowBatteryWarningStartTime > 60000)) {
      Serial.println("🔋 Low battery warning complete - returning to normal mode");
      isLowBatteryWarningActive = false;
      // Just return to normal pet screen, don't enter ultra-low-power yet
      int batteryPct = getBatteryPercentage();
      renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
    }
    
    // Enter ultra-low-power mode only at 5% (critical battery)
    // BUT only if user hasn't interacted recently - let normal screensaver flow handle sleep
    // This ensures button press always wakes device for full timeout period
    if (cachedBatteryPct <= 5 && !isDisplayOff && !isLowBatteryWarningActive) {
      // Only force low-power if we've already gone through the screensaver timeout
      // (i.e., user hasn't pressed a button recently)
      if (now - lastButtonPress > SCREENSAVER_DISPLAY_OFF_TIMEOUT) {
        Serial.println("🔋 CRITICAL BATTERY (5%) - entering ultra-low-power mode");
        isScreensaverActive = true;
        isDisplayOff = true;
        VextOFF();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        // Will only wake for button press or poll every 5 minutes
      }
    }
    
    // Reset low battery alert flag when battery charged above 20%
    if (cachedBatteryPct > 20) {
      lowBatteryAlertPlayed = false;
    }
    
    // Multi-stage sleep transition: screensaver → facts → display OFF
      if (!isScreensaverActive && (now - lastButtonPress > SCREENSAVER_TIMEOUT)) {
      isScreensaverActive = true;
      isBitcoinFactsActive = false;
      isDisplayOff = false;
      // Enable WiFi power saving when idle (saves 20-40mA)
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(true);
      }
  #ifdef DEBUG_LOGGING
      Serial.println(F("Entering screensaver"));
  #endif
    }
    
    // After 40 seconds in screensaver, show Bitcoin facts
    if (isScreensaverActive && !isBitcoinFactsActive && !isDisplayOff && (now - lastButtonPress > BITCOIN_FACTS_TIMEOUT)) {
      isBitcoinFactsActive = true;
      bitcoinFactsStartTime = now;
      
      // Pick 3 random facts to show (20 seconds each = 60 seconds total)
      for (int i = 0; i < 3; i++) {
        bitcoinFactsForSession[i] = random(0, BITCOIN_FACTS_COUNT);
      }
  #ifdef DEBUG_LOGGING
      Serial.println(F("Entering Bitcoin facts mode"));
  #endif
    }
    
    // After 60 seconds of facts, turn display completely OFF
    if (isBitcoinFactsActive && !isDisplayOff && (now - lastButtonPress > SCREENSAVER_DISPLAY_OFF_TIMEOUT)) {
      isDisplayOff = true;
      VextOFF();  // Turn display OFF completely to save power
      
      // Disconnect WiFi to save significant power
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
  #ifdef DEBUG_LOGGING
      Serial.println(F("Display OFF, WiFi OFF (power saving)"));
  #endif
    }
    
    // Debug timing checkpoint
    if (millis() - sectionStart > 100) {
      Serial.print(F("SLOW: Battery section took "));
      Serial.print(millis() - sectionStart);
      Serial.println(F("ms"));
    }
    sectionStart = millis();
    
    if (!isPaired) {
      // Check every 5 seconds if we're now paired
      if (now - lastUpdate > 5000) {
        lastUpdate = now;
        
        extern int getLastHttpCode();
        bool fetchSuccess = fetchGanamosConfig();
        
        if (fetchSuccess) {
          isPaired = true;
          saveDeviceConfig(); // SAVE TO FLASH!
          Serial.println("Successfully paired and saved to flash memory!");
          consecutiveConfig404s = 0;
          
          display.clear();
          display.setFont(ArialMT_Plain_10);
          display.setTextAlignment(TEXT_ALIGN_CENTER);
          display.drawString(64, 20, "Connected!");
          display.drawString(64, 35, ganamosConfig.petName);
          display.display();
          delay(2000);
          
          // Start onboarding flow
          onboardingStep = 1;
          renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
        } else {
          // Check if we got a 404 - only regenerate if we had saved config that's now invalid
          // If we're starting fresh (no saved config), 404 is expected - don't regenerate code
          static bool hasHandledSavedConfig404 = false;
          bool got404 = (getLastHttpCode() == 404);

          if (got404) {
            consecutiveConfig404s++;
            Serial.println("⚠️ Pairing fetch returned 404 (" + String(consecutiveConfig404s) + "/" + String(CONFIG_404_THRESHOLD) + ")");
          } else {
            consecutiveConfig404s = 0;
          }
          
          if (got404 && hadSavedConfigOnBoot && !hasHandledSavedConfig404 && consecutiveConfig404s >= CONFIG_404_THRESHOLD) {
            // Had saved config but it's invalid - clear and regenerate ONCE
            Serial.println("⚠️ Saved pairing info invalid (404) - clearing saved config");
            clearDeviceConfig();
            isPaired = false;
            generatePairingCode(); // Generate new pairing code
            Serial.println("New pairing code generated: " + pairingCode);
            hasHandledSavedConfig404 = true;
            displayPairingCode();
            consecutiveConfig404s = 0;
          } else {
            // Either no 404, or no saved config (expected), or already handled
            // Just show the current pairing code (keep it static)
            if (!got404) {
              Serial.println("Connection failed (not 404) - keeping pairing code");
            } else {
              Serial.println("Not paired yet - waiting for user to pair (pairing code static)");
            }
            displayPairingCode();
          }
        }
      }
    } else {
      // Debug timing checkpoint
      if (millis() - sectionStart > 100) {
        Serial.print(F("SLOW: Unpaired section took "));
        Serial.print(millis() - sectionStart);
        Serial.println(F("ms"));
      }
      sectionStart = millis();
      
      // Debug: Print state flags
      static unsigned long lastStateLog = 0;
      if (millis() - lastStateLog > 10000) {
        Serial.print(F("State: menu="));
        Serial.print(inMenuMode);
        Serial.print(F(" scrsvr="));
        Serial.print(isScreensaverActive);
        Serial.print(F(" dispOff="));
        Serial.print(isDisplayOff);
        Serial.print(F(" celeb="));
        Serial.println(showCelebration);
        lastStateLog = millis();
      }
      
      // Normal operation - fetch data less frequently during screensaver to save power
      unsigned long updateInterval = isScreensaverActive ? 300000 : UPDATE_INTERVAL; // 5 minutes during screensaver, 20s normal
      
      if (now - lastUpdate > updateInterval) {
        lastUpdate = now;
        unsigned long fetchStart = millis();
        
        // Non-blocking WiFi reconnection during screensaver
        // WiFi.begin() starts background reconnection, doesn't block
        // If not connected yet, we skip this fetch and try next cycle
              if (WiFi.status() != WL_CONNECTED) {
          unsigned long backoffDelay = getWifiBackoffDelay();
          
          // Check if enough time has passed since last attempt
          if (now - lastWifiReconnectAttempt < backoffDelay) {
            // Not time yet - just skip this fetch cycle
            return;
          }
          
          wifiReconnectAttempts++;
          lastWifiReconnectAttempt = now;
          
          Serial.print(F("📶 WiFi reconnect attempt #"));
          Serial.print(wifiReconnectAttempts);
          Serial.print(F(" (next in "));
          Serial.print(getWifiBackoffDelay() / 1000);
          Serial.println(F("s)"));
          
          // After many failures, suggest config portal
          if (wifiReconnectAttempts >= WIFI_MAX_QUICK_RETRIES && wifiReconnectAttempts % 5 == 0) {
            Serial.println(F("💡 Tip: Hold PRG button 5 seconds to enter WiFi setup"));
          }
          
          WiFi.mode(WIFI_STA);
          WiFi.begin();  // Try with saved credentials
          
          // Wait briefly to see if connection starts
          unsigned long wifiWaitStart = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - wifiWaitStart < 3000) {
            delay(100);
            yield();
          }
          
          if (WiFi.status() == WL_CONNECTED) {
            Serial.println(F("✅ WiFi reconnected!"));
            wifiReconnectAttempts = 0;
          } else {
            Serial.println(F("❌ WiFi reconnect failed"));
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);  // Turn off to save power between attempts
          }
          
          return;  // Skip to next loop iteration
        } else {
          // Connected - reset retry counter
          if (wifiReconnectAttempts > 0) {
            wifiReconnectAttempts = 0;
            Serial.println(F("✅ WiFi connection restored"));
          }
        }
        
        extern int getLastHttpCode();
        bool fetchSuccess = fetchGanamosConfig();
        
        // Log fetch timing
        unsigned long fetchTime = millis() - fetchStart;
        if (fetchTime > 100) {
          Serial.print(F("SLOW FETCH: "));
          Serial.print(fetchTime);
          Serial.println(F("ms"));
        }
        
        // If we got a 404, the saved pairing code is invalid - clear config and reset
        if (!fetchSuccess) {
          if (getLastHttpCode() == 404) {
            consecutiveConfig404s++;
            Serial.println("⚠️ Device config 404 (" + String(consecutiveConfig404s) + "/" + String(CONFIG_404_THRESHOLD) + ")");
            if (consecutiveConfig404s >= CONFIG_404_THRESHOLD) {
              Serial.println("⚠️ Device not found after repeated attempts - clearing saved config");
              clearDeviceConfig();
              isPaired = false;
              generatePairingCode();
              // Turn display back on to show pairing code
              VextON();
              delay(100);
              display.init();
              displayPairingCode();
              Serial.println("New pairing code generated: " + pairingCode);
              consecutiveConfig404s = 0;
              return; // Skip to next loop iteration
            } else {
              Serial.println("⚠️ Waiting for additional 404s before resetting.");
            }
          } else {
            consecutiveConfig404s = 0;
          }
          // Don't return here - continue to button handling below
        }
        
        consecutiveConfig404s = 0;

        if (fetchSuccess) {
          static int lastBalance = ganamosConfig.balance;
          
          // Sync economy: update local balance from server and sync pending spends
          extern int getLocalCoins();
          extern void setLocalCoins(int coins);
          extern int syncPendingSpends();
          extern void clearSyncedSpends();
          extern int getPendingSpendCount();
          
          int serverCoins = ganamosConfig.coins;
          int localCoins = getLocalCoins();
          
          // Sync pending spends FIRST before reconciling balance
          int synced = syncPendingSpends();
          if (synced > 0) {
            Serial.println("✅ Synced " + String(synced) + " pending spends");
            clearSyncedSpends();
          }
          
          // Check how many pending spends are still unsynced
          int remainingPending = getPendingSpendCount();
          
          // After sync attempt, reconcile local balance with server
          // BUT only trust server if we have NO pending spends - otherwise our offline
          // spending hasn't been acknowledged by the server yet!
          if (serverCoins != localCoins) {
            int diff = localCoins - serverCoins;
            
            if (remainingPending > 0) {
              // DON'T trust server - we have unsynced spends that server doesn't know about
              Serial.println("⏳ Balance mismatch but " + String(remainingPending) + " pending spends not synced yet");
              Serial.println("   Local: " + String(localCoins) + ", Server: " + String(serverCoins));
              Serial.println("   Keeping local balance until spends sync successfully");
              // Don't call setLocalCoins - keep local balance as-is
            } else if (diff > 0) {
              // No pending spends, but local is higher - this shouldn't happen normally
              // Could be corrupted data, so trust server
              Serial.println("⚠️ Local balance higher than server by " + String(diff) + " coins (no pending spends)");
              Serial.println("   This may indicate corrupted data - trusting server balance");
              setLocalCoins(serverCoins);
            } else {
              // Server is higher - user earned coins while offline, safe to update
              Serial.println("💰 Server balance higher by " + String(-diff) + " coins (earned while offline)");
              setLocalCoins(serverCoins);
            }
          }
          
          // Apply time-based decay to pet stats
          extern void applyTimeBasedDecay();
          applyTimeBasedDecay();
          
          updatePetMood(ganamosConfig.btcPrice, ganamosConfig.balance);
          cachedBatteryPct = getBatteryPercentage(); // Update cached battery value
          
          // Check for balance increase (wake from screensaver/facts if needed)
            if (ganamosConfig.balance > lastBalance && (isScreensaverActive || isBitcoinFactsActive)) {
            // Balance increased! Wake from sleep mode
            isScreensaverActive = false;
            isBitcoinFactsActive = false;
            if (isDisplayOff) {
              // Display was completely off, turn it back on
              VextON();  // Turn display back on
              delay(100);  // Wait for display to stabilize
              display.init();
              display.setFont(ArialMT_Plain_10);
              display.setTextAlignment(TEXT_ALIGN_LEFT);
              Serial.println("💰 Balance increased - waking from display OFF!");
            } else {
              Serial.println("💰 Balance increased - waking from animated screensaver!");
            }
            // Disable WiFi power saving for responsive active use
            if (WiFi.status() == WL_CONNECTED) {
              WiFi.setSleep(false);
            }
            isDisplayOff = false;
          }
          lastBalance = ganamosConfig.balance;

          // Check for fix rejection notification
          static String lastSeenRejectionId = "";
          if (ganamosConfig.lastRejectionId.length() > 0 && 
              ganamosConfig.lastRejectionId != lastSeenRejectionId) {
              // New rejection detected!
              lastSeenRejectionId = ganamosConfig.lastRejectionId;
              triggerRejection(ganamosConfig.rejectionMessage);
              }
          
          // Check for critical pet states (sad/dying) - wake if needed
          extern PetStats petStats;
          if ((isScreensaverActive || isBitcoinFactsActive || isDisplayOff)) {
            if (petStats.fullness == 0 && petStats.happiness == 0) {
              // Pet died! Wake immediately and alert
              Serial.println("💀 CRITICAL: Pet died! Waking from sleep mode");
              isScreensaverActive = false;
              isBitcoinFactsActive = false;
              
              if (isDisplayOff) {
                VextON();
                delay(100);
                display.init();
              }
              isDisplayOff = false;
              
              // Play death sound and show warning
              extern void playDeathSound();
              playDeathSound();
              renderDeathWarning(display, ganamosConfig.petName);
              
            } else if (petStats.fullness < 20 || petStats.happiness < 20) {
              // Pet became sad! Wake and alert
              Serial.println("😢 ALERT: Pet needs attention! Waking from sleep mode");
              isScreensaverActive = false;
              isBitcoinFactsActive = false;
              
              if (isDisplayOff) {
                VextON();
                delay(100);
                display.init();
              }
              isDisplayOff = false;
              
              // Play sad sound and show appropriate warning
              extern void playSadSound();
              playSadSound();
              
              // Show hunger warning if fullness is low, otherwise sadness warning
              if (petStats.fullness < 20) {
                renderHungerWarning(display, ganamosConfig.petName, petStats.fullness);
              } else {
                renderSadnessWarning(display, ganamosConfig.petName, petStats.happiness);
              }
            }
          }
          
          // Disconnect WiFi again after poll to save power (if still in screensaver)
          if (isScreensaverActive && isDisplayOff) {
            Serial.println("📡 Disconnecting WiFi to save power...");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
          }
          
          // Render based on power saving mode
          if (isDisplayOff) {
            // Display is completely OFF - just log (maximum power saving)
            // DO NOT RENDER - display is powered off
            Serial.println("💤💤 Display OFF: Balance=" + String(ganamosConfig.balance) + " Battery=" + String(cachedBatteryPct) + "%");
          } else if (isLowBatteryWarningActive) {
            // Low battery warning mode - keep showing warning for 60 seconds
            renderLowBatteryWarning(display, cachedBatteryPct);
          } else if (isBitcoinFactsActive && !isDisplayOff) {
            // Bitcoin facts mode - rotate through 3 facts (20 seconds each)
            unsigned long timeInFacts = now - bitcoinFactsStartTime;
            int factSlot = (timeInFacts / 20000) % 3; // 0, 1, or 2
            renderBitcoinFact(display, bitcoinFactsForSession[factSlot]);
          } else if (isScreensaverActive && !isDisplayOff) {
            // Animated screensaver - render it (only if display is ON and not turned off)
            renderScreensaver(display, ganamosConfig.balance);
            Serial.println("💤 Screensaver: Balance=" + String(ganamosConfig.balance) + " Battery=" + String(cachedBatteryPct) + "%");
          } else if (onboardingStep > 0) {
            // Onboarding active - render current step (don't overwrite with pet)
            renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
          } else {
            // Normal mode - render pet
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, cachedBatteryPct);
            // Log battery voltage for debugging
            Serial.println("Battery: " + String(getBatteryVoltage(), 2) + "V (" + String(cachedBatteryPct) + "%)");
          }
        }
        // If fetch failed, just continue - pet will show with cached data
      }
      
      // Apply decay even when offline (every minute)
      static unsigned long lastDecayCheck = 0;
      if (now - lastDecayCheck > 60000) { // Check every minute
        extern void applyTimeBasedDecay();
        applyTimeBasedDecay();
        lastDecayCheck = now;
      }
      
      // Keep updating the display based on current mode
      extern bool showCelebration;
      extern bool showNewJobNotification;
      extern unsigned long newJobNotificationStart;
      extern const unsigned long NEW_JOB_NOTIFICATION_DURATION;
      extern void renderNewJobNotification(SSD1306Wire &display);
      extern bool showRejection;
      extern unsigned long rejectionStart;
      extern void renderRejection(SSD1306Wire &display);
      
      // Check if new job notification should end
      if (showNewJobNotification && (millis() - newJobNotificationStart >= NEW_JOB_NOTIFICATION_DURATION)) {
        showNewJobNotification = false;
        digitalWrite(RGB_LED, LOW);
      }

      // NEW: Check if rejection notification should end (5 seconds)
      if (showRejection && (millis() - rejectionStart >= 5000)) {
        showRejection = false;
        digitalWrite(RGB_LED, LOW);
      }
      
      // Menu mode is handled separately in button handling section
      if (!inMenuMode) {
        unsigned long renderStart = millis();
        if (isDisplayOff) {
          // Display is completely OFF - no rendering needed (maximum power saving)
          // Just skip rendering entirely
        } else if (showNewJobNotification) {
          // New job notification takes priority
          renderNewJobNotification(display);
        } else if (showRejection) {
          // NEW: Rejection notification
          renderRejection(display);
        } else if (isBitcoinFactsActive) {
          unsigned long timeInFacts = now - bitcoinFactsStartTime;
          int factSlot = (timeInFacts / 20000) % 3;
          renderBitcoinFact(display, bitcoinFactsForSession[factSlot]);
        } else if (isScreensaverActive) {
          if (!isDisplayOff) {
            static unsigned long lastScreensaverUpdate = 0;
            if (now - lastScreensaverUpdate > 500) {
              renderScreensaver(display, ganamosConfig.balance);
              lastScreensaverUpdate = now;
            }
          }
        } else if (showCelebration) {
          renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, cachedBatteryPct);
        } else if (onboardingStep > 0) {
          // Onboarding active - don't overwrite, let button handler manage it
          // Optionally re-render to keep it on screen:
          renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
        } else {
          // Normal mode
          static unsigned long lastDisplayUpdate = 0;
          static int lastDisplayedCoins = -1;
          static int lastDisplayedFullness = -1;
          static int lastDisplayedHappiness = -1;
          static int lastDisplayedBattery = -1;
          
          extern int getLocalCoins();
          int currentCoins = getLocalCoins();
          bool dataChanged = (currentCoins != lastDisplayedCoins ||
                            petStats.fullness != lastDisplayedFullness ||
                            petStats.happiness != lastDisplayedHappiness ||
                            cachedBatteryPct != lastDisplayedBattery);
          
          bool animationUpdate = (now - lastDisplayUpdate > 500);
          
          if (dataChanged || animationUpdate) {
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, cachedBatteryPct);
            lastDisplayUpdate = now;
            lastDisplayedCoins = currentCoins;
            lastDisplayedFullness = petStats.fullness;
            lastDisplayedHappiness = petStats.happiness;
            lastDisplayedBattery = cachedBatteryPct;
          }
        }
        // Check if rendering took too long
        if (millis() - renderStart > 100) {
          Serial.print(F("SLOW RENDER: "));
          Serial.print(millis() - renderStart);
          Serial.println(F("ms"));
        }
      }
    }
    
    // Debug timing checkpoint
    if (millis() - sectionStart > 100) {
      Serial.print(F("SLOW: Display section took "));
      Serial.print(millis() - sectionStart);
      Serial.println(F("ms"));
    }
    
    // Handle button presses
    bool prgButtonState = digitalRead(BUTTON_PIN_PRG) == LOW;
    bool externalButtonState = digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
    bool currentButtonState = prgButtonState || externalButtonState;

    if (currentButtonState && !buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
      lastButtonPress = millis();
      if (prgButtonState && externalButtonState) {
        lastButtonSource = BUTTON_SOURCE_BOTH;
        Serial.println(F("Both buttons pressed"));
      } else if (externalButtonState) {
        lastButtonSource = BUTTON_SOURCE_EXTERNAL;
        Serial.println(F("External button pressed"));
      } else if (prgButtonState) {
        lastButtonSource = BUTTON_SOURCE_PRG;
        Serial.println(F("PRG button pressed"));
      } else {
        lastButtonSource = BUTTON_SOURCE_NONE;
      }
      
      // Dismiss low battery warning on any button press
      if (isLowBatteryWarningActive && !inMenuMode) {
        isLowBatteryWarningActive = false;
        // Force immediate display update
        int batteryPct = getBatteryPercentage();
        renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
        
        // Reset button state
        buttonPressed = false;
        lastButtonSource = BUTTON_SOURCE_NONE;
        return; // Skip rest of button handling for this press
      }
      
      // Dismiss new job notification on any button press
      extern bool showNewJobNotification;
      if (showNewJobNotification && !inMenuMode) {
        showNewJobNotification = false;
        digitalWrite(RGB_LED, LOW);
        // Force immediate display update
        int batteryPct = getBatteryPercentage();
        renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
        
        // Reset button state
        buttonPressed = false;
        lastButtonSource = BUTTON_SOURCE_NONE;
        return; // Skip rest of button handling for this press
      }
      
      // Wake from screensaver/facts on any button press
        if ((isScreensaverActive || isBitcoinFactsActive) && !inMenuMode) {
        Serial.println(F("Waking from screensaver..."));
        isScreensaverActive = false;
        isBitcoinFactsActive = false;
        if (isDisplayOff) {
          VextON();
          delay(100);
          display.init();
          display.setFont(ArialMT_Plain_10);
          display.setTextAlignment(TEXT_ALIGN_LEFT);
          
          // Start WiFi reconnection in background (non-blocking)
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println(F("Starting WiFi reconnection..."));
            WiFi.mode(WIFI_STA);
            WiFi.begin();  // Uses saved credentials, connects in background
          }
        }
        // Disable WiFi power saving for responsive active use
        if (WiFi.status() == WL_CONNECTED) {
          WiFi.setSleep(false);
        }
        isDisplayOff = false;
        int batteryPct = getBatteryPercentage();
        renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
        Serial.println(F("Wake complete"));
        
        buttonPressed = false;
        lastButtonSource = BUTTON_SOURCE_NONE;
        return;
      }
    }
    
    if (!currentButtonState && buttonPressed) {
      unsigned long pressDuration = millis() - buttonPressTime;
      buttonPressed = false;
      ButtonSource triggeredSource = lastButtonSource;
      lastButtonSource = BUTTON_SOURCE_NONE;

      bool isVeryLongPress = pressDuration >= VERY_LONG_PRESS;
      bool isHoldPress = (pressDuration >= HOLD_PRESS_MIN) && !isVeryLongPress;
      bool isShortPress = pressDuration <= SHORT_PRESS_MAX;
      
      Serial.print(F("Button released - duration: "));
      Serial.print(pressDuration);
      Serial.println(F("ms"));

      if (isVeryLongPress) {
        if (triggeredSource == BUTTON_SOURCE_EXTERNAL) {
          inMenuMode = false;
          if (confirmFactoryReset()) {
            handleFactoryReset();
          }
        }
        return;
      }

// Handle onboarding navigation
      if (onboardingStep > 0) {
        if (isShortPress) {
          if (onboardingStep < 4) {
            // Advance to next step
            onboardingStep++;
            playButtonChirp();
            renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
          } else {
            // Last step - complete onboarding
            onboardingStep = 0;
            playMenuSelectTone();
            // Go to normal pet screen
            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          }
        }
        return; // Don't process other button actions during onboarding
      }

      if (inMenuMode) {
        if (isHoldPress) {
          int selectedOption = currentMenuOption;
          playMenuSelectTone();
          inMenuMode = false;
          currentMenuOption = 0;

          if (selectedOption == 0) {
            // Home - refresh main screen
            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          } else if (selectedOption == 1) {
            extern int handleLightningGame(SSD1306Wire &display);
            handleLightningGame(display);
            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
                  } else if (selectedOption == 2) {
            bool inFoodMenu = true;
            int selectedFood = 0;
            int foodOptionCount = getFoodOptionCount();
            int totalFoodItems = foodOptionCount + 1;  // +1 for Back option
            unsigned long lastFoodCycle = millis();
            renderFoodSelectionMenu(display, selectedFood);

            while (inFoodMenu) {
              unsigned long now = millis();

              if (now - lastFoodCycle > 30000) { // 30 seconds timeout (was 10)
                inFoodMenu = false;
                break;
              }

              bool prgStateFood = digitalRead(BUTTON_PIN_PRG) == LOW;
              bool externalStateFood = digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
              bool currentStateFood = prgStateFood || externalStateFood;

              if (currentStateFood && !buttonPressed) {
                buttonPressed = true;
                buttonPressTime = millis();
              }

              if (!currentStateFood && buttonPressed) {
                unsigned long foodPressDuration = millis() - buttonPressTime;
                buttonPressed = false;

                bool foodShort = foodPressDuration <= SHORT_PRESS_MAX;
                bool foodHold = (foodPressDuration >= HOLD_PRESS_MIN) && (foodPressDuration < VERY_LONG_PRESS);

                if (foodShort) {
                  selectedFood = (selectedFood + 1) % totalFoodItems;
                  lastFoodCycle = millis();
                  playButtonChirp();
                  renderFoodSelectionMenu(display, selectedFood);
                } else if (foodHold) {
                  // Check if Back is selected
                  if (selectedFood == foodOptionCount) {
                    // Back selected - exit food menu
                    playMenuSelectTone();
                    inFoodMenu = false;
                  } else {
                    int foodCost = getFoodCostByIndex(selectedFood);
                    inFoodMenu = false;
                    playMenuSelectTone();
                    
                    // Check local coin balance (not stale server balance)
                    extern int getLocalCoins();
                    int localCoins = getLocalCoins();
                    bool hadEnoughCoins = localCoins >= foodCost;
                    bool success = false;
                    if (hadEnoughCoins) {
                      success = handleFeedPet(selectedFood);
                    }
                    renderFeedResult(display, success, selectedFood, hadEnoughCoins);
                    delay(2500); // Show result for 2.5 seconds
                  }
                }
              }

              delay(45);
            }

            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          } else if (selectedOption == 3) {
            // Jobs - browse open jobs from user's groups
            extern void handleJobsMenu(SSD1306Wire &display);
            handleJobsMenu(display);
            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          }
        } else if (isShortPress) {
          currentMenuOption = (currentMenuOption + 1) % 4;  // 4 menu items: Home, Play, Feed, Jobs
          lastMenuCycle = millis();
          playButtonChirp();
          renderMenu(display, currentMenuOption);
        }
      } else {
        static bool justWokeUp = false;
        if (isScreensaverActive) {
          justWokeUp = true;
          return;
        }

        if (justWokeUp && isShortPress) {
          justWokeUp = false;
          return;
        }
        justWokeUp = false;

        // Only allow menu/game access if device is paired
        if (isPaired) {
          if (isShortPress) {
            // Short press → open menu
            inMenuMode = true;
            currentMenuOption = 0;
            lastMenuCycle = millis();
            playButtonChirp();
            renderMenu(display, currentMenuOption);
          } else if (isHoldPress) {
            // Hold press → WiFi setup (only if disconnected)
            if (WiFi.status() != WL_CONNECTED) {
              Serial.println(F("📶 Hold press + no WiFi - entering config portal"));
              playMenuSelectTone();
              enterWifiConfigPortal();
              int batteryPct = getBatteryPercentage();
              renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
            } else {
              // Connected to WiFi - just open menu like short press
              inMenuMode = true;
              currentMenuOption = 0;
              lastMenuCycle = millis();
              playButtonChirp();
              renderMenu(display, currentMenuOption);
            }
          }
        }
      }
    }
    
    // Auto-exit menu after 10 seconds of inactivity
    if (inMenuMode && (millis() - lastMenuCycle > 10000)) {
      inMenuMode = false;
      int batteryPct = getBatteryPercentage();
      renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
    }
    
    // If in menu mode, keep rendering menu
    if (inMenuMode) {
      static unsigned long lastMenuRender = 0;
      if (millis() - lastMenuRender > 500) { // Update menu display every 500ms
        renderMenu(display, currentMenuOption);
        lastMenuRender = millis();
      }
    }
    
    // Let system tasks run (prevents WiFi stack from blocking)
    yield();
  }

  void playButtonChirp() {
    tone(BUZZER_PIN, 880, 80);
    delay(60);
    tone(BUZZER_PIN, 1319, 80);
    delay(60);
    noTone(BUZZER_PIN);
  }

  void playMenuSelectTone() {
    tone(BUZZER_PIN, 988, 100);
    delay(90);
    tone(BUZZER_PIN, 1319, 160);
    delay(150);
    noTone(BUZZER_PIN);
  }

  // Check if current time is during quiet hours (8pm-8am)
  // Returns true if sounds should be suppressed
  bool isQuietHours() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {  // 10ms timeout
      int hour = timeinfo.tm_hour;
      // Quiet hours: 8pm (20:00) to 8am (08:00)
      return (hour >= 20 || hour < 8);
    }
    // If time not available, don't suppress sounds
    return false;
  }

  void playSadSound() {
    if (isQuietHours()) return;  // Silent during quiet hours
    tone(BUZZER_PIN, 659, 200);
    delay(180);
    tone(BUZZER_PIN, 587, 200);
    delay(180);
    tone(BUZZER_PIN, 523, 300);
    delay(280);
    noTone(BUZZER_PIN);
  }

  void playDeathSound() {
    if (isQuietHours()) return;  // Silent during quiet hours
    tone(BUZZER_PIN, 523, 250);
    delay(230);
    tone(BUZZER_PIN, 440, 250);
    delay(230);
    tone(BUZZER_PIN, 349, 250);
    delay(230);
    tone(BUZZER_PIN, 262, 500);
    delay(480);
    noTone(BUZZER_PIN);
  }

  void playLowBatterySound() {
    if (isQuietHours()) return;  // Silent during quiet hours
    tone(BUZZER_PIN, 880, 150);
    delay(140);
    tone(BUZZER_PIN, 659, 150);
    delay(140);
    tone(BUZZER_PIN, 440, 200);
    delay(190);
    noTone(BUZZER_PIN);
  }

  void playNewJobChirp() {
    if (isQuietHours()) return;  // Silent during quiet hours
    // Distinct attention-getting chirp - two rising tones
    tone(BUZZER_PIN, 784, 150);  // G5
    delay(130);
    tone(BUZZER_PIN, 988, 150);  // B5
    delay(130);
    tone(BUZZER_PIN, 1175, 200); // D6
    delay(180);
    noTone(BUZZER_PIN);
  }

  void renderLowBatteryWarning(SSD1306Wire &display, int batteryPercent) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    
    // Battery icon area (simple representation)
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 8, "Low Battery");
    
    // Large battery percentage
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 28, String(batteryPercent) + "%");
    
    // Instruction
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 54, "Please charge soon");
    
    display.display();
  }

  void renderDeathWarning(SSD1306Wire &display, String petName) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 4, petName + " died!");
    
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 26, "Hunger and happiness");
    display.drawString(64, 38, "reached zero.");
    
    display.drawString(64, 54, "Feed & play to revive!");
    
    display.display();
  }

  void renderHungerWarning(SSD1306Wire &display, String petName, int fullness) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 4, petName + " is hungry!");
    
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 24, String(fullness) + "%");
    
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 54, "Feed " + petName + " soon!");
    
    display.display();
  }

  void renderSadnessWarning(SSD1306Wire &display, String petName, int happiness) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    
    display.setFont(ArialMT_Plain_16);
    display.drawString(64, 4, petName + " is sad!");
    
    display.setFont(ArialMT_Plain_24);
    display.drawString(64, 24, String(happiness) + "%");
    
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 54, "Play with " + petName + "!");
    
    display.display();
  }

    void renderOnboardingStep(SSD1306Wire &display, int step, String petName) {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_10);
    
    int textY = 4;
    int lineHeight = 10;
    
    switch(step) {
      case 1:
        display.drawString(0, textY, petName + " is your");
        textY += lineHeight;
        display.drawString(0, textY, "new pet. It likes");
        textY += lineHeight;
        display.drawString(0, textY, "to play and eat.");
        break;
        
      case 2:
        display.drawString(0, textY, "Earn bitcoin by");
        textY += lineHeight;
        display.drawString(0, textY, "doing jobs. The sats");
        textY += lineHeight;
        display.drawString(0, textY, "you earn give coins");
        textY += lineHeight;
        display.drawString(0, textY, "to keep " + petName + " fed");
        textY += lineHeight;
        display.drawString(0, textY, "and happy.");
        break;
        
      case 3:
        display.drawString(0, textY, "Play games to keep");
        textY += lineHeight;
        display.drawString(0, textY, petName + " happy. If you");
        textY += lineHeight;
        display.drawString(0, textY, "don't play, happiness");
        textY += lineHeight;
        display.drawString(0, textY, "goes to zero after");
        textY += lineHeight;
        display.drawString(0, textY, "4 days.");
        break;
        
      case 4:
        display.drawString(0, textY, "Feed " + petName + " treats.");
        textY += lineHeight;
        display.drawString(0, textY, "If you don't feed");
        textY += lineHeight;
        display.drawString(0, textY, petName + ", fullness goes");
        textY += lineHeight;
        display.drawString(0, textY, "to zero after");
        textY += lineHeight;
        display.drawString(0, textY, "3 days.");
        break;
    }
    
    // Draw step indicator at bottom right
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 54, String(step) + "/4");
    
    display.display();
}

  void renderFactoryResetPrompt(int selectedOption) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 4, "Factory Reset?");
    display.drawString(64, 16, "Erase pairing & data");

    display.setTextAlignment(TEXT_ALIGN_LEFT);
    const char* options[2] = {"No", "Yes"};
    int baseY = 34;
    for (int i = 0; i < 2; i++) {
      String prefix = (i == selectedOption) ? "> " : "  ";
      display.setFont(ArialMT_Plain_10);
      display.drawString(26, baseY + (i * 14), prefix + String(options[i]));
    }

    display.display();
  }

  bool confirmFactoryReset() {
    int selectedOption = 0; // 0 = No (default), 1 = Yes
    bool confirmButtonPressed = false;
    unsigned long pressStart = 0;
    unsigned long lastInteraction = millis();
    const unsigned long timeoutMs = 15000; // 15 seconds

    renderFactoryResetPrompt(selectedOption);

    while (millis() - lastInteraction < timeoutMs) {
      bool externalState = digitalRead(BUTTON_PIN_EXTERNAL) == LOW;
      bool prgState = digitalRead(BUTTON_PIN_PRG) == LOW;

      if (prgState) {
        return false;
      }

      if (externalState && !confirmButtonPressed) {
        confirmButtonPressed = true;
        pressStart = millis();
      }

      if (!externalState && confirmButtonPressed) {
        unsigned long duration = millis() - pressStart;
        confirmButtonPressed = false;
        lastInteraction = millis();

        bool shortPress = duration <= SHORT_PRESS_MAX;
        bool holdPress = duration >= HOLD_PRESS_MIN;

        if (shortPress) {
          selectedOption = (selectedOption + 1) % 2;
          playButtonChirp();
          renderFactoryResetPrompt(selectedOption);
        } else if (holdPress) {
          if (selectedOption == 1) {
            playMenuSelectTone();
            return true;
          } else {
            playButtonChirp();
            return false;
          }
        }
      }

      delay(45);
    }

    return false;
  }

  void handleForceSync() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.drawString(64, 25, "Syncing...");
    display.display();
    
    fetchGanamosConfig();
    
    display.clear();
    display.drawString(64, 25, "Synced!");
    display.display();
    delay(1000);
  }

  void handleFactoryReset() {
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 15, "Factory Reset");
    display.drawString(64, 30, "Clearing memory...");
    display.display();
    
    extern void clearEconomyData();
    clearDeviceConfig();
    clearEconomyData(); // Also clear economy/pending spends
    
    display.clear();
    display.drawString(64, 20, "Reset complete!");
    display.drawString(64, 35, "Rebooting...");
    display.display();
    delay(2000);
    
    ESP.restart(); // Reboot the device
  }