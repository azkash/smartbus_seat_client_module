#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
// #include <esp_now.h>          // replaced: ESP-NOW → WebSocket client
#include <WebSocketsClient.h>
#include <Preferences.h>        // NVS storage for the seat id
// #include "soc/rtc.h"          // replaced: low-level RTC API gone in core 3.x
#include "FastLED.h"
#include "seat_pressure_logic.h"

// ID Kursi (Seat ID) — identifies this seat in the JSON sent to the master.
// An unprovisioned node reports "RandomID" plus a MAC-derived UID. The first
// call opens the assignment popup on the head unit; the selected ID is saved
// to this node's NVS. Serial `setid=B15` remains available as a fallback.
char seatId[32] = {0};
char nodeUid[13] = {0};
bool seatIdAssigned = false;

static void saveSeatId(const char *id);
static void applyClearSeatIdCommand(JsonDocument& doc);

#define LOAD_SENSOR_PIN 34  // GPIO34 / ADC1_CH6; input-only, external FSR divider required
#define BUTTON_PIN 27       // button to GND; GPIO27 supports INPUT_PULLUP
#define SEATBELT_PIN 18     // seatbelt buckle switch to GND; INPUT_PULLUP
#define LED_PIN    25       // WS2812B data; common GND and a level shifter are recommended
#define NUM_LEDS   10

// Seatbelt buckle wiring: switch between GPIO18 and GND. With INPUT_PULLUP the
// pin reads LOW when the buckle is closed. Flip SEATBELT_FASTENED_LEVEL if the
// installed switch is normally-closed instead.
constexpr unsigned long SEATBELT_DEBOUNCE_MS = 200;
constexpr int SEATBELT_FASTENED_LEVEL = HIGH;

// ─── Uplink APs — try master first, fall back to the slave gateway ────
// One firmware serves every seat: at boot it scans, prefers the master, and
// only falls back to the gateway if the master isn't reachable. The matching
// WebSocket host IP is selected automatically for whichever AP we join.
const char *MASTER_SSID   = "Seat-Monitoring-Bus";  // master softAP
const char *MASTER_HOST   = "192.168.4.1";
// Every gateway broadcasts "Seat-GW-<n>". Its AP address is
// 192.168.<5+n>.1 (GW1 = 192.168.6.1, GW2 = 192.168.7.1, and so on).
#define     GW_SSID_PREFIX  "Seat-GW-"
const char *WIFI_PASSWORD = "12345678";
const int   WS_PORT       = 81;
const char *WS_PATH       = "/";

String wsHost;    // resolved at connect time to the chosen AP's host IP
// Uplink assignment, stored in NVS and set over serial with `setgw=`:
//   -1  (setgw=master) → join the master directly, gateways only as fallback
//    0  (setgw=0)      → auto: strongest gateway, master as last resort
//   1..99 (setgw=N)    → join Seat-GW-<N>, other gateways then master as fallback
#define MASTER_DIRECT (-1)
int    gwId = 1;  // new installs default to Gateway 1

CRGB leds[NUM_LEDS];

WebSocketsClient webSocket;
Preferences prefs;   // NVS storage for the seat id

// Router-centric transport. All seats associate with the real router; one UDP
// socket on the master receives the entire fleet without persistent TCP slots.
static const char* ROUTER_SSID = "iot_bus-015";
static const char* ROUTER_PASS = "iot_BUS@5758";
static constexpr uint16_t MASTER_UDP_PORT = 5758;
static constexpr uint16_t SEAT_UDP_PORT = 5759;
static constexpr unsigned long DISCOVERY_INTERVAL_MS = 2000;
static constexpr unsigned long MASTER_ACK_TIMEOUT_MS = 12000;
static constexpr unsigned long CALL_RETRY_FAST_MS = 400;
static constexpr unsigned long CALL_RETRY_SLOW_MS = 2000;
static WiFiUDP smartbusUdp;
static IPAddress masterIp;
static uint32_t udpSession = 0;
static uint32_t udpSequence = 0;
static unsigned long lastMasterAckMs = 0;
static unsigned long lastDiscoveryMs = 0;
static bool pendingCall = false;
static uint32_t pendingCallSeq = 0;
static bool pendingCallOccupied = false;
static uint16_t pendingCallRetries = 0;
static unsigned long pendingCallSentMs = 0;

const unsigned long SAMPLING_INTERVAL_MS = 200;
const unsigned long HEARTBEAT_MS         = 2000;  // lightweight UDP heartbeat for offline detection

volatile bool wsConnected      = false;  // true while the WS link to master is up
// millis() of the last frame received FROM the master. The master broadcasts
// the roster once a second, so prolonged silence means the link is dead even
// if the socket still claims to be connected (half-open TCP).
volatile unsigned long lastMasterMsgMs = 0;
#define MASTER_SILENCE_MS 30000   // ~30 missed broadcasts before we give up
volatile uint16_t pressureReading = 0;
volatile bool seatOccupied     = false;
volatile bool occupancyChanged = false;  // set by sensorTask when occupancy flips
volatile bool seatbeltFastened = false;  // true = buckle closed (fastened)
volatile bool seatbeltChanged  = false;  // set when the debounced buckle state flips
SemaphoreHandle_t dataMutex;

// Debounce state machine. A press must remain LOW for this long and, after it
// fires, the input must return HIGH stably before another call is accepted.
// This prevents both contact bounce and a noisy/held input from spamming calls.
constexpr unsigned long BUTTON_DEBOUNCE_MS = 100;
constexpr unsigned long BUTTON_LOCKOUT_MS = 10000;
bool buttonRawState = HIGH;
bool buttonStableState = HIGH;
bool buttonArmed = true;
unsigned long buttonRawChangedMs = 0;
unsigned long lastButtonCallMs = 0;
bool buttonLockoutActive = false;

