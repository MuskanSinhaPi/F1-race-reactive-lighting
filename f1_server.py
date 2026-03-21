#!/usr/bin/env python3
"""
F1 Live Server — PRODUCTION
=============================
Data sources:
  Primary:  ESPN scoreboard API  — live race status, P1 constructor
  Standings: Jolpica (Ergast)   — driver standings (WDC projection + confirmation)

Push format:
  GET /update?team=X&status=Y [&gp=Z] [&wdc_team=A&wdc_status=projected|confirmed]

Pi:      192.168.1.53:8080
NodeMCU: 192.168.1.55:8080
"""

import time
import logging
import threading
import requests
from datetime import datetime, timezone
from flask import Flask, jsonify

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("f1")

app = Flask(__name__)

NODEMCU_IP   = "192.168.1.55"
NODEMCU_PORT = 8080

ESPN_URL     = "https://site.api.espn.com/apis/site/v2/sports/racing/f1/scoreboard"
JOLPICA_BASE = "https://api.jolpi.ca/ergast/f1"

POLL_NORMAL  = 60
POLL_RACE    = 10
POLL_FINISH  = 15
CHAMP_POLL_S = 120
STANDINGS_SEED_LEAD_S = 21600
RACE_POINTS = [25, 18, 15, 12, 10, 8, 6, 4, 2, 1]

CONSTRUCTOR_MAP = {
    "red bull racing":        "Red Bull",
    "red bull":               "Red Bull",
    "oracle red bull racing": "Red Bull",
    "mclaren":                "McLaren",
    "mclaren f1 team":        "McLaren",
    "mercedes":               "Mercedes",
    "mercedes-amg petronas":  "Mercedes",
    "ferrari":                "Ferrari",
    "scuderia ferrari":       "Ferrari",
    "aston martin":           "Aston Martin",
    "aston martin aramco":    "Aston Martin",
    "alpine":                 "Alpine",
    "alpine f1 team":         "Alpine",
    "haas":                   "Haas",
    "haas f1 team":           "Haas",
    "williams":               "Williams",
    "williams racing":        "Williams",
    "racing bulls":           "Racing Bulls",
    "visa cashapp rb":        "Racing Bulls",
    "rb f1 team":             "Racing Bulls",
    "sauber":                 "Audi",
    "stake f1":               "Audi",
    "audi":                   "Audi",
    "cadillac":               "Cadillac",
    "andretti cadillac":      "Cadillac",
    "twg cadillac":           "Cadillac",
}

def norm(name: str) -> str:
    if not name:
        return ""
    return CONSTRUCTOR_MAP.get(name.lower().strip(), name.strip())

_last_push_key  = None
_last_push_lock = threading.Lock()

def push_update(team: str, status: str,
                wdc_team: str = None, wdc_status: str = None,
                gp: str = None):
    global _last_push_key
    push_key = (team, status, wdc_team, wdc_status, gp)

    # finished and confirmed-WDC are one-time events — never dedup them.
    # All other statuses are deduplicated so we don't spam the NodeMCU
    # with identical live/scheduled/idle pushes every poll cycle.
    is_critical = (status == "finished" or wdc_status == "confirmed")

    if not is_critical:
        with _last_push_lock:
            if _last_push_key == push_key:
                log.debug(f"[PUSH] dedup skip — {status} {team}")
                return
            _last_push_key = push_key

    try:
        params = f"team={team.replace(' ', '+')}&status={status}"
        if gp:         params += f"&gp={gp.replace(' ', '+')}"
        if wdc_team:   params += f"&wdc_team={wdc_team.replace(' ', '+')}"
        if wdc_status: params += f"&wdc_status={wdc_status}"
        url = f"http://{NODEMCU_IP}:{NODEMCU_PORT}/update?{params}"
        log.info(f"[PUSH] -> {url}")
        r = requests.get(url, timeout=2)
        log.info(f"[PUSH] OK {r.status_code}")

        # For critical events, update dedup key only on success so
        # a failed push will be retried on the next poll cycle
        if is_critical:
            with _last_push_lock:
                _last_push_key = push_key

    except Exception as e:
        log.error(f"[PUSH] FAILED — {e}")
        # Non-critical: clear dedup so next poll retries
        # Critical: dedup key was never set, so next poll retries automatically
        if not is_critical:
            with _last_push_lock:
                _last_push_key = None

def reset_push_dedup():
    global _last_push_key
    with _last_push_lock:
        _last_push_key = None

_lock = threading.Lock()

