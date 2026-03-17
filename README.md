# F1 Live Leader Display

An ambient IoT display that tracks live Formula 1 race leadership using a WS2812B LED strip, 1.8" TFT screen, and a three-device architecture.

During a race, the strip glows in the leading constructor's team colour. Leadership changes trigger smooth fade transitions. The race finish plays a checkered wipe animation, and the Abu Dhabi season finale confirms the WDC constructor champion with a gold celebration sequence.

---

## System Architecture

```
Internet
    │
    ├── Jolpica F1 API  (schedule, standings, results)
    └── ESPN Scoreboard (live positions, race status)
              │
              ▼
    Raspberry Pi  ──────────────────────────────────────
    Flask /p1 server                                    │
    • ESPN live polling (10s)                           │
    • WDC points projection (final race)                │
    • Jolpica champion confirmation                     │
    • Serves JSON over local network                    │
              │  HTTP                                   │
              ▼                                         │
    NodeMCU ESP8266  ←──────── HTTPS (Jolpica fallback)─┘
    • WiFi + API client
    • Race phase logic (0–4)
    • EEPROM persistence
    • TFT display driver
    • Serial command dispatcher
              │  UART
              ▼
    Arduino Nano
    • WS2812B LED timing (800kHz)
    • Smooth fade engine
    • Pulse, checkered, lights-out animations
    • Accent pixel rendering
              │
              ▼
    WS2812B LED Strip  (60–300 LEDs)
```

The Nano drives the strip at 5V logic, solving the ESP8266's 3.3V logic-level limitation with WS2812B. The Pi handles all heavy computation so the NodeMCU stays responsive.

---

## Features

- Live race P1 constructor display with team colours
- Smooth colour fade on leadership changes
- Pulse animation during live race
- Lights-out countdown animation (T-0 trigger)
- Checkered wipe on race finish
- WDC champion confirmation with gold celebration sequence (season finale)
- TFT shows: live clock, GP name, P1 team, status, mode, countdown, round number
- 13-mode manual override (button cycles through all constructors)
- EEPROM persistence — winner and mode survive power loss
- Boot replay — checkered/champion animation replays on reboot within 12h of race finish
- Pi-projected champion hint during final race ("WDC LIKELY") before Jolpica confirms
- Automatic WiFi reconnection
- NTP time sync with validity guard (epoch > 1,700,000,000)
- Race phase system (phases 0–4) derived from NTP time vs race start epoch

---

## Hardware

| Component | Notes |
|-----------|-------|
| NodeMCU (ESP8266) | WiFi, API, race logic, TFT |
| Arduino Nano | LED timing, animations |
| WS2812B LED strip | 60–300 LEDs, 5V |
| Raspberry Pi (any) | Pi server, same local network |
| 1.8" SPI TFT (ST7735) | 128×160 pixels |
| 5V PSU ≥ 4A | For LED strip |
| 330Ω resistor | Data line protection |
| 1000µF capacitor | Across LED strip 5V/GND |
| Push button | Mode cycle |
| 1kΩ resistor | NodeMCU D8 → Nano RX protection |

---

## Wiring

### NodeMCU → TFT (ST7735)

| TFT Pin | NodeMCU Pin |
|---------|-------------|
| VCC | 3V3 |
| GND | GND |
| LED | 3V3 |
| SCK | D5 (GPIO14) |
| SDA (MOSI) | D7 (GPIO13) |
| CS | D2 (GPIO4) |
| A0 / DC | D1 (GPIO5) |
| RESET | — (not connected, use `TFT_RST -1`) |

> Connect TFT RESET to 3.3V permanently and set `#define TFT_RST -1` in code. The display resets on power-up. Avoids conflicts with D0 deep-sleep behaviour.

### NodeMCU → Nano (Serial)

```
NodeMCU D8  →  1kΩ  →  Nano D8
NodeMCU GND →  Nano GND
```

### Nano → LED Strip

```
Nano D6  →  330Ω  →  Strip DIN
Nano 5V  →  Strip 5V  (or external PSU positive)
Strip GND  →  Nano GND
```

> All grounds must share a common reference.  
> Do **not** power the Nano through VIN from the LED PSU. Power it separately via USB.  
> The 5V PSU feeds the LED strip directly.

---

## Pin Reference (NodeMCU)