// Seatbelt debounce mirrors the button: the pin must hold its new level for
// SEATBELT_DEBOUNCE_MS before the state is accepted and reported.
constexpr unsigned long SEATBELT_LOCKOUT_MS = 10000;
bool seatbeltRawState = HIGH;
bool seatbeltStableState = HIGH;
unsigned long seatbeltRawChangedMs = 0;
unsigned long lastSeatbeltCallMs = 0;
bool seatbeltLockoutActive = false;

// -- removed ESP-NOW pieces (peer MACs, struct_message, getCleanMacAddress(),
//    OnDataSent, esp_now_send/init). The master now consumes JSON over WebSocket:
//    {"id":"B15","sensorState":1,"seatOccupied":true}
// uint8_t peerAddressDashboard[] = {0xD0, 0xCF, 0x13, 0x01, 0x63, 0xF0};
// uint8_t peerAddressActuator[]  = {0x30, 0x76, 0xF5, 0xA6, 0x0C, 0x0C};
// typedef struct struct_message { char id[13]; int sensorState; bool seatOccupied; } struct_message;

void sensorTask(void *pvParameters) {
  bool lastOccupied = false;
  for (;;) {
    // Average several ADC samples to reduce vibration and electrical noise.
    uint32_t total = 0;
    constexpr uint8_t sampleCount = 8;
    for (uint8_t i = 0; i < sampleCount; i++) {
      total += analogRead(LOAD_SENSOR_PIN);
      delay(2);
    }
    uint16_t pressure = total / sampleCount;
    bool occupied = updateSeatOccupied(lastOccupied, pressure);

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    pressureReading = pressure;
    seatOccupied = occupied;
    if (occupied != lastOccupied) { occupancyChanged = true; lastOccupied = occupied; }
    xSemaphoreGive(dataMutex);

    // Report the LOGICAL button state, not the raw pin. With INPUT_PULLUP an
    // idle button floats HIGH, so printing the pin directly showed "Button: 1"
    // when nothing was pressed. This matches the sensorState we send to the
    // master: 0 = released (default), 1 = pressed.
    Serial.printf("ID: %s  Pressure ADC: %u  Occupied: %s  Button: %d  Seatbelt: %s\n",
                  seatId, (unsigned)pressure, occupied ? "YES" : "NO",
                  digitalRead(BUTTON_PIN) == LOW ? 1 : 0,
                  seatbeltFastened ? "FASTENED" : "OPEN");

    vTaskDelay(pdMS_TO_TICKS(SAMPLING_INTERVAL_MS));
  }
}

static bool sendUdpJson(const IPAddress& target, uint16_t port, const char* json) {
  if (target == IPAddress() || WiFi.status() != WL_CONNECTED) return false;
  if (!smartbusUdp.beginPacket(target, port)) return false;
  smartbusUdp.write(reinterpret_cast<const uint8_t*>(json), strlen(json));
  return smartbusUdp.endPacket() == 1;
}

static uint32_t nextUdpSequence() {
  ++udpSequence;
  if (udpSequence == 0) ++udpSequence;
  return udpSequence;
}

static bool sendSeatPacket(uint32_t seq, int sensorState, bool occupied,
                           bool seatbelt, uint16_t pressure = 0) {
  if (masterIp == IPAddress()) return false;
  if (pressure == 0 && occupied) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    pressure = pressureReading;
    xSemaphoreGive(dataMutex);
  }
  char buf[352];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"seat\",\"id\":\"%s\",\"uid\":\"%s\","
           "\"unassigned\":%s,\"session\":%lu,\"seq\":%lu,"
           "\"sensorState\":%d,\"seatOccupied\":%s,\"seatbelt\":%s,"
           "\"pressure\":%u,\"uptime\":%lu,\"rssi\":%d}",
           seatId, nodeUid, seatIdAssigned ? "false" : "true",
           (unsigned long)udpSession, (unsigned long)seq,
           sensorState, occupied ? "true" : "false",
           seatbelt ? "true" : "false",
           (unsigned)pressure,
           millis() / 1000UL, (int)WiFi.RSSI());
  const bool sent = sendUdpJson(masterIp, MASTER_UDP_PORT, buf);
  if (sensorState || !sent) {
    Serial.printf("[UDP] %s seq=%lu to %s%s\n",
                  sensorState ? "CALL" : "send",
                  (unsigned long)seq, masterIp.toString().c_str(),
                  sent ? "" : " FAILED");
  }
  return sent;
}

// A call is retained and retransmitted with the SAME sequence until ACK. The
// master deduplicates it, so a lost ACK cannot replay the audio announcement.
static void sendSeatEvent(int sensorState, bool occupied) {
  // Do not let a newer heartbeat sequence overtake an unacknowledged CALL.
  // Otherwise the master could correctly reject the later CALL retry as old.
  if (!sensorState && pendingCall) return;
  const uint32_t seq = nextUdpSequence();
  if (sensorState) {
    pendingCall = true;
    pendingCallSeq = seq;
    pendingCallOccupied = occupied;
    pendingCallRetries = 0;
    pendingCallSentMs = millis();
  }
  sendSeatPacket(seq, sensorState, occupied, seatbeltFastened);
}

