# Great question — this gets into **how the display controller actually boots**.

### What `#define TFT_RST -1` means

In the **Adafruit ST7735 library**, the reset pin is optional.

When you write:

```c
#define TFT_RST -1
```

you are telling the library:

> "There is **no reset GPIO connected**, so don't try to control one."

Inside the library code, it basically does something like:

```c
if (rstPin != -1) {
    digitalWrite(rstPin, LOW);
    delay(10);
    digitalWrite(rstPin, HIGH);
}
```

So when `-1` is used:
- The **software reset step is skipped**
- The display relies on its **power-on reset instead**

### Why connecting RESET to 3.3V works

The **RESET pin on the display is active-LOW**.

| Reset Pin State | Meaning |
|------------------|---------|
| LOW              | Reset the display controller |
| HIGH             | Normal operation |

So when you connect:

```plaintext
RESET → 3.3V
```
the display is **held in the non-reset state**.

When the board powers up:
1. Power rises from **0V → 3.3V**
2. The controller internally resets
3. RESET becomes HIGH
4. The display starts normally

This is called a **power-on reset**.

Most display controllers like:
- **ST7735**
- **ILI9341**
- **ST7789**
designed to handle this automatically.

### Why libraries sometimes toggle RESET anyway
The manual reset is useful when:
- the display **locks up**
- you **reinitialize it without rebooting the MCU**
- SPI communication gets corrupted 
the reset sequence forces the controller into a known state.
But in many small projects, it isn't necessary.

### Why your display still worked without a reset pulse

Because the **initialization commands sent over SPI** already configure everything:

sleep exit

pixel format

frame rate

color mode

display on

So the sequence becomes:

Power on

↓

Display internal reset

↓

ESP8266 sends initialization commands

↓

Display works

### When you *would* need a reset pin

You'd want a reset pin if:

-   the display **sometimes shows garbage after reboot**

-   you **reset the ESP8266 but not the display**

-   the display **shares SPI with other devices**

-   you want **very reliable startup**

1. **Strip blinking on boot for ~3 mins
This is the NodeMCU sending commands before the Nano is ready, and also the NTP sync loop blocking while Nano sits idle receiving garbage. Fix: add a startup delay on Nano before it starts listening, and send an initial command only after NTP sync completes.

2. **3.3kΩ is too high for a data line — 
it slows signal edges. Remove it or replace with 330Ω max. The blinking is likely the resistor corrupting the NeoPixel signal at higher speeds.

Several issues to fix. Let me address them one by one:

---

**1. Strip blinking on boot for ~3 mins**

This is the NodeMCU sending commands before the Nano is ready, and also the NTP sync loop blocking while Nano sits idle receiving garbage. Fix: add a startup delay on Nano before it starts listening, and send an initial command only after NTP sync completes.

**2. Race finished but still showing McLaren / "Race in Progress"**

Two bugs:
- `fetchRaceData()` uses `current/last/results.json` which returns the last race. On race day it should work, but the round guard might be blocking it if `currentRound` doesn't match
- At boot on a finished race day, `applyMode()` never calls `fetchRaceData()` — it just shows the EEPROM saved team. Need to fetch results on boot if it's race Sunday

**3. 3.3kΩ resistor causing issues**

3.3kΩ is too high for a data line — it slows signal edges. Remove it or replace with 330Ω max. The blinking is likely the resistor corrupting the NeoPixel signal at higher speeds.

---

Here's the corrected full code:

