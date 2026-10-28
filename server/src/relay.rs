//! Free roam with others: rooms of up to CARBON_ROOM_MAX players (default 256) passing car
//! poses, chat, the horn, crews, physics and impacts, each player getting the cars of the players
//! near it. The same protocol and rules as tools/carbon-net-relay.mjs (its header is the protocol
//! reference, "Interest" included; this follows its version of 2026-10-25):
//!
//!   browser -> relay: {t:"hi", name, crew, physics?, bin?, cops?, drive?}  {t:"pose", k, s, m, ts, ghost?, acks?}
//!                     {t:"chat", text}  {t:"horn", on}  {t:"crew", crew}  {t:"physics", on}
//!                     {t:"impact", to, seq, k, ts, s}  {t:"race", k, ...}  {t:"look", k, data}
//!                     {t:"follow", ids}  {t:"cophit", to, c, seq, k, ts, s}  a cops frame
//!                     {t:"pursuit", to, k, a, b, f}  {t:"lights", s}  {t:"ping", n}
//!   relay -> browser: {t:"welcome", id, physics?, bin?, cops?, drive?, peers:[{id, name, crew, physics?}]}
//!                     {t:"join", id, name, crew, physics?}  {t:"left", id}
//!                     {t:"pose", id, k, s, m, ts, physics?, ghost?, acks?}
//!                     {t:"chat", id, text}  {t:"horn", id, on}  {t:"crew", id, crew}
//!                     {t:"physics", id, on}  {t:"impact", id, seq, k, ts, s}
//!                     {t:"race", id, k, ...} (to the whole room, the sender too)
//!                     {t:"look", id, k, data} (to the players that get the sender's car)
//!                     {t:"where", p:[[id, x, z, heading], ...]} (every second, to everyone)
//!                     {t:"cophit", id, c, seq, k, ts, s} (to the chased player alone)
//!                     a cops frame (to the players that get the chased player's car)
//!                     {t:"pursuit", id, k, a, b, f} (shared police chase: to the players that get the
//!                     sender's car, or to the one player it is about; "Shared police chase" in the
//!                     Node relay's header)
//!                     {t:"lights", id, s} (car lights: to the players that get the sender's car; the
//!                     newest non-zero s again to a player who picks it later; "Car lights" in the
//!                     Node relay's header)
//!                     {t:"pong", n} (to the sender alone: its ping's n, for the page to time; "Ping"
//!                     in the Node relay's header)
//!
//! Interest, in short (the Node relay's header has the whole of it): every 500 ms each player
//! picks the K (CARBON_INTEREST_K, 24) players whose cars it gets: the ones it follows, then its
//! crew-mates within 1000 m (16 at most), then the nearest, with a hold so the picks do not flap.
//! Poses, looks, chat and the horn go only to the players concerned; a player picked later gets
//! the sender's look and newest pose at once. The idea is nfsu2-web's free-roam hub (NOTICE).
//!
//! Compact poses: a page whose hi says bin:1 (and whose welcome says so back) sends and gets
//! poses, and gets the map feed, as binary frames of 46 bytes (+ 8 per ack) instead of about 180
//! of JSON; pages of both kinds share a room, each getting every pose in its own kind (Wire makes
//! each kind once per pose). The layout and its rounding are in the Node relay's header,
//! "Compact poses". CARBON_RELAY_BIN=0 turns them off (JSON only, as a relay from before).
//!
//! Police chases (the Node relay's header, "Police chases"): a page whose hi says cops:1 and that
//! got compact poses sends its player's chasing cops as binary frames (type 3, 10 + 38 per cop)
//! and gets the cops of the players near it; {t:"cophit"} takes a hit on one of those cops to the
//! chased player, checked against its newest cops frame.
//!
//! Driving (the Node relay's header, "Driving"): a page whose hi says drive:1 and that got compact
//! poses sends its car's controls (8 bytes after a pose's acks, flags bit 3) and gets those of the
//! cars it gets; every other page gets the same poses without them. They pass as they came.
//!
//! Messages are built as text here, in the Node relay's key order; values that pass through
//! (s, ts, k) are written back as the client sent them. Distances are worked out with the same
//! operations in the same order as the Node relay, so both pick exactly the same players.

use std::cmp::Ordering;
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{Map, Value};

use crate::http::Conn;
use crate::util::{js_slice, js_trim, json_str, label, lock};
use crate::ws::{self, Outbox, Session};
use crate::App;

/// Players a room holds by default (CARBON_ROOM_MAX); the next is refused.
pub const MAX_ROOM: usize = 256;
/// Cars a player gets by default (CARBON_INTEREST_K).
pub const INTEREST_K: usize = 24;
/// Unsent data per player (see ws::Outbox).
const MAX_PENDING: usize = 64 * 1024;
/// A connection that has not said hi by then is dropped.
const HELLO_TIMEOUT: Duration = Duration::from_secs(10);
/// An impact needs both cars' poses fresher than this.
const POSE_FRESH: Duration = Duration::from_millis(1000);
/// A race message, as sent, is at most this many bytes.
const RACE_MAX: usize = 1024;
/// A car look (web/net.js, mods/core/nfsc/look.h) stands for at most this many bytes.
const LOOK_MAX: usize = 2048;
/// A pose names at most this many acknowledged impact sources.
const ACKS_MAX: usize = 8;

// Compact poses (the Node relay's header, "Compact poses"): binary frames for the pages that
// ask for them in their hi. The same sums in the same order as the Node relay, so both send
// the same bytes.
/// The binary format this relay speaks by default (CARBON_RELAY_BIN; 0 = JSON only).
const BIN: usize = 1;
const BIN_POSE: u8 = 1;
const BIN_WHERE: u8 = 2;
const POSE_BYTES: usize = 46;
const ACK_BYTES: usize = 8;
const WHERE_BYTES: usize = 14;
// Driving (the Node relay's header, "Driving"): flags bit 3 = this many DRIVE bytes follow the acks.
const DRIVE_BIT: u8 = 8;
const DRIVE_BYTES: usize = 8;
// Police chases (the Node relay's header, "Police chases").
const BIN_COPS: u8 = 3;
const COPS_HEAD: usize = 10;
const COP_BYTES: usize = 38;
const COPS_MAX: usize = 8;
/// A hit counts against a cops frame at most this old.
const COP_FRESH: Duration = Duration::from_millis(1000);
/// Metres from the cop, and from the hitting car, a hit may be.
const COP_REACH: f64 = 25.0;
/// The outbox key of a player's cops frames (its id plus this; a pose's key is its id).
const COPS_KEY: u64 = 1 << 40;
/// s[i] travels as a whole number of 1/SCALE[i], at most limit(i) either way.
const SCALE: [f64; 13] =
    [32767.0, 32767.0, 32767.0, 32767.0, 1000.0, 1000.0, 1000.0, 100.0, 100.0, 100.0, 500.0, 500.0, 500.0];

fn limit(i: usize) -> f64 {
    if (4..7).contains(&i) {
        2147483647.0
    } else {
        32767.0
    }
}

// Interest (the Node relay's header). The relay's ticker runs every TICK; picks every second
// tick, the map feed every fourth.
pub const TICK: Duration = Duration::from_millis(250);
const FAR_EVERY: Duration = TICK;
const MID_EVERY: Duration = Duration::from_millis(100);
const CREW_RANGE: f64 = 1000.0;
const CREW_MAX: usize = 16;
const FOLLOW_MAX: usize = 8;
/// A player whose newest pose is older than this is not driving now.
const ACTIVE: Duration = Duration::from_millis(5000);
/// A pick counts at HELD of its distance this long, then at KEPT.
const HOLD: Duration = Duration::from_millis(5000);
const HELD: f64 = 0.25;
const KEPT: f64 = 0.5;
/// Within this every pose goes (the game removes a car beyond 500 m); then one per MID_EVERY
/// within MID_RANGE, beyond one per FAR_EVERY.
const FULL_RANGE: f64 = 500.0;
const MID_RANGE: f64 = 700.0;
const TOP_SPEED: f64 = 100.0;
const FAR: f64 = 1e9;
/// The map feed leaves out a car farther out than this (whole metres).
const MAP_LIMIT: f64 = 1e6;

/// Per player: a burst of `burst` messages, then `rate` a second.
#[derive(Clone, Copy)]
enum Limit {
    Chat,
    Horn,
    Crew,
    Impact,
    Race,
    Look,
    Follow,
    Cops,
    CopHit,
    Pursuit,
    Lights,
    Ping,
    Street,
    Rivals,
}

