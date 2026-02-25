#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <time.h>

#define LED_PIN D4
#define NUM_LEDS 60

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------- WIFI ----------
const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

// ---------- TIMING ----------
unsigned long lastCheck = 0;
const unsigned long interval = 120000;  // 2 min

// ---------- STATE ----------
bool raceFinished = false;

// ---------- COLOR TRACKING ----------
int currentR = 0, currentG = 0, currentB = 0;
int targetR = 0, targetG = 0, targetB = 0;

// ---------- TEAM IDs ----------
#define TEAM_REDBULL 1
#define TEAM_FERRARI 2
#define TEAM_MERCEDES 3
#define TEAM_MCLAREN 4
#define TEAM_ASTON 5

// ==========================================================

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.show();
  strip.setBrightness(100);

  EEPROM.begin(10);

  connectWiFi();

  // IST time sync
  configTime(19800, 0, "pool.ntp.org");

  delay(2000);

  int savedTeam = EEPROM.read(0);

  if (savedTeam < 1 || savedTeam > 5) {
    savedTeam = TEAM_MCLAREN;   // Default McLaren
    EEPROM.write(0, savedTeam);
    EEPROM.commit();
  }

  applyTeamColor(savedTeam);
}

// ==========================================================

void loop() {

  if (!raceFinished) {

    if (millis() - lastCheck > interval) {
      lastCheck = millis();
      fetchRaceData();
    }

    pulseEffect();
    delay(30);
  }
}

// ==========================================================
// WIFI
// ==========================================================

void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

// ==========================================================
// FETCH F1 DATA
// ==========================================================

void fetchRaceData() {

  HTTPClient http;
  http.begin("http://ergast.com/api/f1/current/last/results.json");

  int httpCode = http.GET();

  if (httpCode == 200) {

    String payload = http.getString();

    StaticJsonDocument<4096> doc;
    deserializeJson(doc, payload);

    const char* status =
      doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"];

    const char* team =
      doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];

    int teamID = getTeamID(String(team));

    applyTeamColor(teamID);

    if (String(status) == "Finished") {

      raceFinished = true;

      checkeredWipe();

      EEPROM.write(0, teamID);
      EEPROM.commit();

      applyTeamColor(teamID);  // final solid winner color
    }
  }

  http.end();
}

// ==========================================================
// TEAM MAPPING
// ==========================================================

int getTeamID(String team) {

  if (team == "Red Bull") return TEAM_REDBULL;
  if (team == "Ferrari") return TEAM_FERRARI;
  if (team == "Mercedes") return TEAM_MERCEDES;
  if (team == "McLaren") return TEAM_MCLAREN;
  if (team == "Aston Martin") return TEAM_ASTON;

  return TEAM_MCLAREN;
}

// ==========================================================
// APPLY TEAM COLOR (FADE)
// ==========================================================

void applyTeamColor(int teamID) {

  switch (teamID) {
    case TEAM_REDBULL:   setTargetColor(0, 0, 255); break;
    case TEAM_FERRARI:   setTargetColor(255, 0, 0); break;
    case TEAM_MERCEDES:  setTargetColor(0, 255, 200); break;
    case TEAM_MCLAREN:   setTargetColor(255, 80, 0); break;
    case TEAM_ASTON:     setTargetColor(0, 100, 0); break;
  }

  smoothFade();
}

// ==========================================================
// SMOOTH FADE
// ==========================================================

void setTargetColor(int r, int g, int b) {
  targetR = r;
  targetG = g;
  targetB = b;
}

void smoothFade() {

  int steps = 40;

  for (int i = 0; i <= steps; i++) {

    int r = currentR + (targetR - currentR) * i / steps;
    int g = currentG + (targetG - currentG) * i / steps;
    int b = currentB + (targetB - currentB) * i / steps;

    for (int j = 0; j < NUM_LEDS; j++) {
      strip.setPixelColor(j, strip.Color(r, g, b));
    }

    updateBrightness();
    strip.show();
    delay(20);
  }

  currentR = targetR;
  currentG = targetG;
  currentB = targetB;
}

// ==========================================================
// PULSE WHILE LIVE
// ==========================================================

void pulseEffect() {

  static float factor = 0.3;
  static bool rising = true;

  if (rising) {
    factor += 0.01;
    if (factor >= 1.0) rising = false;
  } else {
    factor -= 0.01;
    if (factor <= 0.3) rising = true;
  }

  int r = currentR * factor;
  int g = currentG * factor;
  int b = currentB * factor;

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }

  updateBrightness();
  strip.show();
}

// ==========================================================
// CHECKERED FLAG WIPE
// ==========================================================

void checkeredWipe() {

  int blockSize = 4;
  int cycles = 15;

  for (int c = 0; c < cycles; c++) {

    for (int offset = 0; offset < blockSize * 2; offset++) {

      for (int i = 0; i < NUM_LEDS; i++) {

        int pattern = ((i + offset) / blockSize) % 2;

        if (pattern == 0)
          strip.setPixelColor(i, strip.Color(240, 240, 240));
        else
          strip.setPixelColor(i, strip.Color(0, 0, 0));
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
  struct tm* timeinfo = localtime(&now);

  int hour = timeinfo->tm_hour;

  if (hour >= 7 && hour <= 19)
    strip.setBrightness(120);   // Day
  else
    strip.setBrightness(40);    // Night
}