| Pin | Use | Boot-strap note |
|-----|-----|-----------------|
| D1 (GPIO5) | TFT DC | Safe |
| D2 (GPIO4) | TFT CS | Safe |
| D3 (GPIO0) | Push button | Must be HIGH at boot — button must not be held during power-on |
| D5 (GPIO14) | TFT SCK | Safe |
| D6 (GPIO12) | Nano RX | Safe |
| D7 (GPIO13) | TFT MOSI | Safe |
| D8 (GPIO15) | Nano TX | Must be LOW at boot — Nano must not drive this HIGH during startup |

---

## Serial Protocol (NodeMCU → Nano)

| Value | Meaning |
|-------|---------|
| 0 | Display mode (warm white) |
| 1–11 | Team ID → set colour |
| 77 | Lights-out animation |
| 99 | Checkered wipe |
| 255 | Pulse effect |

---

## Team IDs and Colours

| ID | Team | RGB |
|----|------|-----|
| 1 | Ferrari | (220, 0, 0) |
| 2 | Alpine | (0, 90, 255) |
| 3 | Aston Martin | (0, 100, 60) |
| 4 | Haas | (180, 180, 180) + red accents |
| 5 | Audi | (160, 0, 0) |
| 6 | Cadillac | (200, 200, 210) + blue accents |
| 7 | McLaren | (255, 95, 0) |
| 8 | Mercedes | (0, 210, 170) |
| 9 | Racing Bulls | (0, 70, 200) |
| 10 | Red Bull | (0, 20, 120) + red accents |
| 11 | Williams | (0, 140, 255) |

White-based teams (Haas, Cadillac) use reduced intensity to limit current draw.

---

## Button Mode Cycle

| Mode | Function |
|------|----------|
| 0 | Display mode — shows defending/current WDC champion |
| 1 | Live race mode |
| 2–12 | Ferrari → Alpine → Aston Martin → Haas → Audi → Cadillac → McLaren → Mercedes → Racing Bulls → Red Bull → Williams |

Mode is persisted to EEPROM and restored on boot.

---

## TFT Display Layout

```
┌──────────────────────────────┐
│ F1 LIVE           HH:MM:SS  │  ← gold header during season end
├──────────────────────────────┤
│ Next GP: Abu Dhabi          │  ← "Off season" after final race
├─┬────────────────────────────┤
│█│ Last GP: Abu Dhabi | P1:  │  ← label changes by mode/season state
│█│ McLAREN                   │  ← team name in team colour
├─┴────────────────────────────┤
│ STATUS: LIVE                 │
├──────────────────────────────┤
│ MODE: LIVE                   │
├──────────────────────────────┤
│ LIGHTS OUT: 01:23:45        │  ← countdown, red under 5 min
├──────────────────────────────┤
│ Rnd 4/24          Pi+ESPN   │  ← "Jolpica" if Pi unavailable
└──────────────────────────────┘
```

---

## Race Phase System

The NodeMCU derives all race logic from a single phase value computed against NTP time:

| Phase | Condition | Behaviour |
|-------|-----------|-----------|
| 0 | > 6h before race | Normal week, no polling |
| 1 | 0–6h before race | Countdown shown |
| 2 | 0–2h after start | Live polling active |
| 3 | 2–12h after start | Post-race window, boot replay active |
| 4 | > 12h after start | Flags reset, idle |

---

## EEPROM Layout

| Bytes | Name | Contents |
|-------|------|----------|
| 0 | LAST_RACE | Race winner teamID (1–11) |
| 1 | SAVED_MODE | Last button mode (0–12) |
| 2 | SEASON_CHAMP | Confirmed WDC constructor teamID |
| 3–22 | LAST_GP_NAME | Short GP name string |
| 24–27 | LAST_RACE_TIME | Epoch of race finish |
| 28–31 | SEASON_TIME | Epoch of champion confirmation |
| 32–33 | SEASON_YEAR | Current season year |
| 34 | INIT_FLAG | Magic byte 0xA5 |
| 35 | ANIM_PLAYED | Reserved |

All epoch writes are guarded: only written if `time(nullptr) > 1,700,000,000` (NTP must be valid).

---

## Pi Server

The Raspberry Pi runs a Flask server (`f1_server.py`) on port 5000.

**Endpoint: `GET /p1`**

```json
{
  "team":   "McLaren",
  "status": "live",
  "gp":     "Abu Dhabi Grand Prix"
}
```

During the season finale only, two additional fields appear:

```json
{
  "team":             "Ferrari",
  "status":           "live",
  "gp":               "Abu Dhabi Grand Prix",
  "champion":         "McLaren",
  "champion_status":  "projected"
}
```