state = {
    "status":        "idle",
    "team":          "---",
    "gp":            "",
    "race_finished": False,
    "current_round":  0,
    "total_rounds":   24,
    "is_final_round": False,
    "race_start_utc": None,
    "wdc_driver":      None,
    "wdc_driver_team": None,
    "wdc_status":      None,
    "wdc_confirmed":   False,
    "base_driver_points": {},
    "driver_constructor": {},
    "standings_seeded_at_round": 0,
    "pre_race_seed_done": False,
    "pre_race_seed_time": None,
    "last_poll":       0,
    "last_champ_poll": 0,
}

def fetch_schedule():
    """Fetch total rounds for current season from current.json.
    Uses limit=1 to minimise response size — MRData.total is always
    the full season race count regardless of limit.
    """
    try:
        r = requests.get(f"{JOLPICA_BASE}/current.json?limit=1", timeout=10)
        r.raise_for_status()
        total = int(r.json()["MRData"]["total"])
        with _lock:
            state["total_rounds"] = total
        log.info(f"[Jolpica] season total: {total} rounds")
    except Exception as e:
        log.error(f"[Jolpica] schedule fetch failed: {e}")


def fetch_next_race_info():
    """Fetch next race start time and current round.
    FIX: does NOT use MRData.total from next.json to set total_rounds
    because next.json always returns total=1 (count of results, not
    season length). total_rounds is set by fetch_schedule() only.
    is_final_round is derived by comparing round against total_rounds
    already set by fetch_schedule().
    """
    try:
        r = requests.get(f"{JOLPICA_BASE}/current/next.json", timeout=10)
        r.raise_for_status()
        data  = r.json()
        races = data["MRData"]["RaceTable"]["Races"]
        if not races:
            log.info("[Jolpica] no next race found (end of season?)")
            return
        race     = races[0]
        rnd      = int(race["round"])
        date_str = race.get("date", "")
        time_str = race.get("time", "00:00:00Z")

        race_start = None
        if date_str:
            try:
                combined = f"{date_str}T{time_str}".replace("Z", "+00:00")
                race_start = datetime.fromisoformat(combined)
            except Exception:
                pass

        with _lock:
            total_rounds = state["total_rounds"]   # set by fetch_schedule()
            # next.json "round" is the UPCOMING round, so completed = round - 1
            state["current_round"]  = rnd - 1
            state["is_final_round"] = (rnd == total_rounds)
            if race_start:
                state["race_start_utc"] = race_start
                seed_time = race_start.timestamp() - STANDINGS_SEED_LEAD_S
                state["pre_race_seed_time"] = seed_time
                state["pre_race_seed_done"] = False
            is_final = state["is_final_round"]

        log.info(f"[Jolpica] next race: round {rnd}/{total_rounds}  "
                 f"final={is_final}  start={race_start}")
    except Exception as e:
        log.error(f"[Jolpica] next race fetch failed: {e}")


def fetch_driver_standings() -> dict:
    try:
        r = requests.get(
            f"{JOLPICA_BASE}/current/driverStandings.json", timeout=10)
        r.raise_for_status()
        data  = r.json()
        lists = data["MRData"]["StandingsTable"]["StandingsLists"]
        if not lists:
            return {}
        sl  = lists[0]
        rnd = int(sl["round"])
        standings = []
        for s in sl["DriverStandings"]:
            d    = s["Driver"]
            cons = s.get("Constructors", [{}])
            standings.append({
                "driver_id":   d["driverId"],
                "driver_name": f"{d['givenName']} {d['familyName']}",
                "constructor": norm(cons[0].get("name", "") if cons else ""),
                "points":      float(s["points"]),
                "position":    int(s["position"]),
            })
        return {"round": rnd, "standings": standings}
    except Exception as e:
        log.error(f"[Jolpica] driver standings failed: {e}")
        return {}


def seed_driver_standings_for_final_race():
    log.info("[Standings] seeding driver standings for WDC projection...")
    ds = fetch_driver_standings()
    if not ds.get("standings"):
        log.error("[Standings] failed to seed — WDC projection unavailable")
        return
    rnd = ds["round"]
    with _lock:
        state["base_driver_points"] = {
            s["driver_id"]: s["points"] for s in ds["standings"]}
        state["driver_constructor"] = {
            s["driver_id"]: s["constructor"] for s in ds["standings"]}
        state["standings_seeded_at_round"] = rnd
        state["pre_race_seed_done"] = True
    top5 = ds["standings"][:5]
    log.info(f"[Standings] seeded at round {rnd} — top 5:")
    for s in top5:
        log.info(f"  P{s['position']} {s['driver_name']} ({s['constructor']}) {s['points']:.0f}pts")


