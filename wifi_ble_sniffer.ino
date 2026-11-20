#include <SPI.h>
#include <Wire.h>
#include <RF24.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include "esp_wifi.h"

// user config, edit for your wiring

// OLED (I2C, SH1106)
#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_ADDR    0x3C
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

// joystick
#define JOY_VRX      0
#define JOY_VRY      1
#define JOY_SW       10  // to GND when pressed
#define JOY_DEADZONE 700 // around calibrated center, ADC is 0-4095
#define JOY_REPEAT_MS 220
#define JOY_SAMPLES   6  // averaged per read
#define DOUBLE_TAP_MS 350 // two taps in this window = back

// flip these if the stick feels rotated or inverted
#define JOY_SWAP_XY       false
#define JOY_INVERT_UPDOWN false

// NRF24L01
#define NRF_CE       2
#define NRF_CSN      3
#define NRF_SCK      4
#define NRF_MISO     5
#define NRF_MOSI     6

Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
RF24 radio(NRF_CE, NRF_CSN);

// auto-prototype chokes on IRAM_ATTR, so declare it here
void IRAM_ATTR wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type);

enum Screen {
  SCR_HOME, SCR_WIFI, SCR_WIFI_APMENU, SCR_WIFI_DETAIL, SCR_WIFI_DEAUTH,
  SCR_WIFI_CLIENTS, SCR_PROBE, SCR_EXPORT, SCR_BLE, SCR_ABOUT, SCR_JOYDEBUG
};
Screen currentScreen = SCR_HOME;
int homeCursor = 0;
const char* homeItems[] = { "WiFi Scan", "BLE Sniff", "Probe Log", "Export Report", "Joystick Debug", "About" };
const int homeItemCount = 6;

enum Dir { DIR_NONE, DIR_UP, DIR_DOWN };
int joyCenterX = 2048, joyCenterY = 2048;
bool joyArmed = true;
unsigned long lastJoyMove = 0;

bool lastSwState = HIGH;
bool pendingTap = false;
unsigned long pendingTapTime = 0;

void calibrateJoystick() {
  long sx = 0, sy = 0;
  const int N = 30;
  for (int i = 0; i < N; i++) {
    sx += analogRead(JOY_VRX);
    sy += analogRead(JOY_VRY);
    delay(5);
  }
  joyCenterX = sx / N;
  joyCenterY = sy / N;
}

void readJoyFiltered(int &x, int &y) {
  long sx = 0, sy = 0;
  for (int i = 0; i < JOY_SAMPLES; i++) {
    sx += analogRead(JOY_VRX);
    sy += analogRead(JOY_VRY);
  }
  x = sx / JOY_SAMPLES;
  y = sy / JOY_SAMPLES;
}

Dir getJoyDirection() {
  int x, y;
  readJoyFiltered(x, y);

  int navValue  = JOY_SWAP_XY ? x : y;
  int navCenter = JOY_SWAP_XY ? joyCenterX : joyCenterY;
  if (JOY_INVERT_UPDOWN) navValue = 2 * navCenter - navValue;

  unsigned long now = millis();
  if (abs(navValue - navCenter) < JOY_DEADZONE / 2) joyArmed = true;
  if (now - lastJoyMove < JOY_REPEAT_MS) return DIR_NONE;

  if (joyArmed && navValue < navCenter - JOY_DEADZONE) { joyArmed = false; lastJoyMove = now; return DIR_UP; }
  if (joyArmed && navValue > navCenter + JOY_DEADZONE) { joyArmed = false; lastJoyMove = now; return DIR_DOWN; }
  return DIR_NONE;
}

// vendor OUI lookup, not exhaustive
struct OuiEntry { uint8_t prefix[3]; const char* name; };
const OuiEntry ouiTable[] = {
  {{0xB8, 0x27, 0xEB}, "RaspberryPi"},
  {{0xDC, 0xA6, 0x32}, "RaspberryPi"},
  {{0x24, 0x6F, 0x28}, "Espressif"},
  {{0xA4, 0xCF, 0x12}, "Espressif"},
  {{0x3C, 0x71, 0xBF}, "Espressif"},
  {{0xAC, 0xDE, 0x48}, "Apple"},
  {{0xF0, 0x18, 0x98}, "Apple"},
  {{0x3C, 0x5A, 0xB4}, "Google"},
  {{0xF0, 0x9F, 0xC2}, "Ubiquiti"},
  {{0x00, 0x1D, 0x0F}, "TP-Link"},
  {{0x50, 0xC7, 0xBF}, "TP-Link"},
  {{0x34, 0xCE, 0x00}, "Xiaomi"},
  {{0x28, 0x6C, 0x07}, "Xiaomi"},
  {{0x00, 0x50, 0xF2}, "Microsoft"},
};
const int ouiTableCount = sizeof(ouiTable) / sizeof(ouiTable[0]);

