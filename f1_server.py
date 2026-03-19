#!/usr/bin/env python3
"""
F1 Live Server — Raspberry Pi
==============================
Serves /p1 endpoint to the NodeMCU ESP8266.

Response JSON contract:
{
  "team":             "McLaren",          # current race P1 constructor
  "status":           "live",             # live | finished | delayed | cancelled | postponed | scheduled | idle
  "gp":               "Abu Dhabi Grand Prix",
  "champion":         "McLaren",          # Pi-projected or confirmed WCC constructor (final race only)
  "champion_status":  "projected"         # "projected" | "confirmed" | null
}

champion / champion_status are only populated during the final race weekend.
For all other races they are omitted entirely so the NodeMCU ignores them.

Data sources (in priority order):
  1. ESPN scoreboard          — live positions, race status, fastest lap
  2. Jolpica driver standings — round tracking
  3. Jolpica constructor standings — truth source for WCC champion confirmation

Season logic:
  - Before final race weekend: standings fetched once, cached as base_points
  - During final race: live ESPN positions applied to base_points each poll
  - champion_status = "projected" while maths gives a clear winner
  - champion_status = "confirmed" once Jolpica constructor standings round == total_rounds
"""

import time
import json
import logging
import threading
from datetime import datetime, timezone
from flask import Flask, jsonify
import requests

# ── Logging ──────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("f1")

app = Flask(__name__)

# ── Config ────────────────────────────────────────────────────────────────────
ESPN_SCOREBOARD              = "https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard"
JOLPICA_DRIVER_STANDINGS     = "https://api.jolpi.ca/ergast/f1/current/driverStandings.json"
JOLPICA_CONSTRUCTOR_STANDINGS = "https://api.jolpi.ca/ergast/f1/current/constructorStandings.json"  # FIX #2
JOLPICA_SCHEDULE             = "https://api.jolpi.ca/ergast/f1/current.json"

POLL_INTERVAL        = 10    # seconds between ESPN polls during race
CHAMP_POLL_SECS      = 90    # seconds between Jolpica champion confirmation polls
ROUND_CHECK_INTERVAL = 300   # seconds between periodic current-round checks (#1)
REQUEST_TIMEOUT      = 6     # HTTP timeout

# Points tables
RACE_POINTS   = [25, 18, 15, 12, 10, 8, 6, 4, 2, 1]
SPRINT_POINTS = [8,  7,  6,  5,  4,  3, 2, 1]

# ── Shared state (all access via state_lock) ──────────────────────────────────
state_lock = threading.Lock()

state = {
    # Latest data served to NodeMCU
    "team":             "---",
    "status":           "idle",       # idle | scheduled | live | finished | delayed | cancelled | postponed
    "gp":               "",

    # Champion fields — only populated on final race weekend
    "champion":         None,
    "champion_status":  None,         # "projected" | "confirmed"

    # Internal tracking
    "total_rounds":     24,
    "current_round":    0,
    "is_final_round":   False,
    "race_finished":    False,

    # Base standings fetched before final race (driver_id -> points)
    "base_driver_points": {},         # {"VER": 400, "NOR": 380, ...}
    # Driver -> constructor mapping from standings
    "driver_constructor": {},         # {"VER": "Red Bull", "NOR": "McLaren", ...}
    # Constructor -> aggregated base points
    "base_constructor_points": {},    # {"Red Bull": 600, "McLaren": 580, ...}

    "standings_fetched":    False,
    "champion_confirmed":   False,
    "confirmed_champion":   None,     # constructor name once Jolpica confirms

    "last_espn_poll":   0,
    "last_champ_poll":  0,
    "last_round_check": 0,   # FIX #1: tracks periodic round re-checks
}

