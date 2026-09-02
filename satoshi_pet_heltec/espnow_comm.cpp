#include "espnow_comm.h"

// ============================================================================
// ESP-NOW Communication Implementation
// ============================================================================

static ESPNowReceiveCallback _userReceiveCallback = nullptr;
static bool _espnowInitialized = false;

// Internal receive callback (ESP-NOW API signature)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void _onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (_userReceiveCallback && info) {
    _userReceiveCallback(info->src_addr, data, len);
  }
}
#else
static void _onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (_userReceiveCallback) {
    _userReceiveCallback(mac, data, len);
  }
}
#endif

// Internal send callback (optional, for debugging)
static void _onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println(F("⚠️ ESP-NOW send failed"));
  }
}

bool espnowInit() {
  if (_espnowInitialized) return true;

  // ESP-NOW requires WiFi to be initialized (STA or AP mode)
  // If WiFi is OFF, temporarily set to STA
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
  }

  // Set WiFi channel (ESP-NOW works on current WiFi channel)
  // If connected to AP, it uses that channel. If not, default channel 1.

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return false;
  }

  esp_now_register_recv_cb(_onDataRecv);
  esp_now_register_send_cb(_onDataSent);

  _espnowInitialized = true;
  Serial.println(F("✅ ESP-NOW initialized"));
  Serial.println("📡 MAC: " + espnowGetMacString());

  return true;
}

void espnowDeinit() {
  if (!_espnowInitialized) return;

  espnowRemoveAllPeers();
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();

  _espnowInitialized = false;
  Serial.println(F("ESP-NOW deinitialized"));
}

bool espnowAddPeer(const uint8_t mac[6]) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;  // Use current channel
  peerInfo.encrypt = false;

  // Check if already added
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.printf("❌ Failed to add ESP-NOW peer: %d\n", result);
    return false;
  }

  Serial.printf("✅ Added ESP-NOW peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

bool espnowAddPeerFromString(const String &macStr) {
  uint8_t mac[6];
  if (!espnowParseMac(macStr, mac)) {
    Serial.println("❌ Invalid MAC string: " + macStr);
    return false;
  }
  return espnowAddPeer(mac);
}

void espnowRemoveAllPeers() {
  esp_now_peer_info_t peer;
  // Iterate and remove all peers
  while (esp_now_fetch_peer(true, &peer) == ESP_OK) {
    esp_now_del_peer(peer.peer_addr);
  }
}

bool espnowSend(const uint8_t *peerMac, const uint8_t *data, size_t len) {
  if (!_espnowInitialized) return false;

  esp_err_t result = esp_now_send(peerMac, data, len);
  return result == ESP_OK;
}

bool espnowBroadcast(const uint8_t *data, size_t len) {
  if (!_espnowInitialized) return false;

  // Send to each registered peer individually for reliability
  esp_now_peer_info_t peer;
  bool allOk = true;
  bool isFirst = true;

  while (true) {
    esp_err_t err = esp_now_fetch_peer(isFirst, &peer);
    isFirst = false;
    if (err != ESP_OK) break;

    if (esp_now_send(peer.peer_addr, data, len) != ESP_OK) {
      allOk = false;
    }
  }

  return allOk;
}

void espnowSetReceiveCallback(ESPNowReceiveCallback cb) {
  _userReceiveCallback = cb;
}

String espnowGetMacString() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool espnowParseMac(const String &macStr, uint8_t mac[6]) {
  // Parse "AA:BB:CC:DD:EE:FF" format
  if (macStr.length() != 17) return false;

  int values[6];
  int parsed = sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
    &values[0], &values[1], &values[2],
    &values[3], &values[4], &values[5]);

  if (parsed != 6) return false;

  for (int i = 0; i < 6; i++) {
    mac[i] = (uint8_t)values[i];
  }
  return true;
}
