# F1 Live Leader NeoPixel Display

## Overview

This project is an IoT-based ambient display system built using a **NodeMCU (ESP8266)** and a **1-meter WS2812B NeoPixel LED strip**.

The system fetches live Formula 1 race results from the Ergast API and dynamically updates LED colors to represent the leading constructor during a race.

At race completion, a checkered flag animation is triggered, and the winning constructor's color is stored in EEPROM so that the display persists until the next race.

The project is fully standalone and does not require a backend server.

---

## Features

- Fetches live race results from Ergast F1 API
- Displays leading constructor color during race
- Smooth fade transitions between teams
- Subtle pulse animation while race is live
- Checkered flag wipe animation on race completion
- Winner color persists using EEPROM
- Automatic brightness adjustment based on time of day (IST)
- Default boot state set to McLaren if EEPROM is uninitialized

---

## Hardware Requirements

- NodeMCU (ESP8266)
- 1m WS2812B LED strip (60 LEDs recommended)
- External 5V power supply (minimum 4A recommended)
- 330Ω resistor (data line protection)
- 1000µF capacitor across 5V and GND (recommended)
- Jumper wires

---

## Wiring

| NodeMCU Pin | LED Strip |
|-------------|-----------|
| D4 (GPIO2)  | DIN (via 330Ω resistor) |
| GND         | GND |
| External 5V | 5V |

**Important:**
- The external power supply GND must be connected to NodeMCU GND.
- Do not power the LED strip directly from the NodeMCU 5V pin.

---

## Software Requirements

Install in Arduino IDE:

- ESP8266 Board Package
- ESP8266WiFi
- ESP8266HTTPClient
- ArduinoJson
- Adafruit NeoPixel
- EEPROM (built-in)

---

## API Used

Ergast Developer API (free, no API key required):
