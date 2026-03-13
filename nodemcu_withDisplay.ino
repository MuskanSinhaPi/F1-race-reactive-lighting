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
#define TFT_RST  -1

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// ================= SERIAL TO NANO =================
#define BUTTON_PIN D3
#define NANO_RX    D6
#define NANO_TX    D8
SoftwareSerial nanoSerial(NANO_RX, NANO_TX);

// ================= WIFI =================
const char* ssid     = "YOUR WIFI SSID";
const char* password = "YOUR WIFI PASSWORD";

// ================= TIMING =================
unsigned long lastCheck       = 0;
unsigned long lastWifiCheck   = 0;
unsigned long lastClockUpdate = 0;
const unsigned long interval     = 120000;
const unsigned long wifiRetry    = 30000;
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

// ================= EEPROM SLOTS =================
#define EEPROM_LAST_RACE    0
#define EEPROM_SEASON_CHAMP 2
#define EEPROM_LAST_GP_NAME 3 

// ================= TFT COLOR CONSTANTS =================
#define C_BG        0x0000
#define C_PANEL     0x0841
#define C_BORDER    0x2945
#define C_WHITE     0xFFFF
#define C_GREY      0x8C51
#define C_RED       0xF800
#define C_GREEN     0x07E0
#define C_YELLOW    0xFFE0
#define C_DIM       0x4A49
#define C_BLACK     0x0000
#define C_ORANGE    0xFB00
#define C_TEAL      0x0697

// ================= TEAM TFT COLORS =================
uint16_t teamTFTColor[] = {
  C_WHITE,   // 0 unused
  0xD800,    // 1  Ferrari
  0x1252,    // 2  Alpine
  0x0494,    // 3  Aston
  0xD805,    // 4  Haas
  0xB000,    // 5  Audi
  0xBDD7,    // 6  Cadillac
  0xFB00,    // 7  McLaren
  0x0697,    // 8  Mercedes
  0x2832,    // 9  Racing Bulls
  0x0A32,    // 10 Red Bull
  0x1496,    // 11 Williams
};

const char* teamNames[] = {
  "", "FERRARI", "ALPINE", "ASTON MARTIN",
  "HAAS", "AUDI", "CADILLAC", "McLAREN",
  "MERCEDES", "RACING BULLS", "RED BULL", "WILLIAMS"
};

// ================= STATE =================
bool raceFinished       = false;
bool raceSunday         = false;
bool lightsOutTriggered = false;
bool lastButtonState    = HIGH;
bool nanoReady          = false;
unsigned long lastDebounce = 0;

int  currentMode   = MODE_LIVE;
int  currentRound  = -1;
int  totalRounds   = 24;   // fallback, updated from API
int  lastSentTeam  = -1;
int  currentTeamID = TEAM_MCLAREN;

char raceName[64]   = "---";
char lastGPName[64] = "Australian";
char p1Team[32]     = "---";
char p1Status[16]   = "---";
time_t raceStartEpoch = 0;

// ================= FORWARD DECLARATIONS =================
void fetchRaceData();
void applyMode(int mode);
void updateTeamDisplay(int teamID, bool isLive);
void updateStatus(const char* status, uint16_t color);
void updateModeDisplay();
void sendToNano(int cmd);
void checkButton();
void connectWiFi();
void maintainWiFi();
void detectNextRace();
void checkLightsOutCountdown();
void drawBootScreen();
void drawStatusLine(const char* msg, uint16_t color);
void drawMainScreen();
void drawRaceFinished(int teamID);
void updateClock();
void updateCountdown();
const char* getModeName(int mode);
String getTodayDate();
int getTeamID(String team);

// ==========================================================
// SETUP
// ==========================================================

