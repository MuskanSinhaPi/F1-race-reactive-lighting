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
const char* ssid     = "YOUR WIFI SSID";
const char* password = "YOUR WIFI PASSWORD";

// ================= PI SERVER =================
const char* piHost = "192.168.1.x";   // <-- CHANGE THIS
const int   piPort = 5000;

// ================= TIMING =================
unsigned long lastCheck         = 0;
unsigned long lastWifiCheck     = 0;
unsigned long lastClockUpdate   = 0;
unsigned long lastNTPSync       = 0;
unsigned long lastChampionPoll  = 0;   // tracks periodic WDC poll after final race
const unsigned long interval       = 5000;
const unsigned long wifiRetry      = 30000;
const unsigned long clockRefresh   = 1000;
const unsigned long ntpResync      = 1800000;
const unsigned long championPollMs = 120000; // poll every 2 min until champ confirmed

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
// Layout (64 bytes total):
//  0        EEPROM_LAST_RACE       1 byte  — GP race winner teamID
//  1        EEPROM_SAVED_MODE      1 byte  — last user mode
//  2        EEPROM_SEASON_CHAMP    1 byte  — confirmed WDC constructor teamID
//  3-22     EEPROM_LAST_GP_NAME   20 bytes — short GP name string
//  24-27    EEPROM_LAST_RACE_TIME  4 bytes — epoch of race finish
//  28-31    EEPROM_SEASON_TIME     4 bytes — epoch when season was confirmed
//  32-33    EEPROM_SEASON_YEAR     2 bytes — current season year
//  34       EEPROM_INIT_FLAG       1 byte  — magic byte
//  35       EEPROM_ANIM_PLAYED     1 byte  — 0x01 once boot-replay animation has fired
//                                            cleared on new race/season or after 12h window
#define EEPROM_LAST_RACE      0
#define EEPROM_SAVED_MODE     1
#define EEPROM_SEASON_CHAMP   2
#define EEPROM_LAST_GP_NAME   3
#define EEPROM_LAST_RACE_TIME 24
#define EEPROM_SEASON_TIME    28
#define EEPROM_SEASON_YEAR    32
#define EEPROM_INIT_FLAG      34
#define EEPROM_ANIM_PLAYED    35
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

// ================= CORE STATE =================
bool raceFinished       = false;
bool raceSunday         = false;
bool raceCancelled      = false;
bool lightsOutTriggered = false;
bool lastButtonState    = HIGH;
bool nanoReady          = false;
bool piAvailable        = false;

// ================= SEASON STATE =================
// seasonFinished   — true once the final round race result is saved (race winner known)
// championConfirmed — true once Jolpica driverStandings returns the NEW season champion
//                    (this lags by up to ~60 min after the race ends)
// championTeamID   — the confirmed WDC constructor; 0 = not yet known this session
// pendingFinalRound — true between final race finish and champion confirmation;
//                    drives the background polling loop
bool seasonFinished     = false;
bool championConfirmed  = false;
int  championTeamID     = 0;
bool pendingFinalRound  = false;

// ================= ANIMATION FLAGS =================
bool raceAnimationPlayedThisBoot    = false;
bool championAnimationPlayedThisBoot = false;
bool isBootReplay                   = false;

unsigned long lastDebounce = 0;

int    currentMode    = MODE_LIVE;
int    currentRound   = -1;
int    totalRounds    = 24;
int    lastSentTeam   = -1;
int    currentTeamID  = TEAM_MCLAREN;
time_t raceStartEpoch = 0;

char raceName[64]   = "---";
char lastGPName[64] = "Australian";
char p1Status[16]   = "LOADING";
double lastDiff     = 999999;

// ================= FORWARD DECLARATIONS =================
void fetchRaceData();
void fetchRaceDataJolpica();
bool checkESPNFinished();
int  getRacePhase();
void applyMode(int mode);
void updateTeamDisplay(int teamID, bool isLive);
void updateStatus(const char* status, uint16_t color);
void updateModeDisplay();
void sendToNano(int cmd);
bool raceFinishedRecently();
bool seasonFinishedRecently();
void checkIfRaceFinishedToday();
bool tryFetchChampion();           // polls Jolpica driverStandings; returns true when champ confirmed
void triggerChampionCelebration(int champID); // fires the WDC animation + screen transition
void saveFinalWDCConstructor();
void checkButton();
void connectWiFi();
void maintainWiFi();
void detectNextRace();
void checkLightsOutCountdown();
void checkChampionPoll();          // called from loop() while pendingFinalRound
void drawBootScreen();
void drawStatusLine(const char* msg, uint16_t color);
void drawMainScreen();
void drawRaceFinished(int teamID);
void drawChampionScreen(int champID);
void updateClock();
void updateCountdown();
void handleRaceFinished(int teamID);
const char* getModeName(int mode);
time_t parseUTCEpoch(const char* dateStr, const char* timeStr);
int getTeamID(String team);

