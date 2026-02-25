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