void setup() {
  Serial.begin(115200);
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(32);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawBootScreen();

  connectWiFi();
  drawStatusLine("WiFi OK", C_GREEN);

  configTime(19800, 0, "pool.ntp.org");
  drawStatusLine("Syncing time...", C_YELLOW);

  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    now = time(nullptr);
  }
  drawStatusLine("Time synced", C_GREEN);
  delay(300);

  detectNextRace();
  drawStatusLine("Race data OK", C_GREEN);
  delay(300);

  drawStatusLine("Waiting Nano...", C_YELLOW);
  delay(2000);
  nanoReady = true;

  int savedMode = EEPROM.read(1);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;

  int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
  if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

  char savedGP[21] = {};
  for (int i = 0; i < 20; i++)
    savedGP[i] = EEPROM.read(EEPROM_LAST_GP_NAME + i);
  savedGP[20] = '\0';
  if (savedGP[0] >= 'A' && savedGP[0] <= 'z')  // basic validity check
    strncpy(lastGPName, savedGP, sizeof(lastGPName) - 1);
  
  drawMainScreen();

  if (raceSunday && currentMode == MODE_LIVE) {
    fetchRaceData();
  } else {
    applyMode(currentMode);
  }
}

// ==========================================================
// LOOP
// ==========================================================

