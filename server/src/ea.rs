//! Stand-ins for EA's online services at 10.77.0.1 (plans/lan-protocol.md, phase 4): just
//! enough of FESL (TCP 18210), Theater (TCP and UDP 18215) and EA Messenger (TCP 13505) for
//! Carbon's own Online mode to log in, look for a Custom Match (PlayNow) and join a game that
//! a host has made. Unclaimed names stay open. An explicit claim can protect a name with a
//! cloud-save proof. Passwords and CD keys are never read, kept or written (their values are
//! dropped when a packet is parsed and masked in the trace).
//!
//! Theater is the game browser, and the one stand-in that links players: a host's CGAM makes
//! a game (Games, below) that GLST/GDAT list to everyone and PlayNow offers (JOIN); a
//! joiner's EGAM is passed to the host's connection as EGRQ, the host's EGRS answer goes back
//! to the joiner as EGEG with the host's address and the game's tunnel key, and from then on
//! the two games talk to each other directly over UDP (port 1042, routed by the switch). So a
//! stand-in's answer is not always for the connection that asked: Event::Push names another
//! connection. UDP ECHO on 18215 tells the game the address and port we see it at, as EA's
//! server did for NAT.
//!
//! Carbon's client itself never hosts (see playnow_status): EA ran every race on its own host
//! program, which is gone. Until something hosts, PlayNow answers NOMATCH, and with
//! CARBON_TRACE_GAME a game "hosted" by the trace host records what a joining game sends.
//!
//! All three services frame packets the same way (EA's "Aries" framing; checked against the
//! game, ProtoAries 00977f40-009781f3, and its first Hello in our trace):
//!   [0..4] a 4-character type ("fsys", "acct", "CONN", "AUTH"..),
//!   [4..8] a big-endian word: FESL's transaction id with the packet kind in the top bits
//!          (0x8 = a whole transaction, 0xb = one fragment of a long one), 0 elsewhere,
//!   [8..12] the big-endian length of the whole packet, header included (12 to 0x8000),
//!   then "key=value\n" lines and a NUL. Values with spaces are quoted; '=' and ':' inside
//!   values are written %3d and %3a.
//!
//! FESL replies carry the request's id; a transaction the server starts (MemCheck) has id
//! 0x80000000. Long transactions arrive as 0xb fragments, each "size=<base64 length>" and
//! "data=<a piece of the base64 text>" ('=' written %3d), answered once whole. Theater
//! replies echo TID=, Messenger replies ID=. The replies follow EA's own, as the March 2021
//! captures that ship with xan1242's NFSC_MasterServer show them (used as reference data
//! only; none of its GPL code is used here).

use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::host::{Expected, Host};
use crate::identity::{Claims, Owner};
use crate::messenger::{KeyDigest, Messenger};
use crate::stats::{num, Period, Stats};
use crate::util::{base64, base64_decode, http_date, lock};

/// A connection to a stand-in, as the trace host keys it: (client vip, client port, our port,
/// connection id).
pub type ConnKey = (u32, u16, u16, u32);

/// Theater's one lobby, as EA had: "Internet", id 257.
pub const LOBBY: u32 = 257;
/// Games open at once on this server, and attribute lines kept per game.
const MAX_GAMES: usize = 256;
const MAX_ATTRS: usize = 96;

pub const FESL_PORT: u16 = 18210;
pub const THEATER_PORT: u16 = 18215;
pub const MESSENGER_PORT: u16 = 13505;

pub const HEADER: usize = 12;
/// The game's own limit for one packet, header included (009780e3).
pub const MAX_PACKET: usize = 0x8000;
/// A long FESL transaction put back together from its fragments.
const MAX_JOINED: usize = 256 * 1024;
/// Logins remembered for Theater and Messenger, which name the player by its login key.
const MAX_LOGINS: usize = 1024;

/// Keys whose values are dropped when parsed and masked in the trace: Login sends the
/// password in clear text (FESL had SSL for that); the others are for safety.
const SECRETS: [&str; 7] = ["password", "encryptedInfo", "cdKey", "cdkey", "newPassword", "oldPassword", "serialNumber"];

fn secret(key: &str) -> bool {
    SECRETS.iter().any(|s| key.eq_ignore_ascii_case(s))
}

/// Kept (Messenger relays them) but never shown: what players write to each other.
const PRIVATE: [&str; 2] = ["BODY", "SUBJ"];

fn hidden(key: &str) -> bool {
    secret(key) || PRIVATE.iter().any(|s| key.eq_ignore_ascii_case(s))
}

/// How long a stand-in connection may be quiet before the stand-in pings it (EA's servers
/// pinged: FESL `fsys` Ping, Theater and Messenger PING in the March 2021 captures; the
/// game answers them). The game's FESL connection breaks after 180 s without traffic.
pub const PING_AFTER_SECS: u64 = 60;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Service {
    Fesl,
    Theater,
    Messenger,
}

impl Service {
    pub fn on(port: u16) -> Option<Service> {
        match port {
            FESL_PORT => Some(Service::Fesl),
            THEATER_PORT => Some(Service::Theater),
            MESSENGER_PORT => Some(Service::Messenger),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Service::Fesl => "fesl",
            Service::Theater => "theater",
            Service::Messenger => "messenger",
        }
    }
}

/// One packet, its lines in order.
#[derive(Clone, Debug, PartialEq)]
pub struct Packet {
    pub kind: [u8; 4],
    pub id: u32,
    pub fields: Vec<(String, String)>,
}

impl Packet {
    pub fn new(kind: &[u8; 4], id: u32) -> Packet {
        Packet { kind: *kind, id, fields: Vec::new() }
    }

