//! The public leaderboards (GET /leaderboard, for nfscrb.xyz/leaderboards) and what keeps fake
//! results off them: the review queue, boards bans and the admin's audit log (Review), the rate
//! limits for result submissions (Limits), and the admin API (/leaderboard/admin).
//!
//! Where a public number comes from (stats.rs has the details):
//! - "race" boards are counted by this server's race host from a race of two or more players,
//!   and only when the other players' games report the same finish (stats.rs cross_check): the
//!   best time and best lap per race (kept as Srv_Bst_<race>[_L<laps>] and Srv_Lap_<race>), and
//!   the online wins and best win streak (Srv_Wins, Srv_Best_Streak), counted only from
//!   CARBON_LEADERBOARD_RESET on: each count carries the reset it was counted under, and one
//!   from an earlier reset neither shows nor adds up. The game's own Overall_Solo_On_Wins and
//!   Best_Online_Win_Streak (its Player Stats screens) are never public. A player's own game
//!   alone never puts a time there, and only names that are claimed (identity.rs) and proven
//!   by the racing browser are recorded.
//! - "game" boards are what a player's own game sends with UpdateStats (career and quick race
//!   bests, Canyon Duel, drift and pursuit records). Nobody else saw those races; a modified page
//!   can send any number. They are left out unless CARBON_LEADERBOARD_GAME=1.
//! - "ingame" boards ("In-game leaderboard", only with ?source=ingame or ?source=all; the plain
//!   answer is as before) are exactly what the game's own LEADERBOARD and ONLINE PLAYER STATS
//!   screens show (stats.rs Stats::ingame_boards, the rows of GetTopN): per race the best
//!   (Evt_Bst_<race>, whatever the lap count), the best lap, the races and the wins, and the
//!   game's own online wins and best win streak (counted before the reset, and before the games'
//!   reports were cross-checked); every name, claimed or not, except names banned from the
//!   boards. The page labels them as the game's records, not verified ones. All time, this week
//!   and today. CARBON_LEADERBOARD_INGAME=0 leaves them out.
//!
//! On every board: a result much better than the board's established top waits in the review
//! queue (stats.rs Stats::record) until an admin approves it; names banned from the boards and
//! unclaimed names are not shown. The answer is rebuilt at most every CARBON_LEADERBOARD_SECS
//! (default 60) and has display names, values, car keys and times only.
//!
//! Admin: GET or POST /leaderboard/admin with `Authorization: Bearer <the admin's cloud-save
//! id>` (its owner digest must be in CARBON_ADMIN_OWNERS), and ?key= when CARBON_RELAY_KEY is
//! set. Operations: pending, approve {id}, reject {id, text}, remove {name, key}, ban {name,
//! text}, unban {name}, banned, audit. Every change is kept in the audit log (leaderboard.json).

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Instant;

use serde_json::{json, Value};

use crate::http::{client, Conn, Request, Url};
use crate::stats::Update;
use crate::util::{lock, same_bytes};
use crate::App;

/// Results waiting for an admin, in all and per name; past that the oldest goes (all) or the
/// name's new one is dropped.
pub const MAX_PENDING: usize = 500;
pub const MAX_PENDING_PER_NAME: usize = 10;
const MAX_AUDIT: usize = 10_000;
const MAX_REJECTED: usize = 5_000;
const MAX_BANNED: usize = 10_000;
/// Rows per board kept in the public answer at most, and the default asked for.
pub const MAX_ROWS: usize = 100;
pub const DEFAULT_ROWS: usize = 20;

// ---------------------------------------------------------------------------------------
// Rate limits

/// A token bucket: `burst` at once, then `rate` a second.
#[derive(Clone, Copy, Debug)]
struct Bucket {
    tokens: f64,
    at: Instant,
}

/// What a limit is for, with its burst and rate a second.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Kind {
    /// UpdateStats from one client address (X-Real-IP; all its browsers together)
    UpdateFromClient,
    /// UpdateStats for one name
    UpdateForName,
    /// race results recorded on the public boards for one name
    ResultForName,
    /// the public GET /leaderboard from one client address
    Read,
    /// /leaderboard/admin from one client address
    Admin,
}

impl Kind {
    fn limits(self) -> (f64, f64) {
        match self {
            // The game sends 7 UpdateStats in its first 1.4 s online (its career; runs e2-e4,
            // r1, r2) and none after, races included. One address may have 8 browsers (lan.rs
            // PER_CLIENT): 60 at once, then one every 10 s; a name 20, then one every 30 s.
            Kind::UpdateFromClient => (60.0, 1.0 / 10.0),
            Kind::UpdateForName => (20.0, 1.0 / 30.0),
            // A race takes more than a minute with its lobby: 20 at once, one a minute after.
            Kind::ResultForName => (20.0, 1.0 / 60.0),
            Kind::Read => (60.0, 2.0),
            Kind::Admin => (30.0, 0.5),
        }
    }
}

