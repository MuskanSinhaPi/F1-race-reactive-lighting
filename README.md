# F1 Live Leader NeoPixel Display

## Overview

This project is a standalone embedded IoT ambient display system that visualizes live Formula 1 race leadership using a WS2812B LED strip.

The system uses:

- **NodeMCU (ESP8266)** — WiFi + API + race logic
- **Arduino Nano** — deterministic LED control
- **WS2812B NeoPixel strip (1m)** — visual output

During a race:
- The leading constructor’s color is displayed.
- Smooth fade transitions occur when P1 changes.
- A pulse effect runs while race is live.
- A lights-out countdown animation runs before start.
- A checkered wipe plays when race finishes.
- Winner is saved to EEPROM and persists after power loss.

No backend server is required.

---

# System Architecture

The final production architecture separates networking from LED timing.

```
WiFi
  │
  ▼
Jolpica F1 API
  │
  ▼
NodeMCU (ESP8266)
  ├── WiFi + HTTPS
  ├── JSON parsing
  ├── Race logic
  ├── EEPROM (winner + mode)
  └── Serial commands
        │
        ▼
Arduino Nano
  ├── Smooth fade
  ├── Pulse engine
  ├── Checkered animation
  ├── Lights-out animation
  └── Accent rendering
        │
        ▼
WS2812B LED Strip
```

This design avoids 3.3V logic-level instability by letting the **Nano drive the strip directly at 5V logic**.

---

# Features

- Live race leader display
- Sunday-only race detection
- Smooth fade between constructors
- Pulse while race status = LIVE
- Lights-out red countdown animation
- Checkered wipe on race completion
- EEPROM persistence (winner + mode)
- 13-mode manual override system
- Button debounce using `millis()`
- Automatic WiFi reconnection
- LED-optimized team colors
- Power-safe white levels
- Accent rendering for Haas, Cadillac, Red Bull

---

# Button Mode Cycle (13 Modes)

| PressMode | Function |
|-----------|----------|
| 0 | Display mode (warm white) |
| 1 | Live race mode |
| 2 | Ferrari |
| 3 | Alpine |
| 4 | Aston Martin |
| 5 | Haas |
| 6 | Audi |
| 7 | Cadillac |
| 8 | McLaren |
| 9 | Mercedes |
| 10 | Racing Bulls |
| 11 | Red Bull |
| 12 | Williams |
| → | Wraps back to 0 |

- Mode is stored in EEPROM slot 1.
- Last race winner stored in EEPROM slot 0.

---

# LED-Optimized Team Colors

| Team | RGB | Notes |
|------|-----|-------|
| Ferrari | (220, 0, 0) | Saturated red |
| Alpine | (0, 90, 255) | Bright blue |
| Aston Martin | (0, 100, 60) | Racing green |
| Haas | (180, 180, 180) | Soft white + red accents |
| Audi | (160, 0, 0) | Deep red |
| Cadillac | (200, 200, 210) | Silver + blue accents |
| McLaren | (255, 95, 0) | Papaya orange |
| Mercedes | (0, 210, 170) | Teal |
| Racing Bulls | (0, 70, 200) | Bright blue |
| Red Bull | (0, 20, 120) | Navy + red accents |
| Williams | (0, 140, 255) | Royal blue |

White-based teams use reduced intensity to prevent excessive current draw.

---

# Hardware Requirements

- NodeMCU (ESP8266)
- Arduino Nano
- WS2812B LED strip (60–300 LEDs supported)
- 5V regulated power supply (≥ 4A for 60 LEDs)
- 330Ω resistor (data line)
- 1000µF capacitor across 5V & GND
- Push button
- 1kΩ resistor (NodeMCU → Nano RX protection)

---

# Wiring

## NodeMCU → Nano (Serial)

```
NodeMCU D8 → 1kΩ → Nano D8
NodeMCU GND → Nano GND
```

## Nano → LED Strip

```
Nano D6 → 330Ω → Strip DIN
Nano 5V → Strip 5V (or external PSU)
Strip GND → Nano GND
```

Important:
- All grounds must be common.
- Do NOT power Nano through VIN from LED PSU.
- Power Nano via USB separately.
- PSU 5V should feed LED strip directly.

---

# Serial Protocol

| Value | Meaning |
|-------|---------|
| 0 | Display mode |
| 1–11 | Team ID |
| 255 | Pulse |
| 99 | Checkered |
| 77 | Lights out |

---

# API Used

Jolpica (Ergast-compatible fork):

Race results:
```
https://api.jolpi.ca/ergast/f1/current/last/results.json
```

Next race:
```
https://api.jolpi.ca/ergast/f1/current/next.json
```

No API key required.

---

# Troubleshooting

## Symptom: Only First LED Turns On (NodeMCU Direct Drive)

Cause: Logic-level mismatch.

- ESP8266 outputs 3.3V logic
- WS2812 powered at 5V
- Required HIGH ≈ 3.5V+

3.3V becomes borderline.

When powered from stable 5V supply, failure becomes more obvious.

### Why Nano Pass-Through Does Not Work

WS2812 uses 800kHz protocol:

- 0 bit ≈ 0.35µs
- 1 bit ≈ 0.7µs

Arduino `digitalRead()` cannot mirror this.

This will NOT work:

```cpp
digitalWrite(6, digitalRead(2));
```

The correct solution is architectural separation:
- NodeMCU handles networking
- Nano handles LED timing

##While uploading to NodeMCU (ESP8266): _A fatal esptool.py error occurred: failed to connect to esp8266: timed out waiting for packet header_