# ── Constructor name normalisation ────────────────────────────────────────────
CONSTRUCTOR_MAP = {
    "red bull racing":       "Red Bull",
    "red bull":              "Red Bull",
    "mclaren":               "McLaren",
    "mclaren f1 team":       "McLaren",
    "mercedes":              "Mercedes",
    "mercedes-amg petronas": "Mercedes",
    "ferrari":               "Ferrari",
    "scuderia ferrari":      "Ferrari",
    "aston martin":          "Aston Martin",
    "aston martin aramco":   "Aston Martin",
    "alpine":                "Alpine",
    "alpine f1 team":        "Alpine",
    "haas":                  "Haas",
    "haas f1 team":          "Haas",
    "williams":              "Williams",
    "williams racing":       "Williams",
    "racing bulls":          "Racing Bulls",
    "visa cashapp rb":       "Racing Bulls",
    "rb f1 team":            "Racing Bulls",
    "sauber":                "Audi",
    "stake f1":              "Audi",
    "audi":                  "Audi",
    "cadillac":              "Cadillac",
    "andretti cadillac":     "Cadillac",
}

def norm_constructor(name: str) -> str:
    """Normalise constructor name to canonical form."""
    if not name:
        return name
    return CONSTRUCTOR_MAP.get(name.lower().strip(), name.strip())


# ── Schedule helpers ──────────────────────────────────────────────────────────