const char* lookupVendor(const uint8_t* mac) {
  for (int i = 0; i < ouiTableCount; i++) {
    if (mac[0] == ouiTable[i].prefix[0] && mac[1] == ouiTable[i].prefix[1] && mac[2] == ouiTable[i].prefix[2])
      return ouiTable[i].name;
  }
  return "Unknown";
}

#define MAX_CLIENTS_PER_AP 4
struct ApEntry {
  uint8_t bssid[6];
  char ssid[33];
  int8_t rssi;
  uint8_t channel;
  char enc[5];      // "OPEN","WEP","WPA","WPA2","WPA3"
  bool wps;
  bool weak;        // OPEN or WEP
  uint8_t clientMacsAp[MAX_CLIENTS_PER_AP][6];
  uint8_t clientCountAp;
};
#define MAX_APS 24
ApEntry apList[MAX_APS];
int apCount = 0;

#define MAX_CLIENTS 40
uint8_t clientMacs[MAX_CLIENTS][6]; // unique clients, for the header count
int clientCount = 0;

uint8_t wifiChannel = 1;
unsigned long lastChannelHop = 0;
const unsigned long CHANNEL_HOP_MS = 350;
bool wifiModeActive = false;
bool wifiLockChannel = false;

int wifiCursor = 0;
int wifiScrollOffset = 0;
#define WIFI_VISIBLE_ROWS 5

int selectedApIndex = -1;
#define GRAPH_POINTS 64
int8_t rssiGraph[GRAPH_POINTS];
unsigned long lastGraphSample = 0;

int apMenuCursor = 0;
const char* apMenuItems[] = { "Graph", "Deauth Monitor", "Clients" };
const int apMenuItemCount = 3;
bool apMenuNeedsRedraw = true;

uint32_t deauthCount = 0;
uint32_t disassocCount = 0;
unsigned long lastDeauthSeen = 0;

int clientsScrollOffset = 0;
#define CLIENTS_VISIBLE_ROWS 5

struct ProbeEntry { uint8_t mac[6]; char ssid[33]; };
#define MAX_PROBES 20
ProbeEntry probeList[MAX_PROBES];
int probeCount = 0;
int probeScrollOffset = 0;
#define PROBE_VISIBLE_ROWS 5

const uint8_t bleChannels[3]    = { 37, 38, 39 };
const uint8_t bleRawChannels[3] = { 2, 26, 80 };
int bleChIdx = 0;
unsigned long lastBleHop = 0;
const unsigned long BLE_HOP_MS = 100;

uint32_t bleAdvCount = 0;
uint32_t bleLastAdvMac[6] = {0};
char bleLastName[20] = "";
bool bleModeActive = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(JOY_SW, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(OLED_ADDR, true);
  display.setRotation(0);

  drawBootScreen();
  delay(1800);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 24);
  display.println(" Calibrating");
  display.println(" joystick...");
  display.println(" (don't touch it)");
  display.display();

  calibrateJoystick();

  drawHomeForce();

  SPI.begin(NRF_SCK, NRF_MISO, NRF_MOSI, NRF_CSN);
  radio.begin();
  radio.stopListening();
  radio.powerDown();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}

void loop() {
  handleInput();

  switch (currentScreen) {
    case SCR_HOME:          drawHome(); break;
    case SCR_WIFI:          runWifiScan();   drawWifi();       break;
    case SCR_WIFI_APMENU:   drawWifiApMenu(); break;
    case SCR_WIFI_DETAIL:   runWifiDetail(); drawWifiDetail(); break;
    case SCR_WIFI_DEAUTH:   drawDeauthMonitor(); break;
    case SCR_WIFI_CLIENTS:  drawClients(); break;
    case SCR_PROBE:         runWifiScan();   drawProbeLog();   break;
    case SCR_EXPORT:        drawExport(); break;
    case SCR_BLE:           runBleSniff();   drawBle();        break;
    case SCR_ABOUT:         drawAbout(); break;
    case SCR_JOYDEBUG:      drawJoyDebug(); break;
  }

  delay(15);
}