// Health report → master. Lets a seat that drops out in the field be diagnosed
// from the master's log alone, with no USB cable and no physical access:
//   reset  — why it last booted (11 = BROWNOUT means the 5V supply sagged)
//   heap   — free bytes; a steady decline over hours indicates a leak
//   rssi   — link margin; below about -75 dBm drops become likely
//   uptime — seconds since boot, so silent reboot loops are obvious
static void sendDiag() {
  if (masterIp == IPAddress()) return;
  char buf[320];
  // "ap" is the AP actually joined — compare it against "gw" (the assignment)
  // to spot seats that fell back to another gateway or to the master.
  snprintf(buf, sizeof(buf),
           "{\"type\":\"diag\",\"id\":\"%s\",\"uid\":\"%s\","
           "\"unassigned\":%s,\"uptime\":%lu,\"heap\":%u,\"rssi\":%d,"
           "\"reset\":%d,\"ap\":\"%s\",\"pressure\":%u,\"seatbelt\":%s}",
           seatId, nodeUid, seatIdAssigned ? "false" : "true",
           millis() / 1000, (unsigned)ESP.getFreeHeap(),
           (int)WiFi.RSSI(), (int)esp_reset_reason(),
           WiFi.SSID().c_str(), (unsigned)pressureReading,
           seatbeltFastened ? "true" : "false");
  sendUdpJson(masterIp, MASTER_UDP_PORT, buf);
  Serial.printf("[Diag] %s\n", buf);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      lastMasterMsgMs = millis();   // start the silence clock fresh
      Serial.printf("[WS] connected to %s:%d\n", wsHost.c_str(), WS_PORT);
      break;
    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("[WS] disconnected from master");
      break;
    case WStype_TEXT: {
      lastMasterMsgMs = millis();   // proof the master is still alive
      String msg = String((char*)payload);
      Serial.printf("[WS] rx: %s\n", msg.c_str());
      if (msg.indexOf("\"status\":\"identify\"") >= 0) {
        // Master is asking us to identify — reply with our role + current state
        bool occupied;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        occupied = seatOccupied;
        xSemaphoreGive(dataMutex);
        sendSeatEvent(0, occupied);   // immediate heartbeat so master registers us
      } else if (msg.indexOf("\"action\":\"setSeatId\"") >= 0) {
        const String uidMarker = "\"uid\":\"";
        const String idMarker  = "\"id\":\"";
        int uidStart = msg.indexOf(uidMarker);
        int idStart  = msg.indexOf(idMarker);
        String targetUid;
        String requestedId;
        if (uidStart >= 0) {
          uidStart += uidMarker.length();
          int uidEnd = msg.indexOf('"', uidStart);
          if (uidEnd > uidStart) targetUid = msg.substring(uidStart, uidEnd);
        }
        if (idStart >= 0) {
          idStart += idMarker.length();
          int idEnd = msg.indexOf('"', idStart);
          if (idEnd > idStart) requestedId = msg.substring(idStart, idEnd);
        }
        requestedId.trim();

        bool validId = requestedId.length() > 0 && requestedId.length() < sizeof(seatId);
        for (size_t i = 0; validId && i < requestedId.length(); ++i) {
          const char c = requestedId[i];
          validId = isAlphaNumeric(c) || c == '-' || c == '_';
        }
        if (requestedId.equalsIgnoreCase("RandomID")) validId = false;

        // Gateways broadcast downstream commands, so only the node whose
        // hardware UID matches is allowed to consume this assignment.
        if (targetUid == nodeUid && !seatIdAssigned && validId) {
          saveSeatId(requestedId.c_str());
          requestedId.toCharArray(seatId, sizeof(seatId));
          seatIdAssigned = true;
          char ack[128];
          snprintf(ack, sizeof(ack),
                   "{\"event\":\"seatIdSaved\",\"uid\":\"%s\",\"id\":\"%s\"}",
                   nodeUid, seatId);
          webSocket.sendTXT(ack);
          Serial.printf("[Seat] ID saved from head unit: %s\n", seatId);

          bool occupied;
          xSemaphoreTake(dataMutex, portMAX_DELAY);
          occupied = seatOccupied;
          xSemaphoreGive(dataMutex);
          sendSeatEvent(0, occupied);  // register the friendly ID immediately
        }
      } else if (msg.indexOf("\"action\":\"clearSeatId\"") >= 0) {
        StaticJsonDocument<128> doc;
        if (!deserializeJson(doc, msg)) {
          applyClearSeatIdCommand(doc);
        }
      }
      break;
    }
    default:
      break;
  }
}

// ─── Seat ID persistence (NVS) ────────────────────────────────────────
static void saveSeatId(const char *id) {
  prefs.begin("seat", false);
  prefs.putString("id", id);
  prefs.end();
}

static void clearSeatId() {
  prefs.begin("seat", false);
  prefs.remove("id");
  prefs.end();
}

static void loadSeatId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(nodeUid, sizeof(nodeUid), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  prefs.begin("seat", true);
  String stored = prefs.getString("id", "");
  prefs.end();
  if (stored.length() > 0 && !stored.equalsIgnoreCase("RandomID")) {
    stored.toCharArray(seatId, sizeof(seatId));
    seatIdAssigned = true;
  } else {
    strlcpy(seatId, "RandomID", sizeof(seatId));
    seatIdAssigned = false;
  }
  Serial.printf("[Seat] ID = %s  UID = %s  assigned=%s\n",
                seatId, nodeUid, seatIdAssigned ? "yes" : "NO");
}

// ─── Assigned gateway persistence (NVS) ───────────────────────────────
// Which gateway this seat belongs to, so load is planned rather than left to
// whichever AP happens to be loudest. 0 = unassigned (picks the strongest).
static void saveGwId(int id) {
  prefs.begin("seat", false);
  prefs.putInt("gw", id);
  prefs.end();
}

