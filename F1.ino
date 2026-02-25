#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <time.h>

#define LED_PIN     D4
#define BUTTON_PIN  D3
#define NUM_LEDS    60

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ================= WIFI =================
const char* ssid     = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

// ================= TIMING =================
unsigned long lastCheck       = 0;
unsigned long lastWifiCheck   = 0;
const unsigned long interval  = 120000; // 2 min
const unsigned long wifiRetry = 30000;  // 30 sec

// ================= STATE =================
bool raceFinished   = false;
bool displayMode    = false;
bool raceWeekend    = false;
bool lastButtonState = HIGH;

int  currentRound   = -1;  // track which round results we've seen

// ================= COLOR TRACKING =================
int currentR = 0, currentG = 0, currentB = 0;
int targetR  = 0, targetG  = 0, targetB  = 0;

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

// ==========================================================

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.show();
  strip.setBrightness(100);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  EEPROM.begin(10);

  connectWiFi();

  configTime(19800, 0, "pool.ntp.org"); // IST

  // Wait for NTP sync properly
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 100000 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  Serial.println();

  detectRaceWeekend();

  int savedTeam = EEPROM.read(0);
  if (savedTeam < 1 || savedTeam > 11) {
    savedTeam = TEAM_MCLAREN;
    EEPROM.write(0, savedTeam);
    EEPROM.commit();
  }

  applyTeamColor(savedTeam);
}

// ==========================================================

void loop() {
  checkButton();
  if (displayMode) return;

  // WiFi watchdog
  if (millis() - lastWifiCheck > wifiRetry) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      connectWiFi();
    }
  }

  if (!raceWeekend) return;
  if (raceFinished) return;

  if (millis() - lastCheck > interval) {
    lastCheck = millis();
    fetchRaceData();
  }

  pulseEffect();
  delay(30);
}

// ==========================================================
// BUTTON
// ==========================================================

void checkButton() {
  bool currentState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentState == LOW) {
    displayMode = !displayMode;

    if (displayMode) {
      showDisplayWhite();
    } else {
      applyTeamColor(EEPROM.read(0));
    }

    delay(300);
  }

  lastButtonState = currentState;
}

// ==========================================================
// DISPLAY MODE
// ==========================================================

void showDisplayWhite() {
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(180, 180, 170));
  strip.setBrightness(150);
  strip.show();
}

// ==========================================================
// WIFI
// ==========================================================

void connectWiFi() {
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
}

// ==========================================================
// RACE WEEKEND DETECTION
// ==========================================================

void detectRaceWeekend() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/next.json");

  int httpCode = https.GET();

  if (httpCode == 200) {
    String payload = https.getString();

    StaticJsonDocument<64> filter;
    filter["MRData"]["RaceTable"]["Races"][0]["date"]  = true;
    filter["MRData"]["RaceTable"]["Races"][0]["round"] = true;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload,
                                 DeserializationOption::Filter(filter));

    if (!err) {
      const char* raceDate = doc["MRData"]["RaceTable"]["Races"][0]["date"];
      const char* roundStr = doc["MRData"]["RaceTable"]["Races"][0]["round"];

      if (raceDate) {
        // Parse race date components
        int ry, rm, rd;
        sscanf(raceDate, "%d-%d-%d", &ry, &rm, &rd);

        // Build race date as time_t (midnight IST = UTC-5:30 offset already applied)
        struct tm raceTm = {};
        raceTm.tm_year = ry - 1900;
        raceTm.tm_mon  = rm - 1;
        raceTm.tm_mday = rd;
        raceTm.tm_hour = 12; // noon on race day
        time_t raceEpoch = mktime(&raceTm);

        time_t now = time(nullptr);
        double diff = difftime(raceEpoch, now);

        // Active if we're within 3 days before race (Thu/Fri) through race day
        // if (diff >= 0 && diff <= 3 * 86400) {
        // Active if final race day (Sunday)
        if (diff >= 0 && diff <= 86400) {
          raceWeekend = true;
          if (roundStr) currentRound = atoi(roundStr);
          Serial.printf("Race weekend active! Round %d on %s\n", currentRound, raceDate);
        }
      }
    }
  }

  https.end();
}

String getTodayDate() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
  return String(buf);
}

// ==========================================================
// FETCH RACE DATA
// ==========================================================