void handleInput() {
  Dir dir = getJoyDirection();

  bool sw = digitalRead(JOY_SW); // LOW = pressed
  unsigned long now = millis();
  bool selectEvent = false, backEvent = false;

  // second tap inside the window = back, otherwise select once it expires
  if (lastSwState == LOW && sw == HIGH) {
    if (pendingTap && (now - pendingTapTime < DOUBLE_TAP_MS)) {
      backEvent = true;
      pendingTap = false;
    } else {
      pendingTap = true;
      pendingTapTime = now;
    }
  }
  lastSwState = sw;

  if (pendingTap && (now - pendingTapTime >= DOUBLE_TAP_MS)) {
    selectEvent = true;
    pendingTap = false;
  }

  switch (currentScreen) {
    case SCR_HOME:
      if (dir == DIR_UP)   homeCursor = (homeCursor - 1 + homeItemCount) % homeItemCount;
      if (dir == DIR_DOWN) homeCursor = (homeCursor + 1) % homeItemCount;
      if (selectEvent) onHomeSelect();
      if (backEvent) { currentScreen = SCR_JOYDEBUG; }
      break;

    case SCR_WIFI:
      if (dir == DIR_UP)   moveWifiCursor(-1);
      if (dir == DIR_DOWN) moveWifiCursor(1);
      if (selectEvent) enterWifiApMenu();
      if (backEvent) { stopWifiScan(); currentScreen = SCR_HOME; }
      break;

    case SCR_WIFI_APMENU:
      if (dir == DIR_UP)   apMenuCursor = (apMenuCursor - 1 + apMenuItemCount) % apMenuItemCount;
      if (dir == DIR_DOWN) apMenuCursor = (apMenuCursor + 1) % apMenuItemCount;
      if (selectEvent) {
        if (apMenuCursor == 0) enterWifiDetail();
        else if (apMenuCursor == 1) enterDeauthMonitor();
        else enterClientsScreen();
      }
      if (backEvent) { wifiLockChannel = false; currentScreen = SCR_WIFI; }
      break;

    case SCR_WIFI_DETAIL:
      if (selectEvent || backEvent) { apMenuNeedsRedraw = true; currentScreen = SCR_WIFI_APMENU; }
      break;

    case SCR_WIFI_DEAUTH:
      if (selectEvent || backEvent) { apMenuNeedsRedraw = true; currentScreen = SCR_WIFI_APMENU; }
      break;

    case SCR_WIFI_CLIENTS:
      if (dir == DIR_UP   && clientsScrollOffset > 0) clientsScrollOffset--;
      if (dir == DIR_DOWN && clientsScrollOffset < apList[selectedApIndex].clientCountAp - CLIENTS_VISIBLE_ROWS)
        clientsScrollOffset++;
      if (selectEvent || backEvent) { apMenuNeedsRedraw = true; currentScreen = SCR_WIFI_APMENU; }
      break;

    case SCR_PROBE:
      if (dir == DIR_UP   && probeScrollOffset > 0) probeScrollOffset--;
      if (dir == DIR_DOWN && probeScrollOffset < probeCount - PROBE_VISIBLE_ROWS) probeScrollOffset++;
      if (selectEvent || backEvent) { stopWifiScan(); currentScreen = SCR_HOME; }
      break;

    case SCR_EXPORT:
      if (selectEvent || backEvent) currentScreen = SCR_HOME;
      break;

    case SCR_BLE:
      if (selectEvent || backEvent) { stopBleSniff(); currentScreen = SCR_HOME; }
      break;

    case SCR_ABOUT:
      if (selectEvent || backEvent) currentScreen = SCR_HOME;
      break;

    case SCR_JOYDEBUG:
      if (selectEvent || backEvent) currentScreen = SCR_HOME;
      break;
  }
}