    pub fn with(mut self, key: &str, value: impl ToString) -> Packet {
        self.fields.push((key.to_string(), value.to_string()));
        self
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.fields.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    pub fn kind_str(&self) -> String {
        String::from_utf8_lossy(&self.kind).into_owned()
    }

    /// Header, lines, NUL.
    pub fn encode(&self) -> Vec<u8> {
        let mut body = String::new();
        for (k, v) in &self.fields {
            body.push_str(k);
            body.push('=');
            if v.contains(' ') {
                body.push('"');
                body.push_str(v);
                body.push('"');
            } else {
                body.push_str(v);
            }
            body.push('\n');
        }
        let len = HEADER + body.len() + 1;
        let mut out = Vec::with_capacity(len);
        out.extend_from_slice(&self.kind);
        out.extend_from_slice(&self.id.to_be_bytes());
        out.extend_from_slice(&(len as u32).to_be_bytes());
        out.extend_from_slice(body.as_bytes());
        out.push(0);
        out
    }

    /// A whole packet as the reader cut it (header included).
    pub fn decode(raw: &[u8]) -> Packet {
        let mut p = Packet::new(&[raw[0], raw[1], raw[2], raw[3]], u32::from_be_bytes([raw[4], raw[5], raw[6], raw[7]]));
        p.fields = parse_lines(&raw[HEADER..]);
        p
    }

    /// Every line, for a trace file: "key=value", secret values written as "(not kept)".
    pub fn lines(&self) -> String {
        let mut s = String::new();
        for (k, v) in &self.fields {
            s.push_str("    ");
            s.push_str(k);
            s.push('=');
            s.push_str(if secret(k) {
                "(not kept)"
            } else if hidden(k) {
                "(private)"
            } else {
                v
            });
            s.push('\n');
        }
        s
    }

    /// One line for logs: type, id and the lines, secrets masked, long values cut.
    pub fn summary(&self) -> String {
        let mut s = format!("{} {:#010x} {{", self.kind_str(), self.id);
        for (i, (k, v)) in self.fields.iter().enumerate() {
            if i > 0 {
                s.push_str(", ");
            }
            let v = if secret(k) {
                "(not kept)".to_string()
            } else if hidden(k) {
                "(private)".to_string()
            } else if v.chars().count() > 60 {
                format!("{}...", v.chars().take(60).collect::<String>())
            } else {
                v.clone()
            };
            s.push_str(&format!("{k}={v}"));
            if s.len() > 600 {
                s.push_str(", ...");
                break;
            }
        }
        s.push('}');
        s
    }
}

/// "key=value" lines up to the first NUL; quotes around a value are taken off; secret
/// values are dropped here, so nothing after this ever holds them.
fn parse_lines(body: &[u8]) -> Vec<(String, String)> {
    let end = body.iter().position(|&b| b == 0).unwrap_or(body.len());
    let text = String::from_utf8_lossy(&body[..end]);
    let mut out = Vec::new();
    for line in text.split('\n') {
        let line = line.trim_end_matches('\r');
        if line.is_empty() {
            continue;
        }
        let (k, v) = line.split_once('=').unwrap_or((line, ""));
        let v = if v.len() >= 2 && v.starts_with('"') && v.ends_with('"') { &v[1..v.len() - 1] } else { v };
        out.push((k.to_string(), if secret(k) { String::new() } else { v.to_string() }));
    }
    out
}

/// A copy of a raw packet for the trace, every secret or private value replaced by '*'.
pub fn masked(raw: &[u8]) -> Vec<u8> {
    let mut out = raw.to_vec();
    let mut start = HEADER.min(out.len());
    while start < out.len() {
        let end = out[start..].iter().position(|&b| b == b'\n' || b == 0).map_or(out.len(), |i| start + i);
        let line = &out[start..end];
        if let Some(eq) = line.iter().position(|&b| b == b'=') {
            if hidden(&String::from_utf8_lossy(&line[..eq])) {
                out[start + eq + 1..end].fill(b'*');
            }
        }
        start = end + 1;
    }
    out
}

/// Cuts a TCP byte stream into packets.
#[derive(Default)]
pub struct Reader {
    buf: Vec<u8>,
}

impl Reader {
    pub fn push(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    /// The next whole packet, None until one is in, or an error for a length the game would
    /// never send (the stream cannot be followed after that).
    pub fn next(&mut self) -> Result<Option<Vec<u8>>, String> {
        if self.buf.len() < HEADER {
            return Ok(None);
        }
        let len = u32::from_be_bytes([self.buf[8], self.buf[9], self.buf[10], self.buf[11]]) as usize;
        if !(HEADER..=MAX_PACKET).contains(&len) {
            return Err(format!("a packet length of {len} bytes (12 to {MAX_PACKET} expected)"));
        }
        if self.buf.len() < len {
            return Ok(None);
        }
        Ok(Some(self.buf.drain(..len).collect()))
    }

    /// Bytes of a packet not yet complete.
    pub fn pending(&self) -> usize {
        self.buf.len()
    }
}

/// A player logged in to FESL. Theater and Messenger learn who it is from the login key.
#[derive(Clone, Debug, PartialEq)]
pub struct User {
    pub name: String,
    pub id: u32,
    pub lkey: String,
}

/// The login keys handed out, oldest first out.
#[derive(Default)]
pub struct Logins {
    by_key: HashMap<String, User>,
    order: VecDeque<String>,
    seq: u64,
}

/// A name for the game's menus: letters, digits, '_', '-' and '.', at most 16; "Player" if
/// nothing is left.
pub fn clean_name(name: &str) -> String {
    let n: String = name.chars().filter(|c| c.is_ascii_alphanumeric() || "_-.".contains(*c)).take(16).collect();
    if n.is_empty() {
        "Player".into()
    } else {
        n
    }
}

fn fnv1a(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| (h ^ b as u64).wrapping_mul(0x0100_0000_01b3))
}

/// A stable id per name (the same name is the same player in every session), nine digits as
/// EA's were.
pub fn user_id(name: &str) -> u32 {
    100_000_000 + (fnv1a(name.to_ascii_lowercase().as_bytes()) % 900_000_000) as u32
}

impl Logins {
    pub fn login(&mut self, name: &str) -> User {
        self.seq += 1;
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
        let mut seed = fnv1a(format!("{name}/{}/{now}", self.seq).as_bytes());
        const ABC: &[u8; 62] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        let mut lkey = String::with_capacity(27);
        for _ in 0..26 {
            seed = seed.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1_442_695_040_888_963_407);
            lkey.push(ABC[(seed >> 33) as usize % 62] as char);
        }
        lkey.push('.'); // EA's keys ended in '.'
        let user = User { name: name.to_string(), id: user_id(name), lkey: lkey.clone() };
        if self.order.len() >= MAX_LOGINS {
            if let Some(old) = self.order.pop_front() {
                self.by_key.remove(&old);
            }
        }
        self.order.push_back(lkey.clone());
        self.by_key.insert(lkey, user.clone());
        user
    }

    pub fn find(&self, lkey: &str) -> Option<&User> {
        self.by_key.get(lkey)
    }
}

/// What one TCP segment from the game led to, in order, for the trace and the wire.
#[derive(Debug)]
pub enum Event {
    /// A packet from the game: the raw bytes with secrets masked, and its summary.
    In(Vec<u8>, String),
    /// A fragment of a long transaction (its data is not written out).
    Fragment(usize, String),
    /// A packet to the game.
    Out(Vec<u8>, String),
    /// A long transaction put back together, every line (secrets already dropped), for the
    /// trace file only: the summary on stdout is cut.
    Joined(String),
    /// A packet to another connection of the same stand-in (Theater: a join request to the
    /// host, the host's address to the joiner).
    Push(ConnKey, Vec<u8>, String),
    /// A note for the trace and stdout (an unknown request, ...).
    Note(String),
}

/// Everything the stand-ins share between connections: who logged in, and Theater's games.
#[derive(Default)]
pub struct Hub {
    pub logins: Logins,
    pub games: Games,
    /// Theater connections that said who they are (USER), for PCNT.
    theater_users: HashSet<ConnKey>,
    rng: u64,
    /// CARBON_TRACE_GAME: when no player's game is open, PlayNow sends players to a game
    /// "hosted" by the trace host at 10.77.0.1, so what a joining game sends its host
    /// (GameManager over UDP 1042) is recorded. Carbon's client never hosts a game itself
    /// (its PlayNow listeners ignore CREATE), so this is how its host protocol is traced.
    pub trace_game: bool,
    /// CARBON_HOST_GAME: the server hosts a race game of its own (host.rs), as EA's
    /// dedicated hosts did, and PlayNow sends players there when no player's game is open.
    pub host_game: bool,
    /// The games this server hosts, by game id.
    pub hosts: BTreeMap<u32, Host>,
    /// The races the game offers per kind (race_type_circuit ...), as its PlayNow said.
    pub races: BTreeMap<String, Vec<String>>,
    /// EA Messenger: friends, presence, chat, invites (messenger.rs).
    pub messenger: Messenger,
    /// Each page's non-authoritative browser key (lan.js), as its SHA-256, by vip. Messenger
    /// and Player Stats keep a short fingerprint as metadata; this key never owns a name.
    pub player_keys: HashMap<u32, KeyDigest>,
    /// The cloud-keyphrase owner each page proves in lan.js's hello. The raw cloud-save id is
    /// never kept here.
    pub player_claims: HashMap<u32, Owner>,
    /// Explicit name claims, shared with /names and the free-roam relay.
    pub identities: Arc<Mutex<Claims>>,
    /// Player Stats and the Leaderboard: FESL's rank service (stats.rs).
    pub stats: Stats,
}

/// The trace host's game (CARBON_TRACE_GAME): its id, and the port its "host" listens on.
pub const TRACE_GID: u32 = 100_000;
const TRACE_GAME_PORT: u16 = 1042;
/// The server's own race games (CARBON_HOST_GAME): ids from here, each on its own UDP port
/// of 10.77.0.1 (EA's hosts used ports like 19018).
pub const SERVED_GID: u32 = 200_000;
const SERVED_PORT: u16 = 19_000;
/// The name the server's games show as their host.
pub const SERVED_HOST: &str = "RaceHost";

impl Hub {
    /// Random-looking numbers for tickets and keys (not secrets: they only tell the two games
    /// of one match apart from the others on this server).
    fn next(&mut self) -> u64 {
        if self.rng == 0 {
            let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(1);
            self.rng = fnv1a(format!("{now}").as_bytes()) | 1;
        }
        self.rng = self.rng.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1_442_695_040_888_963_407);
        self.rng >> 11
    }

    fn bytes(&mut self, n: usize) -> Vec<u8> {
        (0..n).map(|_| self.next() as u8).collect()
    }

    /// The game a player is in, picked by its name and user id: the race host's room it is
    /// in, or a game it hosts or was let into. An EA Messenger invite names its sender, not
    /// the game, so the invited game asks Theater for the sender's room: its details by
    /// name (GDAT USER=Metin), then the join by user id (EGAM UID=...), both seen in game.
    fn game_of(&self, is: &dyn Fn(&str, u64) -> bool) -> Option<u32> {
        let player = |u: &User| is(&u.name, u64::from(u.id));
        self.games
            .by_gid
            .values()
            .find(|g| {
                if g.served {
                    self.hosts.get(&g.gid).is_some_and(|h| h.has_player(is))
                } else {
                    !g.traced && (player(&g.host_user) || g.members.iter().any(|m| m.entered && player(&m.user)))
                }
            })
            .map(|g| g.gid)
    }

    /// The players in a game, for PDAT: (player id, name, user id). The race host's room
    /// lists the players it has (not itself); a player's game, its host (id 1) and those let in.
    fn players_of(&self, gid: u32) -> Vec<(u32, String, u64)> {
        let Some(g) = self.games.by_gid.get(&gid) else { return Vec::new() };
        if g.served {
            return self.hosts.get(&gid).map(Host::roster).unwrap_or_default();
        }
        let mut list = vec![(1, g.host_user.name.clone(), u64::from(g.host_user.id))];
        list.extend(g.members.iter().filter(|m| m.entered).map(|m| (m.pid, m.user.name.clone(), u64::from(m.user.id))));
        list
    }

    /// A connection ended: the games it hosted close, a join it had pending is dropped, and
    /// the players still in its game learn nothing new (their game sees the host go from the
    /// peer traffic stopping).
    pub fn closed(&mut self, key: ConnKey) -> Vec<Event> {
        self.theater_users.remove(&key);
        self.messenger.closed(key);
        let mut ev = Vec::new();
        let hosted: Vec<u32> = self.games.by_gid.values().filter(|g| g.host == key).map(|g| g.gid).collect();
        for gid in hosted {
            if let Some(g) = self.games.by_gid.remove(&gid) {
                ev.push(Event::Note(format!("game {gid} \"{}\" closed: its host left", g.name)));
            }
        }
        for g in self.games.by_gid.values_mut() {
            let before = g.members.len();
            g.members.retain(|m| m.conn != key);
            if g.members.len() != before {
                ev.push(Event::Note(format!("a player left game {}", g.gid)));
            }
        }
        ev
    }
}