void fetchRaceData() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  int httpCode = https.GET();
  Serial.printf("fetchRaceData HTTP: %d\n", httpCode);

  if (httpCode == 200) {
    String payload = https.getString();

    // Filter to only what we need — avoids memory overflow on full payload
    StaticJsonDocument<128> filter;
    filter["MRData"]["RaceTable"]["Races"][0]["round"]                                    = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"]                     = true;
    filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"]        = true;

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload,
                                 DeserializationOption::Filter(filter));

    if (err) {
      Serial.print("JSON error: ");
      Serial.println(err.c_str());
      https.end();
      return;
    }

    const char* roundStr = doc["MRData"]["RaceTable"]["Races"][0]["round"];
    const char* status   = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"];
    const char* team     = doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];

    if (!roundStr || !status || !team) {
      Serial.println("Missing fields in response");
      https.end();
      return;
    }

    int resultRound = atoi(roundStr);

    // Guard: only act on results from the CURRENT round
    // (prevents last week's "Finished" from triggering on weekend start)
    if (currentRound > 0 && resultRound != currentRound) {
      Serial.printf("Results are for round %d, waiting for round %d\n",
                    resultRound, currentRound);
      https.end();
      return;
    }

    int teamID = getTeamID(String(team));
    applyTeamColor(teamID);

    Serial.printf("P1: %s | Status: %s\n", team, status);

    if (String(status) == "Finished") {
      raceFinished = true;
      checkeredWipe();

      EEPROM.write(0, teamID);
      EEPROM.commit();

      applyTeamColor(teamID);
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
  return TEAM_MCLAREN; // default
}

// ==========================================================
// APPLY TEAM COLOR
// ==========================================================

void applyTeamColor(int teamID) {
  switch (teamID) {
    case TEAM_FERRARI:     setTargetColor(220,  20,  20); break;
    case TEAM_ALPINE:      setTargetColor( 20,  40, 150); break;
    case TEAM_ASTON:       setTargetColor(  0, 130,  90); break;
    case TEAM_HAAS:        setTargetColor(220,   0,  45); break;
    case TEAM_AUDI:        setTargetColor(180,   0,   0); break;
    case TEAM_CADILLAC:    setTargetColor(180, 180, 180); break;
    case TEAM_MCLAREN:     setTargetColor(255, 120,   0); break;
    case TEAM_MERCEDES:    setTargetColor(  0, 255, 210); break;
    case TEAM_RACINGBULLS: setTargetColor( 40,  60, 200); break;
    case TEAM_REDBULL:     setTargetColor( 10,  60, 200); break;
    case TEAM_WILLIAMS:    setTargetColor( 20,  40, 180); break;
  }
  smoothFade();
}

// ==========================================================
// SMOOTH FADE
// ==========================================================

void setTargetColor(int r, int g, int b) {
  targetR = r; targetG = g; targetB = b;
}

void smoothFade() {
  int steps = 40;
  for (int i = 0; i <= steps; i++) {
    int r = currentR + (targetR - currentR) * i / steps;
    int g = currentG + (targetG - currentG) * i / steps;
    int b = currentB + (targetB - currentB) * i / steps;

    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(r, g, b));

    updateBrightness();
    strip.show();
    delay(20);
  }
  currentR = targetR;
  currentG = targetG;
  currentB = targetB;
}

// ==========================================================
// PULSE EFFECT
// ==========================================================

void pulseEffect() {
  static float factor = 0.3;
  static bool  rising = true;

  if (rising) { factor += 0.01; if (factor >= 1.0) rising = false; }
  else        { factor -= 0.01; if (factor <= 0.3) rising = true;  }

  int r = currentR * factor;
  int g = currentG * factor;
  int b = currentB * factor;

  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(r, g, b));

  updateBrightness();
  strip.show();
}

// ==========================================================
// CHECKERED WIPE
// ==========================================================

void checkeredWipe() {
  int blockSize = 4;
  int cycles    = 15;

  for (int c = 0; c < cycles; c++) {
    for (int offset = 0; offset < blockSize * 2; offset++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        int pattern = ((i + offset) / blockSize) % 2;
        strip.setPixelColor(i, pattern == 0
          ? strip.Color(240, 240, 240)
          : strip.Color( 30,  30,  30));
      }
      updateBrightness();
      strip.show();
      delay(80);
    }
  }
}

// ==========================================================
// AUTO BRIGHTNESS (IST)
// ==========================================================

void updateBrightness() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  strip.setBrightness((t->tm_hour >= 7 && t->tm_hour <= 19) ? 120 : 40);
}