```cpp
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
const char* ssid     = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

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

// ================= TFT COLOR CONSTANTS =================
// Defined as constants to avoid calling tft.color565() in global scope
#define C_BG        0x0801
#define C_PANEL     0x1093
#define C_BORDER    0x2A16
#define C_WHITE     0xFFFF
#define C_GREY      0x7BCF
#define C_RED       0xD800
#define C_GREEN     0x0650
#define C_YELLOW    0xFF80
#define C_DIM       0x3A16
#define C_BLACK     0x0000
#define C_ORANGE    0xFB00
#define C_TEAL      0x0697

// ================= TEAM TFT COLORS =================
uint16_t teamTFTColor[] = {
  C_BLACK,   // 0 unused
  0xD800,    // 1  Ferrari      red
  0x1252,    // 2  Alpine       blue
  0x0494,    // 3  Aston        green
  0xD805,    // 4  Haas         red-pink
  0xB000,    // 5  Audi         dark red
  0xBDD7,    // 6  Cadillac     silver
  0xFB00,    // 7  McLaren      orange
  0x0697,    // 8  Mercedes     teal
  0x2832,    // 9  Racing Bulls blue
  0x0A32,    // 10 Red Bull     blue
  0x1496,    // 11 Williams     blue
};

const char* teamNames[] = {
  "", "FERRARI", "ALPINE", "ASTON MR",
  "HAAS", "AUDI", "CADILLAC", "McLAREN",
  "MERCEDES", "RB", "RED BULL", "WILLIAMS"
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
int  lastSentTeam  = -1;
int  currentTeamID = TEAM_MCLAREN;

char raceName[64]     = "---";
char p1Team[32]       = "---";
char p1Status[16]     = "---";
time_t raceStartEpoch = 0;

// ==========================================================
// SETUP
// ==========================================================

void setup() {
  Serial.begin(115200);
  nanoSerial.begin(9600);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  EEPROM.begin(10);

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

  // Wait for Nano to be ready before sending anything
  drawStatusLine("Waiting Nano...", C_YELLOW);
  delay(2000); // give Nano time to boot and init strip
  nanoReady = true;

  int savedMode = EEPROM.read(1);
  if (savedMode < 0 || savedMode >= MODE_COUNT) savedMode = MODE_LIVE;
  currentMode = savedMode;

  int savedTeam = EEPROM.read(0);
  if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

  drawMainScreen();

  // On race Sunday, always fetch results first before applying mode
  // This ensures finished races show correct winner at boot
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
  tft.fillScreen(C_BG);

  // Header
  tft.fillRect(0, 0, 160, 18, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("F1 LIVE");
  tft.setCursor(100, 5);
  tft.print("--:--:--");

  tft.drawFastHLine(0, 18, 160, C_BORDER);

  // Race name
  tft.fillRect(0, 19, 160, 14, C_PANEL);
  tft.setTextColor(C_GREY);
  tft.setTextSize(1);
  tft.setCursor(4, 24);
  // Truncate race name to fit
  char shortName[22];
  strncpy(shortName, raceName, 21);
  shortName[21] = '\0';
  tft.print(shortName);

  // P1 team stripe + panel
  tft.fillRect(0, 33, 4, 40, teamTFTColor[currentTeamID]);
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
  if (raceFinished)
    tft.print("RACE COMPLETE");
  else if (raceSunday)
    tft.print("LIGHTS OUT: --:--:--");
  else
    tft.print("NO RACE TODAY");

  // Footer
  tft.drawFastHLine(0, 115, 160, C_BORDER);
  tft.fillRect(0, 116, 160, 12, C_PANEL);
  tft.setTextColor(C_DIM);
  tft.setCursor(4, 119);
  tft.print("Round ");
  tft.print(currentRound > 0 ? currentRound : 0);
  tft.setCursor(90, 119);
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
  tft.fillRect(96, 2, 62, 14, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(98, 5);
  tft.print(buf);
}

// ==========================================================
// COUNTDOWN
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
// UPDATE TEAM DISPLAY
// ==========================================================

void updateTeamDisplay(int teamID) {
  currentTeamID = teamID;
  tft.fillRect(0, 33, 4, 40, teamTFTColor[teamID]);
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
// RACE FINISHED SCREEN
// ==========================================================

void drawRaceFinished(int teamID) {
  for (int i = 0; i < 3; i++) {
    tft.fillRect(0, 0, 160, 18, C_WHITE);
    delay(200);
    tft.fillRect(0, 0, 160, 18, C_BLACK);
    delay(200);
  }
  tft.fillRect(0, 0, 160, 18, C_RED);
  tft.setTextColor(C_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 5);
  tft.print("RACE FINISHED!");
  updateClock();

  tft.fillRect(0, 33, 160, 40, teamTFTColor[teamID]);
  tft.setTextColor(C_BLACK);
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
  if (!nanoReady) return; // block all sends until Nano is ready
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
    updateTeamDisplay(TEAM_MCLAREN);
    updateStatus("DISPLAY", C_GREY);
    return;
  }

  if (mode == MODE_LIVE) {
    raceFinished = false;
    lastSentTeam = -1;

    int savedTeam = EEPROM.read(0);
    if (savedTeam < 1 || savedTeam > 11) savedTeam = TEAM_MCLAREN;

    sendToNano(savedTeam);
    updateTeamDisplay(savedTeam);

    if (raceSunday) {
      sendToNano(CMD_PULSE);
      updateStatus("LIVE", C_GREEN);
      fetchRaceData(); // always fetch on entering live mode on race day
    } else {
      updateStatus("NO RACE", C_GREY);
    }
    return;
  }

  // Manual team modes
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
  updateStatus("FETCHING", C_YELLOW);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  if (https.GET() == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, https.getString());

    auto race          = doc["MRData"]["RaceTable"]["Races"][0];
    const char* roundStr = race["round"];
    const char* status   = race["Results"][0]["status"];
    const char* team     = race["Results"][0]["Constructor"]["name"];

    if (!status || !team) {
      updateStatus("NO DATA", C_GREY);
      https.end();
      return;
    }

    // Round guard — only act on current round results
    if (roundStr && currentRound > 0) {
      int resultRound = atoi(roundStr);
      if (resultRound != currentRound) {
        Serial.printf("Results round %d, expected %d — skipping\n",
                      resultRound, currentRound);
        updateStatus("WAITING", C_YELLOW);
        https.end();
        return;
      }
    }

    strncpy(p1Team,   team,   sizeof(p1Team)   - 1);
    strncpy(p1Status, status, sizeof(p1Status) - 1);

    int teamID = getTeamID(String(team));
    Serial.printf("P1: %s | %s\n", team, status);

    // Update display and strip regardless of whether team changed
    // so boot always shows correct state
    updateTeamDisplay(teamID);

    if (teamID != lastSentTeam) {
      sendToNano(teamID);
      if (!raceFinished) sendToNano(CMD_PULSE);
      lastSentTeam = teamID;
    }

    if (String(status) == "Finished") {
      raceFinished = true;
      EEPROM.write(0, teamID);
      EEPROM.commit();

      // Update countdown row to show race complete
      tft.fillRect(4, 103, 155, 11, C_PANEL);
      tft.setTextColor(C_GREEN);
      tft.setTextSize(1);
      tft.setCursor(4, 106);
      tft.print("RACE COMPLETE");

      updateStatus("FINISHED", C_GREEN);
      sendToNano(CMD_CHECKERED);
      sendToNano(teamID);
      drawRaceFinished(teamID);

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
```