void onHomeSelect() {
  switch (homeCursor) {
    case 0: enterWifiScreen();  break;
    case 1: enterBleScreen();   break;
    case 2: enterProbeLog();    break;
    case 3: enterExportReport(); break;
    case 4: currentScreen = SCR_JOYDEBUG; break;
    case 5: currentScreen = SCR_ABOUT; break;
  }
}

void enterWifiScreen() {
  apCount = 0;
  clientCount = 0;
  wifiCursor = 0;
  wifiScrollOffset = 0;
  wifiLockChannel = false;
  memset(apList, 0, sizeof(apList));
  memset(clientMacs, 0, sizeof(clientMacs));

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
  wifiChannel = 1;
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);

  wifiModeActive = true;
  currentScreen = SCR_WIFI;
}

void stopWifiScan() {
  esp_wifi_set_promiscuous(false);
  wifiModeActive = false;
  wifiLockChannel = false;
}

void runWifiScan() {
  if (!wifiModeActive || wifiLockChannel) return;
  unsigned long now = millis();
  if (now - lastChannelHop > CHANNEL_HOP_MS) {
    wifiChannel = (wifiChannel % 13) + 1;
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    lastChannelHop = now;
  }
}

void moveWifiCursor(int delta) {
  if (apCount == 0) return;
  wifiCursor += delta;
  if (wifiCursor < 0) wifiCursor = 0;
  if (wifiCursor > apCount - 1) wifiCursor = apCount - 1;
  if (wifiCursor < wifiScrollOffset) wifiScrollOffset = wifiCursor;
  if (wifiCursor >= wifiScrollOffset + WIFI_VISIBLE_ROWS) wifiScrollOffset = wifiCursor - WIFI_VISIBLE_ROWS + 1;
}

