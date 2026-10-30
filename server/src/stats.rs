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

use serde_json::{json, Map, Value};

use crate::ea::{user_id, Packet};
use crate::messenger::KeyDigest;

/// Players kept at most; past that a new name's stats are counted for this run only.
pub const MAX_PEOPLE: usize = 10_000;
/// Stats kept per player.
const MAX_KEYS: usize = 256;
/// A stat's text (the leaderboard's car, best lap and top speed) at most.
const MAX_TEXT: usize = 64;
/// Rows a leaderboard request may ask for at once.
const MAX_ROWS: usize = 100;

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
    ["Evt_", "Total_Rnkd_Evt_Games_", "Canyon_", "PK_Best_", "CD_"].iter().any(|p| key.starts_with(p))
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
    key.starts_with("Overall_")
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
        Stat { v: 0.0, t: String::new(), low, p: Vec::new() }
    }

    /// Applies an update at `now` (seconds); for leaderboard keys the periods too.
    fn update(&mut self, key: &str, how: Update, v: f64, t: &str, fresh: bool, now: u64) {
        let (nv, better) = apply((!fresh).then_some(self.v), how, v);
        self.v = nv;
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
                    if better && (how != Update::Add || !t.is_empty()) {
                        e.3 = t.to_string();
                    }
                }
                None => self.p.push((kind, n, v, t.to_string())),
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