---

**Summary of all fixes:**

| Issue | Fix |
|---|---|
| Strip blinking on boot | `nanoReady` flag — blocks all `sendToNano()` calls until after 2s Nano boot delay |
| Wrong team color at boot on race day | `fetchRaceData()` called directly in `setup()` on race Sunday before `applyMode()` |
| Race in progress after finish | Round guard now logs and skips correctly; `updateTeamDisplay()` always called regardless of `lastSentTeam` |
| `tft.color565()` in global array | Replaced with pre-calculated RGB565 hex constants — fixes potential init-order crash |
| 3.3kΩ resistor | Remove it or replace with 330Ω max |

---
## System Architecture

Two microcontrollers with distinct responsibilities:

```
[Internet] → [NodeMCU] → (serial) → [Nano] → [LED Strip]
                ↕                      ↑
            [TFT Screen]           [Button on NodeMCU]
```

---

## NodeMCU — Brain

### Boot sequence (`setup()`)

1. Initialises TFT, draws boot screen
2. Connects WiFi — blocking loop, won't proceed until connected
3. NTP sync — polls `time()` until epoch > 100000 (year 1970 sanity check), confirms real time received from `pool.ntp.org` at IST offset (+19800 seconds = +5:30)
4. `detectNextRace()` — hits `jolpi.ca/ergast/f1/current/next.json`, parses race name, date, time, round. Converts race datetime to `time_t epoch`. Checks if today is Sunday (`tm_wday == 0`) and sets `raceSunday` flag
5. Waits 2 seconds with `nanoReady = false` — all `sendToNano()` calls are silently blocked during this window, preventing garbage from hitting Nano while it's still booting and initialising the strip
6. Reads `currentMode` from EEPROM slot 1, `currentTeamID` from slot 0
7. If `raceSunday && MODE_LIVE` → calls `fetchRaceData()` directly instead of `applyMode()` — this ensures a finished race shows the correct winner immediately at boot without waiting 2 minutes for the polling interval

---

### Main loop (`loop()`)

Runs every ~50ms. Four things happen:

**`checkButton()`** — reads D3 with internal pull-up. Button press pulls pin LOW. Detects falling edge (HIGH→LOW transition). 300ms debounce via `millis()` comparison. Increments `currentMode` modulo 13, saves to EEPROM, calls `applyMode()` and `updateModeDisplay()`

**`maintainWiFi()`** — every 30 seconds checks `WiFi.status()`. If disconnected, calls `connectWiFi()` which blocks until reconnected. Updates TFT status field during reconnect

**`checkLightsOutCountdown()`** — only active on race Sunday before race finish. Computes `difftime(raceStartEpoch, now)`. When diff hits ≤5 seconds, fires `CMD_LIGHTS_OUT` (77) to Nano once via `lightsOutTriggered` flag. Flashes screen red 5 times then redraws main screen

**API polling** — only in `MODE_LIVE` on race Sunday with race not finished. Every 120 seconds calls `fetchRaceData()`

**Clock update** — every 1000ms redraws the time in the TFT header

---

### `fetchRaceData()`

Hits `current/last/results.json`. Parses P1 constructor name and status.

