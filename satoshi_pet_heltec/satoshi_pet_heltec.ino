  #include <Wire.h>
  #include <WiFi.h>
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
  #include "espnow_comm.h"
  #include "pickleball.h"
#include "game_rooms.h"
  #include <esp_task_wdt.h>  // Watchdog timer support (framework auto-initializes)
  #include <esp_idf_version.h>
  #include <esp_sleep.h>
  #include <esp_wifi.h>
  #include <driver/gpio.h>
  #include "sound.h"
  #include "hold_press.h"

  // Keep declarations explicit so this sketch builds consistently with both
  // Arduino IDE and non-IDE toolchains such as PlatformIO/Wokwi.
  bool isQuietHours();
  void playLowBatterySound();
  void renderLowBatteryWarning(SSD1306Wire &display, int batteryPercent);
  void renderOnboardingStep(SSD1306Wire &display, int step, String petName);
  void renderDeathWarning(SSD1306Wire &display, String petName);
  void renderHungerWarning(SSD1306Wire &display, String petName, int fullness);
  void renderSadnessWarning(SSD1306Wire &display, String petName, int happiness);
  void handleFactoryReset();
  void playButtonChirp();
  void playMenuSelectTone();
  void VextON();
  void playDeathSound();
  void playSadSound();
  void wifiRadioOff();
  float getBatteryVoltage();
  void wifiStartConnect();
  void wifiService();
  bool wifiReady();
  void invalidateRender();
  void playMenuSelectToneBlocking();
  void readButtons(bool &prgDown, bool &externalDown);

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

  #ifdef WOKWI_SIMULATOR
  // Wokwi's standalone SSD1306 part has no reset pin. The real Heltec OLED
  // does, so preserve the hardware behavior in the normal build.
  SSD1306Wire display(0x3c, 400000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, -1);
  #else
  SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);
  #endif

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
  // Server pollInterval is in seconds; clamp so config can't create a battery-heavy loop.
  const long DEFAULT_POLL_INTERVAL_S = 60;
  const long MIN_POLL_INTERVAL_S = 60;
  const long MAX_POLL_INTERVAL_S = 3600;
  // Display off/screensaver: check every 5 min so new jobs and earned sats
  // wake the pet (each check is a ~1 s fast reconnect + one request)
  const unsigned long IDLE_POLL_INTERVAL_MS = 300000UL;
  const unsigned long DECAY_CHECK_INTERVAL_MS = 60000UL;
  bool buttonPressed = false;
  bool consumeButtonUntilRelease = false;
  bool lastSyncSucceeded = false;
  bool syncAttempted = false;  // a sync has been tried since boot (success or not)
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
  // Non-blocking WiFi connection state (driven by WiFi.onEvent + wifiService())
  volatile bool wifiHasIp = false;
  volatile bool wifiDisconnectEvt = false;
  bool wifiConnecting = false;
  bool wifiFastAttempt = false;
  unsigned long wifiConnectStart = 0;
  const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;
  const unsigned long WIFI_FAST_CONNECT_TIMEOUT_MS = 4000;
  bool syncPending = false;            // A scheduled sync is waiting for an IP
  unsigned long lastDecayCheck = 0;
  unsigned long lastBatteryCheck = 0;
  // Debounced buttons (button_handler.h)
  DebouncedButton dbPrg;
  DebouncedButton dbExt;
  // Redraw throttling for the main render chain
  uint32_t lastRenderKey = 0;
  unsigned long lastRenderAt = 0;
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
#ifdef WOKWI_SIMULATOR
  // Accelerate power-state transitions so sleep/wake can be covered quickly.
  const unsigned long SCREENSAVER_TIMEOUT = 15000;
  const unsigned long BITCOIN_FACTS_TIMEOUT = 30000;
  const unsigned long SCREENSAVER_DISPLAY_OFF_TIMEOUT = 45000;
  const unsigned long BATTERY_CHECK_INTERVAL = 5000;
#else
  const unsigned long SCREENSAVER_TIMEOUT = 30000; // Dim after 30 seconds
  const unsigned long BITCOIN_FACTS_TIMEOUT = 45000; // Brief facts screen
  const unsigned long SCREENSAVER_DISPLAY_OFF_TIMEOUT = 60000; // Off after one minute
  const unsigned long BATTERY_CHECK_INTERVAL = 60000;
