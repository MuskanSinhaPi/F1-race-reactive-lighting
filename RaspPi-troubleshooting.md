# F1 LED Controller — Build Notes & Troubleshooting

**Hardware:** NodeMCU (ESP8266) + Raspberry Pi 3/4 + Arduino Nano  
**Network:** Home LAN, 192.168.1.x subnet  
**Architecture:** Pi server (192.168.1.53:8080) pushes to NodeMCU (192.168.1.55:8080)

---

## 1. ISP Router — Locked Admin Panel

### Problem
The home router's admin panel at `192.168.1.1` was inaccessible or locked. Many ISPs remotely manage consumer routers and restrict the admin interface so customers cannot change certain settings. The login credentials (usually printed on a sticker on the bottom of the router) may work for basic settings but higher-level options — particularly anything touching port forwarding, firewall rules, or AT/modem configuration — are grayed out or hidden entirely.

### What this blocked
- Could not open or forward ports for external access
- Could not inspect or modify firewall rules
- Could not change any advanced network configuration

### Workaround
All communication was kept strictly **within the local network** (LAN-only). The Pi and NodeMCU both sit on the same 192.168.1.x subnet so no port forwarding was needed. The Pi's Flask server binds to `0.0.0.0:8080` and the NodeMCU's WiFiServer listens on port 8080 — both are LAN-internal connections that never need to traverse the router's WAN firewall.

**Key lesson:** Design embedded home projects around LAN-only communication whenever possible. External access (e.g. for remote monitoring) can be added later via a VPN tunnel (WireGuard on the Pi is a clean option) without ever needing to touch ISP router settings.

---

## 2. Port 80 Blocked on NodeMCU

### Problem
The first implementation ran `WiFiServer server(80)` on the NodeMCU, which is the standard HTTP port. Despite the NodeMCU successfully connecting to WiFi and `server.begin()` being called, all attempts to reach it on port 80 timed out:

```
curl: (28) Connection timed out after 3002 milliseconds
```

Ping worked fine — the NodeMCU was reachable — but the TCP connection on port 80 consistently failed.

### Root cause
Many home ISP routers silently block port 80 (and 443) for devices on the LAN that are acting as *servers*, not clients. This is a common consumer router behaviour to prevent accidental hosting of web servers. The block is applied at the router's internal firewall, not at the device itself, which is why the port appears closed from other LAN devices even though the NodeMCU had the socket open.

This was confirmed by testing port 8080 in parallel:

```
* Trying 192.168.1.55:80...   → Connection timed out
* Trying 192.168.1.55:8080... → Connected
```

Port 8080 connected immediately.

### Fix
Changed `WiFiServer server(80)` to `WiFiServer server(8080)` and updated `NODEMCU_PORT = 8080` in the Pi server. All subsequent communication uses port 8080.

---

## 3. ESP8266 WiFi Mode — AP+STA Interference

### Problem
Even after moving to port 8080, connections were still intermittently failing. The ESP8266 boots by default in `WIFI_AP_STA` mode — it runs a soft Access Point alongside the station connection. The internal soft-AP intercepts certain incoming TCP connections before `WiFiServer` can see them, causing the socket to appear open (`server.status() == 1`) but never receiving clients (`server.hasClient() == 0`).

### Fix
Three lines added to `connectWiFi()`:

```cpp
WiFi.persistent(false);       // Don't write credentials to flash on every connect
WiFi.setAutoReconnect(false); // Disable automatic reconnect (managed manually)
WiFi.mode(WIFI_STA);          // Station-only mode — shuts down the soft-AP
WiFi.disconnect();            // Clear any stale AP state
delay(100);
WiFi.begin(ssid, password);
```

`WIFI_STA` mode shuts down the soft-AP entirely, removing the interference. `persistent(false)` prevents repeated flash writes on every boot (important for ESP8266 flash longevity). `setAutoReconnect(false)` allows the sketch to manage reconnection explicitly through `maintainWiFi()`.