/// A player in a game, from its EGAM on.
struct Member {
    pid: u32,
    user: User,
    conn: ConnKey,
    ticket: String,
    /// The host said it entered (PENT).
    entered: bool,
}

/// A game a player hosts (CGAM), as Theater lists it.
pub struct Game {
    pub gid: u32,
    /// The host's Theater connection (for the trace host's game: 10.77.0.1, no connection).
    host: ConnKey,
    /// Hosted by the trace host (CARBON_TRACE_GAME): joins are let in at once.
    traced: bool,
    /// Hosted by this server's race host (host.rs): joins are let in at once and expected
    /// on its port.
    served: bool,
    host_user: User,
    name: String,
    /// The host's game port as it gave it: the port our ECHO answer told it.
    port: String,
    int_ip: String,
    int_port: String,
    max_players: u32,
    ugid: String,
    secret: String,
    ekey: String,
    /// "O" open, "W" wait, "C" closed.
    join: String,
    password: bool,
    version: String,
    /// The game's own attributes, B-* (B-version, B-U-track, ...), as last sent.
    attrs: Vec<(String, String)>,
    /// Its detailed attributes, D-* (UGDE).
    details: Vec<(String, String)>,
    members: Vec<Member>,
    next_pid: u32,
}

impl Game {
    /// Players in the game: the host, and every joiner the host let in.
    fn active(&self) -> usize {
        1 + self.members.iter().filter(|m| m.entered).count()
    }

    fn joining(&self) -> usize {
        self.members.iter().filter(|m| !m.entered).count()
    }

    /// GDAT: one game as the list shows it (EA's fields, from the March 2021 captures).
    fn gdat(&self, tid: &str) -> Packet {
        let mut p = Packet::new(b"GDAT", 0)
            .with("TID", tid)
            .with("LID", LOBBY)
            .with("GID", self.gid)
            .with("HN", &self.host_user.name)
            .with("HU", self.host_user.id)
            .with("N", &self.name)
            .with("I", Ipv4Addr::from(self.host.0))
            .with("P", &self.port)
            .with("JP", self.joining())
            .with("QP", 0)
            .with("AP", self.active())
            .with("MP", self.max_players)
            .with("PL", "PC")
            .with("F", 0)
            .with("NF", 0)
            .with("J", &self.join)
            .with("TYPE", "G")
            .with("PW", u8::from(self.password))
            .with("V", &self.version);
        for (k, v) in &self.attrs {
            p = p.with(k, v);
        }
        p
    }

    fn set_attrs(&mut self, p: &Packet) {
        for (k, v) in &p.fields {
            let list = if k.starts_with("B-") {
                &mut self.attrs
            } else if k.starts_with("D-") {
                &mut self.details
            } else {
                continue;
            };
            if let Some(slot) = list.iter_mut().find(|(key, _)| key == k) {
                slot.1 = v.clone();
            } else if list.len() < MAX_ATTRS {
                list.push((k.clone(), v.clone()));
            }
        }
        if let Some(n) = p.get("MAX-PLAYERS").and_then(|n| n.parse().ok()) {
            self.max_players = n;
        }
        if let Some(j) = p.get("JOIN").filter(|j| !j.is_empty()) {
            self.join = j.to_string();
        }
        if let Some(n) = p.get("NAME").filter(|n| !n.is_empty()) {
            self.name = n.to_string();
        }
    }
}

/// Theater's games, by id.
#[derive(Default)]
pub struct Games {
    by_gid: BTreeMap<u32, Game>,
    last_gid: u32,
    /// PlayNow session ids handed out.
    last_session: u32,
}

impl Games {
    pub fn len(&self) -> usize {
        self.by_gid.len()
    }

    /// The race host's word on whether its game takes new players (its leader's
    /// DisableJoins/EnableJoins): Theater lists it closed or open, and PlayNow follows.
    pub fn set_open(&mut self, gid: u32, open: bool) {
        if let Some(g) = self.by_gid.get_mut(&gid) {
            g.join = if open { "O" } else { "C" }.into();
        }
    }

    /// The trace host's game, made once: attributes that match any Custom Match.
    fn trace_game(&mut self) {
        if self.by_gid.contains_key(&TRACE_GID) {
            return;
        }
        let host = User { name: "TraceHost".into(), id: user_id("TraceHost"), lkey: String::new() };
        let mut g = Game {
            gid: TRACE_GID,
            host: (crate::lan::HOST, 0, THEATER_PORT, 0),
            traced: true,
            served: false,
            host_user: host,
            name: "TraceHost".into(),
            port: TRACE_GAME_PORT.to_string(),
            int_ip: "10.77.0.1".into(),
            int_port: TRACE_GAME_PORT.to_string(),
            max_players: 8,
            ugid: "00000000-0000-4000-a000-000000100000".into(),
            secret: String::new(),
            ekey: "AAAAAAAAAAAAAAAAAAAAAA%3d%3d".into(),
            join: "O".into(),
            password: false,
            version: "2.0".into(),
            attrs: Vec::new(),
            details: Vec::new(),
            members: Vec::new(),
            next_pid: 2,
        };
        for (k, v) in [("B-version", "298_prod_server+22012b18"), ("B-U-version", "298_prod_server+22012b18"), ("B-U-game_type", "1"), ("B-U-matchmaking_state", "0")] {
            g.attrs.push((k.into(), v.into()));
        }
        self.by_gid.insert(TRACE_GID, g);
    }

    /// An open game of the server's own (CARBON_HOST_GAME) for a player to join: the first
    /// with room, else a new one, with its race host. Its tunnel key is random, as EA's were.
    fn served_game(&mut self, hosts: &mut BTreeMap<u32, Host>, ekey: String) -> u32 {
        // A room whose race is on cannot take a player (its host would refuse it): the
        // player gets a room of its own instead, and waits there as its leader.
        let open = |g: &Game| {
            g.served && hosts.get(&g.gid).is_some_and(|h| !h.in_race()) && hosts.get(&g.gid).map_or(0, |h| h.players()) + g.joining() < g.max_players as usize
        };
        if let Some(g) = self.by_gid.values().find(|g| open(g)) {
            return g.gid;
        }
        let n = (0..1000u32).find(|n| !self.by_gid.contains_key(&(SERVED_GID + n))).unwrap_or(0);
        let gid = SERVED_GID + n;
        let port = SERVED_PORT + n as u16;
        let host = User { name: SERVED_HOST.into(), id: user_id(SERVED_HOST), lkey: String::new() };
        let mut g = Game {
            gid,
            host: (crate::lan::HOST, 0, THEATER_PORT, 0),
            traced: false,
            served: true,
            host_user: host.clone(),
            name: format!("{SERVED_HOST}{}", n + 1),
            port: port.to_string(),
            int_ip: "10.77.0.1".into(),
            int_port: port.to_string(),
            max_players: 8,
            ugid: format!("00000000-0000-4000-a000-{:012}", gid),
            secret: String::new(),
            ekey: ekey.clone(),
            join: "O".into(),
            password: false,
            version: "2.0".into(),
            attrs: Vec::new(),
            details: Vec::new(),
            members: Vec::new(),
            next_pid: 2,
        };
        for (k, v) in [("B-version", "298_prod_server+22012b18"), ("B-U-version", "298_prod_server+22012b18"), ("B-U-game_type", "1"), ("B-U-matchmaking_state", "0")] {
            g.attrs.push((k.into(), v.into()));
        }
        hosts.insert(gid, Host::new(gid, port, host.id as u64, &ekey));
        self.by_gid.insert(gid, g);
        gid
    }
}

/// One connection to a stand-in.
pub struct Session {
    pub service: Service,
    /// This connection, so its games and joins can be found again.
    key: ConnKey,
    reader: Reader,
    /// FESL fragments by transaction id: (whole base64 length, the base64 so far)
    joining: HashMap<u32, (usize, String)>,
    user: Option<User>,
    /// Theater: the version the game said in CONN (GDAT's V).
    version: String,
    /// Theater: an open UBRA bracket's TIDs, answered when it closes.
    bracket: Vec<String>,
    /// FESL's Goodbye: the stand-in closes after answering.
    pub closing: bool,
}

