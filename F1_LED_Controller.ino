// ============================================================
//  F1 NodeMCU — PRODUCTION
//
//  Push format from Pi:
//    GET /update?team=X&status=Y
//                [&gp=Z]
//                [&wdc_team=A&wdc_status=projected|confirmed]
//
//  Display modes:
//    LIVE    — last race P1 colour (Jolpica on boot, live push during race)
//              idle/delay/cancel/postponed/scheduled = lastRaceTeamID colour
//    DISPLAY — WDC driver's constructor (defending champion)
//              label: "DEFENDING WDC" normally, "WDC CHAMPION" when confirmed
//              status: "DEFENDING" normally, "CHAMPIONS!" when confirmed
//    Team    — fixed team colour, no live updates
//
//  Final race flow:
//    1. Race finishes → checkered animation with RACE WINNER colour
//    2. Main screen, status = "WDC:[team]?" (projection from Pi)
//    3. Pi polls Jolpica every 2 min → status cycles "WDC PENDING"
//    4. WDC confirmed push → gold season end screen (WDC driver's team)
//    5. EEPROM updated, display mode shows WDC team going forward
//    6. Season end screen persists 12h, button dismisses
//
//  Non-final race: checkered animation only, main screen persists
//
//  Boot paths:
//    Season end within 12h  → restore gold screen
//    Race finish within 12h → Jolpica re-confirms (handles DQ) → show finish
//    Normal boot            → Jolpica last race seeds colour + lastGPName
//                           → Jolpica next race seeds countdown + round info
//
//  Bug fixes applied:
//    1. lastRaceTeamID/currentTeamID seeded from EEPROM before Jolpica
//       so warm white never appears even on Jolpica failure
//    2. NTP sync waits up to 30s; updateClock() shows -- until synced
//    3. drawMainScreen() then applyMode() — correct colour on boot
//       without needing button press
// ============================================================

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

#define TFT_CS   D2
#define TFT_DC   D1
#define TFT_RST  -1
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

#define BUTTON_PIN D3
#define NANO_RX    D6
#define NANO_TX    D8
SoftwareSerial nanoSerial(NANO_RX, NANO_TX);

const char* ssid     = "YOUR WIFI SSID";
const char* password = "YOUR WIFI PASSWORD";

unsigned long lastWifiCheck   = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastNTPSync     = 0;
const unsigned long wifiRetry    =  30000;
const unsigned long clockRefresh =   1000;
const unsigned long ntpResync    = 1800000;

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

#define CMD_DISPLAY      0
#define CMD_PULSE      255
#define CMD_CHECKERED   99
#define CMD_LIGHTS_OUT  77

#define EEPROM_LAST_RACE      0
#define EEPROM_SAVED_MODE     1
#define EEPROM_WDC_CHAMP      2
#define EEPROM_LAST_GP_NAME   3
#define EEPROM_LAST_RACE_TIME 24
#define EEPROM_SEASON_TIME    28
#define EEPROM_INIT_FLAG      32
#define EEPROM_MAGIC          0xA5

#define C_PANEL     0x0841
#define C_BORDER    0x2945
#define C_WHITE     0xFFFF
#define C_GREY      0x8C51
#define C_RED       0xF800
#define C_GREEN     0x07E0
#define C_YELLOW    0xFFE0
#define C_GOLD      0xC600
#define C_DIM       0x4A49
#define C_BLACK     0x0000
#define C_ORANGE    0xFB00

uint16_t teamTFTColor[] = {
  C_WHITE,
  0xD800, 0x1252, 0x0494, 0xD805, 0xB000,
  0xBDD7, 0xFB00, 0x0697, 0x2832, 0xF926, 0x1496,
};

const char* teamNames[] = {
  "", "FERRARI", "ALPINE", "ASTON MARTIN",
  "HAAS", "AUDI", "CADILLAC", "McLAREN",
  "MERCEDES", "RACING BULLS", "RED BULL", "WILLIAMS"
};

WiFiServer server(8080);

bool raceFinished       = false;
bool raceSunday         = false;
bool raceCancelled      = false;
bool lightsOutTriggered = false;
bool lastButtonState    = HIGH;
bool nanoReady          = false;
bool piAvailable        = false;
bool wdcConfirmed       = false;
int  wdcTeamID          = 0;
bool seasonScreenActive = false;
bool raceAnimationPlayedThisBoot   = false;
bool seasonAnimationPlayedThisBoot = false;
bool isFinalRound = false;
bool pendingWDC   = false;
unsigned long lastDebounce = 0;
int    currentMode    = MODE_LIVE;
int    currentRound   = -1;
int    totalRounds    = 24;
int    lastSentTeam   = -1;
int    lastRaceTeamID = TEAM_MCLAREN;   // FIX 1: never 0
int    currentTeamID  = TEAM_MCLAREN;   // FIX 1: never 0
time_t raceStartEpoch = 0;
char raceName[64]  = "TBC";
char lastGPName[64] = "";
char p1Status[16]  = "LOADING";
double lastDiff    = 999999;

