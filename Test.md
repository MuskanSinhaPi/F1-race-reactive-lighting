# System Test & Validation Guide

This document provides a structured validation checklist for the F1 Live Race Reactive LED Display system.

Run these tests in order before installing the system permanently.

---

## Test 1 — LED Strip Hardware Test

Purpose:
Verify that the LED strip, power supply, and wiring are functioning correctly.

Expected:
- Entire strip lights up solid red
- No flickering
- No dim or dead LEDs

```cpp
#include <Adafruit_NeoPixel.h>

#define LED_PIN D4
#define NUM_LEDS 60

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  strip.begin();
  strip.setBrightness(120);
  strip.show();

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(255, 0, 0));
  }
  strip.show();
}

void loop() {}
```

If flickering occurs:
- Ensure external 5V supply is adequate (≥4A recommended)
- Ensure common ground between NodeMCU and LED strip
- Add 1000µF capacitor across 5V and GND

---

## Test 2 — Button Test

Purpose:
Ensure the push button is wired correctly and does not trigger randomly.

Expected:
Serial Monitor prints "Pressed" only when button is physically pressed.

```cpp
#define BUTTON_PIN D5

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Pressed");
    delay(300);
  }
}
```

If random prints occur:
- Check wiring orientation of 4-pin tactile switch
- Ensure one leg is connected to GND and the opposite leg to D5

---

## Test 3 — WiFi Connection Test

Purpose:
Confirm WiFi credentials and connectivity.

Expected:
- "Connected" printed
- Local IP address displayed

```cpp
#include <ESP8266WiFi.h>

const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected");
  Serial.println(WiFi.localIP());
}

void loop() {}
```

If connection fails:
- Recheck SSID/password
- Confirm 2.4GHz WiFi network

---

## Test 4 — Jolpica API Test

Purpose:
Verify HTTPS request and API response.

Expected:
- HTTP status code 200
- JSON payload printed in Serial Monitor

```cpp
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  int httpCode = https.GET();
  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    String payload = https.getString();
    Serial.println(payload);
  }

  https.end();
}

void loop() {}
```

If HTTP code is:
- -1 → SSL or connection issue
- 404 → Endpoint incorrect
- 500 → API temporarily unavailable

---

## Test 5 — EEPROM Persistence Test

Purpose:
Verify EEPROM write and read operations.

Expected:
- Value written is correctly read back
- Value persists after power cycle

```cpp
#include <EEPROM.h>

void setup() {
  Serial.begin(115200);
  EEPROM.begin(10);

  EEPROM.write(0, 7);
  EEPROM.commit();

  int value = EEPROM.read(0);
  Serial.print("Stored Value: ");
  Serial.println(value);
}

void loop() {}
```

After verifying, power cycle the board and re-run read test.

---

## Test 6 — Brightness Time Sync Test

Purpose:
Confirm NTP time synchronization and time-based brightness logic.

Expected:
- Correct hour printed in Serial Monitor

```cpp
#include <ESP8266WiFi.h>
#include <time.h>

const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  configTime(19800, 0, "pool.ntp.org"); // IST
  delay(2000);

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  Serial.print("Current Hour: ");
  Serial.println(t->tm_hour);
}

void loop() {}
```

Verify that reported hour matches local time.

---

## Test 7 — Animation Test

Purpose:
Verify smooth fade and animation logic independently.

Expected:
- LED strip fades from Ferrari red to McLaren orange smoothly

```cpp
#include <Adafruit_NeoPixel.h>

#define LED_PIN D4
#define NUM_LEDS 60

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

int currentR=0, currentG=0, currentB=0;
int targetR=220, targetG=20, targetB=20;

void smoothFade(int r, int g, int b) {

  int steps = 40;

  for (int i = 0; i <= steps; i++) {

    int rr = currentR + (r - currentR) * i / steps;
    int gg = currentG + (g - currentG) * i / steps;
    int bb = currentB + (b - currentB) * i / steps;

    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(rr, gg, bb));

    strip.show();
    delay(20);
  }

  currentR = r;
  currentG = g;
  currentB = b;
}

void setup() {
  strip.begin();
  strip.setBrightness(120);
  strip.show();

  smoothFade(220,20,20);     // Ferrari
  delay(2000);
  smoothFade(255,120,0);     // McLaren
}

void loop() {}
```

---

## Final System Validation

Once all tests pass individually:

1. Upload final production firmware.
2. Confirm:
   - Button toggles display mode.
   - Race weekend detection works.
   - API polling works.
   - Checkered wipe triggers on race completion.
   - EEPROM stores winner.
   - System resumes correctly after power cycle.

System is ready for enclosure installation once all validation steps succeed.

# Firmware Test & Validation Guide

This document provides structured validation tests for:

- Mode cycling (13 modes)
- Display mode behavior
- Live mode immediate API fetch
- Button debounce logic
- Jolpica API integration

Run these tests before final deployment.

---