/// "Mar-17-2021 20%3a22%3a55 UTC", as FESL's Hello sent curTime.
fn fesl_time(t: SystemTime) -> String {
    // http_date: "Sat, 24 Oct 2026 17:05:09 GMT"
    let d = http_date(t);
    let p: Vec<&str> = d.split(' ').collect();
    if p.len() < 5 {
        return String::new();
    }
    format!("{}-{}-{} {} UTC", p[2], p[1], p[3], p[4].replace(':', "%3a"))
}

fn unix_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

impl Session {
    pub fn new(service: Service, key: ConnKey) -> Session {
        Session {
            service,
            key,
            reader: Reader::default(),
            joining: HashMap::new(),
            user: None,
            version: "2.0".into(),
            bracket: Vec::new(),
            closing: false,
        }
    }

    pub fn pending(&self) -> usize {
        self.reader.pending()
    }

    /// Bytes from the game: the packets they complete and the answers. An error means the
    /// stream is broken and the connection should be reset.
    pub fn feed(&mut self, data: &[u8], hub: &mut Hub) -> Result<Vec<Event>, String> {
        self.reader.push(data);
        let mut events = Vec::new();
        while let Some(raw) = self.reader.next()? {
            let p = Packet::decode(&raw);
            if self.service == Service::Fesl && p.id >> 28 == 0xb {
                let piece = p.get("data").unwrap_or("").replace("%3d", "=");
                let size = p.get("size").and_then(|s| s.parse::<usize>().ok()).unwrap_or(0);
                events.push(Event::Fragment(raw.len(), format!("{} {:#010x} {{size={size}, data=({} bytes)}}", p.kind_str(), p.id, piece.len())));
                let tid = p.id & 0x00ff_ffff;
                let entry = self.joining.entry(tid).or_insert((size, String::new()));
                entry.1.push_str(&piece);
                if entry.1.len() > MAX_JOINED.min(entry.0.max(1) * 2) {
                    return Err(format!("transaction {tid} grew past its size"));
                }
                if entry.1.len() < entry.0 {
                    continue;
                }
                let (_, text) = self.joining.remove(&tid).expect("just used");
                let Some(body) = base64_decode(&text) else {
                    events.push(Event::Note(format!("transaction {tid}: its fragments are not base64; not answered")));
                    continue;
                };
                let mut whole = Packet::new(&p.kind, 0x8000_0000 | tid);
                whole.fields = parse_lines(&body);
                events.push(Event::Note(format!("joined {}", whole.summary())));
                events.push(Event::Joined(whole.lines()));
                self.answer(&whole, hub, &mut events);
            } else {
                events.push(Event::In(masked(&raw), p.summary()));
                self.answer(&p, hub, &mut events);
            }
        }
        Ok(events)
    }

    fn send(events: &mut Vec<Event>, p: Packet) {
        events.push(Event::Out(p.encode(), p.summary()));
    }

    /// The keep-alive for a quiet connection, as EA's servers sent it: FESL's Ping is not a
    /// transaction (id 0; the game answers with id 0x80000000), Theater's and Messenger's
    /// PING have no fields (the game answers PING).
    pub fn ping(&self) -> (Vec<u8>, String) {
        let p = match self.service {
            Service::Fesl => Packet::new(b"fsys", 0).with("TXN", "Ping"),
            Service::Theater | Service::Messenger => Packet::new(b"PING", 0),
        };
        (p.encode(), p.summary())
    }

    fn answer(&mut self, p: &Packet, hub: &mut Hub, ev: &mut Vec<Event>) {
        match self.service {
            Service::Fesl => self.fesl(p, hub, ev),
            Service::Theater => self.theater(p, hub, ev),
            Service::Messenger => self.messenger(p, hub, ev),
        }
    }

    fn push(events: &mut Vec<Event>, to: ConnKey, p: Packet) {
        events.push(Event::Push(to, p.encode(), p.summary()));
    }

    fn fesl(&mut self, p: &Packet, hub: &mut Hub, ev: &mut Vec<Event>) {
        let txn = p.get("TXN").unwrap_or("").to_string();
        let reply = || Packet::new(&p.kind, p.id).with("TXN", &txn);
        match (&p.kind, txn.as_str()) {
            (b"fsys", "Hello") => {
                let r = reply()
                    .with("domainPartition.domain", "eagames")
                    .with("domainPartition.subDomain", "NFS-2007")
                    .with("messengerIp", "messaging.ea.com")
                    .with("messengerPort", MESSENGER_PORT)
                    .with("theaterIp", "nfs-pc.theater.ea.com")
                    .with("theaterPort", THEATER_PORT)
                    .with("activityTimeoutSecs", 0)
                    .with("curTime", fesl_time(SystemTime::now()));
                Self::send(ev, r);
                // EA's server then asked for a memory check; an empty one is what it sent.
                let salt = (fnv1a(format!("{}", unix_secs()).as_bytes()) % 2_000_000_000) as u32;
                Self::send(
                    ev,
                    Packet::new(b"fsys", 0x8000_0000)
                        .with("TXN", "MemCheck")
                        .with("memcheck.[]", 0)
                        .with("type", 0)
                        .with("salt", salt),
                );
            }
            // The game's answer to MemCheck: nothing to say.
            (b"fsys", "MemCheck") => {}
            // The game's own Ping is answered; id 0x80000000 is its answer to ours.
            (b"fsys", "Ping") if p.id != 0x8000_0000 => Self::send(ev, reply()),
            (b"fsys", "Ping") => {}
            (b"fsys", "Goodbye") => self.closing = true,
            (b"fsys", "GetPingSites") => {
                // Three sites, as EA had; all on our host. The game may give up on them
                // (minPingSitesToPing=0 lets it).
                let mut r = reply().with("pingSite.[]", 3).with("minPingSitesToPing", 0);
                for (i, name) in ["nfs-wc", "nfs-eu", "nfs-ec"].iter().enumerate() {
                    r = r.with(&format!("pingSite.{i}.addr"), "10.77.0.1").with(&format!("pingSite.{i}.name"), name).with(&format!("pingSite.{i}.type"), 0);
                }
                Self::send(ev, r);
            }
            (b"acct", "Login") => {
                // The password is never looked at (it was dropped when parsed). An unclaimed
                // name stays open. A claimed name needs the cloud keyphrase's derived proof.
                let name = clean_name(p.get("name").unwrap_or(""));
                let owner = hub.player_claims.get(&self.key.0);
                let browser = hub.player_keys.get(&self.key.0).map(crate::identity::browser_digest);
                if !lock(&hub.identities).may_use(&name, owner, browser.as_deref()) {
                    ev.push(Event::Note(format!("{name} is protected: login refused without its identity proof")));
                    Self::send(ev, error_code(p, &txn, "This online name is claimed by another player", 122));
                    return;
                }
                let key = hub.player_keys.get(&self.key.0).copied();
                hub.messenger.store.use_name(&name, key.as_ref(), unix_secs());
                let user = hub.logins.login(&name);
                ev.push(Event::Note(format!("{} logged in (userId {})", user.name, user.id)));
                hub.stats.signed_in(&user.name, key.as_ref(), unix_secs());
                let r = reply()
                    .with("lkey", &user.lkey)
                    .with("profileId", user.id)
                    .with("userId", user.id)
                    .with("displayName", &user.name);
                self.user = Some(user);
                Self::send(ev, r);
            }
            (b"acct", "GetSubAccounts") => {
                let name = self.user.as_ref().map_or("Player".to_string(), |u| u.name.clone());
                Self::send(ev, reply().with("subAccounts.[]", 1).with("subAccounts.0", name));
            }
            (b"acct", "LoginSubAccount") => match self.user.clone() {
                Some(u) => Self::send(ev, reply().with("lkey", &u.lkey).with("profileId", u.id).with("userId", u.id)),
                None => Self::send(ev, error(p, &txn, "Not logged in")),
            },
            (b"dobj", "GetObjectInventory") => Self::send(ev, reply().with("entitlements.[]", 0)),
            // PlayNow, the matchmaker behind both Quick Match and Custom Match: Start answers at
            // once with a session id, then an async Status says what to do (playnow_status).
            (b"pnow", "Start") => {
                hub.games.last_session = hub.games.last_session.wrapping_add(1);
                let id = hub.games.last_session;
                Self::send(ev, reply().with("id.id", id).with("id.partition", "/eagames/NFS-2007"));
                if hub.trace_game {
                    hub.games.trace_game();
                }
                // The races the game offers for each kind (its own playnowconfig.txt, sent as
                // fitValues-race_type_*): the race host picks from these when the leader leaves
                // the track to chance.
                for (k, v) in p.fields.iter().filter(|(k, _)| k.starts_with("players.0.props.{fitValues-race_type_")) {
                    let kind = k.trim_start_matches("players.0.props.{fitValues-").trim_end_matches('}').to_string();
                    let list: Vec<String> = v.trim_matches('"').split(',').filter(|r| !r.is_empty() && *r != "ABSTAIN").take(64).map(str::to_string).collect();
                    for h in hub.hosts.values_mut() {
                        h.races.insert(kind.clone(), list.clone());
                    }
                    hub.races.insert(kind, list);
                }
                if hub.host_game && playnow_status(p, id, self.user.as_ref(), &hub.games, &hub.hosts).get("props.{resultType}") != Some("JOIN") {
                    let ekey = base64(&hub.bytes(16)).replace('=', "%3d");
                    let gid = hub.games.served_game(&mut hub.hosts, ekey);
                    if let Some(h) = hub.hosts.get_mut(&gid) {
                        for (k, v) in &hub.races {
                            h.races.entry(k.clone()).or_insert_with(|| v.clone());
                        }
                    }
                    ev.push(Event::Note(format!("PlayNow: the race host's game {gid} has room")));
                }
                let status = playnow_status(p, id, self.user.as_ref(), &hub.games, &hub.hosts);
                ev.push(Event::Note(format!("PlayNow session {id}: {}", status.get("props.{resultType}").unwrap_or("?"))));
                Self::send(ev, status);
            }
            (b"pnow", "Cancel") => Self::send(ev, reply().with("id.id", p.get("id.id").unwrap_or("0")).with("id.partition", "/eagames/NFS-2007")),
            (b"mtrx", "ReportMetrics") => Self::send(ev, reply()),
            (b"rank", _) if self.rank(p, &txn, hub, ev) => {}
            _ => {
                ev.push(Event::Note(format!("no stand-in for {} {txn}: answered with an error", p.kind_str())));
                Self::send(ev, error(p, &txn, "Not available on this server"));
            }
        }
    }