#endif
  float cachedBatteryVoltage = 0.0f;
  unsigned long lastBatteryVoltageSample = 0;
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

  const unsigned long HOLD_PRESS_MIN = HOLD_SELECT_MS;
  const unsigned long SHORT_PRESS_MAX = HOLD_PRESS_MIN - 1;
  const unsigned long VERY_LONG_PRESS = 10000; // 10 seconds to prevent accidental factory reset

  bool notificationActive = false;
  bool notificationReturnToSleep = false;
  unsigned long notificationStarted = 0;
  const unsigned long NOTIFICATION_DURATION = 6000;
  int petWarning = 0;

  void wakeForNotification() {
    notificationReturnToSleep |= isScreensaverActive || isDisplayOff;
    notificationActive = true;
    notificationStarted = millis();
    if (isDisplayOff) {
      VextON();
      delay(100);
      display.init();
    }
    isDisplayOff = isScreensaverActive = isBitcoinFactsActive = false;
    setOLEDContrast(NORMAL_BRIGHTNESS);
  }

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

  // Charging detection requires a hardware status signal, not a voltage guess.
  // A LiPo never reads below ~3 V while it can power the board, so a reading
  // under 2.5 V means no battery voltage reaches the sense pin (no battery
  // plugged in, or a bad battery lead) and the board is running on USB.
  bool batteryPresent() {
    return getBatteryVoltage() >= 2.5f;
  }

  bool isBatteryCharging() {
    // Voltage alone cannot distinguish a full battery from active charging.
    return false;
  }

  // Helper function to read battery once
  float readVbatOnce() {
    // Use calibrated analogReadMilliVolts for better accuracy
    float v_meas = analogReadMilliVolts(VBAT_PIN) / 1000.0;
    return v_meas * 4.9;  // 100kΩ/(100kΩ+390kΩ) divider = multiply by 4.9
  }

  float getBatteryVoltage() {
    unsigned long now = millis();
    if (cachedBatteryVoltage > 0.0f &&
        now - lastBatteryVoltageSample < BATTERY_CHECK_INTERVAL) {
      return cachedBatteryVoltage;
    }

    // Brief wait for voltage to settle after any WiFi transmission
    delay(10);
    
    // Re-initialize ADC config (can reset after WiFi init or deep sleep)
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    
    // Heltec's V3 ADC control is active-high: enable it only while sampling.
    pinMode(ADC_CTRL, OUTPUT);
    digitalWrite(ADC_CTRL, HIGH);
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
    cachedBatteryVoltage = voltage;
    lastBatteryVoltageSample = now;

    // Disable the divider between samples so its resistor
    // path does not continuously drain the battery.
    digitalWrite(ADC_CTRL, LOW);
    
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

  // Melodies are static so the non-blocking sound player can keep pointers.
  // freq 0 = rest. Quiet hours are enforced inside soundPlay().
  static const ToneNote SATS_EARNED_MELODY[] = {
    {523, 100}, {0, 20}, {587, 100}, {0, 20}, {659, 100}, {0, 20},
    {784, 150}, {0, 20}, {880, 150}, {0, 20}, {1047, 300}
  };
  static const ToneNote FIX_REJECTED_MELODY[] = {
    {880, 150}, {0, 30}, {784, 150}, {0, 30}, {659, 150}, {0, 30},
    {587, 200}, {0, 30}, {523, 200}, {0, 30}, {392, 400}
  };
  static const ToneNote BUTTON_CHIRP[] = {{880, 60}, {1319, 60}};
  static const ToneNote MENU_SELECT_TONE[] = {{988, 90}, {1319, 150}};
  static const ToneNote SAD_MELODY[] = {{659, 180}, {587, 180}, {523, 280}};
  static const ToneNote DEATH_MELODY[] = {{523, 230}, {440, 230}, {349, 230}, {262, 480}};
  static const ToneNote LOW_BATTERY_MELODY[] = {{880, 140}, {659, 140}, {440, 190}};
  static const ToneNote NEW_JOB_CHIRP[] = {{784, 130}, {988, 130}, {1175, 180}};
  #define NOTE_COUNT(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

  void playSatsEarnedSound() {
    // Uplifting 8-bit style melody for earning sats
    soundPlay(SATS_EARNED_MELODY, NOTE_COUNT(SATS_EARNED_MELODY));
  }

  void playFixRejectedSound() {
    // Sad descending tone - opposite of the celebration melody
    soundPlay(FIX_REJECTED_MELODY, NOTE_COUNT(FIX_REJECTED_MELODY));
  }

  void displayPairingCode() {
  #ifdef DEBUG_LOGGING
    Serial.print(F("Pairing code: "));
    Serial.println(maskForLog(pairingCode));
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
    strncpy_P(factBuffer, (char*)pgm_read_ptr(&(bitcoin_facts[factIndex])), sizeof(factBuffer) - 1);
    factBuffer[sizeof(factBuffer) - 1] = '\0';

    // Word wrap in place (max ~18 chars per line for 92px width, 4 lines).
    const int textX = 36;
    const int lineHeight = 11;
    const int startY = 6;
    const int maxCharsPerLine = 18;
    const int len = strlen(factBuffer);
    char line[maxCharsPerLine + 1];
    int pos = 0;

    for (int currentLine = 0; currentLine < 4 && pos < len; currentLine++) {
      while (pos < len && factBuffer[pos] == ' ') pos++;  // Skip leading spaces
      int end = min(pos + maxCharsPerLine, len);
      // Break at the last space if the line would split a word
      if (end < len && factBuffer[end] != ' ') {
        for (int i = end - 1; i > pos; i--) {
          if (factBuffer[i] == ' ') { end = i; break; }
        }
      }
      int lineLen = end - pos;
      while (lineLen > 0 && factBuffer[pos + lineLen - 1] == ' ') lineLen--;
      memcpy(line, factBuffer + pos, lineLen);
      line[lineLen] = '\0';
      display.drawString(textX, startY + (currentLine * lineHeight), line);
      pos = end;
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

  // ===== Non-blocking WiFi =====
  // Runs in the WiFi event task: only set flags here.
  void onWifiEvent(arduino_event_id_t event) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        wifiHasIp = true;
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        wifiHasIp = false;
        wifiDisconnectEvt = true;
        break;
      default:
        break;
    }
  }

  bool wifiReady() {
    return wifiHasIp && WiFi.status() == WL_CONNECTED;
  }

  // Turn the radio fully off and drop the shared TLS connection.
  void wifiRadioOff() {
    ganamosHttpReset();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnecting = false;
    wifiHasIp = false;
  }

  // Issue one WiFi.begin(). Fast path pins the cached BSSID/channel (skips the
  // full scan); the fallback uses the stored SSID/password without pinning so
  // the driver config is unpinned again if the AP changed.
  void wifiBeginAttempt(bool allowFast) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    wifiFastAttempt = false;
  #ifdef WOKWI_SIMULATOR
    (void)allowFast;
    WiFi.begin("Wokwi-GUEST", "", 6);
  #else
    // Credentials saved by WiFiManager live in the WiFi driver's NVS config.
    char ssid[33] = {0};
    char pass[65] = {0};
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
      memcpy(ssid, conf.sta.ssid, sizeof(conf.sta.ssid));
      memcpy(pass, conf.sta.password, sizeof(conf.sta.password));
    }
    uint8_t bssid[6];
    int32_t channel = 0;
    if (ssid[0] == '\0') {
      WiFi.begin();
    } else if (allowFast && loadWifiFastConnect(bssid, channel)) {
      wifiFastAttempt = true;
      WiFi.begin(ssid, pass[0] ? pass : nullptr, channel, bssid);
    } else {
      WiFi.begin(ssid, pass[0] ? pass : nullptr);
    }
  #endif
    wifiDisconnectEvt = false;
    wifiConnectStart = millis();
    wifiConnecting = true;
  }

  // Start a background connect. No-op while connected or already connecting,
  // so it never races a pending WiFi.begin().
  void wifiStartConnect() {
    if (wifiConnecting || wifiReady()) return;
    Serial.println(F("📶 Starting WiFi connect (non-blocking)"));
    wifiBeginAttempt(true);
  }

  // Start a connect for scheduled work, honoring exponential backoff.
  bool wifiRequestConnect(unsigned long now) {
    if (wifiConnecting) return true;
    if (lastWifiReconnectAttempt != 0 && now - lastWifiReconnectAttempt < getWifiBackoffDelay()) {
      return false;
    }
    wifiReconnectAttempts++;
    lastWifiReconnectAttempt = now;
    Serial.print(F("📶 WiFi reconnect attempt #"));
    Serial.println(wifiReconnectAttempts);
    if (wifiReconnectAttempts >= WIFI_MAX_QUICK_RETRIES && wifiReconnectAttempts % 5 == 0) {
      Serial.println(F("💡 Tip: Hold the button to enter WiFi setup"));
    }
    wifiStartConnect();
    return wifiConnecting;
  }

  // Advance the connect state machine; call every loop iteration.
  void wifiService() {
    if (!wifiConnecting) {
      if (wifiDisconnectEvt) {
        wifiDisconnectEvt = false;
        ganamosHttpReset();  // Connection dropped: the TLS session is dead
      }
      return;
    }
    unsigned long now = millis();
    if (wifiReady()) {
      wifiConnecting = false;
      wifiReconnectAttempts = 0;
      Serial.print(F("✅ WiFi connected in "));
      Serial.print(now - wifiConnectStart);
      Serial.print(F("ms: "));
      Serial.println(WiFi.localIP());
  #ifndef WOKWI_SIMULATOR
      saveWifiFastConnect(WiFi.BSSID(), WiFi.channel());
  #endif
      WiFi.setSleep(WIFI_PS_MAX_MODEM);
      if (time(nullptr) < 1600000000) startTimeSync();  // Retry SNTP now that we have a route
      return;
    }
    unsigned long elapsed = now - wifiConnectStart;
    if (wifiFastAttempt &&
        (elapsed > WIFI_FAST_CONNECT_TIMEOUT_MS || (wifiDisconnectEvt && elapsed > 300))) {
      Serial.println(F("📶 Fast reconnect failed - falling back to full scan"));
      WiFi.disconnect(false);
      wifiBeginAttempt(false);
      return;
    }
    if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println(F("❌ WiFi connect timed out"));
      wifiRadioOff();  // Save power until the next attempt
    }
  }

  bool wifiEnsureConnected(unsigned long timeoutMs) {
    if (wifiReady()) return true;
    if (!wifiConnecting) {
      lastWifiReconnectAttempt = millis();
      wifiStartConnect();
    }
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
      wifiService();
      if (wifiReady()) return true;
      if (!wifiConnecting) return false;
      esp_task_wdt_reset();
      soundUpdate();
      delay(50);
    }
    return wifiReady();
  }

  // ===== Redraw throttling =====
  // The render chain only redraws when the screen identity/content key changes
  // or its refresh period elapses (static screens: slow self-heal refresh;
  // animated screens: ~8 fps).
  const unsigned long STATIC_SCREEN_REFRESH_MS = 2000;
  const unsigned long ANIMATED_FRAME_MS = 125;
  enum RenderScreen {
    SCREEN_LOW_BATTERY = 1, SCREEN_PET_WARNING, SCREEN_NEW_JOB, SCREEN_REJECTION,
    SCREEN_FACT, SCREEN_SCREENSAVER, SCREEN_CELEBRATION, SCREEN_ONBOARDING, SCREEN_PET
  };

  uint32_t renderKey(uint32_t screen, uint32_t content) {
    return (screen << 24) | (content & 0xFFFFFFUL);
  }

  void invalidateRender() {
    lastRenderKey = 0;
  }

  bool renderDue(uint32_t key, unsigned long now, unsigned long refreshMs) {
    if (key != lastRenderKey || now - lastRenderAt >= refreshMs) {
      lastRenderKey = key;
      lastRenderAt = now;
      return true;
    }
    return false;
  }

  // Debounced, active-low button levels.
  void readButtons(bool &prgDown, bool &externalDown) {
    unsigned long now = millis();
    debounceUpdate(dbPrg, now);
    debounceUpdate(dbExt, now);
    prgDown = debounceIsDown(dbPrg);
    externalDown = debounceIsDown(dbExt);
  }

  bool anyButtonDown() {
    bool prgDown, externalDown;
    readButtons(prgDown, externalDown);
    return prgDown || externalDown;
  }

  // Poll interval: server value is seconds, clamped; idle polls at least every 5 minutes.
  unsigned long currentPollIntervalMs() {
    long seconds = ganamosConfig.pollInterval > 0 ? ganamosConfig.pollInterval : DEFAULT_POLL_INTERVAL_S;
    seconds = constrain(seconds, MIN_POLL_INTERVAL_S, MAX_POLL_INTERVAL_S);
    unsigned long intervalMs = (unsigned long)seconds * 1000UL;
    if (isScreensaverActive && intervalMs < IDLE_POLL_INTERVAL_MS) {
      intervalMs = IDLE_POLL_INTERVAL_MS;
    }
    return intervalMs;
  }

  unsigned long msUntil(unsigned long last, unsigned long interval, unsigned long now) {
    unsigned long elapsed = now - last;
    return elapsed >= interval ? 0 : interval - elapsed;
  }

  // Time until the next scheduled job (poll, decay tick, battery sample).
  unsigned long msUntilNextScheduledJob(unsigned long now) {
    unsigned long next = msUntil(lastDecayCheck, DECAY_CHECK_INTERVAL_MS, now);
    next = min(next, msUntil(lastBatteryCheck, BATTERY_CHECK_INTERVAL, now));
    if (isPaired) {
      next = min(next, msUntil(lastUpdate, currentPollIntervalMs(), now) + 1);
    }
    return constrain(next, 20UL, DECAY_CHECK_INTERVAL_MS);
  }

  // A titled tap/hold list (tap = next, hold = pick). Returns the picked
  // index, or -1 after 30 s without input or when the last item is "Back".
  int runListMenu(const char* title, const char* const* options, int count) {
    int selected = 0;
    unsigned long lastInput = millis();
    auto render = [&]() {
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_LEFT);
      display.drawString(0, 0, title);
      display.drawLine(0, 12, 127, 12);
      for (int i = 0; i < count; i++) {
        display.drawString(4, 15 + i * 12, String(i == selected ? "> " : "  ") + options[i]);
      }
      display.display();
    };
    render();
    while (millis() - lastInput < 30000) {
      PressEvent ev = holdPressUpdate(anyButtonDown());
      if (holdPressActive()) lastInput = lastButtonPress = millis();
      if (ev == PRESS_TAP) {
        lastInput = lastButtonPress = millis();
        selected = (selected + 1) % count;
        playButtonChirp();
        render();
      } else if (ev == PRESS_HOLD) {
        lastButtonPress = millis();
        bool back = strcmp(options[selected], "Back") == 0;
        if (back) playMenuSelectTone();
        return back ? -1 : selected;
      }
      holdPressDrawBar(display);
      esp_task_wdt_reset();
      soundUpdate();
      delay(20);
    }
    return -1;
  }

  void applyGameHappinessBonus(int bonus) {
    if (bonus <= 0) return;
    int oldHappiness = petStats.happiness;
    petStats.happiness = min(100, petStats.happiness + bonus);
    savePetStats();
    Serial.println("Game bonus: Happiness " + String(oldHappiness) + " → " + String(petStats.happiness));
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
    ganamosHttpReset();
    WiFi.disconnect(true);
    wifiConnecting = false;
    delay(100);
    
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);  // 3 minute timeout
    wm.setConnectTimeout(30);
    
    // This will block until connected or timeout
    bool connected = wm.startConfigPortal("SatoshiPet-Setup");
    
    if (connected) {
      Serial.println(F("✅ WiFi connected via config portal!"));
      wifiReconnectAttempts = 0;
      clearWifiFastConnect();  // New network: re-learn BSSID/channel
      saveWifiFastConnect(WiFi.BSSID(), WiFi.channel());
      WiFi.setSleep(WIFI_PS_MAX_MODEM);
      lastWifiReconnectAttempt = 0;
      
      display.clear();
      display.setFont(ArialMT_Plain_16);
      display.drawString(64, 25, "Connected!");
      display.display();
      delay(1500);
    } else {
      Serial.println(F("❌ Config portal timed out"));
      wifiRadioOff();  // Turn off to save power
    }
    
    wifiConfigPortalRequested = false;
    
    // Re-enable watchdog
    esp_task_wdt_add(NULL);
    Serial.println(F("🐕 Watchdog re-enabled"));
  }

  void setup() {
    // 80 MHz is ample for the UI, WiFi and TLS and cuts active current.
    setCpuFrequencyMhz(80);
    Serial.begin(115200);
    Serial.println(F("Satoshi Pet boot"));

    // Track WiFi link state from driver events (non-blocking connect logic).
    WiFi.onEvent(onWifiEvent);
    
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
    soundInit();  // Buzzer: GPIO 48 (+GND), or GPIO 46 + 45 on case v2 wiring
    
    // Configure ADC for battery reading
    analogReadResolution(12);  // 12-bit resolution (0-4095)
    analogSetAttenuation(ADC_11db);  // Full scale ~3.3V

      // FIRST: Check if device is already paired (before WiFi setup)
    // This determines whether we need mandatory WiFi setup or can try quick reconnect
    hadSavedConfigOnBoot = loadDeviceConfig();
#ifdef WOKWI_SIMULATOR
    // Wokwi is an isolated functional test fixture. Seed a paired device so the
    // complete UI can be exercised without a phone, personal WiFi, or production
    // account. This block is compiled out of physical Heltec firmware.
    hadSavedConfigOnBoot = true;
    pairingCode = "SIM-TEST";
    ganamosConfig.deviceId = "wokwi-simulator";
    ganamosConfig.petName = "Satoshi";
    ganamosConfig.petType = "cat";
    ganamosConfig.userName = "Simulator";
    ganamosConfig.balance = 12345;
    ganamosConfig.coins = 1000;
    ganamosConfig.btcPrice = 100000.0;
    ganamosConfig.pollInterval = DEFAULT_POLL_INTERVAL_S;
#endif
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
#ifndef WOKWI_SIMULATOR
        saveWifiFastConnect(WiFi.BSSID(), WiFi.channel());
#endif
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
      // Fast path pins the cached BSSID/channel; wifiService() falls back to
      // a full scan and gives up after the connect timeout (radio off).
      Serial.println(F("Device paired - trying quick WiFi reconnect..."));
      wifiStartConnect();
      while (wifiConnecting) {
        wifiService();
        delay(50);
      }
      
      if (wifiReady()) {
        Serial.print(F("WiFi connected: "));
        Serial.println(WiFi.localIP());
      } else {
        Serial.println(F("WiFi unavailable - continuing offline"));
        wifiRadioOff();
      }
    }
    if (wifiReady()) {
      // Max modem sleep keeps the association while the radio idles.
      WiFi.setSleep(WIFI_PS_MAX_MODEM);
    }

    // Initialize NTP with a POSIX TZ (DST-aware); server config may override it.
    startTimeSync();

    initPet("default");
    
    // Initialize economy system
    extern void initEconomy();
    extern void loadPetStats();
    extern void setLocalCoins(int coins);
    extern int getPendingSpendCount();
    extern void clearEconomyData();
    
    initEconomy();