void fetchLastRaceFromJolpica(bool forRebootReplay);
void fetchNextRaceFromJolpica();
void fetchSeasonTotal();
int  getRacePhase();
void applyMode(int mode);
void updateTeamDisplay(int teamID, bool isLive);
void updateStatus(const char* status, uint16_t color);
void updateModeDisplay();
void updateInfoLine();
void sendToNano(int cmd);
bool raceFinishedRecently();
bool seasonFinishedRecently();
void triggerRaceFinishAnimation(int teamID);
void triggerSeasonEndCelebration(int champID);
void checkButton();
void connectWiFi();
void maintainWiFi();
void checkLightsOutCountdown();
void drawBootScreen();
void drawStatusLine(const char* msg, uint16_t color);
void drawMainScreen();
void drawRaceFinishedScreen(int teamID);
void drawSeasonEndScreen(int champID);
void updateClock();
void handleRaceFinished(int teamID);
void handleIncoming();
void flushPendingClients();
const char* getModeName(int mode);
time_t parseUTCEpoch(const char* dateStr, const char* timeStr);
int getTeamID(String team);

int getRacePhase() {
  if (raceStartEpoch == 0) return 0;
  time_t now = time(nullptr);
  double diff = difftime(now, raceStartEpoch);
  if (diff < -21600) return 0;
  if (diff < 0)      return 1;
  if (diff < 7200)   return 2;
  if (diff < 43200)  return 3;
  return 4;
}

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

void flushPendingClients() {
  unsigned long start = millis();
  while (millis() - start < 200) {
    WiFiClient stale = server.available();
    if (!stale) break;
    stale.stop();
    yield();
  }
  Serial.println("TCP backlog flushed");
}

void fetchLastRaceFromJolpica(bool forRebootReplay) {
  drawStatusLine(forRebootReplay ? "Confirming result..." : "Fetching last GP...", C_YELLOW);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");
  https.setTimeout(8000);
  int code = https.GET();
  if (code != 200) {
    https.end();
    Serial.printf("Jolpica last: HTTP %d — using EEPROM seed\n", code);
    return;
  }
  StaticJsonDocument<128> filter;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"]                          = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["round"]                             = true;
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.println("Jolpica last: parse err — using EEPROM seed"); return; }

  const char* con   = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];
  const char* gpN   = doc["MRData"]["RaceTable"]["Races"][0]["raceName"];
  const char* rndS  = doc["MRData"]["RaceTable"]["Races"][0]["round"];
  if (con) {
    int t = getTeamID(String(con));
    if (t >= 1 && t <= 11) {
      lastRaceTeamID = t; currentTeamID = t;
      EEPROM.write(EEPROM_LAST_RACE, t);
      Serial.printf("Jolpica P1: %s\n", teamNames[t]);
    }
  }
  if (gpN) {
    strncpy(lastGPName, gpN, sizeof(lastGPName) - 1);
    lastGPName[sizeof(lastGPName) - 1] = '\0';
    char* g = strstr(lastGPName, " Grand Prix"); if (g) *g = '\0';
    for (int i = 0; i < 20; i++)
      EEPROM.write(EEPROM_LAST_GP_NAME + i, i < (int)strlen(lastGPName) ? lastGPName[i] : 0);
  }
  if (rndS) currentRound = atoi(rndS);
  EEPROM.commit();
  Serial.printf("Jolpica: rnd %d P1=%s GP=%s\n", currentRound, teamNames[lastRaceTeamID], lastGPName);
}

void fetchSeasonTotal() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current.json?limit=1");
  https.setTimeout(8000);
  int code = https.GET();
  if (code != 200) { https.end(); Serial.printf("Season total: HTTP %d\n", code); return; }
  StaticJsonDocument<64> filter;
  filter["MRData"]["total"] = true;
  StaticJsonDocument<128> doc;
  deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  const char* totS = doc["MRData"]["total"];
  if (totS) {
    totalRounds = atoi(totS);
    Serial.printf("Season total: %d rounds\n", totalRounds);
  }
}

