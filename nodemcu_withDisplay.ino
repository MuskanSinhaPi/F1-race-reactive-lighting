#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
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
const char* ssid     = "your WiFi SSID";
const char* password = "your WiFi Password";

// ================= PI SERVER =================
const char* piHost = "192.168.1.x";   // <-- CHANGE THIS
const int   piPort = 5000;

// ================= TIMING =================
unsigned long lastCheck       = 0;
unsigned long lastWifiCheck   = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastNTPSync     = 0;
const unsigned long interval     = 5000;
const unsigned long wifiRetry    = 30000;
const unsigned long clockRefresh = 1000;
const unsigned long ntpResync    = 1800000;

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
// Slot 0: last race winner team ID (1 byte)
// Slot 1: last saved mode (1 byte)
// Slot 2: season champ team ID (1 byte)
// Slot 3-22: last GP short name (20 bytes)
// Slot 24-27: last race finish time_t (4 bytes)
// Slot 31: init flag (1 byte)
#define EEPROM_LAST_RACE      0
#define EEPROM_SAVED_MODE     1
#define EEPROM_SEASON_CHAMP   2
#define EEPROM_LAST_GP_NAME   3
#define EEPROM_LAST_RACE_TIME 24
#define EEPROM_INIT_FLAG      31
#define EEPROM_MAGIC          0xA5

// ================= TFT COLOR CONSTANTS =================
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

// ================= TEAM TFT COLORS =================
uint16_t teamTFTColor[] = {
  C_WHITE,
  0xD800, 0x1252, 0x0494, 0xD805, 0xB000,
  0xBDD7, 0xFB00, 0x0697, 0x2832, 0x0A32, 0x1496,
};

const char* teamNames[] = {
  "", "FERRARI", "ALPINE", "ASTON MARTIN",
  "HAAS", "AUDI", "CADILLAC", "McLAREN",
  "MERCEDES", "RACING BULLS", "RED BULL", "WILLIAMS"
};

// ================= STATE =================
bool raceFinished       = false;
bool raceSunday         = false;
bool raceCancelled      = false;
bool lightsOutTriggered = false;
bool lastButtonState    = HIGH;
bool nanoReady          = false;
bool piAvailable        = false;
unsigned long lastDebounce = 0;

int    currentMode    = MODE_LIVE;
int    currentRound   = -1;
int    totalRounds    = 24;
int    lastSentTeam   = -1;
int    currentTeamID  = TEAM_MCLAREN;
time_t raceStartEpoch = 0;  // always UTC epoch

char raceName[64]  = "---";
char lastGPName[64] = "Australian";
char p1Status[16]  = "LOADING";
double lastDiff = 999999;

// ================= FORWARD DECLARATIONS =================
void fetchRaceData();
void fetchRaceDataJolpica();
bool checkESPNFinished();
void applyMode(int mode);
void updateTeamDisplay(int teamID, bool isLive);
void updateStatus(const char* status, uint16_t color);
void updateModeDisplay();
void sendToNano(int cmd);
bool raceFinishedRecently();
void checkIfRaceFinishedToday();
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
void handleRaceFinished(int teamID);
const char* getModeName(int mode);
time_t parseUTCEpoch(const char* dateStr, const char* timeStr);
int getTeamID(String team);

// ==========================================================
// UTC EPOCH PARSER
// Parses Jolpica date "2026-03-29" + time "05:00:00Z" → UTC epoch
// Uses NO timezone offset — raw UTC so difftime() against time()
// (which returns UTC internally) is always correct regardless of
// what configTime() offset is set for display purposes.
// ==========================================================
time_t parseUTCEpoch(const char* dateStr, const char* timeStr) {
  int ry, rm, rd, hh, mm, ss = 0;
  sscanf(dateStr, "%d-%d-%d", &ry, &rm, &rd);
  sscanf(timeStr, "%d:%d:%dZ", &hh, &mm, &ss);

  int y = ry, m = rm, d = rd;
  int days = (y - 1970) * 365;
  for (int i = 1970; i < y; i++)
    if ((i % 4 == 0 && i % 100 != 0) || i % 400 == 0) days++;
  int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) mdays[1] = 29;
  for (int i = 0; i < m - 1; i++) days += mdays[i];
  days += d - 1;
  return (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
}