/// Buckets per (kind, who). Full buckets are forgotten when there are too many.
#[derive(Default)]
pub struct Limits {
    buckets: HashMap<(Kind, String), Bucket>,
}

const MAX_BUCKETS: usize = 20_000;

impl Limits {
    pub fn allow(&mut self, kind: Kind, who: &str) -> bool {
        self.allow_at(kind, who, Instant::now())
    }

    pub fn allow_at(&mut self, kind: Kind, who: &str, now: Instant) -> bool {
        let (burst, rate) = kind.limits();
        if self.buckets.len() >= MAX_BUCKETS {
            self.buckets.retain(|(k, _), b| {
                let (burst, rate) = k.limits();
                b.tokens + now.saturating_duration_since(b.at).as_secs_f64() * rate < burst
            });
        }
        let b = self.buckets.entry((kind, who.to_ascii_lowercase())).or_insert(Bucket { tokens: burst, at: now });
        b.tokens = (b.tokens + now.saturating_duration_since(b.at).as_secs_f64() * rate).min(burst);
        b.at = now;
        if b.tokens < 1.0 {
            return false;
        }
        b.tokens -= 1.0;
        true
    }
}

// ---------------------------------------------------------------------------------------
// The review queue, boards bans and the audit log

/// A result held back from the boards until an admin decides.
#[derive(Clone, Debug, PartialEq)]
pub struct Pending {
    pub id: u64,
    pub name: String,
    pub key: String,
    pub how: Update,
    pub value: f64,
    pub text: String,
    /// when it came (seconds since 1970)
    pub at: u64,
    /// "race" (the race host) or "game" (the player's own UpdateStats)
    pub source: String,
    /// why it waits
    pub why: String,
}

fn ut(how: Update) -> u8 {
    match how {
        Update::Set => 0,
        Update::Max => 1,
        Update::Min => 2,
        Update::Add => 3,
    }
}

#[derive(Default)]
pub struct Review {
    pub pending: Vec<Pending>,
    /// lowercase name -> (the name as shown, when, why)
    pub banned: BTreeMap<String, (String, u64, String)>,
    /// results an admin rejected, so the game sending them again at every sign-in does not
    /// queue them again: (lowercase name, key, value in thousandths)
    rejected: VecDeque<(String, String, i64)>,
    audit: VecDeque<Value>,
    next: u64,
    file: Option<PathBuf>,
}

fn milli(v: f64) -> i64 {
    (v * 1000.0).round() as i64
}

