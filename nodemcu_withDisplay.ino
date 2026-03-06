#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ================= TFT PINS =================
#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  D0

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// ================= SERIAL TO NANO =================
#define BUTTON_PIN D3
#define NANO_RX  D6
#define NANO_TX  D8
SoftwareSerial nanoSerial(NANO_RX, NANO_TX);

// ================= WIFI =================
const char* ssid     = "Muskan_2.4 G";
const char* password = "8527245177";

// ================= TIMING =================
unsigned long lastCheck     = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastClockUpdate = 0;
const unsigned long interval    = 120000;
const unsigned long wifiRetry   = 30000;
const unsigned long clockRefresh = 1000;

// ================= TEAM IDs =================
#define TEAM_FERRARI       1
#define TEAM_ALPINE        2
#define TEAM_ASTON         3
#define TEAM_HAAS          4
#define TEAM_AUDI          5
#define TEAM_CADILLAC      6
#define TEAM_MCLAREN       7
#define TEAM_MERCEDES      8
#define TEAM_RACINGBULLS   9
#define TEAM_REDBULL      10
#define TEAM_WILLIAMS     11

// ================= MODES =================
#define MODE_DISPLAY   0
#define MODE_LIVE      1
#define MODE_FERRARI   2
#define MODE_ALPINE    3
#define MODE_ASTON     4
#define MODE_HAAS      5
#define MODE_AUDI      6
#define MODE_CADILLAC  7
#define MODE_MCLAREN   8
#define MODE_MERCEDES  9
#define MODE_RBULLS   10
#define MODE_REDBULL  11
#define MODE_WILLIAMS 12
#define MODE_COUNT    13

// ================= NANO COMMANDS =================
#define CMD_DISPLAY      0
#define CMD_PULSE      255
#define CMD_CHECKERED   99
#define CMD_LIGHTS_OUT  77

// ================= TEAM COLORS (RGB565 for TFT) =================
uint16_t teamTFTColor[] = {
  ST7735_BLACK,                        // 0 unused
  tft.color565(220,  20,  20),         // 1  Ferrari
  tft.color565( 20,  40, 150),         // 2  Alpine
  tft.color565(  0, 130,  90),         // 3  Aston
  tft.color565(220,   0,  45),         // 4  Haas
  tft.color565(180,   0,   0),         // 5  Audi
  tft.color565(180, 180, 180),         // 6  Cadillac
  tft.color565(255, 120,   0),         // 7  McLaren
  tft.color565(  0, 210, 190),         // 8  Mercedes
  tft.color565( 40,  60, 200),         // 9  Racing Bulls
  tft.color565( 10,  60, 200),         // 10 Red Bull
  tft.color565( 20,  40, 180),         // 11 Williams
};

const char* teamNames[] = {
  "", "FERRARI", "ALPINE", "ASTON MR",
  "HAAS", "AUDI", "CADILLAC", "McLAREN",
  "MERCEDES", "RB", "RED BULL", "WILLIAMS"
};

// ================= STATE =================
bool raceFinished        = false;
bool raceSunday          = false;
bool lightsOutTriggered  = false;
bool lastButtonState     = HIGH;
unsigned long lastDebounce = 0;

int  currentMode   = MODE_LIVE;
int  currentRound  = -1;
int  lastSentTeam  = -1;
int  currentTeamID = TEAM_MCLAREN;

char raceName[64]      = "---";
char p1Team[32]        = "---";
char p1Status[16]      = "---";
time_t raceStartEpoch  = 0;

// ================= TFT COLORS =================
#define C_BG        tft.color565(8, 8, 12)
#define C_PANEL     tft.color565(18, 18, 28)
#define C_BORDER    tft.color565(40, 40, 60)
#define C_WHITE     ST7735_WHITE
#define C_GREY      tft.color565(120, 120, 140)
#define C_RED       tft.color565(220, 20, 20)
#define C_GREEN     tft.color565(0, 200, 80)
#define C_YELLOW    tft.color565(255, 200, 0)
#define C_DIM       tft.color565(60, 60, 80)