impl Limit {
    fn rule(self) -> (f64, f64) {
        match self {
            Limit::Chat => (5.0, 0.5),
            Limit::Horn => (12.0, 6.0),
            Limit::Crew => (3.0, 0.2),
            Limit::Impact => (128.0, 120.0),
            Limit::Race => (20.0, 10.0),
            Limit::Look => (4.0, 0.2),
            Limit::Follow => (5.0, 1.0),
            Limit::Cops => (10.0, 20.0),
            Limit::CopHit => (64.0, 60.0),
            Limit::Pursuit => (40.0, 30.0),
            Limit::Lights => (10.0, 4.0),
            Limit::Ping => (5.0, 1.0),
            Limit::Street => (10.0, 5.0),
            Limit::Rivals => (10.0, 15.0),
        }
    }
}

#[derive(Clone, Copy)]
struct Bucket {
    tokens: f64,
    at: Instant,
}

struct Pose {
    k: f64,
    s: [f64; 13],
    m: f64,
    ts: f64,
    at: Instant,
    ghost: bool,
}

/// What a player's newest pose is passed on as: its parts, and each kind of frame once made.
struct Wire {
    k: u32,
    m: u8,
    /// ts >>> 0, for binary pages
    ts: u32,
    physics: bool,
    ghost: bool,
    /// ts and s as JSON pages get them: as a JSON page sent them, or made from n
    ts_json: String,
    s_json: String,
    /// s quantised (SCALE)
    n: [i32; 13],
    /// the acknowledgements passed on (physics only), as numbers and as JSON
    acks: Vec<(u32, u32)>,
    acks_json: String,
    json: Option<Arc<[u8]>>,
    bin: Option<Arc<[u8]>>,
    /// the car's controls (Driving), and the binary frame with them for the pages that read them
    drive: Option<[u8; DRIVE_BYTES]>,
    bin_drive: Option<Arc<[u8]>>,
}