**Date guard** — compares `race["date"]` from the API response against today's date from NTP. If they don't match (meaning the API is returning last week's race) it skips entirely and shows "WAITING". This replaces the old round number guard which caused mismatches at race end.

If date matches:
- Gets team name → `getTeamID()` → integer 1–11
- Calls `updateTeamDisplay()` always (ensures correct state on boot regardless of `lastSentTeam`)
- If team changed since last send → `sendToNano(teamID)` + `sendToNano(CMD_PULSE)`
- If `status == "Finished"` → sets `raceFinished = true`, saves winner to EEPROM slot 0, sends `CMD_CHECKERED` (99) then `teamID` to Nano, calls `drawRaceFinished()` on TFT

---

### `applyMode()`

Maps the 13 mode values to actions:

- `MODE_DISPLAY (0)` → sends `CMD_DISPLAY (0)` to Nano, shows neutral white display
- `MODE_LIVE (1)` → reads last winner from EEPROM, sends team color, sends `CMD_PULSE` if race Sunday, calls `fetchRaceData()`
- `MODE_FERRARI (2)` through `MODE_WILLIAMS (12)` → `teamID = mode - 1`, sends team ID directly to Nano

---

### TFT display

160×128 pixels, landscape. Screen is divided into fixed pixel regions, each updated independently without full redraws:

| Region | Y pixels | Content |
|---|---|---|
| Header | 0–18 | "F1 LIVE" + live clock |
| Race name | 19–33 | Grand Prix name (truncated 21 chars) |
| P1 panel | 33–73 | Team color stripe + constructor name |
| Status | 73–87 | LIVE / NO RACE / FETCHING etc |
| Mode | 87–101 | Current mode name |
| Countdown | 101–115 | Lights out timer / race complete |
| Footer | 116–128 | Round number + API source |

Colors are pre-calculated RGB565 hex constants — `tft.color565()` can't be called at global scope before `tft.initR()` runs, so raw hex values are used instead.

---

## Nano — LED Controller

### Boot (`setup()`)

- `pinMode(ESP_RX_PIN, INPUT)` set explicitly before `SoftwareSerial.begin()` — critical, prevents pin fighting NodeMCU during boot
- Initialises strip, sets brightness 120
- Fades to default McLaren orange, sets pulse variables, starts pulsing

---

### Main loop

Two things run:

**Serial listener** — `espSerial.available()` checks SoftwareSerial buffer. Reads until `\n` newline character, trims whitespace, converts to integer, passes to `handleCommand()`

**Pulse animator** — if `pulsing == true`, runs `doPulse()` every 30ms via `millis()` — non-blocking, no `delay()`, so serial is never missed during animation

---

### `handleCommand()`

| Command | Action |
|---|---|
| 1–11 | Stop pulsing, `smoothFade()` to team color, store color in `pulseR/G/B` for later pulse use |
| 0 | Stop pulsing, fade to warm white (180,180,170), brightness 150 |
| 99 | Stop pulsing, run `checkeredWipe()` |
| 255 | Set `pulsing = true` — starts pulsing whatever color is currently stored in `pulseR/G/B` |
| 77 | Lights out — handled same as pulse start (you can customise this further) |

---

### `smoothFade()`

Interpolates linearly between `currentR/G/B` and target `tr/tg/tb` over 40 steps with 20ms delay each = ~800ms total fade. Updates `currentR/G/B` at end so next fade starts from correct position.

### `doPulse()`

Modulates brightness via a `pulseFactor` float between 0.3 and 1.0, incrementing/decrementing by 0.01 each call. At 30ms interval, one full pulse cycle (0.3→1.0→0.3) takes about 2.1 seconds. Multiplies stored `pulseR/G/B` by factor and writes to all LEDs.

### `checkeredWipe()`

15 cycles × 8 offsets × 80ms = ~9.6 seconds total animation. Each frame shifts a 4-LED block pattern across the strip by incrementing `offset`, creating a scrolling black-and-white checkered flag effect.

---

## EEPROM Layout

| Slot | Value |
|---|---|
| 0 | Last race winner team ID (1–11) |
| 1 | Last saved mode (0–12) |

Both persist across power cycles. On boot, slot 0 restores the LED color shown before the Nano receives its first serial command. Slot 1 restores the mode so the system boots exactly as it was left.

---

## Serial Protocol (NodeMCU → Nano)

One-way only. NodeMCU transmits, Nano receives. Each message is an ASCII integer followed by `\n`. Nano reads with `readStringUntil('\n')` and converts with `toInt()`.

SoftwareSerial at 9600 baud on NodeMCU D8 → 2.2kΩ resistor → Nano pin 8. The resistor protects Nano's 5V RX pin from NodeMCU's 3.3V TX — technically the direction (3.3V into 5V input) is safe without a resistor since 3.3V is within the logic LOW/HIGH threshold of a 5V AVR, but the resistor adds protection against any accidental voltage spikes.