impl Review {
    /// The queue kept in `dir`/leaderboard.json, or in memory only.
    pub fn open(dir: Option<&PathBuf>) -> Review {
        let mut r = Review { next: 1, ..Review::default() };
        let Some(dir) = dir else { return r };
        let file = dir.join("leaderboard.json");
        match fs::read_to_string(&file) {
            Ok(text) => match serde_json::from_str::<Value>(&text) {
                Ok(v) => r.load(&v),
                Err(e) => eprintln!("leaderboard: {} is not readable ({e}); starting with an empty review queue", file.display()),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => eprintln!("leaderboard: cannot read {}: {e}", file.display()),
        }
        r.file = Some(file);
        r
    }

    fn load(&mut self, v: &Value) {
        let s = |x: &Value, k: &str| x.get(k).and_then(Value::as_str).unwrap_or("").to_string();
        let n = |x: &Value, k: &str| x.get(k).and_then(Value::as_u64).unwrap_or(0);
        for p in v.get("pending").and_then(Value::as_array).into_iter().flatten().take(MAX_PENDING) {
            let (Some(value), name, key) = (p.get("value").and_then(Value::as_f64), s(p, "name"), s(p, "key")) else { continue };
            if name.is_empty() || key.is_empty() || key.len() > 64 {
                continue;
            }
            self.pending.push(Pending {
                id: n(p, "id"),
                name,
                key,
                how: Update::from_ut(n(p, "ut") as i64),
                value,
                text: s(p, "text"),
                at: n(p, "at"),
                source: s(p, "source"),
                why: s(p, "why"),
            });
        }
        for (k, b) in v.get("banned").and_then(Value::as_object).into_iter().flatten().take(MAX_BANNED) {
            self.banned.insert(k.to_ascii_lowercase(), (s(b, "name"), n(b, "at"), s(b, "why")));
        }
        for r in v.get("rejected").and_then(Value::as_array).into_iter().flatten().take(MAX_REJECTED) {
            if let (Some(name), Some(key), Some(m)) = (r.get(0).and_then(Value::as_str), r.get(1).and_then(Value::as_str), r.get(2).and_then(Value::as_i64)) {
                self.rejected.push_back((name.to_string(), key.to_string(), m));
            }
        }
        for a in v.get("audit").and_then(Value::as_array).into_iter().flatten().take(MAX_AUDIT) {
            self.audit.push_back(a.clone());
        }
        let top = self.pending.iter().map(|p| p.id).chain(self.audit.iter().filter_map(|a| a.get("id").and_then(Value::as_u64))).max().unwrap_or(0);
        self.next = n(v, "next").max(top + 1);
    }

    /// Written whole through a temporary file. False when it could not be written.
    pub fn save(&self) -> bool {
        let Some(file) = &self.file else { return true };
        let pending: Vec<Value> = self
            .pending
            .iter()
            .map(|p| json!({"id": p.id, "name": p.name, "key": p.key, "ut": ut(p.how), "value": p.value, "text": p.text, "at": p.at, "source": p.source, "why": p.why}))
            .collect();
        let banned: serde_json::Map<String, Value> = self.banned.iter().map(|(k, (n, at, why))| (k.clone(), json!({"name": n, "at": at, "why": why}))).collect();
        let rejected: Vec<Value> = self.rejected.iter().map(|(n, k, m)| json!([n, k, m])).collect();
        let text = json!({"version": 1, "next": self.next, "pending": pending, "banned": banned, "rejected": rejected, "audit": self.audit}).to_string();
        let tmp = file.with_extension("json.tmp");
        let result = (|| {
            let mut options = fs::OpenOptions::new();
            options.create(true).truncate(true).write(true);
            #[cfg(unix)]
            std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
            let mut out = options.open(&tmp)?;
            out.write_all(text.as_bytes())?;
            fs::rename(&tmp, file)
        })();
        if let Err(e) = result {
            eprintln!("leaderboard: cannot write {}: {e}", file.display());
            return false;
        }
        true
    }

    pub fn is_banned(&self, name: &str) -> bool {
        !self.banned.is_empty() && self.banned.contains_key(&name.to_ascii_lowercase())
    }

    pub fn was_rejected(&self, name: &str, key: &str, v: f64) -> bool {
        let (n, m) = (name.to_ascii_lowercase(), milli(v));
        self.rejected.iter().any(|(a, b, c)| *a == n && b == key && *c == m)
    }

    /// Holds a result. A name's second result for the same board replaces the first when it is
    /// better (it is the one that would show). False when the name has too many waiting.
    pub fn hold(&mut self, mut p: Pending, low: bool) -> bool {
        if let Some(old) = self.pending.iter_mut().find(|o| o.name.eq_ignore_ascii_case(&p.name) && o.key == p.key) {
            let better = if low { p.value < old.value } else { p.value > old.value };
            if better {
                p.id = old.id;
                *old = p;
            }
            return true;
        }
        if self.pending.iter().filter(|o| o.name.eq_ignore_ascii_case(&p.name)).count() >= MAX_PENDING_PER_NAME {
            return false;
        }
        if self.pending.len() >= MAX_PENDING {
            self.pending.remove(0);
        }
        self.next = self.next.max(1);
        p.id = self.next;
        self.next += 1;
        self.pending.push(p);
        true
    }

    pub fn take(&mut self, id: u64) -> Option<Pending> {
        let i = self.pending.iter().position(|p| p.id == id)?;
        Some(self.pending.remove(i))
    }

    pub fn reject(&mut self, p: &Pending) {
        if self.rejected.len() >= MAX_REJECTED {
            self.rejected.pop_front();
        }
        self.rejected.push_back((p.name.to_ascii_lowercase(), p.key.clone(), milli(p.value)));
    }

    pub fn ban(&mut self, name: &str, why: &str, now: u64) -> bool {
        if self.banned.len() >= MAX_BANNED && !self.is_banned(name) {
            return false;
        }
        self.banned.insert(name.to_ascii_lowercase(), (name.to_string(), now, why.to_string()));
        true
    }

    pub fn unban(&mut self, name: &str) -> bool {
        self.banned.remove(&name.to_ascii_lowercase()).is_some()
    }

    /// One admin action in the audit log, with a new id.
    #[allow(clippy::too_many_arguments)]
    pub fn log(&mut self, op: &str, actor: &str, name: &str, key: &str, value: Option<f64>, text: &str, now: u64) -> u64 {
        self.next = self.next.max(1);
        let id = self.next;
        self.next += 1;
        if self.audit.len() >= MAX_AUDIT {
            self.audit.pop_front();
        }
        self.audit.push_back(json!({"id": id, "at": now, "op": op, "actor": actor, "name": name, "key": key, "value": value, "text": text}));
        println!("leaderboard: admin {actor} {op} {name} {key} {}", value.map_or(String::new(), crate::stats::num));
        id
    }

    pub fn audit(&self, offset: usize, limit: usize) -> (usize, Vec<Value>) {
        (self.audit.len(), self.audit.iter().rev().skip(offset).take(limit).cloned().collect())
    }
}

// ---------------------------------------------------------------------------------------
// The public answer

/// One board of the public answer.
#[derive(Clone, Debug, PartialEq)]
pub struct Board {
    /// "Evt_Bst:30F07253", "Evt_Bst:30F07253:2" (a circuit of 2 laps), "Evt_Bst_Lap:30F07253",
    /// "Overall_Solo_On_Wins"; game boards start "game:", in-game boards "ingame:".
    pub id: String,
    pub family: String,
    /// "race" (counted by the race host), "game" (the player's own game said so) or "ingame"
    /// (what the game's own leaderboard shows)
    pub source: &'static str,
    /// the race's Attrib key in hex (the page names it from its own data), or None
    pub race: Option<String>,
    pub laps: Option<u32>,
    /// lowest wins (a time)
    pub low: bool,
    pub unit: &'static str,
    /// names with an all-time value
    pub players: usize,
    pub all: Vec<Row>,
    pub week: Vec<Row>,
    /// today (UTC); in-game boards only
    pub day: Vec<Row>,
}

/// The label of the in-game group, for the page.
pub const INGAME_LABEL: &str = "In-game leaderboard";

#[derive(Clone, Debug, PartialEq)]
pub struct Row {
    pub name: String,
    pub value: f64,
    /// the car's Attrib key ("0x8774322c"), or ""
    pub car: String,
    /// when it was set (seconds since 1970), 0 when not known (kept before times were kept)
    pub when: u64,
    /// in-game best times: the two numbers the game's board shows by race type (a circuit's
    /// best lap and top speed in m/s, a sprint's top speed and NOS), as elements 4 and 5
    pub numbers: Option<(f64, f64)>,
}

fn rows_json(rows: &[Row], limit: usize) -> Value {
    Value::Array(
        rows.iter()
            .take(limit)
            .map(|r| {
                let mut row = vec![json!(r.name), json!((r.value * 1000.0).round() / 1000.0), json!(r.car), if r.when > 0 { json!(r.when) } else { Value::Null }];
                if let Some((a, b)) = r.numbers {
                    row.extend([json!((a * 1000.0).round() / 1000.0), json!((b * 1000.0).round() / 1000.0)]);
                }
                Value::Array(row)
            })
            .collect(),
    )
}

/// Which boards an answer has: `source` None is the plain answer as before (race boards, and
/// game boards when on); "ingame" only the in-game group; "all" everything. A board asked for by
/// id is found in any group.
fn wanted(b: &Board, board: Option<&str>, source: Option<&str>) -> bool {
    match (board, source) {
        (Some(id), _) => b.id == id,
        (None, Some("all")) => true,
        (None, Some("ingame")) => b.source == "ingame",
        (None, _) => b.source != "ingame",
    }
}

/// The JSON for `boards`: every board of the group, or the one asked for; one period or all.
/// In-game boards also have "day", and their best times two more numbers per row.
#[allow(clippy::too_many_arguments)]
pub fn answer(boards: &[Board], made: u64, week: u32, board: Option<&str>, period: Option<&str>, limit: usize, source: Option<&str>) -> Value {
    let limit = limit.clamp(1, MAX_ROWS);
    let chosen: Vec<&Board> = boards.iter().filter(|b| wanted(b, board, source)).collect();
    let list: Vec<Value> = chosen
        .iter()
        .map(|b| {
            let mut o = json!({
                "id": b.id, "family": b.family, "source": b.source, "race": b.race, "laps": b.laps,
                "low": b.low, "unit": b.unit, "players": b.players,
            });
            if b.source == "ingame" {
                o["label"] = json!(INGAME_LABEL);
            }
            if matches!(period, None | Some("all")) {
                o["all"] = rows_json(&b.all, limit);
            }
            if matches!(period, None | Some("week")) {
                o["week"] = rows_json(&b.week, limit);
            }
            if b.source == "ingame" && matches!(period, None | Some("day")) {
                o["day"] = rows_json(&b.day, limit);
            }
            o
        })
        .collect();
    let mut families: BTreeMap<(&str, &str), usize> = BTreeMap::new();
    for b in boards.iter().filter(|b| wanted(b, None, source)) {
        *families.entry((b.family.as_str(), b.source)).or_insert(0) += 1;
    }
    let families: Vec<Value> = families.into_iter().map(|((f, source), n)| json!({"family": f, "boards": n, "source": source})).collect();
    let mut v = json!({"version": 1, "made": made, "week": week, "rows": ["name", "value", "car", "when"], "families": families, "boards": list});
    if chosen.iter().any(|b| b.source == "ingame") {
        v["ingame"] = json!({"label": INGAME_LABEL, "rows": ["name", "value", "car", "when", "a", "b"], "day": made / 86_400});
    }
    v
}

/// When the boards were made (our clock, and seconds since 1970), their week, the boards, and
/// the plain answer's text (of the race group, or of ?source=ingame).
type Cached = (Instant, u64, u32, Vec<Board>, String);

/// The answer, rebuilt at most every `every` seconds: the race (and game) boards and the in-game
/// boards apart, each only when asked for.
pub struct Public {
    every: u64,
    game_boards: bool,
    ingame_boards: bool,
    cached: Mutex<Option<Cached>>,
    ingame: Mutex<Option<Cached>>,
    limits: Mutex<Limits>,
}

impl Public {
    pub fn from_env() -> Public {
        let every = std::env::var("CARBON_LEADERBOARD_SECS").ok().and_then(|v| v.parse().ok()).unwrap_or(60u64).clamp(1, 3600);
        let game_boards = std::env::var("CARBON_LEADERBOARD_GAME").is_ok_and(|v| v == "1");
        let mut p = Public::new(every, game_boards);
        p.ingame_boards = !std::env::var("CARBON_LEADERBOARD_INGAME").is_ok_and(|v| v == "0");
        p
    }