bool macEquals(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

void addOrUpdateAp(const uint8_t* bssid, int8_t rssi, uint8_t channel,
                    const uint8_t* ssidBytes, uint8_t ssidLen,
                    const char* enc, bool wps, bool weak) {
  for (int i = 0; i < apCount; i++) {
    if (macEquals(apList[i].bssid, bssid)) {
      apList[i].rssi = rssi;
      apList[i].channel = channel;
      return;
    }
  }
  if (apCount >= MAX_APS) return;
  ApEntry &e = apList[apCount];
  memcpy(e.bssid, bssid, 6);
  e.rssi = rssi;
  e.channel = channel;
  uint8_t len = ssidLen > 32 ? 32 : ssidLen;
  memcpy(e.ssid, ssidBytes, len);
  e.ssid[len] = '\0';
  if (len == 0) strcpy(e.ssid, "(hidden)");
  strncpy(e.enc, enc, 4); e.enc[4] = '\0';
  e.wps = wps;
  e.weak = weak;
  e.clientCountAp = 0;
  apCount++;
}

void maybeAddClient(const uint8_t* mac) {
  for (int i = 0; i < clientCount; i++) if (macEquals(clientMacs[i], mac)) return;
  if (clientCount >= MAX_CLIENTS) return;
  memcpy(clientMacs[clientCount], mac, 6);
  clientCount++;
}

void addApClient(int apIndex, const uint8_t* mac) {
  ApEntry &e = apList[apIndex];
  for (int i = 0; i < e.clientCountAp; i++) if (macEquals(e.clientMacsAp[i], mac)) return;
  if (e.clientCountAp >= MAX_CLIENTS_PER_AP) return;
  memcpy(e.clientMacsAp[e.clientCountAp], mac, 6);
  e.clientCountAp++;
}

void addProbeEntry(const uint8_t* mac, const uint8_t* ssidBytes, uint8_t ssidLen) {
  char ssid[33];
  uint8_t len = ssidLen > 32 ? 32 : ssidLen;
  memcpy(ssid, ssidBytes, len);
  ssid[len] = '\0';
  if (len == 0) strcpy(ssid, "(broadcast)");
  for (int i = 0; i < probeCount; i++) {
    if (macEquals(probeList[i].mac, mac) && strcmp(probeList[i].ssid, ssid) == 0) return;
  }
  if (probeCount >= MAX_PROBES) return;
  memcpy(probeList[probeCount].mac, mac, 6);
  strcpy(probeList[probeCount].ssid, ssid);
  probeCount++;
}

// walk the tagged params for RSN/WPA/WPS
void parseBeaconSecurity(const uint8_t* payload, int len, char* encOut, bool* wpsOut, bool* weakOut) {
  bool privacyBit = (payload[34] & 0x10) != 0;
  bool hasRSN = false, hasWPA = false, hasWPS = false;
  uint8_t akmSuiteType = 0;

  int idx = 36;
  while (idx + 1 < len) {
    uint8_t tagNum = payload[idx];
    uint8_t tagLen = payload[idx + 1];
    if (idx + 2 + tagLen > len) break;
    const uint8_t* tagData = payload + idx + 2;

    if (tagNum == 48 && tagLen >= 8) { // RSN
      hasRSN = true;
      int p = 2 + 4; // skip version(2) + group cipher(4)
      if (p + 2 <= tagLen) {
        uint16_t pairwiseCount = tagData[p] | (tagData[p + 1] << 8);
        p += 2 + 4 * pairwiseCount;
        if (p + 2 <= tagLen) {
          uint16_t akmCount = tagData[p] | (tagData[p + 1] << 8);
          p += 2;
          if (akmCount >= 1 && p + 4 <= tagLen) akmSuiteType = tagData[p + 3];
        }
      }
    } else if (tagNum == 221 && tagLen >= 4) {
      if (tagData[0] == 0x00 && tagData[1] == 0x50 && tagData[2] == 0xF2) {
        if (tagData[3] == 1) hasWPA = true;
        if (tagData[3] == 4) hasWPS = true;
      }
    }
    idx += 2 + tagLen;
  }

  const char* enc;
  if (!privacyBit) enc = "OPEN";
  else if (hasRSN) enc = (akmSuiteType == 8) ? "WPA3" : "WPA2";
  else if (hasWPA) enc = "WPA";
  else enc = "WEP";

  strncpy(encOut, enc, 4); encOut[4] = '\0';
  *wpsOut = hasWPS;
  *weakOut = (!privacyBit) || (strcmp(enc, "WEP") == 0);
}

// promiscuous callback, runs in the wifi task so keep it quick
void IRAM_ATTR wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* payload = pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;
  uint8_t channel = pkt->rx_ctrl.channel;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;

  uint8_t fc0 = payload[0];

  // beacon
  if (fc0 == 0x80 && len >= 38) {
    const uint8_t* bssid = payload + 16;
    int offset = 36;
    uint8_t ssidLen = 0;
    const uint8_t* ssidPtr = payload + offset + 2;
    if (offset + 1 < len && payload[offset] == 0x00) {
      ssidLen = payload[offset + 1];
      if (offset + 2 + ssidLen > len) ssidLen = 0;
    }
    char enc[5]; bool wps, weak;
    parseBeaconSecurity(payload, len, enc, &wps, &weak);
    addOrUpdateAp(bssid, rssi, channel, ssidPtr, ssidLen, enc, wps, weak);
  }
  // probe request
  else if (fc0 == 0x40 && len >= 26) {
    const uint8_t* srcMac = payload + 10;
    uint8_t ssidLen = 0;
    const uint8_t* ssidPtr = payload + 24 + 2;
    if (payload[24] == 0x00) {
      ssidLen = payload[25];
      if (24 + 2 + ssidLen > len) ssidLen = 0;
    }
    addProbeEntry(srcMac, ssidPtr, ssidLen);
  }
  // data frame, tie the client to a known AP
  else if ((fc0 & 0x0C) == 0x08 && len >= 22) {
    const uint8_t* addr1 = payload + 4;
    const uint8_t* addr2 = payload + 10;
    maybeAddClient(addr2);
    for (int i = 0; i < apCount; i++) {
      if (macEquals(addr1, apList[i].bssid)) { addApClient(i, addr2); break; }
      if (macEquals(addr2, apList[i].bssid)) { addApClient(i, addr1); break; }
    }
  }
  // deauth / disassoc
  if (currentScreen == SCR_WIFI_DEAUTH && (fc0 == 0xC0 || fc0 == 0xA0)) {
    const uint8_t* bssid = payload + 16;
    if (macEquals(bssid, apList[selectedApIndex].bssid)) {
      if (fc0 == 0xC0) deauthCount++; else disassocCount++;
      lastDeauthSeen = millis();
    }
  }
}