## Test 1 — Mode Cycling (Serial Only)

Purpose:

Verify that:
- Button increments mode
- Mode wraps from 12 back to 0
- Display mode is always index 0
- Live mode is always index 1

Expected Behavior:

Each button press prints the current mode to Serial Monitor.

```cpp
#define BUTTON_PIN D5

int mode = 0;
const int TOTAL_MODES = 13;
bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  printMode();
}

void loop() {
  bool currentState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentState == LOW) {
    mode++;
    if (mode >= TOTAL_MODES) mode = 0;
    printMode();
    delay(300);
  }

  lastButtonState = currentState;
}

void printMode() {
  Serial.print("Mode: ");
  Serial.println(mode);
}
```

Expected Serial Output:

Mode: 0  
Mode: 1  
Mode: 2  
...  
Mode: 12  
Mode: 0  

---

## Test 2 — Live Mode Immediate Fetch

Purpose:

Verify that entering Mode 1 immediately triggers API fetch.

Expected Behavior:

When mode changes to 1:
- API is called immediately
- Leader and status printed to Serial

```cpp
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#define BUTTON_PIN D5

const char* ssid = "YOUR_WIFI";
const char* password = "YOUR_PASSWORD";

int mode = 0;
const int TOTAL_MODES = 13;
bool lastButtonState = HIGH;

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  printMode();
}

void loop() {
  bool currentState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentState == LOW) {
    mode++;
    if (mode >= TOTAL_MODES) mode = 0;

    printMode();

    if (mode == 1) {
      fetchLiveResults();
    }

    delay(300);
  }

  lastButtonState = currentState;
}

void printMode() {
  Serial.print("Mode: ");
  Serial.println(mode);
}

void fetchLiveResults() {

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://api.jolpi.ca/ergast/f1/current/last/results.json");

  int httpCode = https.GET();
  Serial.print("HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    String payload = https.getString();
    StaticJsonDocument<4096> doc;

    if (!deserializeJson(doc, payload)) {
      const char* status =
        doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["status"];

      const char* team =
        doc["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Constructor"]["name"];

      Serial.print("Leader: ");
      Serial.println(team);

      Serial.print("Status: ");
      Serial.println(status);
    }
  }

  https.end();
}
```

Expected Serial Output When Entering Mode 1:

Mode: 1  
HTTP Code: 200  
Leader: McLaren  
Status: Finished  

---

## Test 3 — Button Stability Test

Purpose:

Ensure button does not double-trigger and debounce works correctly.

Expected Behavior:

Each physical press increments exactly once.

```cpp
#define BUTTON_PIN D5

bool lastButtonState = HIGH;
int pressCount = 0;

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {

  bool currentState = digitalRead(BUTTON_PIN);

  if (lastButtonState == HIGH && currentState == LOW) {
    pressCount++;
    Serial.print("Press Count: ");
    Serial.println(pressCount);
    delay(300);
  }

  lastButtonState = currentState;
}
```

If multiple increments occur per press:

- Increase debounce delay
- Check wiring
- Verify button orientation

---

## Test 4 — Full Mode Architecture Validation

Purpose:

Verify correct behavior sequence:

- Mode 0 → Display
- Mode 1 → Live + immediate fetch
- Mode 2–12 → Manual override
- Wrap back to 0

Validation Checklist:

- Button cycles through all 13 modes
- Mode 1 triggers immediate fetch
- No skipped modes
- No double triggers
- Wrap works correctly

---
# Nano Diagnostic Tests

## Test 1 – Fade Verification

Upload to Nano to validate smooth fades before connecting NodeMCU.

```cpp
#include <Adafruit_NeoPixel.h>

#define STRIP_PIN  6
#define NUM_LEDS  60

Adafruit_NeoPixel strip(NUM_LEDS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

int currentR = 0, currentG = 0, currentB = 0;

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(120);
  strip.show();
  Serial.println("Fade test starting...");
}

void loop() {
  Serial.println("→ McLaren (orange)");
  smoothFade(255, 120, 0);
  delay(2000);

  Serial.println("→ Mercedes (teal)");
  smoothFade(0, 255, 210);
  delay(2000);

  Serial.println("→ Ferrari (red)");
  smoothFade(220, 20, 20);
  delay(2000);

  Serial.println("→ Red Bull (blue)");
  smoothFade(10, 60, 200);
  delay(2000);

  Serial.println("→ Display white");
  smoothFade(180, 180, 170);
  delay(2000);
}

void smoothFade(int tr, int tg, int tb) {
  int steps = 40;
  for (int i = 0; i <= steps; i++) {
    int r = currentR + (tr - currentR) * i / steps;
    int g = currentG + (tg - currentG) * i / steps;
    int b = currentB + (tb - currentB) * i / steps;
    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(r, g, b));
    strip.show();
    delay(20);
  }
  currentR = tr;
  currentG = tg;
  currentB = tb;
  Serial.println("Fade done");
}
```

