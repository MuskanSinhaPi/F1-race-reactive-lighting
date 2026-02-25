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

## Team Color Reference

| Team | Primary Color | Secondary Color |
|------|---------------|----------------|
| Ferrari | ![#da291c](https://img.shields.io/badge/-Ferrari-da291c?style=for-the-badge) | ![#ffffff](https://img.shields.io/badge/-ffffff-ffffff?style=for-the-badge&logoColor=black) |
| Alpine | ![#061a4d](https://img.shields.io/badge/-Alpine-061a4d?style=for-the-badge) | ![#FF88BD](https://img.shields.io/badge/-FF88BD-FF88BD?style=for-the-badge) |
| Aston Martin | ![#00665e](https://img.shields.io/badge/-Aston_Martin-00665e?style=for-the-badge) | ![#b9c600](https://img.shields.io/badge/-b9c600-b9c600?style=for-the-badge) |
| Haas | ![#e6002d](https://img.shields.io/badge/-Haas-e6002d?style=for-the-badge) | ![#000000](https://img.shields.io/badge/-000000-000000?style=for-the-badge) |
| Audi | ![#101319](https://img.shields.io/badge/-Audi-101319?style=for-the-badge) | ![#ffffff](https://img.shields.io/badge/-ffffff-ffffff?style=for-the-badge&logoColor=black) |
| Cadillac | ![#ffffff](https://img.shields.io/badge/-Cadillac-ffffff?style=for-the-badge&logoColor=black) | ![#000000](https://img.shields.io/badge/-000000-000000?style=for-the-badge) |
| McLaren | ![#ff8000](https://img.shields.io/badge/-McLaren-ff8000?style=for-the-badge) | ![#ffffff](https://img.shields.io/badge/-ffffff-ffffff?style=for-the-badge&logoColor=black) |
| Mercedes | ![#00f5d0](https://img.shields.io/badge/-Mercedes-00f5d0?style=for-the-badge) | ![#00a39e](https://img.shields.io/badge/-00a39e-00a39e?style=for-the-badge) |
| Racing Bulls | ![#070b36](https://img.shields.io/badge/-Racing_Bulls-070b36?style=for-the-badge) | ![#ffffff](https://img.shields.io/badge/-ffffff-ffffff?style=for-the-badge&logoColor=black) |
| Red Bull Racing | ![#00162b](https://img.shields.io/badge/-Red_Bull-00162b?style=for-the-badge) | ![#db0a40](https://img.shields.io/badge/-db0a40-db0a40?style=for-the-badge) |
| Williams | ![#000a20](https://img.shields.io/badge/-Williams-000a20?style=for-the-badge) | ![#2270ff](https://img.shields.io/badge/-2270ff-2270ff?style=for-the-badge) |

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