void fetchNextRaceFromJolpica() {
  drawStatusLine("Fetching next GP...", C_YELLOW);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");
  https.setTimeout(8000);
  int code = https.GET();
  if (code != 200) { https.end(); Serial.printf("Jolpica next: HTTP %d\n", code); return; }
  StaticJsonDocument<64> filter;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["date"]     = true;
  filter["MRData"]["RaceTable"]["Races"][0]["time"]     = true;
  filter["MRData"]["RaceTable"]["Races"][0]["round"]    = true;
  filter["MRData"]["total"]                             = true;
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.println("Jolpica next: parse err"); return; }
  const char* name  = doc["MRData"]["RaceTable"]["Races"][0]["raceName"];
  const char* date  = doc["MRData"]["RaceTable"]["Races"][0]["date"];
  const char* rtime = doc["MRData"]["RaceTable"]["Races"][0]["time"];
  const char* rndS  = doc["MRData"]["RaceTable"]["Races"][0]["round"];
  const char* totS  = doc["MRData"]["total"];
  if (name) {
    strncpy(raceName, name, sizeof(raceName) - 1);
    raceName[sizeof(raceName) - 1] = '\0';
    char* g = strstr(raceName, " Grand Prix"); if (g) *g = '\0';
  }
  if (date && rtime) raceStartEpoch = parseUTCEpoch(date, rtime);
  if (rndS) currentRound = atoi(rndS) - 1;
  // Don't use totS — "total" in next.json is count of results (always 1),
  // not the season length. Fetch season total separately.
  Serial.printf("Next: %s rnd %d/%d\n", raceName, currentRound + 1, totalRounds);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(64);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawBootScreen();

  connectWiFi();
  server.begin();
  Serial.printf("HTTP server on %s:8080\n", WiFi.localIP().toString().c_str());
  drawStatusLine("WiFi OK", C_GREEN);

  // NTP sync — IST = UTC+5:30 = 19800 seconds offset
  // Try multiple servers in case one is blocked
  configTime(19800, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  drawStatusLine("Syncing time...", C_YELLOW);
  time_t now = time(nullptr);
  unsigned long tStart = millis();
  while (now < 100000 && millis() - tStart < 20000) { delay(500); now = time(nullptr); }
  if (now > 100000) {
    drawStatusLine("Time synced", C_GREEN);
    Serial.printf("NTP OK: %ld\n", (long)now);
  } else {
    drawStatusLine("Time sync failed", C_RED);
    Serial.println("NTP blocked — clock unavailable, continuing boot");
  }
  delay(200);

  if (EEPROM.read(EEPROM_INIT_FLAG) != EEPROM_MAGIC) {
    Serial.println("First boot — EEPROM init");
    EEPROM.write(EEPROM_LAST_RACE,  TEAM_MERCEDES);
    EEPROM.write(EEPROM_WDC_CHAMP,  TEAM_MCLAREN);
    EEPROM.write(EEPROM_SAVED_MODE, MODE_LIVE);
    time_t zero = 0;
    EEPROM.put(EEPROM_LAST_RACE_TIME, zero);
    EEPROM.put(EEPROM_SEASON_TIME,    zero);
    EEPROM.write(EEPROM_INIT_FLAG,    EEPROM_MAGIC);
    EEPROM.commit();
  }

  int savedMode = EEPROM.read(EEPROM_SAVED_MODE);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;
  int savedWDC = EEPROM.read(EEPROM_WDC_CHAMP);

  // FIX 1: seed colour from EEPROM before ANY Jolpica call
  {
    int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
    if (savedTeam >= 1 && savedTeam <= 11) {
      lastRaceTeamID = savedTeam;
      currentTeamID  = savedTeam;
    }
    for (int i = 0; i < 20; i++) lastGPName[i] = EEPROM.read(EEPROM_LAST_GP_NAME + i);
    lastGPName[20] = '\0';
    Serial.printf("EEPROM seed: %s | %s\n", teamNames[lastRaceTeamID], lastGPName);
  }

  // PATH A: season end within 12h
  if (savedWDC >= 1 && savedWDC <= 11 && seasonFinishedRecently()) {
    wdcTeamID = savedWDC; wdcConfirmed = true;
    lastRaceTeamID = savedWDC; currentTeamID = savedWDC;
    nanoReady = true;
    fetchSeasonTotal();
    fetchNextRaceFromJolpica();
    drawSeasonEndScreen(savedWDC);
    seasonScreenActive = true;
    sendToNano(savedWDC); sendToNano(CMD_CHECKERED);
    return;
  }

  // PATH B: race finish within 12h
  if (raceFinishedRecently()) {
    delay(1500);
    fetchLastRaceFromJolpica(true);
    fetchSeasonTotal();
    fetchNextRaceFromJolpica();
    currentTeamID = lastRaceTeamID;
    raceFinished  = true;
    nanoReady     = true;
    if (savedWDC >= 1 && savedWDC <= 11) { wdcTeamID = savedWDC; wdcConfirmed = true; }
    if (!wdcConfirmed && currentRound >= totalRounds) { pendingWDC = true; isFinalRound = true; }
    drawMainScreen();
    updateTeamDisplay(lastRaceTeamID, true);
    updateStatus(pendingWDC ? "WDC PENDING" : "FINISHED",
                 pendingWDC ? C_YELLOW      : C_GREEN);
    updateInfoLine();
    sendToNano(lastRaceTeamID);
    applyMode(currentMode);
    return;
  }

  // PATH C: normal boot
  // 1.5s delay — first HTTPS call after boot needs extra heap settling time
  delay(1500);
  fetchLastRaceFromJolpica(false);
  fetchSeasonTotal();
  fetchNextRaceFromJolpica();
  if (savedWDC >= 1 && savedWDC <= 11) { wdcTeamID = savedWDC; wdcConfirmed = true; }
  nanoReady = true;
  // FIX 3: drawMainScreen then applyMode (applyMode redraws team card correctly)
  drawMainScreen();
  applyMode(currentMode);
}