// ==========================================================
// SETUP
// ==========================================================

void setup() {
  Serial.begin(115200);
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(10);

  // Init TFT
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // landscape — 160×128
  drawBootScreen();

  connectWiFi();
  tft.fillScreen(C_BG);
  drawStatusLine("WiFi OK", C_GREEN);

  configTime(19800, 0, "pool.ntp.org");
  drawStatusLine("Syncing time...", C_YELLOW);

  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    now = time(nullptr);
  }
  drawStatusLine("Time synced", C_GREEN);
  delay(400);

  detectNextRace();

  int savedMode = EEPROM.read(1);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;

  int savedTeam = EEPROM.read(0);
  if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

  drawMainScreen();
  applyMode(currentMode);
}

// ==========================================================
// LOOP
// ==========================================================

void loop() {
  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();

  // Update clock every second
  if (millis() - lastClockUpdate > clockRefresh) {
    lastClockUpdate = millis();
    updateClock();
    if (raceSunday && !raceFinished && raceStartEpoch > 0)
      updateCountdown();
  }

  if (currentMode == MODE_LIVE && raceSunday && !raceFinished) {
    if (millis() - lastCheck > interval) {
      lastCheck = millis();
      fetchRaceData();
    }
  }

  delay(50);
}

// ==========================================================
// DRAW BOOT SCREEN
// ==========================================================

void drawBootScreen() {
  tft.fillScreen(ST7735_BLACK);

  // Top red bar
  tft.fillRect(0, 0, 160, 22, C_RED);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(2);
  tft.setCursor(18, 5);
  tft.print("F1  LIVE");

  // Bottom bar
  tft.fillRect(0, 106, 160, 22, tft.color565(20, 20, 20));
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(32, 112);
  tft.print("LED CONTROLLER");

  // Center logo area
  tft.fillRect(60, 30, 40, 40, tft.color565(255, 120, 0)); // orange block
  tft.fillRect(65, 35, 30, 30, ST7735_BLACK);
  tft.setTextColor(tft.color565(255, 120, 0));
  tft.setTextSize(2);
  tft.setCursor(68, 40);
  tft.print("F1");

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(38, 82);
  tft.print("Booting...");
}

void drawStatusLine(const char* msg, uint16_t color) {
  tft.fillRect(0, 82, 160, 12, ST7735_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(1);
  int x = (160 - strlen(msg) * 6) / 2;
  tft.setCursor(x, 84);
  tft.print(msg);
  delay(300);
}

// ==========================================================
// DRAW MAIN SCREEN LAYOUT
// ==========================================================

void drawMainScreen() {
  tft.fillScreen(C_BG);

  // Top header bar
  tft.fillRect(0, 0, 160, 18, C_RED);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("F1 LIVE");
  // clock area placeholder right side of header
  tft.setCursor(100, 5);
  tft.print("--:--:--");

  // Divider
  tft.drawFastHLine(0, 18, 160, C_BORDER);

  // Race name row
  tft.fillRect(0, 19, 160, 14, C_PANEL);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 24);
  tft.print(raceName);

  // P1 team color stripe (left accent)
  tft.fillRect(0, 33, 4, 40, teamTFTColor[currentTeamID]);

  // P1 section
  tft.fillRect(4, 33, 156, 40, C_PANEL);
  tft.drawRect(4, 33, 156, 40, C_BORDER);

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(8, 37);
  tft.print("P1 CONSTRUCTOR");

  tft.setTextColor(teamTFTColor[currentTeamID]);
  tft.setTextSize(2);
  tft.setCursor(8, 50);
  tft.print(teamNames[currentTeamID]);

  // Status row
  tft.fillRect(0, 73, 160, 14, C_PANEL);
  tft.drawFastHLine(0, 73, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 78);
  tft.print("STATUS: ");
  tft.setTextColor(C_GREEN);
  tft.print(p1Status);

  // Mode row
  tft.fillRect(0, 87, 160, 14, C_PANEL);
  tft.drawFastHLine(0, 87, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 92);
  tft.print("MODE: ");
  tft.setTextColor(C_WHITE);
  tft.print(getModeName(currentMode));

  // Countdown row
  tft.fillRect(0, 101, 160, 14, C_PANEL);
  tft.drawFastHLine(0, 101, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 106);
  if (raceSunday && !raceFinished)
    tft.print("LIGHTS OUT: --:--:--");
  else if (raceFinished)
    tft.print("RACE COMPLETE");
  else
    tft.print("NO RACE TODAY");

  // Bottom border
  tft.drawFastHLine(0, 115, 160, C_BORDER);
  tft.fillRect(0, 116, 160, 12, tft.color565(12, 12, 18));
  tft.setTextColor(C_DIM);
  tft.setCursor(4, 119);
  tft.print("Round ");
  tft.print(currentRound > 0 ? currentRound : 0);
  tft.setCursor(90, 119);
  tft.print("jolpi.ca API");
}