// ==========================================================
// RACE PHASE
// Phase 0 — normal week (>6h before race, or no race data)
// Phase 1 — pre-race countdown (0–6h before race start)
// Phase 2 — race running (0–2h after race start)
// Phase 3 — post-race window (2–12h after race start)
// Phase 4 — weekend over (>12h after race start)
// ==========================================================
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

// ==========================================================
// UTC EPOCH PARSER
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
// ==========================================================
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
  drawStatusLine("WiFi OK", C_GREEN);

  configTime(19800, 0, "pool.ntp.org");
  drawStatusLine("Syncing time...", C_YELLOW);
  time_t now = time(nullptr);
  while (now < 100000) { delay(500); now = time(nullptr); }
  drawStatusLine("Time synced", C_GREEN);
  delay(300);

  for (int attempt = 0; attempt < 3 && raceStartEpoch == 0; attempt++) {
    if (attempt > 0) { drawStatusLine("Retry...", C_YELLOW); delay(2000); }
    detectNextRace();
  }
  drawStatusLine(raceStartEpoch == 0 ? "No race data" : "Race data OK",
                 raceStartEpoch == 0 ? C_RED : C_GREEN);
  delay(300);

  drawStatusLine("Waiting Nano...", C_YELLOW);
  delay(2000);
  nanoReady = true;

  if (EEPROM.read(EEPROM_INIT_FLAG) != EEPROM_MAGIC) {
    Serial.println("EEPROM first-time init");
    EEPROM.write(EEPROM_LAST_RACE,    TEAM_MERCEDES);
    EEPROM.write(EEPROM_SEASON_CHAMP, 0);
    EEPROM.write(EEPROM_SAVED_MODE,   MODE_LIVE);
    time_t zero = 0;
    EEPROM.put(EEPROM_LAST_RACE_TIME, zero);
    EEPROM.put(EEPROM_SEASON_TIME,    zero);
    int zeroYear = 0;
    EEPROM.put(EEPROM_SEASON_YEAR, zeroYear);
    EEPROM.write(EEPROM_INIT_FLAG, EEPROM_MAGIC);
    EEPROM.commit();
  }

  int savedMode = EEPROM.read(EEPROM_SAVED_MODE);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;

  int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
  if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

  // Restore champion from EEPROM if season was confirmed this session window.
  // Require phase >= 3 so we don't re-enter celebration on a mid-week reboot.
  int storedChamp = EEPROM.read(EEPROM_SEASON_CHAMP);
  if (storedChamp >= 1 && storedChamp <= 11 && seasonFinishedRecently()) {
    if (getRacePhase() >= 3) {
      championTeamID    = storedChamp;
      championConfirmed = true;
      seasonFinished    = true;
      Serial.printf("Boot: champion confirmed from EEPROM: %s\n", teamNames[championTeamID]);
    }
  }

  char savedGP[21] = {};
  for (int i = 0; i < 20; i++) savedGP[i] = EEPROM.read(EEPROM_LAST_GP_NAME + i);
  savedGP[20] = '\0';
  if (savedGP[0] >= 'A' && savedGP[0] <= 'z')
    strncpy(lastGPName, savedGP, sizeof(lastGPName) - 1);

  int phase = getRacePhase();
  if (phase >= 1 && phase <= 3) raceSunday = true;
  if (phase == 4) {
    raceSunday = raceFinished = lightsOutTriggered = false;
  }

  drawMainScreen();
  applyMode(currentMode);

  // -------------------------------------------------------
  // BOOT REPLAY within 12h window — phases 2 and 3
  // Phase 2: ESP rebooted while race was still running but
  //          EEPROM shows it finished (e.g. power blip mid-race).
  // Phase 3: normal post-race reboot window.
  // -------------------------------------------------------
  if (phase == 2 || phase == 3) {
    if (raceFinishedRecently()) {
      Serial.println("Boot: race finished recently — replaying");
      int teamID = EEPROM.read(EEPROM_LAST_RACE);
      if (teamID < 1 || teamID > 11) teamID = TEAM_MCLAREN;

      raceSunday   = true;
      raceFinished = true;
      lastSentTeam = teamID;
      isBootReplay = true;

      updateTeamDisplay(teamID, true);
      updateStatus("FINISHED", C_GREEN);

      // Race finish animation
      drawRaceFinished(teamID);
      sendToNano(CMD_CHECKERED);
      delay(9000);
      sendToNano(teamID);
      delay(500);
      raceAnimationPlayedThisBoot = true;
      isBootReplay = false;

      // Restore race result screen
      drawMainScreen();
      updateTeamDisplay(teamID, true);
      updateStatus("FINISHED", C_GREEN);

      // If champion is already confirmed from EEPROM, play champion screen now
      if (championConfirmed && !championAnimationPlayedThisBoot) {
        delay(1500);
        triggerChampionCelebration(championTeamID);
      } else if (currentRound >= totalRounds) {
        // Final round but champ not yet confirmed — resume polling
        pendingFinalRound = true;
        updateStatus("WDC PENDING", C_YELLOW);
        Serial.println("Boot: final round, champ not confirmed — resuming poll");
      }

    } else {
      checkIfRaceFinishedToday();
    }
  }
}

