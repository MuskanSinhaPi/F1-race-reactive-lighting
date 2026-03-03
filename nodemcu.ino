#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <time.h>

#define BUTTON_PIN  D3
#define NANO_RX     D7
#define NANO_TX     D8

SoftwareSerial nanoSerial(NANO_RX, NANO_TX);

// ================= WIFI =================
const char* ssid     = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

// ================= TIMING =================
unsigned long lastCheck     = 0;
unsigned long lastWifiCheck = 0;
const unsigned long interval  = 120000;  // 2 min API refresh
const unsigned long wifiRetry = 30000;

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

// ================= STATE =================
bool raceFinished = false;
bool raceSunday   = false;
bool lightsOutTriggered = false;

bool lastButtonState = HIGH;
unsigned long lastDebounce = 0;

int currentMode  = MODE_LIVE;
int currentRound = -1;
int lastSentTeam = -1;

char raceName[64] = "";
time_t raceStartEpoch = 0;

// ==========================================================
// MODE NAME
// ==========================================================

const char* getModeName(int mode) {
  switch (mode) {
    case MODE_DISPLAY:  return "Display Mode";
    case MODE_LIVE:     return "Live Mode";
    case MODE_FERRARI:  return "Ferrari";
    case MODE_ALPINE:   return "Alpine";
    case MODE_ASTON:    return "Aston Martin";
    case MODE_HAAS:     return "Haas";
    case MODE_AUDI:     return "Audi";
    case MODE_CADILLAC: return "Cadillac";
    case MODE_MCLAREN:  return "McLaren";
    case MODE_MERCEDES: return "Mercedes";
    case MODE_RBULLS:   return "Racing Bulls";
    case MODE_REDBULL:  return "Red Bull";
    case MODE_WILLIAMS: return "Williams";
    default: return "Unknown";
  }
}

// ==========================================================
// SETUP
// ==========================================================

void setup() {

  Serial.begin(115200);
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(10);

  Serial.println("\n==================================");
  Serial.println("   F1 LIVE LED CONTROLLER BOOT");
  Serial.println("==================================");

  connectWiFi();

  configTime(19800, 0, "pool.ntp.org");

  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  Serial.print("Current IST Time: ");
  Serial.println(ctime(&now));

  detectNextRace();

  int savedMode = EEPROM.read(1);
  if (savedMode < 0 || savedMode >= MODE_COUNT)
    savedMode = MODE_LIVE;

  currentMode = savedMode;

  Serial.print("Restored Mode: ");
  Serial.println(getModeName(currentMode));

  applyMode(currentMode);
}

// ==========================================================
// LOOP
// ==========================================================

void loop() {

  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();

  if (currentMode == MODE_LIVE && raceSunday && !raceFinished) {
    if (millis() - lastCheck > interval) {
      lastCheck = millis();
      fetchRaceData();
    }
  }

  delay(500);
}

// ==========================================================
// WIFI
// ==========================================================

void connectWiFi() {

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void maintainWiFi() {
  if (millis() - lastWifiCheck > wifiRetry) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi Lost - Reconnecting...");
      connectWiFi();
    }
  }
}

// ==========================================================
// SERIAL SEND
// ==========================================================

void sendToNano(int cmd) {
  nanoSerial.print(cmd);
  nanoSerial.print('\n');
  Serial.printf("→ Sent to Nano: %d\n", cmd);
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

      Serial.print("\nMode Changed → ");
      Serial.println(getModeName(currentMode));

      EEPROM.write(1, currentMode);
      EEPROM.commit();

      applyMode(currentMode);
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
    return;
  }

  if (mode == MODE_LIVE) {

    raceFinished = false;
    lastSentTeam = -1;

    int savedTeam = EEPROM.read(0);
    if (savedTeam < 1 || savedTeam > 11)
      savedTeam = TEAM_MCLAREN;

    sendToNano(savedTeam);

    if (raceSunday) {
      Serial.println("Live Mode: Race Sunday Active");
      sendToNano(CMD_PULSE);
      fetchRaceData();
    } else {
      Serial.println("Live Mode: No Race Today");
    }

    return;
  }

  // Manual team modes
  sendToNano(mode - 1);
}

// ==========================================================
// RACE DETECTION (SUNDAY ONLY)
// ==========================================================

void detectNextRace() {

  raceSunday = false;
  lightsOutTriggered = false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");

  if (https.GET() == 200) {

    StaticJsonDocument<2048> doc;
    deserializeJson(doc, https.getString());

    auto race = doc["MRData"]["RaceTable"]["Races"][0];

    const char* name  = race["raceName"];
    const char* date  = race["date"];
    const char* timeS = race["time"];
    const char* roundStr = race["round"];

    if (name) strncpy(raceName, name, sizeof(raceName));
    if (roundStr) currentRound = atoi(roundStr);

    if (date && timeS) {

      int ry, rm, rd, hh, mm, ss;
      sscanf(date, "%d-%d-%d", &ry, &rm, &rd);
      sscanf(timeS, "%d:%d:%d", &hh, &mm, &ss);

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

      if (nowTm->tm_wday == 0) { // Sunday only
        raceSunday = true;
      }

      Serial.println("\n===== Upcoming Race =====");
      Serial.printf("Race: %s\n", raceName);
      Serial.printf("Round: %d\n", currentRound);
      Serial.printf("Start Time: %s", ctime(&raceStartEpoch));
      Serial.println("=========================");
    }
  }

  https.end();
}

// ==========================================================
// LIGHTS OUT COUNTDOWN
// ==========================================================

void checkLightsOutCountdown() {

  if (!raceSunday || raceFinished || raceStartEpoch == 0)
    return;

  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);

  if (diff > 0) {

    int h = diff / 3600;
    int m = ((int)diff % 3600) / 60;
    int s = (int)diff % 60;

    Serial.printf("Lights Out In: %02d:%02d:%02d\n", h, m, s);

    if (diff <= 5 && !lightsOutTriggered) {
      Serial.println("LIGHTS OUT TRIGGERED");
      sendToNano(CMD_LIGHTS_OUT);
      lightsOutTriggered = true;
    }
  }
}

// ==========================================================
// FETCH RESULTS
// ==========================================================

void fetchRaceData() {

  Serial.println("\nFetching Live Results...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  if (https.GET() == 200) {

    StaticJsonDocument<2048> doc;
    deserializeJson(doc, https.getString());

    auto race = doc["MRData"]["RaceTable"]["Races"][0];

    const char* status =
      race["Results"][0]["status"];

    const char* team =
      race["Results"][0]["Constructor"]["name"];

    if (!status || !team) return;

    Serial.println("===== Race Update =====");
    Serial.printf("Race: %s\n", raceName);
    Serial.printf("P1 Constructor: %s\n", team);
    Serial.printf("Status: %s\n", status);
    Serial.println("=======================");

    int teamID = getTeamID(String(team));

    if (teamID != lastSentTeam) {
      sendToNano(teamID);
      sendToNano(CMD_PULSE);
      lastSentTeam = teamID;
    }

    if (String(status) == "Finished") {

      raceFinished = true;
      EEPROM.write(0, teamID);
      EEPROM.commit();

      Serial.println("Race Finished. Winner Saved.");

      sendToNano(CMD_CHECKERED);
      sendToNano(teamID);
    }
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
