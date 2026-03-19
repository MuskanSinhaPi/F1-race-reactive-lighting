## Full Setup: Raspberry Pi 4 Model B — F1 LED Server

---

### What you'll need before starting

- Raspberry Pi 4 Model B
- MicroSD card (8GB minimum, 16GB recommended)
- Power supply (USB-C, 5V 3A)
- Either: ethernet cable, or your WiFi credentials
- A computer to flash the SD card from

---

## Part 1 — Flash the OS

**1. Download Raspberry Pi Imager** on your computer  
→ https://www.raspberrypi.com/software/

**2. Open Imager, click "Choose OS"**  
→ Raspberry Pi OS (other) → **Raspberry Pi OS Lite (64-bit)**  
Lite = no desktop. You don't need one. Faster, less RAM used.

**3. Click the gear icon ⚙️ (Advanced options) before flashing — this is important**

Set:
- ✅ Enable SSH
- ✅ Set username and password → username: `pi`, password: something you'll remember
- ✅ Configure WiFi → enter your SSID and password (saves you needing a keyboard/monitor)
- ✅ Set locale/timezone → your region

**4. Choose your SD card, click Write**

---

## Part 2 — First Boot and SSH In

**1. Insert SD card into Pi, plug in power**

Wait 60–90 seconds for first boot.

**2. Find the Pi's IP address**

Option A — check your router's connected devices list (easiest)  
Option B — from another device on the same network:
```bash
ping raspberrypi.local
```
This usually works on home networks.

**3. SSH in from your computer**

On Mac/Linux:
```bash
ssh pi@192.168.1.x    # replace with Pi's actual IP
```
On Windows: use PuTTY, or Windows Terminal works natively now.

Accept the fingerprint prompt, enter your password.

You should see:
```
pi@raspberrypi:~ $
```

---

## Part 3 — System Setup

Run these in order. Each line is one command.

**Update everything first:**
```bash
sudo apt update && sudo apt upgrade -y
```
This takes 3–5 minutes on first run.

**Install Python pip and venv:**
```bash
sudo apt install -y python3-pip python3-venv git
```

**Give the Pi a static IP so the NodeMCU always finds it:**

Open the DHCP config file:
```bash
sudo nano /etc/dhcpcd.conf
```

Scroll to the bottom and add these lines. Use your actual router IP and desired static IP:
```
interface wlan0
static ip_address=192.168.1.45/24
static routers=192.168.1.1
static domain_name_servers=192.168.1.1
```
If using ethernet instead of WiFi, replace `wlan0` with `eth0`.

Save: `Ctrl+X` → `Y` → `Enter`

**Reboot to apply:**
```bash
sudo reboot
```

Wait 30 seconds, then SSH back in using the new static IP:
```bash
ssh pi@192.168.1.45
```

---

## Part 4 — Install the F1 Server

**Create the project folder:**
```bash
mkdir -p /home/pi/f1
cd /home/pi/f1
```

**Copy `f1_server.py` from your computer to the Pi:**

Open a new terminal on your computer (not the SSH session) and run:
```bash
scp /path/to/f1_server.py pi@192.168.1.45:/home/pi/f1/
```
Replace `/path/to/f1_server.py` with wherever the file actually is. For example if it's in Downloads:
```bash
scp ~/Downloads/f1_server.py pi@192.168.1.45:/home/pi/f1/
```

On Windows with PuTTY, use `pscp` instead of `scp`, syntax is the same.

**Back in the SSH session, create the virtual environment:**
```bash
cd /home/pi/f1
python3 -m venv venv
venv/bin/pip install flask requests
```

This installs Flask (the web server) and requests (HTTP client). Nothing else needed.

---

## Part 5 — Test It Manually

**Start the server:**
```bash
cd /home/pi/f1
venv/bin/python f1_server.py
```

You should see:
```
INFO  * Running on http://0.0.0.0:5000
INFO  Poll loop started
INFO  Schedule: 24 rounds this season
```

**Open a second terminal on your computer and test the endpoint:**
```bash
curl http://192.168.1.45:5000/p1
```

Expected during off-season:
```json
{"gp": "", "status": "idle", "team": "---"}
```

Expected during a race weekend:
```json
{"gp": "Bahrain Grand Prix", "status": "live", "team": "McLaren"}
```

**Check the debug endpoint:**
```bash
curl http://192.168.1.45:5000/status
```
This shows full internal state — useful for diagnosing what the server thinks is happening.

If both return JSON, the server is working. Press `Ctrl+C` to stop it.

---

## Part 6 — Install as a System Service

This makes the server start automatically on every boot, and restart itself if it crashes.

**Copy the service file to the Pi:**

From your computer:
```bash
scp /path/to/f1server.service pi@192.168.1.45:/home/pi/f1/
```

**Back in SSH, install and enable it:**
```bash
sudo cp /home/pi/f1/f1server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable f1server
sudo systemctl start f1server
```

**Verify it's running:**
```bash
sudo systemctl status f1server
```

You should see `Active: active (running)`.

**Watch live logs:**
```bash
sudo journalctl -u f1server -f
```

Press `Ctrl+C` to stop watching. The server keeps running.