// ==========================================================
// SETUP
// Order: WiFi → NTP → detectNextRace (next GP countdown) →
//        EEPROM init → checkIfRaceFinishedToday (last.json) →
//        drawMainScreen → applyMode
// detectNextRace and checkIfRaceFinishedToday are separate:
//   detectNextRace  = next.json  → upcoming race, countdown
//   checkIfRaceFinishedToday = last.json → was today a race?
// ==========================================================
void setup() {
  Serial.begin(115200);
  delay(200); // let ESP boot noise settle before serial output
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(64);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawBootScreen();

  connectWiFi();
  drawStatusLine("WiFi OK", C_GREEN);

  // configTime sets the IST offset for display (localtime())
  // time() always returns UTC seconds since epoch — correct for our math
  configTime(19800, 0, "pool.ntp.org");
  drawStatusLine("Syncing time...", C_YELLOW);
  time_t now = time(nullptr);
  while (now < 100000) { delay(500); now = time(nullptr); }
  drawStatusLine("Time synced", C_GREEN);
  delay(300);

  // Upcoming race detection (next.json)
  for (int attempt = 0; attempt < 3 && raceStartEpoch == 0; attempt++) {
    if (attempt > 0) { drawStatusLine("Retry...", C_YELLOW); delay(2000); }
    detectNextRace();
  }
  if (raceStartEpoch == 0)
    drawStatusLine("No race data", C_RED);
  else
    drawStatusLine("Race data OK", C_GREEN);
  delay(300);

  drawStatusLine("Waiting Nano...", C_YELLOW);
  delay(2000);
  nanoReady = true;

  // First-time EEPROM initialisation
  if (EEPROM.read(EEPROM_INIT_FLAG) != EEPROM_MAGIC) {
    Serial.println("EEPROM first-time init");
    EEPROM.write(EEPROM_LAST_RACE,    TEAM_MCLAREN);
    EEPROM.write(EEPROM_SEASON_CHAMP, TEAM_MCLAREN);
    EEPROM.write(EEPROM_SAVED_MODE,   MODE_LIVE);
    time_t zero = 0;
    EEPROM.put(EEPROM_LAST_RACE_TIME, zero);
    EEPROM.write(EEPROM_INIT_FLAG, EEPROM_MAGIC);
    EEPROM.commit();
  }

  // Restore saved state
  int savedMode = EEPROM.read(EEPROM_SAVED_MODE);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;

  int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
  if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

  char savedGP[21] = {};
  for (int i = 0; i < 20; i++) savedGP[i] = EEPROM.read(EEPROM_LAST_GP_NAME + i);
  savedGP[20] = '\0';
  if (savedGP[0] >= 'A' && savedGP[0] <= 'z')
    strncpy(lastGPName, savedGP, sizeof(lastGPName) - 1);

  // Draw screen first so it doesn't flicker
  drawMainScreen();
  applyMode(currentMode);

  // Now check if race finished today — this may override with FINISHED state
  // Priority: EEPROM (fast, no API) → last.json (authoritative)
  if (raceFinishedRecently()) {
    Serial.println("Boot: race finished recently — using EEPROM");
    int teamID = EEPROM.read(EEPROM_LAST_RACE);
    if (teamID < 1 || teamID > 11) teamID = TEAM_MCLAREN;
    raceSunday   = true;
    raceFinished = true;
    lastSentTeam = teamID;
    updateTeamDisplay(teamID, true);
    updateStatus("FINISHED", C_GREEN);
  } else {
    checkIfRaceFinishedToday();
  }
}

// ==========================================================
// LOOP
// ==========================================================
void loop() {
  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();

  if (millis() - lastNTPSync > ntpResync) {
    lastNTPSync = millis();
    configTime(19800, 0, "pool.ntp.org");
    Serial.println("NTP resynced");
  }

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
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(18, 5); tft.print("F1  LIVE");
  tft.fillRect(0, 106, 160, 22, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(32, 112); tft.print("LED CONTROLLER");
  tft.fillRect(60, 30, 40, 40, C_ORANGE);
  tft.fillRect(65, 35, 30, 30, C_BLACK);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2);
  tft.setCursor(68, 40); tft.print("F1");
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(38, 82); tft.print("Booting...");
}

void drawStatusLine(const char* msg, uint16_t color) {
  tft.fillRect(0, 82, 160, 12, C_BLACK);
  tft.setTextColor(color); tft.setTextSize(1);
  int x = (160 - strlen(msg) * 6) / 2;
  tft.setCursor(x, 84); tft.print(msg);
  delay(300);
}

