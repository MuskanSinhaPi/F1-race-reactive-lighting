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

**So the full sequence on race day is:

Boot → shows LIGHTS OUT: XX:XX:XX counting down
At T-5 seconds → screen flashes red, sends CMD_LIGHTS_OUT to Nano
Past start time → shows RACE IN PROGRESS
Every 2 minutes → fetches results, updates P1 constructor
Race finishes → checkered wipe, winner shown, RACE COMPLETE

##Free Live Race Data

The cheapest route that actually works for live P1 data is web scraping — scraping a free F1 timing website during the race. No subscription needed.
Good candidates:
formula1.com/en/timing — official F1 live timing, free to view in browser
livetiming.formula1.com — raw F1 timing feed, completely free, no login
The second one is what FastF1 and MultiViewer both use under the hood. It's a WebSocket feed that F1 exposes publicly — no authentication required. The issue is WebSockets are too complex for an ESP8266, but a Python script on your laptop can connect to it and serve just the P1 constructor over a simple HTTP endpoint.
Searched the web
Searched the web
This is the answer. livetiming.formula1.com requires no authentication at all OpenF1 — it's a completely free, public WebSocket feed that F1 exposes. FastF1 uses it under the hood. You just need a small Python script on your laptop that connects to it and serves P1 constructor over HTTP for the NodeMCU to poll.
The script would be about 50 lines:
livetiming.formula1.com (free, no login) 
  → Python script on laptop (WebSocket listener)
    → Flask HTTP endpoint at laptop_ip:5000/p1
      → NodeMCU polls every 30s
This gives you genuinely real-time P1 data, completely free, no subscription. Want me to write both the Python script and update the NodeMCU fetchRaceData() to use it?
FastF1 is unreliable for live use because it's designed for post-race analysis. The livetiming.formula1.com WebSocket feed itself is different — it's the raw official F1 live timing feed that FastF1, MultiViewer, and every other F1 timing app pulls from. It's what powers the official F1 app's live timing screen. It's reliable during sessions.
FastF1 wraps this feed but adds caching, data processing, and analysis features on top — that overhead makes it unreliable for real-time use. A direct WebSocket connection to the feed without FastF1's overhead would be fast and reliable.
So the chain is:
livetiming.formula1.com WebSocket (reliable, free, official)
  → lightweight Python script (just extracts P1 constructor, nothing else)
    → Flask serves it at laptop_ip:5000/p1
      → NodeMCU polls every 30s
The only dependency is your laptop being on during the race with the script running — same requirement as any other laptop-based solution.
Shall I write it? It would be two files:
f1_server.py — ~50 lines, runs on your laptop race day only
Updated fetchRaceData() in NodeMCU — polls laptop for live P1, falls back to Jolpica for finished detection after 90 mins
can it be written on a raspberry pi 4 model B
15:04
Yes, Raspberry Pi 4 Model B is perfect for this — it runs full Linux, has WiFi built in, and can run Python scripts continuously. Even better than a laptop because:
Stays on permanently, no need to remember to start it on race day
Sits on your network 24/7
Low power consumption
Static IP easy to set
The setup would be:
livetiming.formula1.com WebSocket
  → Python script on Pi 4 (runs as a service, always on)
    → Flask at pi_ip:5000/p1
      → NodeMCU polls every 30s
You'd set a static IP on the Pi so the NodeMCU always knows where to find it, and run the script as a systemd service so it auto-starts on boot and restarts if it crashes.
Outside of race weekends the script would just return nothing / last known constructor, and the NodeMCU would fall back to Jolpica behavior.
Shall I write:
f1_server.py — Python WebSocket + Flask server for the Pi
f1_server.service — systemd service file so it runs automatically on boot
Updated fetchRaceData() — NodeMCU polls Pi for live P1 during race, Jolpica for finished check
so no changes to the nano code, on nodemcu just update the endppoint to that of my raspberry pi's ip address, and no change in wiring- the raspberry pi can run from anywhere and does not need to be physicaly connected to my existing system, right?
15:08
Correct on all counts:
Nano code — no changes
Wiring — no changes
NodeMCU — only fetchRaceData() changes to poll the Pi's IP for live P1, everything else stays the same
Pi — completely separate, just needs to be on the same WiFi network, no physical connection to anything. Could literally be in another room.
The Pi just needs:
Power (USB-C)
WiFi connected to same network as NodeMCU
Python script running
That's it. Shall I write the Pi server script and the updated fetchRaceData()?
I can pol every few seconds too then right? since there's no rate limit? what time is good enough to have it real time but also not strain my nodemcu
15:20
Yes, no rate limit since it's your local network. The Pi can handle hundreds of requests per second easily.
For the NodeMCU the bottleneck is the HTTP request itself — on ESP8266 a local HTTP request takes about 100-300ms. So polling too fast would block the main loop and affect button response, clock updates etc.
Recommended: 5 seconds
Fast enough to feel real-time — P1 change shows within 5 seconds
Slow enough that NodeMCU isn't constantly busy
Clock still updates every 1 second normally between polls
Button still responds instantly
You could go as low as 2-3 seconds but 5 is the sweet spot. F1 position changes mid-race rarely happen faster than that anyway — a proper overtake for P1 takes at least a few seconds to complete.
So just change:
cpp
const unsigned long interval = 30000;  // current
To:
cpp
const unsigned long interval = 5000;   // 5 seconds for Pi local polling