// ==========================================================
// LOOP
// ==========================================================
void loop() {
  checkButton();
  maintainWiFi();
  checkLightsOutCountdown();
  checkChampionPoll();   // no-op unless pendingFinalRound

  if (getRacePhase() == 4) {
    if (raceFinished || raceSunday || lightsOutTriggered) {
      Serial.println("Phase 4 — reset race flags");
      raceFinished       = false;
      raceSunday         = false;
      raceCancelled      = false;
      lightsOutTriggered = false;
    }
  }

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

  int phase = getRacePhase();
  if (currentMode == MODE_LIVE && (phase == 1 || phase == 2) && !raceFinished) {
    if (millis() - lastCheck > interval) {
      lastCheck = millis();
      fetchRaceData();
    }
  }

  delay(50);
}

// ==========================================================
// CHAMPION POLL — called every loop tick, fires every 2 min
// while pendingFinalRound is true.
// Once tryFetchChampion() returns true, pendingFinalRound is
// cleared and triggerChampionCelebration() fires.
// ==========================================================
void checkChampionPoll() {
  if (!pendingFinalRound || championConfirmed) return;
  if (millis() - lastChampionPoll < championPollMs) return;
  lastChampionPoll = millis();

  Serial.println("Polling for WDC champion...");
  updateStatus("WDC CHECK", C_YELLOW);

  if (tryFetchChampion()) {
    pendingFinalRound = false;
    // Short pause so the user sees the status change
    delay(500);
    triggerChampionCelebration(championTeamID);
  } else {
    // Still waiting — restore finished status
    int raceWinner = EEPROM.read(EEPROM_LAST_RACE);
    updateTeamDisplay(raceWinner >= 1 ? raceWinner : TEAM_MCLAREN, true);
    updateStatus("WDC PENDING", C_YELLOW);
  }
}

// ==========================================================
// TRY FETCH CHAMPION
// Calls Jolpica driverStandings. Returns true only when the
// standings P1 constructor is DIFFERENT from last season's
// stored champion OR the season year has already been reset.
// This avoids triggering on stale data that still shows the
// previous champion before Jolpica updates.
//
// Strategy: compare the returned champion against the value
// stored in EEPROM_SEASON_CHAMP. If they differ, Jolpica has
// updated to the new champion. If they're the same as a
// prior-year winner, we can't tell — so we accept whoever is
// P1 once the race is confirmed finished.
// ==========================================================
bool tryFetchChampion() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/driverStandings.json");
  if (https.GET() != 200) { https.end(); return false; }

  StaticJsonDocument<128> filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["round"] = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(
    doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) return false;

  const char* roundStr =
    doc["MRData"]["StandingsTable"]["StandingsLists"][0]["round"];
  const char* constructor =
    doc["MRData"]["StandingsTable"]["StandingsLists"][0]
       ["DriverStandings"][0]["Constructors"][0]["name"];

  if (!constructor || !roundStr) return false;

  int standingsRound = atoi(roundStr);
  int teamID = getTeamID(String(constructor));
  if (teamID < 1 || teamID > 11) return false;

  // GUARD 1: don't accept stale standings — wait for Jolpica to catch up
  // to the round that just finished. The "round" field in driverStandings
  // increments when Jolpica processes results, so this is a reliable
  // freshness signal that works regardless of totalRounds.
  if (standingsRound < currentRound) {
    Serial.printf("Standings still at round %d, need %d — waiting\n",
                  standingsRound, currentRound);
    return false;
  }

  // GUARD 2: don't fire during an active race window (phase 1 or 2).
  // Standings could theoretically update mid-race for a previous round;
  // ignore until we're at least in the post-race window (phase >= 3).
  if (getRacePhase() < 3) {
    Serial.println("Race phase < 3 — too early to confirm champion");
    return false;
  }

  // Both guards passed — standings are fresh and race is over.
  // seasonFinished is set only if Jolpica confirms this IS the final
  // round (standingsRound == totalRounds). For mid-season races,
  // standingsRound == currentRound but totalRounds is higher, so we
  // clear pendingFinalRound without setting seasonFinished.
  if (standingsRound < totalRounds) {
    Serial.printf("Round %d/%d — mid-season, not final. Clearing poll.\n",
                  standingsRound, totalRounds);
    pendingFinalRound = false;
    return false;   // not the final race — no celebration
  }

  // Standings are current AND this is the final round — confirmed champion
  championTeamID    = teamID;
  championConfirmed = true;

  // Persist
  EEPROM.write(EEPROM_SEASON_CHAMP, teamID);
  time_t champNow = time(nullptr);
  if (champNow > 1700000000) {
    EEPROM.put(EEPROM_SEASON_TIME, champNow);
  }
  seasonFinished = true;
  EEPROM.commit();

  Serial.printf("WDC champion confirmed: %s (standings round %d)\n",
                teamNames[teamID], standingsRound);
  return true;
}