// ==========================================================
// UPDATE CLOCK (header right side)
// ==========================================================

void updateClock() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

  tft.fillRect(96, 2, 62, 14, C_RED);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(98, 5);
  tft.print(buf);
}

// ==========================================================
// UPDATE COUNTDOWN
// ==========================================================

void updateCountdown() {
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);

  tft.fillRect(4, 103, 155, 11, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(4, 106);

  if (diff > 0) {
    int h = (int)diff / 3600;
    int m = ((int)diff % 3600) / 60;
    int s = (int)diff % 60;
    char buf[22];
    sprintf(buf, "LIGHTS OUT: %02d:%02d:%02d", h, m, s);
    tft.setTextColor(diff < 300 ? C_RED : C_YELLOW);
    tft.print(buf);
  } else {
    tft.setTextColor(C_GREEN);
    tft.print("RACE IN PROGRESS");
  }
}

// ==========================================================
// UPDATE P1 TEAM DISPLAY
// ==========================================================

void updateTeamDisplay(int teamID) {
  currentTeamID = teamID;

  // Color stripe
  tft.fillRect(0, 33, 4, 40, teamTFTColor[teamID]);

  // Panel
  tft.fillRect(4, 33, 156, 40, C_PANEL);
  tft.drawRect(4, 33, 156, 40, C_BORDER);

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(8, 37);
  tft.print("P1 CONSTRUCTOR");

  tft.setTextColor(teamTFTColor[teamID]);
  tft.setTextSize(2);
  tft.setCursor(8, 50);
  tft.print(teamNames[teamID]);
}

// ==========================================================
// UPDATE STATUS
// ==========================================================

void updateStatus(const char* status, uint16_t color) {
  tft.fillRect(52, 75, 105, 11, C_PANEL);
  tft.setTextColor(color);
  tft.setTextSize(1);
  tft.setCursor(52, 78);
  tft.print(status);
}

// ==========================================================
// UPDATE MODE DISPLAY
// ==========================================================

void updateModeDisplay() {
  tft.fillRect(40, 89, 118, 11, C_PANEL);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(40, 92);
  tft.print(getModeName(currentMode));
}

// ==========================================================
// RACE FINISHED OVERLAY
// ==========================================================

void drawRaceFinished(int teamID) {
  // Flash checkered header
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, 0, 160, 18, ST7735_WHITE);
    delay(200);
    tft.fillRect(0, 0, 160, 18, ST7735_BLACK);
    delay(200);
  }
  tft.fillRect(0, 0, 160, 18, C_RED);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("RACE FINISHED!");
  updateClock();

  // Winner banner
  tft.fillRect(0, 33, 160, 40, teamTFTColor[teamID]);
  tft.setTextColor(ST7735_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 37);
  tft.print("WINNER");
  tft.setTextSize(2);
  tft.setCursor(8, 48);
  tft.print(teamNames[teamID]);

  tft.fillRect(0, 101, 160, 14, C_PANEL);
  tft.setTextColor(C_GREEN);
  tft.setTextSize(1);
  tft.setCursor(4, 106);
  tft.print("RACE COMPLETE");
}