void updateInfoLine() {
  tft.fillRect(0, 100, 160, 12, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 103);
  if (seasonScreenActive) tft.print("SEASON COMPLETE!");
  else if (raceFinished)  tft.print("RACE COMPLETE");
  else if (raceCancelled) tft.print("RACE CANCELLED");
  else if (raceSunday)    tft.print("RACE IN PROGRESS");
  else                    tft.print("NO RACE TODAY");
}

void handleIncoming() {
  WiFiClient client = server.available();
  if (!client) return;
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 200) { client.stop(); return; }
  }
  String req = "";
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (req == "") req = line;
    if (line == "\r" || line == "") break;
  }
  Serial.println("Push: " + req);
  if (req.indexOf("/update") >= 0) {
    auto extract = [&](String key) -> String {
      int idx = req.indexOf("?" + key + "=");
      if (idx < 0) idx = req.indexOf("&" + key + "=");
      if (idx < 0) return "";
      int start = idx + key.length() + 2;
      int end   = req.indexOf('&', start);
      if (end < 0) end = req.indexOf(' ', start);
      if (end < 0) end = req.length();
      String val = req.substring(start, end);
      val.replace("+", " "); val.trim();
      return val;
    };
    String team     = extract("team");
    String status   = extract("status");
    String gp       = extract("gp");
    String wdcTeamS = extract("wdc_team");
    String wdcStatS = extract("wdc_status");
    Serial.printf("  team:%s status:%s gp:%s wdc:%s(%s)\n",
                  team.c_str(), status.c_str(), gp.c_str(),
                  wdcTeamS.c_str(), wdcStatS.c_str());
    if (gp.length() > 0) {
      strncpy(lastGPName, gp.c_str(), sizeof(lastGPName) - 1);
      lastGPName[sizeof(lastGPName) - 1] = '\0';
      char* g = strstr(lastGPName, " Grand Prix"); if (g) *g = '\0';
    }
    if (wdcTeamS.length() > 0) isFinalRound = true;
    if (wdcTeamS.length() > 0 && wdcStatS == "confirmed" && !wdcConfirmed) {
      int cid = getTeamID(wdcTeamS);
      if (cid >= 1 && cid <= 11) {
        wdcTeamID = cid; wdcConfirmed = true; pendingWDC = false;
        EEPROM.write(EEPROM_WDC_CHAMP, cid);
        time_t nowT = time(nullptr);
        if (nowT > 1700000000) EEPROM.put(EEPROM_SEASON_TIME, nowT);
        EEPROM.commit();
        Serial.printf("WDC confirmed: %s\n", teamNames[cid]);
        if (!seasonAnimationPlayedThisBoot) { delay(1500); triggerSeasonEndCelebration(cid); }
      }
    }
    if (wdcTeamS.length() > 0 && wdcStatS == "projected" && !wdcConfirmed)
      pendingWDC = true;

    int teamID     = getTeamID(team);
    int wdcTeamIDp = getTeamID(wdcTeamS);
    bool liveMode  = (currentMode == MODE_LIVE);

    if (status == "live") {
      raceCancelled = false; raceSunday = true;
      int col = (isFinalRound && wdcTeamIDp >= 1 && wdcStatS == "projected") ? wdcTeamIDp
              : (teamID >= 1) ? teamID : lastRaceTeamID;
      currentTeamID = col;
      if (liveMode) {
        updateTeamDisplay(col, true); updateInfoLine();
        if (col != lastSentTeam) { sendToNano(col); lastSentTeam = col; }
        sendToNano(CMD_PULSE);
        updateStatus(wdcTeamIDp >= 1 && wdcStatS == "projected" ? "WDC LIVE!" : "LIVE", C_GREEN);
      } else { lastSentTeam = col; }

    } else if (status == "finished") {
      raceCancelled = false; raceSunday = true;
      if (teamID >= 1) { currentTeamID = teamID; lastRaceTeamID = teamID; }
      if (liveMode && teamID >= 1) updateTeamDisplay(teamID, true);
      handleRaceFinished(teamID);
      if (liveMode) {
        updateInfoLine();
        if (!wdcConfirmed && isFinalRound) {
          if (wdcTeamIDp >= 1 && wdcStatS == "projected") {
            char msg[16]; snprintf(msg, sizeof(msg), "WDC:%s?", teamNames[wdcTeamIDp]);
            updateStatus(msg, C_YELLOW);
          } else { updateStatus("WDC PENDING", C_YELLOW); }
        }
      }

    } else if (status == "delayed") {
      raceCancelled = false; currentTeamID = lastRaceTeamID;
      if (liveMode) {
        updateTeamDisplay(lastRaceTeamID, true);
        updateStatus("RAIN DELAY", C_YELLOW); updateInfoLine(); sendToNano(CMD_PULSE);
      }

    } else if (status == "cancelled") {
      raceCancelled = true; raceSunday = false; raceFinished = false;
      lightsOutTriggered = false; raceAnimationPlayedThisBoot = false;
      seasonAnimationPlayedThisBoot = false; isFinalRound = false; pendingWDC = false;
      currentTeamID = lastRaceTeamID;
      if (liveMode) {
        updateStatus("CANCELLED", C_RED); updateInfoLine(); drawMainScreen();
        if (lastGPName[0] != '\0') {
          tft.fillRect(8, 29, 148, 14, C_PANEL);
          tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(8, 33);
          char label[27], gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
          snprintf(label, sizeof(label), "Cancelled: %s", gpShort); tft.print(label);
        }
        sendToNano(CMD_DISPLAY);
      }

    } else if (status == "postponed") {
      raceCancelled = false; currentTeamID = lastRaceTeamID;
      if (liveMode) {
        updateTeamDisplay(lastRaceTeamID, true);
        updateStatus("POSTPONED", C_RED); updateInfoLine();
        if (lastGPName[0] != '\0') {
          tft.fillRect(8, 29, 148, 14, C_PANEL);
          tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(8, 33);
          char label[27], gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
          snprintf(label, sizeof(label), "Postponed: %s", gpShort); tft.print(label);
        }
      }

    } else if (status == "scheduled") {
      raceSunday = false; raceCancelled = false; raceFinished = false;
      currentTeamID = lastRaceTeamID;
      if (liveMode) {
        updateTeamDisplay(lastRaceTeamID, true);
        updateStatus("SCHEDULED", C_GREY); updateInfoLine();
      }

    } else if (status == "idle") {
      raceSunday = false; raceCancelled = false; raceFinished = false;
      currentTeamID = lastRaceTeamID;
      if (liveMode) {
        updateTeamDisplay(lastRaceTeamID, true);
        updateStatus("NO RACE", C_GREY); updateInfoLine();
      }
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.print("OK");
    client.flush();
    delay(1);
  }
  client.stop();
}