#ifdef WOKWI_SIMULATOR
    // Give simulator tests enough local currency to exercise games and feeding.
    setLocalCoins(1000);
#endif
    
    // (Removed the boot-time wipe when >10 spends were pending: queue entries
    //  are checksum-verified on load, and 11+ is a normal offline backlog.)
    
    loadPetStats();
    
    // Now handle pairing - WiFi is guaranteed to be connected for unpaired devices
    if (hadSavedConfigOnBoot) {
      Serial.println("Found saved config! Device already paired.");
      Serial.println("Device ID: " + maskForLog(ganamosConfig.deviceId));
      Serial.println("Pet name: " + ganamosConfig.petName);
      Serial.println("Pairing code: " + maskForLog(pairingCode));
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
      Serial.println(maskForLog(pairingCode));
      displayPairingCode();
      isPaired = false;
    }
    // Initialize buttons using button_handler module
    initButtons();
    debounceInit(dbPrg, BUTTON_PIN_PRG);
    debounceInit(dbExt, BUTTON_PIN_EXTERNAL);
#ifndef WOKWI_SIMULATOR
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN_PRG, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN_EXTERNAL, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
#endif
    
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
    #if ESP_IDF_VERSION_MAJOR >= 5
      esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 15000,           // 15 second timeout
        .idle_core_mask = 0,           // Don't watch idle tasks
        .trigger_panic = true          // Panic (reset) on timeout
      };
      esp_task_wdt_reconfigure(&wdt_config);
    #else
      // Arduino-ESP32 2.x / ESP-IDF 4.x uses the legacy seconds-based API.
      esp_task_wdt_init(15, true);
    #endif
    esp_task_wdt_add(NULL);                  // Add current task (main loop)
    Serial.println("🐕 Watchdog initialized (15s timeout)");

    Serial.println("Setup complete!");
  }

  void logCurrentPairingState() {
    Serial.println("🔍 Current pairing snapshot:");
    Serial.println("  deviceId: " + maskForLog(ganamosConfig.deviceId));
    Serial.println("  pairingCode: " + maskForLog(pairingCode));
    Serial.println("  petName: " + ganamosConfig.petName);
    Serial.println("  petType: " + ganamosConfig.petType);
    Serial.println("  balance: " + String(ganamosConfig.balance));
    Serial.println("  coins: " + String(ganamosConfig.coins));
  }

  int cachedBatteryPct = -1; // Cached battery percentage (-1 = not initialized)

  // One config sync + economy reconciliation. Requires an IP (wifiReady()).
  // The render chain in loop() redraws afterwards.
  void runConfigSync() {
    unsigned long fetchStart = millis();
    // Always restore idle radio state, including early returns and HTTP errors.
    struct IdleRadioCleanup {
      bool wasSleeping;
      ~IdleRadioCleanup() {
        if (wasSleeping) {
          wifiRadioOff();
        }
      }
    } radioCleanup{isDisplayOff};

    // Lighter power save while exchanging data keeps request latency low.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    bool fetchSuccess = fetchGanamosConfig();
    syncAttempted = true;
    lastSyncSucceeded = fetchSuccess;
    esp_task_wdt_reset();

    unsigned long fetchTime = millis() - fetchStart;
    if (fetchTime > 100) {
      Serial.print(F("SLOW FETCH: "));
      Serial.print(fetchTime);
      Serial.println(F("ms"));
    }

    if (!fetchSuccess) {
      // Only consecutive 404s count toward a reset; any success clears the counter.
      if (getLastHttpCode() == 404) {
        consecutiveConfig404s++;
        Serial.println("⚠️ Device config 404 (" + String(consecutiveConfig404s) + "/" + String(CONFIG_404_THRESHOLD) + ")");
        if (consecutiveConfig404s >= CONFIG_404_THRESHOLD) {
          Serial.println("⚠️ Device not found after repeated attempts - clearing saved config");
          clearDeviceConfig();
          isPaired = false;
          hadSavedConfigOnBoot = false;
          ganamosConfig.deviceId = "";  // Pair with the new code only
          generatePairingCode();
          // Turn display back on (full brightness) to show the pairing code,
          // and keep the radio on so pairing can complete.
          if (isDisplayOff) {
            VextON();
            delay(100);
            display.init();
          }
          isDisplayOff = isScreensaverActive = isBitcoinFactsActive = false;
          radioCleanup.wasSleeping = false;
          setOLEDContrast(NORMAL_BRIGHTNESS);
          lastButtonPress = millis();
          displayPairingCode();
          Serial.println("New pairing code generated: " + maskForLog(pairingCode));
          consecutiveConfig404s = 0;
          return;
        }
        Serial.println("⚠️ Waiting for additional 404s before resetting.");
      }
      if (wifiReady()) WiFi.setSleep(WIFI_PS_MAX_MODEM);
      return;  // Pet keeps showing cached data
    }

    consecutiveConfig404s = 0;
    static int lastBalance = ganamosConfig.balance;

    // Sync economy: update local balance from server and sync pending spends
    int serverCoins = ganamosConfig.coins;
    int localCoins = getLocalCoins();

    // Sync pending spends FIRST before reconciling balance
    int synced = syncPendingSpends();
    clearSyncedSpends();  // Persists the synced marks (single NVS write)
    if (synced > 0) {
      Serial.println("✅ Synced " + String(synced) + " pending spends");
    }
    esp_task_wdt_reset();
    soundUpdate();

    // Upload game scores that were queued while offline
    syncPendingGameScores();
    clearSyncedGameScores();
    esp_task_wdt_reset();
    soundUpdate();

    // Check how many pending spends are still unsynced
    int remainingPending = getPendingSpendCount();

    // After sync attempt, reconcile local balance with server
    // BUT only trust server if we have NO pending spends - otherwise our offline
    // spending hasn't been acknowledged by the server yet!
    // serverCoins was read before this round's spends were synced, so it is
    // stale whenever we just synced something; reconcile on the next poll.
    if (synced > 0) {
      Serial.println("⏳ Spends synced this round - reconciling balance next poll");
    } else if (serverCoins != localCoins) {
      int diff = localCoins - serverCoins;

      if (remainingPending > 0) {
        // DON'T trust server - we have unsynced spends that server doesn't know about
        Serial.println("⏳ Balance mismatch but " + String(remainingPending) + " pending spends not synced yet");
        Serial.println("   Local: " + String(localCoins) + ", Server: " + String(serverCoins));
        Serial.println("   Keeping local balance until spends sync successfully");
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
    applyTimeBasedDecay();
    lastDecayCheck = millis();

    cachedBatteryPct = getBatteryPercentage(); // Update cached battery value

    // Preserve the idle state before the celebration renderer wakes us.
    if (ganamosConfig.balance > lastBalance && isScreensaverActive) {
      wakeForNotification();
    }
    lastBalance = ganamosConfig.balance;

    // Check for fix rejection notification. The server keeps sending the
    // last rejection, so remember which one was shown in flash: otherwise
    // every reboot or re-flash would show an old rejection again.
    static String lastSeenRejectionId;
    static bool rejectionSeenLoaded = false;
    if (!rejectionSeenLoaded) {
      Preferences uiPrefs;
      if (uiPrefs.begin("sp-ui", true)) {
        lastSeenRejectionId = uiPrefs.getString("seenReject", "");
        uiPrefs.end();
      }
      rejectionSeenLoaded = true;
      // First run with this firmware: whatever rejection the server reports
      // now is from before; record it without showing it.
      if (lastSeenRejectionId.length() == 0 && ganamosConfig.lastRejectionId.length() > 0) {
        lastSeenRejectionId = ganamosConfig.lastRejectionId;
        Preferences w;
        if (w.begin("sp-ui", false)) { w.putString("seenReject", lastSeenRejectionId); w.end(); }
      }
    }
    if (ganamosConfig.lastRejectionId.length() > 0 &&
        ganamosConfig.lastRejectionId != lastSeenRejectionId) {
      // New rejection detected!
      lastSeenRejectionId = ganamosConfig.lastRejectionId;
      Preferences w;
      if (w.begin("sp-ui", false)) { w.putString("seenReject", lastSeenRejectionId); w.end(); }
      triggerRejection(ganamosConfig.rejectionMessage);
    }

    if (isDisplayOff) {
      Serial.println("💤💤 Display OFF: Balance=" + String(ganamosConfig.balance) + " Battery=" + String(cachedBatteryPct) + "%");
    }
    if (wifiReady()) WiFi.setSleep(WIFI_PS_MAX_MODEM);
    invalidateRender();  // Show fresh data on the next render pass
  }

  void loop() {
    esp_task_wdt_reset();  // Feed watchdog at start of every loop iteration
    soundUpdate();
    wifiService();
    // Ensure WiFi stays off if we're in offline mode (prevents background blocking)
    static bool wifiDisabled = false;
    if (!wifiDisabled && !wifiConnecting && WiFi.status() != WL_CONNECTED) {
      if (WiFi.getMode() != WIFI_OFF) {
        Serial.println(F("Forcing WiFi OFF to prevent blocking"));
        wifiRadioOff();
      }
      wifiDisabled = true;
    }
    
    static unsigned long lastLoopDebug = 0;
    static unsigned long lastLoopTime = 0;
    unsigned long now = millis();
    if (notificationActive && now - notificationStarted >= NOTIFICATION_DURATION) {
      notificationActive = false;
      petWarning = 0;
      isLowBatteryWarningActive = false;
      showCelebration = false;
      showNewJobNotification = false;
      showRejection = false;
      digitalWrite(RGB_LED, LOW);
      if (notificationReturnToSleep) {
        isDisplayOff = isScreensaverActive = true;
        isBitcoinFactsActive = false;
        VextOFF();
        wifiRadioOff();
      }
      notificationReturnToSleep = false;
    }
    
    // Warn if loop is running slowly (taking more than 500ms between iterations)
    if (!isDisplayOff && lastLoopTime > 0 && (now - lastLoopTime) > 500) {
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
    // Initialize battery on first loop, then sample at the target-specific cadence.
    if (cachedBatteryPct < 0 || (now - lastBatteryCheck >= BATTERY_CHECK_INTERVAL)) {
      cachedBatteryPct = getBatteryPercentage();
      lastBatteryCheck = now;
      
      // Trigger low battery warning at 10% (only once per charge cycle)
      if (cachedBatteryPct <= 10 && batteryPresent() && !lowBatteryAlertPlayed && !isLowBatteryWarningActive) {
        Serial.println("🔋 LOW BATTERY WARNING - waking display to show warning");
        
        wakeForNotification();
        
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
    
    // Enter ultra-low-power mode only at 5% (critical battery)
    // BUT only if user hasn't interacted recently - let normal screensaver flow handle sleep
    // This ensures button press always wakes device for full timeout period
    // Not when no battery reads at all: that's USB power with no battery, and
    // shutting the radio off there only breaks syncing and multiplayer.
    if (cachedBatteryPct <= 5 && batteryPresent() && !isDisplayOff && !isLowBatteryWarningActive) {
      // Only force low-power if we've already gone through the screensaver timeout
      // (i.e., user hasn't pressed a button recently)
      if (!notificationActive && !inMenuMode && now - lastButtonPress > SCREENSAVER_DISPLAY_OFF_TIMEOUT) {
        Serial.println("🔋 CRITICAL BATTERY (5%) - entering ultra-low-power mode");
        isScreensaverActive = true;
        isDisplayOff = true;
        VextOFF();
        syncPending = false;
        wifiRadioOff();
        // Will only wake for button press or the idle poll
      }
    }
    
    // Reset low battery alert flag when battery charged above 20%
    if (cachedBatteryPct > 20) {
      lowBatteryAlertPlayed = false;
    }
    
    // Multi-stage sleep transition: screensaver → facts → display OFF.
    // An unpaired device only dims, so its pairing code stays visible and
    // pairing polls keep running.
    if (!notificationActive && !inMenuMode && !buttonPressed && !isScreensaverActive && (now - lastButtonPress > SCREENSAVER_TIMEOUT)) {
      isScreensaverActive = true;
      isBitcoinFactsActive = false;
      isDisplayOff = false;
      setOLEDContrast(DIM_BRIGHTNESS);
      // Strongest modem sleep while idle (radio stays associated)
      if (wifiReady()) {
        WiFi.setSleep(WIFI_PS_MAX_MODEM);
      }
  #ifdef DEBUG_LOGGING
      Serial.println(F("Entering screensaver"));
  #endif
    }
    
    // Briefly show facts before display-off.
    if (isPaired && isScreensaverActive && !isBitcoinFactsActive && !isDisplayOff && (now - lastButtonPress > BITCOIN_FACTS_TIMEOUT)) {
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
    
    // End the idle display period.
    if (isPaired && isBitcoinFactsActive && !isDisplayOff && (now - lastButtonPress > SCREENSAVER_DISPLAY_OFF_TIMEOUT)) {
      isDisplayOff = true;
      VextOFF();  // Turn display OFF completely to save power
      
      // Disconnect WiFi to save significant power. A sync that is still
      // connecting turns the radio off itself when it finishes or times out.
      if (!syncPending) {
        wifiRadioOff();
      }
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
        
        if (!wifiReady()) {
          // Connect in the background; the next 5 s tick fetches once we have an IP.
          wifiRequestConnect(now);
        } else if (fetchGanamosConfig()) {
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
          lastButtonPress = millis();
          invalidateRender();
          renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
          return;
        } else {
          // Check if we got a 404 - only regenerate if we had saved config that's now invalid
          // If we're starting fresh (no saved config), 404 is expected - don't regenerate code
          static bool hasHandledSavedConfig404 = false;
          bool got404 = (getLastHttpCode() == 404);

          if (got404) {
            consecutiveConfig404s++;
            Serial.println("⚠️ Pairing fetch returned 404 (" + String(consecutiveConfig404s) + "/" + String(CONFIG_404_THRESHOLD) + ")");
          }
          
          if (got404 && hadSavedConfigOnBoot && !hasHandledSavedConfig404 && consecutiveConfig404s >= CONFIG_404_THRESHOLD) {
            // Had saved config but it's invalid - clear and regenerate ONCE
            Serial.println("⚠️ Saved pairing info invalid (404) - clearing saved config");
            clearDeviceConfig();
            isPaired = false;
            generatePairingCode(); // Generate new pairing code
            Serial.println("New pairing code generated: " + maskForLog(pairingCode));
            hasHandledSavedConfig404 = true;
            consecutiveConfig404s = 0;
          } else if (!got404) {
            Serial.println("Connection failed (not 404) - keeping pairing code");
          } else {
            Serial.println("Not paired yet - waiting for user to pair (pairing code static)");
          }
        }
        // Keep the (static) pairing code on screen
        displayPairingCode();
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
      if (!isDisplayOff && millis() - lastStateLog > 10000) {
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
      
      // Normal operation - poll less often while idle to save power
      unsigned long updateInterval = currentPollIntervalMs();
#ifdef WOKWI_SIMULATOR
      // Keep simulator tests deterministic and completely separate from the live API.
      updateInterval = 0xFFFFFFFFUL;
#endif
      
      if (!syncPending && now - lastUpdate > updateInterval) {
        lastUpdate = now;
        if (isScreensaverActive && isQuietHours()) {
          // Nobody is looking and sounds are muted: skip idle polls overnight.
          Serial.println(F("💤 Quiet hours - skipping idle poll"));
        } else if (wifiReady() || wifiRequestConnect(now)) {
          // Runs below as soon as we have an IP; never blocks waiting for WiFi.
          syncPending = true;
        }
      }

      if (syncPending) {
        if (wifiReady()) {
          syncPending = false;
          runConfigSync();
          if (!isPaired) return;  // Device was reset to pairing mode
        } else if (!wifiConnecting) {
          // Connect attempt gave up (radio already turned off by wifiService)
          syncPending = false;
          syncAttempted = true;
          lastSyncSucceeded = false;
          Serial.println(F("❌ WiFi reconnect failed - sync skipped"));
        }
      }
      
      // Apply decay even when offline (every minute)
      if (now - lastDecayCheck >= DECAY_CHECK_INTERVAL_MS) {
        applyTimeBasedDecay();
        lastDecayCheck = now;
      }
      
      static int lastPetWarning = 0;
      int warning = petStats.fullness == 0 && petStats.happiness == 0 ? 3
        : petStats.fullness < 20 ? 1 : petStats.happiness < 20 ? 2 : 0;
      if (warning != lastPetWarning) {
        lastPetWarning = warning;
        if (warning && isScreensaverActive && !notificationActive) {
          wakeForNotification();
          petWarning = warning;
          if (warning == 3) playDeathSound();
          else playSadSound();
        }
      }

      // Check if new job notification should end
      if (showNewJobNotification && (millis() - newJobNotificationStart >= NEW_JOB_NOTIFICATION_DURATION)) {
        showNewJobNotification = false;
        digitalWrite(RGB_LED, LOW);
      }

      // Check if rejection notification should end (5 seconds)
      if (showRejection && (millis() - rejectionStart >= 5000)) {
        showRejection = false;
        digitalWrite(RGB_LED, LOW);
      }
      
      // Menu mode is handled separately in button handling section.
      // Each screen redraws only when its content key changes, on a slow
      // self-heal refresh (static screens) or at ~8 fps (animated screens).
      if (!inMenuMode) {
        unsigned long renderStart = millis();
        if (isDisplayOff) {
          // Display is completely OFF - no rendering needed (maximum power saving)
        } else if (isLowBatteryWarningActive) {
          if (renderDue(renderKey(SCREEN_LOW_BATTERY, cachedBatteryPct), now, STATIC_SCREEN_REFRESH_MS)) {
            renderLowBatteryWarning(display, cachedBatteryPct);
          }
        } else if (petWarning) {
          int value = petWarning == 1 ? petStats.fullness : petStats.happiness;
          if (renderDue(renderKey(SCREEN_PET_WARNING, ((uint32_t)petWarning << 8) | (value & 0xFF)), now, STATIC_SCREEN_REFRESH_MS)) {
            if (petWarning == 3) renderDeathWarning(display, ganamosConfig.petName);
            else if (petWarning == 1) renderHungerWarning(display, ganamosConfig.petName, petStats.fullness);
            else renderSadnessWarning(display, ganamosConfig.petName, petStats.happiness);
          }
        } else if (showNewJobNotification) {
          // New job notification takes priority (animated)
          if (renderDue(renderKey(SCREEN_NEW_JOB, 0), now, ANIMATED_FRAME_MS)) {
            renderNewJobNotification(display);
          }
        } else if (showRejection) {
          if (renderDue(renderKey(SCREEN_REJECTION, 0), now, ANIMATED_FRAME_MS)) {
            renderRejection(display);
          }
        } else if (isBitcoinFactsActive) {
          unsigned long timeInFacts = now - bitcoinFactsStartTime;
          int factSlot = (timeInFacts / 20000) % 3;
          int fact = bitcoinFactsForSession[factSlot];
          if (renderDue(renderKey(SCREEN_FACT, ((uint32_t)factSlot << 16) | (fact & 0xFFFF)), now, STATIC_SCREEN_REFRESH_MS)) {
            renderBitcoinFact(display, fact);
          }
        } else if (isScreensaverActive) {
          if (renderDue(renderKey(SCREEN_SCREENSAVER, 0), now, 500)) {
            renderScreensaver(display, ganamosConfig.balance);
          }
        } else if (showCelebration) {
          if (renderDue(renderKey(SCREEN_CELEBRATION, 0), now, ANIMATED_FRAME_MS)) {
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, cachedBatteryPct);
          }
        } else if (onboardingStep > 0) {
          // Onboarding active - button handler advances it; just keep it on screen
          if (renderDue(renderKey(SCREEN_ONBOARDING, onboardingStep), now, STATIC_SCREEN_REFRESH_MS)) {
            renderOnboardingStep(display, onboardingStep, ganamosConfig.petName);
          }
        } else {
          // Normal mode
          static unsigned long lastDisplayUpdate = 0;
          static int lastDisplayedCoins = -1;
          static int lastDisplayedFullness = -1;
          static int lastDisplayedHappiness = -1;
          static int lastDisplayedBattery = -1;
          
          int currentCoins = getLocalCoins();
          bool dataChanged = (currentCoins != lastDisplayedCoins ||
                            petStats.fullness != lastDisplayedFullness ||
                            petStats.happiness != lastDisplayedHappiness ||
                            cachedBatteryPct != lastDisplayedBattery);
          const uint32_t petKey = renderKey(SCREEN_PET, 0);
          bool screenChanged = lastRenderKey != petKey;
          
          // One frame per second keeps the pet feeling alive while halving idle
          // OLED transfers compared with the previous 500 ms refresh cadence.
          // While syncing, refresh faster so the "..." indicator animates.
          // While the pet plays a gesture or eats, redraw every 200 ms so no
          // frame is skipped; otherwise once a second (the breath bob).
          unsigned long frameMs = petAnimating() ? 200UL : ((syncPending || wifiConnecting) ? 400UL : 1000UL);
          bool animationUpdate = (now - lastDisplayUpdate >= frameMs);
          
          if (dataChanged || animationUpdate || screenChanged) {
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, cachedBatteryPct);
            lastRenderKey = petKey;
            lastRenderAt = now;
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
    
    // Handle button presses (debounced levels; long-press timing unchanged)
    bool prgButtonState, externalButtonState;
    readButtons(prgButtonState, externalButtonState);
    bool currentButtonState = prgButtonState || externalButtonState;

    if (consumeButtonUntilRelease) {
      if (!currentButtonState) consumeButtonUntilRelease = false;
      delay(10);
      return;
    }
    if (currentButtonState && !buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
      lastButtonPress = millis();
      notificationActive = notificationReturnToSleep = false;
      petWarning = 0;
      invalidateRender();
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
        consumeButtonUntilRelease = true;
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
        consumeButtonUntilRelease = true;
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
          
          // Start WiFi reconnection in background (non-blocking). A due poll
          // waits for this connect instead of issuing its own WiFi.begin().
          if (!wifiReady() && !wifiConnecting) {
            Serial.println(F("Starting WiFi reconnection..."));
            lastWifiReconnectAttempt = millis();
            wifiStartConnect();
          }
        }
        // Refresh right away on wake (the screen shows "..." meanwhile) so
        // new sats and jobs appear without waiting for the next poll.
        if (isPaired && !syncPending && (wifiReady() || wifiConnecting)) {
          lastUpdate = millis();
          syncPending = true;
          syncAttempted = false;
        }
        // Button handling is local, so modem sleep can remain enabled after wake.
        if (wifiReady()) WiFi.setSleep(WIFI_PS_MAX_MODEM);
        isDisplayOff = false;
        setOLEDContrast(NORMAL_BRIGHTNESS);
        if (isPaired) {
          int batteryPct = getBatteryPercentage();
          renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
        } else {
          displayPairingCode();
        }
        Serial.println(F("Wake complete"));
        
        buttonPressed = false;
        consumeButtonUntilRelease = true;
        lastButtonSource = BUTTON_SOURCE_NONE;
        return;
      }
    }
    
    // In the menu a hold counts as soon as the line fills, without waiting
    // for release; the rest of that press is ignored. Elsewhere (home screen)
    // presses are still measured on release, so the 10 s factory-reset hold
    // and the WiFi-setup hold stay reachable.
    bool menuHoldReached = buttonPressed && currentButtonState && inMenuMode &&
                           onboardingStep == 0 &&
                           millis() - buttonPressTime >= HOLD_PRESS_MIN;
    if ((!currentButtonState && buttonPressed) || menuHoldReached) {
      unsigned long pressDuration = menuHoldReached ? HOLD_PRESS_MIN : millis() - buttonPressTime;
      buttonPressed = false;
      if (menuHoldReached) {
        consumeButtonUntilRelease = true;
        holdPressSwallow();     // nested screens ignore this press too
        holdHintsNoteHold();
      }
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
          if (selectedOption == 3) playMenuSelectToneBlocking();  // Jobs screen blocks
          else playMenuSelectTone();
          inMenuMode = false;
          currentMenuOption = 0;

          if (selectedOption == 0) {
            // Home - refresh main screen
            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          } else if (selectedOption == 1) {
            static const char* const GAME_OPTIONS[] = {"Flappy Pet", "Pickleball", "Sat Catcher", "Back"};
            static const char* const MODE_OPTIONS[] = {"Solo", "Multiplayer", "Back"};
            static const uint8_t GAME_IDS[] = {GAME_FLAPPY, GAME_PICKLEBALL, GAME_SATCATCHER};
            int game = runListMenu("Choose game", GAME_OPTIONS, 4);
            int mode = -1;
            if (game >= 0) {
              // Same choice for every game: play alone, or a room with friends
              mode = runListMenu(GAME_OPTIONS[game], MODE_OPTIONS, 3);
            }
            if (mode >= 0) {
              playMenuSelectToneBlocking();  // Games run their own blocking loops
            }
            if (mode == 0) {
              // Solo: each applies its own happiness and leaderboard
              if (game == 0) handleLightningGame(display);
              else if (game == 1) handlePickleballPractice(display);
              else handleSatCatcherGame(display);
            } else if (mode == 1) {
#ifdef WOKWI_SIMULATOR
              display.clear();
              display.setTextAlignment(TEXT_ALIGN_CENTER);
              display.drawString(64, 22, "Friends games need");
              display.drawString(64, 38, "physical devices");
              display.display();
              delay(1800);
#else
              applyGameHappinessBonus(runFriendsGame(display, GAME_IDS[game]));
#endif
            }
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
              esp_task_wdt_reset();
              unsigned long now = millis();

              if (now - lastFoodCycle > 30000) { // 30 seconds timeout (was 10)
                inFoodMenu = false;
                break;
              }

              soundUpdate();
              PressEvent foodEv = holdPressUpdate(anyButtonDown());
              if (holdPressActive()) lastButtonPress = lastFoodCycle = millis();

              if (foodEv != PRESS_NONE) {
                bool foodShort = foodEv == PRESS_TAP;
                bool foodHold = foodEv == PRESS_HOLD;

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
                    playMenuSelectToneBlocking();  // Result screen below blocks
                    
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

              holdPressDrawBar(display);
              delay(45);
            }

            int batteryPct = getBatteryPercentage();
            renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
          } else if (selectedOption == 3) {
            // Jobs - browse open jobs from user's groups. If the radio is
            // still (re)connecting after wake, wait for it here (user-initiated)
            // instead of failing the fetch.
            if (!wifiReady()) {
              display.clear();
              display.setFont(ArialMT_Plain_10);
              display.setTextAlignment(TEXT_ALIGN_CENTER);
              display.drawString(64, 25, "Connecting WiFi...");
              display.display();
              wifiEnsureConnected(WIFI_CONNECT_TIMEOUT_MS + WIFI_FAST_CONNECT_TIMEOUT_MS);
            }
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
              playMenuSelectToneBlocking();
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
    if (inMenuMode && !buttonPressed && (millis() - lastMenuCycle > 10000)) {
      inMenuMode = false;
      int batteryPct = getBatteryPercentage();
      renderPet(display, ganamosConfig.btcPrice, ganamosConfig.balance, batteryPct);
    }
    
    // If in menu mode, keep rendering menu
    if (inMenuMode) {
      static unsigned long lastMenuRender = 0;
      // Not while the button is down: a redraw would wipe the hold line
      if (!buttonPressed && millis() - lastMenuRender > 500) {
        renderMenu(display, currentMenuOption);
        lastMenuRender = millis();
      }
      if (buttonPressed && !consumeButtonUntilRelease) {
        drawHoldBar(display, millis() - buttonPressTime);
      }
    }
    
    // When both OLED and radio are off, light-sleep the CPU until the next
    // scheduled job (poll / decay tick / battery sample). Buttons wake via
    // GPIO. RAM is retained. Wokwi cannot validate current draw.
#ifndef WOKWI_SIMULATOR
    if (isDisplayOff && WiFi.getMode() == WIFI_OFF && !soundBusy()) {
      unsigned long sleepMs = msUntilNextScheduledJob(millis());
      esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
      // An intentional sleep can exceed the 15 s task-watchdog timeout:
      // unsubscribe for its duration and re-subscribe on wake.
      esp_task_wdt_reset();
      esp_task_wdt_delete(NULL);
      esp_light_sleep_start();
      esp_task_wdt_add(NULL);
      esp_task_wdt_reset();
    } else {
      delay(10);
    }
#else
    delay(10);
#endif
  }

  // User-initiated feedback tones play regardless of quiet hours (as before).
  void playButtonChirp() {
    soundPlay(BUTTON_CHIRP, NOTE_COUNT(BUTTON_CHIRP), false);
  }

  void playMenuSelectTone() {
    soundPlay(MENU_SELECT_TONE, NOTE_COUNT(MENU_SELECT_TONE), false);
  }

  // Blocking variant for selections that hand control to a blocking screen
  // (games, jobs, portal) which does not service soundUpdate().
  void playMenuSelectToneBlocking() {
    soundPlayBlocking(MENU_SELECT_TONE, NOTE_COUNT(MENU_SELECT_TONE), false);
  }

  // Check if current time is during quiet hours (8pm-8am local time).
  // Cheap and non-blocking: no SNTP wait. Returns false until time is valid.
  bool isQuietHours() {
    time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1600000000) return false;  // Clock not yet synced
    struct tm timeinfo;
    localtime_r(&nowEpoch, &timeinfo);
    return (timeinfo.tm_hour >= 20 || timeinfo.tm_hour < 8);
  }

  void playSadSound() {
    soundPlay(SAD_MELODY, NOTE_COUNT(SAD_MELODY));
  }

  void playDeathSound() {
    soundPlay(DEATH_MELODY, NOTE_COUNT(DEATH_MELODY));
  }

  void playLowBatterySound() {
    soundPlay(LOW_BATTERY_MELODY, NOTE_COUNT(LOW_BATTERY_MELODY));
  }

  void playNewJobChirp() {
    // Distinct attention-getting chirp - rising G5, B5, D6
    soundPlay(NEW_JOB_CHIRP, NOTE_COUNT(NEW_JOB_CHIRP));
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
      esp_task_wdt_reset();
      soundUpdate();
      bool prgState, externalState;
      readButtons(prgState, externalState);

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
            playMenuSelectToneBlocking();  // Device reboots right after
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
