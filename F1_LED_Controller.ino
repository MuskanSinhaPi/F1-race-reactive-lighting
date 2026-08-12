// ============================================================
//  F1 NodeMCU — PRODUCTION (patched)
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
//  Bug fixes applied (original):
//    1. lastRaceTeamID/currentTeamID seeded from EEPROM before Jolpica
//       so warm white never appears even on Jolpica failure
//    2. NTP sync waits up to 30s; updateClock() shows -- until synced
//    3. drawMainScreen() then applyMode() — correct colour on boot
//       without needing button press
//
//  PATCH (this revision):
//    4. Jolpica JSON filter documents were undersized (128 / 64 bytes)
//       for the nested filter trees they build. ArduinoJson silently
//       truncates on overflow instead of erroring, so fields like
//       raceName/date/time/round/Constructor.name could come back
//       null even on a clean HTTP 200. Both bumped to 256 bytes.
//    5. Race-finish detection (animation, WDC pending flow) previously
//       depended 100% on a live push from the Pi. If the Pi never sent
//       "status=finished" (crash, WiFi drop, etc.), nothing downstream
//       ever fired — regardless of what Jolpica knew. Added
//       checkRaceFinishViaJolpica(), polled autonomously from loop()
//       during the post-race window, as a Pi-independent fallback that
//       triggers the same handleRaceFinished()/animation path.
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

const char* ssid     = "WiFi ssid";
const char* password = "wifi password";

unsigned long lastWifiCheck   = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastNTPSync     = 0;
const unsigned long wifiRetry    =  30000;
const unsigned long clockRefresh =   1000;
const unsigned long ntpResync    = 1800000;

// PATCH: autonomous Jolpica fallback timing
unsigned long lastJolpicaPoll        = 0;
const unsigned long jolpicaPollInterval = 120000; // 2 min, matches Pi's own poll cadence
int lastKnownFinishedRound = -1;

// PATCH: IST offset — DISPLAY ONLY now. The system clock (time(nullptr)) is
// kept in true UTC (see configTime(0,0,...) in setup()) so every countdown/
// phase comparison is timezone-agnostic — it works the same for a race in
// Austin, Suzuka, or Melbourne, since Jolpica always reports UTC. This
// constant is added only when rendering the on-screen wall clock / date,
// which should show local (IST) time for readability.
#define IST_OFFSET_SEC 19800  // UTC+5:30 — display purposes only

// PATCH: checkered-flag boot-replay eligibility (persisted in EEPROM so it
// survives multiple reboots within the window, not just RAM-lived per-boot).
const unsigned long ANIM_REPLAY_WINDOW_SEC = 21600; // 6 hours
bool bootFinishAnimEligible = false;

// PATCH: handles a WDC-confirmed push arriving before raceFinished is true
// — either because it's bundled in the SAME push as status=finished (which
// was being processed in the wrong order: celebration before the checkered
// flag), or because it genuinely arrives out of order on the network. The
// confirmation is captured here and only acted on once handleRaceFinished()
// actually runs, guaranteeing checkered-flag-then-gold-screen ordering
// either way, instead of silently dropping an early confirmation.
bool wdcConfirmationPending = false;
int pendingConfirmedTeamID = 0;

// PATCH: one-shot (with retry) refetch of the next race once the previous
// one is fully wrapped up (phase 4), so raceStartEpoch/raceName/currentRound
// don't stay pinned to a race that already happened.
bool nextRaceRefetched = false;
unsigned long lastNextRaceRefetchAttempt = 0;
const unsigned long nextRaceRefetchRetryInterval = 300000; // retry every 5 min until it succeeds
time_t raceFinishedAtEpoch = 0; // PATCH: independent of raceStartEpoch — see handleRaceFinished()

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
#define EEPROM_FINISH_ANIM_ROUND 33  // PATCH: which round's checkered-flag replay has already played
#define EEPROM_TOTAL_ROUNDS      34  // PATCH: persist season length across power cycles
#define EEPROM_MAGIC          0xA5
#define EEPROM_LAST_RACE_ROUND 35

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
void checkRaceFinishViaJolpica();   // PATCH: new
void checkWDCViaJolpica();          // PATCH: new
void finalizeWDCConfirmation(int cid); // PATCH: new
bool raceFinishAnimEligible(int round); // PATCH: new
void checkSeasonRollover(int newCompletedRound); // PATCH: new
void updateCountdown();             // PATCH: new
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