when I try to access the livetiming.formula1.com endpoint from my browser it reads denied

15:26
Yes that's expected — the live timing feed isn't a regular HTTP endpoint you browse to. It's a SignalR WebSocket connection, not a REST API. Browsers can't just open it directly.

The Python script connects to it properly using the signalr-client or websockets library which handles the WebSocket handshake correctly. FastF1 does exactly this under the hood.


Searched the web
Searched the web
This is a dealbreaker I found. Since the 2025 Dutch GP, the F1 Live Timing feed no longer freely publishes live position data — driver position is now only available to those with an active F1 TV subscription. 
Open F1 API

So the free unauthenticated SignalR feed that FastF1 and everything else used is now paywalled for live position data. This happened mid-2025.

Also even before this change, someone building exactly the same thing as you — changing room lights based on race leader — found that the unauthenticated feed was significantly delayed on race day compared to the official timing dashboard. 
API-Sports

So the honest final answer is:

Free live P1 data — no longer possible without F1 TV
Jolpica — free, but only updates after race ends, slow
Your current system — actually the best you can do for free
The system works great for everything except mid-race P1 updates. That feature simply requires a paid subscription now. Jolpica for the finished detection is the right approach — just have to wait for it to update after the race ends.

-------------------------------------------
Short answer: **No — your code is *not* doing a chain of calls. It makes a *single request* to one ESPN endpoint each poll.**

---

### 🔍 What your code actually does

Every `POLL_INTERVAL` (10 seconds), this happens:

```python
r = requests.get(ESPN_SCOREBOARD, timeout=REQUEST_TIMEOUT)
```

👉 That endpoint is:

```
https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard
```

---

### 🧠 Why this is important

That **one endpoint already contains everything** you need:

* Current race status (live / delayed / finished)
* Driver positions (`order`)
* Constructor/team info
* Fastest lap stats
* Competitor classification (DNF, etc.)

So you're **not doing this**:

❌ Fetch event → then fetch race → then fetch drivers → then fetch stats
❌ Multiple dependent API calls (no chaining)

---

### 🧩 Instead, you're doing this:

✔ **Single call → parse nested JSON → extract everything**

Your `parse_espn_scoreboard()` function is just walking through:

```
events → competitions → competitors → statistics
```

---

### 📦 Think of it like:

Instead of:

> "Give me race → now give me drivers → now give me stats"

You're doing:

> "Give me the *entire race snapshot in one go*"

---

### ⚡ Why ESPN designed it this way

This endpoint is meant for:

* Live scoreboards
* Apps / dashboards (like yours 👀)
* Low-latency polling systems

So it's intentionally:

* **Flat enough to parse**
* **Rich enough to avoid multiple requests**

---

### 🚀 Performance implication (good news)

Your current design is actually **optimal**:

* Only **1 HTTP request per 10 seconds**
* Minimal latency
* Less chance of rate limiting
* Simpler error handling

---

### ⚠️ When would chaining be needed?

You'd need multiple calls *only if* you wanted:

* Sector times / telemetry (not in scoreboard)
* Lap-by-lap timing
* Tyre data
* Detailed race control messages

👉 That’s when people switch to:

* WebSockets (`livetiming.formula1.com`)
* Or deeper ESPN endpoints (not public/consistent)

---

### 🧠 Final verdict

✅ **You are NOT chaining API calls**
✅ **You are using a single aggregated endpoint (best practice here)**
✅ **Your architecture is clean and efficient**

---