**Also critical:** `server.begin()` must be called **after** `WiFi.begin()` completes and the station is connected. Calling it before WiFi is up binds the socket to no interface — the socket opens but is unreachable. The correct setup order is:

```cpp
connectWiFi();   // Block until WL_CONNECTED
server.begin();  // Only then start the TCP server
```

---

## 4. `server.begin()` Placement Bug

### Problem
During development, `server.begin()` was placed inside a `#ifdef TEST_BUILD` block that ran *before* `connectWiFi()`. On the ESP8266, calling `server.begin()` before WiFi is connected means the TCP stack has no interface to bind to — the call succeeds silently but the server is not reachable from the network.

### Fix
Moved `server.begin()` to immediately after `connectWiFi()` in all build configurations, outside any `#ifdef` guards:

```cpp
connectWiFi();
server.begin();
Serial.print("HTTP server on ");
Serial.println(WiFi.localIP());
```

---

## 5. Architecture — Push vs. Poll (Why Push Won)

### Initial approach: NodeMCU polls Pi
The first design had the NodeMCU polling the Pi's `/p1` endpoint every 2–5 seconds:

```
NodeMCU  -->  GET /p1  -->  Pi  -->  ESPN API
NodeMCU  <--  {team, status}  <--  Pi
```

**Problems with this approach:**
- NodeMCU spent significant time and power making outgoing HTTPS/HTTP requests
- Polling interval created a lag between ESPN data changing and the display updating
- NodeMCU had to manage both polling timers and incoming display logic simultaneously
- Any slowness or failure in the poll could block the main loop
- The ESP8266's single-threaded loop made it hard to handle incoming requests while also doing periodic outgoing polls

### Final architecture: Pi pushes to NodeMCU
The Pi polls ESPN every 10 seconds during races, detects state changes, and pushes updates to the NodeMCU only when something actually changes:

```
Pi  -->  ESPN API  (every 10s during race, 60s otherwise)
Pi  -->  GET /update?team=Ferrari&status=live  -->  NodeMCU  (on change only)
```

**NodeMCU** runs a simple `WiFiServer` that receives push updates. Its `loop()` only needs to:
1. Check for incoming push connections (`handleIncoming()`)
2. Update the TFT display
3. Send commands to the Nano over SoftwareSerial
4. Maintain WiFi

**Benefits:**
- NodeMCU is entirely reactive — no outgoing requests during race
- Updates are immediate (as fast as the Pi detects them) rather than lagged by a poll interval
- Pi handles all the complex API logic, JSON parsing, WDC projection, and deduplication
- NodeMCU sketch is dramatically simpler
- Push deduplication on the Pi means the NodeMCU only receives a network request when state actually changes — not every poll cycle

**Push deduplication (Pi-side):**
```python
_last_push_key = None

def push_update(team, status, champion=None, champion_status=None, gp=None):
    push_key = (team, status, champion, champion_status, gp)
    if _last_push_key == push_key:
        return   # identical to last push — skip
    _last_push_key = push_key
    # ... send HTTP GET to NodeMCU
```

On push failure, `_last_push_key` is reset to `None` so the next poll retries.

---

## 6. TCP Backlog Accumulation During Animations

### Problem
The NodeMCU's main loop blocks during animations (the 9-second checkered flag animation, the 6-second season end flash). While the loop is blocked, the Pi continues sending push updates every 2–10 seconds. These stack up in the ESP8266's TCP receive buffer. When the animation finishes and `handleIncoming()` resumes, it drains all queued packets in rapid succession — causing the display to flicker through multiple state changes (live → finished → live → finished) in less than a second.

### Fix
Added `flushPendingClients()` immediately after any long blocking operation:

```cpp
void flushPendingClients() {
  unsigned long start = millis();
  while (millis() - start < 200) {
    WiFiClient stale = server.available();
    if (!stale) break;
    stale.stop();   // Discard without reading
    yield();
  }
}

// Called after finish animation:
sendToNano(CMD_CHECKERED);
delay(9000);
sendToNano(teamID);
delay(500);
raceAnimationPlayedThisBoot = true;
flushPendingClients();   // ← drain queued pushes before redrawing
drawMainScreen();
```