void loop() {
  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();
  if (getRacePhase() == 4) {
    if (raceFinished || raceSunday || lightsOutTriggered)
      raceFinished = raceSunday = raceCancelled = lightsOutTriggered = false;
    currentTeamID = lastRaceTeamID;
  }
  if (millis() - lastNTPSync > ntpResync) {
    lastNTPSync = millis();
    configTime(19800, 0, "pool.ntp.org");
  }
  if (millis() - lastClockUpdate > clockRefresh) {
    lastClockUpdate = millis();
    updateClock();
  }
  handleIncoming();
  delay(50);
}

void triggerRaceFinishAnimation(int teamID) {
  drawRaceFinishedScreen(teamID);
  sendToNano(CMD_CHECKERED);
  delay(9000);
  sendToNano(teamID);
  delay(500);
  raceAnimationPlayedThisBoot = true;
  flushPendingClients();
  drawMainScreen();
  updateTeamDisplay(teamID, true);
  updateStatus("FINISHED", C_GREEN);
}

void triggerSeasonEndCelebration(int champID) {
  if (seasonAnimationPlayedThisBoot) return;
  seasonAnimationPlayedThisBoot = true;
  for (int i = 0; i < 6; i++) {
    tft.fillScreen(C_GOLD); delay(180);
    tft.fillScreen(C_BLACK); delay(130);
  }
  drawSeasonEndScreen(champID);
  seasonScreenActive = true;
  sendToNano(champID); delay(300); sendToNano(CMD_CHECKERED);
}