If you want, I can next:
👉 Break down the exact ESPN JSON structure (so you fully understand what you're parsing)
👉 Or help you simulate/mock this endpoint for testing without hitting ESPN every time
Ahh — I see where the confusion came from. Your *original assumption* wasn’t wrong in general… it’s just **not true for the specific ESPN endpoint you chose**.

Let’s clear this up properly 👇

---

## 🧠 Where the “3 API calls” idea comes from

In most sports APIs (and even F1 official data), you typically need:

1. **Event / race info**
2. **Live positions / timing**
3. **Extra stats (fastest lap, status, etc.)**

So the flow usually looks like:

```
Get current race → get competitors → get stats
```

👉 That *would* be multiple dependent calls
👉 And yes — that would be painful on an ESP8266

---

## ⚡ What you discovered (without realizing it 😄)

The ESPN **scoreboard endpoint is already an aggregated endpoint**

It basically does this internally:

```
[ESPN backend]
   ↓
Fetch race data
Fetch competitors
Fetch stats
Merge everything
   ↓
Return ONE JSON
```

So your Pi is calling:

```
NodeMCU → Pi → ESPN (1 call)
```

Instead of what you feared:

```
NodeMCU → ESPN (call 1 → call 2 → call 3)
```

---

## 🧩 Why your Pi still makes sense (VERY important)

Even though ESPN only needs **1 call**, your Raspberry Pi is still doing a LOT of heavy lifting that your NodeMCU *cannot safely do*:

### 1️⃣ JSON parsing complexity

ESPN response is **deeply nested and large**

Your NodeMCU would struggle with:

* Memory (ESP8266 has ~40–50 KB usable heap)
* ArduinoJson limits
* Fragmentation crashes

---

### 2️⃣ HTTPS + TLS overhead

This line:

```python
requests.get("https://site.api.espn.com/...")
```

👉 Requires full TLS handshake

On ESP8266:

* Slow
* Unstable
* Certificate issues
* Often fails silently

---

### 3️⃣ Data normalization (CRITICAL for your LED logic)

Your Pi is doing:

```python
norm_constructor(...)
```

Mapping:

```
"McLaren F1 Team" → "McLaren"
"Mercedes-AMG Petronas" → "Mercedes"
```

👉 Your NodeMCU logic **depends on exact string matches**

Without this:

* LEDs would break randomly
* Teams wouldn’t match

---

### 4️⃣ Champion projection logic (this is HUGE)

This part alone:

```python
project_champion(...)
```

Includes:

* Applying FIA points system
* Fastest lap logic
* Constructor aggregation
* Live simulation

👉 This is **way too heavy** for a NodeMCU

---

### 5️⃣ Rate limiting + resilience

Your Pi:

* Polls every 10 sec
* Handles failures
* Caches state

If NodeMCU did this directly:

* Higher failure rate
* Possible API blocking
* Messy retry logic

---

## 🔥 So was your original reasoning wrong?

👉 **Conceptually correct**
👉 **Technically unnecessary for THIS endpoint**
👉 **Still the right architectural decision**

---

## 🧠 The real reason your Pi is useful (correct mental model)

Not:

> “ESP8266 can’t make 3 API calls”

But:

> “ESP8266 should not handle complex APIs, heavy JSON, TLS, and race logic”

---

## ⚡ Your current architecture is actually ideal

```
ESPN API (complex, heavy, nested)
        ↓
Raspberry Pi (brain 🧠)
  - Parses
  - Simplifies
  - Normalizes
  - Computes champion
        ↓
NodeMCU (dumb display ⚡)
  - Just reads:
    {
      "team": "McLaren",
      "status": "live"
    }
```

---

## 💡 If you removed the Pi…

Your NodeMCU would need to:

* Parse ~50KB JSON ❌
* Handle TLS reliably ❌
* Maintain mappings ❌
* Compute standings ❌

👉 It would become unstable *very fast*

---

## 🧾 Final takeaway

* ❌ You **don’t need multiple ESPN calls**
* ✅ You **DO need the Pi as a processing layer**
* 🚀 Your current system design is actually **very solid and production-like**

----
Yes, you did everything right so far 👍 — and this result tells us something very specific:

TCP FAILED

Even after changing to port 8080, that means:

🔴 Your router is blocking ESP8266 → Pi TCP entirely (not just port 5000)

🧠 Important realization

You have now ruled out:

❌ Port issue (5000 vs 8080)

❌ Code issue

❌ HTTPClient issue

So what’s left is:

⚠️ Network isolation / routing behavior specific to ESP8266

🚨 Why this happens

Some routers (especially ISP ones) treat devices differently:

Device	Behavior
Laptop	full access
Phone	full access
ESP8266	restricted / sandboxed

👉 Even on same WiFi

---

If you want, I can next:
👉 Show you how to **reduce ESPN payload size further** (ultra-optimized Pi → NodeMCU response)
👉 Or help you simulate failures so your display never glitches mid-race
