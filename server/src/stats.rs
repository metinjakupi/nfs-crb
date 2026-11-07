//! Player Stats and the Leaderboard: FESL's "rank" service as NFS Carbon's own screens use it
//! (Statistics in the main menu: LEADERBOARD and ONLINE PLAYER STATS; plans/lan-protocol.md,
//! "Player Stats and the Leaderboard"). ea.rs answers the rank transactions from here.
//!
//! What the screens ask (seen in the game):
//! - ONLINE PLAYER STATS: GetStats {owner, ownerType=1, periodId=0, keys} for Online_Rep_Level,
//!   Overall_Solo/Crew_On_Wins/Losses, Best_Online_Win_Streak, Challenge_Series_Beat_Online, DNF,
//!   Total_Online_Games_Started/Finished.
//! - LEADERBOARD (per race, race type and time period): GetRankedStats {owner, keys} for
//!   Evt_Bst_<race> and Total_Rnkd_Evt_Games_<race>, then the rows from the player's rank and
//!   the leader (GetTopN {key, minRank, maxRank}), or the player and its Messenger friends
//!   (GetRankedStatsForOwners). Enter on a row opens that player's Player Stats.
//!   <race> is the Attrib key of the race id (lookup2 of "mu.1.1" = 30F07253, attrib_key).
//!
//! Where the numbers come from:
//! - The game itself sends UpdateStats {u.N.o owner, u.N.ot, u.N.s.M.k/v/t/ut}. At sign-in it
//!   uploads its whole career (~280 keys: cars, cash, reward cards). No screen reads those back, so
//!   only the keys the online screens can ask for are kept (kept_from_game). "ut", the update
//!   type, as the game uses it (00818c50, 008222a0): 0 set, 1 keep the highest, 2 keep the
//!   lowest (a time), 3 add.
//! - The game never sends the Player Stats keys, and in our unranked rooms it sends no race
//!   results at all: EA's servers counted them from what EA's race host reported. So our race
//!   host reports each Custom Match race (host.rs RaceReport: who started it, then every game's
//!   GameResults) and this module counts it the same way: games started and finished, wins and
//!   losses, win streaks, wins per game mode, the share of races left unfinished (DNF), and per
//!   race the best result (Evt_Bst_<race>, circuits and sprints), the games
//!   (Total_Rnkd_Evt_Games_<race>) and the wins (Evt_Wins_<race>). A race needs two players to
//!   count; leaving a race, or not finishing a sprint, circuit or speed trap, is a loss and a DNF.
//!
//! Players are kept by their display name (ea.rs's Login), as Messenger keeps friends. An
//! explicit name claim adds an access check before Login; it does not rewrite or clear these
//! statistics. Changing browsers or losing browser storage does not clear statistics. Kept with CARBON_STATS_DIR in
//! `stats.json` (written whole through a temporary file); in memory only without it. Per player:
//! its name, a short fingerprint of the latest browser key when supplied, the last sign-in day,
//! and its stats. No addresses, nothing else.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::PathBuf;

use serde::{Serialize, Serializer};
use serde_json::{json, Value};

use crate::ea::{user_id, Packet};
use crate::leaderboard::{Kind, Limits, Pending, Review};
use crate::messenger::KeyDigest;

/// Players kept at most; past that a new name's stats are counted for this run only.
pub const MAX_PEOPLE: usize = 10_000;
/// Stats kept per player.
const MAX_KEYS: usize = 256;
/// A stat's text (the leaderboard's car, best lap and top speed) at most.
const MAX_TEXT: usize = 64;
/// Rows a leaderboard request may ask for at once.
const MAX_ROWS: usize = 100;

/// The fastest a timed result can be, in seconds; no race in the game ends sooner. The numbers
/// come from the players' own games, so this and SCORE_MAX only stop the impossible: a believable
/// fake still passes.
const FASTEST_RACE: f64 = 15.0;
const SCORE_MAX: f64 = 1e9;

/// Whether a value can be real. `low` is a time, where smaller is better.
fn plausible(v: f64, low: bool) -> bool {
    v.is_finite() && v.abs() <= SCORE_MAX && (!low || v >= FASTEST_RACE)
}

/// The keys only the race host writes, for the public boards (leaderboard.rs): Srv_Bst_<race>
/// (a sprint's best time), Srv_Bst_<race>_L<laps> (a circuit's, per lap count: online circuits
/// are raced with 1 lap or more), Srv_Lap_<race> (a circuit's best lap). UpdateStats can never
/// set them (server_counted).
pub const SRV: &str = "Srv_";
/// The public wins, the current and the best win streak, counted from the leaderboard reset
/// (CARBON_LEADERBOARD_RESET) on; each value's text is the reset it was counted under.
const PUBLIC_WINS: &str = "Srv_Wins";
const PUBLIC_STREAK: &str = "Srv_Streak";
const PUBLIC_BEST_STREAK: &str = "Srv_Best_Streak";

/// Two games' rows for one racer agree when they finished alike, in the same place, and their
/// times are within this much (seen: 315.70 s and 315.73 s for one racer, run r1). A best time
/// is the slowest of the agreeing reports, so one game cannot make it faster.
const TIME_TOLERANCE: f64 = 0.25;
const TIME_TOLERANCE_SHARE: f64 = 0.0025;
/// The top speed a GameResults row may have, in m/s (432 km/h; the fastest kept on the live
/// boards is 104 m/s).
const TOP_SPEED_MAX: f32 = 120.0;
/// A race time cannot be longer than the time from the race host's StartRaceSyncBegin to a
/// game's GameResults that has it, plus this (seen: 327 s from the start to the report of a
/// 315.70 s and a 320.95 s finish, run r1). A game whose clock runs fast fails this.
const CLOCK_SLACK: f64 = 2.0;
/// The review floor (leaderboard.rs): a board with this many other entries has an established
/// top; a result faster than its best by more than FLOOR_TIME_MARGIN, or a score more than
/// FLOOR_SCORE_FACTOR (Canyon Duel: FLOOR_CANYON_FACTOR) times its best, waits for an admin.
const FLOOR_ENTRIES: usize = 5;
const FLOOR_TIME_MARGIN: f64 = 0.05;
const FLOOR_SCORE_FACTOR: f64 = 1.5;
const FLOOR_CANYON_FACTOR: f64 = 3.0;
/// Board keys one UpdateStats may set (each is compared with every player's).
const BOARD_KEYS_PER_UPDATE: usize = 64;

/// What became of a board result (Stats::record).
#[derive(Clone, Debug, PartialEq)]
pub enum Recorded {
    Kept,
    /// waits for an admin, and why
    Held(String),
    Dropped,
}

/// The number of the week `secs` falls in (weeks from Monday, UTC), as the boards count them.
pub fn week_number(secs: u64) -> u32 {
    Period::Week.number(secs)
}

/// Board keys: a best time or score that a leaderboard ranks, held for review when far ahead.
fn board_key(key: &str) -> bool {
    ["Evt_Bst_", "CD_Best_Score_", "PK_Best_Cop_Score_", SRV].iter().any(|p| key.starts_with(p))
        || matches!(key, "Best_Drift_Combo" | "Best_Drift_Lap" | "PT_Most_Busts")
}

/// A public board a stat key belongs to.
struct PublicSpec {
    id: String,
    family: &'static str,
    source: &'static str,
    race: Option<String>,
    laps: Option<u32>,
    /// a count (wins, a streak, busts) rather than points
    count: bool,
}

fn race_hash(s: &str) -> Option<String> {
    (s.len() == 8 && s.bytes().all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b))).then(|| s.to_string())
}

/// The public board of `key`, if it has one: the race host's boards always, the boards of what
/// the games send themselves only with `game`.
fn public_spec(key: &str, game: bool) -> Option<PublicSpec> {
    let spec = |id: String, family, source, race, laps, count| Some(PublicSpec { id, family, source, race, laps, count });
    if let Some(rest) = key.strip_prefix("Srv_Bst_") {
        let (h, laps) = match rest.split_once("_L") {
            Some((h, n)) => (h, Some(n.parse::<u32>().ok().filter(|n| (1..=99).contains(n))?)),
            None => (rest, None),
        };
        let h = race_hash(h)?;
        let id = laps.map_or(format!("Evt_Bst:{h}"), |n| format!("Evt_Bst:{h}:{n}"));
        return spec(id, "Evt_Bst", "race", Some(h), laps, false);
    }
    if let Some(h) = key.strip_prefix("Srv_Lap_").and_then(race_hash) {
        return spec(format!("Evt_Bst_Lap:{h}"), "Evt_Bst_Lap", "race", Some(h), None, false);
    }
    // The public wins and best streak: counted since the reset (Stats::public_count), shown
    // under the families the page knows. The game's own Overall_Solo_On_Wins and
    // Best_Online_Win_Streak (its Player Stats screens) are never public: they were counted
    // before the games' reports were cross-checked.
    match key {
        PUBLIC_WINS => return spec("Overall_Solo_On_Wins".into(), "Overall_Solo_On_Wins", "race", None, None, true),
        PUBLIC_BEST_STREAK => return spec("Best_Online_Win_Streak".into(), "Best_Online_Win_Streak", "race", None, None, true),
        _ => {}
    }
    if !game {
        return None;
    }
    for family in ["Evt_Bst_Lap", "Evt_Bst", "CD_Best_Score", "PK_Best_Cop_Score"] {
        if let Some(h) = key.strip_prefix(family).and_then(|r| r.strip_prefix('_')).and_then(race_hash) {
            return spec(format!("game:{family}:{h}"), family, "game", Some(h), None, false);
        }
    }
    match key {
        "Best_Drift_Combo" => spec("game:Best_Drift_Combo".into(), "Best_Drift_Combo", "game", None, None, false),
        "Best_Drift_Lap" => spec("game:Best_Drift_Lap".into(), "Best_Drift_Lap", "game", None, None, false),
        "PT_Most_Busts" => spec("game:PT_Most_Busts".into(), "PT_Most_Busts", "game", None, None, true),
        _ => None,
    }
}

/// The in-game group of the public answer (leaderboard.rs, source "ingame"): the keys the game's
/// own Statistics screens rank (plans/lan-protocol.md "Player Stats and the Leaderboard"): per
/// race the best (Evt_Bst_<race>, one per race whatever the lap count, as the game keeps it), the
/// best lap (Evt_Bst_Lap_<race>), the races and the wins (Total_Rnkd_Evt_Games_<race>,
/// Evt_Wins_<race>), and the game's own online wins and best win streak. Canyon Duel, drift,
/// pursuit and the other game-only keys are not in it (they stay behind CARBON_LEADERBOARD_GAME).
/// Returns (family, race hash, a count).
fn ingame_spec(key: &str) -> Option<(&'static str, Option<String>, bool)> {
    for (family, count) in [("Evt_Bst_Lap", false), ("Total_Rnkd_Evt_Games", true), ("Evt_Wins", true), ("Evt_Bst", false)] {
        if let Some(h) = key.strip_prefix(family).and_then(|r| r.strip_prefix('_')).and_then(race_hash) {
            return Some((family, Some(h), count));
        }
    }
    match key {
        "Overall_Solo_On_Wins" => Some(("Overall_Solo_On_Wins", None, true)),
        "Best_Online_Win_Streak" => Some(("Best_Online_Win_Streak", None, true)),
        _ => None,
    }
}

/// The two numbers of a best time's text ("0x<car>,a,b,0x0", 008222f0) that the game's board
/// shows by race type: a circuit's best lap and top speed, a sprint's top speed and NOS.
fn numbers_of(text: &str) -> Option<(f64, f64)> {
    let mut it = text.split(',').skip(1);
    let a = it.next()?.parse::<f64>().ok().filter(|v| v.is_finite())?;
    let b = it.next()?.parse::<f64>().ok().filter(|v| v.is_finite())?;
    Some((a, b))
}

/// The car's Attrib key from a stat's text ("0x8774322C,..."), lowercase, or "".
fn car_of(text: &str) -> String {
    let car = text.split(',').next().unwrap_or("");
    if car.len() == 10 && car.starts_with("0x") && car[2..].bytes().all(|b| b.is_ascii_hexdigit()) && car != "0x00000000" {
        car.to_ascii_lowercase()
    } else {
        String::new()
    }
}

/// What the games said about one racer, checked against each other (cross_check).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Checked {
    /// the row the games agree on (with the slowest of the agreeing times), if they agree
    pub row: Option<Row>,
    /// the slots of the games whose report has that row
    pub from: Vec<u32>,
    /// another player's game is among them
    pub witnessed: bool,
    /// the slots of the games whose row differs from it (all of them when there is no majority)
    pub against: Vec<u32>,
    /// no row has a majority
    pub disputed: bool,
}

fn close(a: f32, b: f32) -> bool {
    let (a, b) = (f64::from(a), f64::from(b));
    (a - b).abs() <= TIME_TOLERANCE.max(a.abs().max(b.abs()) * TIME_TOLERANCE_SHARE)
}

/// Two games' rows for one racer say the same: both finished or both not, and when both
/// finished, the same place and (`times`: a sprint or circuit) the same time and best lap.
fn agree(a: &Row, b: &Row, times: bool) -> bool {
    a.finished == b.finished
        && (!a.finished || a.place == b.place)
        && (!a.finished || !times || (close(a.time, b.time) && (a.best_lap <= 0.0 || b.best_lap <= 0.0 || close(a.best_lap, b.best_lap))))
}