// ==========================================================
// MAIN SCREEN
// ==========================================================
void drawMainScreen() {
  tft.fillScreen(C_BLACK);

  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("F1 LIVE");
  tft.setCursor(100, 4); tft.print("--:--:--");

  tft.fillRect(0, 16, 160, 12, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 20); tft.print("Next GP: ");
  tft.setTextColor(C_WHITE);
  char shortName[20]; strncpy(shortName, raceName, 19); shortName[19] = '\0';
  tft.print(shortName);

  tft.drawFastHLine(0, 28, 160, C_BORDER);

  tft.fillRect(0, 29, 4, 44, teamTFTColor[currentTeamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY)   tft.print("LAST SEASON P1");
  else if (currentMode == MODE_LIVE) tft.print("Last GP:");
  else                               tft.print("CONSTRUCTOR");
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[currentTeamID]); tft.setTextSize(2);
  tft.setCursor(8, 45); tft.print(teamNames[currentTeamID]);

  tft.fillRect(0, 73, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 73, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 77); tft.print("STATUS:");
  tft.setTextColor(C_GREEN); tft.setCursor(52, 77); tft.print(p1Status);

  tft.fillRect(0, 86, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 86, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 90); tft.print("MODE:");
  tft.setTextColor(C_WHITE); tft.setCursor(40, 90); tft.print(getModeName(currentMode));

  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 99, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 103);
  if (raceFinished)
      tft.print("RACE COMPLETE");
  else if (raceCancelled)
      tft.print("RACE CANCELLED");
  else if (raceSunday)
      tft.print("LIGHTS OUT: --:--:--");
  else
      tft.print("NO RACE TODAY");
  tft.drawFastHLine(0, 112, 160, C_BORDER);
  tft.fillRect(0, 113, 160, 15, C_PANEL);
  tft.setTextColor(C_DIM);
  tft.setCursor(4, 117); tft.print("Rnd ");
  tft.print(currentRound > 0 ? currentRound : 0);
  tft.setCursor(60, 117);
  tft.print(piAvailable ? "Pi+ESPN" : "Jolpica");
}

// ==========================================================
// CLOCK — uses localtime() which respects configTime IST offset
// ==========================================================
void updateClock() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  tft.fillRect(96, 1, 63, 14, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(98, 4); tft.print(buf);
}

// ==========================================================
// COUNTDOWN
// difftime(raceStartEpoch, time()) — both UTC, always correct
// ==========================================================
void updateCountdown() {
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);
  tft.fillRect(4, 100, 155, 11, C_PANEL);
  tft.setTextSize(1); tft.setCursor(4, 103);
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
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY) {
    tft.print("LAST SEASON P1");
  } else if (isLive) {
    char label[27];
    char gpShort[14];
    strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
    char* gp = strstr(gpShort, " Grand");
    if (gp) *gp = '\0';
    snprintf(label, sizeof(label), "Last GP: %s | P1:", gpShort);
    tft.print(label);
  } else {
    tft.print("CONSTRUCTOR");
  }
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[teamID]); tft.setTextSize(2);
  tft.setCursor(8, 45); tft.print(teamNames[teamID]);
}

// ==========================================================
// UPDATE STATUS — syncs p1Status so drawMainScreen() never stale
// ==========================================================
void updateStatus(const char* status, uint16_t color) {
  strncpy(p1Status, status, sizeof(p1Status) - 1);
  p1Status[sizeof(p1Status) - 1] = '\0';
  tft.fillRect(52, 74, 106, 11, C_PANEL);
  tft.setTextColor(color); tft.setTextSize(1);
  tft.setCursor(52, 77); tft.print(status);
}

// ==========================================================
// UPDATE MODE DISPLAY
// ==========================================================
void updateModeDisplay() {
  tft.fillRect(40, 87, 118, 11, C_PANEL);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(40, 90); tft.print(getModeName(currentMode));
}

// ==========================================================
// RACE FINISHED SCREEN
// ==========================================================
void drawRaceFinished(int teamID) {
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, 0, 160, 16, C_WHITE); delay(200);
    tft.fillRect(0, 0, 160, 16, C_BLACK); delay(200);
  }
  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("RACE FINISHED!");
  updateClock();
  tft.fillRect(0, 29, 160, 44, teamTFTColor[teamID]);
  tft.setTextColor(C_BLACK); tft.setTextSize(1);
  tft.setCursor(8, 33); tft.print("WINNER");
  tft.setTextSize(2); tft.setCursor(8, 45); tft.print(teamNames[teamID]);
  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(4, 103); tft.print("RACE COMPLETE");
}