// ==========================================================
// TRIGGER CHAMPION CELEBRATION
// Sequence:
//   1. Flash header + champion screen (season win)
//   2. Send CMD_CHECKERED then CMD_PULSE in champion colour
//   3. After 10s return to NORMAL race result screen
//      (Abu Dhabi P1 constructor / race winner stays displayed)
//   4. Update header/status to reflect season end
// applyMode() and mode toggles will always show the race
// winner in LIVE mode — champion only shown in DISPLAY mode
// and during this one celebration sequence.
// ==========================================================
void triggerChampionCelebration(int champID) {
  if (championAnimationPlayedThisBoot) return;
  championAnimationPlayedThisBoot = true;

  Serial.printf("Champion celebration: %s\n", teamNames[champID]);

  // Gold flash
  for (int i = 0; i < 4; i++) {
    tft.fillScreen(0xC600); delay(180);
    tft.fillScreen(C_BLACK); delay(180);
  }

  // Champion screen
  drawChampionScreen(champID);
  sendToNano(CMD_CHECKERED);
  delay(3000);
  sendToNano(CMD_PULSE);   // pulse in champion LED colour — Nano maps CMD_PULSE
  delay(7000);

  // Transition back to race result (Abu Dhabi winner)
  int raceWinner = EEPROM.read(EEPROM_LAST_RACE);
  if (raceWinner < 1 || raceWinner > 11) raceWinner = champID;

  // Rebuild main screen with season-end header, but RACE WINNER team shown
  drawMainScreen();         // redraws with seasonFinished=true header ("F1 SEASON END")
  updateTeamDisplay(raceWinner, true);   // shows Abu Dhabi P1 — not the champ
  updateStatus("CHAMPIONS!", C_YELLOW);
  sendToNano(raceWinner);   // LEDs show race winner colour (stays)
}

