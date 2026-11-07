#!/usr/bin/env python3
"""carbon-leaderboard-audit.py: outlier checks on a copy of the server's stats.json (server/src/stats.rs).

Usage:
  python3 tools/carbon-leaderboard-audit.py STATS_JSON [--races RACE_NAMES_JSON] [--show-names] [--json OUT]

Reads only. Prints, per leaderboard, the entries that look impossible or far out of line with that
board's own top 20, and why. Names are replaced by a short stable tag (#ab12cd) unless --show-names is
given, so the output can be pasted into notes without naming anyone.

A stat in stats.json is [value, text, low, periods]; low=1 means a time (lowest wins). Periods are
[kind, number, value, text] with kind 1 = a day, 2 = a week.

Checks (thresholds are constants below; they are heuristics, not proof of cheating):
  negative       a negative score (Canyon Duel legs can end below zero in the game itself)
  impossible     value not finite, <= 0, a time under 15 s, or a score over 1e9 (stats.rs plausible())
  far-ahead      a time faster than TIME_AHEAD x the median of the rest of the board's top 20, or a
                 score above SCORE_AHEAD x that median (Canyon Duel: CD_AHEAD, its scores vary widely)
  gap-to-next    the #1 is faster than GAP_TIME x the #2 time (or above GAP_SCORE x the #2 score)
  lap-vs-race    a race time shorter than laps x the same player's best lap on that circuit
  top-speed      a race-host result whose text carries a top speed over TOP_SPEED_MAX m/s
  period-better  a period value better than the all-time value (the server never keeps that)
  same-browser   two or more names in one board's top 20 whose latest browser key is the same
  same-value     the same time to 1/10000 s held by two or more names on one board
Each finding also says where the board value most likely came from: "race host" when the player has
Total_Rnkd_Evt_Games_<race> (the server counted at least one 2+ player race of that event), else
"game only" (the player's own game sent it with UpdateStats, nobody else saw it).
"""
import argparse
import hashlib
import json
import math
import re
import statistics
import sys
from collections import defaultdict

FASTEST_RACE = 15.0
SCORE_MAX = 1e9
TIME_AHEAD = 0.80      # 20 % faster than the board's typical top-20 time
SCORE_AHEAD = 3.0
CD_AHEAD = 6.0
GAP_TIME = 0.90        # 10 % ahead of #2
GAP_SCORE = 2.0
TOP_SPEED_MAX = 115.0  # m/s (414 km/h); the fastest stock cars top out near 100 m/s
MIN_BOARD = 5          # boards with fewer plausible entries are not compared

# Boards the public page shows (logs/leaderboards-preview/build-data.py), and their direction.
FAMILIES = {"Evt_Bst": True, "Evt_Bst_Lap": True, "CD_Best_Score": False, "PK_Best_Cop_Score": False}
SERVER_COUNTED = {"Best_Online_Win_Streak", "Overall_Solo_On_Wins"}
GLOBAL = {"Best_Drift_Combo": False, "Best_Drift_Lap": False, "Best_Online_Win_Streak": False,
          "Overall_Solo_On_Wins": False, "PT_Most_Busts": False}


def tag(name, show):
    return name if show else "#" + hashlib.sha256(name.lower().encode()).hexdigest()[:6]