// ==========================================================
// WIFI
// ==========================================================

void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected");
}

void maintainWiFi() {
  if (millis() - lastWifiCheck > wifiRetry) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      updateStatus("WiFi Lost", C_RED);
      connectWiFi();
      updateStatus("WiFi OK", C_GREEN);
    }
  }
}

// ==========================================================
// SEND TO NANO
// ==========================================================

void sendToNano(int cmd) {
  nanoSerial.print(cmd);
  nanoSerial.print('\n');
  Serial.printf("→ Nano: %d\n", cmd);
}

// ==========================================================
// BUTTON
// ==========================================================

void checkButton() {
  bool state = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && state == LOW) {
    if (millis() - lastDebounce > 300) {
      lastDebounce = millis();
      currentMode = (currentMode + 1) % MODE_COUNT;
      Serial.printf("Mode → %s\n", getModeName(currentMode));
      EEPROM.write(1, currentMode);
      EEPROM.commit();
      applyMode(currentMode);
      updateModeDisplay();
    }
  }
  lastButtonState = state;
}

// ==========================================================
// APPLY MODE
// ==========================================================

void applyMode(int mode) {
  if (mode == MODE_DISPLAY) {
    sendToNano(CMD_DISPLAY);
    updateTeamDisplay(TEAM_MCLAREN); // neutral
    updateStatus("DISPLAY", C_GREY);
    return;
  }

  if (mode == MODE_LIVE) {
    raceFinished  = false;
    lastSentTeam  = -1;

    int savedTeam = EEPROM.read(0);
    if (savedTeam < 1 || savedTeam > 11) savedTeam = TEAM_MCLAREN;

    sendToNano(savedTeam);
    updateTeamDisplay(savedTeam);

    if (raceSunday) {
      sendToNano(CMD_PULSE);
      updateStatus("LIVE", C_GREEN);
      fetchRaceData();
    } else {
      updateStatus("NO RACE", C_GREY);
    }
    return;
  }

  // Manual team modes — mode 2 = team 1, mode 3 = team 2 etc
  int teamID = mode - 1;
  sendToNano(teamID);
  updateTeamDisplay(teamID);
  updateStatus(teamNames[teamID], teamTFTColor[teamID]);
}

// ==========================================================
// MODE NAME
// ==========================================================

const char* getModeName(int mode) {
  switch (mode) {
    case MODE_DISPLAY:  return "DISPLAY";
    case MODE_LIVE:     return "LIVE";
    case MODE_FERRARI:  return "FERRARI";
    case MODE_ALPINE:   return "ALPINE";
    case MODE_ASTON:    return "ASTON MARTIN";
    case MODE_HAAS:     return "HAAS";
    case MODE_AUDI:     return "AUDI";
    case MODE_CADILLAC: return "CADILLAC";
    case MODE_MCLAREN:  return "McLAREN";
    case MODE_MERCEDES: return "MERCEDES";
    case MODE_RBULLS:   return "RACING BULLS";
    case MODE_REDBULL:  return "RED BULL";
    case MODE_WILLIAMS: return "WILLIAMS";
    default: return "---";
  }
}

// ==========================================================
// RACE DETECTION
// ==========================================================