// ==========================================================
// HANDLE RACE FINISHED
// Guard: if already finished, silently return
// ==========================================================
void handleRaceFinished(int teamID) {
  if (raceFinished) return;

  raceFinished = true;
  lastSentTeam = teamID;

  EEPROM.write(EEPROM_LAST_RACE, teamID);
  time_t now = time(nullptr);
  EEPROM.put(EEPROM_LAST_RACE_TIME, now);

  // Strip " Grand Prix" suffix — store clean short name
  char shortGP[21] = {};
  strncpy(shortGP, lastGPName, 20);
  char* grandPtr = strstr(shortGP, " Grand Prix");
  if (grandPtr) *grandPtr = '\0';
  for (int i = 0; i < 20; i++)
    EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);

  if (currentRound == totalRounds) {
    EEPROM.write(EEPROM_SEASON_CHAMP, teamID);
    Serial.printf("Season champion saved: %s\n", teamNames[teamID]);
  }
  EEPROM.commit();

  updateStatus("FINISHED", C_GREEN);
  drawRaceFinished(teamID);
  sendToNano(CMD_CHECKERED);
  delay(9000);
  sendToNano(teamID);
  delay(500);
}

// ==========================================================
// WIFI
// ==========================================================
void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(" Connected");
}

void maintainWiFi() {
  if (millis() - lastWifiCheck > wifiRetry) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      updateStatus("WiFi Lost", C_RED);
      connectWiFi();
      // Restore last known status
      tft.fillRect(52, 74, 106, 11, C_PANEL);
      tft.setTextColor(C_GREEN); tft.setTextSize(1);
      tft.setCursor(52, 77); tft.print(p1Status);
    }
  }
}

// ==========================================================
// SEND TO NANO
// ==========================================================
void sendToNano(int cmd) {
  if (!nanoReady) return;
  nanoSerial.print(cmd); nanoSerial.print('\n');
  Serial.printf("-> Nano: %d\n", cmd);
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
      Serial.printf("Mode -> %s\n", getModeName(currentMode));
      EEPROM.write(EEPROM_SAVED_MODE, currentMode); EEPROM.commit();
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
    lastSentTeam = -1;

    int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
    if (savedTeam < 1 || savedTeam > 11) savedTeam = TEAM_MCLAREN;
    currentTeamID = savedTeam;

    updateTeamDisplay(savedTeam, true);
    updateModeDisplay();
    sendToNano(savedTeam);
    lastSentTeam = savedTeam;

    time_t now = time(nullptr);
    double elapsed = difftime(now, raceStartEpoch);
    if (raceSunday && !raceFinished && elapsed >= 0)
      sendToNano(CMD_PULSE);

    if (raceFinished) {
      updateStatus("FINISHED", C_GREEN);
    } else if (raceSunday) {
      updateStatus("FETCHING", C_YELLOW);
      fetchRaceData();
    } else {
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
    default:            return "LIVE";
  }
}

// ==========================================================
// RACE DETECTION — Jolpica next.json
// Responsibilities: next GP name, raceStartEpoch, raceSunday flag
// raceStartEpoch is pure UTC epoch from parseUTCEpoch()
// raceSunday window: -6h to +6h around race start
// (tight window — finish detection handled separately by last.json)
// ==========================================================
void detectNextRace() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");
  if (https.GET() != 200) { https.end(); return; }

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, https.getString());
  https.end();
  if (err) return;

  const char* totalStr = doc["MRData"]["total"];
  if (totalStr) totalRounds = atoi(totalStr);

  auto race        = doc["MRData"]["RaceTable"]["Races"][0];
  const char* name     = race["raceName"];
  const char* date     = race["date"];
  const char* timeS    = race["time"];
  const char* roundStr = race["round"];

  if (name)     strncpy(raceName, name, sizeof(raceName) - 1);
  if (roundStr) currentRound = atoi(roundStr);

  if (date && timeS) {
    raceCancelled = false;
    raceStartEpoch = parseUTCEpoch(date, timeS);
    time_t now = time(nullptr);
    double diff = difftime(raceStartEpoch, now);

    // -6h to +12h: covers race day only, no false positives between weekends
    if (diff >= -21600 && diff <= 43200) raceSunday = true;
    
    if (diff > 43200){
        lightsOutTriggered = false;
        raceSunday = false;
    }

    Serial.printf("Race: %s | Rnd: %d/%d | raceDay: %s | diff: %.0f s\n",
                  raceName, currentRound, totalRounds,
                  raceSunday ? "YES" : "NO", diff);
  }
}