impl Wire {
    fn text(&self, id: u64) -> Arc<[u8]> {
        let extra = if self.physics {
            format!(r#","physics":1,"ghost":{},"acks":{}"#, self.ghost, self.acks_json)
        } else {
            String::new()
        };
        text_frame(format!(
            r#"{{"t":"pose","id":{id},"k":{},"s":{},"m":{},"ts":{}{extra}}}"#,
            self.k, self.s_json, self.m, self.ts_json
        ))
    }

    fn binary(&self, id: u64, with_drive: bool) -> Arc<[u8]> {
        let acks: &[(u32, u32)] = if self.physics { &self.acks } else { &[] };
        let physics = if self.physics { 4 | if self.ghost { 2 } else { 0 } } else { 0 };
        let drive = if with_drive { self.drive } else { None };
        let mut b = Vec::with_capacity(POSE_BYTES + ACK_BYTES * acks.len() + DRIVE_BYTES);
        b.push(BIN_POSE);
        b.push(self.m | physics | if drive.is_some() { DRIVE_BIT } else { 0 } | ((acks.len() as u8) << 4));
        b.extend_from_slice(&(id as u32).to_le_bytes());
        b.extend_from_slice(&self.k.to_le_bytes());
        b.extend_from_slice(&self.ts.to_le_bytes());
        for (i, n) in self.n.iter().enumerate() {
            if (4..7).contains(&i) {
                b.extend_from_slice(&n.to_le_bytes());
            } else {
                b.extend_from_slice(&(*n as i16).to_le_bytes());
            }
        }
        for (from, seq) in acks {
            b.extend_from_slice(&from.to_le_bytes());
            b.extend_from_slice(&seq.to_le_bytes());
        }
        if let Some(d) = drive {
            b.extend_from_slice(&d);
        }
        ws::frame(&b, 2).into()
    }
}

/// A binary pose as a page sent it (browser -> relay; its id field is 0 there).
struct BinPose {
    k: u32,
    ts: u32,
    m: u8,
    ghost: bool,
    n: [i32; 13],
    acks: Vec<(u32, u32)>,
    /// the controls after the acks (flags bit 3)
    drive: Option<[u8; DRIVE_BYTES]>,
}

/// A binary pose, or None if the bytes are not one: type, flags (m, ghost, the number of acks
/// in the top four bits), id, k, ts, the 13 numbers, then the acks.
fn read_pose(b: &[u8]) -> Option<BinPose> {
    let drive_len = if b.len() > 1 && b[1] & DRIVE_BIT != 0 { DRIVE_BYTES } else { 0 };
    if b.len() < POSE_BYTES || b[0] != BIN_POSE || b.len() != POSE_BYTES + ACK_BYTES * (b[1] >> 4) as usize + drive_len {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    let i16_at = |o: usize| i16::from_le_bytes([b[o], b[o + 1]]) as i32;
    let n = std::array::from_fn(|i| {
        let v = match i {
            0..=3 => i16_at(14 + 2 * i),
            4..=6 => u32_at(22 + 4 * (i - 4)) as i32,
            _ => i16_at(34 + 2 * (i - 7)),
        };
        v.clamp(-(limit(i) as i32), limit(i) as i32)
    });
    let acks = (0..(b[1] >> 4) as usize).map(|j| (u32_at(46 + 8 * j), u32_at(50 + 8 * j))).collect();
    let drive = (drive_len != 0).then(|| std::array::from_fn(|j| b[b.len() - DRIVE_BYTES + j]));
    Some(BinPose { k: u32_at(6), ts: u32_at(10), m: b[1] & 1, ghost: (b[1] & 2) != 0, n, acks, drive })
}

/// A pose's 13 numbers as the whole numbers a binary pose carries.
fn quantise(s: &[f64; 13]) -> [i32; 13] {
    std::array::from_fn(|i| js_round(s[i] * SCALE[i]).clamp(-limit(i), limit(i)) as i32)
}

/// JavaScript's `x >>> 0`.
fn to_u32(x: f64) -> u32 {
    if x.is_finite() {
        x.trunc().rem_euclid(4294967296.0) as u32
    } else {
        0
    }
}

/// m / 10^places as JavaScript writes it (no trailing zeros, no exponent).
fn decimal(m: i64, places: u32) -> String {
    let scale = 10u64.pow(places);
    let (sign, a) = if m < 0 { ("-", m.unsigned_abs()) } else { ("", m as u64) };
    let (whole, mut frac, mut digits) = (a / scale, a % scale, places as usize);
    if frac == 0 {
        return format!("{sign}{whole}");
    }
    while frac % 10 == 0 {
        frac /= 10;
        digits -= 1;
    }
    format!("{sign}{whole}.{frac:0digits$}")
}

/// A binary pose's number as a JSON page gets it (the Node relay's jsonNumbers): the
/// quaternion to 5 decimals, the rest exactly (whole mm, cm/s and 1/500 rad/s).
fn json_number(n: i32, i: usize) -> String {
    match i {
        0..=3 => decimal(js_round(n as f64 / SCALE[i] * 1e5) as i64, 5),
        4..=6 => decimal(n as i64, 3),
        7..=9 => decimal(n as i64, 2),
        _ => decimal(2 * n as i64, 3),
    }
}

/// One player that gets a car: its id, when it last got a pose of it, and whether a newer one
/// was held back (a far car; see forward).
struct Edge {
    id: u64,
    sent: Option<Instant>,
    /// how long a held-back pose waits since `sent` (zero: none held back)
    owed: Duration,
}

struct Member {
    id: u64,
    name: String,
    crew: String,
    /// the crew as compared: lower case, no spaces
    crew_key: String,
    named: bool,
    physics: bool,
    pose: Option<Pose>,
    /// the last impact this player sent that was passed on
    impact_seq: f64,
    /// sender id -> the last impact seq passed on to this player (what its acks may name)
    receipts: HashMap<u64, f64>,
    buckets: [Option<Bucket>; 14],
    /// the newest non-empty car look: (k, base64 data)
    look: Option<(u32, String)>,
    /// its car's newest lights (lights.h bits 0-15; 0 = none on)
    lights: u8,
    out: Arc<Outbox>,
    /// the binary format it reads (compact poses), 0 = JSON only
    bin: usize,
    /// place and speed of its newest driving pose
    at: Option<[f64; 3]>,
    vel: [f64; 3],
    /// its newest pose as passed on, also for players that pick it later
    wire: Option<Wire>,
    /// the players whose cars it gets, and since when
    interest: Vec<(u64, Instant)>,
    /// the players that get its car
    audience: Vec<Edge>,
    /// the players it follows (racing together)
    follow: Vec<u64>,
    /// who its horn's "on" reached
    horn_to: Vec<u64>,
    /// it reads and sends police chases (hi cops:1, with compact poses)
    cops: bool,
    /// it reads and sends cars' controls (hi drive:1, with compact poses)
    drive: bool,
    /// its newest cops frame: when, and its cops (number, model key, place)
    chase: Option<(Instant, Vec<(u8, u32, [f64; 3])>)>,
    /// the last cophit seq it sent that was passed on
    cop_hit_seq: f64,
}

impl Member {
    fn allow(&mut self, kind: Limit, now: Instant) -> bool {
        let (burst, rate) = kind.rule();
        let b = self.buckets[kind as usize].get_or_insert(Bucket { tokens: burst, at: now });
        b.tokens = (b.tokens + now.duration_since(b.at).as_secs_f64() * rate).min(burst);
        b.at = now;
        if b.tokens < 1.0 {
            return false;
        }
        b.tokens -= 1.0;
        true
    }

    /// {id, name, crew, physics?} as JSON members, for welcome lists and joins.
    fn identity(&self) -> String {
        format!(
            r#""id":{},"name":{},"crew":{}{}"#,
            self.id,
            json_str(&self.name),
            json_str(&self.crew),
            if self.physics { r#","physics":1"# } else { "" }
        )
    }

    fn linked(&self, id: u64) -> Option<Instant> {
        self.interest.iter().find(|(s, _)| *s == id).map(|(_, since)| *since)
    }

    /// Driving now: its newest pose says so and is fresh.
    fn active(&self, now: Instant) -> bool {
        self.pose.as_ref().is_some_and(|p| p.m == 1.0 && now.duration_since(p.at) <= ACTIVE)
    }

    /// Its newest pose as a page of that kind reads it, each kind made once per pose.
    fn pose_frame(&mut self, bin: bool, drive: bool) -> Option<Arc<[u8]>> {
        let id = self.id;
        let w = self.wire.as_mut()?;
        if bin && drive && w.drive.is_some() {
            if w.bin_drive.is_none() {
                w.bin_drive = Some(w.binary(id, true));
            }
            w.bin_drive.clone()
        } else if bin {
            if w.bin.is_none() {
                w.bin = Some(w.binary(id, false));
            }
            w.bin.clone()
        } else {
            if w.json.is_none() {
                w.json = Some(w.text(id));
            }
            w.json.clone()
        }
    }
}

/// The players of one room, in the order they came, and where each is in that list.
#[derive(Default)]
struct Room {
    members: Vec<Member>,
    at: HashMap<u64, usize>,
}

impl Room {
    fn index(&self, id: u64) -> Option<usize> {
        self.at.get(&id).copied()
    }
}

pub struct Relay {
    /// room name -> its players
    rooms: HashMap<String, Room>,
    next_id: u64,
    room_max: usize,
    k: usize,
    /// the binary format it speaks (CARBON_RELAY_BIN), 0 = JSON only
    bin: usize,
    ticks: u64,
}

/// A whole number from the environment within [min, max], else the default (unset and "" too).
fn env_count(name: &str, fallback: usize, min: usize, max: usize) -> usize {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse::<f64>().ok())
        .filter(|n| n.fract() == 0.0 && *n >= min as f64 && *n <= max as f64)
        .map_or(fallback, |n| n as usize)
}

impl Default for Relay {
    fn default() -> Relay {
        Relay {
            rooms: HashMap::new(),
            next_id: 0,
            room_max: env_count("CARBON_ROOM_MAX", MAX_ROOM, 1, 10000),
            k: env_count("CARBON_INTEREST_K", INTEREST_K, 1, 1000),
            bin: env_count("CARBON_RELAY_BIN", BIN, 0, 1),
            ticks: 0,
        }
    }
}

fn text_frame(json: String) -> Arc<[u8]> {
    ws::frame(json.as_bytes(), 1).into()
}

/// To every named player in the room but `from`.
fn others(members: &[Member], from: u64, msg: &Arc<[u8]>, key: u64) {
    for p in members.iter().filter(|p| p.id != from && p.named) {
        p.out.push(key, msg.clone());
    }
}

fn number(v: Option<&Value>) -> Option<f64> {
    v.filter(|v| v.is_number()).and_then(Value::as_f64)
}

/// JavaScript's uint(): an integer from 1 to 2^32 - 1.
fn uint(v: Option<&Value>) -> Option<f64> {
    number(v).filter(|n| n.fract() == 0.0 && *n > 0.0 && *n <= 4_294_967_295.0)
}

fn numbers<const N: usize>(v: Option<&Value>) -> Option<[f64; N]> {
    let list = v?.as_array()?;
    if list.len() != N {
        return None;
    }
    let mut out = [0.0; N];
    for (o, x) in out.iter_mut().zip(list) {
        *o = number(Some(x))?;
    }
    Some(out)
}

fn json(v: Option<&Value>) -> String {
    v.map(|v| v.to_string()).unwrap_or_else(|| "null".into())
}

fn hypot(v: &[f64]) -> f64 {
    v.iter().map(|x| x * x).sum::<f64>().sqrt()
}

fn distance(a: &[f64], b: &[f64]) -> f64 {
    hypot(&[a[0] - b[0], a[1] - b[1], a[2] - b[2]])
}

// ---- Interest: who gets whose car ----

/// The Node relay's length3: the same sum in the same order.
fn length3(x: f64, y: f64, z: f64) -> f64 {
    (x * x + y * y + z * z).sqrt()
}

fn crew_key(crew: &str) -> String {
    crew.to_lowercase().chars().filter(|c| !c.is_whitespace()).collect()
}

/// A pose's speed, at most TOP_SPEED.
fn speed(s: &[f64; 13]) -> [f64; 3] {
    let n = length3(s[7], s[8], s[9]);
    if n > TOP_SPEED {
        [s[7] * (TOP_SPEED / n), s[8] * (TOP_SPEED / n), s[9] * (TOP_SPEED / n)]
    } else {
        [s[7], s[8], s[9]]
    }
}

/// How far c's car counts from r's: the nearer of the two cars 1 s and 4 s on at their speeds.
/// FAR when c is not driving now or r has never driven.
fn apart(r: &Member, c: &Member, now: Instant) -> f64 {
    let (Some(ra), Some(ca)) = (r.at, c.at) else { return FAR };
    if !c.active(now) {
        return FAR;
    }
    let rv = if r.active(now) { r.vel } else { [0.0; 3] };
    let cv = c.vel;
    let mut best = FAR;
    for t in [1.0, 4.0] {
        let d = length3(
            ra[0] + rv[0] * t - ca[0] - cv[0] * t,
            ra[1] + rv[1] * t - ca[1] - cv[1] * t,
            ra[2] + rv[2] * t - ca[2] - cv[2] * t,
        );
        if d < best {
            best = d; // NaN (from huge numbers) never is
        }
    }
    best
}

fn by_distance(a: &(u8, f64, u64, usize), b: &(u8, f64, u64, usize)) -> Ordering {
    a.1.partial_cmp(&b.1).unwrap_or(Ordering::Equal).then(a.2.cmp(&b.2))
}

/// Player i starts getting player j's car: j's look and newest pose go to i at once.
fn enter(room: &mut Room, i: usize, j: usize, now: Instant) {
    if !room.members[i].out.is_open() {
        return;
    }
    let (rid, sid) = (room.members[i].id, room.members[j].id);
    room.members[i].interest.push((sid, now));
    let s = &room.members[j];
    let look = s.look.as_ref().map(|(k, data)| look_frame(sid, *k, data));
    let lights = (s.lights != 0).then(|| lights_frame(sid, s.lights));
    let fresh = s.pose.as_ref().is_some_and(|p| now.duration_since(p.at) <= ACTIVE);
    let (bin, drive) = (room.members[i].bin != 0, room.members[i].drive);
    let pose = if fresh { room.members[j].pose_frame(bin, drive) } else { None };
    let out = room.members[i].out.clone();
    let mut edge = Edge { id: rid, sent: None, owed: Duration::ZERO };
    if let Some(f) = look {
        out.push(0, f);
    }
    if let Some(f) = lights {
        out.push(0, f);
    }
    if let Some(f) = pose {
        edge.sent = Some(now);
        out.push(sid, f);
    }
    room.members[j].audience.push(edge);
}

/// Player i stops getting the car of player `sid`.
fn unlink(room: &mut Room, i: usize, sid: u64) {
    let rid = room.members[i].id;
    room.members[i].interest.retain(|(s, _)| *s != sid);
    if let Some(j) = room.index(sid) {
        room.members[j].audience.retain(|e| e.id != rid);
    }
}

/// Picks whose cars player i gets now (the Node relay's pick) and applies the change.
fn pick(room: &mut Room, i: usize, now: Instant, k: usize) {
    let r = &room.members[i];
    if !r.named || !r.out.is_open() {
        return;
    }
    // (group, distance as counted, id, index)
    let mut list: Vec<(u8, f64, u64, usize)> = Vec::with_capacity(room.members.len());
    for (j, c) in room.members.iter().enumerate() {
        if j == i || !c.named {
            continue;
        }
        let mut d = apart(r, c, now);
        if let Some(since) = r.linked(c.id) {
            d *= if now.duration_since(since) < HOLD { HELD } else { KEPT };
        }
        let group = if r.follow.contains(&c.id) {
            0
        } else if !r.crew_key.is_empty() && c.crew_key == r.crew_key && d <= CREW_RANGE {
            1
        } else {
            2
        };
        list.push((group, d, c.id, j));
    }
    list.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| by_distance(a, b)));
    let (mut chosen, mut rest) = (Vec::with_capacity(k), Vec::new());
    let mut crew = 0;
    for x in list {
        if x.0 == 0 || (x.0 == 1 && crew < CREW_MAX) {
            if x.0 == 1 {
                crew += 1;
            }
            chosen.push(x);
        } else {
            rest.push(x); // a crew past CREW_MAX counts by distance like everyone
        }
    }
    rest.sort_by(by_distance);
    chosen.extend(rest);
    chosen.truncate(k);
    let gone: Vec<u64> =
        room.members[i].interest.iter().map(|(s, _)| *s).filter(|s| !chosen.iter().any(|x| x.2 == *s)).collect();
    for s in gone {
        unlink(room, i, s);
    }
    for (_, _, id, j) in chosen {
        if !room.members[i].out.is_open() {
            return;
        }
        if room.members[i].linked(id).is_none() {
            enter(room, i, j, now);
        }
    }
}