void loop() {
  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();

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
// BOOT SCREEN
// ==========================================================

void drawBootScreen() {
  tft.fillScreen(C_BLACK);

  tft.fillRect(0, 0, 160, 22, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(2);
  tft.setCursor(18, 5);
  tft.print("F1  LIVE");

  tft.fillRect(0, 106, 160, 22, C_PANEL);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(32, 112);
  tft.print("LED CONTROLLER");

  tft.fillRect(60, 30, 40, 40, C_ORANGE);
  tft.fillRect(65, 35, 30, 30, C_BLACK);
  tft.setTextColor(C_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(68, 40);
  tft.print("F1");

  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(38, 82);
  tft.print("Booting...");
}

void drawStatusLine(const char* msg, uint16_t color) {
  tft.fillRect(0, 82, 160, 12, C_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(1);
  int x = (160 - strlen(msg) * 6) / 2;
  tft.setCursor(x, 84);
  tft.print(msg);
  delay(300);
}

// ==========================================================
// MAIN SCREEN
// ==========================================================

void drawMainScreen() {
  tft.fillScreen(C_BLACK);

  // Header
  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print("F1 LIVE");
  tft.setCursor(100, 4);
  tft.print("--:--:--");

  // Next GP row
  tft.fillRect(0, 16, 160, 12, C_PANEL);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 20);
  tft.print("Next GP: ");
  tft.setTextColor(C_WHITE);
  char shortName[20];
  strncpy(shortName, raceName, 19);
  shortName[19] = '\0';
  tft.print(shortName);

  tft.drawFastHLine(0, 28, 160, C_BORDER);

  // P1 panel
  tft.fillRect(0, 29, 4, 44, teamTFTColor[currentTeamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY)
    tft.print("LAST SEASON P1");
  else if (currentMode == MODE_LIVE)
    tft.print("Last GP:");
  else
    tft.print("CONSTRUCTOR");
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[currentTeamID]);
  tft.setTextSize(2);
  tft.setCursor(8, 45);
  tft.print(teamNames[currentTeamID]);

  // Status row
  tft.fillRect(0, 73, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 73, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 77);
  tft.print("STATUS:");
  tft.setTextColor(C_GREEN);
  tft.setCursor(52, 77);
  tft.print(p1Status);

  // Mode row
  tft.fillRect(0, 86, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 86, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 90);
  tft.print("MODE:");
  tft.setTextColor(C_WHITE);
  tft.setCursor(40, 90);
  tft.print(getModeName(currentMode));

  // Countdown row
  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 99, 160, C_BORDER);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 103);
  if (raceFinished)
    tft.print("RACE COMPLETE");
  else if (raceSunday)
    tft.print("LIGHTS OUT: --:--:--");
  else
    tft.print("NO RACE TODAY");

  // Footer
  tft.drawFastHLine(0, 112, 160, C_BORDER);
  tft.fillRect(0, 113, 160, 15, C_PANEL);
  tft.setTextColor(C_DIM);
  tft.setCursor(4, 117);
  tft.print("Rnd ");
  tft.print(currentRound > 0 ? currentRound : 0);
  tft.setCursor(52, 117);
  tft.print("jolpi.ca API");
}

// ==========================================================
// CLOCK
// ==========================================================

void updateClock() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  tft.fillRect(96, 1, 63, 14, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(98, 4);
  tft.print(buf);
}

// ==========================================================
// COUNTDOWN
// ==========================================================

void updateCountdown() {
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);
  tft.fillRect(4, 100, 155, 11, C_PANEL);
  tft.setTextSize(1);
  tft.setCursor(4, 103);
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
// UPDATE TEAM DISPLAY
// ==========================================================

void updateTeamDisplay(int teamID, bool isLive) {
  currentTeamID = teamID;

  tft.fillRect(0, 29, 4, 44, teamTFTColor[teamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);

  tft.fillRect(8, 29, 148, 14, C_PANEL);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(8, 33);

  if (currentMode == MODE_DISPLAY) {
    tft.print("LAST SEASON P1");
  } else if (isLive) {
    char label[27];
    char gpShort[14];
    strncpy(gpShort, lastGPName, 13);
    gpShort[13] = '\0';
    char* gp = strstr(gpShort, " Grand");
    if (gp) *gp = '\0';
    snprintf(label, sizeof(label), "Last GP: %s | P1:", gpShort);
    tft.print(label);
  } else {
    tft.print("CONSTRUCTOR");
  }

  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[teamID]);
  tft.setTextSize(2);
  tft.setCursor(8, 45);
  tft.print(teamNames[teamID]);
}

// ==========================================================
// UPDATE STATUS
// ==========================================================

void updateStatus(const char* status, uint16_t color) {
  tft.fillRect(52, 74, 106, 11, C_PANEL);
  tft.setTextColor(color);
  tft.setTextSize(1);
  tft.setCursor(52, 77);
  tft.print(status);
}

// ==========================================================
// UPDATE MODE DISPLAY
// ==========================================================

void updateModeDisplay() {
  tft.fillRect(40, 87, 118, 11, C_PANEL);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(40, 90);
  tft.print(getModeName(currentMode));
}

// ==========================================================
// RACE FINISHED SCREEN
// ==========================================================

void drawRaceFinished(int teamID) {
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, 0, 160, 16, C_WHITE);
    delay(200);
    tft.fillRect(0, 0, 160, 16, C_BLACK);
    delay(200);
  }
  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print("RACE FINISHED!");
  updateClock();

  tft.fillRect(0, 29, 160, 44, teamTFTColor[teamID]);
  tft.setTextColor(C_BLACK);
  tft.setTextSize(1);
  tft.setCursor(8, 33);
  tft.print("WINNER");
  tft.setTextSize(2);
  tft.setCursor(8, 45);
  tft.print(teamNames[teamID]);

  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.setTextColor(C_GREEN);
  tft.setTextSize(1);
  tft.setCursor(4, 103);
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
  if (!nanoReady) return;
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
    int seasonChamp = EEPROM.read(EEPROM_SEASON_CHAMP);
    if (seasonChamp < 1 || seasonChamp > 11) seasonChamp = TEAM_MCLAREN;
    updateTeamDisplay(seasonChamp, false);
    updateStatus("DISPLAY", C_GREY);
    updateModeDisplay();
    return;
  }

  if (mode == MODE_LIVE) {
    raceFinished = false;
    lastSentTeam = -1;

    int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
    if (savedTeam < 1 || savedTeam > 11) savedTeam = TEAM_MCLAREN;
    currentTeamID = savedTeam;

    updateTeamDisplay(savedTeam, true);
    updateModeDisplay();

    if (raceSunday) {
      updateStatus("FETCHING", C_YELLOW);
      fetchRaceData();
    } else {
      sendToNano(savedTeam);
      updateStatus("NO RACE", C_GREY);
    }
    return;
  }

  // Manual team modes
  int teamID = mode - 1;
  sendToNano(teamID);
  updateTeamDisplay(teamID, false);
  updateStatus(teamNames[teamID], teamTFTColor[teamID]);
  updateModeDisplay();
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
    case MODE_ASTON:    return "ASTON MR";
    case MODE_HAAS:     return "HAAS";
    case MODE_AUDI:     return "AUDI";
    case MODE_CADILLAC: return "CADILLAC";
    case MODE_MCLAREN:  return "McLAREN";
    case MODE_MERCEDES: return "MERCEDES";
    case MODE_RBULLS:   return "RB";
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

    // Total rounds this season — used to detect final race
    const char* totalStr = doc["MRData"]["total"];
    if (totalStr) totalRounds = atoi(totalStr);
    Serial.printf("Total rounds this season: %d\n", totalRounds);

    auto race        = doc["MRData"]["RaceTable"]["Races"][0];
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
      raceStartEpoch = mktime(&raceTm) - 19800;
      
      time_t now = time(nullptr);
      double diff = difftime(raceStartEpoch, now);
      if (diff >= 0 && diff <= 86400) raceSunday = true;

      Serial.printf("Race: %s | Round: %d/%d | Sunday: %s\n",
                    raceName, currentRound, totalRounds,
                    raceSunday ? "YES" : "NO");
    }
  }
  https.end();
}