// ==========================================================
// CHECK IF RACE ALREADY FINISHED — boot-time check via last.json
// Runs only if EEPROM doesn't already confirm recent finish
// Sets raceSunday=true and calls handleRaceFinished() if confirmed
// ==========================================================
void checkIfRaceFinishedToday() {
  Serial.println("Checking last.json for finished race...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");
  if (https.GET() != 200) { https.end(); return; }

  StaticJsonDocument<128> filter;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"]                          = true;
  filter["MRData"]["RaceTable"]["Races"][0]["date"]                              = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"]              = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.printf("checkIfRace err: %s\n", err.c_str()); return; }

  auto race          = doc["MRData"]["RaceTable"]["Races"][0];
  const char* gpName = race["raceName"];
  const char* date   = race["date"];

  JsonArray results = race["Results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return;

  const char* team   = results[0]["Constructor"]["name"];
  const char* status = results[0]["status"];
  if (!team || !status || !date) return;

  if (String(status) != "Finished") return;

  // Date guard: race date must be today or yesterday (UTC)
  // Use raceStartEpoch if available, otherwise derive from race date
  time_t now = time(nullptr);
  time_t raceDateEpoch = parseUTCEpoch(date, "00:00:00Z");
  double diff = difftime(now, raceDateEpoch);
  
  // Ignore if race older than ~36h
  if (diff > 129600) return;

  Serial.printf("last.json: race finished, winner: %s\n", team);

  if (gpName) strncpy(lastGPName, gpName, sizeof(lastGPName) - 1);
  int teamID = getTeamID(String(team));

  // Write EEPROM so next boot uses fast path
  EEPROM.put(EEPROM_LAST_RACE_TIME, now);
  EEPROM.write(EEPROM_LAST_RACE, teamID);
  char shortGP[21] = {};
  strncpy(shortGP, lastGPName, 20);
  char* grandPtr = strstr(shortGP, " Grand Prix");
  if (grandPtr) *grandPtr = '\0';
  for (int i = 0; i < 20; i++)
    EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);
  EEPROM.commit();

  raceSunday = true;
  handleRaceFinished(teamID);
}

// ==========================================================
// LIGHTS OUT
// difftime(raceStartEpoch, time()) — both UTC, always correct
// ==========================================================
void checkLightsOutCountdown() {
  if (!raceSunday || raceFinished || raceStartEpoch == 0) return;
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);
  if (!lightsOutTriggered && lastDiff > 0 && diff <= 0) {
    Serial.println("LIGHTS OUT!");
    sendToNano(CMD_LIGHTS_OUT);
    lightsOutTriggered = true;
    for (int i = 0; i < 5; i++) {
      tft.fillScreen(C_RED); delay(150);
      tft.fillScreen(C_BLACK); delay(150);
    }
    drawMainScreen();
    updateStatus("LIVE", C_GREEN);
  }
  
  lastDiff = diff;
  
}

// ==========================================================
// ESPN FINISHED CHECK
// Only called after 90 min elapsed during live polling
// Checks Race competition (type.id=="3") for STATUS_FINAL
// ==========================================================
bool checkESPNFinished() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard");
  if (https.GET() != 200) { https.end(); return false; }

  StaticJsonDocument<128> filter;
  filter["events"][0]["competitions"][0]["type"]["id"]                  = true;
  filter["events"][0]["competitions"][0]["status"]["type"]["name"]      = true;
  filter["events"][0]["competitions"][0]["status"]["type"]["completed"] = true;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) return false;

  for (JsonObject comp : doc["events"][0]["competitions"].as<JsonArray>()) {
    const char* typeId = comp["type"]["id"];
    if (typeId && String(typeId) == "3") {
      bool completed    = comp["status"]["type"]["completed"] | false;
      const char* sName = comp["status"]["type"]["name"];
      if (completed || (sName && String(sName) == "STATUS_FINAL")) {
        Serial.println("ESPN: race FINISHED");
        return true;
      }
      return false;
    }
  }
  return false;
}