#[derive(Clone, Debug, PartialEq)]
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
#[derive(Clone, Debug, PartialEq)]
pub struct Racer {
    pub slot: u32,
    pub name: String,
    pub car: u32,
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
    /// still in the game, and the GameResults each of their games sent (by slot).
    Ended { gid: u32, settings: Vec<(&'static str, String)>, players: Vec<Racer>, results: Vec<(u32, Vec<u8>)> },
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
        let mut s = Stats::default();
        let Some(dir) = dir else { return s };
        if let Err(e) = fs::create_dir_all(&dir) {
            eprintln!("stats: cannot use {}: {e}; player stats are kept for this run only", dir.display());
            return s;
        }
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

    fn to_json(&self) -> Value {
        let mut people = Map::new();
        for (k, p) in self.people.iter().filter(|(_, p)| !p.stats.is_empty()) {
            let mut stats = Map::new();
            for (key, s) in &p.stats {
                let periods: Vec<Value> = s.p.iter().map(|(kind, n, v, t)| json!([kind.code(), n, v, t])).collect();
                stats.insert(key.clone(), json!([s.v, s.t, u8::from(s.low), periods]));
            }
            people.insert(k.clone(), json!({ "name": p.name, "key": p.key, "seen": p.seen, "stats": stats }));
        }
        json!({ "version": 1, "people": people })
    }

    /// Written at once, whole, through a temporary file (a crash leaves the old one).
    fn save(&self) {
        let Some(file) = &self.file else { return };
        let tmp = file.with_extension("json.tmp");
        let text = serde_json::to_string(&self.to_json()).unwrap_or_default();
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
        if !v.is_finite() || key.is_empty() || key.len() > 64 {
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
        let (mut kept, mut left) = (0, 0);
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
                if kept_from_game(key) && self.put(name, key, how, v, field("t"), now) {
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
                let (v, t) = s.at(period, past, now)?;
                Some((user_id(&p.name), p.name.as_str(), v, t))
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

    /// The race host's word on a race: counted as EA's servers counted a ranked race. Returns
    /// lines for stdout.
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
            RaceReport::Ended { gid, settings, players, results } => {
                let Some(started) = self.racing.remove(&gid) else { return Vec::new() };
                let setting = |name: &str| settings.iter().find(|(k, _)| *k == name).map(|(_, v)| v.clone()).unwrap_or_default();
                let mode = setting("game_mode").parse::<u32>().ok();
                let race = mode.and_then(crate::olmsg::race_attribute).map(setting).filter(|r| !r.is_empty() && r != "ABSTAIN");
                // Each racer's row as the lowest slot's game saw it: the report the host sent
                // everyone as FinalGameResults, so the board shows the times STANDINGS showed (the
                // games' own reports differ by hundredths); else from another game's report.
                let mut reports: Vec<&(u32, Vec<u8>)> = results.iter().collect();
                reports.sort_by_key(|r| r.0);
                let row_of = |slot: u32| -> Option<Row> {
                    let id = 1 + 4 * slot;
                    reports.iter().find_map(|r| game_results_rows(&r.1).into_iter().find(|x| x.racer == id))
                };
                let mut lines = Vec::new();
                let mut outcome: Vec<(&Racer, Option<Row>, bool)> = Vec::new();
                for r in &started {
                    let still = players.iter().any(|p| p.name.eq_ignore_ascii_case(&r.name));
                    let row = if still { row_of(r.slot) } else { None };
                    outcome.push((r, row, still));
                }
                let winner = outcome.iter().filter(|(_, row, _)| row.is_some_and(|w| w.place == 1)).map(|(r, _, _)| r.name.clone()).next();
                let key = race.as_deref().map(attrib_key);
                for (r, row, still) in &outcome {
                    let won = winner.as_deref() == Some(r.name.as_str());
                    // a racer that left, or whose game says it did not finish a race to a line
                    let dnf = !still || row.is_none() || (matches!(mode, Some(0 | 1 | 5)) && !row.is_some_and(|w| w.finished));
                    self.put(&r.name, "Total_Online_Games", Update::Add, 1.0, "", now);
                    if !dnf {
                        self.put(&r.name, "Total_Online_Games_Finished", Update::Add, 1.0, "", now);
                    }
                    self.dnf(&r.name, now);
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
                                let top = f64::from(w.top_speed.max(0.0));
                                let (a, b) = if m == 1 { (f64::from(w.best_lap.max(0.0)), top) } else { (top, 0.0) };
                                let text = format!("0x{:08X},{a:.6},{b:.6},0x{:08X}", r.car, 0);
                                self.put(&r.name, &format!("Evt_Bst_{h:08X}"), Update::Min, f64::from(w.time), &text, now);
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
        names.iter().enumerate().map(|(i, n)| Racer { slot: i as u32, name: n.to_string(), car: attrib_key("r8") }).collect()
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
            let lines = s.race(RaceReport::Ended { gid: 200_000, settings: circuit(), players: both.clone(), results: vec![(0, r.clone()), (1, r.clone())] }, NOW);
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
        s.race(RaceReport::Ended { gid: 1, settings: circuit(), players: racers(&["Metin"]), results: vec![(0, r)] }, NOW);
        let keys: Vec<String> = ["Overall_Solo_On_Wins", "Overall_Solo_On_Losses", "Total_Online_Games_Started", "Total_Online_Games_Finished", "DNF"].iter().map(|k| k.to_string()).collect();
        let v = |s: &Stats, who: &str| s.get(user_id(who), &keys, Period::All, 0, NOW).into_iter().map(|x| x.1).collect::<Vec<_>>();
        assert_eq!(v(&s, "Metin"), [1.0, 0.0, 1.0, 1.0, 0.0]);
        assert_eq!(v(&s, "Jan"), [0.0, 1.0, 1.0, 0.0, 1.0]);
        assert_eq!(s.top("Evt_Bst_30F07253", 1, 10, Period::All, 0, NOW).len(), 1);
        // alone in a room: nothing counts
        let lines = s.race(RaceReport::Started { gid: 2, players: racers(&["Ada"]) }, NOW);
        assert!(lines[0].contains("one player"));
        assert!(s.race(RaceReport::Ended { gid: 2, settings: circuit(), players: racers(&["Ada"]), results: Vec::new() }, NOW).is_empty());
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
            s.race(RaceReport::Ended { gid: 1, settings: circuit(), players: racers(&["Metin", "Jan"]), results: vec![(0, results(&[(1, 1, true, 100.0, 70.0), (5, 2, true, 101.0, 70.0)]))] }, NOW);
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
}