void drawWifi() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("ch:%2d APs:%d clients:%d", wifiChannel, apCount, clientCount);

  for (int row = 0; row < WIFI_VISIBLE_ROWS; row++) {
    int i = wifiScrollOffset + row;
    display.setCursor(0, 9 + row * 9);
    if (i >= apCount) continue;
    display.print(i == wifiCursor ? ">" : " ");
    display.printf("%-8.8s%4d %-4s%s", apList[i].ssid, apList[i].rssi, apList[i].enc, apList[i].wps ? "*" : "");
  }
  display.display();
}

void enterWifiApMenu() {
  if (apCount == 0) return;
  selectedApIndex = wifiCursor;
  apMenuCursor = 0;
  apMenuNeedsRedraw = true;
  wifiLockChannel = true;
  esp_wifi_set_channel(apList[selectedApIndex].channel, WIFI_SECOND_CHAN_NONE);
  currentScreen = SCR_WIFI_APMENU;
}

void drawWifiApMenu() {
  static int lastCursor = -1;
  if (!apMenuNeedsRedraw && lastCursor == apMenuCursor) return;
  lastCursor = apMenuCursor;
  apMenuNeedsRedraw = false;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("%-12.12s ch%d %s%s\n", apList[selectedApIndex].ssid, apList[selectedApIndex].channel,
                  apList[selectedApIndex].enc, apList[selectedApIndex].weak ? "!" : "");
  for (int i = 0; i < apMenuItemCount; i++) {
    display.setCursor(4, 14 + i * 11);
    display.print(i == apMenuCursor ? "> " : "  ");
    display.println(apMenuItems[i]);
  }
  display.display();
}

void enterWifiDetail() {
  for (int i = 0; i < GRAPH_POINTS; i++) rssiGraph[i] = apList[selectedApIndex].rssi;
  lastGraphSample = millis();
  currentScreen = SCR_WIFI_DETAIL;
}

void runWifiDetail() {
  unsigned long now = millis();
  if (now - lastGraphSample > 150) {
    for (int i = 0; i < GRAPH_POINTS - 1; i++) rssiGraph[i] = rssiGraph[i + 1];
    rssiGraph[GRAPH_POINTS - 1] = apList[selectedApIndex].rssi;
    lastGraphSample = now;
  }
}

void drawWifiDetail() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 150) return;
  lastDraw = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("%-13.13s %d dBm\n", apList[selectedApIndex].ssid, apList[selectedApIndex].rssi);

  const int graphTop = 11, graphH = 45, graphLeft = 0, graphW = 128;
  display.drawRect(graphLeft, graphTop, graphW, graphH, SH110X_WHITE);

  const int minDb = -95, maxDb = -30;
  int prevX = -1, prevY = -1;
  for (int i = 0; i < GRAPH_POINTS; i++) {
    int x = graphLeft + (i * graphW) / GRAPH_POINTS;
    int v = rssiGraph[i];
    if (v < minDb) v = minDb;
    if (v > maxDb) v = maxDb;
    int y = graphTop + graphH - 1 - ((v - minDb) * (graphH - 1)) / (maxDb - minDb);
    if (prevX >= 0) display.drawLine(prevX, prevY, x, y, SH110X_WHITE);
    prevX = x; prevY = y;
  }
  display.display();
}

void enterDeauthMonitor() {
  deauthCount = 0;
  disassocCount = 0;
  lastDeauthSeen = 0;
  currentScreen = SCR_WIFI_DEAUTH;
}

void drawDeauthMonitor() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  bool recentHit = (lastDeauthSeen != 0) && (millis() - lastDeauthSeen < 2000);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("%-13.13s ch%d\n", apList[selectedApIndex].ssid, apList[selectedApIndex].channel);
  display.printf("Deauth:%lu Disassoc:%lu\n", (unsigned long)deauthCount, (unsigned long)disassocCount);
  if (recentHit) {
    display.setTextSize(2);
    display.setCursor(14, 30);
    display.print("ALERT!");
  } else {
    display.setCursor(0, 30);
    display.print(deauthCount + disassocCount > 0 ? "quiet now" : "monitoring...");
  }
  display.display();
}

void enterClientsScreen() {
  clientsScrollOffset = 0;
  currentScreen = SCR_WIFI_CLIENTS;
}