    /// FESL's rank service: Player Stats and the Leaderboard (stats.rs has what the game asks
    /// and where the numbers come from). False for a transaction it does not know.
    fn rank(&mut self, p: &Packet, txn: &str, hub: &mut Hub, ev: &mut Vec<Event>) -> bool {
        let now = unix_secs();
        let reply = || Packet::new(&p.kind, p.id).with("TXN", txn);
        let count = |k: &str, max: usize| p.get(k).and_then(|n| n.parse::<usize>().ok()).unwrap_or(0).min(max);
        let keys: Vec<String> = (0..count("keys.[]", 256)).filter_map(|i| p.get(&format!("keys.{i}"))).map(str::to_string).collect();
        let period = Period::from_id(p.get("periodId").and_then(|v| v.parse().ok()).unwrap_or(0));
        let past = p.get("periodPast").and_then(|v| v.parse::<u32>().ok()).unwrap_or(0).min(1);
        let number = |k: &str| p.get(k).and_then(|v| v.parse::<i64>().ok()).unwrap_or(0);
        // owners of type 1 are players (by user id); others (the game's own counters) have none
        let player = |id: i64, kind: i64| if kind == 1 { u32::try_from(id).ok() } else { None };
        match txn {
            "UpdateStats" => {
                match &self.user {
                    Some(u) => {
                        let (kept, left) = hub.stats.update(&u.name, p, now);
                        if kept > 0 {
                            ev.push(Event::Note(format!("{}: {kept} stat(s) kept, {left} left out", u.name)));
                        }
                    }
                    None => ev.push(Event::Note("UpdateStats before Login: nothing kept".into())),
                }
                Self::send(ev, reply());
            }
            // A player's stats: Player Stats, the online reward cards, PlayNow's skill.
            "GetStats" | "GetRankedStats" => {
                let owner = player(number("owner"), p.get("ownerType").map_or(1, |_| number("ownerType")));
                let mut r = reply().with("stats.[]", keys.len());
                if txn == "GetStats" {
                    let got = owner.map(|o| hub.stats.get(o, &keys, period, past, now)).unwrap_or_else(|| keys.iter().map(|k| (k.clone(), 0.0, String::new())).collect());
                    for (i, (k, v, t)) in got.into_iter().enumerate() {
                        r = r.with(&format!("stats.{i}.key"), k).with(&format!("stats.{i}.value"), num(v)).with(&format!("stats.{i}.text"), t);
                    }
                } else {
                    let got = owner.map(|o| hub.stats.ranked(o, &keys, period, past, now)).unwrap_or_else(|| keys.iter().map(|k| (k.clone(), 0.0, 0, String::new())).collect());
                    for (i, (k, v, rank, t)) in got.into_iter().enumerate() {
                        r = r.with(&format!("stats.{i}.key"), k).with(&format!("stats.{i}.value"), num(v)).with(&format!("stats.{i}.rank"), rank).with(&format!("stats.{i}.text"), t);
                    }
                }
                Self::send(ev, r);
            }
            // Several players at once (a friends filter): plain or with their ranks.
            "GetStatsForOwners" | "GetRankedStatsForOwners" => {
                let ranked = txn == "GetRankedStatsForOwners";
                let list = if ranked { "rankedStats" } else { "stats" };
                let n = count("owners.[]", 100);
                let mut r = reply().with(&format!("{list}.[]"), n);
                for i in 0..n {
                    let (id, kind) = (number(&format!("owners.{i}.ownerId")), number(&format!("owners.{i}.ownerType")));
                    r = r.with(&format!("{list}.{i}.ownerId"), id).with(&format!("{list}.{i}.ownerType"), kind).with(&format!("{list}.{i}.{list}.[]"), keys.len());
                    let o = player(id, kind);
                    if ranked {
                        let got = o.map(|o| hub.stats.ranked(o, &keys, period, past, now)).unwrap_or_else(|| keys.iter().map(|k| (k.clone(), 0.0, 0, String::new())).collect());
                        for (j, (k, v, rank, t)) in got.into_iter().enumerate() {
                            let f = |x: &str| format!("{list}.{i}.{list}.{j}.{x}");
                            r = r.with(&f("key"), k).with(&f("value"), num(v)).with(&f("rank"), rank).with(&f("text"), t);
                        }
                    } else {
                        let got = o.map(|o| hub.stats.get(o, &keys, period, past, now)).unwrap_or_else(|| keys.iter().map(|k| (k.clone(), 0.0, String::new())).collect());
                        for (j, (k, v, t)) in got.into_iter().enumerate() {
                            let f = |x: &str| format!("{list}.{i}.{list}.{j}.{x}");
                            r = r.with(&f("key"), k).with(&f("value"), num(v)).with(&f("text"), t);
                        }
                    }
                }
                Self::send(ev, r);
            }
            // A leaderboard's rows, minRank to maxRank (the top, or around the player's rank),
            // with more stats of each row's player for GetTopNAndStats.
            "GetTopN" | "GetTopNAndStats" => {
                let key = p.get("key").unwrap_or("");
                let (min, max) = (number("minRank").max(1) as usize, number("maxRank").max(1) as usize);
                let rows = hub.stats.top(key, min, max, period, past, now);
                let mut r = reply().with("stats.[]", rows.len());
                for (i, row) in rows.iter().enumerate() {
                    let f = |x: &str| format!("stats.{i}.{x}");
                    r = r.with(&f("owner"), row.owner).with(&f("name"), &row.name).with(&f("rank"), row.rank).with(&f("value"), num(row.value)).with(&f("text"), &row.text);
                    if txn == "GetTopNAndStats" {
                        r = r.with(&f("addStats.[]"), keys.len());
                        for (j, (k, v, t)) in hub.stats.get(row.owner, &keys, period, past, now).into_iter().enumerate() {
                            r = r.with(&f(&format!("addStats.{j}.key")), k).with(&f(&format!("addStats.{j}.value")), num(v)).with(&f(&format!("addStats.{j}.text")), t);
                        }
                    }
                }
                Self::send(ev, r);
            }
            _ => return false,
        }
        true
    }