**Test that it survives a reboot:**
```bash
sudo reboot
```

Wait 30 seconds, SSH back in, then:
```bash
curl http://192.168.1.45:5000/p1
```

If it responds, auto-start is working.

---

## Part 7 — Configure the NodeMCU

Open `F1_LED_Controller.ino` in Arduino IDE and update these three lines:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* piHost   = "192.168.1.45";   // your Pi's static IP from Part 3
```

Flash to the NodeMCU. On boot it will try the Pi first, fall back to Jolpica if the Pi is unreachable. The bottom-right of the TFT shows `Pi+ESPN` when the Pi is being used, `Jolpica` when falling back.

---

## Part 8 — Open the Firewall (if needed)

Raspberry Pi OS Lite doesn't enable a firewall by default, so port 5000 is already open. If you've added `ufw` yourself:

```bash
sudo ufw allow 5000
sudo ufw reload
```

---

## Ongoing Maintenance

**View server logs:**
```bash
sudo journalctl -u f1server -n 50
```

**Restart the server:**
```bash
sudo systemctl restart f1server
```

**Update the server script:**
```bash
# From your computer:
scp f1_server.py pi@192.168.1.45:/home/pi/f1/
# Then on Pi:
sudo systemctl restart f1server
```

**Check Pi's IP hasn't changed:**
```bash
hostname -I
```

---

## Verification Checklist

- [ ] Pi boots and SSH works
- [ ] Static IP set and survives reboot
- [ ] `curl http://192.168.1.45:5000/p1` returns JSON
- [ ] Service starts automatically after `sudo reboot`
- [ ] NodeMCU shows `Pi+ESPN` in TFT footer
- [ ] During a race, status changes from `pre` → `live` → `finished`

---
## Why the Raspberry Pi Server is Necessary (F1 Live LED System)

### Initial Assumption: Multiple API Calls Required

In many sports APIs (including official Formula 1 data sources), fetching live race data typically requires multiple dependent API calls:

1. Fetch current event / race information
2. Fetch live driver positions / timing
3. Fetch additional statistics (fastest lap, classifications, etc.)

This leads to a chained request flow:

```
Get race → Get competitors → Get stats
```

Such a design would be inefficient and unreliable on constrained hardware like the ESP8266 (NodeMCU), due to:

* Limited memory
* TLS/HTTPS overhead
* Increased latency
* Complex error handling

---

### Reality: ESPN Scoreboard is an Aggregated Endpoint

The ESPN endpoint used in this project:

```
https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard
```

already provides a **fully aggregated snapshot** of the race.

Internally, ESPN likely performs:

```
Fetch race data
Fetch competitors
Fetch stats
Merge everything
→ Return single JSON response
```

So instead of multiple API calls, the system uses:

```
NodeMCU → Raspberry Pi → ESPN (single request)
```

---

### Why the Raspberry Pi is Still Essential

Even though only **one API call** is required, the Raspberry Pi plays a critical role as a processing layer.

#### 1) JSON Parsing Constraints

The ESPN response is large and deeply nested.

* ESP8266 has ~40–50 KB usable heap
* ArduinoJson struggles with large payloads
* Risk of crashes due to memory fragmentation

The Raspberry Pi safely parses and extracts only the required data.

---

#### 2) HTTPS / TLS Handling

Making HTTPS requests requires a TLS handshake:

* Slow and unreliable on ESP8266
* Certificate validation issues
* Frequent connection failures

The Raspberry Pi handles all HTTPS communication reliably.

---

#### 3) Data Normalisation (Critical for LED Logic)

Different APIs return inconsistent constructor names:

```
"McLaren F1 Team" → "McLaren"
"Mercedes-AMG Petronas" → "Mercedes"
```

Your NodeMCU depends on exact string matches for LED control.
The Raspberry Pi ensures consistent naming via normalization.

---

#### 4) Champion Projection Logic

The Raspberry Pi computes live championship projections using:

* FIA points system
* Fastest lap bonus (+1 point if in top 10)
* Constructor aggregation (both drivers)
* Real-time standings updates

Example:

```
project_champion(live_results)
```

This logic is computationally intensive and unsuitable for ESP8266.

---

#### 5) Polling, Rate Limiting & Stability

The Raspberry Pi:

* Polls ESPN every 10 seconds
* Handles API failures gracefully
* Maintains cached state

Direct polling from NodeMCU would:

* Increase failure rates
* Risk API throttling
* Complicate retry logic

---

### System Architecture

```
ESPN API (complex, heavy, nested JSON)
        ↓
Raspberry Pi (processing layer)
  - Fetches data
  - Parses JSON
  - Normalises names
  - Computes championship logic
        ↓
NodeMCU (display layer)
  - Consumes simplified JSON
  - Controls LED output
```

---

### Final Takeaways

* Multiple API calls are **not required** for ESPN scoreboard
* Raspberry Pi is still **essential** as a processing and abstraction layer
* The current architecture is efficient, scalable, and robust

The correct mental model is:

> The ESP8266 should not directly handle complex APIs, large JSON payloads, TLS, or race logic.

Instead, it acts as a lightweight display client, while the Raspberry Pi serves as the intelligent backend.

---