void drawSeasonEndScreen(int champID) {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, 160, 16, C_GOLD);
  tft.setTextColor(C_BLACK); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("DRIVERS CHAMPION!");
  tft.fillRect(0, 18, 160, 55, teamTFTColor[champID]);
  tft.setTextColor(C_BLACK); tft.setTextSize(1); tft.setCursor(8, 23);
  char yearBuf[8]; time_t n = time(nullptr); struct tm* tm_ = localtime(&n);
  snprintf(yearBuf, sizeof(yearBuf), "%d", 1900 + tm_->tm_year);
  tft.print("WDC "); tft.print(yearBuf); tft.print(" CHAMP'S TEAM");
  tft.setTextSize(2);
  int nl = strlen(teamNames[champID]);
  int cx = (160 - nl * 12) / 2; if (cx < 4) cx = 4;
  tft.setCursor(cx, 34); tft.print(teamNames[champID]);
  tft.setTextSize(1);
  char gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
  char gpBuf[24]; snprintf(gpBuf, sizeof(gpBuf), "Final GP: %s", gpShort);
  tft.setCursor(8, 62); tft.print(gpBuf);
  tft.fillRect(0, 74, 160, 13, C_PANEL); tft.drawFastHLine(0, 74, 160, C_BORDER);
  tft.setTextColor(C_GOLD); tft.setTextSize(1); tft.setCursor(4, 78); tft.print("STATUS: CHAMPIONS!");
  tft.fillRect(0, 87, 160, 13, C_PANEL); tft.drawFastHLine(0, 87, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setCursor(4, 91); tft.print("Press button to dismiss");
  tft.fillRect(0, 100, 160, 13, C_PANEL); tft.drawFastHLine(0, 100, 160, C_BORDER);
  tft.setTextColor(C_YELLOW); tft.setCursor(4, 104); tft.print("SEASON COMPLETE!");
  tft.drawFastHLine(0, 113, 160, C_BORDER); tft.fillRect(0, 114, 160, 14, C_PANEL);
  tft.setTextColor(C_DIM); char rndBuf[20];
  snprintf(rndBuf, sizeof(rndBuf), "Rnd %d/%d FINAL", totalRounds, totalRounds);
  tft.setCursor(4, 118); tft.print(rndBuf);
}

void drawRaceFinishedScreen(int teamID) {
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, 0, 160, 16, C_WHITE); delay(200);
    tft.fillRect(0, 0, 160, 16, C_BLACK); delay(200);
  }
  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("RACE FINISHED!"); updateClock();
  tft.fillRect(0, 29, 160, 44, teamTFTColor[teamID]);
  tft.setTextColor(C_BLACK); tft.setTextSize(1); tft.setCursor(8, 33);
  char gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
  char label[27]; snprintf(label, sizeof(label), "WINNER | %s", gpShort); tft.print(label);
  tft.setTextSize(2); tft.setCursor(8, 45); tft.print(teamNames[teamID]);
  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.setTextColor(C_GREEN); tft.setTextSize(1); tft.setCursor(4, 103); tft.print("RACE COMPLETE");
}