static void loadGwId() {
  prefs.begin("seat", true);
  gwId = prefs.getInt("gw", 1);
  prefs.end();
  if (gwId == MASTER_DIRECT)
    Serial.println("[Seat] uplink = MASTER direct  (send 'setgw=N' for a gateway)");
  else if (gwId > 0)
    Serial.printf("[Seat] uplink = %s%d  (send 'setgw=master' or 'setgw=N' to change)\n",
                  GW_SSID_PREFIX, gwId);
  else
    Serial.println("[Seat] uplink = auto, strongest gateway  (send 'setgw=N' or 'setgw=master')");
}

// Declared early: the uplink routines below run from setup(), where loop()
// (and therefore the console) is not yet running. They call this during their
// waits so the seat can always be reconfigured over serial — otherwise a seat
// pinned to a gateway that does not exist would retry and reboot forever with
// no way to issue setgw to fix it.
static void handleSerial();

// A delay() that keeps the serial console alive.
static void delayServicingSerial(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    handleSerial();
    delay(10);
  }
}

// ─── Uplink selection ─────────────────────────────────────────────────
// Try to associate with one AP profile within a timeout. On success stores
// its host IP in wsHost and returns true.
struct ScannedAp {
  bool    found;
  int32_t rssi;
  int32_t channel;
  int     auth;
  uint8_t bssid[6];
};

static void captureAp(ScannedAp &ap, int scanIndex) {
  ap.found   = true;
  ap.rssi    = WiFi.RSSI(scanIndex);
  ap.channel = WiFi.channel(scanIndex);
  ap.auth    = (int)WiFi.encryptionType(scanIndex);
  memcpy(ap.bssid, WiFi.BSSID(scanIndex), sizeof(ap.bssid));
}

static String fallbackWsHost(const String &ssid) {
  if (ssid == MASTER_SSID) return MASTER_HOST;
  if (ssid.startsWith(GW_SSID_PREFIX)) {
    const int id = ssid.substring(strlen(GW_SSID_PREFIX)).toInt();
    if (id >= 1 && id <= 99) return String("192.168.") + (5 + id) + ".1";
  }
  return "";
}

static bool tryConnect(const String &ssid, const ScannedAp *ap = nullptr) {
  if (ap && ap->found) {
    Serial.printf("[WiFi] Connecting to %s @ %02X:%02X:%02X:%02X:%02X:%02X"
                  " ch=%ld RSSI=%ld auth=%d",
                  ssid.c_str(), ap->bssid[0], ap->bssid[1], ap->bssid[2],
                  ap->bssid[3], ap->bssid[4], ap->bssid[5],
                  (long)ap->channel, (long)ap->rssi, ap->auth);
    // Let the WiFi driver perform normal AP selection. The scan data above is
    // retained for diagnostics, but forcing a BSSID/channel can leave this
    // board retrying a stale association and expiring during WPA.
    WiFi.begin(ssid.c_str(), WIFI_PASSWORD);
  } else {
    Serial.printf("[WiFi] Connecting to %s (blind)", ssid.c_str());
    WiFi.begin(ssid.c_str(), WIFI_PASSWORD);
  }
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    handleSerial();      // stay reachable during the 8 s association wait
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    // Learn the server address from DHCP instead of assuming it. The AP we
    // joined IS our default gateway, and gateways now chain on their own
    // subnets (192.168.<5+n>.1), so a hard-coded address would be wrong for
    // every gateway but the first. The SSID-derived address is only a fallback
    // for the rare case where DHCP hands us no gateway.
    IPAddress gw = WiFi.gatewayIP();
    wsHost = (gw != IPAddress(0, 0, 0, 0)) ? gw.toString()
                                           : fallbackWsHost(ssid);
    if (wsHost.length() == 0) {
      Serial.printf("\n[WiFi] Joined %s but could not determine WS host\n",
                    ssid.c_str());
      WiFi.disconnect();
      return false;
    }
    Serial.printf("\n[WiFi] Joined %s — IP %s → WS host %s\n",
                  ssid.c_str(), WiFi.localIP().toString().c_str(), wsHost.c_str());
    return true;
  }
  Serial.printf("\n[WiFi] %s did not connect\n", ssid.c_str());
  WiFi.disconnect();
  return false;
}