def project_wdc_from_live(competitors: list) -> tuple:
    with _lock:
        base_pts   = dict(state["base_driver_points"])
        driver_con = dict(state["driver_constructor"])
        seed_rnd   = state["standings_seeded_at_round"]

    if not base_pts:
        log.warning("[WDC proj] base standings not seeded — cannot project")
        return None, None

    con_drivers: dict = {}
    for did, con in driver_con.items():
        con_drivers.setdefault(con, []).append((base_pts.get(did, 0), did))
    for con in con_drivers:
        con_drivers[con].sort(reverse=True)

    projected    = dict(base_pts)
    con_assigned: dict = {}
    classified   = [c for c in competitors if c.get("classified", True)]

    for i, c in enumerate(classified):
        if i >= len(RACE_POINTS):
            break
        cname   = c.get("constructor", "")
        drivers = con_drivers.get(cname, [])
        if not drivers:
            continue
        slot = con_assigned.get(cname, 0)
        if slot >= len(drivers):
            continue
        pts = RACE_POINTS[i]
        if c.get("fastest_lap") and i < 10:
            pts += 1
        _, did = drivers[slot]
        projected[did] = projected.get(did, 0) + pts
        con_assigned[cname] = slot + 1

    if not projected:
        return None, None

    leader_id  = max(projected, key=lambda x: projected[x])
    leader_con = driver_con.get(leader_id, "")
    pts_sorted = sorted(projected.values(), reverse=True)
    gap        = pts_sorted[0] - pts_sorted[1] if len(pts_sorted) > 1 else 0
    log.info(f"[WDC proj] {leader_id} ({leader_con}) {pts_sorted[0]:.0f}pts (gap:+{gap:.0f})")
    return leader_id, leader_con


def poll_wdc_confirmation():
    log.info("[WDC poll] checking Jolpica for WDC confirmation...")
    ds = fetch_driver_standings()
    if not ds.get("standings"):
        log.warning("[WDC poll] no standings returned")
        return

    with _lock:
        total_rounds = state["total_rounds"]
        wdc_done     = state["wdc_confirmed"]

    if wdc_done:
        return

    if ds.get("round", 0) < total_rounds:
        log.info(f"[WDC poll] standings at round {ds['round']}, need {total_rounds} — waiting")
        return

    leader = ds["standings"][0]
    log.info(f"[WDC] CONFIRMED: {leader['driver_name']} ({leader['constructor']})")

    with _lock:
        state["wdc_driver"]      = leader["driver_name"]
        state["wdc_driver_team"] = leader["constructor"]
        state["wdc_status"]      = "confirmed"
        state["wdc_confirmed"]   = True
        current_team = state["team"]
        gp           = state["gp"]

    reset_push_dedup()
    push_update(current_team or "---", "finished",
                wdc_team=leader["constructor"],
                wdc_status="confirmed",
                gp=gp)


def fetch_espn() -> dict | None:
    try:
        r = requests.get(ESPN_URL, timeout=8)
        r.raise_for_status()
        return r.json()
    except Exception as e:
        log.error(f"[ESPN] fetch failed: {e}")
        return None


def parse_espn(data: dict) -> dict | None:
    try:
        events = data.get("events", [])
        if not events:
            return None
        event = events[0]
        gp    = event.get("name", "")

        competitions = event.get("competitions", [])
        race_comp = next(
            (c for c in competitions if c.get("type", {}).get("id") == "3"),
            competitions[0] if competitions else None
        )
        if not race_comp:
            return None

        stype       = race_comp.get("status", {}).get("type", {})
        status_name = stype.get("name", "")
        completed   = stype.get("completed", False)

        if completed or status_name == "STATUS_FINAL":
            status = "finished"
        elif status_name in ("STATUS_IN_PROGRESS", "STATUS_LIVE"):
            status = "live"
        elif status_name == "STATUS_DELAYED":
            status = "delayed"
        elif status_name == "STATUS_POSTPONED":
            status = "postponed"
        elif status_name == "STATUS_CANCELED":
            status = "cancelled"
        elif status_name in ("STATUS_SCHEDULED", "STATUS_PRE_RACE"):
            status = "scheduled"
        else:
            status = "idle"

        competitors = []
        for c in race_comp.get("competitors", []):
            team_name = ""
            athlete = c.get("athlete", {})
            if athlete:
                team_name = athlete.get("team", {}).get("displayName", "")
            if not team_name:
                team_name = c.get("vehicle", {}).get("manufacturer", "")
            if not team_name:
                team_name = c.get("team", {}).get("displayName", "")
            competitors.append({
                "pos":         int(c.get("order", c.get("pos", 99))),
                "constructor": norm(team_name),
                "fastest_lap": c.get("fastest", False),
                "classified":  c.get("status", {}).get("type", {}).get("id", "") != "DNF",
            })
        competitors.sort(key=lambda x: x["pos"])

        start_utc = None
        date_str = race_comp.get("date", "")
        if date_str:
            try:
                start_utc = datetime.fromisoformat(date_str.replace("Z", "+00:00"))
            except Exception:
                pass

        return {
            "status":      status,
            "gp":          gp,
            "completed":   completed,
            "competitors": competitors,
            "start_utc":   start_utc,
        }
    except Exception as e:
        log.error(f"[ESPN] parse error: {e}")
        return None