/// The racer in `slot` as every game's report has it (racer id 1 + 4 x slot), checked: a row
/// most reports agree with, or none (disputed) when there is no majority. A game alone in
/// agreeing with itself is not a witness.
pub fn cross_check(slot: u32, reports: &[(u32, Vec<Row>)], times: bool) -> Checked {
    let id = 1 + 4 * slot;
    let rows: Vec<(u32, Row)> = reports.iter().filter_map(|(s, rows)| rows.iter().find(|r| r.racer == id).map(|r| (*s, *r))).collect();
    if rows.is_empty() {
        return Checked::default();
    }
    let support = |r: &Row| rows.iter().filter(|(_, x)| agree(r, x, times)).count();
    // the best supported row; on a tie, another game's before the racer's own
    let pick = rows.iter().max_by_key(|(s, r)| (support(r), *s != slot)).map(|(_, r)| *r).expect("rows");
    let n = support(&pick);
    if n * 2 <= rows.len() && n != rows.len() {
        return Checked { against: rows.iter().map(|(s, _)| *s).collect(), disputed: true, ..Checked::default() };
    }
    let from: Vec<u32> = rows.iter().filter(|(_, x)| agree(&pick, x, times)).map(|(s, _)| *s).collect();
    let against = rows.iter().filter(|(_, x)| !agree(&pick, x, times)).map(|(s, _)| *s).collect();
    let row = rows.iter().filter(|(_, x)| agree(&pick, x, times)).map(|(_, x)| *x).max_by(|a, b| a.time.total_cmp(&b.time));
    Checked { row, witnessed: from.iter().any(|s| *s != slot), from, against, disputed: false }
}

/// The racer's row in its own game's report.
fn own_row(slot: u32, reports: &[(u32, Vec<Row>)]) -> Option<Row> {
    let id = 1 + 4 * slot;
    reports.iter().find(|(s, _)| *s == slot).and_then(|(_, rows)| rows.iter().find(|r| r.racer == id).copied())
}

/// Whether a racer's agreed time can go on the boards: confirmed by another player's game, from
/// a game that did not misreport anyone, possible, and not longer than the race host saw pass.
fn check_time(r: &Racer, c: &Checked, suspects: &[u32], w: &Row, mode: u32, laps: Option<u32>, clock: &[(u32, f64)]) -> Result<(), String> {
    if c.disputed {
        return Err("the games disagree".into());
    }
    if suspects.contains(&r.slot) {
        return Err("its game reported a racer unlike the other games".into());
    }
    if !c.witnessed {
        return Err("no other player's game reported it".into());
    }
    let t = f64::from(w.time);
    if !plausible(t, true) {
        return Err(format!("{t:.2} s is not a possible race time"));
    }
    if w.top_speed > TOP_SPEED_MAX || !w.top_speed.is_finite() {
        return Err(format!("a top speed of {:.1} m/s", w.top_speed));
    }
    let lap = f64::from(w.best_lap);
    if lap > t + TIME_TOLERANCE {
        return Err(format!("a best lap of {lap:.2} s in a {t:.2} s race"));
    }
    if let (1, Some(n)) = (mode, laps.filter(|n| *n > 1)) {
        if lap > 0.0 && t < f64::from(n) * lap * 0.99 {
            return Err(format!("{t:.2} s is less than {n} laps of its best lap, {lap:.2} s"));
        }
    }
    for (s, secs) in clock.iter().filter(|(s, _)| c.from.contains(s)) {
        if t > secs + CLOCK_SLACK {
            return Err(format!("{t:.2} s, but slot {s}'s report came {secs:.2} s after the start"));
        }
    }
    Ok(())
}

/// An update type ("ut"), as the game sends it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Update {
    Set,
    Max,
    Min,
    Add,
}

impl Update {
    pub fn from_ut(ut: i64) -> Update {
        match ut {
            1 => Update::Max,
            2 => Update::Min,
            3 => Update::Add,
            _ => Update::Set,
        }
    }
}

/// The leaderboard's Time Period: the game asks with periodId 2 (Weekly, the default), 0 (All
/// Time, and Player Stats) or 1 (Daily), and periodPast (0 the current period, 1 the one
/// before).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Period {
    All,
    Day,
    Week,
}

impl Period {
    pub fn from_id(id: i64) -> Period {
        match id {
            1 => Period::Day,
            2 => Period::Week,
            _ => Period::All,
        }
    }

    const KINDS: [Period; 2] = [Period::Day, Period::Week];

    fn code(self) -> u8 {
        match self {
            Period::All => 0,
            Period::Day => 1,
            Period::Week => 2,
        }
    }

    fn from_code(c: u64) -> Option<Period> {
        Period::KINDS.into_iter().find(|p| u64::from(p.code()) == c)
    }

    /// The number of the period `secs` (seconds since 1970, UTC) falls in: days, and weeks
    /// from Monday.
    fn number(self, secs: u64) -> u32 {
        let days = secs / 86_400;
        match self {
            Period::All => 0,
            Period::Day => days as u32,
            // 1970-01-01 was a Thursday: +3 starts weeks on Mondays
            Period::Week => ((days + 3) / 7) as u32,
        }
    }
}

/// Keys whose leaderboards have time periods; every other stat is all time only.
fn has_periods(key: &str) -> bool {
    ["Evt_", "Total_Rnkd_Evt_Games_", "Canyon_", "PK_Best_", "CD_", SRV].iter().any(|p| key.starts_with(p))
}

/// What the game sends that is kept: the keys an online screen can ask for, except the ones
/// this server counts itself from the race host's reports (server_counted).
pub fn kept_from_game(key: &str) -> bool {
    const PREFIXES: [&str; 8] = ["Evt_Bst_", "Canyon_", "PK_", "PT_", "CD_", "Best_Drift_", "Virus", "Online_Rep_Level"];
    const KEYS: [&str; 3] = ["Challenge_Series_Beat_Online", "Beat_Moderator", "Skill_Level"];
    key.len() <= 64 && !server_counted(key) && (PREFIXES.iter().any(|p| key.starts_with(p)) || KEYS.contains(&key))
}

/// The keys this server counts from race reports (the game never sends them).
fn server_counted(key: &str) -> bool {
    key.starts_with(SRV)
        || key.starts_with("Overall_")
        || key.starts_with("Total_Online_Games")
        || key.starts_with("Evt_Wins_")
        || key.starts_with("Total_Rnkd_Evt_Games_")
        || key.ends_with("_On_Wins")
        || key.ends_with("_Online_Win_Streak")
        || key == "DNF"
}

/// Text from the game for a stat: no quotes or line breaks (the framing would end the value),
/// and short.
fn clean_text(s: &str) -> String {
    s.chars().filter(|c| !matches!(c, '"' | '\n' | '\r' | '\0')).take(MAX_TEXT).collect()
}

/// A value as the answers write it: at most 4 decimals ("0.0" for 0), as EA's "%.4f".
pub fn num(v: f64) -> String {
    let r = (v * 10_000.0).round() / 10_000.0;
    if r == r.trunc() {
        format!("{r:.1}")
    } else {
        let s = format!("{r:.4}");
        s.trim_end_matches('0').to_string()
    }
}

/// One stat: all time, and for leaderboard keys the current and the past day and week.
#[derive(Clone, Debug, PartialEq)]
struct Stat {
    v: f64,
    t: String,
    /// lower is better (a time): ranked lowest first
    low: bool,
    /// (period, its number, value, text): at most the current one and the one before per kind
    p: Vec<(Period, u32, f64, String)>,
    /// when the all-time value last changed for the better, and when a period's value last
    /// did (seconds since 1970; 0 for stats kept before these were)
    at: u64,
    pat: u64,
}

fn apply(old: Option<f64>, how: Update, v: f64) -> (f64, bool) {
    match (old, how) {
        (None, _) => (v, true),
        (Some(_), Update::Set) => (v, true),
        (Some(o), Update::Add) => (o + v, true),
        (Some(o), Update::Max) => if v > o { (v, true) } else { (o, false) },
        (Some(o), Update::Min) => if v < o { (v, true) } else { (o, false) },
    }
}

impl Stat {
    fn new(low: bool) -> Stat {
        Stat { v: 0.0, t: String::new(), low, p: Vec::new(), at: 0, pat: 0 }
    }

    /// Applies an update at `now` (seconds); for leaderboard keys the periods too.
    fn update(&mut self, key: &str, how: Update, v: f64, t: &str, fresh: bool, now: u64) {
        let (nv, better) = apply((!fresh).then_some(self.v), how, v);
        self.v = nv;
        if better {
            self.at = now;
        }
        if better && (how != Update::Add || !t.is_empty()) {
            self.t = t.to_string();
        }
        if !has_periods(key) {
            return;
        }
        for kind in Period::KINDS {
            let n = kind.number(now);
            self.p.retain(|(k, m, _, _)| *k != kind || *m + 1 >= n);
            match self.p.iter_mut().find(|(k, m, _, _)| *k == kind && *m == n) {
                Some(e) => {
                    let (nv, better) = apply(Some(e.2), how, v);
                    e.2 = nv;
                    if better {
                        self.pat = now;
                    }
                    if better && (how != Update::Add || !t.is_empty()) {
                        e.3 = t.to_string();
                    }
                }
                None => {
                    self.p.push((kind, n, v, t.to_string()));
                    self.pat = now;
                }
            }
        }
    }

    /// The value and text for a period (`past` periods back), if it has one.
    fn at(&self, period: Period, past: u32, now: u64) -> Option<(f64, &str)> {
        if period == Period::All {
            return Some((self.v, &self.t));
        }
        let n = period.number(now).checked_sub(past)?;
        self.p.iter().find(|(k, m, _, _)| *k == period && *m == n).map(|(_, _, v, t)| (*v, t.as_str()))
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
struct Person {
    name: String,
    /// the first 16 hex digits of the latest browser key's SHA-256, when supplied
    key: Option<String>,
    /// last sign-in, in days since 1970
    seen: u64,
    stats: BTreeMap<String, Stat>,
}

/// A racer in a race report: its place in the room (its racer ids are 1 + 4 x slot, as the
/// race host hands them out in GameInitInfo), its name and its car (vehicle_key, 0 unknown).
/// trace.rs adds who it is beyond its name before the report is counted: the client address its
/// browser came from, and the claim owner its browser proved when the name is claimed by it.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct Racer {
    pub slot: u32,
    pub name: String,
    pub car: u32,
    /// its browser's address on the virtual network
    pub vip: u32,
    /// the address the browser connected from (X-Real-IP), "" when not known
    pub client: String,
    /// Some when the name is explicitly claimed (identity.rs) and this browser proved the claim
    pub owner: Option<String>,
}

/// The car a PlayerCarData describes: its vehicle's Attrib key (attrib_key("r8"), as the
/// leaderboard wants it), little-endian after the player's name. Seen (runs c2, k1):
/// [type 4][racer id][join order][0][-1][table hash] [u16 name length][name][0][4][key LE]...
pub fn vehicle_key(car_data: Option<&[u8]>) -> u32 {
    let Some(m) = car_data else { return 0 };
    let Some(len) = m.get(24..26).map(|b| usize::from(u16::from_be_bytes([b[0], b[1]]))) else { return 0 };
    let at = 26 + len + 2;
    m.get(at..at + 4).map_or(0, |k| u32::from_le_bytes([k[0], k[1], k[2], k[3]]))
}

/// What the race host tells about a race (host.rs).
#[derive(Clone, Debug)]
pub enum RaceReport {
    /// StartRaceSyncBegin went out to these players.
    Started { gid: u32, players: Vec<Racer> },
    /// FinalGameResults went out: the room's settings (game_mode, race_type_*), the players
    /// still in the game, the GameResults each of their games sent (by slot), and per slot the
    /// seconds from StartRaceSyncBegin to its GameResults (on the race host's clock; empty
    /// when the start was not seen).
    Ended { gid: u32, settings: Vec<(&'static str, String)>, players: Vec<Racer>, results: Vec<(u32, Vec<u8>)>, clock: Vec<(u32, f64)> },
}

/// One racer's row of a GameResults, as the game writes it (007d4320) and reads it (007d3eb0):
/// 3 bytes (how its race ended: 0 normally, 1-3 from the racer's end state 4-6; its place from
/// 1; a flag), then 11 32-bit values: a count, the racer id, the best lap (f32 s, 0 before a
/// lap is done), the race time (f32 s, as STANDINGS shows it), 0, the average and the top
/// speed (f32 m/s), then mode values. Seen in a 2-lap circuit (run c2): 103.05 s best lap,
/// 217.12 s ("3:37.12" on STANDINGS), 50.5 m/s average, 72.9 m/s top.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Row {
    pub racer: u32,
    pub place: u8,
    pub finished: bool,
    pub time: f32,
    pub best_lap: f32,
    pub top_speed: f32,
}

/// The rows of a GameResults: [type u32][racers u8][race kind u8][u16][u8][u8][sender u32],
/// then 8 rows of 3 + 11 x 4 bytes.
pub fn game_results_rows(m: &[u8]) -> Vec<Row> {
    const HEAD: usize = 14;
    const ROW: usize = 3 + 11 * 4;
    if m.len() < HEAD || m[..4] != crate::olmsg::GAME_RESULTS.to_be_bytes() {
        return Vec::new();
    }
    let n = usize::from(m[4]).min(8);
    let u = |at: usize| u32::from_be_bytes([m[at], m[at + 1], m[at + 2], m[at + 3]]);
    (0..n)
        .filter_map(|i| {
            let at = HEAD + i * ROW;
            if at + ROW > m.len() {
                return None;
            }
            let w = |k: usize| u(at + 3 + 4 * k);
            Some(Row { racer: w(1), place: m[at + 1], finished: m[at] == 0, best_lap: f32::from_bits(w(2)), time: f32::from_bits(w(3)), top_speed: f32::from_bits(w(6)) })
        })
        .filter(|r| r.racer != 0)
        .collect()
}

/// Attrib's key for a name (the race ids "mu.1.1"...): Bob Jenkins' lookup2 with the initial
/// value 0xABCDEF00. The leaderboard's keys end in it: Evt_Bst_30F07253 is mu.1.1's.
pub fn attrib_key(s: &str) -> u32 {
    fn mix(mut a: u32, mut b: u32, mut c: u32) -> (u32, u32, u32) {
        a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 13);
        b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 8);
        c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 13);
        a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 12);
        b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 16);
        c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 5);
        a = a.wrapping_sub(b).wrapping_sub(c) ^ (c >> 3);
        b = b.wrapping_sub(c).wrapping_sub(a) ^ (a << 10);
        c = c.wrapping_sub(a).wrapping_sub(b) ^ (b >> 15);
        (a, b, c)
    }
    let k = s.as_bytes();
    let word = |w: &[u8]| w.iter().enumerate().fold(0u32, |acc, (i, &x)| acc.wrapping_add(u32::from(x) << (8 * i)));
    let (mut a, mut b, mut c) = (0x9e37_79b9u32, 0x9e37_79b9u32, 0xabcd_ef00u32);
    let mut rest = k;
    while rest.len() >= 12 {
        a = a.wrapping_add(word(&rest[0..4]));
        b = b.wrapping_add(word(&rest[4..8]));
        c = c.wrapping_add(word(&rest[8..12]));
        (a, b, c) = mix(a, b, c);
        rest = &rest[12..];
    }
    c = c.wrapping_add(k.len() as u32);
    a = a.wrapping_add(word(&rest[..rest.len().min(4)]));
    if rest.len() > 4 {
        b = b.wrapping_add(word(&rest[4..rest.len().min(8)]));
    }
    if rest.len() > 8 {
        // the last word's first byte is the length (added above), the rest shifted up one
        c = c.wrapping_add(word(&rest[8..]) << 8);
    }
    mix(a, b, c).2
}