// Scan, then prefer the master; fall back to the gateway; restart only after
// several full rounds have failed.
static void connectUplink() {
  const String assignedSsid = gwId > 0 ? String(GW_SSID_PREFIX) + gwId : String();

  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.printf("[WiFi] uplink attempt %d/3 — scanning ...\n", attempt);
    int n = WiFi.scanNetworks();
    bool   masterSeen = false, assignedSeen = false;
    String bestGwSsid;                 // strongest gateway that is NOT ours
    int    bestGwRssi = -127;
    ScannedAp assignedAp = {}, masterAp = {}, bestGwAp = {};

    if (n >= 0) {
      for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s == MASTER_SSID) {
          masterSeen = true;
          captureAp(masterAp, i);
          continue;
        }
        if (!s.startsWith(GW_SSID_PREFIX)) continue;
        if (assignedSsid.length() && s == assignedSsid) {
          assignedSeen = true;
          captureAp(assignedAp, i);
        } else if (WiFi.RSSI(i) > bestGwRssi) {
          bestGwRssi = WiFi.RSSI(i);    // remember the best alternative
          bestGwSsid = s;
          captureAp(bestGwAp, i);
        }
      }
      Serial.printf("[WiFi] scan: assigned(%s)=%s  altGw=%s(%d dBm)  master=%s\n",
                    assignedSsid.length() ? assignedSsid.c_str() : "none",
                    assignedSeen ? "yes" : "no",
                    bestGwSsid.length() ? bestGwSsid.c_str() : "none",
                    bestGwSsid.length() ? bestGwRssi : 0,
                    masterSeen ? "yes" : "no");
    }

    WiFi.scanDelete();   // free the scan results before we start associating

    if (gwId == MASTER_DIRECT) {
      // Explicitly pinned to the master: do not fall back to a gateway.
      if (masterSeen && tryConnect(MASTER_SSID, &masterAp)) return;
    } else {
      // Assigned gateway, then the strongest other gateway (so one dead
      // gateway never strands its seats), then the master as a last resort.
      // The master's slots are reserved for gateway uplinks, so a seat landing
      // there is a fallback, not a normal state.
      if (assignedSeen && tryConnect(assignedSsid, &assignedAp)) return;
      if (bestGwSsid.length() && tryConnect(bestGwSsid, &bestGwAp)) return;
      if (masterSeen && tryConnect(MASTER_SSID, &masterAp)) return;
    }

    if (n < 0) {
      // scanNetworks() returns WIFI_SCAN_FAILED (-2) or WIFI_SCAN_RUNNING (-1)
      // on failure. That is "no information", NOT "no APs" — treating it as
      // absence used to reboot a perfectly healthy seat, forever. With no scan
      // data we cannot rank gateways, so try our assigned one and the master
      // blind and let the next attempt rescan.
      Serial.println("[WiFi] scan unavailable — trying assigned gateway / master blind");
      if (gwId == MASTER_DIRECT) {
        if (tryConnect(MASTER_SSID)) return;
      } else {
        if (assignedSsid.length() && tryConnect(assignedSsid)) return;
        if (tryConnect(MASTER_SSID)) return;
      }
    }

    delayServicingSerial(1000);   // let the radio settle before rescanning
  }

  // Hold the console open before rebooting. If this seat is misconfigured
  // (pinned to a gateway that isn't there) this window is the opportunity to
  // type `setgw 0` or `setgw master` and recover it without reflashing.
  Serial.println("[WiFi] no uplink after 3 attempts — restarting in 5s");
  Serial.println("[WiFi] type 'setgw 0' or 'setgw master' now to change the uplink");
  delayServicingSerial(5000);
  ESP.restart();
}

// ─── Serial command handler (laptop ↔ seat) ───────────────────────────
// Line-based, one command per line, case-insensitive. Accepts both
// "setid=1A" and "setid 1A". Mirrors the master's and gateway's console.
static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;

  String cmd = line, arg;
  int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }
  int eq = cmd.indexOf('=');
  if (eq > 0) { arg = cmd.substring(eq + 1); cmd = cmd.substring(0, eq); arg.trim(); }
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    Serial.println(F(
      "\n--- SEAT commands ---\n"
      "  help                  this list\n"
      "  status                id, uplink, sensor, link, heap\n"
      "  setid <name>          set seat id, e.g. setid 3A (reboots)\n"
      "  clearid               return to RandomID pairing mode (reboots)\n"
      "  setgw ...             retired: router mode needs no gateway\n"
      "  diag                  send a health report to the master now\n"
      "  id?                   short identity line\n"
      "  restart               reboot the seat\n"));

  } else if (cmd == "status") {
    uint16_t pressure; bool occ;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    pressure = pressureReading; occ = seatOccupied;
    xSemaphoreGive(dataMutex);
    Serial.printf("id       : %s\n", seatId);
    Serial.println("transport: router WiFi + UDP");
    Serial.printf("uptime   : %lu s   heap %u\n", millis() / 1000, (unsigned)ESP.getFreeHeap());
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("wifi     : %s  IP %s  RSSI %d dBm\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    else
      Serial.println("wifi     : down");
    Serial.printf("udp link : %s  master %s:%u  session %lu  seq %lu\n",
                  wsConnected ? "up" : "DOWN",
                  masterIp == IPAddress() ? "undiscovered" : masterIp.toString().c_str(),
                  MASTER_UDP_PORT, (unsigned long)udpSession,
                  (unsigned long)udpSequence);
    Serial.printf("call ACK : %s  retries %u\n",
                  pendingCall ? "PENDING" : "clear", pendingCallRetries);
    Serial.printf("sensor   : GPIO%d  pressure ADC %u  occupied %s\n",
                  LOAD_SENSOR_PIN, (unsigned)pressure, occ ? "YES" : "no");
    Serial.printf("button   : %d\n", digitalRead(BUTTON_PIN) == LOW ? 1 : 0);
    Serial.printf("seatbelt : GPIO%d  %s\n", SEATBELT_PIN,
                  seatbeltFastened ? "FASTENED" : "OPEN");

  } else if (cmd == "setid") {
    if (arg.equalsIgnoreCase("RandomID")) {
      clearSeatId();
      Serial.println("id cleared — restarting in RandomID pairing mode...");
      delay(200);
      ESP.restart();
    } else if (arg.length() > 0 && arg.length() < 32) {
      saveSeatId(arg.c_str());
      Serial.printf("id saved: %s — restarting...\n", arg.c_str());
      delay(200);
      ESP.restart();
    } else {
      Serial.println("usage: setid <name>   (1-31 chars)");
    }

  } else if (cmd == "clearid") {
    clearSeatId();
    Serial.println("id cleared — restarting in RandomID pairing mode...");
    delay(200);
    ESP.restart();

  } else if (cmd == "setgw") {
    Serial.println("setgw is retired: this firmware always uses router iot_bus + UDP");
    return;
    String a = arg; a.toLowerCase();
    int id; bool valid = true;
    if (a == "master" || a == "m") id = MASTER_DIRECT;
    else { id = a.toInt(); valid = (id >= 0 && id <= 99); }
    if (valid) {
      saveGwId(id);
      Serial.printf("uplink set to %s — restarting...\n",
                    id == MASTER_DIRECT ? "MASTER"
                  : id == 0             ? "auto (strongest gateway)"
                                        : (String(GW_SSID_PREFIX) + id).c_str());
      delay(200);
      ESP.restart();
    } else {
      Serial.println("usage: setgw master | setgw 0 | setgw <1-99>");
    }

  } else if (cmd == "diag") {
    sendDiag();

  } else if (cmd == "id?" || cmd == "id") {
    Serial.printf("id = %s  transport = UDP  master = %s\n", seatId,
                  masterIp == IPAddress() ? "undiscovered" : masterIp.toString().c_str());

  } else if (cmd == "restart") {
    Serial.println("restarting...");
    delay(200);
    ESP.restart();

  } else {
    Serial.printf("unknown command '%s' — type 'help'\n", cmd.c_str());
  }
}