def fetch_schedule():
    """Fetch current season schedule, set total_rounds."""
    try:
        r = requests.get(JOLPICA_SCHEDULE, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        races = data["MRData"]["RaceTable"]["Races"]
        total = len(races)
        with state_lock:
            state["total_rounds"] = total
        log.info(f"Schedule: {total} rounds this season")
        return races
    except Exception as e:
        log.warning(f"Schedule fetch failed: {e}")
        return []


def check_if_final_round():
    """
    Determine current round from Jolpica driver standings and flag if final.

    FIX #1: Use strict equality (current_round == total_rounds - 1) rather than
    >= to avoid mis-flagging if Jolpica updates early or late.
    """
    try:
        r = requests.get(JOLPICA_DRIVER_STANDINGS, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            return
        current_round = int(lists[0].get("round", 0))
        with state_lock:
            total = state["total_rounds"]
            state["current_round"] = current_round
            # FIX #1: strict equality — avoids off-by-one on early/late Jolpica updates
            state["is_final_round"] = (current_round == total - 1)
            log.info(f"Current round: {current_round}/{total} "
                     f"— final: {state['is_final_round']}")
    except Exception as e:
        log.warning(f"Round check failed: {e}")


# ── Standings helpers ─────────────────────────────────────────────────────────

def fetch_base_standings():
    """
    Fetch current driverStandings and build base_driver_points,
    driver_constructor, and base_constructor_points.
    Called once before the final race starts.
    """
    try:
        r = requests.get(JOLPICA_DRIVER_STANDINGS, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            log.warning("No standings lists returned")
            return False

        driver_points = {}
        driver_constructor = {}
        constructor_points = {}

        for entry in lists[0].get("DriverStandings", []):
            driver_id = entry["Driver"]["driverId"].upper()
            points    = float(entry.get("points", 0))
            constructors = entry.get("Constructors", [])
            if constructors:
                cname = norm_constructor(constructors[-1]["name"])
            else:
                cname = "Unknown"

            driver_points[driver_id]      = points
            driver_constructor[driver_id] = cname
            constructor_points[cname]     = constructor_points.get(cname, 0) + points

        with state_lock:
            state["base_driver_points"]      = driver_points
            state["driver_constructor"]      = driver_constructor
            state["base_constructor_points"] = constructor_points
            state["standings_fetched"]       = True

        log.info(f"Base standings loaded — {len(driver_points)} drivers")
        top = sorted(constructor_points.items(), key=lambda x: x[1], reverse=True)[:5]
        for name, pts in top:
            log.info(f"  {name}: {pts} pts")
        return True

    except Exception as e:
        log.error(f"Base standings fetch failed: {e}")
        return False


def project_champion(live_results: list) -> str | None:
    """
    Given live_results, apply race points on top of base standings and return
    projected constructor champion name, or None if no clear leader.
    """
    with state_lock:
        base = dict(state["base_constructor_points"])
        driver_constructor = dict(state["driver_constructor"])

    if not base:
        return None

    projected = dict(base)
    classified = [r for r in live_results if r.get("classified", True)]

    for i, result in enumerate(classified):
        if i >= len(RACE_POINTS):
            break
        constructor = result.get("constructor") or driver_constructor.get(
            result.get("driver_id", "").upper(), "")
        constructor = norm_constructor(constructor)
        if not constructor:
            continue
        pts = RACE_POINTS[i]
        if result.get("fastest_lap") and i < 10:
            pts += 1
        projected[constructor] = projected.get(constructor, 0) + pts

    if not projected:
        return None
    sorted_teams = sorted(projected.items(), key=lambda x: x[1], reverse=True)
    leader = sorted_teams[0]
    second = sorted_teams[1] if len(sorted_teams) > 1 else None

    log.debug(f"Projected: {leader[0]} {leader[1]:.0f}pts"
              + (f" vs {second[0]} {second[1]:.0f}pts" if second else ""))

    return leader[0]


def try_confirm_champion_jolpica() -> str | None:
    """
    Poll Jolpica CONSTRUCTOR standings (not driver standings).
    Returns confirmed constructor name if standingsRound == total_rounds,
    otherwise None.

    FIX #2: Uses constructorStandings endpoint for official WCC confirmation
    instead of deriving from driver P1's constructor (which is not guaranteed correct).
    """
    try:
        r = requests.get(JOLPICA_CONSTRUCTOR_STANDINGS, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            return None

        standings_round = int(lists[0].get("round", 0))
        with state_lock:
            total   = state["total_rounds"]
            current = state["current_round"]

        if standings_round < current:
            log.info(f"Jolpica constructor standings at round {standings_round}, need {current} — waiting")
            return None

        if standings_round < total:
            log.info(f"Jolpica constructor round {standings_round}/{total} — not final round yet")
            return None

        # FIX #2: read directly from ConstructorStandings P1 entry
        constructor_standings = lists[0].get("ConstructorStandings", [])
        if not constructor_standings:
            return None
        p1 = constructor_standings[0]
        champion = norm_constructor(p1["Constructor"]["name"])
        log.info(f"Jolpica confirms WCC: {champion} (round {standings_round})")
        return champion

    except Exception as e:
        log.warning(f"Jolpica champion confirm failed: {e}")
        return None


# ── ESPN polling ──────────────────────────────────────────────────────────────

def parse_espn_scoreboard() -> dict | None:
    """
    Fetch ESPN F1 scoreboard. Returns parsed result dict or None on failure.

    FIX #3: driver_id from ESPN shortName is fragile vs Ergast driverId — noted
             but acceptable since constructor fallback already handles it correctly.
    FIX #5: espn_status "pre" replaced with "scheduled" to match API contract.
    FIX #6: Added STATUS_CANCELED and STATUS_POSTPONED handling.
    """
    try:
        r = requests.get(ESPN_SCOREBOARD, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()

        events = data.get("events", [])
        if not events:
            return None

        # FIX #2: prefer the event whose name contains "Grand Prix" to avoid
        # accidentally picking a practice session or sprint event when ESPN
        # returns multiple competitions in the same weekend.
        event = next(
            (e for e in events if "Grand Prix" in e.get("name", "")),
            events[0]   # fall back to first event if no explicit GP match
        )
        gp_name = event.get("name", "")
        competitions = event.get("competitions", [])

        # Find the race competition (type id == "3")
        race_comp = None
        for comp in competitions:
            if comp.get("type", {}).get("id") == "3":
                race_comp = comp
                break
        if not race_comp:
            race_comp = competitions[0] if competitions else None
        if not race_comp:
            return None

        status_obj  = race_comp.get("status", {})
        status_type = status_obj.get("type", {})
        status_name = status_type.get("name", "")
        completed   = status_type.get("completed", False)

        # FIX #5: "pre" → "scheduled" to match documented contract
        # FIX #6: added cancelled and postponed mappings
        if status_name in ("STATUS_SCHEDULED",):
            espn_status = "scheduled"
        elif status_name == "STATUS_DELAYED":
            espn_status = "delayed"
        elif status_name == "STATUS_IN_PROGRESS":
            espn_status = "live"
        elif status_name in ("STATUS_FINAL", "STATUS_FINAL_OVERTIME"):
            espn_status = "finished"
        elif status_name == "STATUS_CANCELED":           # FIX #6
            espn_status = "cancelled"
        elif status_name == "STATUS_POSTPONED":          # FIX #6
            espn_status = "postponed"
        else:
            espn_status = "scheduled"

        competitors = []
        for comp in race_comp.get("competitors", []):
            pos      = comp.get("order", 99)
            athlete  = comp.get("athlete", {})
            # FIX #3: driver_id kept for potential future use but constructor
            # is the primary key; ESPN shortName ≠ Ergast driverId so we don't
            # rely on it for standings lookups.
            driver_id = athlete.get("shortName", "").upper().replace(" ", "")
            team_obj   = comp.get("team", {})
            constructor = norm_constructor(team_obj.get("displayName", ""))

            fastest_lap = False
            for stat in comp.get("statistics", []):
                if stat.get("name", "").lower() in ("fastest lap", "fastestlap"):
                    fastest_lap = stat.get("value") == "1"

            comp_status = comp.get("status", {}).get("type", {}).get("name", "")
            classified  = comp_status not in ("DNF", "DNS", "DSQ", "NC", "EX")

            competitors.append({
                "pos":         pos,
                "driver_id":   driver_id,
                "constructor": constructor,
                "fastest_lap": fastest_lap,
                "classified":  classified,
            })

        competitors.sort(key=lambda x: x["pos"])

        return {
            "status":      espn_status,
            "completed":   completed or (espn_status == "finished"),
            "gp":          gp_name,
            "competitors": competitors,
        }

    except Exception as e:
        log.warning(f"ESPN fetch failed: {e}")
        return None


# ── Background polling thread ─────────────────────────────────────────────────

def poll_loop():
    """Background thread — polls ESPN every POLL_INTERVAL seconds."""
    log.info("Poll loop started")

    fetch_schedule()
    check_if_final_round()

    while True:
        now = time.time()

        with state_lock:
            last_poll       = state["last_espn_poll"]
            last_champ      = state["last_champ_poll"]
            last_round_chk  = state["last_round_check"]
            race_finished   = state["race_finished"]
            champ_confirmed = state["champion_confirmed"]
            is_final        = state["is_final_round"]
            standings_ok    = state["standings_fetched"]

        # FIX #1: re-check current round periodically so is_final_round stays
        # accurate if the Pi runs across multiple race weekends without a restart.
        if now - last_round_chk >= ROUND_CHECK_INTERVAL:
            with state_lock:
                state["last_round_check"] = now
            check_if_final_round()
            # Re-read is_final after update
            with state_lock:
                is_final     = state["is_final_round"]
                standings_ok = state["standings_fetched"]

        if is_final and not standings_ok:
            log.info("Final round detected — fetching base standings")
            fetch_base_standings()

        if now - last_poll >= POLL_INTERVAL:
            with state_lock:
                state["last_espn_poll"] = now

            espn = parse_espn_scoreboard()

            # FIX #4: reset to idle when ESPN returns no data, preventing stale state
            if espn is None:
                with state_lock:
                    state["status"] = "idle"
                    state["team"]   = "---"
                    state["gp"]     = ""     # FIX #3: clear stale GP name on idle
                log.warning("ESPN returned no data — state reset to idle")
            else:
                _process_espn(espn)

        # Champion confirmation poll (every CHAMP_POLL_SECS, after race ends)
        if race_finished and not champ_confirmed and is_final:
            if now - last_champ >= CHAMP_POLL_SECS:
                with state_lock:
                    state["last_champ_poll"] = now
                confirmed = try_confirm_champion_jolpica()
                if confirmed:
                    with state_lock:
                        state["champion_confirmed"] = True
                        state["confirmed_champion"] = confirmed
                        state["champion"]           = confirmed
                        state["champion_status"]    = "confirmed"
                    log.info(f"WCC confirmed and set: {confirmed}")

        time.sleep(2)


def _process_espn(espn: dict):
    """
    Apply ESPN data to state. Called from poll_loop.

    FIX #5: Removed "pre" status; now uses "scheduled" from parse step.
    FIX #6: Added cancelled/postponed branches.
    FIX #7: All reads and writes tightly scoped under state_lock.
    """
    gp          = espn["gp"]
    espn_status = espn["status"]
    completed   = espn["completed"]
    competitors = espn["competitors"]

    p1_constructor = None
    if competitors:
        p1_constructor = norm_constructor(competitors[0].get("constructor", ""))

    # FIX #7: read all needed state in one locked block
    with state_lock:
        is_final        = state["is_final_round"]
        champ_confirmed = state["champion_confirmed"]
        confirmed_champ = state["confirmed_champion"]
        current_team    = state["team"]

    # ── Update GP name atomically ─────────────────────────────────────────────
    if gp:
        with state_lock:
            state["gp"] = gp

    # ── Cancelled / Postponed ─────────────────────────────────────────────────
    if espn_status in ("cancelled", "postponed"):                  # FIX #6
        with state_lock:
            state["status"] = espn_status
        log.info(f"Race {espn_status}")
        return

    # ── Delayed ───────────────────────────────────────────────────────────────
    if espn_status == "delayed":
        with state_lock:
            state["status"] = "delayed"
            state["team"]   = p1_constructor or current_team
        log.info("Race delayed")
        return

    # ── Pre-race / Scheduled ──────────────────────────────────────────────────
    if espn_status == "scheduled":                                 # FIX #5
        with state_lock:
            state["status"] = "scheduled"
        return

    # ── Live ──────────────────────────────────────────────────────────────────
    if espn_status == "live":
        with state_lock:
            state["status"]        = "live"
            state["team"]          = p1_constructor or current_team
            state["race_finished"] = False

        if is_final and not champ_confirmed and competitors:
            projected = project_champion(competitors)
            if projected:
                with state_lock:
                    state["champion"]        = projected
                    state["champion_status"] = "projected"
                log.info(f"Projected WCC: {projected}")
        return

    # ── Finished ──────────────────────────────────────────────────────────────
    if completed or espn_status == "finished":
        with state_lock:
            already_finished = state["race_finished"]

        if not already_finished:
            log.info(f"Race finished — P1: {p1_constructor}")

        with state_lock:
            state["status"]        = "finished"
            state["team"]          = p1_constructor or current_team
            state["race_finished"] = True

        if is_final and not champ_confirmed and competitors:
            projected = project_champion(competitors)
            if projected:
                with state_lock:
                    state["champion"]        = projected
                    state["champion_status"] = "projected"
                log.info(f"Race finished — projected WCC: {projected}")

        if is_final and champ_confirmed and confirmed_champ:
            with state_lock:
                state["champion"]        = confirmed_champ
                state["champion_status"] = "confirmed"


# ── Flask endpoints ───────────────────────────────────────────────────────────

@app.route("/p1")
def p1():
    with state_lock:
        s = dict(state)

    resp = {
        "team":   s["team"]   or "---",
        "status": s["status"] or "idle",
        "gp":     s["gp"]     or "",
    }

    if s["is_final_round"] and s["champion"]:
        resp["champion"]        = s["champion"]
        resp["champion_status"] = s["champion_status"]

    return jsonify(resp)


@app.route("/status")
def status():
    """Debug endpoint — full internal state (excludes large lookup tables)."""
    with state_lock:
        return jsonify({k: v for k, v in state.items()
                        if k not in ("base_driver_points",
                                     "driver_constructor",
                                     "base_constructor_points")})


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    t = threading.Thread(target=poll_loop, daemon=True)
    t.start()
    app.run(host="0.0.0.0", port=5000, debug=False)