def in_race_window() -> bool:
    with _lock:
        start  = state.get("race_start_utc")
        status = state["status"]
    if status in ("live", "delayed"):
        return True
    if start is None:
        return False
    now   = datetime.now(timezone.utc)
    delta = (start - now).total_seconds()
    return -7200 < delta < 14400


def poll_loop():
    log.info("=" * 62)
    log.info("  F1 LIVE SERVER — PRODUCTION")
    log.info(f"  Pi 192.168.1.53:8080  →  NodeMCU {NODEMCU_IP}:{NODEMCU_PORT}")
    log.info("=" * 62)

    # fetch_schedule MUST run before fetch_next_race_info so total_rounds
    # is correct before is_final_round is calculated
    fetch_schedule()
    fetch_next_race_info()
    threading.Thread(
        target=seed_driver_standings_for_final_race, daemon=True).start()

    while True:
        now_ts = time.time()

        with _lock:
            last_poll   = state["last_poll"]
            status      = state["status"]
            race_fin    = state["race_finished"]
            is_final    = state["is_final_round"]
            wdc_done    = state["wdc_confirmed"]
            last_champ  = state["last_champ_poll"]
            seed_done   = state["pre_race_seed_done"]
            seed_time   = state["pre_race_seed_time"]

        if is_final and not seed_done and seed_time is not None:
            if now_ts >= seed_time:
                log.info("[Standings] 6h pre-race seed triggered")
                threading.Thread(
                    target=seed_driver_standings_for_final_race, daemon=True).start()
                with _lock:
                    state["pre_race_seed_done"] = True

        if status in ("live", "delayed") or in_race_window():
            interval = POLL_RACE
        elif status == "finished" and not race_fin:
            interval = POLL_FINISH
        else:
            interval = POLL_NORMAL

        if now_ts - last_poll >= interval:
            with _lock:
                state["last_poll"] = now_ts
            _do_poll()

        with _lock:
            is_final = state["is_final_round"]
            race_fin = state["race_finished"]
            wdc_done = state["wdc_confirmed"]

        if is_final and race_fin and not wdc_done:
            if now_ts - last_champ >= CHAMP_POLL_S:
                with _lock:
                    state["last_champ_poll"] = now_ts
                poll_wdc_confirmation()

        time.sleep(1)


def _do_poll():
    data = fetch_espn()
    if data is None:
        with _lock:
            cur = state["status"]
        if cur not in ("finished", "idle", "cancelled", "postponed"):
            with _lock:
                state["status"] = "idle"
            push_update("---", "idle")
        return
    parsed = parse_espn(data)
    if parsed:
        _process(parsed)