    fn theater(&mut self, p: &Packet, hub: &mut Hub, ev: &mut Vec<Event>) {
        let tid = p.get("TID").unwrap_or("").to_string();
        let reply = |kind: &[u8; 4]| Packet::new(kind, 0).with("TID", &tid);
        let gid = p.get("GID").and_then(|g| g.parse::<u32>().ok()).unwrap_or(0);
        match &p.kind {
            b"CONN" => {
                if let Some(v) = p.get("VERS").filter(|v| !v.is_empty() && v.len() <= 16) {
                    self.version = v.to_string();
                }
                Self::send(ev, reply(b"CONN").with("TIME", unix_secs()).with("activityTimeoutSecs", 0).with("PROT", p.get("PROT").unwrap_or("2")))
            }
            b"USER" => {
                self.user = p.get("LKEY").and_then(|k| hub.logins.find(k)).cloned();
                if self.user.is_some() {
                    hub.theater_users.insert(self.key);
                }
                let name = self.user.as_ref().map_or("Player".to_string(), |u| u.name.clone());
                Self::send(ev, reply(b"USER").with("NAME", name));
            }
            b"LLST" => {
                // One lobby, as EA had: "Internet", id 257.
                let n = hub.games.len();
                Self::send(ev, reply(b"LLST").with("NUM-LOBBIES", 1));
                Self::send(
                    ev,
                    reply(b"LDAT")
                        .with("LID", LOBBY)
                        .with("NAME", "Internet")
                        .with("LOCALE", "en_US")
                        .with("PASSING", n)
                        .with("MAX-GAMES", 10000)
                        .with("NUM-GAMES", n)
                        .with("FAVORITE-GAMES", 0)
                        .with("FAVORITE-PLAYERS", 0),
                );
            }
            // The game list: a count, then one GDAT per game with the request's TID.
            b"GLST" => {
                let n = hub.games.len();
                Self::send(ev, reply(b"GLST").with("LID", LOBBY).with("LOBBY-NUM-GAMES", n).with("NUM-GAMES", n).with("LOBBY-MAX-GAMES", 10000));
                for g in hub.games.by_gid.values() {
                    Self::send(ev, g.gdat(&tid));
                }
            }
            // One game's details (before joining it): GDAT, then GDET with its detailed
            // attributes, as EA answered. A game invite (EA Messenger) asks by the sender's
            // name instead of an id: GDAT USER=Metin TYPE=G, then joins what this answers.
            b"GDAT" => {
                let by_user = p.get("USER").filter(|_| gid == 0);
                let gid = match by_user {
                    Some(name) => hub.game_of(&|n, _| n.eq_ignore_ascii_case(name)).unwrap_or(0),
                    None => gid,
                };
                let Some(g) = hub.games.by_gid.get(&gid) else {
                    // As EA's emulators answer a game that is gone: the TID alone.
                    Self::send(ev, reply(b"GDAT"));
                    return;
                };
                let mut gdat = g.gdat(&tid);
                // An invite's lookup wants the whole game: its SDK keeps the request open until
                // it has a PDAT for each of the AP players (0x952260 counts them), and only then
                // shows the invite's details (Game Mode, Track, Laps ...), which it reads from
                // the game's B-U-* attributes. So AP is the players listed, and a race host's
                // room adds its settings.
                let players = if by_user.is_some() { hub.players_of(gid) } else { Vec::new() };
                if by_user.is_some() {
                    set_field(&mut gdat, "AP", players.len());
                    for (k, v) in hub.hosts.get(&gid).map(Host::settings).unwrap_or_default() {
                        if k == "max_online_player" {
                            set_field(&mut gdat, "MP", &v);
                        }
                        set_field(&mut gdat, &format!("B-U-{k}"), v);
                    }
                }
                Self::send(ev, gdat);
                let mut d = reply(b"GDET").with("LID", LOBBY).with("GID", gid).with("UGID", &g.ugid);
                for (k, v) in &g.details {
                    d = d.with(k, v);
                }
                Self::send(ev, d);
                for (pid, name, uid) in players {
                    Self::send(ev, reply(b"PDAT").with("LID", LOBBY).with("GID", gid).with("PID", pid).with("NAME", name).with("UID", uid));
                }
            }
            b"PCNT" => Self::send(ev, reply(b"PCNT").with("LID", LOBBY).with("COUNT", hub.theater_users.len().max(1))),
            // A player hosts a game. Its address comes from the host's own connection (the
            // switch's vip) and the PORT it learnt from our ECHO answer.
            b"CGAM" => {
                let Some(user) = self.user.clone() else {
                    ev.push(Event::Note("CGAM before USER: not answered".into()));
                    return;
                };
                if hub.games.len() >= MAX_GAMES {
                    ev.push(Event::Note("CGAM: the server has its share of games; not answered".into()));
                    return;
                }
                // A host has one game: a new one replaces the last.
                let old: Vec<u32> = hub.games.by_gid.values().filter(|g| g.host == self.key).map(|g| g.gid).collect();
                for o in old {
                    hub.games.by_gid.remove(&o);
                }
                hub.games.last_gid = hub.games.last_gid.wrapping_add(1).max(1);
                let gid = 100_000 + hub.games.last_gid % 900_000;
                let r = hub.bytes(16);
                let ugid = match p.get("UGID").filter(|u| !u.is_empty()) {
                    Some(u) => u.to_string(),
                    None => format!(
                        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-4{:01x}{:02x}-a{:01x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6] & 15, r[7], r[8] & 15, r[9], r[10], r[11], r[12], r[13], r[14], r[15]
                    ),
                };
                let secret = match p.get("SECRET").filter(|s| !s.is_empty()) {
                    Some(s) => s.to_string(),
                    None => {
                        let b = hub.bytes(48);
                        base64(&b).replace('=', "%3d")
                    }
                };
                // The tunnel key both ends of every link in this game use (EA sent 16 random
                // bytes in base64; the game keeps the first 16 characters).
                let ekey = {
                    let b = hub.bytes(16);
                    base64(&b).replace('=', "%3d")
                };
                let mut g = Game {
                    gid,
                    host: self.key,
                    traced: false,
                    served: false,
                    host_user: user.clone(),
                    name: clean_name(p.get("NAME").unwrap_or(&user.name)),
                    port: p.get("PORT").filter(|v| !v.is_empty()).unwrap_or("1042").to_string(),
                    int_ip: p.get("INT-IP").filter(|v| !v.is_empty()).map_or_else(|| Ipv4Addr::from(self.key.0).to_string(), str::to_string),
                    int_port: p.get("INT-PORT").filter(|v| !v.is_empty()).unwrap_or("1042").to_string(),
                    max_players: p.get("MAX-PLAYERS").and_then(|n| n.parse().ok()).unwrap_or(8).clamp(1, 32),
                    ugid,
                    secret,
                    ekey,
                    join: p.get("JOIN").filter(|j| !j.is_empty()).unwrap_or("O").to_string(),
                    password: p.get("PASSWORD") == Some("1"),
                    version: self.version.clone(),
                    attrs: Vec::new(),
                    details: Vec::new(),
                    members: Vec::new(),
                    next_pid: 2,
                };
                g.set_attrs(p);
                g.name = clean_name(p.get("NAME").unwrap_or(&user.name));
                ev.push(Event::Note(format!("{} hosts game {gid} \"{}\" at {}:{}", user.name, g.name, Ipv4Addr::from(self.key.0), g.port)));
                Self::send(
                    ev,
                    reply(b"CGAM")
                        .with("LID", LOBBY)
                        .with("GID", gid)
                        .with("MAX-PLAYERS", g.max_players)
                        .with("EKEY", &g.ekey)
                        .with("UGID", &g.ugid)
                        .with("JOIN", &g.join)
                        .with("J", &g.join)
                        .with("SECRET", &g.secret),
                );
                hub.games.by_gid.insert(gid, g);
            }
            // The host updates its game: attributes (UGAM), detailed ones (UGDE), players'
            // (UPLA). EA's server did not answer these.
            b"UGAM" | b"UGDE" => match hub.games.by_gid.get_mut(&gid) {
                Some(g) if g.host == self.key => g.set_attrs(p),
                _ => ev.push(Event::Note(format!("{} for game {gid}, which this player does not host", p.kind_str()))),
            },
            b"UPLA" => {}
            // Updates come in brackets, UBRA START=1 ... UBRA START=0; both are answered when the
            // bracket closes.
            b"UBRA" => {
                if p.get("START") == Some("1") {
                    self.bracket.push(tid.clone());
                } else {
                    for t in std::mem::take(&mut self.bracket) {
                        Self::send(ev, Packet::new(b"UBRA", 0).with("TID", t));
                    }
                    Self::send(ev, reply(b"UBRA"));
                }
            }
            // A player asks to join: it gets a player id and a ticket, and the host is asked.
            // Accepting a game invite joins the inviter's room by its user id instead of a
            // game id (EGAM UID=<inviter> R-UID=<inviter> TYPE=G, no LID or GID).
            b"EGAM" => {
                let Some(user) = self.user.clone() else {
                    ev.push(Event::Note("EGAM before USER: not answered".into()));
                    return;
                };
                let gid = match p.get("UID").and_then(|u| u.parse::<u64>().ok()).filter(|_| gid == 0) {
                    Some(uid) => hub.game_of(&|_, id| id == uid).unwrap_or(0),
                    None => gid,
                };
                let ticket = format!("{}", 1_000_000_000 + hub.next() % 1_000_000_000);
                let Some(g) = hub.games.by_gid.get_mut(&gid) else {
                    ev.push(Event::Note(format!("EGAM for game {gid}, which does not exist")));
                    Self::send(ev, reply(b"EGAM").with("LID", LOBBY).with("GID", gid));
                    Self::send(ev, Packet::new(b"EGEG", 0).with("LID", LOBBY).with("GID", gid).with("REASON", "GAME_NOT_FOUND"));
                    return;
                };
                g.members.retain(|m| m.conn != self.key);
                let pid = g.next_pid;
                g.next_pid += 1;
                // The race host's players count as in the game from here (it lets everyone in).
                g.members.push(Member { pid, user: user.clone(), conn: self.key, ticket: ticket.clone(), entered: g.served });
                ev.push(Event::Note(format!("{} asks to join game {gid} (pid {pid})", user.name)));
                Self::send(ev, reply(b"EGAM").with("LID", LOBBY).with("GID", gid));
                let ip = Ipv4Addr::from(self.key.0).to_string();
                let mut q = Packet::new(b"EGRQ", 0)
                    .with("R-INT-PORT", p.get("R-INT-PORT").unwrap_or("1042"))
                    .with("R-INT-IP", p.get("R-INT-IP").filter(|v| !v.is_empty()).unwrap_or(&ip))
                    .with("PORT", p.get("PORT").unwrap_or("1042"))
                    .with("NAME", &user.name)
                    .with("PTYPE", p.get("PTYPE").unwrap_or("P"))
                    .with("TICKET", &ticket)
                    .with("PID", pid)
                    .with("UID", user.id)
                    .with("R-USER", &user.name)
                    .with("R-UID", user.id)
                    .with("IP", &ip)
                    .with("LID", LOBBY)
                    .with("GID", gid);
                for (k, v) in p.fields.iter().filter(|(k, _)| k.starts_with("R-U-")) {
                    q = q.with(k, v);
                }
                if g.served {
                    // The race host lets everyone in; the tunnel key and its address are its own.
                    if let Some(h) = hub.hosts.get_mut(&gid) {
                        h.expect(Expected { pid: pid as u16, user_id: user.id as u64, name: user.name.clone(), ticket: ticket.clone(), vip: self.key.0 });
                    }
                    let e = Packet::new(b"EGEG", 0)
                        .with("PL", "pc")
                        .with("TICKET", &ticket)
                        .with("PID", pid)
                        .with("I", "10.77.0.1")
                        .with("P", &g.port)
                        .with("HUID", g.host_user.id)
                        .with("INT-IP", &g.int_ip)
                        .with("INT-PORT", &g.int_port)
                        .with("EKEY", &g.ekey)
                        .with("UGID", &g.ugid)
                        .with("LID", LOBBY)
                        .with("GID", gid);
                    ev.push(Event::Note(format!("{} joins the race host's game {gid} at 10.77.0.1:{}", user.name, g.port)));
                    Self::send(ev, e);
                    return;
                }
                if !g.traced {
                    Self::push(ev, g.host, q);
                    return;
                }
                // The trace host lets everyone in; its UDP port records what the game sends.
                let e = Packet::new(b"EGEG", 0)
                    .with("PL", "pc")
                    .with("TICKET", &ticket)
                    .with("PID", pid)
                    .with("I", "10.77.0.1")
                    .with("P", &g.port)
                    .with("HUID", g.host_user.id)
                    .with("INT-IP", &g.int_ip)
                    .with("INT-PORT", &g.int_port)
                    .with("EKEY", &g.ekey)
                    .with("UGID", &g.ugid)
                    .with("LID", LOBBY)
                    .with("GID", gid);
                ev.push(Event::Note(format!("{} joins the trace host's game: its game traffic to 10.77.0.1:{} is recorded", user.name, g.port)));
                Self::send(ev, e);
            }
            // The host's answer to a join request. Allowed: the joiner gets the host's address
            // and the game's tunnel key (EGEG) and connects to the host itself.
            b"EGRS" => {
                Self::send(ev, reply(b"EGRS"));
                let pid = p.get("PID").and_then(|v| v.parse::<u32>().ok()).unwrap_or(0);
                let Some(g) = hub.games.by_gid.get_mut(&gid).filter(|g| g.host == self.key) else {
                    ev.push(Event::Note(format!("EGRS for game {gid}, which this player does not host")));
                    return;
                };
                let Some(i) = g.members.iter().position(|m| m.pid == pid) else {
                    ev.push(Event::Note(format!("EGRS for player {pid}, who is not joining game {gid}")));
                    return;
                };
                if p.get("ALLOWED") != Some("1") {
                    let m = g.members.remove(i);
                    ev.push(Event::Note(format!("the host turned {} away", m.user.name)));
                    let reason = p.get("REASON").filter(|r| !r.is_empty()).unwrap_or("NOT_ALLOWED").to_string();
                    Self::push(ev, m.conn, Packet::new(b"EGEG", 0).with("LID", LOBBY).with("GID", gid).with("REASON", reason));
                    return;
                }
                let m = &g.members[i];
                let e = Packet::new(b"EGEG", 0)
                    .with("PL", "pc")
                    .with("TICKET", &m.ticket)
                    .with("PID", m.pid)
                    .with("I", Ipv4Addr::from(g.host.0))
                    .with("P", &g.port)
                    .with("HUID", g.host_user.id)
                    .with("INT-IP", &g.int_ip)
                    .with("INT-PORT", &g.int_port)
                    .with("EKEY", &g.ekey)
                    .with("UGID", &g.ugid)
                    .with("LID", LOBBY)
                    .with("GID", gid);
                ev.push(Event::Note(format!("{} may join game {gid}: sent the host's address", m.user.name)));
                Self::push(ev, m.conn, e);
            }
            // The host says a joiner is in (PENT) or left (PLVT).
            b"PENT" | b"PLVT" => {
                let pid = p.get("PID").and_then(|v| v.parse::<u32>().ok()).unwrap_or(0);
                if let Some(g) = hub.games.by_gid.get_mut(&gid).filter(|g| g.host == self.key) {
                    if &p.kind == b"PENT" {
                        if let Some(m) = g.members.iter_mut().find(|m| m.pid == pid) {
                            m.entered = true;
                            ev.push(Event::Note(format!("{} is in game {gid}", m.user.name)));
                        }
                    } else {
                        g.members.retain(|m| m.pid != pid);
                    }
                }
                let r = reply(&p.kind);
                Self::send(ev, if &p.kind == b"PENT" { r.with("PID", pid) } else { r });
            }
            // A joiner gives up, or leaves.
            b"ECNL" => {
                if let Some(g) = hub.games.by_gid.get_mut(&gid) {
                    g.members.retain(|m| m.conn != self.key);
                }
                Self::send(ev, reply(b"ECNL").with("LID", LOBBY).with("GID", gid));
            }
            // The host closes its game.
            b"RGAM" => {
                if hub.games.by_gid.get(&gid).is_some_and(|g| g.host == self.key) {
                    hub.games.by_gid.remove(&gid);
                    ev.push(Event::Note(format!("game {gid} closed by its host")));
                }
                Self::send(ev, reply(b"RGAM"));
            }
            b"PING" => {}
            _ => ev.push(Event::Note(format!("no stand-in for Theater {}: not answered", p.kind_str()))),
        }
    }