void detectNextRace() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");

  if (https.GET() == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, https.getString());

    auto race = doc["MRData"]["RaceTable"]["Races"][0];

    const char* name     = race["raceName"];
    const char* date     = race["date"];
    const char* timeS    = race["time"];
    const char* roundStr = race["round"];

    if (name)     strncpy(raceName, name, sizeof(raceName) - 1);
    if (roundStr) currentRound = atoi(roundStr);

    if (date && timeS) {
      int ry, rm, rd, hh, mm, ss;
      sscanf(date,  "%d-%d-%d", &ry, &rm, &rd);
      sscanf(timeS, "%d:%d:%dZ", &hh, &mm, &ss);

      struct tm raceTm = {};
      raceTm.tm_year = ry - 1900;
      raceTm.tm_mon  = rm - 1;
      raceTm.tm_mday = rd;
      raceTm.tm_hour = hh;
      raceTm.tm_min  = mm;
      raceTm.tm_sec  = ss;
      raceStartEpoch = mktime(&raceTm);

      time_t now = time(nullptr);
      struct tm* nowTm = localtime(&now);
      if (nowTm->tm_wday == 0) raceSunday = true;

      Serial.printf("Race: %s | Round: %d\n", raceName, currentRound);
      Serial.printf("Race Sunday: %s\n", raceSunday ? "YES" : "NO");
    }
  }
  https.end();
}

// ==========================================================
// LIGHTS OUT COUNTDOWN
// ==========================================================

void checkLightsOutCountdown() {
  if (!raceSunday || raceFinished || raceStartEpoch == 0) return;

  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);

  if (diff <= 5 && diff > 0 && !lightsOutTriggered) {
    Serial.println("LIGHTS OUT!");
    sendToNano(CMD_LIGHTS_OUT);
    lightsOutTriggered = true;

    // Flash screen red
    for (int i = 0; i < 5; i++) {
      tft.fillScreen(C_RED);
      delay(150);
      tft.fillScreen(ST7735_BLACK);
      delay(150);
    }
    drawMainScreen();
  }
}

// ==========================================================
// FETCH RACE DATA
// ==========================================================

void fetchRaceData() {
  Serial.println("Fetching results...");
  updateStatus("FETCHING", C_YELLOW);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  if (https.GET() == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, https.getString());

    auto race    = doc["MRData"]["RaceTable"]["Races"][0];
    const char* status = race["Results"][0]["status"];
    const char* team   = race["Results"][0]["Constructor"]["name"];

    if (!status || !team) {
      updateStatus("NO DATA", C_GREY);
      https.end();
      return;
    }

    strncpy(p1Team,   team,   sizeof(p1Team)   - 1);
    strncpy(p1Status, status, sizeof(p1Status) - 1);

    int teamID = getTeamID(String(team));

    Serial.printf("P1: %s | %s\n", team, status);

    if (teamID != lastSentTeam) {
      sendToNano(teamID);
      sendToNano(CMD_PULSE);
      lastSentTeam = teamID;
      updateTeamDisplay(teamID);
    }

    updateStatus("LIVE", C_GREEN);

    if (String(status) == "Finished") {
      raceFinished = true;
      EEPROM.write(0, teamID);
      EEPROM.commit();

      sendToNano(CMD_CHECKERED);
      sendToNano(teamID);

      drawRaceFinished(teamID);
    }
  } else {
    updateStatus("API ERR", C_RED);
  }

  https.end();
}

// ==========================================================
// TEAM IDENTIFICATION
// ==========================================================

int getTeamID(String team) {
  if (team.indexOf("Ferrari")      >= 0) return TEAM_FERRARI;
  if (team.indexOf("Alpine")       >= 0) return TEAM_ALPINE;
  if (team.indexOf("Aston")        >= 0) return TEAM_ASTON;
  if (team.indexOf("Haas")         >= 0) return TEAM_HAAS;
  if (team.indexOf("Audi")         >= 0) return TEAM_AUDI;
  if (team.indexOf("Cadillac")     >= 0) return TEAM_CADILLAC;
  if (team.indexOf("McLaren")      >= 0) return TEAM_MCLAREN;
  if (team.indexOf("Mercedes")     >= 0) return TEAM_MERCEDES;
  if (team.indexOf("Racing Bulls") >= 0) return TEAM_RACINGBULLS;
  if (team.indexOf("Red Bull")     >= 0) return TEAM_REDBULL;
  if (team.indexOf("Williams")     >= 0) return TEAM_WILLIAMS;
  return TEAM_MCLAREN;
}