| `champion_status` | Meaning |
|-------------------|---------|
| `projected` | Pi's points simulation shows this team will win |
| `confirmed` | Jolpica standings have updated to the final round |

The NodeMCU shows "WDC LIKELY" on `projected` but only triggers the celebration animation on `confirmed` (from Pi or its own Jolpica poll — whichever arrives first).

**Endpoint: `GET /status`** — debug view of full internal state.

### Pi Setup (quick)

```bash
mkdir -p /home/pi/f1
cp f1_server.py /home/pi/f1/
cd /home/pi/f1
python3 -m venv venv
venv/bin/pip install flask requests

# Test
venv/bin/python f1_server.py
curl http://localhost:5000/p1

# Install as service
sudo cp f1server.service /etc/systemd/system/
sudo systemctl enable --now f1server
```

See `SETUP.txt` for full instructions including static IP assignment.

---

## APIs Used

| Source | Endpoint | Used for |
|--------|----------|----------|
| Jolpica | `/ergast/f1/current/next.json` | Next race schedule, total rounds |
| Jolpica | `/ergast/f1/current/last/results.json` | Race result on boot |
| Jolpica | `/ergast/f1/current/driverStandings.json` | WDC champion confirmation |
| ESPN | `site.api.espn.com/…/f1/scoreboard` | Live positions, race status |

No API keys required.

---

## Firmware Configuration

In `F1_LED_Controller.ino`, update before flashing:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* piHost   = "192.168.1.x";   // Pi's local IP
const int   piPort   = 5000;
```

In the Nano sketch, update LED count:

```cpp
#define NUM_LEDS  60   // your strip length
```

### Required Arduino Libraries

| Library | Install via |
|---------|-------------|
| Adafruit ST7735 and ST7789 | Arduino Library Manager |
| Adafruit GFX | Arduino Library Manager |
| ArduinoJson | Arduino Library Manager |
| FastLED or Adafruit NeoPixel | Arduino Library Manager |

---

## Troubleshooting

**Only first LED lights up**  
ESP8266 outputs 3.3V; WS2812B at 5V needs ≥ 3.5V HIGH. Use the Nano as the LED driver — this is already the correct architecture.

**`failed to connect to esp8266: timed out waiting for packet header`**  
A peripheral on D3/D8/D4 is likely interfering at boot. Check that the button is not held down, and the Nano is not driving D8 HIGH during startup. See the boot-strap pin table above.

**NodeMCU shows "Jolpica" not "Pi+ESPN"**  
Pi server is unreachable. Verify `piHost` IP in the sketch matches `hostname -I` on the Pi. Check `sudo systemctl status f1server`.

**`champion_status` stuck at `projected` after race**  
Jolpica takes 30–60 minutes to update after race end. The Pi polls every 90 seconds; the NodeMCU polls independently every 2 minutes. Both will converge once Jolpica updates.

**Boot replay animation not playing**  
`EEPROM_LAST_RACE_TIME` was written with invalid NTP time (near-zero epoch). The sanity guard `storedTime < 1,700,000,000` rejects it. This resolves itself — the next race finish will write a valid epoch.

**ESP8266 in boot loop**  
Check boot-strap pin states at power-on. GPIO0 must be HIGH, GPIO15 must be LOW.

---

## Production Checklist

- [ ] WiFi credentials set in firmware
- [ ] `piHost` IP set and Pi is on static IP
- [ ] Pi service starts automatically (`systemctl enable f1server`)
- [ ] Button cycles modes 0–12 correctly
- [ ] Live mode polls Pi and falls back to Jolpica
- [ ] Leadership change triggers fade
- [ ] Race finish triggers checkered animation
- [ ] Winner stored and survives power cycle
- [ ] Boot replay fires within 12h of race finish
- [ ] 5V PSU ≥ 4A
- [ ] 330Ω data resistor installed
- [ ] 1000µF capacitor on strip power rails
- [ ] All grounds common
- [ ] Nano powered via USB (not from LED PSU VIN)
- [ ] 30-minute runtime stable, no resets

---

## Long-Term Notes

- Give the Pi a static DHCP reservation in your router so `piHost` never needs updating
- Jolpica is an Ergast-compatible community fork — monitor for API changes at the season boundary
- The system auto-detects new seasons: EEPROM champion and race data are cleared when a new season year is detected from the API
- Back up your firmware after any working configuration