Confirm:
- Smooth transitions
- No flicker
- No brownouts

---

## Test 2 – Manual Mode Testing

Upload Serial-controlled test to Nano.

```cpp
#include <Adafruit_NeoPixel.h>

#define STRIP_PIN  6
#define NUM_LEDS  60

Adafruit_NeoPixel strip(NUM_LEDS, STRIP_PIN, NEO_GRB + NEO_KHZ800);

int currentR = 0, currentG = 0, currentB = 0;
bool pulsing      = false;
float pulseFactor = 0.3;
bool pulseRising  = true;
int pulseR = 0, pulseG = 0, pulseB = 0;
unsigned long lastPulse = 0;

struct Color { int r, g, b; };
Color teamColors[] = {
  {  0,   0,   0},  // 0 unused
  {220,  20,  20},  // 1  Ferrari
  { 20,  40, 150},  // 2  Alpine
  {  0, 130,  90},  // 3  Aston
  {220,   0,  45},  // 4  Haas
  {180,   0,   0},  // 5  Audi
  {180, 180, 180},  // 6  Cadillac
  {255, 120,   0},  // 7  McLaren
  {  0, 255, 210},  // 8  Mercedes
  { 40,  60, 200},  // 9  Racing Bulls
  { 10,  60, 200},  // 10 Red Bull
  { 20,  40, 180},  // 11 Williams
};

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(120);
  strip.show();

  Serial.println("=== MODE TEST ===");
  Serial.println("Send a number:");
  Serial.println("  0  = Display white");
  Serial.println("  1  = Ferrari");
  Serial.println("  2  = Alpine");
  Serial.println("  3  = Aston Martin");
  Serial.println("  4  = Haas");
  Serial.println("  5  = Audi");
  Serial.println("  6  = Cadillac");
  Serial.println("  7  = McLaren");
  Serial.println("  8  = Mercedes");
  Serial.println("  9  = Racing Bulls");
  Serial.println("  10 = Red Bull");
  Serial.println("  11 = Williams");
  Serial.println("  99 = Checkered wipe");
  Serial.println("  255 = Pulse current color");
}

void loop() {
  // Serial input
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    int cmd = input.toInt();
    Serial.printf("Command: %d\n", cmd);
    handleCommand(cmd);
  }

  // Pulse
  if (pulsing && millis() - lastPulse > 30) {
    lastPulse = millis();
    doPulse();
  }
}

void handleCommand(int cmd) {
  if (cmd == 0) {
    pulsing = false;
    smoothFade(180, 180, 170);
    strip.setBrightness(150);
    strip.show();
    Serial.println("Display white");

  } else if (cmd >= 1 && cmd <= 11) {
    pulsing = false;
    Color c = teamColors[cmd];
    smoothFade(c.r, c.g, c.b);
    pulseR = c.r; pulseG = c.g; pulseB = c.b;

  } else if (cmd == 99) {
    pulsing = false;
    Serial.println("Checkered wipe");
    checkeredWipe();
    Serial.println("Wipe done");

  } else if (cmd == 255) {
    pulsing = true;
    Serial.println("Pulsing started");

  } else {
    Serial.println("Unknown command");
  }
}

void smoothFade(int tr, int tg, int tb) {
  int steps = 40;
  for (int i = 0; i <= steps; i++) {
    int r = currentR + (tr - currentR) * i / steps;
    int g = currentG + (tg - currentG) * i / steps;
    int b = currentB + (tb - currentB) * i / steps;
    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(r, g, b));
    strip.show();
    delay(20);
  }
  currentR = tr;
  currentG = tg;
  currentB = tb;
}

void doPulse() {
  if (pulseRising) {
    pulseFactor += 0.01;
    if (pulseFactor >= 1.0) pulseRising = false;
  } else {
    pulseFactor -= 0.01;
    if (pulseFactor <= 0.3) pulseRising = true;
  }
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(
      pulseR * pulseFactor,
      pulseG * pulseFactor,
      pulseB * pulseFactor));
  strip.show();
}

void checkeredWipe() {
  for (int c = 0; c < 15; c++) {
    for (int offset = 0; offset < 8; offset++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        int pattern = ((i + offset) / 4) % 2;
        strip.setPixelColor(i, pattern == 0
          ? strip.Color(240, 240, 240)
          : strip.Color( 30,  30,  30));
      }
      strip.show();
      delay(80);
    }
  }
}
```

Send:

- 7 → McLaren
- 255 → Pulse
- 1 → Ferrari
- 99 → Checkered
- 0 → Display white

Verify smooth operation across all colors.
------

## Final Deployment Validation

Before installing permanently:

- Confirm WiFi connects reliably
- Confirm HTTPS returns 200
- Confirm JSON parses correctly
- Confirm button stable
- Confirm no memory crashes
- Confirm mode 1 fetches immediately
- Confirm wrap-around behavior

System is ready for production firmware once all tests pass.