/// Each game mode's wins key (the online reward cards and the race-end GetStats ask them).
fn mode_wins(mode: u32) -> Option<&'static str> {
    Some(match mode {
        0 => "Sprint_On_Wins",
        1 => "Circuit_On_Wins",
        5 => "SpeedTrap_On_Wins",
        13 => "Canyon_On_Wins",
        14 => "PursuitTag_On_Wins",
        15 => "PursuitKO_On_Wins",
        _ => return None,
    })
}

/// A leaderboard row.
#[derive(Clone, Debug, PartialEq)]
pub struct Ranked {
    pub owner: u32,
    pub name: String,
    pub rank: usize,
    pub value: f64,
    pub text: String,
}

/// Everything kept.
#[derive(Default)]
pub struct Stats {
    people: BTreeMap<String, Person>,
    /// user id (ea::user_id of the name) -> the key in `people`
    by_uid: HashMap<u32, String>,
    /// races under way, by game id: the players at the start
    racing: HashMap<u32, Vec<Racer>>,
    file: Option<PathBuf>,
    /// results waiting for an admin, names banned from the boards, the admin's log
    /// (leaderboard.rs), kept in leaderboard.json beside stats.json
    pub review: Review,
    /// results recorded for the public boards per name (leaderboard.rs)
    limits: Limits,
    /// the public wins and streaks count from this moment (seconds since 1970; 0: from the
    /// first race this server counts them)
    reset: u64,
}

// Borrow the live store while encoding its existing version-1 schema. Building a
// second tree of Values here paused all sockets for hundreds of milliseconds.
#[derive(Serialize)]
struct Snapshot<'a> {
    people: SavedPeople<'a>,
    version: u8,
}

struct SavedPeople<'a>(&'a BTreeMap<String, Person>);

impl Serialize for SavedPeople<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.collect_map(self.0.iter().filter(|(_, p)| !p.stats.is_empty()))
    }
}

impl Serialize for Stat {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        struct Periods<'a>(&'a [(Period, u32, f64, String)]);
        impl Serialize for Periods<'_> {
            fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
                serializer.collect_seq(self.0.iter().map(|(kind, n, v, t)| (kind.code(), n, v, t)))
            }
        }
        (self.v, &self.t, u8::from(self.low), Periods(&self.p), self.at, self.pat).serialize(serializer)
    }
}

fn low(name: &str) -> String {
    name.to_ascii_lowercase()
}

fn fingerprint(d: &KeyDigest) -> String {
    d.iter().take(8).map(|b| format!("{b:02x}")).collect()
}