// ==========================================================
// FETCH RACE DATA — Pi primary, ESPN finish at 90min, Jolpica fallback
// ==========================================================
void fetchRaceData() {
  time_t now     = time(nullptr);
  double elapsed = difftime(now, raceStartEpoch);

  if (elapsed < 0) {
    updateStatus("PRE RACE", C_YELLOW);
    return;
  }

  WiFiClient piClient;
  HTTPClient http;
  String url = String("http://") + piHost + ":" + piPort + "/p1";
  http.begin(piClient, url);
  http.setTimeout(4000);
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();

    if (!err) {
      piAvailable = true;
      const char* team   = doc["team"];
      const char* status = doc["status"];
      const char* gp     = doc["gp"];

      Serial.printf("Pi -> team:%s status:%s\n",
                    team ? team : "null", status ? status : "null");

      if (gp && strlen(gp) > 0) {
        strncpy(lastGPName, gp, sizeof(lastGPName) - 1);
        char* grandPtr = strstr(lastGPName, " Grand Prix");
        if (grandPtr) *grandPtr = '\0';
      }

      if (team && status) {
        String s = String(status);

        if (s == "finished") {
          updateTeamDisplay(getTeamID(String(team)), true);
          handleRaceFinished(getTeamID(String(team)));
          return;
        }
        if (s == "delayed") {
          updateStatus("RAIN DELAY", C_YELLOW);
          if (lastSentTeam >= 1) sendToNano(CMD_PULSE);
          return;
        }
        if (s == "cancelled") { 
            raceCancelled = true;
            raceSunday = false;
            raceFinished = false;
            lightsOutTriggered = false;
            updateStatus("CANCELLED", C_RED);
            drawMainScreen();
            sendToNano(CMD_DISPLAY);
            return;
        }
    
        if (s == "postponed") { updateStatus("POSTPONED", C_RED); return; }

        if (s == "live") {
          int teamID = getTeamID(String(team));
          updateTeamDisplay(teamID, true);
          if (teamID != lastSentTeam) {
            sendToNano(teamID);
            lastSentTeam = teamID;
          }
          sendToNano(CMD_PULSE); // keepalive every poll
          updateStatus("LIVE", C_GREEN);

          if (elapsed >= 5400 && checkESPNFinished()) {
            fetchRaceDataJolpica();
          }
          return;
        }
        // "pre" or "none" — fall through to Jolpica
      }
    } else {
      http.end();
    }
  } else {
    http.end();
  }

  piAvailable = false;
  fetchRaceDataJolpica();
}

// ==========================================================
// JOLPICA FALLBACK
// ==========================================================
void fetchRaceDataJolpica() {
  Serial.println("Jolpica fallback...");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");
  if (https.GET() != 200) { https.end(); updateStatus("API ERR", C_RED); return; }

  StaticJsonDocument<200> filter;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"]                          = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"]              = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  if (err) { https.end(); updateStatus("J ERR", C_RED); return; }

  auto race = doc["MRData"]["RaceTable"]["Races"][0];
  const char* gpName = race["raceName"];

  JsonArray results = race["Results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) {
    https.end(); updateStatus("NO DATA", C_GREY); return;
  }

  const char* team   = results[0]["Constructor"]["name"];
  const char* status = results[0]["status"];
  if (!status || !team) { https.end(); updateStatus("NO DATA", C_GREY); return; }

  https.end();

  time_t now     = time(nullptr);
  double elapsed = difftime(now, raceStartEpoch);

  if (elapsed < 0) { updateStatus("PRE RACE", C_YELLOW); return; }

  if (elapsed < 5400 && String(status) != "Finished") {
    updateStatus("LIVE", C_GREEN);
    sendToNano(CMD_PULSE);
    return;
  }

  if (gpName) strncpy(lastGPName, gpName, sizeof(lastGPName) - 1);
  int teamID = getTeamID(String(team));
  updateTeamDisplay(teamID, true);

  if (String(status) == "Finished") {
    handleRaceFinished(teamID);
  } else {
    if (teamID != lastSentTeam) { sendToNano(teamID); lastSentTeam = teamID; }
    sendToNano(CMD_PULSE);
    updateStatus("LIVE", C_GREEN);
  }
}

// ==========================================================
// RACE FINISHED RECENTLY — fast EEPROM path on boot
// ==========================================================
bool raceFinishedRecently() {
  time_t storedTime;
  EEPROM.get(EEPROM_LAST_RACE_TIME, storedTime);
  time_t now = time(nullptr);
  if (storedTime < 1700000000 || storedTime > now) return false;
  double diff = difftime(now, storedTime);
  if (diff < 43200) { // within 12h
    Serial.println("EEPROM: race finished recently");
    return true;
  }
  return false;
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