void drawBootScreen() {
  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, 160, 22, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.setCursor(18, 5); tft.print("F1  LIVE");
  tft.fillRect(0, 106, 160, 22, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(32, 112); tft.print("LED CONTROLLER");
  tft.fillRect(60, 30, 40, 40, C_ORANGE); tft.fillRect(65, 35, 30, 30, C_BLACK);
  tft.setTextColor(C_ORANGE); tft.setTextSize(2); tft.setCursor(68, 40); tft.print("F1");
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(38, 82); tft.print("Booting...");
}

void drawStatusLine(const char* msg, uint16_t color) {
  tft.fillRect(0, 82, 160, 12, C_BLACK);
  tft.setTextColor(color); tft.setTextSize(1);
  int x = (160 - strlen(msg) * 6) / 2;
  tft.setCursor(x, 84); tft.print(msg);
  delay(300);
}

void drawMainScreen() {
  tft.fillScreen(C_BLACK);
  seasonScreenActive = false;
  tft.fillRect(0, 0, 160, 16, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("F1 LIVE");
  tft.setCursor(100, 4); tft.print("--:--:--");
  tft.fillRect(0, 16, 160, 12, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(4, 20);
  tft.print("Next GP: "); tft.setTextColor(C_WHITE);
  tft.print(raceName[0] != '\0' ? raceName : "TBC");
  tft.drawFastHLine(0, 28, 160, C_BORDER);
  tft.fillRect(0, 29, 4, 44, teamTFTColor[currentTeamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY) {
    tft.print(wdcConfirmed ? "WDC CHAMPION" : "DEFENDING WDC");
  } else if (currentMode == MODE_LIVE) {
    if (lastGPName[0] != '\0') {
      char label[27], gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
      snprintf(label, sizeof(label), "Last GP: %s", gpShort); tft.print(label);
    } else { tft.print("Last GP: ---"); }
  } else { tft.print("CONSTRUCTOR"); }
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[currentTeamID]); tft.setTextSize(2);
  tft.setCursor(8, 45); tft.print(teamNames[currentTeamID]);
  tft.fillRect(0, 73, 160, 13, C_PANEL); tft.drawFastHLine(0, 73, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(4, 77); tft.print("STATUS:");
  tft.setTextColor(C_GREEN); tft.setCursor(52, 77); tft.print(p1Status);
  tft.fillRect(0, 86, 160, 13, C_PANEL); tft.drawFastHLine(0, 86, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(4, 90); tft.print("MODE:");
  tft.setTextColor(C_WHITE); tft.setCursor(40, 90); tft.print(getModeName(currentMode));
  tft.fillRect(0, 99, 160, 13, C_PANEL); tft.drawFastHLine(0, 99, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(4, 103);
  if (seasonScreenActive) tft.print("SEASON COMPLETE!");
  else if (raceFinished)  tft.print("RACE COMPLETE");
  else if (raceCancelled) tft.print("RACE CANCELLED");
  else if (raceSunday)    tft.print("RACE IN PROGRESS");
  else                    tft.print("NO RACE TODAY");
  tft.drawFastHLine(0, 112, 160, C_BORDER);
  tft.fillRect(0, 113, 160, 15, C_PANEL);
  tft.setTextColor(C_DIM); tft.setCursor(4, 117);
  char rndBuf[16];
  snprintf(rndBuf, sizeof(rndBuf), "Rnd %d/%d",
           currentRound > 0 ? currentRound : 0, totalRounds);
  tft.print(rndBuf);
  tft.setCursor(90, 117);
  tft.print(piAvailable ? "Pi+ESPN" : "Jolpica");
}

void updateClock() {
  if (seasonScreenActive) return;
  time_t now = time(nullptr);
  tft.fillRect(96, 1, 63, 14, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1); tft.setCursor(98, 4);
  if (now > 100000) {
    struct tm* t = localtime(&now);
    char buf[9]; sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    tft.print(buf);
  } else {
    tft.print("--:--:--");
  }
}

void updateTeamDisplay(int teamID, bool isLive) {
  tft.fillRect(0, 29, 4, 44, teamTFTColor[teamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);
  tft.fillRect(8, 29, 148, 14, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1); tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY) {
    tft.print(wdcConfirmed ? "WDC CHAMPION" : "DEFENDING WDC");
  } else if (isLive) {
    char label[27], gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
    snprintf(label, sizeof(label), gpShort[0] != '\0' ? "Last GP: %s | P1:" : "P1:", gpShort);
    tft.print(label);
  } else { tft.print("CONSTRUCTOR"); }
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[teamID]); tft.setTextSize(2);
  tft.setCursor(8, 45); tft.print(teamNames[teamID]);
}

void updateStatus(const char* status, uint16_t color) {
  strncpy(p1Status, status, sizeof(p1Status) - 1);
  p1Status[sizeof(p1Status) - 1] = '\0';
  if (seasonScreenActive) return;
  tft.fillRect(52, 74, 106, 11, C_PANEL);
  tft.setTextColor(color); tft.setTextSize(1); tft.setCursor(52, 77); tft.print(status);
}

void updateModeDisplay() {
  if (seasonScreenActive) return;
  tft.fillRect(40, 87, 118, 11, C_PANEL);
  tft.setTextColor(C_WHITE); tft.setTextSize(1); tft.setCursor(40, 90); tft.print(getModeName(currentMode));
}

void handleRaceFinished(int teamID) {
  bool firstTime = !raceFinished;
  raceFinished = true; lastSentTeam = teamID;
  if (teamID >= 1 && teamID <= 11) {
    currentTeamID = teamID; lastRaceTeamID = teamID;
    EEPROM.write(EEPROM_LAST_RACE, teamID);
  }
  // Write race time only if not already set to a recent timestamp.
  // This prevents a Pi reconnect (days later) from refreshing the timestamp
  // and making raceFinishedRecently() return true for a week-old race.
  if (firstTime) {
    time_t stored;
    EEPROM.get(EEPROM_LAST_RACE_TIME, stored);
    time_t now = time(nullptr);
    bool alreadyRecent = (stored > 1700000000 && stored <= now &&
                          difftime(now, stored) < 43200);
    if (!alreadyRecent) {
      EEPROM.put(EEPROM_LAST_RACE_TIME, now);
    }
  }
  char shortGP[21] = {}; strncpy(shortGP, lastGPName, 20);
  char* g = strstr(shortGP, " Grand Prix"); if (g) *g = '\0';
  for (int i = 0; i < 20; i++) EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);
  EEPROM.commit();
  if (currentMode == MODE_LIVE) updateStatus("FINISHED", C_GREEN);
  if (firstTime && !raceAnimationPlayedThisBoot && currentMode == MODE_LIVE)
    triggerRaceFinishAnimation(teamID >= 1 ? teamID : lastRaceTeamID);
  else if (firstTime && currentMode == MODE_LIVE)
    updateTeamDisplay(teamID >= 1 ? teamID : lastRaceTeamID, true);
  if (isFinalRound && !wdcConfirmed) pendingWDC = true;
}

void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.persistent(false); WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
  WiFi.config(
    IPAddress(192,168,1,55),   // static IP
    IPAddress(192,168,1,1),    // gateway
    IPAddress(255,255,255,0),  // subnet
    IPAddress(192,168,1,1),    // DNS — router as DNS resolver
    IPAddress(8,8,8,8)         // DNS2 — Google fallback
  );
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED
    ? " Connected: " + WiFi.localIP().toString() : " FAILED");
}

void maintainWiFi() {
  if (millis() - lastWifiCheck > wifiRetry) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) { updateStatus("WiFi Lost", C_RED); connectWiFi(); }
  }
}