    /// Messenger: AUTH names the player (by its login key); the rest is messenger.rs's.
    fn messenger(&mut self, p: &Packet, hub: &mut Hub, ev: &mut Vec<Event>) {
        let id = p.get("ID").unwrap_or("").to_string();
        let reply = |kind: &[u8; 4]| Packet::new(kind, 0).with("ID", &id);
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
        match &p.kind {
            b"AUTH" => {
                self.user = p.get("LKEY").and_then(|k| hub.logins.find(k)).cloned();
                let name = self.user.as_ref().map_or("Player".to_string(), |u| u.name.clone());
                Self::send(
                    ev,
                    reply(b"AUTH").with("TIID", 0).with("TITL", "Need for Speed Carbon").with("USER", format!("{name}@messaging.ea.com/eagames/NFS-2007")),
                );
                if self.user.is_some() {
                    for (to, q) in hub.messenger.sign_in(self.key, &name, now) {
                        ev.push(Event::Note(format!("{name} signed in again elsewhere: the older session is logged off")));
                        ev.push(Event::Push(to, q.encode(), q.summary()));
                    }
                }
            }
            b"EPGT" => Self::send(ev, reply(b"EPGT").with("ENAB", "F").with("ADDR", "")),
            b"EPST" => Self::send(ev, reply(b"EPST")),
            b"PING" => {}
            b"RGET" | b"PSET" | b"PADD" | b"PDEL" | b"USCH" | b"RADM" | b"RADD" | b"RRSP" | b"RDEL" | b"RDEM" | b"SEND" | b"GINV" | b"GRSP" | b"GRVK" | b"DISC" => {
                if self.user.is_none() {
                    // not signed in (no AUTH, or an unknown key): answered, nothing more
                    Self::send(ev, reply(&p.kind));
                    return;
                }
                for (to, q) in hub.messenger.handle(self.key, p, now) {
                    if to == self.key {
                        Self::send(ev, q);
                    } else {
                        ev.push(Event::Push(to, q.encode(), q.summary()));
                    }
                }
            }
            _ => ev.push(Event::Note(format!("no stand-in for Messenger {}: not answered", p.kind_str()))),
        }
    }
}

/// A datagram to Theater's UDP port: the game's ECHO, which its GameManager sends from its
/// game socket (port 1042) at sign-in to learn the address and port the server sees it at, as
/// a NAT would show them. EA answered on UDP with IP, PORT, ERR=0 and the request's TID and
/// TYPE (March 2021 capture: "<-D ECHO {PORT=7322, TID=1, IP=..., TXN=ECHO, ERR=0, TYPE=1}");
/// the game matches the TID and takes IP and PORT (handler 009536b0). On our network nobody
/// translates, so the answer is the vip and port the datagram came from. Returns the request
/// and the answer, or None for anything else (which is only traced).
pub fn theater_udp(vip: u32, port: u16, data: &[u8]) -> Option<(Packet, Packet)> {
    if data.len() < HEADER {
        return None;
    }
    let len = u32::from_be_bytes([data[8], data[9], data[10], data[11]]) as usize;
    if &data[..4] != b"ECHO" || !(HEADER..=data.len()).contains(&len) {
        return None;
    }
    let p = Packet::decode(&data[..len]);
    let r = Packet::new(b"ECHO", 0)
        .with("TID", p.get("TID").unwrap_or("1"))
        .with("TXN", "ECHO")
        .with("IP", Ipv4Addr::from(vip))
        .with("PORT", port)
        .with("ERR", 0)
        .with("TYPE", p.get("TYPE").unwrap_or("1"));
    Some((p, r))
}