// Diagnostic only: expose the ESP-IDF reason code when association fails.
// This lets us distinguish a bad password/authentication failure from an AP
// timeout or radio-state problem before changing connection behavior.
static void onWiFiDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
  Serial.printf("[WiFi] disconnect reason=%u\n",
                (unsigned)info.wifi_sta_disconnected.reason);
}

static IPAddress subnetBroadcastAddress() {
  IPAddress result;
  const IPAddress local = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  for (uint8_t i = 0; i < 4; ++i) result[i] = local[i] | (uint8_t)~mask[i];
  return result;
}

static void sendMasterDiscovery() {
  if (WiFi.status() != WL_CONNECTED) return;
  char request[128];
  snprintf(request, sizeof(request),
           "{\"type\":\"discover\",\"role\":\"seat\",\"uid\":\"%s\"}",
           nodeUid);
  const IPAddress broadcast = subnetBroadcastAddress();
  if (sendUdpJson(broadcast, MASTER_UDP_PORT, request)) {
    Serial.printf("[UDP] discovering master via %s:%u\n",
                  broadcast.toString().c_str(), MASTER_UDP_PORT);
  }
}

static void applySeatIdCommand(JsonDocument& doc) {
  const char* targetUid = doc["uid"] | "";
  String requestedId = doc["id"] | "";
  requestedId.trim();
  bool valid = strcmp(targetUid, nodeUid) == 0 && !seatIdAssigned &&
               requestedId.length() > 0 && requestedId.length() < sizeof(seatId) &&
               !requestedId.equalsIgnoreCase("RandomID");
  for (size_t i = 0; valid && i < requestedId.length(); ++i) {
    const char c = requestedId[i];
    valid = isAlphaNumeric(c) || c == '-' || c == '_';
  }
  if (!valid) return;

  saveSeatId(requestedId.c_str());
  requestedId.toCharArray(seatId, sizeof(seatId));
  seatIdAssigned = true;
  char confirmation[160];
  snprintf(confirmation, sizeof(confirmation),
           "{\"type\":\"seatIdSaved\",\"event\":\"seatIdSaved\","
           "\"uid\":\"%s\",\"id\":\"%s\"}", nodeUid, seatId);
  sendUdpJson(masterIp, MASTER_UDP_PORT, confirmation);
  Serial.printf("[Seat] ID saved from Head Unit via UDP: %s\n", seatId);

  bool occupied;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  occupied = seatOccupied;
  xSemaphoreGive(dataMutex);
  sendSeatEvent(0, occupied);
}

static void applyClearSeatIdCommand(JsonDocument& doc) {
  const char* targetId  = doc["id"]  | "";
  const char* targetUid = doc["uid"] | "";
  bool match = false;
  if (targetId[0] != '\0' && strcasecmp(targetId, seatId) == 0) match = true;
  if (targetUid[0] != '\0' && strcasecmp(targetUid, nodeUid) == 0) match = true;
  if (match) {
    clearSeatId();
    Serial.printf("[Seat] pairing cleared for '%s' (UID: %s) via app command — restarting in RandomID pairing mode...\n", seatId, nodeUid);
    delay(200);
    ESP.restart();
  }
}

static void handleUdpTransport() {
  int packetSize;
  while ((packetSize = smartbusUdp.parsePacket()) > 0) {
    const IPAddress remote = smartbusUdp.remoteIP();
    char payload[384];
    const int length = smartbusUdp.read(
      reinterpret_cast<uint8_t*>(payload), sizeof(payload) - 1);
    if (length <= 0) continue;
    payload[length] = '\0';

    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, payload)) continue;
    const char* type = doc["type"] | "";

    if (strcmp(type, "master") == 0) {
      const bool changed = masterIp != remote;
      masterIp = remote;
      wsHost = remote.toString();
      lastMasterAckMs = millis();
      wsConnected = true;
      if (changed) {
        Serial.printf("[UDP] master discovered at %s:%u\n",
                      wsHost.c_str(), MASTER_UDP_PORT);
        bool occupied;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        occupied = seatOccupied;
        xSemaphoreGive(dataMutex);
        sendSeatEvent(0, occupied);
      }
    } else if (strcmp(type, "ack") == 0) {
      const char* ackUid = doc["uid"] | "";
      const uint32_t ackSession = doc["session"] | 0U;
      const uint32_t ackSeq = doc["seq"] | 0U;
      if (strcmp(ackUid, nodeUid) == 0 && ackSession == udpSession) {
        lastMasterAckMs = millis();
        wsConnected = true;
        if (pendingCall && ackSeq == pendingCallSeq) {
          pendingCall = false;
          Serial.printf("[UDP] CALL acknowledged seq=%lu after %u attempt(s)\n",
                        (unsigned long)ackSeq, pendingCallRetries + 1);
        }
      }
    } else if (strcmp(doc["action"] | "", "setSeatId") == 0) {
      masterIp = remote;
      applySeatIdCommand(doc);
    } else if (strcmp(doc["action"] | "", "clearSeatId") == 0) {
      applyClearSeatIdCommand(doc);
    }
  }

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    wsConnected = false;
    masterIp = IPAddress();
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect >= 3000) {
      lastReconnect = now;
      Serial.println("[WiFi] reconnecting to iot_bus...");
      WiFi.reconnect();
    }
    return;
  }

  if (lastMasterAckMs && now - lastMasterAckMs > MASTER_ACK_TIMEOUT_MS) {
    if (wsConnected) Serial.println("[UDP] master ACK timeout - rediscovering");
    wsConnected = false;
    masterIp = IPAddress();
  }
  if (masterIp == IPAddress() && now - lastDiscoveryMs >= DISCOVERY_INTERVAL_MS) {
    lastDiscoveryMs = now;
    sendMasterDiscovery();
  }

  if (pendingCall && masterIp != IPAddress()) {
    const unsigned long interval = pendingCallRetries < 10
      ? CALL_RETRY_FAST_MS : CALL_RETRY_SLOW_MS;
    if (now - pendingCallSentMs >= interval) {
      pendingCallSentMs = now;
      ++pendingCallRetries;
      sendSeatPacket(pendingCallSeq, 1, pendingCallOccupied, seatbeltFastened);
    }
  }
}