// ==========================================================
// CHAMPION SCREEN — shown during the celebration window only
// ==========================================================
void drawChampionScreen(int champID) {
  tft.fillScreen(C_BLACK);

  // Gold header
  tft.fillRect(0, 0, 160, 16, 0xC600);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4); tft.print("WDC CHAMPION!");
  updateClock();

  // Champion banner
  tft.fillRect(0, 18, 160, 60, teamTFTColor[champID]);
  tft.setTextColor(C_BLACK); tft.setTextSize(1);
  tft.setCursor(8, 22); tft.print("CONSTRUCTOR CHAMPION");
  tft.setTextSize(2);
  // Centre the name
  int nameLen = strlen(teamNames[champID]);
  int cx = (160 - nameLen * 12) / 2;
  if (cx < 4) cx = 4;
  tft.setCursor(cx, 38); tft.print(teamNames[champID]);

  tft.fillRect(0, 79, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 79, 160, C_BORDER);
  tft.setTextColor(C_YELLOW); tft.setTextSize(1);
  tft.setCursor(4, 83); tft.print("SEASON COMPLETE!");

  tft.fillRect(0, 92, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 92, 160, C_BORDER);
  char gpBuf[24];
  char gpShort[14]; strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
  char* gp = strstr(gpShort, " Grand"); if (gp) *gp = '\0';
  snprintf(gpBuf, sizeof(gpBuf), "Last GP: %s", gpShort);
  tft.setTextColor(C_GREY); tft.setCursor(4, 96); tft.print(gpBuf);

  tft.fillRect(0, 106, 160, 22, C_PANEL);
  tft.drawFastHLine(0, 106, 160, C_BORDER);
  tft.setTextColor(C_DIM); tft.setTextSize(1);
  char rndBuf[16];
  snprintf(rndBuf, sizeof(rndBuf), "Rnd %d/%d FINAL", totalRounds, totalRounds);
  tft.setCursor(4, 110); tft.print(rndBuf);
  tft.setCursor(4, 120); tft.print("F1 LED CONTROLLER");
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
// When seasonFinished: header goes gold, subtitle "Off season",
// team card label says "<GP> | CHAMPION" — but teamID shown is
// the RACE WINNER (Abu Dhabi P1), NOT the WDC champ.
// WDC champ is only shown in DISPLAY mode and during celebration.
// ==========================================================
void drawMainScreen() {
  tft.fillScreen(C_BLACK);

  uint16_t headerColor = seasonFinished ? 0xC600 : C_RED;
  tft.fillRect(0, 0, 160, 16, headerColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.print(seasonFinished ? "F1 SEASON END" : "F1 LIVE");
  tft.setCursor(100, 4); tft.print("--:--:--");

  tft.fillRect(0, 16, 160, 12, C_PANEL);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 20);
  if (seasonFinished) {
    tft.print("Off season");
  } else {
    tft.print("Next GP: ");
    tft.setTextColor(C_WHITE);
    char shortName[20]; strncpy(shortName, raceName, 19); shortName[19] = '\0';
    tft.print(shortName);
  }

  tft.drawFastHLine(0, 28, 160, C_BORDER);

  tft.fillRect(0, 29, 4, 44, teamTFTColor[currentTeamID]);
  tft.fillRect(4, 29, 156, 44, C_PANEL);
  tft.drawRect(4, 29, 156, 44, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(8, 33);
  if (currentMode == MODE_DISPLAY) {
    if (!seasonFinished && !championConfirmed) {
      tft.print("DEFENDING CHAMP");
    } else {
      tft.print("WDC CONSTRUCTOR");
    }
  } else if (seasonFinished) {
    char label[27];
    char gpShort[14];
    strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
    char* gp = strstr(gpShort, " Grand"); if (gp) *gp = '\0';
    snprintf(label, sizeof(label), "%s | P1:", gpShort);
    tft.print(label);
  } else if (currentMode == MODE_LIVE) {
    tft.print("Last GP:");
  } else {
    tft.print("CONSTRUCTOR");
  }
  tft.fillRect(8, 43, 148, 20, C_PANEL);
  tft.setTextColor(teamTFTColor[currentTeamID]); tft.setTextSize(2);
  tft.setCursor(8, 45); tft.print(teamNames[currentTeamID]);

  tft.fillRect(0, 73, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 73, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 77); tft.print("STATUS:");
  tft.setTextColor(seasonFinished ? C_YELLOW : C_GREEN);
  tft.setCursor(52, 77); tft.print(p1Status);

  tft.fillRect(0, 86, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 86, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 90); tft.print("MODE:");
  tft.setTextColor(C_WHITE); tft.setCursor(40, 90); tft.print(getModeName(currentMode));

  tft.fillRect(0, 99, 160, 13, C_PANEL);
  tft.drawFastHLine(0, 99, 160, C_BORDER);
  tft.setTextColor(C_GREY); tft.setTextSize(1);
  tft.setCursor(4, 103);
  if (seasonFinished)         tft.print("SEASON COMPLETE!");
  else if (raceFinished)      tft.print("RACE COMPLETE");
  else if (raceCancelled)     tft.print("RACE CANCELLED");
  else if (raceSunday)        tft.print("LIGHTS OUT: --:--:--");
  else                        tft.print("NO RACE TODAY");

  tft.drawFastHLine(0, 112, 160, C_BORDER);
  tft.fillRect(0, 113, 160, 15, C_PANEL);
  tft.setTextColor(C_DIM);
  tft.setCursor(4, 117);
  char rndBuf[16];
  snprintf(rndBuf, sizeof(rndBuf), "Rnd %d/%d",
           currentRound > 0 ? currentRound : 0, totalRounds);
  tft.print(rndBuf);
  tft.setCursor(90, 117);
  tft.print(piAvailable ? "Pi+ESPN" : "Jolpica");
}

// ==========================================================
// CLOCK
// ==========================================================
void updateClock() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  uint16_t headerColor = seasonFinished ? 0xC600 : C_RED;
  tft.fillRect(96, 1, 63, 14, headerColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(1);
  tft.setCursor(98, 4); tft.print(buf);
}

// ==========================================================
// COUNTDOWN
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
    if (!seasonFinished && !championConfirmed) {
      tft.print("DEFENDING CHAMP");
    } else {
      tft.print("WDC CONSTRUCTOR");
    }
  } else if (seasonFinished) {
    // In LIVE mode during off-season, label shows race P1 (last GP)
    char label[27];
    char gpShort[14];
    strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
    char* gp = strstr(gpShort, " Grand"); if (gp) *gp = '\0';
    snprintf(label, sizeof(label), "%s | P1:", gpShort);
    tft.print(label);
  } else if (isLive) {
    char label[27];
    char gpShort[14];
    strncpy(gpShort, lastGPName, 13); gpShort[13] = '\0';
    char* gp = strstr(gpShort, " Grand"); if (gp) *gp = '\0';
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
// UPDATE STATUS
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
// RACE FINISHED SCREEN — regular race, not season end
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
// For the final round this sets pendingFinalRound and starts
// the background champion poll. Animation shows race winner
// only — no WDC celebration here (champ data not ready yet).
// ==========================================================
void handleRaceFinished(int teamID) {
  if (raceFinished) return;

  raceFinished = true;
  lastSentTeam = teamID;

  if (!isBootReplay) {
    EEPROM.write(EEPROM_LAST_RACE, teamID);
    time_t now = time(nullptr);
    EEPROM.put(EEPROM_LAST_RACE_TIME, now);

    char shortGP[21] = {};
    strncpy(shortGP, lastGPName, 20);
    char* grandPtr = strstr(shortGP, " Grand Prix");
    if (grandPtr) *grandPtr = '\0';
    for (int i = 0; i < 20; i++)
      EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);

    EEPROM.commit();
  }

  updateStatus("FINISHED", C_GREEN);

  // Race finish animation
  if (!raceAnimationPlayedThisBoot) {
    drawRaceFinished(teamID);
    sendToNano(CMD_CHECKERED);
    delay(9000);
    sendToNano(teamID);
    delay(500);
    raceAnimationPlayedThisBoot = true;

    drawMainScreen();
    updateTeamDisplay(teamID, true);
    updateStatus("FINISHED", C_GREEN);
  }

  // Always poll after every race finish.
  // tryFetchChampion() uses the standings "round" field to confirm
  // freshness — no totalRounds comparison needed here.
  // For mid-season races: pendingFinalRound is set, first poll fires,
  // standingsRound == currentRound so freshness passes, but Jolpica
  // won't set seasonFinished because the season isn't over — poll clears.
  // For the final race: same path, but once Jolpica updates it triggers
  // the celebration automatically.
  if (!championConfirmed && currentRound >= totalRounds) {
    pendingFinalRound = true;
    lastChampionPoll  = 0;  // fire first poll promptly
    Serial.println("Race finished — champion poll started (standings will decide)");
  }
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
// LIVE mode: always shows race winner (Abu Dhabi P1), NOT WDC champ
// DISPLAY mode: shows WDC constructor champion (from EEPROM)
// No animation is ever triggered here.
// ==========================================================
void applyMode(int mode) {
  if (mode == MODE_DISPLAY) {
    sendToNano(CMD_DISPLAY);
    // Show confirmed WDC champion, or last known if pending
    int dispChamp = championConfirmed ? championTeamID : EEPROM.read(EEPROM_SEASON_CHAMP);
    if (dispChamp < 1 || dispChamp > 11) dispChamp = TEAM_MCLAREN;
    updateTeamDisplay(dispChamp, false);
    if (!seasonFinished && !championConfirmed) {
      updateStatus("DEFENDING", C_YELLOW);
    } else {
      updateStatus("CHAMPIONS!", C_YELLOW);
    }
    updateModeDisplay();
    return;
  }

  if (mode == MODE_LIVE) {
    lastSentTeam = -1;

    // Always show race winner in LIVE mode
    int raceWinner = EEPROM.read(EEPROM_LAST_RACE);
    if (raceWinner < 1 || raceWinner > 11) raceWinner = TEAM_MCLAREN;
    currentTeamID = raceWinner;

    updateTeamDisplay(raceWinner, true);
    updateModeDisplay();
    sendToNano(raceWinner);
    lastSentTeam = raceWinner;

    int phase = getRacePhase();
    if (phase == 2 && !raceFinished)
      sendToNano(CMD_PULSE);

    if (raceFinished) {
      updateStatus(pendingFinalRound ? "WDC PENDING" :
                   seasonFinished    ? "CHAMPIONS!"  : "FINISHED",
                   pendingFinalRound ? C_YELLOW :
                   seasonFinished    ? C_YELLOW : C_GREEN);
    } else if (raceSunday) {
      updateStatus("FETCHING", C_YELLOW);
      fetchRaceData();
    } else {
      updateStatus(seasonFinished ? "OFF SEASON" : "NO RACE",
                   seasonFinished ? C_YELLOW : C_GREY);
    }
    return;
  }

  // Team modes
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

  const char* seasonStr = doc["MRData"]["RaceTable"]["season"];
  int apiSeason = seasonStr ? atoi(seasonStr) : 0;
  int storedSeason = 0;
  EEPROM.get(EEPROM_SEASON_YEAR, storedSeason);

  if (apiSeason > 2000 && apiSeason != storedSeason) {
    Serial.printf("New season: %d -> %d\n", storedSeason, apiSeason);
    EEPROM.put(EEPROM_SEASON_YEAR, apiSeason);
    EEPROM.write(EEPROM_SEASON_CHAMP, 0);
    EEPROM.write(EEPROM_LAST_RACE, 0);
    time_t zero = 0;
    EEPROM.put(EEPROM_SEASON_TIME, zero);
    EEPROM.commit();
    raceFinished                     = false;
    raceCancelled                    = false;
    lightsOutTriggered               = false;
    seasonFinished                   = false;
    championConfirmed                = false;
    championTeamID                   = 0;
    pendingFinalRound                = false;
    raceAnimationPlayedThisBoot      = false;
    championAnimationPlayedThisBoot  = false;
  }

  auto race        = doc["MRData"]["RaceTable"]["Races"][0];
  const char* name     = race["raceName"];
  const char* date     = race["date"];
  const char* timeS    = race["time"];
  const char* roundStr = race["round"];

  if (name)     strncpy(raceName, name, sizeof(raceName) - 1);
  if (roundStr) currentRound = atoi(roundStr);

  if (date && timeS) {
    raceStartEpoch = parseUTCEpoch(date, timeS);

    int phase = getRacePhase();
    raceSunday = (phase >= 1 && phase <= 3);

    if (phase == 4) {
      raceFinished       = false;
      raceCancelled      = false;
      lightsOutTriggered = false;
      raceSunday         = false;
    }

    static int lastRoundSeen = -1;
    if (currentRound > 0 && currentRound != lastRoundSeen) {
      if (lastRoundSeen != -1) {
        raceFinished                    = false;
        raceCancelled                   = false;
        lightsOutTriggered              = false;
        raceAnimationPlayedThisBoot     = false;
        championAnimationPlayedThisBoot = false;
        pendingFinalRound               = false;
        Serial.printf("Round %d->%d reset\n", lastRoundSeen, currentRound);
      }
      lastRoundSeen = currentRound;
    }

    Serial.printf("Race: %s | Rnd: %d/%d | phase: %d | raceDay: %s\n",
                  raceName, currentRound, totalRounds,
                  phase, raceSunday ? "YES" : "NO");
  }
}

// ==========================================================
// CHECK IF RACE ALREADY FINISHED — boot-time via last.json
// ==========================================================
void checkIfRaceFinishedToday() {
  Serial.println("Checking last.json...");

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
  JsonArray results  = race["Results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return;

  const char* team   = results[0]["Constructor"]["name"];
  const char* status = results[0]["status"];
  if (!team || !status || !date) return;
  if (String(status) != "Finished") return;
  if (raceFinished) return;

  // GP dedup guard
  char storedGP[21] = {};
  for (int i = 0; i < 20; i++) storedGP[i] = EEPROM.read(EEPROM_LAST_GP_NAME + i);
  storedGP[20] = '\0';

  char apiGPStripped[64] = {};
  if (gpName) {
    strncpy(apiGPStripped, gpName, sizeof(apiGPStripped) - 1);
    char* gp = strstr(apiGPStripped, " Grand Prix"); if (gp) *gp = '\0';
    char* sp = strrchr(apiGPStripped, ' ');
    if (sp) memmove(apiGPStripped, sp + 1, strlen(sp + 1) + 1);
  }
  if (strlen(storedGP) > 0 && strcmp(apiGPStripped, storedGP) == 0) {
    Serial.println("Same GP as stored — skipping");
    return;
  }

  // Date guard — reject if older than 36h
  time_t now           = time(nullptr);
  time_t raceDateEpoch = parseUTCEpoch(date, "00:00:00Z");
  if (difftime(now, raceDateEpoch) > 129600) return;

  Serial.printf("last.json: finished, winner: %s\n", team);

  if (gpName) strncpy(lastGPName, gpName, sizeof(lastGPName) - 1);
  int teamID = getTeamID(String(team));

  if (now > 1700000000) {
    EEPROM.put(EEPROM_LAST_RACE_TIME, now);
  }
  EEPROM.write(EEPROM_LAST_RACE, teamID);
  char shortGP[21] = {};
  strncpy(shortGP, lastGPName, 20);
  char* grandPtr = strstr(shortGP, " Grand Prix"); if (grandPtr) *grandPtr = '\0';
  char* sp = strrchr(shortGP, ' ');
  if (sp) memmove(shortGP, sp + 1, strlen(sp + 1) + 1);
  for (int i = 0; i < 20; i++)
    EEPROM.write(EEPROM_LAST_GP_NAME + i, shortGP[i]);
  EEPROM.commit();

  raceSunday = true;
  handleRaceFinished(teamID);
}

// ==========================================================
// LIGHTS OUT — zero-crossing trigger
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
// FETCH RACE DATA
// ==========================================================
void fetchRaceData() {
  time_t now     = time(nullptr);
  double elapsed = difftime(now, raceStartEpoch);

  if (elapsed < 0) { updateStatus("PRE RACE", C_YELLOW); return; }

  WiFiClient piClient;
  HTTPClient http;
  String url = String("http://") + piHost + ":" + piPort + "/p1";
  http.begin(piClient, url);
  http.setTimeout(4000);
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();

    if (!err) {
      piAvailable = true;
      const char* team    = doc["team"];
      const char* status  = doc["status"];
      const char* gp      = doc["gp"];
      const char* champ   = doc["champion"];        // Pi-projected WDC constructor
      const char* cStatus = doc["champion_status"]; // "projected" or "confirmed"

      Serial.printf("Pi -> team:%s status:%s champ:%s(%s)\n",
                    team ? team : "null", status ? status : "null",
                    champ ? champ : "none", cStatus ? cStatus : "-");

      if (gp && strlen(gp) > 0) {
        strncpy(lastGPName, gp, sizeof(lastGPName) - 1);
        char* grandPtr = strstr(lastGPName, " Grand Prix");
        if (grandPtr) *grandPtr = '\0';
      }

      if (team && status) {
        String s = String(status);
        if (s == "finished") {
          int raceWinnerID = getTeamID(String(team));
          updateTeamDisplay(raceWinnerID, true);
          handleRaceFinished(raceWinnerID);

          // If Pi already confirmed the champion at finish time, fast-path it.
          // This covers the case where the race ends and Jolpica is already
          // updated (Pi polled it before sending "finished").
          if (champ && cStatus && !championConfirmed) {
            String cs = String(cStatus);
            if (cs == "confirmed") {
              int champID = getTeamID(String(champ));
              if (champID >= 1 && champID <= 11) {
                Serial.printf("Pi confirms WDC at finish: %s\n", champ);
                championTeamID    = champID;
                championConfirmed = true;
                pendingFinalRound = false;
                EEPROM.write(EEPROM_SEASON_CHAMP, champID);
                time_t nowT = time(nullptr);
                if (nowT > 1700000000) {
                  EEPROM.put(EEPROM_SEASON_TIME, nowT);
                }
                seasonFinished = true;
                EEPROM.commit();
                delay(1500);
                triggerChampionCelebration(champID);
              }
            }
            // "projected" at finish — don't act, let Jolpica poll confirm
          }
          return;
        }
        if (s == "delayed") {
          updateStatus("RAIN DELAY", C_YELLOW);
          if (lastSentTeam >= 1) sendToNano(CMD_PULSE);
          return;
        }
        if (s == "cancelled") {
          raceCancelled = true; raceSunday = false;
          raceFinished = false; lightsOutTriggered = false;
          updateStatus("CANCELLED", C_RED);
          drawMainScreen(); sendToNano(CMD_DISPLAY);
          return;
        }
        if (s == "postponed") { updateStatus("POSTPONED", C_RED); return; }
        if (s == "live") {
          int teamID = getTeamID(String(team));
          updateTeamDisplay(teamID, true);
          if (teamID != lastSentTeam) { sendToNano(teamID); lastSentTeam = teamID; }
          sendToNano(CMD_PULSE);

          // Projected champion hint from Pi — shown only during the final race
          // while we're still waiting for Jolpica confirmation.
          // "projected"  → Pi's maths says this team will win the WDC
          // "confirmed"  → Pi has cross-checked with Jolpica (belt-and-braces)
          // NodeMCU never acts on this as truth — Jolpica via tryFetchChampion()
          // remains the only gate for the real celebration.
          if (champ && cStatus && pendingFinalRound && !championConfirmed) {
            String cs = String(cStatus);
            if (cs == "projected") {
              updateStatus("WDC LIKELY", C_YELLOW);
              Serial.printf("Pi projects WDC: %s\n", champ);
            } else if (cs == "confirmed") {
              // Pi already cross-checked Jolpica — trust it and fast-path
              Serial.printf("Pi confirms WDC: %s\n", champ);
              int champID = getTeamID(String(champ));
              if (champID >= 1 && champID <= 11) {
                championTeamID    = champID;
                championConfirmed = true;
                pendingFinalRound = false;
                EEPROM.write(EEPROM_SEASON_CHAMP, champID);
                time_t nowT = time(nullptr);
                if (nowT > 1700000000) {
                  EEPROM.put(EEPROM_SEASON_TIME, nowT);
                }
                seasonFinished = true;
                EEPROM.commit();
                triggerChampionCelebration(champID);
                return;
              }
            }
          } else {
            updateStatus("LIVE", C_GREEN);
          }

          if (elapsed >= 5400 && checkESPNFinished()) fetchRaceDataJolpica();
          return;
        }
      }
    } else { http.end(); }
  } else { http.end(); }

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

  auto race          = doc["MRData"]["RaceTable"]["Races"][0];
  const char* gpName = race["raceName"];
  JsonArray results  = race["Results"].as<JsonArray>();
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
    updateStatus("LIVE", C_GREEN); sendToNano(CMD_PULSE); return;
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
// saveFinalWDCConstructor — kept for compatibility but no
// longer called from handleRaceFinished. Champion detection
// is now handled exclusively by tryFetchChampion() via the
// polling loop. This prevents storing a stale last-season
// champion at race-finish time.
// ==========================================================
void saveFinalWDCConstructor() {
  // Intentionally empty — use tryFetchChampion() + checkChampionPoll()
}

// ==========================================================
// RACE FINISHED RECENTLY — EEPROM fast path on boot
// ==========================================================
bool raceFinishedRecently() {
  time_t storedTime;
  EEPROM.get(EEPROM_LAST_RACE_TIME, storedTime);
  time_t now = time(nullptr);
  if (storedTime < 1700000000 || storedTime > now) return false;
  if (difftime(now, storedTime) < 43200) {
    Serial.println("EEPROM: race finished recently");
    return true;
  }
  return false;
}

// ==========================================================
// SEASON FINISHED RECENTLY — EEPROM fast path on boot
// ==========================================================
bool seasonFinishedRecently() {
  time_t storedTime;
  EEPROM.get(EEPROM_SEASON_TIME, storedTime);
  time_t now = time(nullptr);
  if (storedTime < 1700000000 || storedTime > now) return false;
  return difftime(now, storedTime) < 43200;
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