/// PlayNow's Status for a Start (async, id 0x80000000), shaped as EA's server sent it (March
/// 2021 captures): JOIN with the game when another player's game is open (its host not this
/// player, joins open, room left, the game type the player filters on), else NOMATCH, which
/// the game shows as "Could not create game session. Please try again."
///
/// Why not CREATE ("you host"), which PlayNow can answer: Carbon's client ignores it. Its three
/// PlayNow listeners (vtables 009f60b8, 009f5108, 009f5430) all implement the create callback
/// (+0x38, called from GameBrowserImpl::PlayNowCreateGame 0090a4f0) as an empty `ret 0xc`
/// (0081d0c0), so the game stays on "Creating game session..." for ever. EA ran every game on
/// its own hosts (HN=nfsdevserver, B-version 298_prod_server); a player's game only joins.
fn playnow_status(p: &Packet, id: u32, user: Option<&User>, games: &Games, hosts: &BTreeMap<u32, Host>) -> Packet {
    let game_type = p.get("players.0.props.{filter-game_type}").unwrap_or("");
    let me = user.map_or(0, |u| u.id);
    let open = games.by_gid.values().find(|g| {
        g.host_user.id != me
            && (g.join != "C" || g.served)
            // a served room whose race is on would refuse the player (the match has started)
            && !(g.served && hosts.get(&g.gid).is_some_and(|h| h.in_race()))
            && !g.password
            && g.active() + g.joining() < g.max_players as usize
            && (game_type.is_empty()
                || g.traced
                || g.served
                || g.attrs.iter().find(|(k, _)| k == "B-U-game_type").map_or(true, |(_, v)| game_type.split('|').any(|t| t == v)))
    });
    let base = Packet::new(b"pnow", 0x8000_0000)
        .with("TXN", "Status")
        .with("id.id", id)
        .with("id.partition", "/eagames/NFS-2007")
        .with("sessionState", "COMPLETE");
    match open {
        Some(g) => base
            .with("props.{}", 3)
            .with("props.{resultType}", "JOIN")
            .with("props.{avgFit}", "1.0")
            .with("props.{games}.[]", 1)
            .with("props.{games}.0.gid", g.gid)
            .with("props.{games}.0.lid", LOBBY),
        None => base.with("props.{}", 1).with("props.{resultType}", "NOMATCH"),
    }
}

/// A FESL error answer, shaped as EA's were (localizedMessage, errorContainer, errorCode).
/// A field's value replaced (or added): GDAT's AP and MP for an invite's lookup.
fn set_field(p: &mut Packet, key: &str, value: impl ToString) {
    match p.fields.iter_mut().find(|(k, _)| k == key) {
        Some(f) => f.1 = value.to_string(),
        None => p.fields.push((key.to_string(), value.to_string())),
    }
}

fn error(p: &Packet, txn: &str, message: &str) -> Packet {
    error_code(p, txn, message, 99)
}

/// A FESL error with EA's code (122: the password does not match the account).
fn error_code(p: &Packet, txn: &str, message: &str, code: u32) -> Packet {
    Packet::new(&p.kind, p.id)
        .with("TXN", txn)
        .with("localizedMessage", message)
        .with("errorContainer.[]", 0)
        .with("errorCode", code)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The game's own first packet, from our trace of a run (2026-10-24).
    const HELLO: &[u8] = b"fsys\x80\x00\x00\x01\x00\x00\x00\xb5TXN=Hello\nclientString=nfs-pc\nsku=America\nlocale=en_US\n\
clientPlatform=PC\nclientVersion=2.0\nSDKVersion=2.9.7.1.0\nprotocolVersion=2.0\nfragmentSize=2048\nclientType=client\n\0";

    #[test]
    fn the_games_hello_parses() {
        assert_eq!(HELLO.len(), 0xb5);
        let mut r = Reader::default();
        r.push(&HELLO[..20]);
        assert_eq!(r.next(), Ok(None));
        r.push(&HELLO[20..]);
        let raw = r.next().unwrap().unwrap();
        assert_eq!(r.pending(), 0);
        let p = Packet::decode(&raw);
        assert_eq!((&p.kind, p.id), (b"fsys", 0x8000_0001));
        assert_eq!(p.get("TXN"), Some("Hello"));
        assert_eq!(p.get("fragmentSize"), Some("2048"));
        assert_eq!(p.fields.len(), 10);
    }

    #[test]
    fn encoding_round_trips_and_quotes_spaces() {
        let p = Packet::new(b"fsys", 0x8000_0002).with("TXN", "Hello").with("curTime", "Oct-24-2026 18%3a30%3a00 UTC").with("empty", "");
        let raw = p.encode();
        assert_eq!(&raw[..12], b"fsys\x80\x00\x00\x02\x00\x00\x00\x45"); // 69 bytes in all
        assert_eq!(&raw[12..], b"TXN=Hello\ncurTime=\"Oct-24-2026 18%3a30%3a00 UTC\"\nempty=\n\0");
        assert_eq!(Packet::decode(&raw), p);
    }

    #[test]
    fn bad_lengths_are_errors() {
        let mut r = Reader::default();
        r.push(b"fsys\x80\x00\x00\x01\x00\x00\x00\x05");
        assert!(r.next().is_err());
        let mut r = Reader::default();
        r.push(b"fsys\x80\x00\x00\x01\x00\x01\x00\x00");
        assert!(r.next().is_err());
    }

    #[test]
    fn passwords_are_dropped_and_masked() {
        let raw = Packet::new(b"acct", 0x8000_0002)
            .with("TXN", "Login")
            .with("name", "Night Owl")
            .with("password", "hunter2")
            .with("macAddr", "$0a4d00100000")
            .encode();
        let p = Packet::decode(&raw);
        assert_eq!(p.get("password"), Some(""));
        assert!(p.summary().contains("password=(not kept)"));
        let m = masked(&raw);
        assert_eq!(m.len(), raw.len());
        assert!(String::from_utf8_lossy(&m).contains("password=*******\n"));
        assert!(!String::from_utf8_lossy(&m).contains("hunter2"));
        assert!(String::from_utf8_lossy(&m).contains("name=\"Night Owl\"\n"));
    }

    #[test]
    fn names_ids_and_keys() {
        assert_eq!(clean_name("Night Owl!<>"), "NightOwl");
        assert_eq!(clean_name("  "), "Player");
        assert_eq!(clean_name("abcdefghijklmnopqrstuvwxyz"), "abcdefghijklmnop");
        assert_eq!(user_id("LanTest"), user_id("lantest"));
        assert!((100_000_000..1_000_000_000).contains(&user_id("x")));
        let mut l = Logins::default();
        let a = l.login("LanTest");
        let b = l.login("LanTest");
        assert_ne!(a.lkey, b.lkey);
        assert_eq!(a.lkey.len(), 27);
        assert!(a.lkey.ends_with('.'));
        assert_eq!(l.find(&b.lkey), Some(&b));
    }

    #[test]
    fn fesl_time_is_eas_format() {
        let t = UNIX_EPOCH + std::time::Duration::from_secs(1_615_998_175); // 2021-03-17 16:22:55 UTC
        assert_eq!(fesl_time(t), "Mar-17-2021 16%3a22%3a55 UTC");
    }

    fn out(events: &[Event]) -> Vec<Packet> {
        events.iter().filter_map(|e| if let Event::Out(raw, _) = e { Some(Packet::decode(raw)) } else { None }).collect()
    }

    #[test]
    fn long_transactions_are_joined_from_fragments() {
        let mut s = Session::new(Service::Fesl, (0x0a4d_0010, 50000, FESL_PORT, 1));
        let mut hub = Hub::default();
        let text = b"TXN=ReportMetrics\nevents.[]=1\nevents.0.locationType=55\n\0";
        let b64 = crate::util::base64(text).replace('=', "%3d");
        let (a, b) = b64.split_at(20);
        let size = crate::util::base64(text).len();
        let frag = |piece: &str| Packet::new(b"mtrx", 0xb000_0005).with("size", size).with("data", piece).encode();
        let ev = s.feed(&frag(a), &mut hub).unwrap();
        assert!(out(&ev).is_empty());
        let ev = s.feed(&frag(b), &mut hub).unwrap();
        let o = out(&ev);
        assert_eq!(o.len(), 1);
        assert_eq!((&o[0].kind, o[0].id, o[0].get("TXN")), (b"mtrx", 0x8000_0005, Some("ReportMetrics")));
    }
}