void sendToNano(int cmd) {
  if (!nanoReady) return;
  nanoSerial.print(cmd); nanoSerial.print('\n');
  Serial.printf("-> Nano: %d\n", cmd);
}

void checkButton() {
  bool state = digitalRead(BUTTON_PIN);
  if (lastButtonState == HIGH && state == LOW) {
    if (millis() - lastDebounce > 300) {
      lastDebounce = millis();
      if (seasonScreenActive) {
        seasonScreenActive = false; drawMainScreen(); applyMode(currentMode);
      } else {
        currentMode = (currentMode + 1) % MODE_COUNT;
        EEPROM.write(EEPROM_SAVED_MODE, currentMode); EEPROM.commit();
        applyMode(currentMode);
      }
    }
  }
  lastButtonState = state;
}

void applyMode(int mode) {
  if (seasonScreenActive) return;
  if (mode == MODE_DISPLAY) {
    sendToNano(CMD_DISPLAY);
    int d = wdcConfirmed && wdcTeamID >= 1 ? wdcTeamID : (int)EEPROM.read(EEPROM_WDC_CHAMP);
    if (d < 1 || d > 11) d = lastRaceTeamID;
    updateTeamDisplay(d, false);
    updateStatus(wdcConfirmed ? "CHAMPIONS!" : "DEFENDING", C_YELLOW);
    updateModeDisplay(); return;
  }
  if (mode == MODE_LIVE) {
    lastSentTeam = -1;
    if (currentTeamID < 1 || currentTeamID > 11) currentTeamID = lastRaceTeamID;
    updateTeamDisplay(currentTeamID, true); updateModeDisplay();
    sendToNano(currentTeamID); lastSentTeam = currentTeamID;
    if (getRacePhase() == 2 && !raceFinished) sendToNano(CMD_PULSE);
    if (raceFinished && pendingWDC)  updateStatus("WDC PENDING", C_YELLOW);
    else if (raceFinished)           updateStatus("FINISHED",    C_GREEN);
    else if (raceSunday)             updateStatus("LIVE",        C_GREEN);
    else if (raceCancelled)          updateStatus("CANCELLED",   C_RED);
    else                             updateStatus("NO RACE",     C_GREY);
    updateInfoLine(); return;
  }
  int teamID = mode - 1;
  sendToNano(teamID); updateTeamDisplay(teamID, false);
  updateStatus(teamNames[teamID], teamTFTColor[teamID]); updateModeDisplay();
}

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

void checkLightsOutCountdown() {
  if (!raceSunday || raceFinished || raceStartEpoch == 0) return;
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);
  if (!lightsOutTriggered && lastDiff > 0 && diff <= 0) {
    sendToNano(CMD_LIGHTS_OUT); lightsOutTriggered = true;
    for (int i = 0; i < 5; i++) { tft.fillScreen(C_RED); delay(150); tft.fillScreen(C_BLACK); delay(150); }
    drawMainScreen(); updateStatus("LIVE", C_GREEN);
  }
  lastDiff = diff;
}

bool raceFinishedRecently() {
  time_t stored; EEPROM.get(EEPROM_LAST_RACE_TIME, stored);
  time_t now = time(nullptr);
  if (stored < 1700000000 || stored > now) return false;
  return difftime(now, stored) < 43200;
}

bool seasonFinishedRecently() {
  time_t stored; EEPROM.get(EEPROM_SEASON_TIME, stored);
  time_t now = time(nullptr);
  if (stored < 1700000000 || stored > now) return false;
  return difftime(now, stored) < 43200;
}

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
  return 0;
}
