#include <Adafruit_NeoPixel.h>
#include <SoftwareSerial.h>

#define STRIP_PIN   6
#define NUM_LEDS    125
#define ESP_RX_PIN  8
#define ESP_TX_PIN  9

#define CMD_DISPLAY      0
#define CMD_PULSE      255
#define CMD_CHECKERED   99
#define CMD_LIGHTS_OUT  77

Adafruit_NeoPixel strip(NUM_LEDS, STRIP_PIN, NEO_GRB + NEO_KHZ800);
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);

// ================= COLOR TRACKING =================
int currentR = 0, currentG = 0, currentB = 0;
int currentTeam = -1;

// ================= PULSE =================
bool pulsing      = false;
float pulseFactor = 0.3;
bool pulseRising  = true;
int pulseR = 0, pulseG = 0, pulseB = 0;
unsigned long lastPulse = 0;

// ================= TEAM COLORS (UPDATED) =================
struct Color { int r, g, b; };
Color teamColors[] = {
  {  0,   0,   0},
  {220,   0,   0},  // 1 Ferrari
  {  0,  90, 255},  // 2 Alpine
  {  0, 100,  60},  // 3 Aston Martin
  {180, 180, 180},  // 4 Haas (soft white)
  {160,   0,   0},  // 5 Audi
  {200, 200, 210},  // 6 Cadillac (soft silver)
  {255,  95,   0},  // 7 McLaren
  {  0, 210, 170},  // 8 Mercedes
  {  0,  70, 200},  // 9 Racing Bulls
  {  0,  20, 120},  // 10 Red Bull (deep navy)
  {  0, 140, 255},  // 11 Williams
};

// ==========================================================

void setup() {
  Serial.begin(115200);
  espSerial.begin(9600);

  strip.begin();
  strip.setBrightness(120);
  strip.show();

  // Default McLaren
  setTeamColor(7);
  pulsing = true;
}

// ==========================================================
  void loop() {

  // From ESP8266
  if (espSerial.available()) {
    String msg = espSerial.readStringUntil('\n');
    msg.trim();
    if (msg.length() == 0) return;

    int cmd = msg.toInt();
    Serial.print("Command Received: ");
    Serial.println(cmd);

    handleCommand(cmd);
  }

  // From USB Serial Monitor
  if (Serial.available()) {
    String msg = Serial.readStringUntil('\n');
    msg.trim();
    if (msg.length() == 0) return;

    int cmd = msg.toInt();
    Serial.print("USB Test Command: ");
    Serial.println(cmd);

    handleCommand(cmd);
  }

  if (pulsing && millis() - lastPulse > 30) {
    lastPulse = millis();
    doPulse();
  }
}

// ==========================================================
// COMMAND HANDLER
// ==========================================================

void handleCommand(int cmd) {

  if (cmd == CMD_CHECKERED) {
    pulsing = false;
    checkeredWipe();
    return;
  }

  if (cmd == CMD_LIGHTS_OUT) {
    pulsing = false;
    lightsOutAnimation();
    restoreTeamAfterAnimation();
    pulsing = true;
    return;
  }

  if (cmd == CMD_PULSE) {
    pulsing = true;
    return;
  }

  if (cmd == CMD_DISPLAY) {
    pulsing = false;
    smoothFade(200, 200, 190);  // soft white
    strip.setBrightness(150);
    return;
  }

  if (cmd >= 1 && cmd <= 11) {
    setTeamColor(cmd);
  }
}

// ==========================================================
// TEAM SETTER WITH CHANGE CHECK
// ==========================================================

void setTeamColor(int team) {

  if (team == currentTeam) return;

  currentTeam = team;

  Color c = teamColors[team];

  smoothFade(c.r, c.g, c.b);

  pulseR = c.r;
  pulseG = c.g;
  pulseB = c.b;

  applyAccent(team);
}

// ==========================================================
// ACCENTS
// ==========================================================

void applyAccent(int team) {

  if (team == 4) {          // Haas
    for (int i = 0; i < NUM_LEDS; i++)
      if (i % 12 == 0)
        strip.setPixelColor(i, strip.Color(220, 0, 0));
  }

  if (team == 6) {          // Cadillac
    for (int i = 0; i < NUM_LEDS; i++)
      if (i % 15 == 0)
        strip.setPixelColor(i, strip.Color(100, 150, 255));
  }

  if (team == 10) {         // Red Bull
    for (int i = 0; i < NUM_LEDS; i++)
      if (i % 20 == 0)
        strip.setPixelColor(i, strip.Color(220, 0, 40));
  }

  strip.show();
}

// ==========================================================
// SMOOTH FADE
// ==========================================================

void smoothFade(int tr, int tg, int tb) {

  int steps = 40;

  for (int i = 0; i <= steps; i++) {

    int r = currentR + (tr - currentR) * i / steps;
    int g = currentG + (tg - currentG) * i / steps;
    int b = currentB + (tb - currentB) * i / steps;

    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(r, g, b));

    strip.show();
    delay(15);
  }

  currentR = tr;
  currentG = tg;
  currentB = tb;
}

// ==========================================================
// PULSE
// ==========================================================

void doPulse() {

  if (pulseRising) {
    pulseFactor += 0.02;
    if (pulseFactor >= 1.0) pulseRising = false;
  } else {
    pulseFactor -= 0.02;
    if (pulseFactor <= 0.3) pulseRising = true;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(
      pulseR * pulseFactor,
      pulseG * pulseFactor,
      pulseB * pulseFactor));
  }

  strip.show();
}

// ==========================================================
// LIGHTS OUT ANIMATION
// ==========================================================

void lightsOutAnimation() {

  for (int i = 5; i > 0; i--) {

    Serial.print("Lights Out in: ");
    Serial.println(i);

    for (int j = 0; j < NUM_LEDS; j++)
      strip.setPixelColor(j, strip.Color(255, 0, 0));

    strip.show();
    delay(300);

    strip.clear();
    strip.show();
    delay(700);
  }
}

// ==========================================================
// CHECKERED WIPE
// ==========================================================

void checkeredWipe() {

  for (int c = 0; c < 12; c++) {
    for (int offset = 0; offset < 8; offset++) {
      for (int i = 0; i < NUM_LEDS; i++) {
        int pattern = ((i + offset) / 4) % 2;
        strip.setPixelColor(i, pattern == 0
          ? strip.Color(240, 240, 240)
          : strip.Color( 20,  20,  20));
      }
      strip.show();
      delay(70);
    }
  }
  currentTeam = -1;
}

// ==========================================================
// RESTORE AFTER ANIMATION
// ==========================================================

void restoreTeamAfterAnimation() {
  setTeamColor(currentTeam);
}