    pub fn new(every: u64, game_boards: bool) -> Public {
        Public { every, game_boards, ingame_boards: true, cached: Mutex::new(None), ingame: Mutex::new(None), limits: Mutex::new(Limits::default()) }
    }

    /// After an admin change: the next answer is made again.
    fn clear(&self) {
        *lock(&self.cached) = None;
        *lock(&self.ingame) = None;
    }
}

fn unix_secs() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

/// CORS: nfscrb.xyz (and www.) over https, and localhost / 127.0.0.1 on any port.
pub fn allowed_origin(origin: &str) -> bool {
    if matches!(origin, "https://nfscrb.xyz" | "https://www.nfscrb.xyz") {
        return true;
    }
    for base in ["http://localhost", "http://127.0.0.1", "https://localhost"] {
        if let Some(rest) = origin.strip_prefix(base) {
            if rest.is_empty() || rest.strip_prefix(':').is_some_and(|p| !p.is_empty() && p.len() <= 5 && p.bytes().all(|b| b.is_ascii_digit())) {
                return true;
            }
        }
    }
    false
}

/// GET /leaderboard and /leaderboard/admin.
pub async fn handle(app: &App, conn: &mut Conn, req: &mut Request, url: &Url) -> std::io::Result<()> {
    let origin = req.header("origin").filter(|o| allowed_origin(o)).map(str::to_string);
    let mut headers: Vec<(&str, &str)> = vec![("content-type", "application/json"), ("vary", "Origin"), ("cross-origin-resource-policy", "cross-origin")];
    if let Some(o) = &origin {
        headers.push(("access-control-allow-origin", o.as_str()));
    }
    if url.path == "/leaderboard/admin" {
        return admin(app, conn, req, url, headers).await;
    }
    if url.path != "/leaderboard" {
        return conn.reply(req, 404, &headers, br#"{"error":"not-found"}"#).await;
    }
    if req.method == "OPTIONS" {
        headers.extend_from_slice(&[("access-control-allow-methods", "GET, OPTIONS"), ("access-control-max-age", "600")]);
        return conn.reply(req, 204, &headers, b"").await;
    }
    if req.method != "GET" && req.method != "HEAD" {
        headers.push(("allow", "GET, OPTIONS"));
        return conn.reply(req, 405, &headers, br#"{"error":"method"}"#).await;
    }
    if !lock(&app.boards.limits).allow(Kind::Read, &client(conn, req)) {
        headers.push(("retry-after", "5"));
        return conn.reply(req, 429, &headers, br#"{"error":"busy"}"#).await;
    }
    let board = url.query("board").filter(|b| !b.is_empty() && b.len() <= 80);
    let period = url.query("period").filter(|p| p == "week" || p == "all" || p == "day");
    let limit = url.query("limit").and_then(|l| l.parse::<usize>().ok());
    let source = url.query("source").filter(|s| s == "ingame" || s == "all");
    let body = public_json(app, board.as_deref(), period.as_deref(), limit, source.as_deref());
    let cache = format!("public, max-age={}", app.boards.every.min(60));
    headers.push(("cache-control", cache.as_str()));
    conn.reply(req, 200, &headers, body.as_bytes()).await
}

/// The public JSON: the cached text for the plain request (and for the plain ?source=ingame),
/// else made from the cached boards.
pub fn public_json(app: &App, board: Option<&str>, period: Option<&str>, limit: Option<usize>, source: Option<&str>) -> String {
    let plain = board.is_none() && period.is_none() && limit.map_or(true, |l| l == DEFAULT_ROWS);
    let limit = limit.unwrap_or(DEFAULT_ROWS);
    let ingame_id = board.is_some_and(|b| b.starts_with("ingame:"));
    let want_race = if board.is_some() { !ingame_id } else { source != Some("ingame") };
    let want_ingame = if board.is_some() { ingame_id } else { matches!(source, Some("ingame" | "all")) };
    // the race group's lock first, then the in-game one's (admin clears them one at a time)
    let mut race = want_race.then(|| lock(&app.boards.cached));
    let mut ingame = want_ingame.then(|| lock(&app.boards.ingame));
    if let Some(c) = race.as_deref_mut() {
        refresh(app, c, None, |now| {
            let game = app.boards.game_boards;
            app.lan.with_stats(|s| {
                let ids = lock(&app.identities);
                s.public_boards(&|name: &str| ids.is_claimed(name), game, now)
            })
        });
    }
    if let Some(c) = ingame.as_deref_mut() {
        refresh(app, c, Some("ingame"), |now| if app.boards.ingame_boards { app.lan.with_stats(|s| s.ingame_boards(now)) } else { Vec::new() });
    }
    let (r, i) = (race.as_deref().and_then(Option::as_ref), ingame.as_deref().and_then(Option::as_ref));
    match (r, i) {
        (Some(c), None) if plain && source.is_none() => c.4.clone(),
        (None, Some(c)) if plain && source == Some("ingame") => c.4.clone(),
        (Some(c), None) | (None, Some(c)) => answer(&c.3, c.1, c.2, board, period, limit, source).to_string(),
        (Some(r), Some(i)) => {
            // ?source=all: both groups (made apart, so up to CARBON_LEADERBOARD_SECS apart)
            let all: Vec<Board> = r.3.iter().chain(i.3.iter()).cloned().collect();
            answer(&all, r.1.min(i.1), r.2, board, period, limit, source).to_string()
        }
        (None, None) => {
            let now = unix_secs();
            answer(&[], now, crate::stats::week_number(now), board, period, limit, source).to_string()
        }
    }
}

/// Makes one cache again when it is older than CARBON_LEADERBOARD_SECS (or empty), with the
/// plain answer's text for `source`.
fn refresh(app: &App, cached: &mut Option<Cached>, source: Option<&str>, make: impl FnOnce(u64) -> Vec<Board>) {
    if cached.as_ref().is_some_and(|c| c.0.elapsed().as_secs() < app.boards.every) {
        return;
    }
    let now = unix_secs();
    let boards = make(now);
    let week = crate::stats::week_number(now);
    let text = answer(&boards, now, week, None, None, DEFAULT_ROWS, source).to_string();
    *cached = Some((Instant::now(), now, week, boards, text));
}

fn bearer(req: &Request) -> Option<&str> {
    req.header("authorization")?.strip_prefix("Bearer ")
}

async fn admin(app: &App, conn: &mut Conn, req: &mut Request, url: &Url, mut headers: Vec<(&str, &str)>) -> std::io::Result<()> {
    headers.push(("cache-control", "no-store"));
    if req.method == "OPTIONS" {
        headers.extend_from_slice(&[("access-control-allow-methods", "GET, POST, OPTIONS"), ("access-control-allow-headers", "Authorization, Content-Type"), ("access-control-max-age", "600")]);
        return conn.reply(req, 204, &headers, b"").await;
    }
    let reply = |status: u16, v: Value| (status, v.to_string());
    let (status, body) = 'answer: {
        if !lock(&app.boards.limits).allow(Kind::Admin, &client(conn, req)) {
            req.keep = false;
            break 'answer reply(429, json!({"error": "busy"}));
        }
        let given = url.query("key").unwrap_or_default();
        if !app.key.is_empty() && !same_bytes(given.as_bytes(), app.key.as_bytes()) {
            break 'answer reply(403, json!({"error": "forbidden"}));
        }
        let owner = bearer(req).and_then(crate::identity::owner_of);
        let Some(owner) = owner.filter(|o| lock(&app.identities).is_admin_owner(o)) else {
            break 'answer reply(401, json!({"error": "admin"}));
        };
        // the audit log names the admin by a short digest of its owner, never the owner itself
        let actor = format!("admin:{}", &owner[..12]);
        let args = match req.method.as_str() {
            "GET" => {
                let mut m = serde_json::Map::new();
                for k in ["op", "id", "name", "key", "text", "offset"] {
                    if let Some(v) = url.query(k) {
                        m.insert(k.into(), v.parse::<u64>().map(Value::from).unwrap_or(Value::String(v)));
                    }
                }
                Value::Object(m)
            }
            "POST" => {
                let Some(length) = req.length.filter(|l| *l <= 4096) else {
                    req.keep = false;
                    break 'answer reply(413, json!({"error": "invalid"}));
                };
                let mut bytes = Vec::with_capacity(length as usize);
                while req.left > 0 {
                    let n = conn.body_ready(req).await?;
                    bytes.extend_from_slice(&conn.buf[..n]);
                    conn.consume(req, n);
                }
                match serde_json::from_slice::<Value>(&bytes) {
                    Ok(v @ Value::Object(_)) => v,
                    _ => break 'answer reply(400, json!({"error": "invalid"})),
                }
            }
            _ => break 'answer reply(405, json!({"error": "method"})),
        };
        let op = args.get("op").and_then(Value::as_str).unwrap_or("").to_string();
        let read_only = matches!(op.as_str(), "pending" | "banned" | "audit");
        if req.method == "GET" && !read_only {
            break 'answer reply(405, json!({"error": "use POST for changes"}));
        }
        let result = app.lan.with_stats(|s| s.admin(&op, &actor, &args, unix_secs()));
        if !read_only {
            app.boards.clear(); // the next public answer shows the change
        }
        match result {
            Ok(v) => reply(200, json!({"ok": true, "op": op, "data": v})),
            Err(e) => reply(if e == "storage" { 503 } else { 400 }, json!({"ok": false, "op": op, "error": e})),
        }
    };
    conn.reply(req, status, &headers, body.as_bytes()).await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn buckets_allow_a_burst_then_a_rate() {
        let mut l = Limits::default();
        let t0 = Instant::now();
        let ok = (0..70).filter(|_| l.allow_at(Kind::UpdateFromClient, "1.2.3.4", t0)).count();
        assert_eq!(ok, 60);
        assert!(!l.allow_at(Kind::UpdateFromClient, "1.2.3.4", t0 + Duration::from_secs(5)));
        assert!(l.allow_at(Kind::UpdateFromClient, "1.2.3.4", t0 + Duration::from_secs(11)));
        // another address and another kind have their own
        assert!(l.allow_at(Kind::UpdateFromClient, "5.6.7.8", t0));
        assert!(l.allow_at(Kind::UpdateForName, "1.2.3.4", t0));
        // names are counted case-blind
        let ok = (0..25).filter(|i| l.allow_at(Kind::UpdateForName, if i % 2 == 0 { "Metin" } else { "METIN" }, t0)).count();
        assert_eq!(ok, 20);
    }

    #[test]
    fn origins_allowed() {
        for o in ["https://nfscrb.xyz", "https://www.nfscrb.xyz", "http://localhost:8098", "http://127.0.0.1:5173", "http://localhost"] {
            assert!(allowed_origin(o), "{o}");
        }
        for o in ["http://nfscrb.xyz", "https://nfscrb.xyz.evil.com", "https://evilnfscrb.xyz", "http://localhost.evil.com", "http://localhost:80a", "null", ""] {
            assert!(!allowed_origin(o), "{o}");
        }
    }

    #[test]
    fn the_queue_keeps_one_per_board_and_name_and_survives_a_restart() {
        let dir = std::env::temp_dir().join(format!("carbon-review-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let p = |name: &str, v: f64| Pending { id: 0, name: name.into(), key: "Evt_Bst_30F07253".into(), how: Update::Min, value: v, text: String::new(), at: 1, source: "game".into(), why: "w".into() };
        {
            let mut r = Review::open(Some(&dir));
            assert!(r.hold(p("Ada", 50.0), true));
            assert!(r.hold(p("ada", 45.0), true)); // better: replaces
            assert!(r.hold(p("Ada", 60.0), true)); // worse: kept as it was
            assert_eq!(r.pending.len(), 1);
            assert_eq!(r.pending[0].value, 45.0);
            let id = r.pending[0].id;
            let taken = r.take(id).unwrap();
            r.reject(&taken);
            r.ban("Cheat", "fake times", 5);
            r.log("reject", "admin:x", "Ada", "Evt_Bst_30F07253", Some(45.0), "", 6);
            assert!(r.save());
        }
        let r = Review::open(Some(&dir));
        assert!(r.pending.is_empty());
        assert!(r.was_rejected("ADA", "Evt_Bst_30F07253", 45.0));
        assert!(!r.was_rejected("ADA", "Evt_Bst_30F07253", 44.0));
        assert!(r.is_banned("cheat"));
        assert_eq!(r.audit(0, 10).0, 1);
        assert!(r.next > 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_name_cannot_fill_the_queue() {
        let mut r = Review::open(None);
        for i in 0..20 {
            let key = format!("Evt_Bst_{i:08X}");
            r.hold(Pending { id: 0, name: "Spam".into(), key, how: Update::Min, value: 20.0, text: String::new(), at: 0, source: "game".into(), why: String::new() }, true);
        }
        assert_eq!(r.pending.len(), MAX_PENDING_PER_NAME);
    }

    #[test]
    fn the_answer_has_the_pages_shape() {
        let b = Board {
            id: "Evt_Bst:30F07253".into(),
            family: "Evt_Bst".into(),
            source: "race",
            race: Some("30F07253".into()),
            laps: None,
            low: true,
            unit: "time",
            players: 2,
            all: vec![Row { name: "Metin".into(), value: 103.050_003, car: "0x8774322c".into(), when: 1_793_000_000, numbers: None }, Row { name: "Jan".into(), value: 104.76, car: String::new(), when: 0, numbers: None }],
            week: vec![],
            day: vec![],
        };
        let v = answer(&[b.clone()], 1, 2966, None, None, 20, None);
        assert_eq!(v["boards"][0]["all"][0], json!(["Metin", 103.05, "0x8774322c", 1_793_000_000u64]));
        assert_eq!(v["boards"][0]["all"][1][3], Value::Null);
        assert_eq!(v["families"][0], json!({"family": "Evt_Bst", "boards": 1, "source": "race"}));
        let one = answer(&[b], 1, 2966, Some("Evt_Bst:30F07253"), Some("week"), 1, None);
        assert!(one["boards"][0].get("all").is_none());
        assert_eq!(one["boards"][0]["week"], json!([]));
        assert_eq!(answer(&[], 1, 2966, Some("nope"), None, 5, None)["boards"], json!([]));
    }

    #[test]
    fn the_in_game_group_comes_only_when_asked_for() {
        let row = |n: &str, v: f64, numbers| Row { name: n.into(), value: v, car: String::new(), when: 0, numbers };
        let race = Board { id: "Evt_Bst:30F07253:2".into(), family: "Evt_Bst".into(), source: "race", race: Some("30F07253".into()), laps: Some(2), low: true, unit: "time", players: 1, all: vec![row("Metin", 210.0, None)], week: vec![], day: vec![] };
        let ingame = Board {
            id: "ingame:Evt_Bst:30F07253".into(),
            family: "Evt_Bst".into(),
            source: "ingame",
            race: Some("30F07253".into()),
            laps: None,
            low: true,
            unit: "time",
            players: 2,
            all: vec![row("Jan", 99.25, Some((51.5, 72.912_34))), row("Metin", 103.0, None)],
            week: vec![row("Jan", 99.25, Some((51.5, 72.9)))],
            day: vec![row("Jan", 99.25, Some((51.5, 72.9)))],
        };
        let boards = [race.clone(), ingame];
        // the plain answer is as before: race boards only, no "day", no in-game header
        let plain = answer(&boards, 86_400 * 3, 2966, None, None, 20, None);
        assert_eq!(plain["boards"].as_array().unwrap().len(), 1);
        assert_eq!(plain, answer(&[race], 86_400 * 3, 2966, None, None, 20, None));
        assert!(plain["boards"][0].get("day").is_none() && plain.get("ingame").is_none());
        // ?source=ingame: the in-game boards, labelled, with today and the game's two numbers
        let v = answer(&boards, 86_400 * 3, 2966, None, None, 20, Some("ingame"));
        let b = &v["boards"][0];
        assert_eq!((v["boards"].as_array().unwrap().len(), b["source"].as_str(), b["label"].as_str()), (1, Some("ingame"), Some(INGAME_LABEL)));
        assert_eq!(b["all"][0], json!(["Jan", 99.25, "", null, 51.5, 72.912]));
        assert_eq!(b["all"][1], json!(["Metin", 103.0, "", null]));
        assert_eq!(b["day"].as_array().unwrap().len(), 1);
        assert_eq!(v["ingame"]["day"], json!(3));
        assert_eq!(v["families"], json!([{"family": "Evt_Bst", "boards": 1, "source": "ingame"}]));
        // ?source=all has both; one board by id from either group; ?period=day
        assert_eq!(answer(&boards, 1, 2966, None, None, 20, Some("all"))["boards"].as_array().unwrap().len(), 2);
        let one = answer(&boards, 1, 2966, Some("ingame:Evt_Bst:30F07253"), Some("day"), 100, None);
        assert_eq!(one["boards"].as_array().unwrap().len(), 1);
        assert!(one["boards"][0].get("all").is_none() && one["boards"][0].get("week").is_none());
        assert_eq!(one["boards"][0]["day"][0][0], json!("Jan"));
    }
}