// ==========================================================
// LIGHTS OUT
// ==========================================================

void checkLightsOutCountdown() {
  if (!raceSunday || raceFinished || raceStartEpoch == 0) return;

  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);

  if (diff <= 5 && diff > 0 && !lightsOutTriggered) {
    Serial.println("LIGHTS OUT!");
    sendToNano(CMD_LIGHTS_OUT);
    lightsOutTriggered = true;

    for (int i = 0; i < 5; i++) {
      tft.fillScreen(C_RED);
      delay(150);
      tft.fillScreen(C_BLACK);
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

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  if (https.GET() == 200) {

    StaticJsonDocument<200> filter;
    filter["MRData"]["RaceTable"]["Races"][0]["raceName"]                          = true;
    filter["MRData"]["RaceTable"]["Races"][0]["date"]                              = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"]              = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(
      doc, https.getStream(),
      DeserializationOption::Filter(filter)
    );

    if (err) {
      Serial.printf("JSON error: %s\n", err.c_str());
      updateStatus("JSON ERR", C_RED);
      https.end();
      return;
    }

    auto race              = doc["MRData"]["RaceTable"]["Races"][0];
    const char* resultDate = race["date"];
    const char* gpName     = race["raceName"];
    const char* status     = race["Results"][0]["status"];
    const char* team       = race["Results"][0]["Constructor"]["name"];

    if (!status || !team || !resultDate) {
      Serial.println("Missing fields in response");
      updateStatus("NO DATA", C_GREY);
      https.end();
      return;
    }

    // Date guard
    String today = getTodayDate();
    if (String(resultDate) != today) {
      Serial.printf("Results from %s, today %s — skipping\n",
                    resultDate, today.c_str());
      updateStatus("NO RACE", C_GREY);
      https.end();
      return;
    }

    if (gpName) strncpy(lastGPName, gpName, sizeof(lastGPName) - 1);
    strncpy(p1Team,   team,   sizeof(p1Team)   - 1);
    strncpy(p1Status, status, sizeof(p1Status) - 1);

    int teamID = getTeamID(String(team));
    Serial.printf("P1: %s | %s\n", team, status);

    updateTeamDisplay(teamID, true);

    if (teamID != lastSentTeam && String(status) != "Finished") {
      sendToNano(teamID);
      sendToNano(CMD_PULSE);
      lastSentTeam = teamID;
    }

    if (String(status) == "Finished") {
      raceFinished = true;

      // Save last race winner
      EEPROM.write(EEPROM_LAST_RACE, teamID);

      // Strip " Grand Prix" to get just country name
      char shortGP[21] = {};
      strncpy(shortGP, lastGPName, 20);
      char* gp = strstr(shortGP, " Grand");
      if (gp) *gp = '\0';
      for (int i = 0; i < 20; i++)
        EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);
      EEPROM.commit();

      // If this is the final round, also save season champion
      if (currentRound == totalRounds) {
        EEPROM.write(EEPROM_SEASON_CHAMP, teamID);
        Serial.printf("Season champion saved: %s\n", teamNames[teamID]);
      }

      EEPROM.commit();
      
            https.end(); // close SSL connection BEFORE sending to Nano
      
            updateStatus("FINISHED", C_GREEN);
            drawRaceFinished(teamID); // ~1.2s natural delay while TFT animates
            sendToNano(CMD_CHECKERED);
            delay(9000);             // wait for full checkered animation
            sendToNano(teamID);
            delay(500);
            return;                   // skip the https.end() at bottom
      
          } else {
            updateStatus("LIVE", C_GREEN);
          }
      
        } else {
          Serial.printf("HTTP error: %d\n", https.GET());
          updateStatus("API ERR", C_RED);
        }
      
        https.end();
      }

// ==========================================================
// DATE HELPER
// ==========================================================

String getTodayDate() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
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