void fetchLastRaceFromJolpica(bool forRebootReplay) {
  drawStatusLine(
      forRebootReplay ? "Confirming result..." : "Fetching last GP...",
      C_YELLOW
  );

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient https;

  if (!https.begin(
      client,
      "https://api.jolpi.ca/ergast/f1/current/last/results.json?limit=1"
  )) {
    Serial.println("Jolpica last: begin() FAILED");
    return;
  }

  https.setTimeout(15000);
  https.setUserAgent("F1LiveController/1.0");
  https.addHeader("Accept-Encoding", "identity");

  int code = https.GET();

  Serial.printf("Jolpica last HTTP: %d\n", code);

  if (code <= 0) {
    Serial.printf(
      "Jolpica last error: %s\n",
      https.errorToString(code).c_str()
    );
    https.end();
    return;
  }

  String payload = https.getString();

  Serial.printf(
    "Jolpica last payload length: %d\n",
    payload.length()
  );

  Serial.println("----- JOLPICA LAST RESPONSE -----");
  Serial.println(payload);
  Serial.println("----- END RESPONSE -----");

  https.end();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Jolpica last: HTTP %d\n", code);
    return;
  }

  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf(
      "Jolpica last JSON ERROR: %s\n",
      err.c_str()
    );
    return;
  }

  Serial.println("Jolpica last JSON parsed OK");

  const char* con =
      doc["MRData"]["RaceTable"]["Races"][0]
         ["Results"][0]["Constructor"]["name"];

  const char* gpN =
      doc["MRData"]["RaceTable"]["Races"][0]["raceName"];

  const char* rndS =
      doc["MRData"]["RaceTable"]["Races"][0]["round"];

  Serial.printf("last P1    = %s\n", con  ? con  : "NULL");
  Serial.printf("last GP    = %s\n", gpN ? gpN : "NULL");
  Serial.printf("last round = %s\n", rndS ? rndS : "NULL");

  if (con) {
    int t = getTeamID(String(con));

    if (t >= 1 && t <= 11) {
      lastRaceTeamID = t;
      currentTeamID = t;

      EEPROM.write(EEPROM_LAST_RACE, t);

      Serial.printf(
        "Jolpica P1: %s\n",
        teamNames[t]
      );
    }
  }

  if (gpN) {
    strncpy(
      lastGPName,
      gpN,
      sizeof(lastGPName) - 1
    );

    lastGPName[sizeof(lastGPName) - 1] = '\0';

    char* g = strstr(lastGPName, " Grand Prix");
    if (g) *g = '\0';

    for (int i = 0; i < 20; i++) {
      EEPROM.write(
        EEPROM_LAST_GP_NAME + i,
        i < (int)strlen(lastGPName)
          ? lastGPName[i]
          : 0
      );
    }
  }

  if (rndS) {
    currentRound = atoi(rndS);
  }

      if (currentRound >= 1 && currentRound <= 40) {
        EEPROM.write(EEPROM_LAST_RACE_ROUND,
                     (uint8_t)currentRound);
    }
    
  EEPROM.commit();

  Serial.printf(
    "Jolpica: rnd %d P1=%s GP=%s\n",
    currentRound,
    teamNames[lastRaceTeamID],
    lastGPName
  );
}

//void fetchLastRaceFromJolpica(bool forRebootReplay) {
//  drawStatusLine(forRebootReplay ? "Confirming result..." : "Fetching last GP...", C_YELLOW);
//  WiFiClientSecure client;
//  client.setInsecure();
//  HTTPClient https;
//  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");
//  https.setTimeout(8000);
//  https.setUserAgent("F1LiveController/1.0"); // PATCH: courteous identification per Jolpica community norms
//  int code = https.GET();
//  if (code != 200) {
//    https.end();
//    Serial.printf("Jolpica last: HTTP %d — using EEPROM seed\n", code);
//    return;
//  }
//  // PATCH: 128 -> 256. The filter tree here is
//  // MRData{RaceTable{Races[0]{raceName, round, Results[0]{Constructor{name}}}}}
//  // which needs ~220 bytes to build without truncating. At 128 bytes,
//  // ArduinoJson silently drops keys once it runs out of room instead of
//  // erroring, so con/gpN/rndS could come back null on a clean HTTP 200.
//  JsonDocument filter;
//  filter["MRData"]["RaceTable"]["Races"][0]["raceName"]                          = true;
//  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;
//  filter["MRData"]["RaceTable"]["Races"][0]["round"]                             = true;
//  JsonDocument doc;
//  DeserializationError err = deserializeJson(
//    doc, https.getStream(), DeserializationOption::Filter(filter));
//  https.end();
//
//  if (err) { Serial.println("Jolpica last: parse err — using EEPROM seed"); return; }
//
//  const char* con   = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];
//  const char* gpN   = doc["MRData"]["RaceTable"]["Races"][0]["raceName"];
//  const char* rndS  = doc["MRData"]["RaceTable"]["Races"][0]["round"];
//  if (con) {
//    int t = getTeamID(String(con));
//    if (t >= 1 && t <= 11) {
//      lastRaceTeamID = t; currentTeamID = t;
//      EEPROM.write(EEPROM_LAST_RACE, t);
//      Serial.printf("Jolpica P1: %s\n", teamNames[t]);
//    }
//  }
//  if (gpN) {
//    strncpy(lastGPName, gpN, sizeof(lastGPName) - 1);
//    lastGPName[sizeof(lastGPName) - 1] = '\0';
//    char* g = strstr(lastGPName, " Grand Prix"); if (g) *g = '\0';
//    for (int i = 0; i < 20; i++)
//      EEPROM.write(EEPROM_LAST_GP_NAME + i, i < (int)strlen(lastGPName) ? lastGPName[i] : 0);
//  }
//  if (rndS) currentRound = atoi(rndS);
//  EEPROM.commit();
//  Serial.printf("Jolpica: rnd %d P1=%s GP=%s\n", currentRound, teamNames[lastRaceTeamID], lastGPName);
//}

void fetchSeasonTotal() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current.json?limit=1");
  https.setTimeout(15000);
  https.setUserAgent("F1LiveController/1.0");
  https.addHeader("Accept-Encoding", "identity"); // PATCH: courteous identification per Jolpica community norms
  int code = https.GET();
  if (code != 200) { https.end(); Serial.printf("Season total: HTTP %d\n", code); return; }
  JsonDocument filter;
  filter["MRData"]["total"] = true;
  JsonDocument doc;