void drawClients() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  ApEntry &e = apList[selectedApIndex];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("%-13.13s (%d)\n", e.ssid, e.clientCountAp);
  for (int row = 0; row < CLIENTS_VISIBLE_ROWS; row++) {
    int i = clientsScrollOffset + row;
    display.setCursor(0, 11 + row * 9);
    if (i >= e.clientCountAp) continue;
    display.printf("%02X:%02X:%02X %s", e.clientMacsAp[i][3], e.clientMacsAp[i][4], e.clientMacsAp[i][5],
                    lookupVendor(e.clientMacsAp[i]));
  }
  if (e.clientCountAp == 0) { display.setCursor(0, 20); display.print("none seen yet"); }
  display.display();
}

void enterProbeLog() {
  probeCount = 0;
  probeScrollOffset = 0;

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCallback);
  wifiChannel = 1;
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
  wifiModeActive = true;
  wifiLockChannel = false;

  currentScreen = SCR_PROBE;
}

void drawProbeLog() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 200) return;
  lastDraw = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("Probe Log ch:%2d (%d)", wifiChannel, probeCount);
  for (int row = 0; row < PROBE_VISIBLE_ROWS; row++) {
    int i = probeScrollOffset + row;
    display.setCursor(0, 9 + row * 9);
    if (i >= probeCount) continue;
    display.printf("%s %-14.14s", lookupVendor(probeList[i].mac), probeList[i].ssid);
  }
  display.display();
}

void enterExportReport() {
  Serial.println("===== RF MONITOR REPORT =====");
  Serial.printf("APs discovered: %d\n", apCount);
  for (int i = 0; i < apCount; i++) {
    ApEntry &e = apList[i];
    Serial.printf("- SSID:%s BSSID:%02X:%02X:%02X:%02X:%02X:%02X ch:%d rssi:%d enc:%s wps:%s %s\n",
      e.ssid, e.bssid[0], e.bssid[1], e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5],
      e.channel, e.rssi, e.enc, e.wps ? "yes" : "no", e.weak ? "**WEAK**" : "");
    for (int c = 0; c < e.clientCountAp; c++) {
      Serial.printf("    client: %02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
        e.clientMacsAp[c][0], e.clientMacsAp[c][1], e.clientMacsAp[c][2],
        e.clientMacsAp[c][3], e.clientMacsAp[c][4], e.clientMacsAp[c][5], lookupVendor(e.clientMacsAp[c]));
    }
  }
  Serial.printf("Probe requests logged: %d\n", probeCount);
  for (int i = 0; i < probeCount; i++) {
    Serial.printf("- %02X:%02X:%02X:%02X:%02X:%02X (%s) probed for \"%s\"\n",
      probeList[i].mac[0], probeList[i].mac[1], probeList[i].mac[2],
      probeList[i].mac[3], probeList[i].mac[4], probeList[i].mac[5],
      lookupVendor(probeList[i].mac), probeList[i].ssid);
  }
  Serial.println("===== END REPORT =====");
  currentScreen = SCR_EXPORT;
}

void drawExport() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 10);
  display.println("Report exported.");
  display.println("Check Serial Monitor");
  display.println("at 115200 baud.");
  display.display();
}

uint8_t reverseBits(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

uint8_t bleAdvAddress[4];

void enterBleScreen() {
  uint8_t rawAddr[4] = { 0xD6, 0xBE, 0x89, 0x8E };
  for (int i = 0; i < 4; i++) bleAdvAddress[i] = reverseBits(rawAddr[i]);

  radio.powerUp();
  delay(5);
  radio.setAutoAck(false);
  radio.setDataRate(RF24_1MBPS);
  radio.disableCRC();
  radio.setAddressWidth(4);
  radio.setPayloadSize(32);
  radio.openReadingPipe(0, bleAdvAddress);
  radio.setChannel(bleRawChannels[0]);
  bleChIdx = 0;
  radio.startListening();

  bleAdvCount = 0;
  bleLastName[0] = '\0';
  memset(bleLastAdvMac, 0, sizeof(bleLastAdvMac));

  bleModeActive = true;
  currentScreen = SCR_BLE;
}

void stopBleSniff() {
  radio.stopListening();
  radio.powerDown();
  bleModeActive = false;
}

void bleDewhiten(uint8_t* data, uint8_t len, uint8_t channel) {
  uint8_t lfsr = channel | 0x40;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t out = 0;
    for (uint8_t bit = 0; bit < 8; bit++) {
      uint8_t lfsrBit = lfsr & 1;
      if (lfsrBit) { lfsr ^= 0x88; out |= (1 << bit); }
      lfsr >>= 1;
    }
    data[i] ^= out;
  }
}