static void connectRouter() {
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  Serial.printf("[WiFi] connecting to router %s", ROUTER_SSID);
  WiFi.begin(ROUTER_SSID, ROUTER_PASS);
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    handleSerial();
    delay(250);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] router connected - IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] router unavailable - continuing with auto reconnect");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("[Firmware] Passenger Seat v1.7 ESP32-WROOM-32U DynamicID");

  // Why did we boot? This is the single most useful clue when a seat drops
  // out in the field with no USB attached:
  //   POWERON(1)=clean start   BROWNOUT(11)=supply sagged (power problem)
  //   TASK_WDT/INT_WDT(6,7)    PANIC(4)=crash   SW(3)=our own ESP.restart()
  // It is also reported to the master (see sendDiag) so it is visible without
  // physically reaching the seat.
  Serial.printf("[Boot] reset reason: %d\n", (int)esp_reset_reason());

  // -- was low-level RTC clock API to drop CPU to 80MHz (removed in core 3.x) --
  // rtc_cpu_freq_config_t config;
  // rtc_clk_cpu_freq_get_config(&config);
  // rtc_clk_cpu_freq_to_config(RTC_CPU_FREQ_80M, &config);
  // rtc_clk_cpu_freq_set_config_fast(&config);
  setCpuFrequencyMhz(80);

  WiFi.onEvent(onWiFiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);   // station mode (also needed for MAC fallback in loadSeatId)
  // Disable WiFi modem sleep. By default the radio powers down between beacons
  // (WIFI_PS_MIN_MODEM); on a marginal 5V supply that leads to missed beacons
  // and a silent drop-off after tens of minutes of otherwise healthy uptime.
  // A seat node is mains-powered and must stay associated, so trade the few mA
  // for a link that stays up.
  WiFi.setSleep(false);
  loadSeatId();          // seat id from NVS, else MAC-derived unique fallback

  pinMode(LOAD_SENSOR_PIN, INPUT);
  analogReadResolution(12);
  // GPIO34 has no internal pull-up or pull-down. The external FSR voltage
  // divider must always bias this input so it never floats.
  analogSetPinAttenuation(LOAD_SENSOR_PIN, ADC_11db);
  Serial.printf("[Pressure] analog sensor ready on GPIO%d (ON >= %u, OFF <= %u)\n",
                LOAD_SENSOR_PIN, (unsigned)PRESSURE_OCCUPIED_ON,
                (unsigned)PRESSURE_OCCUPIED_OFF);

  // -- was INPUT: floating pin caused random ISR fires from noise --
  pinMode(BUTTON_PIN, INPUT_PULLUP);   // button to GND: pressed = LOW = sensorState 1
  // Seatbelt buckle switch to GND. INPUT_PULLUP keeps the pin HIGH when the
  // buckle is open; a closed buckle pulls it LOW (see SEATBELT_FASTENED_LEVEL).
  pinMode(SEATBELT_PIN, INPUT_PULLUP);
  seatbeltRawState = digitalRead(SEATBELT_PIN);
  seatbeltStableState = seatbeltRawState;
  seatbeltFastened = (seatbeltStableState == SEATBELT_FASTENED_LEVEL);
  seatbeltRawChangedMs = millis();
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(50);
  // Paint "not linked" yellow right away. setup() then blocks in
  // connectUplink() for several seconds, and loop() — which normally drives
  // the strip — does not run until that returns. Without this the strip stays
  // dark for the whole scan/join and yellow is never actually visible.
  fill_solid(leds, NUM_LEDS, CRGB::Yellow);
  FastLED.show();
  buttonRawState = digitalRead(BUTTON_PIN);
  buttonStableState = buttonRawState;
  buttonArmed = (buttonStableState == HIGH);  // held at boot must be released first
  buttonRawChangedMs = millis();

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreate(sensorTask, "Sensor", 8192, NULL, 1, NULL);

  // ─── Join master (or gateway), then open the WebSocket ─────────────
  // -- removed: ESP-NOW init (WIFI_MODE_STA + WiFi.disconnect + esp_now_init) --
  udpSession = esp_random();
  if (udpSession == 0) udpSession = 1;
  connectRouter();
  if (!smartbusUdp.begin(SEAT_UDP_PORT)) {
    Serial.printf("[UDP] ERROR: cannot bind local port %u\n", SEAT_UDP_PORT);
  } else {
    Serial.printf("[UDP] local receiver ready on port %u, session %lu\n",
                  SEAT_UDP_PORT, (unsigned long)udpSession);
  }
}