//  deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
    DeserializationError err =
      deserializeJson(
          doc,
          https.getStream(),
          DeserializationOption::Filter(filter)
      );
  
  https.end();
  
  if (err) {
      Serial.printf("Season total JSON ERROR: %s\n",
                    err.c_str());
      return;
  }

  const char* totS = doc["MRData"]["total"];
  if (totS) {
    totalRounds = atoi(totS);
    // PATCH: persist so a fetch failure right after a power cycle doesn't
    // silently fall back to the hardcoded default of 24.
    if (totalRounds >= 1 && totalRounds <= 250) {
      EEPROM.write(EEPROM_TOTAL_ROUNDS, (uint8_t)totalRounds);
      EEPROM.commit();
    }
    Serial.printf("Season total: %d rounds\n", totalRounds);
  }
}
void fetchNextRaceFromJolpica() {
  drawStatusLine("Fetching next GP...", C_YELLOW);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient https;

  if (!https.begin(client,
      "https://api.jolpi.ca/ergast/f1/current/next.json")) {
    Serial.println("Jolpica next: begin() FAILED");
    return;
  }

  https.setTimeout(15000);
  https.setUserAgent("F1LiveController/1.0");

  // IMPORTANT: prevent gzip/compressed response
  https.addHeader("Accept-Encoding", "identity");

  int code = https.GET();

  Serial.printf("Jolpica next HTTP: %d\n", code);

  if (code <= 0) {
    Serial.printf("Jolpica next error: %s\n",
                  https.errorToString(code).c_str());
    https.end();
    return;
  }

  String payload = https.getString();

  Serial.printf("Jolpica payload length: %d\n", payload.length());
  Serial.println("----- JOLPICA RESPONSE -----");
  Serial.println(payload);
  Serial.println("----- END RESPONSE -----");

  https.end();

  if (code != HTTP_CODE_OK) {
    Serial.printf("Jolpica next: HTTP %d\n", code);
    return;
  }

  JsonDocument doc;

  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf("Jolpica JSON ERROR: %s\n", err.c_str());
    return;
  }

  Serial.println("Jolpica JSON parsed OK");

  const char* name =
      doc["MRData"]["RaceTable"]["Races"][0]["raceName"];

  const char* date =
      doc["MRData"]["RaceTable"]["Races"][0]["date"];

  const char* rtime =
      doc["MRData"]["RaceTable"]["Races"][0]["time"];

  const char* rndS =
      doc["MRData"]["RaceTable"]["Races"][0]["round"];

  Serial.printf("name  = %s\n", name  ? name  : "NULL");
  Serial.printf("date  = %s\n", date  ? date  : "NULL");
  Serial.printf("time  = %s\n", rtime ? rtime : "NULL");
  Serial.printf("round = %s\n", rndS  ? rndS  : "NULL");

  if (name) {
    strncpy(raceName, name, sizeof(raceName) - 1);
    raceName[sizeof(raceName) - 1] = '\0';

    char* g = strstr(raceName, " Grand Prix");
    if (g) *g = '\0';
  }

  if (date && rtime) {
    raceStartEpoch = parseUTCEpoch(date, rtime);

    lastDiff = difftime(raceStartEpoch, time(nullptr));
    lightsOutTriggered = false;
    raceAnimationPlayedThisBoot = false;
  }

  if (rndS) {
    currentRound = atoi(rndS);
  }

  Serial.printf("Next: %s rnd %d/%d\n",
                raceName,
                currentRound,
                totalRounds);
}
//void fetchNextRaceFromJolpica() {
//  drawStatusLine("Fetching next GP...", C_YELLOW);
//  WiFiClientSecure client;
//  client.setInsecure();
//  HTTPClient https;
//  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");
//  https.setTimeout(8000);
//  https.setUserAgent("F1LiveController/1.0"); // PATCH: courteous identification per Jolpica community norms
//  int code = https.GET();
//  if (code != 200) { https.end(); Serial.printf("Jolpica next: HTTP %d\n", code); return; }
//  // PATCH: 64 -> 256. This filter tree (raceName, date, time, round under
//  // Races[0], plus top-level total) needs ~180 bytes minimum. At 64 bytes
//  // it was overflowing and silently dropping fields — most likely why
//  // date/time/round/total came back missing or stale on non-race days.
//  JsonDocument filter;
//  filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
//  filter["MRData"]["RaceTable"]["Races"][0]["date"]     = true;
//  filter["MRData"]["RaceTable"]["Races"][0]["time"]     = true;
//  filter["MRData"]["RaceTable"]["Races"][0]["round"]    = true;
//  filter["MRData"]["total"]                             = true;
//  JsonDocument doc;
//  DeserializationError err = deserializeJson(
//    doc, https.getStream(), DeserializationOption::Filter(filter));
//  https.end();
//  if (err) { Serial.println("Jolpica next: parse err"); return; }
//  const char* name  = doc["MRData"]["RaceTable"]["Races"][0]["raceName"];
//  const char* date  = doc["MRData"]["RaceTable"]["Races"][0]["date"];
//  const char* rtime = doc["MRData"]["RaceTable"]["Races"][0]["time"];
//  const char* rndS  = doc["MRData"]["RaceTable"]["Races"][0]["round"];
//  const char* totS  = doc["MRData"]["total"];
//  if (name) {
//    strncpy(raceName, name, sizeof(raceName) - 1);
//    raceName[sizeof(raceName) - 1] = '\0';
//    char* g = strstr(raceName, " Grand Prix"); if (g) *g = '\0';
//  }
//  // PATCH: raceStartEpoch stays pure true-UTC (straight from Jolpica's UTC
//  // date/time string) with NO offset applied. The system clock is now also
//  // kept in true UTC (see configTime(0,0,...) in setup()), so this
//  // comparison is timezone-agnostic for both the user's local time AND the
//  // race's own local time — it works identically for Austin, Suzuka, or
//  // Silverstone since Jolpica always reports UTC.
//  if (date && rtime) {
//    raceStartEpoch = parseUTCEpoch(date, rtime);
//    // PATCH: reseed the lights-out crossing-detector and re-arm it for this
//    // (new) race. Without this, lastDiff kept its stale 999999 sentinel (or
//    // a value from the previous race) across reboots, so a device rebooted
//    // mid-race would see lastDiff > threshold on its very first check and
//    // immediately replay the lights-out sequence even though the race had
//    // already started.
//    lastDiff = difftime(raceStartEpoch, time(nullptr));
//    lightsOutTriggered = false;
//    // PATCH: previously only reset on a "cancelled" push, meaning the
//    // checkered-flag animation would play for the first race after power-on
//    // and then never again for any later race that same session. A fresh
//    // raceStartEpoch means a genuinely new race — re-arm it here too.
//    raceAnimationPlayedThisBoot = false;
//  }
//  if (rndS) currentRound = atoi(rndS) - 1;
//  // Don't use totS — "total" in next.json is count of results (always 1),
//  // not the season length. Fetch season total separately.
//  Serial.printf("Next: %s rnd %d/%d\n", raceName, currentRound + 1, totalRounds);
//}
//
//// PATCH: new function. Autonomous, Pi-independent confirmation that the
//// last race has finished. Polled from loop() during the post-race window.
//// Without this, the checkered-flag animation / WDC-pending flow / final
//// WDC-confirmed flow ALL depended entirely on the Pi successfully pushing
//// "status=finished" — if the Pi crashed, lost WiFi, or ESPN scraping
//// failed, nothing downstream ever fired even though Jolpica had already
//// posted the result.
void checkRaceFinishViaJolpica() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json?limit=1");
  https.setTimeout(15000);
  https.setUserAgent("F1LiveController/1.0");
  https.addHeader("Accept-Encoding", "identity"); // PATCH: courteous identification per Jolpica community norms
  int code = https.GET();
  if (code != 200) {
    https.end();
    Serial.printf("Auto-check: HTTP %d\n", code);
    return;
  }

  JsonDocument filter;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["round"]                             = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.println("Auto-check: parse err"); return; }

  const char* con  = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];
  const char* rndS = doc["MRData"]["RaceTable"]["Races"][0]["round"];
  if (!con || !rndS) return;

  int finishedRound = atoi(rndS);
  int teamID = getTeamID(String(con));

  // Only act if this is a result we haven't already processed AND the Pi
  // hasn't already told us the race finished (avoid double-firing the
  // animation if the Pi push and this poll land close together).
  if (finishedRound != lastKnownFinishedRound && teamID >= 1 && !raceFinished) {
    // PATCH: this was the one call site I'd planned to add this to but
    // never actually did — and it's the most important of the three,
    // since it's the only rollover-detection point that doesn't require a
    // reboot. A device left running continuously across the off-season
    // would otherwise never catch the new season starting, and wdcConfirmed
    // would stay stuck true from last year, permanently blocking this
    // season's isFinalRound/pendingWDC.
    checkSeasonRollover(finishedRound);
    lastKnownFinishedRound = finishedRound;
    // PATCH: previously only set via a Pi push's wdc_team param — meaning
    // a Pi outage during the season finale would confirm the race winner
    // here but silently skip pendingWDC/"WDC PENDING" entirely, since that
    // state is gated on isFinalRound. Mirror PATH B's own check.
    if (!wdcConfirmed && finishedRound >= totalRounds) isFinalRound = true;
    Serial.printf("Auto-check: Jolpica confirms round %d finished, P1=%s (no Pi push seen)\n",
                  finishedRound, teamNames[teamID]);
    if (currentMode == MODE_LIVE) updateTeamDisplay(teamID, true);
    handleRaceFinished(teamID);
    if (currentMode == MODE_LIVE) updateInfoLine();
  } else if (finishedRound != lastKnownFinishedRound) {
    // PATCH: this branch is reached when the team name failed to parse
    // (raceFinished is guaranteed false here by the caller's own gating) —
    // still need the rollover check, since a new season can start even if
    // we couldn't identify the winning constructor this particular poll.
    checkSeasonRollover(finishedRound);
    lastKnownFinishedRound = finishedRound; // stay in sync even if no action taken
  }
}