def _process(parsed: dict):
    gp          = parsed["gp"]
    status      = parsed["status"]
    competitors = parsed["competitors"]
    start_utc   = parsed["start_utc"]
    p1          = competitors[0]["constructor"] if competitors else ""

    with _lock:
        is_final = state["is_final_round"]
        wdc_done = state["wdc_confirmed"]

    if start_utc:
        with _lock:
            state["race_start_utc"] = start_utc
            seed_done = state["pre_race_seed_done"]
            if is_final and not seed_done and state["pre_race_seed_time"] is None:
                seed_time = start_utc.timestamp() - STANDINGS_SEED_LEAD_S
                state["pre_race_seed_time"] = seed_time
                log.info(f"[Standings] seed scheduled for {datetime.fromtimestamp(seed_time)}")

    if gp:
        with _lock:
            state["gp"] = gp

    if status in ("cancelled", "postponed"):
        with _lock:
            state["status"] = status
        push_update("---", status, gp=gp)
        return

    if status == "delayed":
        with _lock:
            state["status"] = "delayed"
            state["team"]   = p1 or state["team"]
        push_update(p1 or "---", "delayed", gp=gp)
        return

    if status == "scheduled":
        with _lock:
            state["status"]        = "scheduled"
            state["race_finished"] = False
        push_update("---", "scheduled", gp=gp)
        return

    if status == "live":
        with _lock:
            prev_team              = state["team"]
            state["status"]        = "live"
            if p1: state["team"]   = p1
            state["race_finished"] = False

        wdc_team = None; wdc_status = None
        if is_final and competitors and not wdc_done:
            _, wdc_team = project_wdc_from_live(competitors)
            if wdc_team:
                wdc_status = "projected"
                with _lock:
                    state["wdc_driver_team"] = wdc_team
                    state["wdc_status"]      = "projected"
        elif is_final and wdc_done:
            with _lock:
                wdc_team   = state["wdc_driver_team"]
                wdc_status = "confirmed"

        push_update(p1 or prev_team or "---", "live",
                    wdc_team=wdc_team, wdc_status=wdc_status, gp=gp)
        return

    if status == "finished":
        with _lock:
            already   = state["race_finished"]
            prev_team = state["team"]
        if not already:
            log.info(f"Race finished — P1: {p1}  GP: {gp}")
        with _lock:
            state["status"]        = "finished"
            if p1: state["team"]   = p1
            state["race_finished"] = True

        wdc_team = None; wdc_status = None
        if is_final and competitors and not wdc_done:
            _, wdc_team = project_wdc_from_live(competitors)
            if wdc_team:
                wdc_status = "projected"
                with _lock:
                    state["wdc_driver_team"] = wdc_team
                    state["wdc_status"]      = "projected"
        elif is_final and wdc_done:
            with _lock:
                wdc_team   = state["wdc_driver_team"]
                wdc_status = "confirmed"

        push_update(p1 or prev_team or "---", "finished",
                    wdc_team=wdc_team, wdc_status=wdc_status, gp=gp)


@app.route("/p1")
def p1_endpoint():
    with _lock:
        s = dict(state)
    resp = {
        "team":               s.get("team", "---"),
        "status":             s.get("status", "idle"),
        "gp":                 s.get("gp", ""),
        "round":              s.get("current_round", 0),
        "total":              s.get("total_rounds", 24),
        "is_final_round":     s.get("is_final_round", False),
        "standings_round":    s.get("standings_seeded_at_round", 0),
        "pre_race_seed_done": s.get("pre_race_seed_done", False),
    }
    if s.get("wdc_driver_team"):
        resp["wdc_team"]   = s["wdc_driver_team"]
        resp["wdc_driver"] = s.get("wdc_driver", "")
        resp["wdc_status"] = s.get("wdc_status", "")
    return jsonify(resp)


@app.route("/status")
def status_endpoint():
    with _lock:
        pts   = state.get("base_driver_points", {})
        d_con = state.get("driver_constructor", {})
        s     = {k: v for k, v in state.items()
                 if k not in ("base_driver_points", "driver_constructor")}
        s["standings_top5"] = [
            {"driver": did, "constructor": d_con.get(did, ""), "points": f"{p:.0f}"}
            for did, p in sorted(pts.items(), key=lambda x: -x[1])[:5]
        ]
        seed_time = state.get("pre_race_seed_time")
        s["pre_race_seed_scheduled"] = (
            datetime.fromtimestamp(seed_time).isoformat() if seed_time else None)
    with _last_push_lock:
        s["last_push_key"] = str(_last_push_key)
    return jsonify(s)


@app.route("/refresh_standings")
def refresh_standings():
    threading.Thread(target=seed_driver_standings_for_final_race, daemon=True).start()
    return jsonify({"ok": True, "msg": "standings refresh triggered"})


@app.route("/force_push")
def force_push():
    reset_push_dedup()
    with _lock:
        team     = state.get("team", "---")
        status   = state.get("status", "idle")
        gp       = state.get("gp", "")
        wdc_team = state.get("wdc_driver_team")
        wdc_st   = state.get("wdc_status")
    push_update(team, status, wdc_team=wdc_team, wdc_status=wdc_st, gp=gp)
    return jsonify({"ok": True, "pushed": status})


if __name__ == "__main__":
    t = threading.Thread(target=poll_loop, daemon=True)
    t.start()
    app.run(host="0.0.0.0", port=8080)