void loop() {
  // Update LEDs from the main loop to keep FastLED and WiFi servicing serialized.
  //   yellow → not linked to the master (occupancy is not being reported, so
  //            showing a seat colour here would be a lie)
  //   green  → seat occupied with the seatbelt fastened
  //   red    → seat occupied without the seatbelt
  //   white  → seat free
  {
    bool occupied;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    occupied = seatOccupied;
    xSemaphoreGive(dataMutex);
    const bool fastened = seatbeltFastened;
    CRGB colour = !wsConnected ? CRGB::Yellow
                : !occupied    ? CRGB::White
                : fastened     ? CRGB::Green
                :                CRGB::Red;
    fill_solid(leds, NUM_LEDS, colour);
    FastLED.show();
  }

  handleUdpTransport();

  // ── Link watchdog ──────────────────────────────────────────────────
  // The seat must hold a LIVE WebSocket to the master. If it's down for >15s
  // — WiFi drop, AP eviction, or a half-open socket that never recovers — a
  // reboot re-runs the full scan+connect, which reliably re-registers us.
  // (WiFi.status() alone is unreliable here: the association can linger while
  // the WS link is dead, so we watchdog on the actual WS state.)
  // UDP health/reconnect/discovery is handled without rebooting the seat.

  // Button → seat call. Accept exactly one event for each stable press, then
  // keep it disarmed until a stable release is observed.
  const bool rawButton = digitalRead(BUTTON_PIN);
  if (rawButton != buttonRawState) {
    buttonRawState = rawButton;
    buttonRawChangedMs = millis();
  }
  if (buttonStableState != buttonRawState &&
      millis() - buttonRawChangedMs >= BUTTON_DEBOUNCE_MS) {
    buttonStableState = buttonRawState;
    if (buttonStableState == HIGH) {
      buttonArmed = true;
      Serial.println("[Button] released — armed");
    } else if (buttonArmed) {
      buttonArmed = false;
      if (!buttonLockoutActive || millis() - lastButtonCallMs >= BUTTON_LOCKOUT_MS) {
        lastButtonCallMs = millis();
        buttonLockoutActive = true;
        bool occupied;
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        occupied = seatOccupied;
        xSemaphoreGive(dataMutex);
        Serial.println("[Button] stable press — sending one call, lockout 10s");
        sendSeatEvent(1, occupied);
      } else {
        Serial.println("[Button] press ignored — 10s lockout active");
      }
    }
  }

  // Seatbelt buckle → debounced state, reported as telemetry. A change is sent
  // immediately so the master can raise the "no seatbelt" warning without
  // waiting for the next heartbeat.
  const bool rawSeatbelt = digitalRead(SEATBELT_PIN);
  if (rawSeatbelt != seatbeltRawState) {
    seatbeltRawState = rawSeatbelt;
    seatbeltRawChangedMs = millis();
  }
  if (seatbeltStableState != seatbeltRawState &&
      millis() - seatbeltRawChangedMs >= SEATBELT_DEBOUNCE_MS) {
    seatbeltStableState = seatbeltRawState;
    const bool fastened = (seatbeltStableState == SEATBELT_FASTENED_LEVEL);
    if (fastened != seatbeltFastened) {
      seatbeltFastened = fastened;
      seatbeltChanged = true;
      Serial.printf("[Seatbelt] %s\n", fastened ? "fastened" : "OPEN");
    }
  }

  // Occupancy change → telemetry only (sensorState=0, no announcement)
  if (occupancyChanged) {
    bool occupied;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    occupied = seatOccupied;
    occupancyChanged = false;
    xSemaphoreGive(dataMutex);
    sendSeatEvent(0, occupied);
  }

  // Seatbelt change → telemetry only (sensorState=0); the master decides
  // whether the "no seatbelt" announcement is warranted.
  // Lockout 10000ms prevents spamming telemetry if the buckle switch bounces or toggles on/off.
  if (seatbeltChanged) {
    seatbeltChanged = false;
    if (!seatbeltLockoutActive || millis() - lastSeatbeltCallMs >= SEATBELT_LOCKOUT_MS) {
      lastSeatbeltCallMs = millis();
      seatbeltLockoutActive = true;
      bool occupied;
      xSemaphoreTake(dataMutex, portMAX_DELAY);
      occupied = seatOccupied;
      xSemaphoreGive(dataMutex);
      Serial.printf("[Seatbelt] stable change — sending telemetry (%s), lockout 10s\n",
                    seatbeltFastened ? "fastened" : "OPEN");
      sendSeatEvent(0, occupied);
    } else {
      Serial.println("[Seatbelt] change ignored — 10s lockout active");
    }
  }

  // Heartbeat → refresh the master's lastSeen so a still/occupied seat isn't
  // marked offline. sensorState=0, so it never triggers an announcement.
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    bool occupied;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    occupied = seatOccupied;
    xSemaphoreGive(dataMutex);
    sendSeatEvent(0, occupied);
  }

  // Health report every 60 s — cheap, and the only visibility into a seat
  // once its USB cable is removed.
  static unsigned long lastDiag = 0;
  if (millis() - lastDiag >= 60000) {
    lastDiag = millis();
    sendDiag();
  }

  handleSerial();   // laptop console — type 'help' for the command list

  delay(10);
}