def plausible(v, low):
    return isinstance(v, (int, float)) and math.isfinite(v) and 0 < v <= SCORE_MAX and (not low or v >= FASTEST_RACE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stats")
    ap.add_argument("--races", help="race-names.json (race id -> {hash, type, laps, ...})")
    ap.add_argument("--show-names", action="store_true")
    ap.add_argument("--json", help="also write the findings as JSON here")
    a = ap.parse_args()

    people = json.load(open(a.stats))["people"]
    info = {}
    if a.races:
        for rid, r in json.load(open(a.races)).items():
            info[r["hash"]] = {**r, "id": rid}

    boards = defaultdict(list)          # board id -> [(name, value, text, all stat)]
    lows = {}
    counted = set()                     # (name, race hash) the race host counted
    laps_best = {}                      # (name, race hash) -> best lap
    findings = []
    sources = defaultdict(int)
    browser = {}                        # name -> its latest browser-key fingerprint (stats.rs)

    def find(kind, board, name, value, why, source=""):
        findings.append({"check": kind, "board": board, "player": tag(name, a.show_names), "value": value,
                         "why": why, "source": source})

    for key_name, p in people.items():
        name = p.get("name", key_name)
        for key, s in p["stats"].items():
            m = re.match(r"(.*)_([0-9A-F]{8})$", key)
            if m and m.group(1) == "Total_Rnkd_Evt_Games":
                counted.add((name.lower(), m.group(2)))
            if m and m.group(1) == "Evt_Bst_Lap":
                laps_best[(name.lower(), m.group(2))] = s[0]

    for key_name, p in people.items():
        name = p.get("name", key_name)
        for key, s in p["stats"].items():
            m = re.match(r"(.*)_([0-9A-F]{8})$", key)
            fam, race = (m.group(1), m.group(2)) if m and m.group(1) in FAMILIES else (key, None)
            if race is None and fam not in GLOBAL:
                continue
            low = bool(s[2]) if len(s) > 2 else (FAMILIES.get(fam) if race else GLOBAL[fam])
            bid = f"{fam}:{race}" if race else fam
            if race and info.get(race):
                bid += f" ({info[race]['id']}, {info[race].get('type')})"
            lows[bid] = low
            v, text = s[0], s[1] if len(s) > 1 else ""
            # Only Evt_Bst is ever written by the race host; every other board key comes from UpdateStats.
            if fam == "Evt_Bst":
                src = "race host" if (name.lower(), race) in counted else "game only"
            else:
                src = "server count" if fam in SERVER_COUNTED else "game only"
            sources[src] += 1
            if not plausible(v, low):
                if v < 0 and not low:
                    find("negative", bid, name, v, "a negative score (the game's own; build-data.py leaves it off the page)", src)
                elif v != 0:
                    find("impossible", bid, name, v, "not a possible value (kept, but off the boards)", src)
                continue
            boards[bid].append((name, v, text, src))
            if p.get("key"):
                browser[name] = p["key"]
            for per in (s[3] if len(s) > 3 else []):
                pv = per[2]
                if plausible(pv, low) and ((low and pv < v - 1e-3) or (not low and pv > v + 1e-3)):
                    find("period-better", bid, name, v, f"period {per[0]}/{per[1]} has {pv}, better than all time", src)
            if fam == "Evt_Bst" and race and low:
                laps = (info.get(race) or {}).get("laps") or 0
                lap = laps_best.get((name.lower(), race))
                if laps > 1 and lap and plausible(lap, True) and v < laps * lap * 0.99:
                    find("lap-vs-race", bid, name, v, f"{laps} laps x best lap {lap:.3f} = {laps * lap:.3f} > race time", src)
            parts = text.split(",") if isinstance(text, str) else []
            if fam == "Evt_Bst" and len(parts) == 4 and parts[0].startswith("0x"):
                try:
                    nums = [float(x) for x in parts[1:3]]
                except ValueError:
                    nums = []
                kind = ((info.get(race) or {}).get("race_type") or "")
                # circuit: "car,best lap,top speed,0"; sprint: "car,top speed,NOS,0" (stats.rs); others differ
                top = (nums[1] if kind == "circuit" else nums[0]) if len(nums) == 2 and kind in ("circuit", "p2p") else 0
                if top > TOP_SPEED_MAX:
                    find("top-speed", bid, name, v, f"top speed {top:.1f} m/s in its text", src)

    for bid, rows in boards.items():
        low = lows[bid]
        rows.sort(key=lambda r: r[1], reverse=not low)
        if len(rows) < MIN_BOARD:
            continue
        top = rows[:20]
        canyon = bid.startswith("CD_")
        circuit = "Circuit" in bid or "Race Wars" in bid
        note = "; online circuits mix lap counts" if circuit else ""
        for i, (name, v, _, src) in enumerate(top[:5]):
            rest = [r[1] for j, r in enumerate(top) if j != i]
            med = statistics.median(rest)
            if low and v < med * TIME_AHEAD:
                find("far-ahead", bid, name, v, f"rank {i + 1}: {v:.3f} s vs top-20 median {med:.3f} s ({v / med:.0%}){note}", src)
            if not low and v > med * (CD_AHEAD if canyon else SCORE_AHEAD):
                find("far-ahead", bid, name, v, f"rank {i + 1}: {v:,.0f} vs top-20 median {med:,.0f} (x{v / med:.1f})", src)
        v1, v2 = top[0][1], top[1][1]
        if (low and v1 < v2 * GAP_TIME) or (not low and not canyon and v1 > v2 * GAP_SCORE):
            find("gap-to-next", bid, top[0][0], v1, f"#1 {v1:,.3f} vs #2 {v2:,.3f}{note}", top[0][3])
        shared = defaultdict(list)
        for i, (name, v, _, src) in enumerate(top):
            if name in browser:
                shared[browser[name]].append((i + 1, name, v))
        for rows_ in shared.values():
            if len(rows_) > 1:
                find("same-browser", bid, rows_[0][1], rows_[0][2], "one browser holds "
                     + ", ".join(f"rank {r} ({tag(n, a.show_names)} {v:,.3f})" for r, n, v in rows_) + " of the top 20", "")
        if low:
            same = defaultdict(list)
            for name, v, _, src in rows:
                same[round(v, 4)].append(name)
            for v, names in same.items():
                if len(names) > 1:
                    find("same-value", bid, names[0], v, f"{len(names)} names hold exactly {v}: "
                         + ", ".join(tag(n, a.show_names) for n in names[:6]), "")

    by_check = defaultdict(int)
    for f in findings:
        by_check[f["check"]] += 1
    print(f"{len(people)} players, {len(boards)} boards with a plausible value")
    print("board values by source: " + ", ".join(f"{k} {v}" for k, v in sorted(sources.items())))
    print("findings: " + (", ".join(f"{k} {v}" for k, v in sorted(by_check.items())) or "none"))
    for f in sorted(findings, key=lambda f: (f["check"], f["board"])):
        print(f"  [{f['check']}] {f['board']} {f['player']} {f['value']}: {f['why']}" + (f" [{f['source']}]" if f["source"] else ""))
    if a.json:
        json.dump(findings, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