Combined with Pi-side deduplication, this eliminates the flicker entirely.

---

## 7. Display Updates During Non-Live Modes

### Problem
When the user had toggled the button to a team-specific mode (e.g. `MODE_FERRARI`), incoming Pi pushes would still call `updateTeamDisplay()` and `updateStatus()`, overwriting the team colour and status with live race data. The mode-specific display would flicker back to live data on every push.

### Fix
All display-mutating calls in `handleIncoming()` are gated on `currentMode == MODE_LIVE`:

```cpp
bool liveMode = (currentMode == MODE_LIVE);

if (status == "live") {
    raceCancelled = false;
    raceSunday    = true;
    currentTeamID = teamID;        // State always updates...
    if (liveMode) {
        updateTeamDisplay(teamID, true);  // ...but display only in LIVE mode
        updateInfoLine();
        sendToNano(teamID);
        sendToNano(CMD_PULSE);
        updateStatus("LIVE", C_GREEN);
    }
}
```

State variables (`raceSunday`, `currentTeamID`, etc.) always update regardless of mode, so switching back to `MODE_LIVE` immediately reflects the correct current state.

---

## 8. Empty Team Name Sending `getTeamID()` = 0

### Problem
When ESPN returned competitors with an empty constructor name (e.g. during early race loading or a data gap), `getTeamID("")` returned 0 (the fallback), and `teamTFTColor[0]` is `C_WHITE` (warm white). The display would flash white between overtakes or at race start.

### Fix 1 — `getTeamID()` returns 0 for unresolved teams
Changed the default return from `TEAM_MCLAREN` to `0`:

```cpp
int getTeamID(String team) {
    if (team.indexOf("Ferrari") >= 0) return TEAM_FERRARI;
    // ... other teams ...
    return 0;   // ← was TEAM_MCLAREN; 0 means "unresolved"
}
```

### Fix 2 — All callers check `teamID >= 1` before updating display
```cpp
if (teamID >= 1 && teamID <= 11) {
    currentTeamID = teamID;
    if (liveMode) updateTeamDisplay(teamID, true);
}
```

### Fix 3 — Pi-side fallback for empty `p1_constructor`
On the Pi, if ESPN returns no competitors or an empty constructor, the previously known team name is used instead of sending a blank:

```python
team_to_send = p1_constructor or prev_team or "---"
push_update(team_to_send, "live", ...)
```

---

## 9. Stale `raceCancelled` Flag Bleeding Across Sessions

### Problem
When a cancelled race push arrived, `raceCancelled = true` was set. If the Pi server was then restarted (e.g. during development), it would send `idle` to reset the NodeMCU. But the `idle` branch did not clear `raceCancelled`, so the info line continued to show "RACE CANCELLED" even after a reset. Similarly, `delayed` and `postponed` branches did not clear `raceCancelled`, so following a cancelled race, a rain delay would still display "RACE CANCELLED" in the info line.

### Fix
Added `raceCancelled = false` explicitly in the `idle`, `scheduled`, `delayed`, and `postponed` branches:

```cpp
} else if (status == "idle") {
    raceSunday    = false;
    raceCancelled = false;   // ← clears stale cancelled state
    raceFinished  = false;
    if (liveMode) { updateStatus("NO RACE", C_GREY); updateInfoLine(); }
```

---

## 10. Red Bull Colour Invisible on Dark Background

### Problem
The initial TFT colour for Red Bull was `0x0A32` — a near-black navy blue that was completely invisible against the dark `C_PANEL` background colour `0x0841`. The accent bar appeared to show nothing.

### Fix
Changed to `0xF926` (red-orange), which is visible against both light and dark backgrounds and loosely reflects Red Bull's racing livery accent colour:

```cpp
uint16_t teamTFTColor[] = {
    C_WHITE,
    0xD800,  // FERRARI        red
    0x1252,  // ALPINE         blue
    0x0494,  // ASTON MARTIN   green
    0xD805,  // HAAS           red-grey
    0xB000,  // AUDI           dark red
    0xBDD7,  // CADILLAC       silver
    0xFB00,  // McLAREN        orange
    0x0697,  // MERCEDES       teal
    0x2832,  // RACING BULLS   blue
    0xF926,  // RED BULL       red-orange  ← was 0x0A32 (invisible)
    0x1496,  // WILLIAMS       blue
};
```

---

## 11. Boot Colour — EEPROM Read Timing

### Problem
In early builds, `drawMainScreen()` was called before the EEPROM read that sets `currentTeamID`. The TFT would paint the accent bar using index 0 (`C_WHITE`) instead of the saved team colour, showing warm white until the first Pi push arrived or the mode was toggled.

### Fix
Ensure `currentTeamID` is loaded from EEPROM **before** calling `drawMainScreen()`:

```cpp
// Load EEPROM values FIRST
int savedTeam = EEPROM.read(EEPROM_LAST_RACE);
if (savedTeam >= 1 && savedTeam <= 11) currentTeamID = savedTeam;

// THEN draw the screen with the correct colour
drawMainScreen();
applyMode(currentMode);
```

In production, `currentTeamID` is seeded from Jolpica on boot (fetching the actual last race P1 constructor), so the correct team colour is guaranteed regardless of EEPROM state.

---

## Quick Reference — IP Addresses & Ports

| Device | IP | Port | Role |
|--------|-----|------|------|
| Raspberry Pi | 192.168.1.53 | 8080 | Flask server, ESPN polling |
| NodeMCU | 192.168.1.55 | 8080 | WiFiServer, receives Pi pushes |
| Router admin | 192.168.1.1 | 80 | ISP-locked, do not rely on access |

**Why port 8080?** Port 80 is blocked by the ISP router for LAN server devices. Port 8080 is unblocked and works reliably.

---

## Diagnostic Commands

```bash
# Confirm NodeMCU is reachable
ping 192.168.1.55

# Test port 80 (should time out)
curl -v --max-time 3 "http://192.168.1.55:80/"

# Test port 8080 (should get "Empty reply" — normal, / is not handled)
curl -v --max-time 3 "http://192.168.1.55:8080/"

# Send a manual live push
curl "http://192.168.1.55:8080/update?team=Ferrari&status=live&gp=Monaco+Grand+Prix"

# Check Pi server state
curl http://192.168.1.53:8080/p1
curl http://192.168.1.53:8080/status

# Manually refresh standings (Pi)
curl http://192.168.1.53:8080/refresh
```

---

## Summary of All Fixes Applied

| # | Issue | Fix |
|---|-------|-----|
| 1 | ISP router blocks admin/port changes | LAN-only architecture, no port forwarding needed |
| 2 | Port 80 blocked by ISP router | Switched to port 8080 throughout |
| 3 | ESP8266 AP+STA mode intercepts connections | `WiFi.mode(WIFI_STA)` + `persistent(false)` + `setAutoReconnect(false)` |
| 4 | `server.begin()` before WiFi connected | Moved `server.begin()` to after `connectWiFi()` completes |
| 5 | NodeMCU polling Pi (slow, complex) | Inverted to Pi-pushes-NodeMCU event-driven architecture |
| 6 | TCP backlog flicker after animations | `flushPendingClients()` after every long blocking delay |
| 7 | Live pushes overwrite non-live mode display | All display updates gated on `currentMode == MODE_LIVE` |
| 8 | Empty team name shows warm white | `getTeamID()` returns 0; callers check `>= 1`; Pi fallback to prev team |
| 9 | `raceCancelled` persists across server restarts | Clear flag in `idle`, `scheduled`, `delayed`, `postponed` branches |
| 10 | Red Bull colour invisible on dark panel | `0x0A32` → `0xF926` (red-orange, visible) |
| 11 | Boot shows warm white before first push | Load EEPROM / Jolpica before calling `drawMainScreen()` |
