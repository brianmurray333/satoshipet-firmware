#include "espnow_comm.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============================================================================
// ESP-NOW Communication Implementation
// ============================================================================

static bool _espnowInitialized = false;
static QueueHandle_t _rxQueue = nullptr;
static volatile uint32_t _rxDropped = 0;
static volatile uint32_t _txFailed = 0;

// Expected wire size for each message type; 0 = unknown type.
static size_t _expectedLen(uint8_t type) {
  switch (type) {
    case MSG_GAME_STATE:     return sizeof(GameStateMessage);
    case MSG_BUTTON_PRESS:   return sizeof(ButtonPressMessage);
    case MSG_GAME_START:     return sizeof(GameStartMessage);
    case MSG_POINT_SCORED:   return sizeof(PointScoredMessage);
    case MSG_GAME_OVER:      return sizeof(GameOverMessage);
    case MSG_PING:           return sizeof(PingMessage);
    case MSG_PONG:           return sizeof(PingMessage);
    case MSG_LOBBY_READY:    return sizeof(LobbyReadyMessage);
    case MSG_GAME_START_ACK: return sizeof(GameStartAckMessage);
    default:                 return 0;
  }
}

// Runs in the WiFi task. No Serial, no game state: validate and enqueue only.
static void _enqueuePacket(const uint8_t *mac, const uint8_t *data, int len) {
  if (!mac || !data || len < 1 || len > ESPNOW_RX_MAX_LEN || !_rxQueue) {
    _rxDropped = _rxDropped + 1;
    return;
  }
  size_t expected = _expectedLen(data[0]);
  if (expected == 0 || (size_t)len < expected) {
    _rxDropped = _rxDropped + 1;   // unknown type / other protocol version / short
    return;
  }
  ESPNowRxPacket pkt;
  memcpy(pkt.mac, mac, 6);
  pkt.len = (uint8_t)len;
  memcpy(pkt.data, data, len);
  if (xQueueSend(_rxQueue, &pkt, 0) != pdTRUE) {
    _rxDropped = _rxDropped + 1;
  }
}

// Internal receive callback (ESP-NOW API signature)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void _onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info) return;
  _enqueuePacket(info->src_addr, data, len);
}
#else
static void _onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  _enqueuePacket(mac, data, len);
}
#endif

// Internal send callback. ESP-IDF 5.5 (arduino-esp32 3.3+) changed the first
// argument from the peer MAC to a wifi_tx_info_t pointer.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
static void _onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
static void _onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    _txFailed = _txFailed + 1;   // no Serial in WiFi-task callbacks
  }
}

bool espnowInit() {
  if (_espnowInitialized) return true;

  if (!_rxQueue) {
    _rxQueue = xQueueCreate(ESPNOW_RX_QUEUE_LEN, sizeof(ESPNowRxPacket));
    if (!_rxQueue) {
      Serial.println(F("ESP-NOW rx queue alloc failed"));
      return false;
    }
  }
  xQueueReset(_rxQueue);
  _rxDropped = 0;
  _txFailed = 0;

  // ESP-NOW requires WiFi to be initialized (STA or AP mode)
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
  }

  // ESP-NOW uses the current WiFi channel (the AP's channel when connected).
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW init failed"));
    return false;
  }

  esp_now_register_recv_cb(_onDataRecv);
  esp_now_register_send_cb(_onDataSent);

  _espnowInitialized = true;
  Serial.print(F("ESP-NOW initialized, MAC: "));
  Serial.println(espnowGetMacString());

  return true;
}

void espnowDeinit() {
  if (!_espnowInitialized) return;

  espnowRemoveAllPeers();
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();

  // The queue itself is kept (allocated once) so a late callback can never
  // touch freed memory; just discard anything still pending.
  if (_rxQueue) xQueueReset(_rxQueue);

  _espnowInitialized = false;
  Serial.printf("ESP-NOW deinitialized (rx dropped %u, tx failed %u)\n",
                (unsigned)_rxDropped, (unsigned)_txFailed);
}

bool espnowAddPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;  // Use current channel
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.printf("Failed to add ESP-NOW peer: %d\n", result);
    return false;
  }

  Serial.printf("Added ESP-NOW peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

void espnowRemoveAllPeers() {
  esp_now_peer_info_t peer;
  while (esp_now_fetch_peer(true, &peer) == ESP_OK) {
    esp_now_del_peer(peer.peer_addr);
  }
}

bool espnowSend(const uint8_t *peerMac, const uint8_t *data, size_t len) {
  if (!_espnowInitialized) return false;
  return esp_now_send(peerMac, data, len) == ESP_OK;
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

bool espnowReceive(ESPNowRxPacket &out) {
  if (!_rxQueue) return false;
  return xQueueReceive(_rxQueue, &out, 0) == pdTRUE;
}

uint32_t espnowRxDroppedCount() { return _rxDropped; }
uint32_t espnowTxFailedCount() { return _txFailed; }

String espnowGetMacString() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool espnowParseMac(const char *macStr, uint8_t mac[6]) {
  // Parse "AA:BB:CC:DD:EE:FF" format
  if (!macStr || strlen(macStr) != 17) return false;

  unsigned int values[6];
  int parsed = sscanf(macStr, "%x:%x:%x:%x:%x:%x",
    &values[0], &values[1], &values[2],
    &values[3], &values[4], &values[5]);

  if (parsed != 6) return false;

  for (int i = 0; i < 6; i++) {
    if (values[i] > 0xFF) return false;
    mac[i] = (uint8_t)values[i];
  }
  return true;
}

bool espnowParseMac(const String &macStr, uint8_t mac[6]) {
  return espnowParseMac(macStr.c_str(), mac);
}