/// The least time between two of s's poses to r: none within FULL_RANGE or for a player r
/// follows (every pose), MID_EVERY within MID_RANGE, otherwise FAR_EVERY.
fn every(s: &Member, r: &Member) -> Duration {
    if r.follow.contains(&s.id) {
        return Duration::ZERO;
    }
    let (Some(a), Some(b)) = (s.at, r.at) else { return Duration::ZERO };
    let d = length3(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    if d <= FULL_RANGE {
        Duration::ZERO
    } else if d <= MID_RANGE {
        MID_EVERY
    } else {
        FAR_EVERY
    }
}

/// Player i's newest pose to the players that get its car.
fn forward(room: &mut Room, i: usize, now: Instant) {
    if room.members[i].wire.is_none() {
        return;
    }
    let sid = room.members[i].id;
    for e in 0..room.members[i].audience.len() {
        let Some(j) = room.index(room.members[i].audience[e].id) else { continue };
        let (s, r) = (&room.members[i], &room.members[j]);
        if !r.out.is_open() {
            continue;
        }
        let gap = every(s, r);
        if !gap.is_zero() && s.audience[e].sent.is_some_and(|t| now.duration_since(t) < gap) {
            room.members[i].audience[e].owed = gap;
            continue;
        }
        let (out, bin, drive) = (r.out.clone(), r.bin != 0, r.drive);
        let Some(frame) = room.members[i].pose_frame(bin, drive) else { return };
        let edge = &mut room.members[i].audience[e];
        edge.sent = Some(now);
        edge.owed = Duration::ZERO;
        out.push(sid, frame);
    }
}

/// A far player's pose held back is sent once the time it waited for (Edge::owed) has passed.
fn flush_owed(room: &mut Room, now: Instant) {
    for i in 0..room.members.len() {
        if room.members[i].wire.is_none() {
            continue;
        }
        let sid = room.members[i].id;
        for e in 0..room.members[i].audience.len() {
            let edge = &room.members[i].audience[e];
            if edge.owed.is_zero() || edge.sent.is_some_and(|t| now.duration_since(t) < edge.owed) {
                continue;
            }
            let Some(j) = room.index(edge.id) else { continue };
            if !room.members[j].out.is_open() {
                continue;
            }
            let (out, bin, drive) = (room.members[j].out.clone(), room.members[j].bin != 0, room.members[j].drive);
            let Some(frame) = room.members[i].pose_frame(bin, drive) else { continue };
            let edge = &mut room.members[i].audience[e];
            edge.sent = Some(now);
            edge.owed = Duration::ZERO;
            out.push(sid, frame);
        }
    }
}

/// Whom player i's chat and horn reach: the players it picked, those that picked it, its crew.
fn hearers(room: &Room, i: usize) -> Vec<usize> {
    let p = &room.members[i];
    let mut ids: Vec<u64> = p.interest.iter().map(|(s, _)| *s).chain(p.audience.iter().map(|e| e.id)).collect();
    if !p.crew_key.is_empty() {
        ids.extend(room.members.iter().filter(|q| q.id != p.id && q.crew_key == p.crew_key).map(|q| q.id));
    }
    ids.sort_unstable();
    ids.dedup();
    ids.into_iter()
        .filter_map(|id| room.index(id))
        .filter(|&j| room.members[j].named && room.members[j].out.is_open())
        .collect()
}

/// JavaScript's Math.round: the nearest whole number, halves up (exactly; floor(x + 0.5) is
/// one off for 0.49999999999999994 and for odd numbers from 2^52).
fn js_round(x: f64) -> f64 {
    let r = x.floor();
    if x - r >= 0.5 {
        r + 1.0
    } else {
        r
    }
}

/// Whole degrees from +z toward +x of the way the nose (+z of the body) points.
fn heading(s: &[f64; 13]) -> i64 {
    let (qx, qy, qz, qw) = (s[0], s[1], s[2], s[3]);
    let h = js_round((2.0 * (qx * qz + qw * qy)).atan2(1.0 - 2.0 * (qx * qx + qy * qy)) * 180.0 / std::f64::consts::PI);
    (h as i64).rem_euclid(360)
}

/// The map feed for a room: every player driving now ([id, x, z, heading]), or None while nobody is.
fn where_list(room: &Room, now: Instant) -> Option<Vec<(u64, i64, i64, i64)>> {
    let mut list = Vec::new();
    for x in room.members.iter().filter(|x| x.named && x.active(now)) {
        let (Some(at), Some(pose)) = (x.at, &x.pose) else { continue };
        let (px, pz) = (js_round(at[0]), js_round(at[2]));
        if px.abs() <= MAP_LIMIT && pz.abs() <= MAP_LIMIT {
            list.push((x.id, px as i64, pz as i64, heading(&pose.s)));
        }
    }
    (!list.is_empty()).then_some(list)
}

fn where_json(list: &[(u64, i64, i64, i64)]) -> Arc<[u8]> {
    let p: Vec<String> = list.iter().map(|(id, x, z, h)| format!("[{id},{x},{z},{h}]")).collect();
    text_frame(format!(r#"{{"t":"where","p":[{}]}}"#, p.join(",")))
}

/// The map feed as binary pages get it: type, then per player id, x, z (i32) and heading (u16).
fn where_binary(list: &[(u64, i64, i64, i64)]) -> Arc<[u8]> {
    let mut b = Vec::with_capacity(1 + WHERE_BYTES * list.len());
    b.push(BIN_WHERE);
    for &(id, x, z, h) in list {
        b.extend_from_slice(&(id as u32).to_le_bytes());
        b.extend_from_slice(&(x as i32).to_le_bytes());
        b.extend_from_slice(&(z as i32).to_le_bytes());
        b.extend_from_slice(&(h as u16).to_le_bytes());
    }
    ws::frame(&b, 2).into()
}

impl Relay {
    /// A place in the room, or None when it is full.
    fn admit(&mut self, room: &str, out: Arc<Outbox>) -> Option<u64> {
        if self.rooms.get(room).map_or(0, |r| r.members.len()) >= self.room_max {
            return None;
        }
        self.next_id += 1;
        let id = self.next_id;
        let r = self.rooms.entry(room.to_string()).or_default();
        r.at.insert(id, r.members.len());
        r.members.push(Member {
            id,
            name: "Player".into(),
            crew: String::new(),
            crew_key: String::new(),
            named: false,
            physics: false,
            pose: None,
            impact_seq: 0.0,
            receipts: HashMap::new(),
            buckets: [None; 14],
            look: None,
            lights: 0,
            out,
            bin: 0,
            at: None,
            vel: [0.0; 3],
            wire: None,
            interest: Vec::new(),
            audience: Vec::new(),
            follow: Vec::new(),
            horn_to: Vec::new(),
            cops: false,
            drive: false,
            chase: None,
            cop_hit_seq: 0.0,
        });
        Some(id)
    }

    /// (players on the relay, players in `room`), named ones only.
    pub fn stats(&self, room: &str) -> (usize, usize) {
        let named = |r: &Room| r.members.iter().filter(|p| p.named).count();
        (self.rooms.values().map(named).sum(), self.rooms.get(room).map_or(0, named))
    }

    fn named(&self, room: &str, id: u64) -> bool {
        self.rooms.get(room).and_then(|r| r.index(id).map(|i| r.members[i].named)).unwrap_or(false)
    }

    fn leave(&mut self, room: &str, id: u64) {
        let Some(r) = self.rooms.get_mut(room) else { return };
        let Some(i) = r.index(id) else { return };
        let gone = r.members.remove(i);
        r.at.remove(&id);
        for (j, p) in r.members.iter().enumerate().skip(i) {
            r.at.insert(p.id, j);
        }
        for e in &gone.audience {
            if let Some(j) = r.index(e.id) {
                r.members[j].interest.retain(|(s, _)| *s != id);
            }
        }
        for (s, _) in &gone.interest {
            if let Some(j) = r.index(*s) {
                r.members[j].audience.retain(|e| e.id != id);
            }
        }
        let left = text_frame(format!(r#"{{"t":"left","id":{id}}}"#));
        for p in r.members.iter_mut() {
            if p.named {
                // net.js makes a player for each pose, so a queued old pose must never follow
                // that player's departure.
                p.out.remove(id);
                p.out.remove(id | COPS_KEY);
                p.out.push(0, left.clone());
            }
            p.receipts.remove(&id);
        }
        let remain = r.members.len();
        if remain == 0 {
            self.rooms.remove(room);
        }
        println!("{room}: {} ({id}) left, {remain} remain", gone.name);
    }

    /// Every TICK: owed far poses; every second tick the picks, every fourth the map feed.
    pub fn tick(&mut self) {
        let now = Instant::now();
        self.ticks += 1;
        let (k, picks, feed) = (self.k, self.ticks % 2 == 0, self.ticks % 4 == 0);
        for room in self.rooms.values_mut() {
            flush_owed(room, now);
            if picks {
                for i in 0..room.members.len() {
                    pick(room, i, now, k);
                }
            }
            if feed {
                if let Some(list) = where_list(room, now) {
                    // Each kind made once, if a page here reads it.
                    let (mut json, mut bin) = (None, None);
                    for p in room.members.iter().filter(|p| p.named) {
                        let f = if p.bin != 0 {
                            bin.get_or_insert_with(|| where_binary(&list)).clone()
                        } else {
                            json.get_or_insert_with(|| where_json(&list)).clone()
                        };
                        p.out.push(0, f);
                    }
                }
            }
        }
    }

    /// A binary frame: a compact pose, checked as a JSON one is, or a cops frame.
    fn handle_binary(&mut self, room: &str, id: u64, data: &[u8]) {
        let Some(r) = self.rooms.get_mut(room) else { return };
        let Some(i) = r.index(id) else { return };
        if r.members[i].named {
            if data.first() == Some(&BIN_COPS) {
                cops(r, i, data, Instant::now());
            } else {
                pose_binary(r, i, data, Instant::now());
            }
        }
    }

    fn handle(&mut self, room: &str, id: u64, text: &str) {
        let Ok(Value::Object(m)) = serde_json::from_str::<Value>(text) else { return };
        let (k, bin) = (self.k, self.bin);
        let Some(r) = self.rooms.get_mut(room) else { return };
        let Some(i) = r.index(id) else { return };
        let now = Instant::now();
        let named = r.members[i].named;
        match m.get("t").and_then(Value::as_str) {
            Some("hi") => hi(r, i, &m, room, k, bin),
            Some("pose") if named => pose(r, i, &m, now),
            Some("follow") if named => follow(r, i, &m, now, k),
            Some("physics") if named => {
                if let Some(Value::Bool(on)) = m.get("on") {
                    r.members[i].physics = *on;
                    others(&r.members, id, &text_frame(format!(r#"{{"t":"physics","id":{id},"on":{on}}}"#)), 0);
                }
            }
            Some("impact") => impact(&mut r.members, i, &m, now),
            Some("cophit") => cop_hit(&mut r.members, i, &m, now),
            Some("chat") if named => {
                if let Some(text) = m.get("text").and_then(Value::as_str) {
                    let cleaned: String = text.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
                    let text = js_slice(js_trim(&cleaned), 120);
                    if !text.is_empty() && r.members[i].allow(Limit::Chat, now) {
                        let msg = text_frame(format!(r#"{{"t":"chat","id":{id},"text":{}}}"#, json_str(text)));
                        for j in hearers(r, i) {
                            r.members[j].out.push(0, msg.clone());
                        }
                    }
                }
            }
            Some("crew") if named => {
                if m.get("crew").is_some_and(Value::is_string) && r.members[i].allow(Limit::Crew, now) {
                    // A player changing crew while driving; everyone else, and the sender, learn the cleaned name.
                    let p = &mut r.members[i];
                    p.crew = label(m.get("crew"), "", 16);
                    p.crew_key = crew_key(&p.crew);
                    let msg = text_frame(format!(r#"{{"t":"crew","id":{id},"crew":{}}}"#, json_str(&p.crew)));
                    p.out.push(0, msg.clone());
                    others(&r.members, id, &msg, 0);
                }
            }
            Some("horn") if named => {
                if let Some(&Value::Bool(on)) = m.get("on") {
                    if r.members[i].allow(Limit::Horn, now) {
                        horn(r, i, on);
                    }
                }
            }
            Some("race") if named => race(&mut r.members, i, &m, text.len(), now),
            Some("look") if named => look(r, i, &m, now),
            Some("pursuit") if named => pursuit(r, i, &m, now),
            Some("street") if named => street(r, i, &m, now),
            Some("rivals") if named => rivals(r, i, &m, now),
            Some("lights") if named => lights(r, i, &m, now),
            Some("ping") if named => ping(r, i, &m, now),
            _ => {}
        }
    }
}

/// The horn, to the hearers; its "off" also reaches everyone its "on" reached, even if they have
/// gone out of reach meanwhile.
fn horn(room: &mut Room, i: usize, on: bool) {
    let id = room.members[i].id;
    let mut to = hearers(room, i);
    if on {
        let ids: Vec<u64> = to.iter().map(|&j| room.members[j].id).collect();
        let p = &mut room.members[i];
        for x in ids {
            if !p.horn_to.contains(&x) {
                p.horn_to.push(x);
            }
        }
    } else {
        for x in std::mem::take(&mut room.members[i].horn_to) {
            if let Some(j) = room.index(x) {
                if !to.contains(&j) && room.members[j].named && room.members[j].out.is_open() {
                    to.push(j);
                }
            }
        }
    }
    let msg = text_frame(format!(r#"{{"t":"horn","id":{id},"on":{on}}}"#));
    for j in to {
        room.members[j].out.push(0, msg.clone());
    }
}

/// {t:"follow", ids}: the players racing with this one, picked first from now on.
fn follow(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant, k: usize) {
    let Some(list) = m.get("ids").and_then(Value::as_array) else { return };
    if list.len() > FOLLOW_MAX {
        return;
    }
    let Some(ids) = list.iter().map(|v| uint(Some(v)).map(|n| n as u64)).collect::<Option<Vec<u64>>>() else { return };
    if !room.members[i].allow(Limit::Follow, now) {
        return;
    }
    room.members[i].follow = ids;
    pick(room, i, now, k);
}

/// Racing together (web/race.js): the lobby, start, progress and finish of a race, to everyone in
/// the room, the sender too, so a "go" reaches every player, its host included, the same way. k is
/// a lowercase word of 1-12 letters; the other fields pass as they came, but never another t or id.
fn race(members: &mut [Member], i: usize, m: &Map<String, Value>, bytes: usize, now: Instant) {
    let kind_ok = m
        .get("k")
        .and_then(Value::as_str)
        .is_some_and(|k| (1..=12).contains(&k.len()) && k.bytes().all(|b| b.is_ascii_lowercase()));
    if !kind_ok || bytes > RACE_MAX || !members[i].allow(Limit::Race, now) {
        return;
    }
    let id = members[i].id;
    let mut out = format!(r#"{{"t":"race","id":{id}"#);
    for (key, value) in m.iter().filter(|(key, _)| key.as_str() != "t" && key.as_str() != "id") {
        out.push_str(&format!(",{}:{}", json_str(key), value));
    }
    out.push('}');
    let msg = text_frame(out);
    members[i].out.push(0, msg.clone());
    others(members, id, &msg, 0);
}

/// Shared police chase (pursuit.h; the Node relay's header, "Shared police chase"): k 1-15, a and
/// b 0-65535, f four finite numbers within +-1e6, passed on as sent. to 0: about the sender's own
/// car, to the players that get it; to an id: about that player's car, to that player alone, and
/// only if the sender gets that car.
fn pursuit(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let whole = |key: &str, max: f64| number(m.get(key)).filter(|n| n.fract() == 0.0 && *n >= 0.0 && *n <= max);
    let (Some(k), Some(a), Some(b), Some(to)) =
        (whole("k", 15.0), whole("a", 65535.0), whole("b", 65535.0), whole("to", 4_294_967_295.0))
    else {
        return;
    };
    let Some(f) = m.get("f").and_then(Value::as_array) else { return };
    if k < 1.0 || f.len() != 4 || !f.iter().all(|x| number(Some(x)).is_some_and(|n| n.is_finite() && n.abs() <= 1e6)) {
        return;
    }
    let to: Vec<usize> = if to == 0.0 {
        room.members[i].audience.iter().filter_map(|e| room.index(e.id)).collect()
    } else {
        let target = to as u64;
        if room.members[i].interest.iter().any(|(s, _)| *s == target) {
            room.index(target).into_iter().collect()
        } else {
            Vec::new()
        }
    };
    if to.is_empty() || !room.members[i].allow(Limit::Pursuit, now) {
        return;
    }
    let id = room.members[i].id;
    let list: Vec<String> = f.iter().map(Value::to_string).collect();
    let msg = text_frame(format!(
        r#"{{"t":"pursuit","id":{id},"k":{},"a":{},"b":{},"f":[{}]}}"#,
        k as u64,
        a as u64,
        b as u64,
        list.join(",")
    ));
    for j in to {
        if room.members[j].named {
            room.members[j].out.push(0, msg.clone());
        }
    }
}

/// Visual encounter cars only. No race participation or career events are forwarded.
fn rivals(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let uint = |v: Option<&Value>| number(v).filter(|n| n.fract() == 0.0 && *n > 0.0 && *n <= 4_294_967_295.0);
    let Some(ts) = number(m.get("ts")).filter(|n| n.fract() == 0.0 && *n >= 0.0 && *n <= 4_294_967_295.0) else { return };
    let Some(list) = m.get("cars").and_then(Value::as_array).filter(|v| v.len() <= 4) else { return };
    let mut seen = Vec::new();
    let mut cars = Vec::new();
    for x in list {
        let (Some(c), Some(k), Some(stamp)) = (uint(x.get("c")), uint(x.get("k")), uint(x.get("stamp"))) else { return };
        if seen.contains(&(c as u32)) { return; }
        seen.push(c as u32);
        let Some(s) = x.get("s").and_then(Value::as_array).filter(|v| v.len() == 13) else { return };
        let Some(s) = s.iter().map(|v| number(Some(v)).filter(|n| n.is_finite() && n.abs() <= 1e6)).collect::<Option<Vec<f64>>>() else { return };
        let q: f64 = s[..4].iter().map(|v| v * v).sum();
        if !(0.5..=1.5).contains(&q) { return; }
        let mut car = serde_json::json!({"c": c as u32, "k": k as u32, "stamp": stamp as u32, "s": s});
        if let Some(look) = x.get("look") {
            let Some(data) = look.as_str() else { return };
            if !matches!(look_bytes(data), Some(0) | Some(0x434)) { return; }
            car["look"] = Value::String(data.to_owned());
        }
        cars.push(car);
    }
    if !room.members[i].allow(Limit::Rivals, now) { return; }
    let id = room.members[i].id;
    let msg = text_frame(serde_json::json!({"t":"rivals", "id": id, "ts": ts as u32, "cars": cars}).to_string());
    for e in &room.members[i].audience {
        if let Some(j) = room.index(e.id) {
            if room.members[j].named { room.members[j].out.push(0, msg.clone()); }
        }
    }
}

/// Free-roam challenges go only to a named opponent in the same room, regardless of distance.
fn street(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let whole = |key: &str, max: f64| number(m.get(key)).filter(|n| n.fract() == 0.0 && *n >= 0.0 && *n <= max);
    let (Some(k), Some(r), Some(a), Some(to)) =
        (whole("k", 15.0), whole("r", 4_294_967_295.0), whole("a", 65535.0), whole("to", 4_294_967_295.0))
    else { return };
    let Some(f) = m.get("f").and_then(Value::as_array) else { return };
    if k < 1.0 || to < 1.0 || f.len() != 4 || !f.iter().all(|x| number(Some(x)).is_some_and(|n| n.is_finite() && n.abs() <= 1e6)) {
        return;
    }
    let Some(j) = room.index(to as u64) else { return };
    if i == j || !room.members[j].named || !room.members[i].allow(Limit::Street, now) { return; }
    let id = room.members[i].id;
    let list: Vec<String> = f.iter().map(Value::to_string).collect();
    room.members[j].out.push(0, text_frame(format!(
        r#"{{"t":"street","id":{id},"k":{},"r":{},"a":{},"f":[{}]}}"#,
        k as u64, r as u64, a as u64, list.join(",")
    )));
}

fn lights_frame(id: u64, s: u8) -> Arc<[u8]> {
    text_frame(format!(r#"{{"t":"lights","id":{id},"s":{s}}}"#))
}

/// A car's lights (lights.h; the Node relay's header, "Car lights"): s a whole number 0-15, to the
/// players that get this car; the newest is kept for players who pick it later.
fn lights(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let Some(s) = number(m.get("s")).filter(|s| s.fract() == 0.0 && *s >= 0.0 && *s <= 15.0) else { return };
    if !room.members[i].allow(Limit::Lights, now) {
        return;
    }
    let id = room.members[i].id;
    room.members[i].lights = s as u8;
    let msg = lights_frame(id, s as u8);
    for e in &room.members[i].audience {
        if let Some(j) = room.index(e.id) {
            if room.members[j].named {
                room.members[j].out.push(0, msg.clone());
            }
        }
    }
}

/// {t:"ping", n} (the Node relay's header, "Ping"): n a whole number 0 to 2^32 - 1, straight back
/// to the sender alone as {t:"pong", n}, for the page to time its link.
fn ping(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let Some(n) = number(m.get("n")).filter(|n| n.fract() == 0.0 && *n >= 0.0 && *n <= 4_294_967_295.0) else { return };
    if !room.members[i].allow(Limit::Ping, now) {
        return;
    }
    room.members[i].out.push(0, text_frame(format!(r#"{{"t":"pong","n":{}}}"#, n as u64)));
}

/// The bytes a look's data stands for (standard base64, at most LOOK_MAX), or None.
fn look_bytes(data: &str) -> Option<usize> {
    let body = data.trim_end_matches('=');
    let pad = data.len() - body.len();
    if data.len() % 4 != 0 || pad > 2 || !body.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'+' || b == b'/') {
        return None;
    }
    let n = data.len() / 4 * 3 - pad;
    (n <= LOOK_MAX).then_some(n)
}

fn look_frame(id: u64, k: u32, data: &str) -> Arc<[u8]> {
    text_frame(format!(r#"{{"t":"look","id":{id},"k":{k},"data":{}}}"#, json_str(data)))
}

/// A car's paint, parts and vinyls: to the players that get this car; the newest non-empty one
/// is kept for players who pick it later ("" = the car's stock look). k is the model key, data
/// standard base64 of at most LOOK_MAX bytes, opaque here.
fn look(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let Some(k) = number(m.get("k")).filter(|k| k.fract() == 0.0 && *k >= 0.0 && *k <= 4_294_967_295.0) else { return };
    let Some(data) = m.get("data").and_then(Value::as_str) else { return };
    if look_bytes(data).is_none() || !room.members[i].allow(Limit::Look, now) {
        return;
    }
    let id = room.members[i].id;
    room.members[i].look = (!data.is_empty()).then(|| (k as u32, data.to_string()));
    let msg = look_frame(id, k as u32, data);
    for e in &room.members[i].audience {
        if let Some(j) = room.index(e.id) {
            if room.members[j].named {
                room.members[j].out.push(0, msg.clone());
            }
        }
    }
}

fn hi(room: &mut Room, i: usize, m: &Map<String, Value>, name: &str, k: usize, bin: usize) {
    let members = &mut room.members;
    let id = members[i].id;
    // JSON objects can shadow toString/valueOf in the Node relay, so it never coerces client
    // fields; here: anything but a string is "".
    members[i].name = label(m.get("name"), "Player", 20);
    members[i].crew = label(m.get("crew"), "", 16);
    members[i].crew_key = crew_key(&members[i].crew);
    let physics = number(m.get("physics"));
    members[i].physics = physics == Some(1.0);
    // Compact poses: a page that reads binary poses says which format (1 now); 0 = JSON only.
    let asked = number(m.get("bin")).is_some_and(|b| b.fract() == 0.0 && b >= 1.0);
    members[i].bin = if asked { bin } else { 0 };
    // Police chases: binary frames, so only for a page that also got compact poses.
    members[i].cops = members[i].bin != 0 && number(m.get("cops")) == Some(1.0);
    // Driving: controls inside compact poses, so only for a page that also got them.
    members[i].drive = members[i].bin != 0 && number(m.get("drive")) == Some(1.0);
    let peers: Vec<String> =
        members.iter().filter(|p| p.id != id && p.named).map(|p| format!("{{{}}}", p.identity())).collect();
    let welcome = format!(
        r#"{{"t":"welcome","id":{id}{}{}{}{}{},"peers":[{}]}}"#,
        if physics == Some(0.0) || physics == Some(1.0) { r#","physics":1"# } else { "" },
        if members[i].bin != 0 { format!(r#","bin":{}"#, members[i].bin) } else { String::new() },
        if members[i].cops { r#","cops":1"# } else { "" },
        if members[i].drive { r#","drive":1"# } else { "" },
        if number(m.get("rivals")) == Some(1.0) { r#","rivals":1"# } else { "" },
        peers.join(",")
    );
    members[i].out.push(0, text_frame(welcome));
    if !members[i].out.is_open() {
        return;
    }
    members[i].named = true;
    others(members, id, &text_frame(format!(r#"{{"t":"join",{}}}"#, members[i].identity())), 0);
    if !members[i].out.is_open() {
        return;
    }
    // The cars it gets, at once: their looks (so they are built in them from the start) and poses.
    let now = Instant::now();
    pick(room, i, now, k);
    if !room.members[i].out.is_open() {
        return;
    }
    // Players with room for more take the newcomer now, not at their next pick.
    for j in 0..room.members.len() {
        let p = &room.members[j];
        if j != i && p.named && p.out.is_open() && p.linked(id).is_none() && p.interest.len() < k {
            enter(room, j, i, now);
        }
    }
    if !room.members[i].out.is_open() {
        return;
    }
    println!("{name}: {} ({id}) joined, {} here", room.members[i].name, room.members.len());
}

fn pose(room: &mut Room, i: usize, m: &Map<String, Value>, now: Instant) {
    let Some(s) = numbers::<13>(m.get("s")) else { return };
    let Some(k) = number(m.get("k")).filter(|k| k.fract() == 0.0 && *k >= 0.0 && *k <= 4_294_967_295.0) else { return };
    let Some(mode) = number(m.get("m")).filter(|v| *v == 0.0 || *v == 1.0) else { return };
    let ts_value = m.get("ts").filter(|v| v.is_number());
    let ts = number(ts_value).unwrap_or(0.0);
    let ghost = m.get("ghost") == Some(&Value::Bool(true));
    let p = &room.members[i];
    // An ack names an impact this player really received: [sender id, seq] at most the last
    // seq passed on from that sender.
    let acks: Vec<Value> = match m.get("acks") {
        Some(Value::Array(list)) if p.physics => list
            .iter()
            .take(ACKS_MAX)
            .filter(|a| match a.as_array().map(Vec::as_slice) {
                Some([from, seq]) => match (uint(Some(from)), uint(Some(seq))) {
                    (Some(from), Some(seq)) => seq <= p.receipts.get(&(from as u64)).copied().unwrap_or(0.0),
                    _ => false,
                },
                _ => false,
            })
            .cloned()
            .collect(),
        _ => Vec::new(),
    };
    let pairs = acks
        .iter()
        .filter_map(|a| Some((uint(a.get(0))? as u32, uint(a.get(1))? as u32)))
        .collect();
    // JSON pages get s and ts as sent, binary pages the same numbers quantised.
    let wire = Wire {
        k: k as u32,
        m: mode as u8,
        ts: to_u32(ts),
        physics: p.physics,
        ghost,
        ts_json: ts_value.map_or_else(|| "0".to_string(), Value::to_string),
        s_json: json(m.get("s")),
        n: quantise(&s),
        acks: pairs,
        acks_json: Value::Array(acks).to_string(),
        json: None,
        bin: None,
        drive: None,
        bin_drive: None,
    };
    posed(room, i, Pose { k, s, m: mode, ts, at: now, ghost }, wire, now);
}

/// A compact pose (a binary frame), checked as a JSON one is.
fn pose_binary(room: &mut Room, i: usize, data: &[u8], now: Instant) {
    let Some(b) = read_pose(data) else { return };
    let p = &room.members[i];
    let acks: Vec<(u32, u32)> = if p.physics {
        b.acks
            .iter()
            .take(ACKS_MAX)
            .filter(|&&(from, seq)| {
                from != 0 && seq != 0 && seq as f64 <= p.receipts.get(&(from as u64)).copied().unwrap_or(0.0)
            })
            .copied()
            .collect()
    } else {
        Vec::new()
    };
    let s: [f64; 13] = std::array::from_fn(|i| b.n[i] as f64 / SCALE[i]);
    let numbers: Vec<String> = (0..13).map(|i| json_number(b.n[i], i)).collect();
    let acks_json: Vec<String> = acks.iter().map(|(from, seq)| format!("[{from},{seq}]")).collect();
    let wire = Wire {
        k: b.k,
        m: b.m,
        ts: b.ts,
        physics: p.physics,
        ghost: b.ghost,
        ts_json: b.ts.to_string(),
        s_json: format!("[{}]", numbers.join(",")),
        n: b.n,
        acks,
        acks_json: format!("[{}]", acks_json.join(",")),
        json: None,
        bin: None,
        drive: if p.drive { b.drive } else { None },
        bin_drive: None,
    };
    let pose = Pose { k: b.k as f64, s, m: b.m as f64, ts: b.ts as f64, at: now, ghost: b.ghost };
    posed(room, i, pose, wire, now);
}

/// A player's new pose (JSON or binary): kept, and passed on to the players that get its car.
/// Its frames are made when first needed (Member::pose_frame), one for every receiver of a kind;
/// only the newest unsent pose per car is kept for a slow reader.
fn posed(room: &mut Room, i: usize, pose: Pose, wire: Wire, now: Instant) {
    let p = &mut room.members[i];
    if pose.m == 1.0 {
        p.at = Some([pose.s[4], pose.s[5], pose.s[6]]);
        p.vel = speed(&pose.s);
    }
    p.pose = Some(pose);
    p.wire = Some(wire);
    forward(room, i, now);
}

/// One browser solves a pair (the lower connection id), using Carbon's own contact solver.
/// Only the struck car's owner receives the resulting changes. A reliable event is never
/// coalesced like a position update.
fn impact(members: &mut [Member], i: usize, m: &Map<String, Value>, now: Instant) {
    let p = &members[i];
    if !p.named || !p.physics {
        return;
    }
    let (Some(to), Some(seq)) = (uint(m.get("to")), uint(m.get("seq"))) else { return };
    if seq <= p.impact_seq {
        return;
    }
    let Some(s) = numbers::<12>(m.get("s")) else { return };
    let Some(ts) = number(m.get("ts")) else { return };
    if !members[i].allow(Limit::Impact, now) {
        return;
    }
    let Some(j) = members.iter().position(|q| q.id as f64 == to && q.named && q.physics) else { return };
    let (p, q) = (&members[i], &members[j]);
    if p.id >= q.id {
        return;
    }
    let (Some(pp), Some(qp)) = (&p.pose, &q.pose) else { return };
    if pp.m != 1.0 || qp.m != 1.0 || pp.ghost || qp.ghost
        || now.duration_since(pp.at) > POSE_FRESH || now.duration_since(qp.at) > POSE_FRESH
        || number(m.get("k")) != Some(qp.k) || (ts - pp.ts).abs() > 1000.0
        || distance(&s[0..3], &pp.s[4..7]) > 20.0 || distance(&s[0..3], &qp.s[4..7]) > 20.0
        || hypot(&s[3..6]) > 6.0 || hypot(&s[6..9]) > 100.0 || hypot(&s[9..12]) > 50.0
    {
        return;
    }
    let from = p.id;
    members[i].impact_seq = seq;
    members[j].receipts.insert(from, seq);
    let msg = format!(
        r#"{{"t":"impact","id":{from},"seq":{},"k":{},"ts":{},"s":{}}}"#,
        json(m.get("seq")),
        json(m.get("k")),
        json(m.get("ts")),
        json(m.get("s"))
    );
    members[j].out.push(0, text_frame(msg));
}

// ---- Police chases (the Node relay's header, "Police chases") ----

/// The cops of a cops frame (number, model key, place in metres), or None if it is not one: n at
/// most COPS_MAX, the exact size, each number 1-255 and used once.
fn read_cops(b: &[u8]) -> Option<Vec<(u8, u32, [f64; 3])>> {
    if b.len() < COPS_HEAD || b[0] != BIN_COPS || b[1] as usize > COPS_MAX || b.len() != COPS_HEAD + COP_BYTES * b[1] as usize {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    let mut list: Vec<(u8, u32, [f64; 3])> = Vec::with_capacity(b[1] as usize);
    for j in 0..b[1] as usize {
        let at = COPS_HEAD + COP_BYTES * j;
        let c = b[at];
        if c == 0 || list.iter().any(|x| x.0 == c) {
            return None;
        }
        let p = std::array::from_fn(|i| u32_at(at + 14 + 4 * i) as i32 as f64 / 1000.0);
        list.push((c, u32_at(at + 2), p));
    }
    Some(list)
}

/// Whether r gets s's cops now: it follows s, or their cars are within MID_RANGE (or not known yet).
fn cops_reach(s: &Member, r: &Member) -> bool {
    let (Some(a), Some(b)) = (s.at, r.at) else { return true };
    r.follow.contains(&s.id) || length3(a[0] - b[0], a[1] - b[1], a[2] - b[2]) <= MID_RANGE
}

/// Player i's cops frame: kept to check hits against, and passed on (its id written in) to the
/// players that get its car and asked for cops; the end of a chase (no cops) to all of them.
fn cops(room: &mut Room, i: usize, data: &[u8], now: Instant) {
    if !room.members[i].cops {
        return;
    }
    let Some(list) = read_cops(data) else { return };
    if !room.members[i].allow(Limit::Cops, now) {
        return;
    }
    let sid = room.members[i].id;
    let empty = list.is_empty();
    room.members[i].chase = Some((now, list));
    let mut out = data.to_vec();
    out[2..6].copy_from_slice(&(sid as u32).to_le_bytes());
    let frame: Arc<[u8]> = ws::frame(&out, 2).into();
    let s = &room.members[i];
    for e in &s.audience {
        let Some(j) = room.index(e.id) else { continue };
        let r = &room.members[j];
        if r.named && r.out.is_open() && r.cops && (empty || cops_reach(s, r)) {
            r.out.push(sid | COPS_KEY, frame.clone());
        }
    }
}

/// A hit on another player's cop, to that player alone (see the Node relay's header).
fn cop_hit(members: &mut [Member], i: usize, m: &Map<String, Value>, now: Instant) {
    let p = &members[i];
    if !p.named || !p.cops {
        return;
    }
    let (Some(to), Some(seq)) = (uint(m.get("to")), uint(m.get("seq"))) else { return };
    let Some(c) = number(m.get("c")).filter(|c| c.fract() == 0.0 && *c >= 1.0 && *c <= 255.0) else { return };
    let Some(k) = number(m.get("k")).filter(|k| k.fract() == 0.0 && *k >= 0.0 && *k <= 4_294_967_295.0) else { return };
    let Some(s) = numbers::<9>(m.get("s")) else { return };
    if seq <= p.cop_hit_seq || number(m.get("ts")).is_none() || !members[i].allow(Limit::CopHit, now) {
        return;
    }
    let Some(j) = members.iter().position(|q| q.id as f64 == to && q.named && q.cops) else { return };
    if j == i {
        return;
    }
    let (p, q) = (&members[i], &members[j]);
    let Some((at, list)) = &q.chase else { return };
    let Some(cop) = list.iter().find(|x| x.0 as f64 == c) else { return };
    let Some(pp) = &p.pose else { return };
    if now.duration_since(*at) > COP_FRESH || cop.1 as f64 != k || pp.m != 1.0 || now.duration_since(pp.at) > COP_FRESH
        || distance(&s[0..3], &cop.2) > COP_REACH || distance(&s[0..3], &pp.s[4..7]) > COP_REACH
        || length3(s[3], s[4], s[5]) > 100.0 || length3(s[6], s[7], s[8]) > 50.0
    {
        return;
    }
    let from = p.id;
    members[i].cop_hit_seq = seq;
    let msg = format!(
        r#"{{"t":"cophit","id":{from},"c":{},"seq":{},"k":{},"ts":{},"s":{}}}"#,
        json(m.get("c")),
        json(m.get("seq")),
        json(m.get("k")),
        json(m.get("ts")),
        json(m.get("s"))
    );
    members[j].out.push(0, text_frame(msg));
}

struct Player {
    app: Arc<App>,
    room: String,
    id: u64,
    joined: Instant,
    named: bool,
}

impl Session for Player {
    fn text(&mut self, text: &str) {
        lock(&self.app.relay).handle(&self.room, self.id, text);
    }

    fn binary(&mut self, data: Vec<u8>) {
        lock(&self.app.relay).handle_binary(&self.room, self.id, &data);
    }

    fn alive(&mut self, now: Instant) -> bool {
        if !self.named {
            self.named = lock(&self.app.relay).named(&self.room, self.id);
        }
        self.named || now.duration_since(self.joined) < HELLO_TIMEOUT
    }

    fn leave(&mut self) {
        lock(&self.app.relay).leave(&self.room, self.id);
    }
}

/// A player's connection to a room, from the 101 answer to its departure.
pub async fn join(app: Arc<App>, conn: Conn, room: String, accept: String) {
    let out = Outbox::new(MAX_PENDING);
    // A full room: the connection is closed without an answer, as the Node relay does.
    let Some(id) = lock(&app.relay).admit(&room, out.clone()) else { return };
    let Some((rd, buf, writer)) = ws::open(conn, &accept, &out).await else {
        lock(&app.relay).leave(&room, id);
        return;
    };
    let mut player = Player { app, room, id, joined: Instant::now(), named: false };
    ws::run(rd, buf, out, writer, &mut player).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn headings_and_rounding_match_javascript() {
        let q = std::f64::consts::FRAC_1_SQRT_2;
        let mut s = [0.0; 13];
        s[3] = 1.0;
        assert_eq!(heading(&s), 0);
        s[1] = q;
        s[3] = q;
        assert_eq!(heading(&s), 90);
        s[1] = -q;
        assert_eq!(heading(&s), 270);
        assert_eq!([js_round(-40.5), js_round(12.6), js_round(-7.4), js_round(2.5)], [-40.0, 13.0, -7.0, 3.0]);
        assert_eq!(crew_key(" Night Owls "), "nightowls");
    }

    #[test]
    fn compact_pose_numbers_match_javascript() {
        let written = [decimal(70711, 5), decimal(-5, 5), decimal(100000, 5), decimal(0, 3), decimal(-47750, 3)];
        assert_eq!(written, ["0.70711", "-0.00005", "1", "0", "-47.75"]);
        assert_eq!([json_number(-4085, 1), json_number(32767, 12), json_number(-13, 8)], ["-0.12467", "65.534", "-0.13"]);
        assert_eq!([to_u32(-5.0), to_u32(1500.7), to_u32(4294967299.0), to_u32(f64::NAN)], [4294967291, 1500, 3, 0]);
        assert_eq!([js_round(0.49999999999999994), js_round(-0.5), js_round(-16383.5)], [0.0, 0.0, -16383.0]);
        let mut s = [0.0; 13];
        (s[0], s[1], s[4], s[9], s[12]) = (0.5, -0.5, 1e7, -1e300, -0.0001);
        let n = quantise(&s);
        assert_eq!((n[0], n[1], n[4], n[9], n[12]), (16384, -16383, 2147483647, -32767, 0));
    }

    #[test]
    fn cops_frames_are_checked() {
        let frame = |cops: &[(u8, i32)]| {
            let mut b = vec![BIN_COPS, cops.len() as u8, 0, 0, 0, 0, 0xd0, 0x07, 0, 0];
            for &(c, x) in cops {
                let mut cop = [0u8; COP_BYTES];
                cop[0] = c;
                cop[2..6].copy_from_slice(&0x0c0c0au32.to_le_bytes());
                cop[14..18].copy_from_slice(&x.to_le_bytes());
                cop[18..22].copy_from_slice(&(-2500i32).to_le_bytes());
                b.extend_from_slice(&cop);
            }
            b
        };
        let list = read_cops(&frame(&[(3, 12000), (9, -500)])).unwrap();
        assert_eq!(list.len(), 2);
        assert_eq!((list[0].0, list[0].1, list[0].2), (3, 0x0c0c0a, [12.0, -2.5, 0.0]));
        assert_eq!(list[1].2[0], -0.5);
        assert_eq!(read_cops(&frame(&[])).unwrap().len(), 0);
        assert!(read_cops(&frame(&[(0, 1)])).is_none(), "number 0");
        assert!(read_cops(&frame(&[(4, 1), (4, 2)])).is_none(), "a number twice");
        let nine: Vec<(u8, i32)> = (1..=9).map(|c| (c, 0)).collect();
        assert!(read_cops(&frame(&nine)).is_none(), "more than COPS_MAX");
        let mut long = frame(&[(1, 0)]);
        long.push(0);
        assert!(read_cops(&long).is_none(), "wrong size");
    }

    #[test]
    fn speeds_are_capped() {
        let mut s = [0.0; 13];
        s[7] = 300.0;
        assert_eq!(speed(&s), [100.0, 0.0, 0.0]);
        s[7] = 30.0;
        assert_eq!(speed(&s), [30.0, 0.0, 0.0]);
    }
}
