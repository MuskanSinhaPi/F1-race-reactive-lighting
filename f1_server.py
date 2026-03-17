#!/usr/bin/env python3
"""
F1 Live Server — Raspberry Pi
==============================
Serves /p1 endpoint to the NodeMCU ESP8266.

Response JSON contract:
{
  "team":             "McLaren",          # current race P1 constructor
  "status":           "live",             # live | finished | delayed | cancelled | postponed
  "gp":               "Abu Dhabi Grand Prix",
  "champion":         "McLaren",          # Pi-projected or confirmed WDC constructor (final race only)
  "champion_status":  "projected"         # "projected" | "confirmed" | null
}

champion / champion_status are only populated during the final race weekend.
For all other races they are omitted entirely so the NodeMCU ignores them.

Data sources (in priority order):
  1. ESPN scoreboard  — live positions, race status, fastest lap
  2. Jolpica          — official standings (truth source for champion confirmation)

Season logic:
  - Before final race weekend: standings fetched once, cached as base_points
  - During final race: live ESPN positions applied to base_points each poll
  - champion_status = "projected" while maths gives a clear winner
  - champion_status = "confirmed" once Jolpica standings round == total_rounds
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
ESPN_SCOREBOARD = "https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard"
JOLPICA_STANDINGS = "https://api.jolpi.ca/ergast/f1/current/driverStandings.json"
JOLPICA_SCHEDULE  = "https://api.jolpi.ca/ergast/f1/current.json"

POLL_INTERVAL   = 10      # seconds between ESPN polls during race
CHAMP_POLL_SECS = 90      # seconds between Jolpica champion confirmation polls
REQUEST_TIMEOUT = 6       # HTTP timeout

# Points tables
RACE_POINTS   = [25, 18, 15, 12, 10, 8, 6, 4, 2, 1]
SPRINT_POINTS = [8,  7,  6,  5,  4,  3, 2, 1]

# ── Shared state (all access via state_lock) ──────────────────────────────────
state_lock = threading.Lock()

state = {
    # Latest data served to NodeMCU
    "team":             "---",
    "status":           "idle",       # idle | live | finished | delayed | cancelled | postponed
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
}

# ── Constructor name normalisation ────────────────────────────────────────────
# Maps ESPN / Jolpica name variants to canonical names matching NodeMCU getTeamID()
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
    """Fetch current season schedule, set total_rounds and is_final_round."""
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
    """Determine current round from Jolpica standings and flag if final."""
    try:
        r = requests.get(JOLPICA_STANDINGS, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            return
        current_round = int(lists[0].get("round", 0))
        with state_lock:
            state["current_round"] = current_round
            state["is_final_round"] = (current_round + 1 >= state["total_rounds"])
            log.info(f"Current round: {current_round}/{state['total_rounds']} "
                     f"— final: {state['is_final_round']}")
    except Exception as e:
        log.warning(f"Round check failed: {e}")


# ── Standings helpers ─────────────────────────────────────────────────────────

def fetch_base_standings():
    """
    Fetch current driverStandings and build base_driver_points,
    driver_constructor, and base_constructor_points.
    Called once before the final race starts.
    Points already include all previous races + sprints.
    """
    try:
        r = requests.get(JOLPICA_STANDINGS, timeout=REQUEST_TIMEOUT)
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
        # Log top 5 constructors
        top = sorted(constructor_points.items(), key=lambda x: x[1], reverse=True)[:5]
        for name, pts in top:
            log.info(f"  {name}: {pts} pts")
        return True

    except Exception as e:
        log.error(f"Base standings fetch failed: {e}")
        return False


def project_champion(live_results: list) -> str | None:
    """
    Given live_results = [{"pos": 1, "driver_id": "NOR", "constructor": "McLaren",
                            "fastest_lap": False, "classified": True}, ...]
    Apply race points on top of base standings and return projected
    constructor champion name, or None if no clear leader.

    Handles:
    - Fastest lap +1 point (only if driver finishes top 10)
    - Only classified finishers receive points
    - Constructor aggregation across both drivers
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

        # Fastest lap: +1 only if driver finishes top 10
        if result.get("fastest_lap") and i < 10:
            pts += 1

        projected[constructor] = projected.get(constructor, 0) + pts

    # Find leader
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
    Poll Jolpica standings. Returns confirmed constructor name if
    standingsRound == total_rounds (Jolpica has processed the final race),
    otherwise None.
    """
    try:
        r = requests.get(JOLPICA_STANDINGS, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            return None

        standings_round = int(lists[0].get("round", 0))
        with state_lock:
            total = state["total_rounds"]
            current = state["current_round"]

        if standings_round < current:
            log.info(f"Jolpica standings at round {standings_round}, need {current} — waiting")
            return None

        if standings_round < total:
            log.info(f"Jolpica round {standings_round}/{total} — not final round")
            return None

        # Standings are current and this is the final round
        p1 = lists[0]["DriverStandings"][0]
        constructors = p1.get("Constructors", [])
        if not constructors:
            return None
        champion = norm_constructor(constructors[-1]["name"])
        log.info(f"Jolpica confirms champion: {champion} (round {standings_round})")
        return champion

    except Exception as e:
        log.warning(f"Jolpica champion confirm failed: {e}")
        return None


# ── ESPN polling ──────────────────────────────────────────────────────────────

def parse_espn_scoreboard() -> dict:
    """
    Fetch ESPN F1 scoreboard. Returns:
    {
      "status":      "pre" | "in" | "post",
      "completed":   bool,
      "gp":          str,
      "competitors": [{"pos": int, "driver_id": str, "constructor": str,
                       "fastest_lap": bool, "classified": bool}, ...]
    }
    Returns None on failure.
    """
    try:
        r = requests.get(ESPN_SCOREBOARD, timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        data = r.json()

        events = data.get("events", [])
        if not events:
            return None

        event = events[0]
        gp_name = event.get("name", "")
        competitions = event.get("competitions", [])

        # Find the race competition (type id == "3")
        race_comp = None
        for comp in competitions:
            if comp.get("type", {}).get("id") == "3":
                race_comp = comp
                break
        if not race_comp:
            # Fall back to first competition
            race_comp = competitions[0] if competitions else None
        if not race_comp:
            return None

        status_obj  = race_comp.get("status", {})
        status_type = status_obj.get("type", {})
        status_name = status_type.get("name", "")
        completed   = status_type.get("completed", False)

        if status_name in ("STATUS_SCHEDULED", "STATUS_DELAYED"):
            espn_status = "pre"
        elif status_name == "STATUS_IN_PROGRESS":
            espn_status = "in"
        elif status_name in ("STATUS_FINAL", "STATUS_FINAL_OVERTIME"):
            espn_status = "post"
        else:
            espn_status = "pre"

        if status_name == "STATUS_DELAYED":
            espn_status = "delayed"

        competitors = []
        for comp in race_comp.get("competitors", []):
            pos = comp.get("order", 99)
            athlete = comp.get("athlete", {})
            driver_id = athlete.get("shortName", "").upper().replace(" ", "")
            # ESPN uses "team" for constructor
            team_obj = comp.get("team", {})
            constructor = norm_constructor(team_obj.get("displayName", ""))

            # Fastest lap flag
            fastest_lap = False
            for stat in comp.get("statistics", []):
                if stat.get("name", "").lower() in ("fastest lap", "fastestlap"):
                    fastest_lap = stat.get("value") == "1"

            # Classified: ESPN marks DNFs/DNQs in status
            comp_status = comp.get("status", {}).get("type", {}).get("name", "")
            classified = comp_status not in ("DNF", "DNS", "DSQ", "NC", "EX")

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
            "completed":   completed or (espn_status == "post"),
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

    # Initial schedule fetch
    fetch_schedule()
    check_if_final_round()

    while True:
        now = time.time()

        with state_lock:
            last_poll       = state["last_espn_poll"]
            last_champ      = state["last_champ_poll"]
            race_finished   = state["race_finished"]
            champ_confirmed = state["champion_confirmed"]
            is_final        = state["is_final_round"]
            standings_ok    = state["standings_fetched"]

        # Fetch base standings once before the final race if not yet done
        if is_final and not standings_ok:
            log.info("Final round detected — fetching base standings")
            fetch_base_standings()

        # ESPN poll
        if now - last_poll >= POLL_INTERVAL:
            with state_lock:
                state["last_espn_poll"] = now

            espn = parse_espn_scoreboard()
            if espn:
                _process_espn(espn)

        # Champion confirmation poll (every CHAMP_POLL_SECS, after race ends)
        if race_finished and not champ_confirmed and is_final:
            if now - last_champ >= CHAMP_POLL_SECS:
                with state_lock:
                    state["last_champ_poll"] = now
                confirmed = try_confirm_champion_jolpica()
                if confirmed:
                    with state_lock:
                        state["champion_confirmed"]  = True
                        state["confirmed_champion"]  = confirmed
                        state["champion"]            = confirmed
                        state["champion_status"]     = "confirmed"
                    log.info(f"Champion confirmed and set: {confirmed}")

        time.sleep(2)


def _process_espn(espn: dict):
    """Apply ESPN data to state. Called from poll_loop."""
    gp          = espn["gp"]
    espn_status = espn["status"]
    completed   = espn["completed"]
    competitors = espn["competitors"]

    p1_constructor = None
    if competitors:
        p1_constructor = norm_constructor(competitors[0].get("constructor", ""))

    with state_lock:
        is_final        = state["is_final_round"]
        champ_confirmed = state["champion_confirmed"]
        confirmed_champ = state["confirmed_champion"]

    if gp:
        with state_lock:
            state["gp"] = gp

    # ── Determine race status ─────────────────────────────────────────────────
    if espn_status == "delayed":
        with state_lock:
            state["status"] = "delayed"
            state["team"]   = p1_constructor or state["team"]
        log.info("Race delayed")
        return

    if espn_status == "pre":
        with state_lock:
            state["status"] = "pre"
        return

    if espn_status == "in" or (espn_status == "post" and not completed):
        # Race in progress
        with state_lock:
            state["status"]       = "live"
            state["team"]         = p1_constructor or state["team"]
            state["race_finished"] = False

        # Champion projection for final race
        if is_final and not champ_confirmed and competitors:
            projected = project_champion(competitors)
            if projected:
                with state_lock:
                    state["champion"]        = projected
                    state["champion_status"] = "projected"
                log.info(f"Projected champion: {projected}")
        return

    if completed or espn_status == "post":
        with state_lock:
            already_finished = state["race_finished"]

        if not already_finished:
            log.info(f"Race finished — P1: {p1_constructor}")

        with state_lock:
            state["status"]        = "finished"
            state["team"]          = p1_constructor or state["team"]
            state["race_finished"] = True

        # On final race: set projected champ at finish if not yet confirmed
        if is_final and not champ_confirmed and competitors:
            projected = project_champion(competitors)
            if projected:
                with state_lock:
                    state["champion"]        = projected
                    state["champion_status"] = "projected"
                log.info(f"Race finished — projected champion: {projected}")

        # If already Jolpica-confirmed, keep it
        if is_final and champ_confirmed and confirmed_champ:
            with state_lock:
                state["champion"]        = confirmed_champ
                state["champion_status"] = "confirmed"


# ── Flask endpoint ────────────────────────────────────────────────────────────

@app.route("/p1")
def p1():
    with state_lock:
        s = dict(state)

    resp = {
        "team":   s["team"]   or "---",
        "status": s["status"] or "idle",
        "gp":     s["gp"]     or "",
    }

    # Only include champion fields during final race weekend
    if s["is_final_round"] and s["champion"]:
        resp["champion"]        = s["champion"]
        resp["champion_status"] = s["champion_status"]

    return jsonify(resp)


@app.route("/status")
def status():
    """Debug endpoint — full internal state."""
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