_GPIO 0 should be pulled LOW, though a nodeMCU should do this automatically, Reset should stay HIGH, and again the nodeMCU will do an automated reset.
GPIO15 is pulled down or the board won't work, (also on the board already) Basically you should not have to connect anything special to make it work, and anything that you do connect to any of these pins (+ TX & GPIO 2) may disturb the upload process._
src: https://forum.arduino.cc/t/a-fatal-esptool-py-error-occurred-failed-to-connect-to-esp8266-timed-out-waiting-for-packet-header/1159116

---

# Production Deployment Checklist

## Firmware

- [ ] Button cycles 0–12 correctly
- [ ] Live mode fetches immediately
- [ ] Leader change triggers fade
- [ ] Race finish triggers checkered
- [ ] Winner stored to EEPROM

## API

- [ ] HTTPS returns 200
- [ ] JSON parses cleanly
- [ ] No crashes on network failure

## Electrical

- [ ] 5V supply ≥ 4A
- [ ] 330Ω data resistor installed
- [ ] 1000µF capacitor installed
- [ ] Common ground confirmed

## Stability

- [ ] 30-minute runtime stable
- [ ] No resets
- [ ] No flicker
- [ ] No WiFi drops

## Enclosure

- [ ] Ventilation adequate
- [ ] Wiring secured
- [ ] Button stable
- [ ] Final power test completed

---

# Long-Term Reliability

- Avoid unregulated adapters
- Keep firmware backed up
- Monitor API changes
- Reboot once per race weekend if desired

---

# Project Status

When all checklist items are validated, the system is ready for permanent installation.

#Edit
## 1.8" TFT Display Integration (NodeMCU ESP8266)

This project uses a **1.8" SPI TFT display (ST7735, 128×160)** to show live race information such as session status, leading constructor, and countdown timers.

The display communicates over **SPI**, which is well supported by the ESP8266.

---

## Hardware Wiring

| TFT Pin | NodeMCU |
|--------|---------|
| VCC | **3V3** |
| GND | **GND** |
| LED | **3V3** |
| SCK | **D5 (GPIO14)** |
| SDA (MOSI) | **D7 (GPIO13)** |
| CS | **D2 (GPIO4)** |
| A0 / DC | **D1 (GPIO5)** |
| RESET | **RST** or **D0** |

### Required Libraries

Install the following libraries via the Arduino Library Manager:

- `Adafruit ST7735 and ST7789 Library`
- `Adafruit GFX Library`

---

## Display Layout (160×128 Landscape)

```
┌─────────────────────────────┐
│ F1 LIVE          HH:MM:SS   │
├─────────────────────────────┤
│ Australian Grand Prix       │
├─┬───────────────────────────┤
│█│ P1 CONSTRUCTOR            │
│█│ McLAREN                   │
├─┴───────────────────────────┤
│ STATUS: LIVE                │
├─────────────────────────────┤
│ MODE: LIVE                  │
├─────────────────────────────┤
│ LIGHTS OUT: 01:23:45        │
├─────────────────────────────┤
│ Round 4          jolpi.ca   │
└─────────────────────────────┘
```

Elements displayed:

- **Header**: live clock and system status
- **Race title**: current Grand Prix
- **Team indicator**: colored stripe showing the leading constructor
- **Status line**: session state (LIVE / COMPLETE / UPCOMING)
- **Countdown timer**: time until lights out or race completion
- **Footer**: round number and API source

---

## ESP8266 Boot-Strapping Pins

Some ESP8266 pins are **boot-strapping pins**.  
When the board powers on or resets, the chip briefly reads their voltage levels to determine the **boot mode**.

### Voltage States

| State | Voltage |
|------|---------|
| HIGH | ~3.3V |
| LOW | 0V (GND) |

During the first few milliseconds after startup, these pins must be at specific levels.

### Boot Pin Requirements

| Pin | GPIO | Required State |
|----|------|---------------|
| **D3** | GPIO0 | HIGH |
| **D4** | GPIO2 | HIGH |
| **D8** | GPIO15 | LOW |

If these conditions are met, the ESP8266 **boots normally from flash memory**.

### What Happens if the States are Wrong

| Condition | Result |
|----------|--------|
| GPIO0 LOW | Device enters **programming (flash) mode** |
| GPIO2 LOW | **Boot failure** |
| GPIO15 HIGH | **Boot failure** |

Common symptoms include:

- board stuck in reset loop
- no program execution
- unusual boot messages on the serial monitor

### Why External Circuits Can Cause Issues

Connecting peripherals to these pins can unintentionally force the wrong voltage level during startup.

Example:

- **Button on D3**  
  If pressed during boot, GPIO0 becomes LOW → ESP8266 enters flash mode.

- **Device driving D8 HIGH**  
  GPIO15 must be LOW during boot → the board will fail to start.

### Why NodeMCU Usually Works Anyway

NodeMCU boards include default resistors:

| Pin | Default Pull |
|----|--------------|
| GPIO0 | Pull-up |
| GPIO2 | Pull-up |
| GPIO15 | Pull-down |

These ensure correct boot behavior **unless external hardware overrides them**.

---

## Safe Pins for Peripherals

Recommended pins for most devices:

```
D1
D2
D5
D6
D7
```

Pins that require caution:

```
D3
D4
D8
```

---

## Notes for This Project

Current pin usage:

- **D3 → push button input**
- **D8 → UART TX from Arduino Nano**

This configuration works provided that:

- the push button is **not pressed during boot**
- the Arduino Nano **does not drive D8 HIGH during startup**

If the ESP8266 ever becomes stuck in a boot loop, the **first thing to check is the state of these boot-strapping pins**.
