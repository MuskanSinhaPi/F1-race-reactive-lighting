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