// PATCH: new helper. A race-finish is eligible for the checkered-flag
// boot/toggle replay only within 6h of the actual finish AND only if we
// haven't already shown it for this specific round (persisted in EEPROM
// so repeated reboots within the window don't replay it over and over).
// PATCH: new function. Resets per-season WDC state when a season rollover
// is detected (the newly-learned "last completed round" is numerically
// LESS than the one we knew about — e.g. round 1 of a new season arriving
// after round 24 of the old one). Without this, once a season's WDC is
// confirmed, wdcConfirmed would stay true forever (nothing else resets it),
// silently blocking isFinalRound/pendingWDC/checkWDCViaJolpica() for every
// subsequent season — the same root bug as the boot-time one, just
// recurring annually instead of on every reboot.
void checkSeasonRollover(int newCompletedRound) {
  if (newCompletedRound < lastKnownFinishedRound) {
    wdcConfirmed = false;
    isFinalRound = false;
    pendingWDC = false;
    Serial.println("Season rollover detected — WDC state reset for new season");
  }
}

bool raceFinishAnimEligible(int round) {
  time_t stored; EEPROM.get(EEPROM_LAST_RACE_TIME, stored);
  time_t now = time(nullptr);
  if (stored < 1700000000 || stored > now) return false;
  if (difftime(now, stored) >= ANIM_REPLAY_WINDOW_SEC) return false;
  int playedRound = EEPROM.read(EEPROM_FINISH_ANIM_ROUND);
  return playedRound != round;
}

// PATCH: new function. Closes the last Pi-dependency gap: WDC *confirmation*
// (as opposed to just detecting that the final race finished) previously
// only ever happened via a Pi push's wdc_team/wdc_status=confirmed params.
// If the Pi was down through the whole finale, isFinalRound/pendingWDC would
// correctly show "WDC PENDING" (via checkRaceFinishViaJolpica) but the gold
// champion screen would never fire. Once the season is actually over, the
// P1 entry in Jolpica's current driver standings IS the champion (barring a
// rare post-hoc penalty/appeal), so we can confirm it the same way.
// NOTE: not verified against a live response — confirm the field path
// (StandingsLists[0].DriverStandings[0].Constructors[0].name) once you can
// test against api.jolpi.ca, since I can't reach that domain from here.
// PATCH: new function. Extracted so the exact same confirm-and-celebrate
// logic can be reached from three places: checkWDCViaJolpica() (below),
// the Pi's live confirmed push, and a deferred confirmation that arrived
// before the race-finished push was processed (see wdcConfirmationPending).
void finalizeWDCConfirmation(int cid) {
  wdcTeamID = cid; wdcConfirmed = true; pendingWDC = false;
  EEPROM.write(EEPROM_WDC_CHAMP, cid);
  time_t nowT = time(nullptr);
  if (nowT > 1700000000) EEPROM.put(EEPROM_SEASON_TIME, nowT);
  EEPROM.commit();
  Serial.printf("WDC confirmed: %s\n", teamNames[cid]);
  if (!seasonAnimationPlayedThisBoot) { delay(1500); triggerSeasonEndCelebration(cid); }
}