void runBleSniff() {
  if (!bleModeActive) return;
  unsigned long now = millis();

  if (radio.available()) {
    uint8_t raw[32] = {0};
    radio.read(raw, 32);
    uint8_t pkt[32];
    for (int i = 0; i < 32; i++) pkt[i] = reverseBits(raw[i]);
    bleDewhiten(pkt, 32, bleChannels[bleChIdx]);

    uint8_t pduLen = pkt[1] & 0x3F;
    if (pduLen >= 6 && pduLen <= 31) {
      bleAdvCount++;
      for (int i = 0; i < 6; i++) bleLastAdvMac[i] = pkt[7 - i];

      Serial.print("BLE adv seen, MAC: ");
      for (int i = 0; i < 6; i++) { Serial.printf("%02X", (uint8_t)bleLastAdvMac[i]); if (i < 5) Serial.print(":"); }
      Serial.println();

      int idx = 8;
      bleLastName[0] = '\0';
      while (idx < pduLen + 2 && idx < 30) {
        uint8_t adLen = pkt[idx];
        if (adLen == 0 || idx + adLen >= 32) break;
        uint8_t adType = pkt[idx + 1];
        if (adType == 0x08 || adType == 0x09) {
          int nameLen = adLen - 1;
          if (nameLen > 18) nameLen = 18;
          memcpy(bleLastName, &pkt[idx + 2], nameLen);
          bleLastName[nameLen] = '\0';
          break;
        }
        idx += adLen + 1;
      }
    }
  }

  if (now - lastBleHop > BLE_HOP_MS) {
    bleChIdx = (bleChIdx + 1) % 3;
    radio.stopListening();
    radio.setChannel(bleRawChannels[bleChIdx]);
    radio.startListening();
    lastBleHop = now;
  }
}

void drawBle() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 250) return;
  lastDraw = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.printf("BLE ch:%d pkts:%lu\n", bleChannels[bleChIdx], (unsigned long)bleAdvCount);
  display.print("MAC: ");
  for (int i = 0; i < 6; i++) { display.printf("%02X", (uint8_t)bleLastAdvMac[i]); if (i < 5) display.print(":"); }
  display.println();
  display.print("name: ");
  display.println(strlen(bleLastName) ? bleLastName : "-");
  display.display();
}

void drawBootScreen() {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 16);
  display.println("Sniffer");
  display.setTextSize(1);
  display.setCursor(16, 42);
  display.println("By Moksh Sharma");
  display.drawFastHLine(8, 56, 112, SH110X_WHITE);
  display.display();
}

void drawHomeForce() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  bool sw = digitalRead(JOY_SW);
  display.printf("RF Tool  SW:%s\n", sw == LOW ? "DOWN" : "up");
  for (int i = 0; i < homeItemCount; i++) {
    display.setCursor(4, 10 + i * 9);
    display.print(i == homeCursor ? "> " : "  ");
    display.println(homeItems[i]);
  }
  display.display();
}

void drawHome() {
  static int lastCursor = -1;
  static unsigned long lastDraw = 0;
  // redraw now and then so SW status updates
  if (lastCursor == homeCursor && millis() - lastDraw < 150) return;
  lastCursor = homeCursor;
  lastDraw = millis();
  drawHomeForce();
}

void drawJoyDebug() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();

  int x, y;
  readJoyFiltered(x, y);
  bool sw = digitalRead(JOY_SW);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("Joystick Debug");
  display.printf("VRX:%4d (ctr%4d)\n", x, joyCenterX);
  display.printf("VRY:%4d (ctr%4d)\n", y, joyCenterY);
  display.printf("SW: %s\n", sw == LOW ? "PRESSED" : "released");
  display.println();
  display.println("Push each direction,");
  display.println("note which value moves");
  display.display();
}

void drawAbout() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("About");
  display.println("ESP32-C3 WiFi/BLE");
  display.println("passive recon tool.");
  display.println("Authorized use only.");
  display.println("No deauth/injection.");
  display.display();
}