impl Stats {
    /// The store in `dir` (created), or one kept in memory only.
    pub fn open(dir: Option<PathBuf>) -> Stats {
        let mut s = Stats { review: Review::open(None), ..Stats::default() };
        let Some(dir) = dir else { return s };
        if let Err(e) = fs::create_dir_all(&dir) {
            eprintln!("stats: cannot use {}: {e}; player stats are kept for this run only", dir.display());
            return s;
        }
        s.review = Review::open(Some(&dir));
        let file = dir.join("stats.json");
        if let Ok(text) = fs::read_to_string(&file) {
            match serde_json::from_str::<Value>(&text) {
                Ok(v) => s.load(&v),
                Err(e) => eprintln!("stats: {} is not readable ({e}); starting empty", file.display()),
            }
        }
        s.file = Some(file);
        s
    }

    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.people.len()
    }

    fn load(&mut self, v: &Value) {
        let Some(m) = v.get("people").and_then(Value::as_object) else { return };
        for (k, p) in m.iter().take(MAX_PEOPLE) {
            let Some(name) = p.get("name").and_then(Value::as_str).map(crate::ea::clean_name) else { continue };
            if low(&name) != low(k) {
                continue;
            }
            let mut stats = BTreeMap::new();
            for (key, s) in p.get("stats").and_then(Value::as_object).into_iter().flatten().take(MAX_KEYS) {
                let Some(a) = s.as_array() else { continue };
                let v = a.first().and_then(Value::as_f64).unwrap_or(0.0);
                let t = clean_text(a.get(1).and_then(Value::as_str).unwrap_or(""));
                let low_first = a.get(2).and_then(Value::as_u64) == Some(1);
                let mut st = Stat::new(low_first);
                st.v = v;
                st.t = t;
                st.at = a.get(4).and_then(Value::as_u64).unwrap_or(0);
                st.pat = a.get(5).and_then(Value::as_u64).unwrap_or(0);
                for e in a.get(3).and_then(Value::as_array).into_iter().flatten().take(8) {
                    let Some(e) = e.as_array() else { continue };
                    let Some(kind) = e.first().and_then(Value::as_u64).and_then(Period::from_code) else { continue };
                    let n = e.get(1).and_then(Value::as_u64).unwrap_or(0) as u32;
                    let pv = e.get(2).and_then(Value::as_f64).unwrap_or(0.0);
                    let pt = clean_text(e.get(3).and_then(Value::as_str).unwrap_or(""));
                    st.p.push((kind, n, pv, pt));
                }
                stats.insert(key.clone(), st);
            }
            let key = p.get("key").and_then(Value::as_str).filter(|k| k.len() == 16).map(str::to_string);
            let seen = p.get("seen").and_then(Value::as_u64).unwrap_or(0);
            self.by_uid.insert(user_id(&name), low(&name));
            self.people.insert(low(&name), Person { name, key, seen, stats });
        }
    }

    /// Written at once, whole, through a temporary file (a crash leaves the old one).
    fn save(&self) {
        let Some(file) = &self.file else { return };
        let tmp = file.with_extension("json.tmp");
        let text = match serde_json::to_vec(&Snapshot { people: SavedPeople(&self.people), version: 1 }) {
            Ok(text) => text,
            Err(e) => {
                eprintln!("stats: cannot encode {}: {e}", file.display());
                return;
            }
        };
        if let Err(e) = fs::write(&tmp, text).and_then(|_| fs::rename(&tmp, file)) {
            eprintln!("stats: cannot write {}: {e}", file.display());
        }
    }

    /// The player `name`, made if new (None when the store is full of players with stats).
    fn person(&mut self, name: &str, now: u64) -> Option<&mut Person> {
        let k = low(name);
        if !self.people.contains_key(&k) {
            if self.people.len() >= MAX_PEOPLE {
                // room from a name that only signed in (nothing counted yet)
                let idle = self.people.iter().find(|(_, p)| p.stats.is_empty()).map(|(k, p)| (k.clone(), user_id(&p.name)));
                let Some((old, uid)) = idle else { return None };
                self.people.remove(&old);
                self.by_uid.remove(&uid);
            }
            self.by_uid.insert(user_id(name), k.clone());
            self.people.insert(k.clone(), Person { name: name.to_string(), key: None, seen: now / 86_400, stats: BTreeMap::new() });
        }
        self.people.get_mut(&k)
    }

    /// A sign-in (FESL Login) under the display `name`. The latest browser-key fingerprint is
    /// metadata only; changing or losing the key never clears statistics.
    pub fn signed_in(&mut self, name: &str, key: Option<&KeyDigest>, now: u64) {
        let fp = key.map(fingerprint);
        if !self.people.contains_key(&low(name)) {
            // kept in memory until it has stats (then saved with them): who holds the name
            if let Some(p) = self.person(name, now) {
                p.key = fp;
            }
            return;
        }
        let Some(p) = self.people.get_mut(&low(name)) else { return };
        let changed = (fp.is_some() && fp != p.key) || p.seen != now / 86_400 || p.name != name;
        if fp.is_some() {
            p.key = fp;
        }
        p.seen = now / 86_400;
        p.name = name.to_string();
        if changed {
            self.save();
        }
    }

    /// Adds or changes one stat of a player; false if it could not be kept.
    fn put(&mut self, name: &str, key: &str, how: Update, v: f64, t: &str, now: u64) -> bool {
        if !plausible(v, how == Update::Min) || key.is_empty() || key.len() > 64 {
            return false;
        }
        let Some(p) = self.person(name, now) else { return false };
        let fresh = !p.stats.contains_key(key);
        if fresh && p.stats.len() >= MAX_KEYS {
            return false;
        }
        let s = p.stats.entry(key.to_string()).or_insert_with(|| Stat::new(how == Update::Min));
        if how == Update::Min {
            s.low = true;
        } else if how == Update::Max {
            s.low = false;
        }
        s.update(key, how, v, &clean_text(t), fresh, now);
        true
    }

    /// UpdateStats from `name`'s game: its own stats only (owner = its user id, ownerType 1),
    /// and only the keys kept_from_game. Returns (kept, left out).
    pub fn update(&mut self, name: &str, p: &Packet, now: u64) -> (usize, usize) {
        let uid = user_id(name).to_string();
        let count = |k: &str| p.get(k).and_then(|n| n.parse::<usize>().ok()).unwrap_or(0).min(64);
        let (mut kept, mut left, mut boards) = (0, 0, 0usize);
        for u in 0..count("u.[]") {
            let n = count(&format!("u.{u}.s.[]")).min(512);
            if p.get(&format!("u.{u}.o")) != Some(uid.as_str()) || p.get(&format!("u.{u}.ot")).unwrap_or("1") != "1" {
                left += n;
                continue;
            }
            for s in 0..n {
                let field = |f: &str| p.get(&format!("u.{u}.s.{s}.{f}")).unwrap_or("");
                let key = field("k");
                let v = field("v").parse::<f64>().unwrap_or(f64::NAN);
                let how = Update::from_ut(field("ut").parse().unwrap_or(0));
                // board keys are checked against every player's (floor): a career has at most 47
                // (the live stats), so more than BOARD_KEYS_PER_UPDATE in one request are left out
                let board = board_key(key);
                boards += usize::from(board);
                let keep = |s: &mut Self| {
                    if board {
                        boards <= BOARD_KEYS_PER_UPDATE && s.record(name, key, how, v, field("t"), now, "game") == Recorded::Kept
                    } else {
                        s.put(name, key, how, v, field("t"), now)
                    }
                };
                if kept_from_game(key) && keep(self) {
                    kept += 1;
                } else {
                    left += 1;
                }
            }
        }
        if kept > 0 {
            self.save();
        }
        (kept, left)
    }

    fn name_of(&self, uid: u32) -> Option<&Person> {
        self.by_uid.get(&uid).and_then(|k| self.people.get(k))
    }

    /// A player's stats (GetStats): (key, value, text); 0 for what it has none of.
    pub fn get(&self, uid: u32, keys: &[String], period: Period, past: u32, now: u64) -> Vec<(String, f64, String)> {
        let p = self.name_of(uid);
        keys.iter()
            .map(|k| {
                let at = p.and_then(|p| p.stats.get(k)).and_then(|s| s.at(period, past, now));
                (k.clone(), at.map_or(0.0, |a| a.0), at.map_or(String::new(), |a| a.1.to_string()))
            })
            .collect()
    }

    /// Everyone with a value for `key` in the period, best first: (owner, name, value, text).
    fn board(&self, key: &str, period: Period, past: u32, now: u64) -> Vec<(u32, &str, f64, &str)> {
        let mut low_first = false;
        let mut rows: Vec<(u32, &str, f64, &str)> = self
            .people
            .values()
            .filter_map(|p| {
                let s = p.stats.get(key)?;
                low_first |= s.low;
                if self.review.is_banned(&p.name) {
                    return None;
                }
                let (v, t) = s.at(period, past, now)?;
                plausible(v, s.low).then(|| (user_id(&p.name), p.name.as_str(), v, t)) // also what was kept before
            })
            .collect();
        rows.sort_by(|a, b| {
            let by = if low_first { a.2.total_cmp(&b.2) } else { b.2.total_cmp(&a.2) };
            by.then_with(|| low(a.1).cmp(&low(b.1)))
        });
        rows
    }

    /// A player's rank for each key (GetRankedStats, GetRankedStatsForOwners): (key, value,
    /// rank, text); rank 0 when it has no value in that period.
    pub fn ranked(&self, uid: u32, keys: &[String], period: Period, past: u32, now: u64) -> Vec<(String, f64, usize, String)> {
        keys.iter()
            .map(|k| {
                let board = self.board(k, period, past, now);
                match board.iter().position(|r| r.0 == uid) {
                    Some(i) => (k.clone(), board[i].2, i + 1, board[i].3.to_string()),
                    None => (k.clone(), 0.0, 0, String::new()),
                }
            })
            .collect()
    }

    /// Ranks `min`..=`max` of a leaderboard (GetTopN, GetTopNAndStats).
    pub fn top(&self, key: &str, min: usize, max: usize, period: Period, past: u32, now: u64) -> Vec<Ranked> {
        let min = min.max(1);
        let max = max.max(min).min(min + MAX_ROWS - 1);
        self.board(key, period, past, now)
            .into_iter()
            .enumerate()
            .skip(min - 1)
            .take(max + 1 - min)
            .map(|(i, (owner, name, value, text))| Ranked { owner, name: name.to_string(), rank: i + 1, value, text: text.to_string() })
            .collect()
    }

    /// The race host's word on a race: counted as EA's servers counted a ranked race, from what
    /// the games agree on (cross_check). A best time also needs another player's game to confirm
    /// it and the race host's clock to allow it (check_time); the public boards also need a
    /// claimed name and two addresses in the race. Returns lines for stdout.
    pub fn race(&mut self, report: RaceReport, now: u64) -> Vec<String> {
        match report {
            RaceReport::Started { gid, players } => {
                if players.len() < 2 {
                    self.racing.remove(&gid);
                    return vec![format!("stats: game {gid}: a race with one player does not count")];
                }
                for r in &players {
                    self.put(&r.name, "Total_Online_Games_Started", Update::Add, 1.0, "", now);
                    self.dnf(&r.name, now);
                }
                let names: Vec<&str> = players.iter().map(|r| r.name.as_str()).collect();
                let line = format!("stats: game {gid}: race started by {}", names.join(", "));
                self.racing.insert(gid, players);
                self.save();
                vec![line]
            }
            RaceReport::Ended { gid, settings, players, results, clock } => {
                let Some(started) = self.racing.remove(&gid) else { return Vec::new() };
                let setting = |name: &str| settings.iter().find(|(k, _)| *k == name).map(|(_, v)| v.clone()).unwrap_or_default();
                let mode = setting("game_mode").parse::<u32>().ok();
                let race = mode.and_then(crate::olmsg::race_attribute).map(setting).filter(|r| !r.is_empty() && r != "ABSTAIN");
                // the room's "length": a circuit's laps
                let laps = setting("length").parse::<u32>().ok().filter(|n| (1..=99).contains(n));
                let times = matches!(mode, Some(0 | 1));
                let reports: Vec<(u32, Vec<Row>)> = results.iter().map(|(s, m)| (*s, game_results_rows(m))).collect();
                let mut lines = Vec::new();
                let mut outcome: Vec<(&Racer, Option<Row>, bool, Checked)> = Vec::new();
                for r in &started {
                    let still = players.iter().any(|p| p.name.eq_ignore_ascii_case(&r.name));
                    let c = if still { cross_check(r.slot, &reports, times) } else { Checked::default() };
                    // how the racer's race ended: what the games agree on; when they do not, what
                    // its own game says (for finished and DNF only)
                    let row = c.row.or_else(|| if c.disputed { own_row(r.slot, &reports) } else { None });
                    outcome.push((r, row, still, c));
                }
                let suspects: Vec<u32> = outcome.iter().flat_map(|o| o.3.against.iter().copied()).collect();
                let disputed = outcome.iter().any(|o| o.3.disputed);
                if disputed {
                    lines.push(format!("stats: game {gid}: the games' reports disagree: no wins, losses or best times counted"));
                }
                let winner = if disputed { None } else { outcome.iter().filter(|o| o.3.row.is_some_and(|w| w.place == 1)).map(|o| o.0.name.clone()).next() };
                let key = race.as_deref().map(attrib_key);
                // browsers from one address racing each other count for the game's own boards only
                let one_address = started.len() > 1 && started.iter().all(|r| !r.client.is_empty() && r.client == started[0].client);
                for (r, row, still, c) in &outcome {
                    let won = winner.as_deref() == Some(r.name.as_str());
                    // a racer that left, or whose game says it did not finish a race to a line
                    let dnf = !still || row.is_none() || (matches!(mode, Some(0 | 1 | 5)) && !row.is_some_and(|w| w.finished));
                    self.put(&r.name, "Total_Online_Games", Update::Add, 1.0, "", now);
                    if !dnf {
                        self.put(&r.name, "Total_Online_Games_Finished", Update::Add, 1.0, "", now);
                    }
                    self.dnf(&r.name, now);
                    if !disputed {
                        self.put(&r.name, if won { "Overall_Solo_On_Wins" } else { "Overall_Solo_On_Losses" }, Update::Add, 1.0, "", now);
                        if won {
                            self.put(&r.name, "Current_Online_Win_Streak", Update::Add, 1.0, "", now);
                            let streak = self.get(user_id(&r.name), &["Current_Online_Win_Streak".into()], Period::All, 0, now)[0].1;
                            self.put(&r.name, "Best_Online_Win_Streak", Update::Max, streak, "", now);
                            if let Some(k) = mode.and_then(mode_wins) {
                                self.put(&r.name, k, Update::Add, 1.0, "", now);
                            }
                        } else {
                            self.put(&r.name, "Current_Online_Win_Streak", Update::Set, 0.0, "", now);
                        }
                        // The public boards' wins and streaks, counted from the reset on (and
                        // only for a proven claimed name in a race of two or more addresses);
                        // a loss ends the public streak too.
                        if won && r.owner.is_some() && !one_address {
                            self.public_count(&r.name, PUBLIC_WINS, Update::Add, 1.0, now);
                            self.public_count(&r.name, PUBLIC_STREAK, Update::Add, 1.0, now);
                            let streak = self.public_value(&r.name, PUBLIC_STREAK);
                            self.public_count(&r.name, PUBLIC_BEST_STREAK, Update::Max, streak, now);
                        } else if !won {
                            self.public_count(&r.name, PUBLIC_STREAK, Update::Set, 0.0, now);
                        }
                    }
                    if let Some(h) = key {
                        self.put(&r.name, &format!("Total_Rnkd_Evt_Games_{h:08X}"), Update::Add, 1.0, "", now);
                        if won {
                            self.put(&r.name, &format!("Evt_Wins_{h:08X}"), Update::Add, 1.0, "", now);
                        }
                        // The best time of a sprint or circuit, as the game's own report writes
                        // it (008222f0): the time, and "car,a,b,0" as text, which the board
                        // shows as Car Name and two columns (00593970 gets them by race type,
                        // 005898d0): a circuit's best lap and top speed, a sprint's top speed and
                        // NOS. GameResults has no NOS: that shows 0.
                        if let (Some(w), false, Some(m @ (0 | 1))) = (row, dnf, mode) {
                            if w.time > 0.0 {
                                match check_time(r, c, &suspects, w, m, laps, &clock) {
                                    Err(why) => lines.push(format!("stats: game {gid}: {}'s time is not counted: {why}", r.name)),
                                    Ok(()) => {
                                        let top = f64::from(w.top_speed.max(0.0));
                                        let lap = f64::from(w.best_lap.max(0.0));
                                        let (a, b) = if m == 1 { (lap, top) } else { (top, 0.0) };
                                        let text = format!("0x{:08X},{a:.6},{b:.6},0x{:08X}", r.car, 0);
                                        let t = f64::from(w.time);
                                        if let Recorded::Held(why) = self.record(&r.name, &format!("Evt_Bst_{h:08X}"), Update::Min, t, &text, now, "race") {
                                            lines.push(format!("stats: game {gid}: {}'s time waits for review: {why}", r.name));
                                        }
                                        let public = if r.owner.is_none() {
                                            Err("the name is not claimed by this browser")
                                        } else if one_address {
                                            Err("every racer came from one address")
                                        } else if !self.limits.allow(Kind::ResultForName, &r.name) {
                                            Err("too many results for this name")
                                        } else {
                                            Ok(())
                                        };
                                        match public {
                                            Err(why) => lines.push(format!("stats: game {gid}: {}'s time stays off the public boards: {why}", r.name)),
                                            Ok(()) => {
                                                let best = match (m, laps) {
                                                    (1, Some(n)) => format!("{SRV}Bst_{h:08X}_L{n}"),
                                                    _ => format!("{SRV}Bst_{h:08X}"),
                                                };
                                                if let Recorded::Held(why) = self.record(&r.name, &best, Update::Min, t, &text, now, "race") {
                                                    lines.push(format!("stats: game {gid}: {}'s public time waits for review: {why}", r.name));
                                                }
                                                if m == 1 && plausible(lap, false) && lap > 0.0 {
                                                    let lap_text = format!("0x{:08X},{lap:.6},{top:.6},0x{:08X}", r.car, 0);
                                                    if let Recorded::Held(why) = self.record(&r.name, &format!("{SRV}Lap_{h:08X}"), Update::Min, lap, &lap_text, now, "race") {
                                                        lines.push(format!("stats: game {gid}: {}'s public lap waits for review: {why}", r.name));
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    let how = if dnf {
                        if *still { "did not finish" } else { "left" }.to_string()
                    } else {
                        row.map_or("finished".into(), |w| format!("finished {}. in {:.2} s", w.place, w.time))
                    };
                    lines.push(format!("stats: game {gid}: {} {how}{}", r.name, if won { ", the win" } else { "" }));
                }
                if let Some(rid) = &race {
                    lines.push(format!("stats: game {gid}: race {rid} (key {:08X}), game mode {}", attrib_key(rid), mode.map_or("?".into(), |m| m.to_string())));
                }
                self.save();
                lines
            }
        }
    }

    /// A board result (a best time or score; Update::Min or Max): kept as put() keeps it, unless
    /// it is far better than the board's established top (floor): then it waits for an admin
    /// (leaderboard.rs Review) and the boards keep showing the old one.
    #[allow(clippy::too_many_arguments)]
    pub fn record(&mut self, name: &str, key: &str, how: Update, v: f64, t: &str, now: u64, source: &str) -> Recorded {
        if !matches!(how, Update::Min | Update::Max) {
            return if self.put(name, key, how, v, t, now) { Recorded::Kept } else { Recorded::Dropped };
        }
        let lower = how == Update::Min;
        if !plausible(v, lower) || self.review.was_rejected(name, key, v) {
            return Recorded::Dropped;
        }
        let person = self.people.get(&low(name));
        if person.is_some_and(|p| p.stats.len() >= MAX_KEYS && !p.stats.contains_key(key)) {
            return Recorded::Dropped; // put() would not keep it either
        }
        let own = person.and_then(|p| p.stats.get(key)).map(|s| s.v);
        let improves = own.map_or(true, |o| if lower { v < o } else { v > o });
        if improves {
            if let Some(why) = self.floor(name, key, lower, v) {
                let p = Pending { id: 0, name: name.to_string(), key: key.to_string(), how, value: v, text: clean_text(t), at: now, source: source.to_string(), why: why.clone() };
                let held = self.review.hold(p, lower);
                self.review.save();
                println!("stats: {name}'s {key} {} waits for review: {why}", num(v));
                return if held { Recorded::Held(why) } else { Recorded::Dropped };
            }
        }
        if self.put(name, key, how, v, t, now) { Recorded::Kept } else { Recorded::Dropped }
    }

    /// Why `v` must wait for an admin: it beats the best of the board's other entries by more than
    /// the margin, on a board with FLOOR_ENTRIES of them (a newer board has no established top).
    fn floor(&self, name: &str, key: &str, lower: bool, v: f64) -> Option<String> {
        let me = low(name);
        let (mut n, mut best) = (0usize, None::<f64>);
        for (k, p) in &self.people {
            let Some(s) = p.stats.get(key) else { continue };
            if *k == me || !plausible(s.v, s.low) || s.v <= 0.0 || self.review.is_banned(&p.name) {
                continue;
            }
            n += 1;
            best = Some(best.map_or(s.v, |b| if lower { b.min(s.v) } else { b.max(s.v) }));
        }
        let b = best?;
        if n < FLOOR_ENTRIES {
            return None;
        }
        if lower && v < b * (1.0 - FLOOR_TIME_MARGIN) {
            return Some(format!("{} s is more than {:.0}% faster than the board's best of {n}, {} s", num(v), FLOOR_TIME_MARGIN * 100.0, num(b)));
        }
        let factor = if key.starts_with("CD_") { FLOOR_CANYON_FACTOR } else { FLOOR_SCORE_FACTOR };
        if !lower && b > 0.0 && v > b * factor {
            return Some(format!("{} is more than {factor} times the board's best of {n}, {}", num(v), num(b)));
        }
        None
    }

    /// The public boards (leaderboard.rs): every board key with its rows best first, at most
    /// leaderboard::MAX_ROWS each. Names banned from the boards and names `claimed` says are
    /// not claimed are left out; so are impossible values and zeros. `game` adds the boards of
    /// what the players' own games sent.
    pub fn public_boards(&self, claimed: &dyn Fn(&str) -> bool, game: bool, now: u64) -> Vec<crate::leaderboard::Board> {
        use crate::leaderboard::{Board, Row as Entry};
        let week = week_number(now);
        let tag = self.reset_tag();
        let mut boards: BTreeMap<String, Board> = BTreeMap::new();
        for p in self.people.values() {
            if self.review.is_banned(&p.name) {
                continue;
            }
            let mut is_claimed = None;
            for (key, s) in &p.stats {
                let Some(spec) = public_spec(key, game) else { continue };
                if !plausible(s.v, s.low) || s.v <= 0.0 {
                    continue;
                }
                // a count from before the latest reset does not show
                let counted = key == PUBLIC_WINS || key == PUBLIC_BEST_STREAK;
                if counted && s.t != tag {
                    continue;
                }
                if !*is_claimed.get_or_insert_with(|| claimed(&p.name)) {
                    break;
                }
                let b = boards.entry(spec.id.clone()).or_insert_with(|| Board {
                    id: spec.id,
                    family: spec.family.to_string(),
                    source: spec.source,
                    race: spec.race,
                    laps: spec.laps,
                    low: s.low,
                    unit: if s.low { "time" } else if spec.count { "count" } else { "points" },
                    players: 0,
                    all: Vec::new(),
                    week: Vec::new(),
                    day: Vec::new(),
                });
                b.all.push(Entry { name: p.name.clone(), value: s.v, car: car_of(&s.t), when: s.at, numbers: None });
                if let Some((wv, wt)) = s.at(Period::Week, 0, now) {
                    if plausible(wv, s.low) && wv > 0.0 && (!counted || wt == tag) {
                        let when = if Period::Week.number(s.pat) == week { s.pat } else { 0 };
                        b.week.push(Entry { name: p.name.clone(), value: wv, car: car_of(wt), when, numbers: None });
                    }
                }
            }
        }
        let mut out: Vec<Board> = boards.into_values().collect();
        for b in &mut out {
            b.players = b.all.len();
            for rows in [&mut b.all, &mut b.week] {
                let lower = b.low;
                rows.sort_by(|x, y| {
                    let by = if lower { x.value.total_cmp(&y.value) } else { y.value.total_cmp(&x.value) };
                    by.then_with(|| low(&x.name).cmp(&low(&y.name)))
                });
                rows.truncate(crate::leaderboard::MAX_ROWS);
            }
        }
        out
    }

    /// The in-game group of the public answer (leaderboard.rs, source "ingame"): what the game's
    /// own LEADERBOARD and ONLINE PLAYER STATS show, from the same rows as board() (and so top(),
    /// GetTopN): every name, banned names left out, plausible() values only (zeros and the
    /// game's own numbers included, as the game shows them), best first, for all time, the
    /// current week and the current day; at most leaderboard::MAX_ROWS each. One pass over the
    /// players instead of one board() per key and period. One difference for the public web
    /// page: a name the chat's word filter refuses (moderation::profane, whole words) is left
    /// out too; the game's own board still lists it.
    pub fn ingame_boards(&self, now: u64) -> Vec<crate::leaderboard::Board> {
        use crate::leaderboard::{Board, Row as Entry};
        const PERIODS: [Period; 3] = [Period::All, Period::Week, Period::Day];
        let mut boards: BTreeMap<String, (Board, bool)> = BTreeMap::new();
        for p in self.people.values() {
            let mut banned = None; // worked out at the player's first in-game key
            for (key, s) in &p.stats {
                let Some((family, race, count)) = ingame_spec(key) else { continue };
                let banned = *banned.get_or_insert_with(|| self.review.is_banned(&p.name) || crate::moderation::profane(&p.name));
                let id = race.as_ref().map_or(format!("ingame:{family}"), |h| format!("ingame:{family}:{h}"));
                let (b, _) = boards.entry(id.clone()).or_insert_with(|| {
                    let board = Board { id, family: family.to_string(), source: "ingame", race, laps: None, low: false, unit: "", players: 0, all: Vec::new(), week: Vec::new(), day: Vec::new() };
                    (board, count)
                });
                // board(): a key ranks lowest first when any holder's stat is a time, banned ones too
                // (and names left out for the word filter)
                b.low |= s.low;
                if banned {
                    continue;
                }
                for period in PERIODS {
                    let Some((v, t)) = s.at(period, 0, now) else { continue };
                    if !plausible(v, s.low) {
                        continue;
                    }
                    let when = match period {
                        Period::All => s.at,
                        _ if period.number(s.pat) == period.number(now) => s.pat,
                        _ => 0,
                    };
                    let numbers = if family == "Evt_Bst" { numbers_of(t) } else { None };
                    let row = Entry { name: p.name.clone(), value: v, car: car_of(t), when, numbers };
                    match period {
                        Period::All => b.all.push(row),
                        Period::Week => b.week.push(row),
                        Period::Day => b.day.push(row),
                    }
                }
            }
        }
        let mut out = Vec::with_capacity(boards.len());
        for (mut b, count) in boards.into_values() {
            if b.all.is_empty() {
                continue; // only banned names hold it: the game shows no rows either
            }
            b.players = b.all.len();
            b.unit = if b.low { "time" } else if count { "count" } else { "points" };
            let lower = b.low;
            for rows in [&mut b.all, &mut b.week, &mut b.day] {
                // board()'s order: best first, then the name case-blind
                rows.sort_by(|x, y| {
                    let by = if lower { x.value.total_cmp(&y.value) } else { y.value.total_cmp(&x.value) };
                    by.then_with(|| low(&x.name).cmp(&low(&y.name)))
                });
                rows.truncate(crate::leaderboard::MAX_ROWS);
            }
            out.push(b);
        }
        out
    }

    /// The admin's leaderboard tools (leaderboard.rs /leaderboard/admin): pending, approve {id},
    /// reject {id, text}, remove {name, key}, ban {name, text}, unban {name}, banned, audit. Every
    /// change goes to the audit log with `actor`.
    pub fn admin(&mut self, op: &str, actor: &str, args: &Value, now: u64) -> Result<Value, &'static str> {
        let field = |k: &str| args.get(k).and_then(Value::as_str).map(str::trim).unwrap_or("").to_string();
        let text = field("text");
        if text.chars().count() > 200 || text.chars().any(char::is_control) {
            return Err("text");
        }
        let offset = args.get("offset").and_then(Value::as_u64).unwrap_or(0).min(1_000_000) as usize;
        let id = args.get("id").and_then(Value::as_u64);
        let name = field("name");
        let target = || -> Result<String, &'static str> {
            if name.is_empty() || name.len() > 20 { Err("name") } else { Ok(name.clone()) }
        };
        match op {
            "pending" => {
                let rows: Vec<Value> = self
                    .review
                    .pending
                    .iter()
                    .skip(offset)
                    .take(50)
                    .map(|p| {
                        let best = self.floor_best(&p.name, &p.key, p.how == Update::Min);
                        json!({"id": p.id, "name": p.name, "key": p.key, "value": p.value, "text": p.text, "at": p.at, "source": p.source, "why": p.why, "best": best})
                    })
                    .collect();
                Ok(json!({"total": self.review.pending.len(), "rows": rows}))
            }
            "approve" => {
                let p = self.review.take(id.ok_or("id")?).ok_or("id")?;
                let at = if p.at > 0 { p.at } else { now };
                self.put(&p.name, &p.key, p.how, p.value, &p.text, at);
                self.save();
                let log = self.review.log("approve", actor, &p.name, &p.key, Some(p.value), &text, now);
                if !self.review.save() {
                    return Err("storage");
                }
                Ok(json!({"id": p.id, "log": log, "name": p.name, "key": p.key, "value": p.value}))
            }
            "reject" => {
                let p = self.review.take(id.ok_or("id")?).ok_or("id")?;
                self.review.reject(&p);
                let log = self.review.log("reject", actor, &p.name, &p.key, Some(p.value), &text, now);
                if !self.review.save() {
                    return Err("storage");
                }
                Ok(json!({"id": p.id, "log": log, "name": p.name, "key": p.key, "value": p.value}))
            }
            "remove" => {
                let name = target()?;
                let key = field("key");
                let person = self.people.get_mut(&low(&name)).ok_or("name")?;
                let old = person.stats.remove(&key).ok_or("key")?;
                let shown = person.name.clone();
                self.save();
                let log = self.review.log("remove", actor, &shown, &key, Some(old.v), &text, now);
                if !self.review.save() {
                    return Err("storage");
                }
                Ok(json!({"log": log, "name": shown, "key": key, "value": old.v}))
            }
            "ban" | "unban" => {
                let name = target()?;
                let changed = if op == "ban" { self.review.ban(&name, &text, now) } else { self.review.unban(&name) };
                if !changed {
                    return Err(if op == "ban" { "full" } else { "name" });
                }
                let log = self.review.log(op, actor, &name, "", None, &text, now);
                if !self.review.save() {
                    return Err("storage");
                }
                Ok(json!({"log": log, "name": name}))
            }
            "banned" => {
                let rows: Vec<Value> = self.review.banned.values().skip(offset).take(100).map(|(n, at, why)| json!({"name": n, "at": at, "why": why})).collect();
                Ok(json!({"total": self.review.banned.len(), "rows": rows}))
            }
            "audit" => {
                let (total, rows) = self.review.audit(offset, 50);
                Ok(json!({"total": total, "rows": rows}))
            }
            _ => Err("operation"),
        }
    }

    /// From when the public wins and streaks count (CARBON_LEADERBOARD_RESET).
    pub fn set_reset(&mut self, at: u64) {
        self.reset = at;
    }

    fn reset_tag(&self) -> String {
        format!("reset:{}", self.reset)
    }

    /// A public count (wins, streaks) of `name`: a value counted under an earlier reset starts
    /// again from nothing, and nothing counts before the reset.
    fn public_count(&mut self, name: &str, key: &str, how: Update, v: f64, now: u64) {
        if now < self.reset {
            return;
        }
        let tag = self.reset_tag();
        if let Some(p) = self.people.get_mut(&low(name)) {
            if p.stats.get(key).is_some_and(|s| s.t != tag) {
                p.stats.remove(key);
            }
        }
        self.put(name, key, how, v, &tag, now);
    }

    /// A public count's value under the current reset (0 when it has none).
    fn public_value(&self, name: &str, key: &str) -> f64 {
        let tag = self.reset_tag();
        self.people.get(&low(name)).and_then(|p| p.stats.get(key)).filter(|s| s.t == tag).map_or(0.0, |s| s.v)
    }

    /// The best of a board's other entries (what floor() compares with), for the admin.
    fn floor_best(&self, name: &str, key: &str, lower: bool) -> Option<f64> {
        let me = low(name);
        self.people
            .iter()
            .filter(|(k, p)| **k != me && !self.review.is_banned(&p.name))
            .filter_map(|(_, p)| p.stats.get(key).map(|s| s.v))
            .filter(|v| plausible(*v, lower) && *v > 0.0)
            .reduce(|a, b| if lower { a.min(b) } else { a.max(b) })
    }

    /// A race report (trace.rs hands over the race hosts'), counted now.
    pub fn race_report(&mut self, report: RaceReport) -> Vec<String> {
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        self.race(report, now)
    }

    /// DNF: the share of the races a player started that it did not finish (0..1; the game
    /// shows it as a percentage).
    fn dnf(&mut self, name: &str, now: u64) {
        let got = self.get(user_id(name), &["Total_Online_Games_Started".into(), "Total_Online_Games_Finished".into()], Period::All, 0, now);
        let (started, finished) = (got[0].1, got[1].1);
        if started > 0.0 {
            self.put(name, "DNF", Update::Set, ((started - finished) / started).clamp(0.0, 1.0), "", now);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: u64 = 1_793_000_000; // 2026-10-26

    fn update(name: &str, stats: &[(&str, &str, &str, u8)]) -> Packet {
        let mut p = Packet::new(b"rank", 0x8000_0020).with("TXN", "UpdateStats").with("u.0.o", user_id(name)).with("u.0.ot", 1);
        for (i, (k, v, t, ut)) in stats.iter().enumerate() {
            p = p.with(&format!("u.0.s.{i}.ut"), ut).with(&format!("u.0.s.{i}.k"), k).with(&format!("u.0.s.{i}.v"), v).with(&format!("u.0.s.{i}.t"), t);
        }
        p.with("u.0.s.[]", stats.len()).with("u.[]", 1)
    }

    #[test]
    fn attrib_keys_are_the_games() {
        // the leaderboard asked for Evt_Bst_30F07253 with the first circuit (mu.1.1) shown
        assert_eq!(attrib_key("mu.1.1"), 0x30F0_7253);
        assert_eq!(format!("{:08X}", attrib_key("ex.5.1")), "F907507E");
    }

    #[test]
    fn impossible_results_stay_off_the_leaderboard() {
        let mut s = Stats::default();
        for (who, t) in [("Real", "103.05"), ("Zero", "0"), ("Fast", "1.5"), ("Less", "-40"), ("Nan", "NaN"), ("Inf", "inf")] {
            s.update(who, &update(who, &[("Evt_Bst_30F07253", t, "", 2)]), NOW);
        }
        s.update("Huge", &update("Huge", &[("PK_Racer_Wins", "1e12", "", 1)]), NOW);
        let top = s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW);
        assert_eq!(top.iter().map(|r| r.name.as_str()).collect::<Vec<_>>(), ["Real"]);
        assert_eq!(s.get(user_id("huge"), &["PK_Racer_Wins".into()], Period::All, 0, NOW)[0].1, 0.0);
        // A time kept before this check came in leaves the board too.
        s.update("Old", &update("Old", &[("Evt_Bst_30F07253", "90", "", 2)]), NOW);
        s.people.get_mut("old").unwrap().stats.get_mut("Evt_Bst_30F07253").unwrap().v = 2.0;
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).len(), 1);
    }

    #[test]
    fn the_game_keeps_its_best_and_adds_counts() {
        let mut s = Stats::open(None);
        let (kept, left) = s.update("Metin", &update("Metin", &[("Evt_Bst_30F07253", "103.0500", "0x00000001,50.5,72.9,0x0", 2), ("Cars_Unlocked_00000001", "1.0000", "", 0), ("PK_Racer_Wins", "1.0000", "", 3)]), NOW);
        assert_eq!((kept, left), (2, 1));
        s.update("Metin", &update("Metin", &[("Evt_Bst_30F07253", "110.0000", "slower", 2), ("PK_Racer_Wins", "1.0000", "", 3)]), NOW);
        let got = s.get(user_id("metin"), &["Evt_Bst_30F07253".into(), "PK_Racer_Wins".into(), "Cars_Unlocked_00000001".into()], Period::All, 0, NOW);
        assert_eq!(got[0], ("Evt_Bst_30F07253".into(), 103.05, "0x00000001,50.5,72.9,0x0".into()));
        assert_eq!(got[1].1, 2.0);
        assert_eq!(got[2].1, 0.0);
        // another player's stats, and the ones the server counts itself, are not taken
        let (kept, left) = s.update("Jan", &update("Metin", &[("PK_Racer_Wins", "5.0", "", 3)]), NOW);
        assert_eq!((kept, left), (0, 1));
        let (kept, _) = s.update("Metin", &update("Metin", &[("Overall_Solo_On_Wins", "50.0", "", 0)]), NOW);
        assert_eq!(kept, 0);
    }

    #[test]
    fn leaderboards_rank_times_lowest_first_per_period() {
        let mut s = Stats::open(None);
        for (who, t) in [("Metin", "103.0"), ("Jan", "99.5"), ("Ada", "120.0")] {
            s.update(who, &update(who, &[("Evt_Bst_30F07253", t, "", 2)]), NOW);
        }
        let top = s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW);
        assert_eq!(top.iter().map(|r| (r.name.as_str(), r.rank)).collect::<Vec<_>>(), [("Jan", 1), ("Metin", 2), ("Ada", 3)]);
        assert_eq!(s.ranked(user_id("Metin"), &["Evt_Bst_30F07253".into()], Period::Week, 0, NOW)[0].2, 2);
        // last week's board is empty; next week Metin's time is last week's
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::Week, 1, NOW).is_empty());
        let later = NOW + 7 * 86_400;
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::Week, 0, later).is_empty());
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::Week, 1, later).len(), 3);
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, later).len(), 3);
        // a player with no time has no rank
        assert_eq!(s.ranked(user_id("Nobody"), &["Evt_Bst_30F07253".into()], Period::All, 0, NOW)[0], ("Evt_Bst_30F07253".into(), 0.0, 0, String::new()));
        // the rows asked for: ranks 2 to 3
        assert_eq!(s.top("Evt_Bst_30F07253", 2, 3, Period::All, 0, NOW).iter().map(|r| r.rank).collect::<Vec<_>>(), [2, 3]);
    }

    /// A GameResults as a game sends it: the rows (racer id, place, finished, time, top speed).
    pub fn results(rows: &[(u32, u8, bool, f32, f32)]) -> Vec<u8> {
        let mut m = crate::olmsg::GAME_RESULTS.to_be_bytes().to_vec();
        m.extend_from_slice(&[rows.len() as u8, 1, 0x13, 0x88, 0, 2, 0, 0, 0, 1]);
        for (id, place, fin, time, top) in rows {
            m.extend_from_slice(&[u8::from(!*fin), *place, 1]);
            for w in [0, *id, (time / 2.0).to_bits(), time.to_bits(), 0, 50f32.to_bits(), top.to_bits(), 0, 0, 0, 0] {
                m.extend_from_slice(&w.to_be_bytes());
            }
        }
        m.resize(crate::olmsg::GAME_RESULTS_LEN, 0);
        m
    }

    fn racers(names: &[&str]) -> Vec<Racer> {
        // each from its own address, with its claimed name proven
        names
            .iter()
            .enumerate()
            .map(|(i, n)| Racer { slot: i as u32, name: n.to_string(), car: attrib_key("r8"), vip: 16 + i as u32, client: format!("192.0.2.{i}"), owner: Some(format!("owner-of-{n}")) })
            .collect()
    }

    fn circuit() -> Vec<(&'static str, String)> {
        vec![("game_type", "1".into()), ("game_mode", "1".into()), ("race_type_circuit", "mu.1.1".into())]
    }

    #[test]
    fn a_race_counts_wins_losses_streaks_and_the_board() {
        let mut s = Stats::open(None);
        let both = racers(&["Metin", "Jan"]);
        let r = results(&[(1, 1, true, 103.05, 72.9), (5, 2, true, 104.76, 72.5)]);
        for _ in 0..2 {
            s.race(RaceReport::Started { gid: 200_000, players: both.clone() }, NOW);
            let lines = s.race(RaceReport::Ended { gid: 200_000, settings: circuit(), players: both.clone(), results: vec![(0, r.clone()), (1, r.clone())], clock: Vec::new() }, NOW);
            assert!(lines.iter().any(|l| l.contains("Metin finished 1. in 103.05 s, the win")), "{lines:?}");
        }
        let keys: Vec<String> = ["Overall_Solo_On_Wins", "Overall_Solo_On_Losses", "Best_Online_Win_Streak", "Circuit_On_Wins", "Total_Online_Games_Started", "Total_Online_Games_Finished", "DNF", "Total_Rnkd_Evt_Games_30F07253", "Evt_Wins_30F07253"].iter().map(|k| k.to_string()).collect();
        let v = |who: &str| s.get(user_id(who), &keys, Period::All, 0, NOW).into_iter().map(|x| x.1).collect::<Vec<_>>();
        assert_eq!(v("Metin"), [2.0, 0.0, 2.0, 2.0, 2.0, 2.0, 0.0, 2.0, 2.0]);
        assert_eq!(v("Jan"), [0.0, 2.0, 0.0, 0.0, 2.0, 2.0, 0.0, 2.0, 0.0]);
        let top = s.top("Evt_Bst_30F07253", 1, 10, Period::Week, 0, NOW);
        assert_eq!(top.iter().map(|r| r.name.as_str()).collect::<Vec<_>>(), ["Metin", "Jan"]);
        assert!((top[0].value - 103.05).abs() < 1e-3);
        assert!(top[0].text.starts_with("0x8774322C,51.525002,72.9"), "{}", top[0].text);
    }

    #[test]
    fn leaving_is_a_loss_and_a_dnf_and_one_player_counts_nothing() {
        let mut s = Stats::open(None);
        s.race(RaceReport::Started { gid: 1, players: racers(&["Metin", "Jan"]) }, NOW);
        // Jan left before the end: only Metin's game reports, and lists Jan as not finished
        let r = results(&[(1, 1, true, 103.0, 70.0), (5, 2, false, 0.0, 0.0)]);
        s.race(RaceReport::Ended { gid: 1, settings: circuit(), players: racers(&["Metin"]), results: vec![(0, r)], clock: Vec::new() }, NOW);
        let keys: Vec<String> = ["Overall_Solo_On_Wins", "Overall_Solo_On_Losses", "Total_Online_Games_Started", "Total_Online_Games_Finished", "DNF"].iter().map(|k| k.to_string()).collect();
        let v = |s: &Stats, who: &str| s.get(user_id(who), &keys, Period::All, 0, NOW).into_iter().map(|x| x.1).collect::<Vec<_>>();
        assert_eq!(v(&s, "Metin"), [1.0, 0.0, 1.0, 1.0, 0.0]);
        assert_eq!(v(&s, "Jan"), [0.0, 1.0, 1.0, 0.0, 1.0]);
        // Metin's time is his own game's word alone: no board
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
        // alone in a room: nothing counts
        let lines = s.race(RaceReport::Started { gid: 2, players: racers(&["Ada"]) }, NOW);
        assert!(lines[0].contains("one player"));
        assert!(s.race(RaceReport::Ended { gid: 2, settings: circuit(), players: racers(&["Ada"]), results: Vec::new(), clock: Vec::new() }, NOW).is_empty());
        assert_eq!(v(&s, "Ada"), [0.0; 5]);
    }

    #[test]
    fn the_store_survives_a_restart_and_a_new_browser_keeps_stats() {
        let dir = std::env::temp_dir().join(format!("carbon-stats-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let key_a = crate::messenger::digest("browser-a-key-0123456789");
        let key_b = crate::messenger::digest("browser-b-key-0123456789");
        {
            let mut s = Stats::open(Some(dir.clone()));
            s.race(RaceReport::Started { gid: 1, players: racers(&["Metin", "Jan"]) }, NOW);
            let r = results(&[(1, 1, true, 100.0, 70.0), (5, 2, true, 101.0, 70.0)]);
            s.race(RaceReport::Ended { gid: 1, settings: circuit(), players: racers(&["Metin", "Jan"]), results: vec![(0, r.clone()), (1, r)], clock: Vec::new() }, NOW);
            s.signed_in("Metin", Some(&key_a), NOW);
        }
        let text = fs::read_to_string(dir.join("stats.json")).unwrap();
        assert!(!text.contains("browser-a"));
        let mut s = Stats::open(Some(dir.clone()));
        assert_eq!(s.len(), 2);
        assert_eq!(s.get(user_id("Metin"), &["Overall_Solo_On_Wins".into()], Period::All, 0, NOW)[0].1, 1.0);
        assert_eq!(s.ranked(user_id("Jan"), &["Evt_Bst_30F07253".into()], Period::Week, 0, NOW)[0].2, 2);
        // The same browser and a different browser both keep the name's statistics.
        s.signed_in("Metin", Some(&key_a), NOW + 86_400);
        s.signed_in("Metin", Some(&key_b), NOW + 40 * 86_400);
        assert_eq!(s.get(user_id("Metin"), &["Overall_Solo_On_Wins".into()], Period::All, 0, NOW)[0].1, 1.0);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_car_is_read_from_player_car_data() {
        // Jan's PlayerCarData in run k1 (the start of it): an Audi R8
        let m = [0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0, 3, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0x22, 1, 0x2b, 0x18, 0, 3, b'J', b'a', b'n', 0, 4, 0x2c, 0x32, 0x74, 0x87, 4, 0x2c];
        assert_eq!(vehicle_key(Some(&m)), attrib_key("r8"));
        assert_eq!(attrib_key("gallardo"), 0x7b2c_c40b);
        assert_eq!(vehicle_key(Some(&m[..30])), 0);
        assert_eq!(vehicle_key(None), 0);
    }

    #[test]
    fn numbers_are_written_as_eas() {
        assert_eq!(num(0.0), "0.0");
        assert_eq!(num(2.0), "2.0");
        assert_eq!(num(103.050_003), "103.05");
        assert_eq!(num(0.333_333), "0.3333");
    }
    // ---- the leaderboard's defences (leaderboard.rs) ----

    fn race_of(s: &mut Stats, gid: u32, players: &[Racer], results: Vec<(u32, Vec<u8>)>, clock: Vec<(u32, f64)>) -> Vec<String> {
        s.race(RaceReport::Started { gid, players: players.to_vec() }, NOW);
        s.race(RaceReport::Ended { gid, settings: circuit(), players: players.to_vec(), results, clock }, NOW)
    }

    fn value(s: &Stats, who: &str, key: &str) -> f64 {
        s.get(user_id(who), &[key.into()], Period::All, 0, NOW)[0].1
    }

    fn names(top: &[Ranked]) -> Vec<&str> {
        top.iter().map(|r| r.name.as_str()).collect()
    }

    #[test]
    fn the_games_reports_are_cross_checked() {
        let row = |racer, place, time: f32| Row { racer, place, finished: true, time, best_lap: time / 2.0, top_speed: 70.0 };
        let same = vec![row(1, 1, 100.0), row(5, 2, 101.0)];
        // both games agree (within hundredths): each racer is witnessed by the other's game, and
        // the slower of the two times counts
        let c = cross_check(0, &[(0, same.clone()), (1, vec![row(1, 1, 100.03), row(5, 2, 101.0)])], true);
        assert!(c.witnessed && !c.disputed && c.against.is_empty());
        assert!((c.row.unwrap().time - 100.03).abs() < 1e-4);
        // only its own game says so: agreed, but no witness
        let c = cross_check(0, &[(0, same.clone())], true);
        assert!(!c.witnessed && c.row.is_some());
        // two games, two stories: disputed, and both are suspects
        let c = cross_check(0, &[(0, vec![row(1, 1, 80.0)]), (1, vec![row(1, 1, 100.0)])], true);
        assert!(c.disputed && c.row.is_none() && c.against == [0, 1]);
        // three games: the majority wins and the odd one out is named
        let c = cross_check(1, &[(0, vec![row(5, 1, 70.0)]), (1, vec![row(5, 1, 70.0)]), (2, vec![row(5, 1, 99.0)])], true);
        assert!(!c.disputed && c.witnessed && c.against == [2]);
        // places and finishing must agree too
        assert!(cross_check(0, &[(0, vec![row(1, 1, 100.0)]), (1, vec![row(1, 2, 100.0)])], true).disputed);
        // no report has the racer
        assert_eq!(cross_check(3, &[(0, same)], true), Checked::default());
    }

    #[test]
    fn a_faked_time_in_ones_own_report_counts_nothing() {
        let mut s = Stats::open(None);
        let both = racers(&["Cheat", "Jan"]);
        // Cheat's game says it won in 60 s; Jan's game saw Cheat finish second in 110 s
        let fake = results(&[(1, 1, true, 60.0, 70.0), (5, 2, true, 101.0, 70.0)]);
        let real = results(&[(1, 2, true, 110.0, 70.0), (5, 1, true, 101.0, 70.0)]);
        let lines = race_of(&mut s, 1, &both, vec![(0, fake), (1, real)], Vec::new());
        assert!(lines.iter().any(|l| l.contains("disagree")), "{lines:?}");
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
        assert!(s.top("Srv_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
        assert_eq!((value(&s, "Cheat", "Overall_Solo_On_Wins"), value(&s, "Jan", "Overall_Solo_On_Wins")), (0.0, 0.0));
        assert_eq!(value(&s, "Cheat", "Total_Online_Games"), 1.0);
    }

    #[test]
    fn three_games_outvote_a_liar_who_then_counts_for_nothing() {
        let mut s = Stats::open(None);
        let three = racers(&["Ada", "Jan", "Liar"]);
        let honest = results(&[(1, 1, true, 100.0, 70.0), (5, 2, true, 101.0, 70.0), (9, 3, true, 102.0, 70.0)]);
        // Liar's game makes Jan slow and itself fast
        let lie = results(&[(1, 1, true, 100.0, 70.0), (5, 3, true, 140.0, 70.0), (9, 2, true, 90.0, 70.0)]);
        let lines = race_of(&mut s, 1, &three, vec![(0, honest.clone()), (1, honest), (2, lie)], Vec::new());
        let top = s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW);
        assert_eq!(names(&top), ["Ada", "Jan"], "{lines:?}");
        assert!((top[1].value - 101.0).abs() < 1e-3);
        assert!(lines.iter().any(|l| l.contains("Liar's time is not counted")), "{lines:?}");
        assert_eq!(value(&s, "Ada", "Overall_Solo_On_Wins"), 1.0);
    }

    #[test]
    fn the_race_hosts_clock_and_the_rows_own_numbers_bound_a_time() {
        let both = racers(&["Metin", "Jan"]);
        let r = results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]);
        // the reports came 109 s after the start: fine (run r1: 327 s for 315.70 s and 320.95 s)
        let mut s = Stats::open(None);
        race_of(&mut s, 1, &both, vec![(0, r.clone()), (1, r.clone())], vec![(0, 109.0), (1, 109.0)]);
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).len(), 2);
        // a game whose clock runs fast: 103 s of race in 60 s of the host's time
        let mut s = Stats::open(None);
        let lines = race_of(&mut s, 1, &both, vec![(0, r.clone()), (1, r)], vec![(0, 60.0), (1, 60.0)]);
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty(), "{lines:?}");
        assert!(lines.iter().any(|l| l.contains("after the start")), "{lines:?}");
        // a top speed no car has
        let mut s = Stats::open(None);
        let fast = results(&[(1, 1, true, 103.0, 300.0), (5, 2, true, 104.0, 72.0)]);
        race_of(&mut s, 1, &both, vec![(0, fast.clone()), (1, fast)], Vec::new());
        assert_eq!(names(&s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW)), ["Jan"]);
        // a 3-lap circuit shorter than three of its best laps (the helper's best lap is time / 2)
        let mut s = Stats::open(None);
        let mut three_laps = circuit();
        three_laps.push(("length", "3".into()));
        s.race(RaceReport::Started { gid: 1, players: both.clone() }, NOW);
        let r = results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]);
        s.race(RaceReport::Ended { gid: 1, settings: three_laps, players: both, results: vec![(0, r.clone()), (1, r)], clock: Vec::new() }, NOW);
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
    }

    #[test]
    fn the_public_boards_need_a_claimed_name_two_addresses_and_keep_laps_apart() {
        let r = results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]);
        let mut s = Stats::open(None);
        let mut both = racers(&["Metin", "Guest"]);
        both[1].owner = None; // Guest's name is not claimed (or its browser did not prove it)
        race_of(&mut s, 1, &both, vec![(0, r.clone()), (1, r.clone())], Vec::new());
        // the game's own board has both; the public one only Metin
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).len(), 2);
        assert_eq!(names(&s.top("Srv_Bst_30F07253", 1, 10, Period::All, 0, NOW)), ["Metin"]);
        assert_eq!(names(&s.top("Srv_Lap_30F07253", 1, 10, Period::All, 0, NOW)), ["Metin"]);
        // two browsers on one address racing each other
        let mut s = Stats::open(None);
        let mut alts = racers(&["Main", "Alt"]);
        alts[1].client = alts[0].client.clone();
        race_of(&mut s, 1, &alts, vec![(0, r.clone()), (1, r.clone())], Vec::new());
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).len(), 2);
        assert!(s.top("Srv_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
        // a 2-lap circuit is its own board
        let mut s = Stats::open(None);
        let both = racers(&["Metin", "Jan"]);
        let mut two_laps = circuit();
        two_laps.push(("length", "2".into()));
        s.race(RaceReport::Started { gid: 1, players: both.clone() }, NOW);
        let r2 = results(&[(1, 1, true, 210.0, 72.0), (5, 2, true, 212.0, 72.0)]);
        s.race(RaceReport::Ended { gid: 1, settings: two_laps, players: both, results: vec![(0, r2.clone()), (1, r2)], clock: Vec::new() }, NOW);
        assert_eq!(s.top("Srv_Bst_30F07253_L2", 1, 10, Period::All, 0, NOW).len(), 2);
        assert!(s.top("Srv_Bst_30F07253", 1, 10, Period::All, 0, NOW).is_empty());
        let boards = s.public_boards(&|_| true, false, NOW);
        let ids: Vec<&str> = boards.iter().map(|b| b.id.as_str()).collect();
        assert!(ids.contains(&"Evt_Bst:30F07253:2") && ids.contains(&"Evt_Bst_Lap:30F07253") && ids.contains(&"Overall_Solo_On_Wins"), "{ids:?}");
        let b = boards.iter().find(|b| b.id == "Evt_Bst:30F07253:2").unwrap();
        assert_eq!((b.laps, b.race.as_deref(), b.source, b.unit, b.low), (Some(2), Some("30F07253"), "race", "time", true));
        assert_eq!(b.all[0].car, format!("0x{:08x}", attrib_key("r8")));
        assert_eq!(b.all[0].when, NOW);
        assert_eq!(b.week.len(), 2);
    }

    #[test]
    fn a_name_cannot_flood_the_public_boards_with_races() {
        let mut s = Stats::open(None);
        let both = racers(&["Metin", "Jan"]);
        let mut kept = 0;
        for gid in 0..25 {
            // each race a little faster than the last, so each one would be kept
            let t = 200.0 - gid as f32;
            let r = results(&[(1, 1, true, t, 72.0), (5, 2, true, t + 1.0, 72.0)]);
            let lines = race_of(&mut s, gid, &both, vec![(0, r.clone()), (1, r)], Vec::new());
            kept += usize::from(!lines.iter().any(|l| l.contains("Metin's time stays off the public boards: too many")));
        }
        assert_eq!(kept, 20);
    }

    #[test]
    fn a_time_far_ahead_of_an_established_board_waits_for_an_admin() {
        let mut s = Stats::open(None);
        for (i, who) in ["A", "B", "C", "D", "E"].iter().enumerate() {
            s.update(who, &update(who, &[("Evt_Bst_30F07253", &format!("{}", 100 + i), "", 2)]), NOW);
        }
        // 4 % faster than the best: kept
        s.update("Quick", &update("Quick", &[("Evt_Bst_30F07253", "96", "", 2)]), NOW);
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 1, Period::All, 0, NOW)[0].name, "Quick");
        // 40 % faster: held, the board keeps its top
        let (kept, _) = s.update("Cheat", &update("Cheat", &[("Evt_Bst_30F07253", "58", "0x00000001,1,2,0x0", 2)]), NOW);
        assert_eq!(kept, 0);
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 1, Period::All, 0, NOW)[0].name, "Quick");
        assert_eq!(s.review.pending.len(), 1);
        // sent again at the next sign-in: still one waiting
        s.update("Cheat", &update("Cheat", &[("Evt_Bst_30F07253", "58", "", 2)]), NOW);
        assert_eq!(s.review.pending.len(), 1);
        let listed = s.admin("pending", "admin:t", &json!({}), NOW).unwrap();
        assert_eq!(listed["rows"][0]["best"], json!(96.0));
        let id = listed["rows"][0]["id"].as_u64().unwrap();
        // rejected: gone, and not queued again when the game sends it again
        s.admin("reject", "admin:t", &json!({"id": id, "text": "no"}), NOW).unwrap();
        s.update("Cheat", &update("Cheat", &[("Evt_Bst_30F07253", "58", "", 2)]), NOW);
        assert!(s.review.pending.is_empty());
        // a real record that is far ahead: approved, it shows, with its time as it came
        s.update("Pro", &update("Pro", &[("Evt_Bst_30F07253", "80", "0x7b2cc40b,1,2,0x0", 2)]), NOW - 3_600);
        let id = s.review.pending[0].id;
        assert_eq!(s.admin("approve", "admin:t", &json!({"id": id}), NOW).unwrap()["value"], json!(80.0));
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 1, Period::All, 0, NOW)[0].name, "Pro");
        let boards = s.public_boards(&|_| true, true, NOW);
        let b = boards.iter().find(|b| b.id == "game:Evt_Bst:30F07253").unwrap();
        assert_eq!((b.all[0].name.as_str(), b.all[0].when, b.all[0].car.as_str()), ("Pro", NOW - 3_600, "0x7b2cc40b"));
        // the audit log has both decisions, newest first
        let audit = s.admin("audit", "admin:t", &json!({}), NOW).unwrap();
        assert_eq!(audit["total"], json!(2));
        assert_eq!(audit["rows"][0]["op"], json!("approve"));
        assert_eq!(audit["rows"][1]["actor"], json!("admin:t"));
        // scores: far above the best waits too
        for (i, who) in ["A", "B", "C", "D", "E"].iter().enumerate() {
            s.update(who, &update(who, &[("PK_Best_Cop_Score_258FBFCB", &format!("{}", 5000 + i), "", 1)]), NOW);
        }
        s.update("Cheat", &update("Cheat", &[("PK_Best_Cop_Score_258FBFCB", "30000", "", 1)]), NOW);
        assert_eq!(s.top("PK_Best_Cop_Score_258FBFCB", 1, 1, Period::All, 0, NOW)[0].name, "E");
    }

    #[test]
    fn banned_removed_and_unclaimed_names_leave_the_boards() {
        let mut s = Stats::open(None);
        for (who, t) in [("Metin", "103"), ("Jan", "104"), ("Guest", "105")] {
            s.update(who, &update(who, &[("Evt_Bst_30F07253", t, "", 2), ("Best_Drift_Combo", "5000", "", 1)]), NOW);
        }
        assert!(s.admin("ban", "admin:t", &json!({"name": "metin", "text": "fake times"}), NOW).is_ok());
        assert_eq!(names(&s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW)), ["Jan", "Guest"]);
        // only names that are claimed, and only the game's own boards when asked for
        let claimed = |n: &str| !n.eq_ignore_ascii_case("guest");
        assert!(s.public_boards(&claimed, false, NOW).is_empty());
        let boards = s.public_boards(&claimed, true, NOW);
        let b = boards.iter().find(|b| b.id == "game:Evt_Bst:30F07253").unwrap();
        assert_eq!(b.all.iter().map(|r| r.name.as_str()).collect::<Vec<_>>(), ["Jan"]);
        assert!(boards.iter().any(|b| b.id == "game:Best_Drift_Combo" && !b.low && b.unit == "points"));
        s.admin("unban", "admin:t", &json!({"name": "Metin"}), NOW).unwrap();
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 1, Period::All, 0, NOW)[0].name, "Metin");
        // an entry removed
        assert_eq!(s.admin("remove", "admin:t", &json!({"name": "Metin", "key": "Evt_Bst_30F07253"}), NOW).unwrap()["value"], json!(103.0));
        assert_eq!(names(&s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW)), ["Jan", "Guest"]);
        assert_eq!(s.admin("remove", "admin:t", &json!({"name": "Metin", "key": "Evt_Bst_30F07253"}), NOW), Err("key"));
        assert_eq!(s.admin("ban", "admin:t", &json!({"name": ""}), NOW), Err("name"));
        assert_eq!(s.admin("drop-table", "admin:t", &json!({}), NOW), Err("operation"));
        assert_eq!(s.admin("audit", "admin:t", &json!({}), NOW).unwrap()["total"], json!(3));
    }

    #[test]
    fn one_request_cannot_make_the_server_compare_thousands_of_board_keys() {
        // 5 owner entries (u.N) of 64 stats each: all of them the sender's own
        let mut s = Stats::open(None);
        let mut p = Packet::new(b"rank", 0x8000_0020).with("TXN", "UpdateStats").with("u.[]", 5);
        for u in 0..5 {
            p = p.with(&format!("u.{u}.o"), user_id("Spam")).with(&format!("u.{u}.ot"), 1).with(&format!("u.{u}.s.[]"), 64);
            for i in 0..64 {
                let f = |x: &str| format!("u.{u}.s.{i}.{x}");
                p = p.with(&f("k"), format!("Evt_Bst_{:08X}", u * 64 + i)).with(&f("v"), 100).with(&f("ut"), 2).with(&f("t"), "");
            }
        }
        let (kept, left) = s.update("Spam", &p, NOW);
        assert_eq!((kept, left), (BOARD_KEYS_PER_UPDATE, 5 * 64 - BOARD_KEYS_PER_UPDATE));
    }

    #[test]
    fn public_wins_and_streaks_count_from_the_reset_and_the_game_keeps_its_own() {
        let mut s = Stats::open(None);
        let both = racers(&["Metin", "Jan"]);
        // Metin wins, or (lose) Jan wins, at `at`
        let race = |s: &mut Stats, gid: u32, lose: bool, at: u64| {
            let r = if lose { results(&[(1, 2, true, 105.0, 72.0), (5, 1, true, 104.0, 72.0)]) } else { results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]) };
            s.race(RaceReport::Started { gid, players: both.clone() }, at);
            s.race(RaceReport::Ended { gid, settings: circuit(), players: both.clone(), results: vec![(0, r.clone()), (1, r)], clock: Vec::new() }, at);
        };
        let public = |s: &Stats, id: &str, period: &str| -> Vec<(String, f64)> {
            s.public_boards(&|_| true, false, NOW)
                .into_iter()
                .find(|b| b.id == id)
                .map(|b| (if period == "week" { b.week } else { b.all }).into_iter().map(|r| (r.name, r.value)).collect())
                .unwrap_or_default()
        };
        s.set_reset(NOW - 1_000);
        // three wins before the reset: the game's own count has them, the public one does not
        for gid in 0..3 {
            race(&mut s, gid, false, NOW - 5_000);
        }
        assert_eq!(value(&s, "Metin", "Overall_Solo_On_Wins"), 3.0);
        assert!(public(&s, "Overall_Solo_On_Wins", "all").is_empty());
        assert!(public(&s, "Best_Online_Win_Streak", "all").is_empty());
        // after it: win, win, lose, win
        for (gid, lose) in [(10, false), (11, false), (12, true), (13, false)] {
            race(&mut s, gid, lose, NOW);
        }
        assert_eq!(public(&s, "Overall_Solo_On_Wins", "all"), [("Metin".to_string(), 3.0), ("Jan".to_string(), 1.0)]);
        assert_eq!(public(&s, "Overall_Solo_On_Wins", "week"), [("Metin".to_string(), 3.0), ("Jan".to_string(), 1.0)]);
        assert_eq!(public(&s, "Best_Online_Win_Streak", "all")[0], ("Metin".to_string(), 2.0));
        // the game's own Player Stats are untouched: 6 wins, a best streak of 5 (3 + 2)
        assert_eq!(value(&s, "Metin", "Overall_Solo_On_Wins"), 6.0);
        assert_eq!(value(&s, "Metin", "Best_Online_Win_Streak"), 5.0);
        assert_eq!(value(&s, "Metin", "Current_Online_Win_Streak"), 1.0);
        // a later reset: what was counted before it leaves the board, and counting starts again
        s.set_reset(NOW + 10);
        assert!(public(&s, "Overall_Solo_On_Wins", "all").is_empty());
        race(&mut s, 20, false, NOW + 20);
        assert_eq!(public(&s, "Overall_Solo_On_Wins", "all"), [("Metin".to_string(), 1.0)]);
        assert_eq!(public(&s, "Best_Online_Win_Streak", "all"), [("Metin".to_string(), 1.0)]);
        assert_eq!(value(&s, "Metin", "Overall_Solo_On_Wins"), 7.0);
        // the old in-game keys themselves never reach the public boards
        assert!(s.public_boards(&|_| true, true, NOW).iter().all(|b| b.all.iter().all(|r| r.value != 7.0)));
    }

    #[test]
    fn public_wins_need_a_proven_claim_and_two_addresses() {
        let mut s = Stats::open(None);
        let mut both = racers(&["Guest", "Jan"]);
        both[0].owner = None;
        let r = results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]);
        race_of(&mut s, 1, &both, vec![(0, r.clone()), (1, r.clone())], Vec::new());
        let mut alts = racers(&["Main", "Alt"]);
        alts[1].client = alts[0].client.clone();
        race_of(&mut s, 2, &alts, vec![(0, r.clone()), (1, r)], Vec::new());
        assert_eq!((value(&s, "Guest", "Overall_Solo_On_Wins"), value(&s, "Main", "Overall_Solo_On_Wins")), (1.0, 1.0));
        assert!(s.public_boards(&|_| true, false, NOW).iter().all(|b| b.id != "Overall_Solo_On_Wins"));
    }

    #[test]
    fn the_game_cannot_send_the_servers_own_board_keys() {
        let mut s = Stats::open(None);
        let (kept, left) = s.update("Metin", &update("Metin", &[("Srv_Bst_30F07253", "20", "", 2), ("Srv_Lap_30F07253", "20", "", 2), ("Srv_Wins", "99", "reset:0", 3), ("Srv_Best_Streak", "99", "reset:0", 1), ("Overall_Solo_On_Wins", "99", "", 0)]), NOW);
        assert_eq!((kept, left), (0, 5));
        assert!(s.public_boards(&|_| true, true, NOW).is_empty());
    }

    #[test]
    fn when_a_record_was_set_survives_a_restart_and_old_files_still_load() {
        let dir = std::env::temp_dir().join(format!("carbon-stats-when-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        // a file from before: [value, text, low, periods] only
        fs::write(dir.join("stats.json"), r#"{"version":1,"people":{"old":{"name":"Old","key":null,"seen":1,"stats":{"Evt_Bst_30F07253":[99.0,"0x7B2CC40B,1,2,0x0",1,[]]}}}}"#).unwrap();
        {
            let mut s = Stats::open(Some(dir.clone()));
            s.update("New", &update("New", &[("Evt_Bst_30F07253", "98", "", 2)]), NOW);
            s.update("Next", &update("Next", &[("Evt_Bst_30F07253", "98.5", "", 2)]), NOW);
        }
        let s = Stats::open(Some(dir.clone()));
        let boards = s.public_boards(&|_| true, true, NOW);
        let b = &boards[0];
        assert_eq!(b.all.iter().map(|r| (r.name.as_str(), r.when)).collect::<Vec<_>>(), [("New", NOW), ("Next", NOW), ("Old", 0)]);
        assert_eq!(b.all[2].car, "0x7b2cc40b");
        let _ = fs::remove_dir_all(&dir);
    }

    /// Every in-game board and period has exactly the rows of top() (GetTopN) for its key.
    fn assert_ingame_is_top(s: &Stats, now: u64) -> usize {
        let mut checked = 0;
        for b in s.ingame_boards(now) {
            let rest = b.id.strip_prefix("ingame:").unwrap();
            let key = match rest.split_once(':') {
                Some((family, h)) => format!("{family}_{h}"),
                None => rest.to_string(),
            };
            for (period, rows) in [(Period::All, &b.all), (Period::Week, &b.week), (Period::Day, &b.day)] {
                // the public group leaves out the names the word filter refuses
                let top: Vec<Ranked> = s.board(&key, period, 0, now).into_iter().filter(|r| !crate::moderation::profane(r.1)).take(crate::leaderboard::MAX_ROWS).map(|(owner, name, value, text)| Ranked { owner, name: name.into(), rank: 0, value, text: text.into() }).collect();
                if !s.people.values().any(|p| p.stats.contains_key(&key) && crate::moderation::profane(&p.name)) {
                    assert_eq!(top, s.top(&key, 1, crate::leaderboard::MAX_ROWS, period, 0, now).into_iter().map(|r| Ranked { rank: 0, ..r }).collect::<Vec<_>>());
                }
                let want: Vec<(&str, f64, String)> = top.iter().map(|r| (r.name.as_str(), r.value, car_of(&r.text))).collect();
                let got: Vec<(&str, f64, String)> = rows.iter().map(|r| (r.name.as_str(), r.value, r.car.clone())).collect();
                assert_eq!(got, want, "{} {period:?}", b.id);
                checked += 1;
            }
            assert_eq!(b.players, s.board(&key, Period::All, 0, now).iter().filter(|r| !crate::moderation::profane(r.1)).count(), "{}", b.id);
        }
        checked
    }

    #[test]
    fn the_in_game_group_has_the_games_own_rows_for_every_name_but_banned_ones() {
        let mut s = Stats::open(None);
        // career and online bests from the games, a drift score (highest first), a zero score,
        // and the game-only keys that stay out of the group
        for (who, t, drift) in [("Metin", "103.5", "5000"), ("Jan", "99.25", "9000"), ("Guest", "120", "0"), ("Cheat", "80", "99999"), ("Fag", "95", "1")] {
            let text = "0x8774322C,51.500000,72.900000,0x00000000";
            s.update(who, &update(who, &[("Evt_Bst_30F07253", t, text, 2), ("Evt_Bst_Lap_30F07253", "51.5", "", 2), ("Evt_Bst_9C09A497", drift, "0x7B2CC40B,1.0,2.0,0x0", 1), ("CD_Best_Score_2656C6D0", "700", "", 1), ("Best_Drift_Combo", "5000", "", 1)]), NOW);
        }
        // the online records: counted by the race host
        let both = racers(&["Metin", "Jan"]);
        let r = results(&[(1, 1, true, 103.0, 72.0), (5, 2, true, 104.0, 72.0)]);
        race_of(&mut s, 1, &both, vec![(0, r.clone()), (1, r)], Vec::new());
        // and last week's time, which is in all time only
        s.update("Old", &update("Old", &[("Evt_Bst_30F07253", "101", "", 2)]), NOW - 8 * 86_400);
        s.admin("ban", "admin:t", &json!({"name": "cheat", "text": "fake"}), NOW).unwrap();
        assert!(assert_ingame_is_top(&s, NOW) > 0);
        let boards = s.ingame_boards(NOW);
        // the word filter's names are on the game's own board, not on the public page
        assert!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).iter().any(|r| r.name == "Fag"));
        assert!(boards.iter().all(|b| b.all.iter().chain(&b.week).chain(&b.day).all(|r| r.name != "Fag")));
        let ids: Vec<&str> = boards.iter().map(|b| b.id.as_str()).collect();
        for id in ["ingame:Evt_Bst:30F07253", "ingame:Evt_Bst_Lap:30F07253", "ingame:Evt_Bst:9C09A497", "ingame:Total_Rnkd_Evt_Games:30F07253", "ingame:Evt_Wins:30F07253", "ingame:Overall_Solo_On_Wins", "ingame:Best_Online_Win_Streak"] {
            assert!(ids.contains(&id), "{id} in {ids:?}");
        }
        // Canyon Duel, drift records and the server's own keys are not in it
        assert!(ids.iter().all(|i| !i.contains("CD_") && !i.contains("Best_Drift") && !i.contains("Srv_") && !i.contains("Overall_Solo_On_Losses")), "{ids:?}");
        let b = boards.iter().find(|b| b.id == "ingame:Evt_Bst:30F07253").unwrap();
        // every name, claimed or not (the in-game board has no claims), not the banned one
        let rows = |v: &[crate::leaderboard::Row]| v.iter().map(|r| r.name.clone()).collect::<Vec<_>>();
        assert_eq!(rows(&b.all), ["Jan", "Old", "Metin", "Guest"]);
        assert_eq!(rows(&b.week), ["Jan", "Metin", "Guest"]);
        assert_eq!(rows(&b.day), ["Jan", "Metin", "Guest"]);
        assert_eq!((b.source, b.low, b.unit, b.players, b.laps), ("ingame", true, "time", 4, None));
        // the race host's time (103.0) replaced Metin's 103.5: the game's own board has it too
        assert_eq!(b.all[2].value, 103.0);
        assert_eq!(b.all[0].numbers, Some((51.5, 72.9)));
        assert_eq!(b.all[0].car, "0x8774322c");
        assert_eq!(b.all[0].when, NOW);
        assert_eq!(b.all[1].when, NOW - 8 * 86_400);
        // drift: highest first, the zero shows as the game shows it
        let d = boards.iter().find(|b| b.id == "ingame:Evt_Bst:9C09A497").unwrap();
        assert_eq!((d.low, d.unit), (false, "points"));
        assert_eq!(d.all.iter().map(|r| (r.name.as_str(), r.value)).collect::<Vec<_>>(), [("Jan", 9000.0), ("Metin", 5000.0), ("Guest", 0.0)]);
        let w = boards.iter().find(|b| b.id == "ingame:Overall_Solo_On_Wins").unwrap();
        assert_eq!((w.unit, w.all.len(), w.week.len(), w.all[0].name.as_str()), ("count", 1, 0, "Metin"));
        assert_eq!(boards.iter().find(|b| b.id == "ingame:Total_Rnkd_Evt_Games:30F07253").unwrap().all.len(), 2);
        // unbanned: back on the in-game group at once
        s.admin("unban", "admin:t", &json!({"name": "Cheat"}), NOW).unwrap();
        assert_eq!(s.ingame_boards(NOW).iter().find(|b| b.id == "ingame:Evt_Bst:30F07253").unwrap().all[0].name, "Cheat");
        assert_ingame_is_top(&s, NOW);
        // a board only a banned name holds has no rows: it is left out
        let mut s = Stats::open(None);
        s.update("Solo", &update("Solo", &[("Evt_Bst_7335A1D3", "90", "", 2)]), NOW);
        s.admin("ban", "admin:t", &json!({"name": "Solo"}), NOW).unwrap();
        assert!(s.ingame_boards(NOW).is_empty());
        assert!(s.top("Evt_Bst_7335A1D3", 1, 10, Period::All, 0, NOW).is_empty());
    }

    #[test]
    fn a_rejected_value_never_reaches_the_in_game_group() {
        let mut s = Stats::open(None);
        for (i, who) in ["A", "B", "C", "D", "E"].iter().enumerate() {
            s.update(who, &update(who, &[("Evt_Bst_30F07253", &format!("{}", 100 + i), "", 2)]), NOW);
        }
        s.update("Cheat", &update("Cheat", &[("Evt_Bst_30F07253", "50", "", 2)]), NOW);
        let id = s.review.pending[0].id;
        s.admin("reject", "admin:t", &json!({"id": id}), NOW).unwrap();
        s.update("Cheat", &update("Cheat", &[("Evt_Bst_30F07253", "50", "", 2)]), NOW);
        let b = s.ingame_boards(NOW);
        assert!(b[0].all.iter().all(|r| r.name != "Cheat"));
        assert_ingame_is_top(&s, NOW);
    }

    /// The real stats copy: CARBON_STATS_COPY=<a folder with stats.json> cargo test --release --
    /// --ignored real_stats --nocapture. Read only (nothing is changed, so nothing is written).
    #[test]
    #[ignore]
    fn real_stats_in_game_group_is_top_and_its_size() {
        let dir = PathBuf::from(std::env::var("CARBON_STATS_COPY").expect("CARBON_STATS_COPY"));
        let s = Stats::open(Some(dir));
        let now: u64 = std::env::var("CARBON_STATS_NOW").ok().and_then(|v| v.parse().ok()).unwrap_or_else(|| std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs());
        let t = std::time::Instant::now();
        let boards = s.ingame_boards(now);
        let built = t.elapsed();
        let week = week_number(now);
        let t = std::time::Instant::now();
        let text = crate::leaderboard::answer(&boards, now, week, None, None, crate::leaderboard::DEFAULT_ROWS, Some("ingame")).to_string();
        let made = t.elapsed();
        let full = crate::leaderboard::answer(&boards, now, week, None, None, crate::leaderboard::MAX_ROWS, Some("ingame")).to_string();
        let t = std::time::Instant::now();
        let race = s.public_boards(&|_| false, false, now);
        let race_built = t.elapsed();
        let rows: usize = boards.iter().map(|b| b.all.len() + b.week.len() + b.day.len()).sum();
        let filtered = s.people.values().filter(|p| p.stats.keys().any(|k| ingame_spec(k).is_some()) && crate::moderation::profane(&p.name)).count();
        println!("names on in-game boards left out for the word filter: {filtered}");
        let t = std::time::Instant::now();
        let checked = assert_ingame_is_top(&s, now);
        println!(
            "players {} | in-game boards {} (rows {rows}) built in {:.1} ms, answer text {:.1} ms | plain ?source=ingame {} bytes, at 100 rows {} bytes | race group (no claims) {} boards in {:.1} ms | {checked} board-periods equal top() ({:.0} ms)",
            s.len(), boards.len(), built.as_secs_f64() * 1e3, made.as_secs_f64() * 1e3, text.len(), full.len(), race.len(), race_built.as_secs_f64() * 1e3, t.elapsed().as_secs_f64() * 1e3
        );
        let mut by: BTreeMap<&str, (usize, usize)> = BTreeMap::new();
        for b in &boards {
            let e = by.entry(b.family.as_str()).or_default();
            e.0 += 1;
            e.1 = e.1.max(b.players);
        }
        println!("families (boards, most players): {by:?}");
        if let Ok(out) = std::env::var("CARBON_STATS_ANSWER") {
            fs::write(out, &text).unwrap();
        }
    }
}