void checkWDCViaJolpica() {
  if (wdcConfirmed || !isFinalRound || !raceFinished) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/driverStandings.json");
  https.setTimeout(15000);
  https.setUserAgent("F1LiveController/1.0");
  https.addHeader("Accept-Encoding", "identity"); // PATCH: courteous identification per Jolpica community norms
  int code = https.GET();
  if (code != 200) { https.end(); Serial.printf("WDC check: HTTP %d\n", code); return; }

  JsonDocument filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.println("WDC check: parse err"); return; }

  const char* con = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"];
  if (!con) return;
  int cid = getTeamID(String(con));
  if (cid < 1 || cid > 11) return;

  Serial.println("Auto-check: WDC confirmed via Jolpica standings (no Pi push seen)");
  finalizeWDCConfirmation(cid);
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

  // PATCH: kept in true UTC now — no gmtOffset. The ESP8266 core's time()
  // would otherwise return "local" time once configTime() is given a
  // non-zero offset, which silently broke every countdown/phase comparison
  // against raceStartEpoch (also true UTC). IST_OFFSET_SEC is now applied
  // only at display time (updateClock(), drawSeasonEndScreen()).
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
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
    EEPROM.write(EEPROM_FINISH_ANIM_ROUND, 255); // PATCH: 255 = "no round played yet" sentinel
    EEPROM.write(EEPROM_TOTAL_ROUNDS, 24);        // PATCH: sane default season length
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
    // PATCH: seed totalRounds too, so a fetch failure right after a power
    // cycle doesn't fall back to the hardcoded default of 24.
    int savedTotalRounds = EEPROM.read(EEPROM_TOTAL_ROUNDS);
    if (savedTotalRounds >= 10 && savedTotalRounds <= 40) totalRounds = savedTotalRounds;
    Serial.printf("EEPROM seed: %s | %s\n", teamNames[lastRaceTeamID], lastGPName);
  }

  // PATH A: season end within 12h
  if (savedWDC >= 1 && savedWDC <= 11 && seasonFinishedRecently()) {
    wdcTeamID = savedWDC; wdcConfirmed = true;
    lastRaceTeamID = savedWDC; currentTeamID = savedWDC;
    nanoReady = true;
    fetchSeasonTotal();
    // PATCH: this had the same currentRound-clobbering bug as PATH B/C —
    // except worse here, since currentRound is never seeded before this
    // point in PATH A at all (still at its -1 default), so
    // lastKnownFinishedRound would get set to garbage, silently breaking
    // checkSeasonRollover()'s comparison for the *next* season. PATH A
    // only ever runs when the season has just ended, so the completed
    // round is definitionally the final one — no need to derive it from
    // whatever fetchNextRaceFromJolpica() happens to leave in currentRound.
    lastKnownFinishedRound = totalRounds;
    isFinalRound = true;
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
    // PATCH: capture the completed round BEFORE fetchNextRaceFromJolpica()
    // runs — that call overwrites currentRound with (next race's round - 1),
    // which is wrong once the season rolls over (next race becomes next
    // season's round 1, silently zeroing out our "is this the final round"
    // check below). This was a latent bug independent of everything else.
    fetchSeasonTotal();
    int lastCompletedRound = currentRound;
    // PATCH: reset per-season WDC state if this boot reveals we've rolled
    // into a new season (round number went backwards) — see
    // checkSeasonRollover() for why this matters.
    checkSeasonRollover(lastCompletedRound);
    fetchNextRaceFromJolpica();
    lastKnownFinishedRound = lastCompletedRound;
    currentTeamID = lastRaceTeamID;
    raceFinished  = true;
    nanoReady     = true;
    // PATCH: this boot path sets raceFinished directly (not via
    // handleRaceFinished()), so the loop()-level cleanup/retry logic — which
    // now keys off raceFinishedAtEpoch instead of getRacePhase() — needs it
    // seeded here too. Use the actual stored finish time (not "now"), since
    // we may be booting hours after the race actually ended.
    EEPROM.get(EEPROM_LAST_RACE_TIME, raceFinishedAtEpoch);
    // raceStartEpoch starts at 0 every boot, so ">0" here reliably means
    // the fetch just above actually succeeded.
    nextRaceRefetched = (raceStartEpoch > 0);
    // PATCH: previously this set wdcConfirmed=true here just because a past
    // champion was known in EEPROM — but PATH B fires for ANY recent race
    // finish, not just the finale. That made wdcConfirmed true on ~every
    // boot for the rest of the device's life (nothing ever set it back to
    // false), which permanently blocked isFinalRound/pendingWDC from being
    // set, blocked the Pi's own "confirmed" push, and made
    // checkWDCViaJolpica() dead code. wdcTeamID still gets seeded for
    // display purposes; applyMode() already has its own EEPROM fallback for
    // the defending-champion color regardless of wdcConfirmed's value.
    if (savedWDC >= 1 && savedWDC <= 11) wdcTeamID = savedWDC;
    if (!wdcConfirmed && lastCompletedRound >= totalRounds) { pendingWDC = true; isFinalRound = true; }

    // PATCH: replay the checkered-flag animation on a reboot landing within
    // 6h of the finish — once per race (EEPROM-tracked so repeated reboots
    // in the window don't replay it), and only if we're in/entering LIVE
    // mode. If we boot into a different mode, defer to checkButton()'s
    // first manual toggle into LIVE (see there).
    bootFinishAnimEligible = raceFinishAnimEligible(lastCompletedRound);
    if (bootFinishAnimEligible && currentMode == MODE_LIVE) {
      triggerRaceFinishAnimation(lastRaceTeamID);
      EEPROM.write(EEPROM_FINISH_ANIM_ROUND, (uint8_t)lastCompletedRound);
      EEPROM.commit();
      bootFinishAnimEligible = false;
    } else {
      drawMainScreen();
      updateTeamDisplay(lastRaceTeamID, true);
      updateStatus(pendingWDC ? "WDC PENDING" : "FINISHED",
                   pendingWDC ? C_YELLOW      : C_GREEN);
      updateInfoLine();
    }
    sendToNano(lastRaceTeamID);
    applyMode(currentMode);
    return;
  }

  // PATH C: normal boot
  // 1.5s delay — first HTTPS call after boot needs extra heap settling time
  delay(1500);
  fetchLastRaceFromJolpica(false);
  fetchSeasonTotal();
  // PATCH: capture before fetchNextRaceFromJolpica() can clobber it (same
  // bug as PATH B — see comment there).
  int lastCompletedRound = currentRound;
  checkSeasonRollover(lastCompletedRound); // PATCH: reset per-season WDC state on rollover
  fetchNextRaceFromJolpica();
  lastKnownFinishedRound = lastCompletedRound;
  // PATCH: no longer sets wdcConfirmed=true here — see the matching comment
  // in PATH B for why that was wrong (this fires on every normal boot, not
  // just at the actual finale, and nothing ever reset it back to false).
  if (savedWDC >= 1 && savedWDC <= 11) wdcTeamID = savedWDC;
  // PATCH: previously missing entirely from this path — a full power-off
  // through an entire race weekend (not just a Pi outage) lands here, not
  // PATH B, so isFinalRound/pendingWDC were silently never evaluated and
  // the WDC-pending flow could be skipped even though the season had
  // actually just ended.
  if (!wdcConfirmed && lastCompletedRound >= totalRounds) { pendingWDC = true; isFinalRound = true; }
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
    // PATCH: this ran BEFORE the status=="finished" branch further down —
    // so a single combined push (status=finished&wdc_status=confirmed,
    // which is exactly how the Pi would send the actual title-deciding
    // push) fired the gold season-end celebration BEFORE the checkered
    // flag animation even started, reversing the intended sequence. Now
    // deferred to handleRaceFinished() (which runs after this) whenever
    // raceFinished isn't already true — that covers both the same-push
    // case and a confirmation that genuinely arrives before the
    // finished push on the network.
    if (wdcTeamS.length() > 0 && wdcStatS == "confirmed" && !wdcConfirmed) {
      int cid = getTeamID(wdcTeamS);
      if (cid >= 1 && cid <= 11) {
        if (raceFinished) {
          finalizeWDCConfirmation(cid);
        } else {
          wdcConfirmationPending = true;
          pendingConfirmedTeamID = cid;
          Serial.println("WDC confirmed push received before race-finished — deferring celebration");
        }
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
      raceSunday = true;          // ← was false; race IS today
      raceCancelled = false;
      raceFinished  = false;

      // Seed current team from Pi if provided (handles Mercedes→McLaren case)
      if (teamID >= 1) {
        currentTeamID = teamID;
      } else {
        currentTeamID = lastRaceTeamID;
      }

    } else if (status == "idle") {
      raceSunday = false; raceCancelled = false; raceFinished = false;
      currentTeamID = lastRaceTeamID;           // keep last known colour
      if (liveMode) {
        updateTeamDisplay(lastRaceTeamID, true);
        updateStatus("NO RACE", C_GREY);
        updateInfoLine();
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

  // ── Autonomous race-day detection ─────────────────────────────────────────
  // Do NOT depend solely on Pi push to set raceSunday.
  // Phase 1 = pre-race window (≤6h before), Phase 2 = race window (≤2h after).
  // Phase 3 = post-race cooldown for the CURRENT/ongoing race. Correct as
  // long as raceFinished is still false — see the PATCH note below for why
  // phase can no longer be used once a race has actually finished.
  int phase = getRacePhase();

  if (phase == 1 || phase == 2) {
    if (!raceSunday && !raceFinished) {
      raceSunday = true;
      Serial.println("Auto raceSunday = true (phase " + String(phase) + ")");
    }
  }

  // PATCH: post-finish cleanup, the next-race refetch retry, and WDC polling
  // used to all be driven by getRacePhase()==4. That broke the moment we
  // added the "immediate refetch on finish" fix (handleRaceFinished() now
  // moves raceStartEpoch to the NEXT race right away, so the countdown
  // resets instantly) — getRacePhase() reads phase 0 almost immediately
  // after a finish, and phase 4 (checking the OLD race) is never reached
  // again. All three are now driven by raceFinishedAtEpoch instead, which
  // is set once, independently, at the actual moment of finish.
  // PATCH: guarded so the finale doesn't get auto-cleared while WDC
  // confirmation is still pending — without this, checkWDCViaJolpica()'s
  // polling window (which requires raceFinished) would silently die after
  // 12h even if the championship genuinely hasn't been confirmed yet
  // (e.g. a delayed stewards' decision).
  if (raceFinished && raceFinishedAtEpoch > 0 &&
      difftime(time(nullptr), raceFinishedAtEpoch) > 43200 &&
      !(isFinalRound && !wdcConfirmed)) {
    raceFinished = raceSunday = raceCancelled = lightsOutTriggered = false;
    currentTeamID = lastRaceTeamID;
  }

  if (raceFinished && !nextRaceRefetched &&
      millis() - lastNextRaceRefetchAttempt > nextRaceRefetchRetryInterval) {
    lastNextRaceRefetchAttempt = millis();
    time_t epochBefore = raceStartEpoch;
    fetchNextRaceFromJolpica();
    fetchSeasonTotal();
    if (raceStartEpoch > 0 && raceStartEpoch != epochBefore) {
      nextRaceRefetched = true;
      drawMainScreen();
      if (currentMode == MODE_LIVE) applyMode(currentMode);
      Serial.println("Next race refetched (retry path)");
    }
    // else: fetch failed or returned the same epoch, will retry in 5 min
  }

  // ── Autonomous Jolpica fallback for race-finish + WDC confirmation ───────
  // Race-finish detection still keys off phase — correct, since it only
  // runs while !raceFinished, meaning raceStartEpoch still points at the
  // CURRENT/ongoing race (it hasn't been moved yet). WDC confirmation is
  // fully phase-independent, since by the time we're waiting on it,
  // raceStartEpoch already points at a future or off-season race.
  bool inFallbackWindow = (phase == 2 || phase == 3) && !raceFinished;
  bool inWDCWindow      = isFinalRound && raceFinished && !wdcConfirmed;
  if ((inFallbackWindow || inWDCWindow) &&
      millis() - lastJolpicaPoll > jolpicaPollInterval) {
    lastJolpicaPoll = millis()- jolpicaPollInterval;
    if (inFallbackWindow) checkRaceFinishViaJolpica();
    else if (inWDCWindow) checkWDCViaJolpica();
  }

  // ── Continuous pulse during live race ─────────────────────────────────────
  if (raceSunday && !raceFinished && currentMode == MODE_LIVE) {
    static unsigned long lastPulse = 0;
    if (millis() - lastPulse > 2500) {
      sendToNano(CMD_PULSE);
      lastPulse = millis();
    }
  }

  if (millis() - lastNTPSync > ntpResync) {
    lastNTPSync = millis();
    configTime(0, 0, "pool.ntp.org"); // PATCH: true UTC, matches the initial sync
  }
  if (millis() - lastClockUpdate > clockRefresh) {
    lastClockUpdate = millis();
    updateClock();
    updateCountdown(); // PATCH: live d/h/m/s countdown to next race
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
  // PATCH: removed flushPendingClients() here. It indiscriminately called
  // .stop() on any TCP client that had queued up during the preceding
  // 9.5s of blocking delay() — WITHOUT reading its request first. A Pi
  // push arriving during the animation (e.g. the actual wdc_status=
  // confirmed push, timed to land right after a finish) would be silently
  // discarded instead of processed, permanently losing that confirmation
  // (nothing re-sends it). handleIncoming() will pick up any queued
  // connection normally on the next loop() pass — no flush needed.
  drawMainScreen();
  updateTeamDisplay(teamID, true);
  // PATCH: previously always hardcoded "FINISHED" here, relying on a
  // follow-up correction in handleIncoming() to fix it to "WDC PENDING"
  // during a season finale. That correction only existed in the Pi-push
  // path — none of the autonomous paths (Jolpica fallback, boot replay,
  // deferred toggle replay) went through it, so they'd briefly show the
  // wrong status. Checking pendingWDC here makes this self-sufficient.
  updateStatus(pendingWDC ? "WDC PENDING" : "FINISHED",
               pendingWDC ? C_YELLOW      : C_GREEN);
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
  char yearBuf[8];
  time_t n = time(nullptr) + IST_OFFSET_SEC; // PATCH: display-only offset
  struct tm* tm_ = gmtime(&n);
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
  if (currentRound >= 1) {
      snprintf(rndBuf, sizeof(rndBuf),
               "Rnd %d/%d",
               currentRound,
               totalRounds);
  } else {
      snprintf(rndBuf, sizeof(rndBuf),
               "Rnd --/%d",
               totalRounds);
  }
  tft.print(rndBuf);
  updateCountdown(); // PATCH: live d/h/m/s countdown, replaces the old static source tag
}

void updateClock() {
  if (seasonScreenActive) return;
  time_t now = time(nullptr); // true UTC — system clock (see setup())
    if (now < 100000) {
      tft.print("--:--:--");
      return;
  }
  tft.fillRect(96, 1, 63, 14, C_RED);
  tft.setTextColor(C_WHITE); tft.setTextSize(1); tft.setCursor(98, 4);
  if (now > 100000) {
    time_t displayTime = now + IST_OFFSET_SEC; // PATCH: display-only offset
    struct tm* t = gmtime(&displayTime);        // gmtime: no further zone shift
    char buf[9]; sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    tft.print(buf);
  } else {
    tft.print("--:--:--");
  }
}

// PATCH: new function. Live countdown to raceStartEpoch in d/h/m/s, drawn
// in the footer's right-hand slot (previously the static "Pi+ESPN"/
// "Jolpica" source tag). Purely local arithmetic on raceStartEpoch — no
// network call — so it works every second without "checking online",
// and it's timezone-agnostic since both sides of the diff are true UTC.
void updateCountdown() {
  if (seasonScreenActive) return;
  tft.fillRect(64, 116, 96, 9, C_PANEL);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(64, 117);
  if (raceStartEpoch == 0) { tft.print("TBC"); return; }
  time_t now = time(nullptr);
  double diff = difftime(raceStartEpoch, now);
  if (diff <= 0) { tft.print("LIVE NOW"); return; }
  long totalSec = (long)diff;
  long d = totalSec / 86400;
  long h = (totalSec % 86400) / 3600;
  long m = (totalSec % 3600) / 60;
  long s = totalSec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%ldd %02ld:%02ld:%02ld", d, h, m, s);
  tft.print(buf);
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
  if (firstTime) {
    bootFinishAnimEligible = false; // a new finish supersedes any pending boot-replay
    // PATCH: captured independently of raceStartEpoch, which is about to
    // get moved to the NEXT race a few lines down. Everything that needs
    // to know "how long since THIS race finished" (the 12h cleanup, the
    // refetch retry) now reads this instead of getRacePhase().
    raceFinishedAtEpoch = time(nullptr);
    nextRaceRefetched = false; // re-arm the retry mechanism for this new finish
  }
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
  // PATCH: compute pendingWDC BEFORE setting status or triggering the
  // animation. It was previously computed after both, meaning the direct
  // updateStatus("FINISHED",...) call right below, AND
  // triggerRaceFinishAnimation()'s own pendingWDC check, would both see
  // the stale (pre-update) value on a race's actual first finish — i.e.
  // exactly the case that matters most.
  if (isFinalRound && !wdcConfirmed) pendingWDC = true;

  if (currentMode == MODE_LIVE)
    updateStatus(pendingWDC ? "WDC PENDING" : "FINISHED",
                 pendingWDC ? C_YELLOW      : C_GREEN);
  if (firstTime && !raceAnimationPlayedThisBoot && currentMode == MODE_LIVE)
    triggerRaceFinishAnimation(teamID >= 1 ? teamID : lastRaceTeamID);
  else if (firstTime && currentMode == MODE_LIVE)
    updateTeamDisplay(teamID >= 1 ? teamID : lastRaceTeamID, true);

  // PATCH: the checkered-flag animation (if any) has now fully played, and
  // raceFinished is true — safe to act on a confirmation that arrived
  // before we got here, guaranteeing checkered-flag-then-gold-screen
  // ordering no matter which order the Pi's pushes actually arrived in.
  if (wdcConfirmationPending) {
    wdcConfirmationPending = false;
    finalizeWDCConfirmation(pendingConfirmedTeamID);
  }

  // PATCH: refetch the next race immediately so the countdown "resets"
  // right when we learn the race is over, instead of waiting for a
  // phase-based transition (raceStartEpoch is about to change, so phase
  // checks against it are no longer meaningful for "time since finish").
  if (firstTime) {
    time_t epochBefore = raceStartEpoch;
    fetchNextRaceFromJolpica();
    fetchSeasonTotal();
    // PATCH: previously set unconditionally, so a failed fetch here (e.g.
    // transient network hiccup right at race end) would falsely mark the
    // refetch as done and the loop() retry mechanism would never fire.
    // Only count it as done if we actually got a new epoch.
    nextRaceRefetched = (raceStartEpoch > 0 && raceStartEpoch != epochBefore);
    // PATCH: this is the critical gap — handleRaceFinished() is called from
    // BOTH the Pi's live "finished" push AND checkRaceFinishViaJolpica()'s
    // fallback, making it the one place every finish-detection path
    // actually converges. It never called checkSeasonRollover() before.
    // checkRaceFinishViaJolpica() only runs when the Pi HASN'T already set
    // raceFinished — so with a reliably-fast Pi, that fallback (and its
    // rollover check) may simply never fire in practice. Without this, a
    // device running continuously across a season boundary with a
    // perfectly working Pi would never detect the rollover at all, and
    // wdcConfirmed would stay stuck true from the old season indefinitely.
    // fetchNextRaceFromJolpica() sets currentRound = (next round - 1),
    // which — once the refetch succeeds — correctly equals the round that
    // JUST finished.
    // PATCH: guarded to currentRound >= 1. An external review raised a
    // theoretical concern: if fetchNextRaceFromJolpica() ever resolves to
    // "round 1 of next season" immediately after this season's finale,
    // currentRound would become (1 - 1) = 0, and checkSeasonRollover(0)
    // would fire prematurely — resetting isFinalRound/pendingWDC/
    // wdcConfirmed before the WDC-confirmation flow has had a chance to
    // run. I couldn't fully verify from here whether Jolpica's "current"
    // pointer actually behaves this way (Ergast's own FAQ suggests
    // "current" doesn't flip to the new season until that season's FIRST
    // race already has results, meaning /current/next.json should return
    // empty through the whole off-season rather than jumping ahead — but
    // I can't confirm this holds for Jolpica specifically without hitting
    // the live API, which I can't do from here). Round 0 isn't a real
    // round either way, so this guard is safe regardless of which theory
    // is correct: it simply skips acting on a round number that can't be
    // genuine, and waits for the next successful refetch — the fallback
    // in checkRaceFinishViaJolpica() (keyed off ACTUAL completed-race
    // data, never 0) will catch the real rollover regardless.
    if (nextRaceRefetched && currentRound >= 1) {
      checkSeasonRollover(currentRound);
      lastKnownFinishedRound = currentRound;
    }
    if (currentMode == MODE_LIVE) { drawMainScreen(); applyMode(currentMode); }
  }
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
        // PATCH: if we booted into a non-LIVE mode while a finish-animation
        // replay was still eligible (race finished <6h ago, not yet shown
        // for this round), play it now — the FIRST time we land on LIVE —
        // instead of never. Consumed immediately so later toggles back to
        // LIVE within the same window don't replay it again.
        if (currentMode == MODE_LIVE && bootFinishAnimEligible) {
          bootFinishAnimEligible = false;
          EEPROM.write(EEPROM_FINISH_ANIM_ROUND, (uint8_t)lastKnownFinishedRound);
          EEPROM.commit();
          triggerRaceFinishAnimation(lastRaceTeamID);
        } else {
          applyMode(currentMode);
        }
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
    if      (raceFinished && pendingWDC)        updateStatus("WDC PENDING", C_YELLOW);
    else if (raceFinished)                      updateStatus("FINISHED",    C_GREEN);
    else if (raceSunday && getRacePhase() == 2) updateStatus("LIVE",        C_GREEN);
    else if (raceSunday)                        updateStatus("RACE TODAY",  C_YELLOW);  
    else if (raceCancelled)                     updateStatus("CANCELLED",   C_RED);
    else                                        updateStatus("NO RACE",     C_GREY);
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
  const double LIGHTS_OUT_LEAD_SEC = 5.0; // PATCH: fires 5s before start, not at t=0
  if (!lightsOutTriggered && lastDiff > LIGHTS_OUT_LEAD_SEC && diff <= LIGHTS_OUT_LEAD_SEC) {
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
