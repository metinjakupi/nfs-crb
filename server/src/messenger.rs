//! EA Messenger, as NFS Carbon's own friends screens use it (the M key in the online menus,
//! "EA Messenger" in Game Setup): friends and friend requests, blocks, presence, chat and game
//! invites. ea.rs's Messenger stand-in (TCP 13505) hands each packet here once AUTH named the
//! player. Commands and fields are the FESL SDK 2.9 Communicator's, read from NFSC.exe and the
//! March 2021 captures (plans/lan-protocol.md, "EA Messenger"):
//!
//! - RGET {LIST=B|I, PRES, PEND}: the reply (SIZE=n), then n ROST {GROUP, UID, ID, ATTR, USER}.
//!   ATTR: AT a friend, RT a request to this player, ST a request from this player (captures).
//!   With PRES=Y the player also watches its friends' presence (PGET, ID=auto-subscribe:n).
//! - RADM {USER} a friend request; RRSP {USER, ANSW=Y|N|B} its answer (accept, decline,
//!   block); RDEL {LIST, USER} removes (a friend, a request, a block); RADD {LIST=I, USER}
//!   blocks. Each change reaches both players as RNOT {UID, ID=, ATTR, LRSC, CHNG, USER}
//!   (CHNG A added, X changed, D deleted).
//! - PSET sets this player's presence (SHOW CHAT|AWAY|XA|DND|GAME|DISC, STAT, ATTR...); friends
//!   who watch it (PADD, or RGET's PRES=Y) get PGET {..., CHNG=1}; SHOW=DISC when it leaves.
//! - SEND {TYPE=C, USER, BODY} goes to a friend as RECV {TYPE, USER, TIME, SUBJ, BODY}: live
//!   only, never stored or logged (ea.rs masks BODY and SUBJ in its traces).
//! - GINV {USER, SESS, TITL, GSTR} goes to a friend as GNOT {HOST, USER, TYPE=I, SESS, TIID,
//!   GSTR}; GRSP {ANSW=Y|N|R} back as GNOT TYPE=A|D; GRVK as GNOT TYPE=R.
//! - A second sign-in under the same name logs the first one off (ADMN {TYPE=DUPL}: "Multiple
//!   log-ins detected. You have been logged off.").
//!
//! Names are claimed by a per-browser player key (lan.js sends it in the /lan hello; only its
//! SHA-256 is kept): a name claimed by another key is refused at FESL Login. A claim lapses
//! after 30 days without a sign-in, and whoever claims it next starts with an empty list. A
//! client without a key may use a name nobody has claimed, without claiming it.
//!
//! Kept between sessions (with CARBON_MESSENGER_DIR, in `messenger.json`): names with the key
//! digest and the last sign-in day, friend links, pending requests and blocks. Nothing else: no
//! chat, no addresses.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::PathBuf;

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::ea::{user_id, ConnKey, Packet};

pub const DOMAIN: &str = "messaging.ea.com";
const RESOURCE: &str = "eagames/NFS-2007";
const TITLE: &str = "Need for Speed Carbon";
/// The game says "Your Friends list is full." past its own limit; these keep the server small.
const MAX_FRIENDS: usize = 100;
const MAX_PENDING: usize = 50;
const MAX_BLOCKED: usize = 100;
/// Names kept at most (about 100 bytes each in messenger.json, which is written whole on each
/// change). When full, a new name makes room by taking the place of the longest-unseen name
/// that holds nothing (no live claim, no friends, requests or blocks); when there is none, it
/// signs in without being kept.
const MAX_PEOPLE: usize = 10_000;
/// A name's last sign-in is written at most once a day (it only counts for the 30-day lapse),
/// so signing in does not rewrite the file each time.
const SEEN_STEP_SECS: u64 = 24 * 3600;
/// A name's claim ends after this long without a sign-in (a player who lost its browser's key
/// gets the name back after a month).
pub const CLAIM_LAPSE_SECS: u64 = 30 * 24 * 3600;
/// Chat: a burst of 6, then one message a second; invites: 3, then one every 5 s.
const CHAT_BURST: f64 = 6.0;
const CHAT_PER_MS: f64 = 1.0 / 1000.0;
const INVITE_BURST: f64 = 3.0;
const INVITE_PER_MS: f64 = 1.0 / 5000.0;
const MAX_TEXT: usize = 256;

/// SHA-256 of a player key: all the server keeps of it.
pub type KeyDigest = [u8; 32];

pub fn digest(key: &str) -> KeyDigest {
    Sha256::digest(key.as_bytes()).into()
}

fn hex(d: &KeyDigest) -> String {
    d.iter().map(|b| format!("{b:02x}")).collect()
}

/// "Metin", "metin@messaging.ea.com" or "Metin@messaging.ea.com/eagames/NFS-2007" -> "Metin".
fn bare(user: &str) -> String {
    let n = user.split(['@', '/']).next().unwrap_or("");
    n.chars().filter(|c| c.is_ascii_alphanumeric() || "_-.".contains(*c)).take(16).collect()
}

fn low(name: &str) -> String {
    name.to_ascii_lowercase()
}

/// Text from one player to another: no quotes or line breaks, which the framing would read as
/// the end of a value or a field, and not too long.
fn clean_text(s: &str) -> String {
    s.chars().filter(|c| !matches!(c, '"' | '\n' | '\r' | '\0')).take(MAX_TEXT).collect()
}

#[derive(Clone, Debug, PartialEq)]
struct Person {
    name: String,
    /// the player key's digest (hex) of the browser that claimed the name, if one did
    key: Option<String>,
    /// last sign-in, in seconds since 1970 (a day's precision would do)
    seen: u64,
}

/// What is kept between sessions.
#[derive(Default)]
pub struct Store {
    people: BTreeMap<String, Person>,
    /// (a, b), a < b
    friends: BTreeSet<(String, String)>,
    /// (from, to)
    pending: BTreeSet<(String, String)>,
    /// (who, whom)
    blocked: BTreeSet<(String, String)>,
    file: Option<PathBuf>,
}

fn pair(a: &str, b: &str) -> (String, String) {
    if a < b {
        (a.to_string(), b.to_string())
    } else {
        (b.to_string(), a.to_string())
    }
}

impl Store {
    /// The store in `dir` (created), or one kept in memory only.
    pub fn open(dir: Option<PathBuf>) -> Store {
        let mut s = Store::default();
        let Some(dir) = dir else { return s };
        if let Err(e) = fs::create_dir_all(&dir) {
            eprintln!("messenger: cannot use {}: {e}; friends are kept for this run only", dir.display());
            return s;
        }
        let file = dir.join("messenger.json");
        if let Ok(text) = fs::read_to_string(&file) {
            match serde_json::from_str::<Value>(&text) {
                Ok(v) => s.load(&v),
                Err(e) => eprintln!("messenger: {} is not readable ({e}); starting empty", file.display()),
            }
        }
        s.file = Some(file);
        s
    }

    fn load(&mut self, v: &Value) {
        if let Some(m) = v.get("people").and_then(Value::as_object) {
            for (k, p) in m.iter().take(MAX_PEOPLE) {
                let Some(name) = p.get("name").and_then(Value::as_str) else { continue };
                let key = p.get("key").and_then(Value::as_str).filter(|k| k.len() == 64).map(str::to_string);
                let seen = p.get("seen").and_then(Value::as_u64).unwrap_or(0);
                self.people.insert(low(k), Person { name: bare(name), key, seen });
            }
        }
        let pairs = |name: &str| -> Vec<(String, String)> {
            v.get(name)
                .and_then(Value::as_array)
                .map(|a| {
                    a.iter()
                        .filter_map(|p| Some((low(p.get(0)?.as_str()?), low(p.get(1)?.as_str()?))))
                        .filter(|(x, y)| !x.is_empty() && !y.is_empty() && x != y)
                        .collect()
                })
                .unwrap_or_default()
        };
        self.friends = pairs("friends").into_iter().map(|(a, b)| pair(&a, &b)).collect();
        self.pending = pairs("pending").into_iter().collect();
        self.blocked = pairs("blocked").into_iter().collect();
    }

    fn to_json(&self) -> Value {
        let people: serde_json::Map<String, Value> = self
            .people
            .iter()
            .map(|(k, p)| (k.clone(), json!({ "name": p.name, "key": p.key, "seen": p.seen })))
            .collect();
        let list = |s: &BTreeSet<(String, String)>| -> Vec<Value> { s.iter().map(|(a, b)| json!([a, b])).collect() };
        json!({
            "version": 1,
            "people": people,
            "friends": list(&self.friends),
            "pending": list(&self.pending),
            "blocked": list(&self.blocked),
        })
    }

    /// Written at once, whole, through a temporary file (a crash leaves the old one).
    fn save(&self) {
        let Some(file) = &self.file else { return };
        let tmp = file.with_extension("json.tmp");
        let text = serde_json::to_string(&self.to_json()).unwrap_or_default();
        if let Err(e) = fs::write(&tmp, text).and_then(|_| fs::rename(&tmp, file)) {
            eprintln!("messenger: cannot write {}: {e}", file.display());
        }
    }

    /// Sign-in under `name` with the browser's key digest (if it sent one). False: the name is
    /// another browser's (claimed, the claim not lapsed). A key claims a free name; a lapsed
    /// claim taken by another key starts empty (the old player's friends are not handed over).
    pub fn claim(&mut self, name: &str, key: Option<&KeyDigest>, now: u64) -> bool {
        let k = low(name);
        let key = key.map(hex);
        let fresh = |p: &Person| p.key.is_some() && now.saturating_sub(p.seen) < CLAIM_LAPSE_SECS;
        match self.people.get(&k) {
            Some(p) if fresh(p) => {
                if key.is_none() || key != p.key {
                    return false;
                }
            }
            Some(p) if p.key.is_some() && key.is_some() && key != p.key => {
                // a lapsed claim, taken over: what the old player had stays with nobody
                self.forget(&k);
            }
            _ => {}
        }
        let new = !self.people.contains_key(&k);
        if new && self.people.len() >= MAX_PEOPLE && !self.make_room(now) {
            return true;
        }
        let p = self.people.entry(k).or_insert(Person { name: name.to_string(), key: None, seen: now });
        let changed = new || p.name != name || (key.is_some() && key != p.key) || now.saturating_sub(p.seen) >= SEEN_STEP_SECS;
        if changed {
            p.name = name.to_string();
            if key.is_some() {
                p.key = key;
            }
            p.seen = now;
            self.save();
        }
        true
    }

    /// The store is full: forgets the longest-unseen name that holds nothing (no key or a
    /// lapsed claim, and no friends, requests or blocks). False if every name holds something.
    fn make_room(&mut self, now: u64) -> bool {
        let busy: BTreeSet<&String> = self.friends.iter().chain(&self.pending).chain(&self.blocked).flat_map(|(a, b)| [a, b]).collect();
        let idle = self
            .people
            .iter()
            .filter(|(k, p)| !busy.contains(k) && (p.key.is_none() || now.saturating_sub(p.seen) >= CLAIM_LAPSE_SECS))
            .min_by_key(|(_, p)| p.seen)
            .map(|(k, _)| k.clone());
        match idle {
            Some(k) => {
                self.people.remove(&k);
                true
            }
            None => false,
        }
    }

    fn forget(&mut self, k: &str) {
        self.friends.retain(|(a, b)| a != k && b != k);
        self.pending.retain(|(a, b)| a != k && b != k);
        self.blocked.retain(|(a, b)| a != k && b != k);
        self.people.remove(k);
    }

    fn display(&self, k: &str) -> String {
        self.people.get(k).map_or_else(|| k.to_string(), |p| p.name.clone())
    }

    fn known(&self, k: &str) -> bool {
        self.people.contains_key(k)
    }

    fn are_friends(&self, a: &str, b: &str) -> bool {
        self.friends.contains(&pair(a, b))
    }

    fn friends_of(&self, k: &str) -> Vec<String> {
        self.friends.iter().filter_map(|(a, b)| if a == k { Some(b.clone()) } else if b == k { Some(a.clone()) } else { None }).collect()
    }

    fn count(&self, set: &BTreeSet<(String, String)>, k: &str) -> usize {
        set.iter().filter(|(a, b)| a == k || b == k).count()
    }
}

#[derive(Clone, Default)]
struct Presence {
    show: String,
    stat: String,
    attr: String,
    sess: String,
    titl: String,
    tiid: String,
    prod: String,
    extr: String,
}

struct Bucket {
    tokens: f64,
    at: u64,
}

impl Bucket {
    fn new(burst: f64) -> Bucket {
        Bucket { tokens: burst, at: 0 }
    }

    fn take(&mut self, now: u64, burst: f64, per_ms: f64) -> bool {
        self.tokens = (self.tokens + now.saturating_sub(self.at) as f64 * per_ms).min(burst);
        self.at = now;
        if self.tokens >= 1.0 {
            self.tokens -= 1.0;
            true
        } else {
            false
        }
    }
}

/// A signed-in player's Messenger connection.
struct Online {
    conn: ConnKey,
    presence: Option<Presence>,
    /// the friends whose presence it watches (PADD, or RGET with PRES=Y)
    watching: BTreeSet<String>,
    chat: Bucket,
    invites: Bucket,
}

/// A packet for a connection: the answer to the one being handled, or a notice for another.
pub type Out = (ConnKey, Packet);

#[derive(Default)]
pub struct Messenger {
    pub store: Store,
    online: BTreeMap<String, Online>,
    by_conn: BTreeMap<ConnKey, String>,
    /// open game invites: (inviter, invitee) -> SESS
    invites: BTreeMap<(String, String), String>,
    /// notices with no packet to answer (a player leaving), sent at the next tick
    pub outbox: Vec<Out>,
    auto_seq: u64,
}

fn user_at(name: &str) -> String {
    format!("{name}@{DOMAIN}")
}

fn user_full(name: &str) -> String {
    format!("{name}@{DOMAIN}/{RESOURCE}")
}

impl Messenger {
    pub fn new(dir: Option<PathBuf>) -> Messenger {
        Messenger { store: Store::open(dir), ..Messenger::default() }
    }

    #[cfg(test)]
    pub fn is_online(&self, name: &str) -> bool {
        self.online.contains_key(&low(name))
    }

    /// AUTH named the player: it is online on `conn`. The same name online elsewhere is logged
    /// off there (ADMN DUPL), as EA's server did.
    pub fn sign_in(&mut self, conn: ConnKey, name: &str, now: u64) -> Vec<Out> {
        let k = low(name);
        let mut out = Vec::new();
        if let Some(old) = self.online.remove(&k) {
            self.by_conn.remove(&old.conn);
            if old.conn != conn {
                out.push((old.conn, Packet::new(b"ADMN", 0).with("TYPE", "DUPL").with("SECS", 0)));
            }
        }
        if let Some(previous) = self.by_conn.remove(&conn) {
            self.online.remove(&previous);
        }
        if !self.store.known(&k) {
            self.store.claim(name, None, now / 1000);
        }
        self.by_conn.insert(conn, k.clone());
        self.online.insert(
            k,
            Online { conn, presence: None, watching: BTreeSet::new(), chat: Bucket::new(CHAT_BURST), invites: Bucket::new(INVITE_BURST) },
        );
        out
    }

    /// The connection ended (or DISC): its friends who watch it see it go offline.
    pub fn closed(&mut self, conn: ConnKey) {
        let Some(k) = self.by_conn.remove(&conn) else { return };
        if self.online.get(&k).is_some_and(|o| o.conn == conn) {
            self.online.remove(&k);
            self.invites.retain(|(a, b), _| *a != k && *b != k);
            let name = self.store.display(&k);
            let off = Presence { show: "DISC".into(), ..Presence::default() };
            for to in self.watchers(&k) {
                if let Some(o) = self.online.get(&to) {
                    self.outbox.push((o.conn, presence_packet(&name, Some(&off), None, true)));
                }
            }
        }
    }

    /// Online friends of `k` who watch its presence.
    fn watchers(&self, k: &str) -> Vec<String> {
        self.online
            .iter()
            .filter(|(w, o)| o.watching.contains(k) && self.store.are_friends(w, k))
            .map(|(w, _)| w.clone())
            .collect()
    }

    fn conn_of(&self, k: &str) -> Option<ConnKey> {
        self.online.get(k).map(|o| o.conn)
    }

    fn rnot(&self, about: &str, attr: &str, chng: &str) -> Packet {
        let name = self.store.display(about);
        Packet::new(b"RNOT", 0)
            .with("UID", user_id(&name))
            .with("ID", "")
            .with("ATTR", attr)
            .with("LRSC", "eagames")
            .with("CHNG", chng)
            .with("USER", user_at(&name))
    }

    /// A packet from the signed-in player on `conn`, at `now` (ms): what goes where.
    pub fn handle(&mut self, conn: ConnKey, p: &Packet, now: u64) -> Vec<Out> {
        let Some(me) = self.by_conn.get(&conn).cloned() else { return Vec::new() };
        let id = p.get("ID").unwrap_or("").to_string();
        let reply = |kind: &[u8; 4]| Packet::new(kind, 0).with("ID", &id);
        let other = p.get("USER").map(|u| low(&bare(u))).unwrap_or_default();
        let mut out: Vec<Out> = Vec::new();
        match &p.kind {
            b"RGET" => {
                let list = p.get("LIST").unwrap_or("B");
                let mut rows: Vec<(String, &str)> = Vec::new();
                if list == "I" {
                    rows.extend(self.store.blocked.iter().filter(|(a, _)| *a == me).map(|(_, b)| (b.clone(), "AT")));
                } else {
                    rows.extend(self.store.friends_of(&me).into_iter().map(|f| (f, "AT")));
                    if p.get("PEND") != Some("N") {
                        rows.extend(self.store.pending.iter().filter(|(_, b)| *b == me).map(|(a, _)| (a.clone(), "RT")));
                        rows.extend(self.store.pending.iter().filter(|(a, _)| *a == me).map(|(_, b)| (b.clone(), "ST")));
                    }
                }
                out.push((conn, reply(b"RGET").with("SIZE", rows.len())));
                for (k, attr) in &rows {
                    let name = self.store.display(k);
                    out.push((
                        conn,
                        Packet::new(b"ROST", 0).with("GROUP", "").with("UID", user_id(&name)).with("ID", &id).with("ATTR", *attr).with("USER", user_at(&name)),
                    ));
                }
                // PRES=Y: the friends' presence too, and from now on its changes
                if list != "I" && p.get("PRES") == Some("Y") {
                    let friends = self.store.friends_of(&me);
                    for f in friends {
                        if let Some(o) = self.online.get_mut(&me) {
                            o.watching.insert(f.clone());
                        }
                        if let Some(their) = self.online.get(&f).map(|o| o.presence.clone()) {
                            self.auto_seq += 1;
                            let sub = format!("auto-subscribe%3a{}", self.auto_seq);
                            out.push((conn, presence_packet(&self.store.display(&f), their.as_ref(), Some(&sub), false)));
                        }
                    }
                }
            }
            b"PSET" => {
                let pr = Presence {
                    show: p.get("SHOW").unwrap_or("CHAT").to_string(),
                    stat: clean_text(p.get("STAT").unwrap_or("")),
                    attr: clean_text(p.get("ATTR").unwrap_or("")),
                    sess: clean_text(p.get("SESS").unwrap_or("")),
                    titl: clean_text(p.get("TITL").unwrap_or(TITLE)),
                    tiid: clean_text(p.get("TIID").unwrap_or("0")),
                    prod: clean_text(p.get("PROD").unwrap_or("")),
                    extr: clean_text(p.get("EXTR").unwrap_or("")),
                };
                if let Some(o) = self.online.get_mut(&me) {
                    o.presence = Some(pr.clone());
                }
                out.push((conn, reply(b"PSET")));
                let name = self.store.display(&me);
                for w in self.watchers(&me) {
                    if let Some(c) = self.conn_of(&w) {
                        out.push((c, presence_packet(&name, Some(&pr), None, true)));
                    }
                }
            }
            b"PADD" => {
                out.push((conn, reply(b"PADD")));
                if self.store.are_friends(&me, &other) {
                    if let Some(o) = self.online.get_mut(&me) {
                        o.watching.insert(other.clone());
                    }
                    let their = self.online.get(&other).and_then(|o| o.presence.clone());
                    let off = Presence { show: "DISC".into(), ..Presence::default() };
                    out.push((conn, presence_packet(&self.store.display(&other), Some(their.as_ref().unwrap_or(&off)), Some(&id), false)));
                }
            }
            b"PDEL" => {
                if let Some(o) = self.online.get_mut(&me) {
                    o.watching.remove(&other);
                }
                out.push((conn, reply(b"PDEL")));
            }
            b"USCH" => {
                let want = low(&bare(p.get("USER").unwrap_or("")));
                let max = p.get("MAXR").and_then(|m| m.parse::<usize>().ok()).unwrap_or(5).clamp(1, 20);
                let mut found: Vec<String> = Vec::new();
                if !want.is_empty() {
                    if self.store.known(&want) {
                        found.push(want.clone());
                    }
                    for k in self.store.people.keys() {
                        if found.len() >= max {
                            break;
                        }
                        if *k != want && k.starts_with(&want) {
                            found.push(k.clone());
                        }
                    }
                }
                out.push((conn, reply(b"USCH").with("SIZE", found.len())));
                for k in found {
                    out.push((conn, Packet::new(b"USER", 0).with("ID", &id).with("RSRC", RESOURCE).with("USER", self.store.display(&k))));
                }
            }
            b"RADM" | b"RADD" if p.get("LIST") != Some("I") => {
                let mut notes = Vec::new();
                let refused = self.request(&me, &other, &mut notes);
                let r = Packet::new(&p.kind, refused.map_or(0, u32::from_be_bytes)).with("ID", &id);
                out.push((conn, r.with("PRES", p.get("PRES").unwrap_or("Y")).with("LRSC", "eagames").with("USER", bare(p.get("USER").unwrap_or("")))));
                out.extend(notes);
            }
            b"RADD" => {
                let mut notes = Vec::new();
                let refused = self.block(&me, &other, &mut notes);
                out.push((conn, Packet::new(b"RADD", refused.map_or(0, u32::from_be_bytes)).with("ID", &id)));
                out.extend(notes);
            }
            b"RRSP" => {
                out.push((conn, reply(b"RRSP")));
                self.answer(&me, &other, p.get("ANSW").unwrap_or("N"), &mut out);
            }
            b"RDEL" | b"RDEM" => {
                out.push((conn, reply(&p.kind)));
                if p.get("LIST") == Some("I") {
                    if self.store.blocked.remove(&(me.clone(), other.clone())) {
                        self.store.save();
                    }
                } else {
                    self.remove(&me, &other, &mut out);
                }
            }
            b"SEND" => {
                let ok = self.online.get_mut(&me).is_some_and(|o| o.chat.take(now, CHAT_BURST, CHAT_PER_MS));
                // Too fast: the game says "You cannot send a message that often." (0x8335e0).
                out.push((conn, Packet::new(b"SEND", if ok { 0 } else { u32::from_be_bytes(*b"rate") }).with("ID", &id)));
                if ok && self.store.are_friends(&me, &other) && !self.store.blocked.contains(&(other.clone(), me.clone())) {
                    if let Some(c) = self.conn_of(&other) {
                        let name = self.store.display(&me);
                        out.push((
                            c,
                            Packet::new(b"RECV", 0)
                                .with("TYPE", if p.get("TYPE") == Some("A") { "A" } else { "C" })
                                .with("USER", user_full(&name))
                                .with("TIME", now / 1000)
                                .with("SUBJ", clean_text(p.get("SUBJ").unwrap_or("")))
                                .with("BODY", clean_text(p.get("BODY").unwrap_or(""))),
                        ));
                    }
                }
            }
            b"GINV" => {
                out.push((conn, reply(b"GINV")));
                let ok = self.online.get_mut(&me).is_some_and(|o| o.invites.take(now, INVITE_BURST, INVITE_PER_MS));
                if ok && self.store.are_friends(&me, &other) && !self.store.blocked.contains(&(other.clone(), me.clone())) {
                    if let Some(c) = self.conn_of(&other) {
                        let sess = clean_text(p.get("SESS").unwrap_or(""));
                        let name = self.store.display(&me);
                        self.invites.insert((me.clone(), other.clone()), sess.clone());
                        out.push((
                            c,
                            Packet::new(b"GNOT", 0)
                                .with("HOST", &name)
                                .with("USER", user_full(&name))
                                .with("TYPE", "I")
                                .with("SESS", &sess)
                                .with("TIID", 0)
                                .with("GSTR", clean_text(p.get("GSTR").unwrap_or(""))),
                        ));
                    }
                }
            }
            b"GRSP" => {
                out.push((conn, reply(b"GRSP")));
                // to the inviter: accepted (A) or declined (D)
                if let Some(sess) = self.invites.remove(&(other.clone(), me.clone())) {
                    if let Some(c) = self.conn_of(&other) {
                        let kind = if p.get("ANSW") == Some("Y") { "A" } else { "D" };
                        let name = self.store.display(&me);
                        out.push((c, Packet::new(b"GNOT", 0).with("HOST", self.store.display(&other)).with("USER", user_full(&name)).with("TYPE", kind).with("SESS", sess).with("TIID", 0)));
                    }
                }
            }
            b"GRVK" => {
                out.push((conn, reply(b"GRVK")));
                if let Some(sess) = self.invites.remove(&(me.clone(), other.clone())) {
                    if let Some(c) = self.conn_of(&other) {
                        let name = self.store.display(&me);
                        out.push((c, Packet::new(b"GNOT", 0).with("HOST", &name).with("USER", user_full(&name)).with("TYPE", "R").with("SESS", sess).with("TIID", 0)));
                    }
                }
            }
            b"DISC" => {
                self.closed(conn);
            }
            _ => {}
        }
        out
    }

    /// A friend request from `me` to `to`. A refusal is the answer's error word, which the
    /// game turns into its own text (0x833450): `user` "You cannot Add or Block yourself.",
    /// `full` "Your Friends list is full.", `blck` "User has declined your friend request."
    /// (what EA's server answered when the other player had blocked this one).
    fn request(&mut self, me: &str, to: &str, out: &mut Vec<Out>) -> Option<[u8; 4]> {
        if to == me {
            return Some(*b"user");
        }
        if to.is_empty() || self.store.are_friends(me, to) {
            return None;
        }
        // asked by the other side already: that is a yes
        if self.store.pending.contains(&(to.to_string(), me.to_string())) {
            self.answer(me, to, "Y", out);
            return None;
        }
        if self.store.count(&self.store.pending, me) >= MAX_PENDING || self.store.friends_of(me).len() >= MAX_FRIENDS {
            return Some(*b"full");
        }
        if !self.store.known(to) {
            // an EA account had to exist; here, a name someone has signed in with
            return None;
        }
        // A player who blocked `me` never hears of it, now or at a later sign-in: nothing is
        // kept, and `me` reads it as declined.
        if self.store.blocked.contains(&(to.to_string(), me.to_string())) {
            return Some(*b"blck");
        }
        self.store.pending.insert((me.to_string(), to.to_string()));
        self.store.save();
        if let Some(c) = self.conn_of(me) {
            out.push((c, self.rnot(to, "ST", "A")));
        }
        if let Some(c) = self.conn_of(to) {
            out.push((c, self.rnot(me, "RT", "A")));
        }
        None
    }

    /// `me` answers `from`'s request: Y, N or B (block).
    fn answer(&mut self, me: &str, from: &str, answ: &str, out: &mut Vec<Out>) {
        if !self.store.pending.remove(&(from.to_string(), me.to_string())) {
            return;
        }
        if answ == "Y" && self.store.friends_of(me).len() < MAX_FRIENDS && self.store.friends_of(from).len() < MAX_FRIENDS {
            self.store.friends.insert(pair(me, from));
            self.store.save();
            for (a, b) in [(me, from), (from, me)] {
                if let Some(c) = self.conn_of(a) {
                    out.push((c, self.rnot(b, "AT", "X")));
                    // and each sees the other's presence from now on
                    if let Some(o) = self.online.get_mut(a) {
                        o.watching.insert(b.to_string());
                    }
                    let their = self.online.get(b).and_then(|o| o.presence.clone());
                    if let Some(pr) = their {
                        out.push((c, presence_packet(&self.store.display(b), Some(&pr), None, true)));
                    }
                }
            }
            return;
        }
        self.store.save();
        if answ == "B" {
            let _ = self.block(me, from, out);
        }
        for (a, b) in [(me, from), (from, me)] {
            if let Some(c) = self.conn_of(a) {
                out.push((c, self.rnot(b, "", "D")));
            }
        }
    }

    /// `me` drops `other`: a friend, or a request either way.
    fn remove(&mut self, me: &str, other: &str, out: &mut Vec<Out>) {
        let was = self.store.friends.remove(&pair(me, other));
        let asked = self.store.pending.remove(&(me.to_string(), other.to_string()));
        let asked_me = self.store.pending.remove(&(other.to_string(), me.to_string()));
        if !(was || asked || asked_me) {
            return;
        }
        self.store.save();
        for (a, b) in [(me, other), (other, me)] {
            if let Some(o) = self.online.get_mut(a) {
                o.watching.remove(b);
            }
            if let Some(c) = self.conn_of(a) {
                out.push((c, self.rnot(b, "", "D")));
            }
        }
    }

    /// `me` blocks `other` (and drops it as a friend or request). Refusals as the game reads
    /// them (0x833540): `user` "You cannot Add or Block yourself.", `full` "Your blocked list
    /// is full."
    fn block(&mut self, me: &str, other: &str, out: &mut Vec<Out>) -> Option<[u8; 4]> {
        if other == me {
            return Some(*b"user");
        }
        if other.is_empty() || self.store.blocked.contains(&(me.to_string(), other.to_string())) {
            return None;
        }
        if self.store.count(&self.store.blocked, me) >= MAX_BLOCKED {
            return Some(*b"full");
        }
        self.remove(me, other, out);
        self.store.blocked.insert((me.to_string(), other.to_string()));
        self.store.save();
        None
    }
}

/// PGET: a player's presence (`p`; none: offline). `id`: the answer's ID; `chng`: a change.
fn presence_packet(name: &str, p: Option<&Presence>, id: Option<&str>, chng: bool) -> Packet {
    let off = Presence { show: "DISC".into(), ..Presence::default() };
    let p = p.unwrap_or(&off);
    let mut pk = Packet::new(b"PGET", 0);
    if p.show != "DISC" {
        pk = pk.with("STAT", &p.stat).with("TIID", if p.tiid.is_empty() { "0" } else { &p.tiid }).with("TITL", if p.titl.is_empty() { TITLE } else { &p.titl });
    }
    if let Some(id) = id {
        pk = pk.with("ID", id);
    }
    pk = pk.with("SHOW", &p.show);
    if p.show != "DISC" {
        pk = pk.with("ATTR", &p.attr);
        if !p.sess.is_empty() {
            pk = pk.with("SESS", &p.sess);
        }
        if !p.prod.is_empty() {
            pk = pk.with("PROD", &p.prod);
        }
        if !p.extr.is_empty() {
            pk = pk.with("EXTR", &p.extr);
        }
    }
    if chng {
        pk = pk.with("CHNG", 1);
    }
    pk.with("USER", user_full(name))
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: ConnKey = (0x0a4d_0010, 50_000, 13_505, 1);
    const B: ConnKey = (0x0a4d_0011, 50_001, 13_505, 2);
    const C: ConnKey = (0x0a4d_0012, 50_002, 13_505, 3);

    fn pk(kind: &[u8; 4], fields: &[(&str, &str)]) -> Packet {
        let mut p = Packet::new(kind, 0);
        for (k, v) in fields {
            p = p.with(k, v);
        }
        p
    }

    fn to(out: &[Out], conn: ConnKey) -> Vec<String> {
        out.iter().filter(|(c, _)| *c == conn).map(|(_, p)| p.summary()).collect()
    }

    fn two() -> Messenger {
        let mut m = Messenger::default();
        m.store.claim("Metin", Some(&digest("a")), 1000);
        m.store.claim("Jan", Some(&digest("b")), 1000);
        m.sign_in(A, "Metin", 1);
        m.sign_in(B, "Jan", 1);
        m
    }

    fn friends(m: &mut Messenger) {
        m.handle(A, &pk(b"RADM", &[("USER", "Jan"), ("LRSC", "eagames"), ("PRES", "Y"), ("ID", "7")]), 10);
        m.handle(B, &pk(b"RRSP", &[("USER", "Metin"), ("ANSW", "Y"), ("ID", "3")]), 20);
    }

    #[test]
    fn a_request_accepted_makes_friends_and_both_hear_it() {
        let mut m = two();
        let out = m.handle(A, &pk(b"RADM", &[("USER", "Jan"), ("LRSC", "eagames"), ("PRES", "Y"), ("ID", "7")]), 10);
        let a = to(&out, A);
        assert!(a[0].starts_with("RADM 0x00000000 {ID=7, PRES=Y, LRSC=eagames, USER=Jan}"), "{a:?}");
        assert!(a[1].contains("ATTR=ST") && a[1].contains("CHNG=A") && a[1].contains("USER=Jan@messaging.ea.com"), "{a:?}");
        let b = to(&out, B);
        assert!(b[0].starts_with("RNOT") && b[0].contains("ATTR=RT") && b[0].contains("USER=Metin@messaging.ea.com"), "{b:?}");
        // Jan's roster shows the request; Metin's shows it as sent
        let out = m.handle(B, &pk(b"RGET", &[("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", "2")]), 11);
        let b = to(&out, B);
        assert_eq!(b[0], "RGET 0x00000000 {ID=2, SIZE=1}");
        assert!(b[1].contains("ATTR=RT") && b[1].contains("USER=Metin@messaging.ea.com"), "{b:?}");
        // accepted: both see AT
        let out = m.handle(B, &pk(b"RRSP", &[("USER", "Metin@messaging.ea.com"), ("ANSW", "Y"), ("ID", "3")]), 20);
        assert!(to(&out, A).iter().any(|s| s.contains("ATTR=AT") && s.contains("CHNG=X") && s.contains("USER=Jan@")));
        assert!(to(&out, B).iter().any(|s| s.contains("ATTR=AT") && s.contains("CHNG=X") && s.contains("USER=Metin@")));
        assert!(m.store.are_friends("metin", "jan"));
        let out = m.handle(A, &pk(b"RGET", &[("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", "2")]), 21);
        assert!(to(&out, A)[1].contains("ATTR=AT") && to(&out, A)[1].contains("USER=Jan@"));
    }

    #[test]
    fn declining_or_removing_tells_both_and_blocks_hold() {
        let mut m = two();
        m.handle(A, &pk(b"RADM", &[("USER", "Jan"), ("ID", "7")]), 10);
        let out = m.handle(B, &pk(b"RRSP", &[("USER", "Metin"), ("ANSW", "N"), ("ID", "3")]), 20);
        assert!(to(&out, A).iter().any(|s| s.contains("CHNG=D") && s.contains("USER=Jan@")));
        assert!(m.store.pending.is_empty() && m.store.friends.is_empty());
        friends(&mut m);
        let out = m.handle(A, &pk(b"RDEL", &[("LIST", "B"), ("USER", "Jan"), ("ID", "9")]), 30);
        assert!(to(&out, B).iter().any(|s| s.contains("CHNG=D") && s.contains("USER=Metin@")));
        assert!(m.store.friends.is_empty());
        // Jan blocks Metin: Metin's requests go nowhere, its chat neither
        m.handle(B, &pk(b"RADD", &[("LIST", "I"), ("USER", "Metin"), ("ID", "4")]), 31);
        let out = m.handle(A, &pk(b"RADM", &[("USER", "Jan"), ("ID", "8")]), 32);
        assert!(to(&out, B).is_empty());
        assert!(m.store.pending.is_empty(), "nothing kept for Jan to find at a later sign-in");
        // Metin's game reads it as "User has declined your friend request."
        assert!(to(&out, A)[0].starts_with(&format!("RADM 0x{:08x}", u32::from_be_bytes(*b"blck"))), "{:?}", to(&out, A));
        // asking or blocking oneself: "You cannot Add or Block yourself."
        let out = m.handle(A, &pk(b"RADM", &[("USER", "Metin"), ("ID", "8")]), 34);
        assert!(to(&out, A)[0].starts_with(&format!("RADM 0x{:08x}", u32::from_be_bytes(*b"user"))));
        let out = m.handle(A, &pk(b"RADD", &[("LIST", "I"), ("USER", "Metin"), ("ID", "8")]), 35);
        assert!(to(&out, A)[0].starts_with(&format!("RADD 0x{:08x}", u32::from_be_bytes(*b"user"))));
        let out = m.handle(B, &pk(b"RGET", &[("LIST", "I"), ("ID", "5")]), 33);
        assert!(to(&out, B)[1].contains("USER=Metin@"));
    }

    #[test]
    fn presence_reaches_watching_friends_and_offline_when_gone() {
        let mut m = two();
        friends(&mut m);
        m.handle(A, &pk(b"PSET", &[("SHOW", "GAME"), ("ATTR", "J"), ("STAT", "en%3dPlaying"), ("ID", "5")]), 40);
        // Jan's roster with PRES=Y brings Metin's presence (auto-subscribe)
        let out = m.handle(B, &pk(b"RGET", &[("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", "2")]), 41);
        let b = to(&out, B);
        assert!(b.iter().any(|s| s.starts_with("PGET") && s.contains("ID=auto-subscribe%3a") && s.contains("SHOW=GAME") && s.contains("ATTR=J")), "{b:?}");
        // a change goes to Jan
        let out = m.handle(A, &pk(b"PSET", &[("SHOW", "CHAT"), ("ID", "6")]), 42);
        assert!(to(&out, B).iter().any(|s| s.contains("SHOW=CHAT") && s.contains("CHNG=1") && s.contains("USER=Metin@messaging.ea.com/eagames/NFS-2007")));
        // Metin's connection ends: Jan sees DISC
        m.closed(A);
        assert_eq!(m.outbox.len(), 1);
        assert!(m.outbox[0].0 == B && m.outbox[0].1.summary().contains("SHOW=DISC"));
        assert!(!m.is_online("Metin"));
    }

    #[test]
    fn chat_and_invites_go_to_friends_only_and_are_limited() {
        let mut m = two();
        m.sign_in(C, "Ada", 1);
        let out = m.handle(A, &pk(b"SEND", &[("TYPE", "C"), ("USER", "Jan"), ("BODY", "hi"), ("ID", "9")]), 50);
        assert!(to(&out, B).is_empty(), "not friends yet");
        friends(&mut m);
        let out = m.handle(A, &pk(b"SEND", &[("TYPE", "C"), ("USER", "Jan"), ("BODY", "hi \"there\""), ("ID", "9")]), 60);
        let b = to(&out, B);
        assert!(b[0].starts_with("RECV") && b[0].contains("TYPE=C") && b[0].contains("USER=Metin@messaging.ea.com/eagames/NFS-2007"), "{b:?}");
        assert_eq!(out.iter().find(|(c, _)| *c == B).unwrap().1.get("BODY"), Some("hi there"));
        // a burst, then the rest is dropped
        let sent = (0..20).filter(|_| !to(&m.handle(A, &pk(b"SEND", &[("TYPE", "C"), ("USER", "Jan"), ("BODY", "x"), ("ID", "9")]), 61), B).is_empty()).count();
        assert!(sent <= CHAT_BURST as usize, "{sent}");
        // and the sender's game is told why ("You cannot send a message that often.")
        let out = m.handle(A, &pk(b"SEND", &[("TYPE", "C"), ("USER", "Jan"), ("BODY", "x"), ("ID", "9")]), 62);
        assert_eq!(to(&out, A)[0], format!("SEND 0x{:08x} {{ID=9}}", u32::from_be_bytes(*b"rate")));
        // an invite and its answer
        let out = m.handle(A, &pk(b"GINV", &[("USER", "Jan"), ("SESS", "200000"), ("GSTR", "x"), ("ID", "10")]), 70);
        assert!(to(&out, B)[0].starts_with("GNOT") && to(&out, B)[0].contains("TYPE=I") && to(&out, B)[0].contains("SESS=200000"));
        let out = m.handle(B, &pk(b"GRSP", &[("USER", "Metin"), ("ANSW", "Y"), ("ID", "4")]), 71);
        assert!(to(&out, A)[0].contains("TYPE=A") && to(&out, A)[0].contains("USER=Jan@"));
        // Ada is nobody's friend: nothing reaches her
        let out = m.handle(A, &pk(b"GINV", &[("USER", "Ada"), ("SESS", "1"), ("ID", "11")]), 72);
        assert!(to(&out, C).is_empty());
    }

    #[test]
    fn names_are_claimed_by_key_and_lapse_after_a_month() {
        let mut s = Store::default();
        assert!(s.claim("Metin", Some(&digest("a")), 100));
        assert!(!s.claim("metin", Some(&digest("b")), 200), "another browser");
        assert!(!s.claim("Metin", None, 200), "no key, claimed name");
        assert!(s.claim("Metin", Some(&digest("a")), 300), "its own browser");
        assert!(s.claim("Nobody", None, 300) && s.people["nobody"].key.is_none(), "no key: used, not claimed");
        assert!(s.claim("Nobody", Some(&digest("c")), 301), "then claimed by a key");
        s.friends.insert(pair("metin", "nobody"));
        // a month without Metin: another browser may take the name, and starts empty
        assert!(s.claim("Metin", Some(&digest("b")), 300 + CLAIM_LAPSE_SECS));
        assert!(s.friends.is_empty());
        assert!(!s.claim("Metin", Some(&digest("a")), 301 + CLAIM_LAPSE_SECS));
    }

    #[test]
    fn a_full_store_makes_room_only_from_names_that_hold_nothing() {
        let mut s = Store::default();
        for i in 0..MAX_PEOPLE {
            assert!(s.claim(&format!("p{i}"), Some(&digest(&format!("k{i}"))), 1000 + i as u64));
        }
        // every name is claimed: a new one signs in without being kept
        assert!(s.claim("Late", Some(&digest("late")), 5000));
        assert!(!s.known("late") && s.people.len() == MAX_PEOPLE);
        // a month on, the claims have lapsed: the longest-unseen name without friends goes
        s.friends.insert(pair("p0", "p1"));
        let later = 1000 + CLAIM_LAPSE_SECS + MAX_PEOPLE as u64;
        assert!(s.claim("Late", Some(&digest("late")), later));
        assert!(s.known("late") && s.known("p0") && s.known("p1") && !s.known("p2"));
        assert_eq!(s.people.len(), MAX_PEOPLE);
    }

    #[test]
    fn a_second_sign_in_logs_the_first_off() {
        let mut m = two();
        let out = m.sign_in(C, "Metin", 5);
        assert_eq!(out.len(), 1);
        assert!(out[0].0 == A && out[0].1.summary().contains("TYPE=DUPL"));
        // the old connection's packets are no one's now; its end changes nothing
        assert!(m.handle(A, &pk(b"PSET", &[("SHOW", "CHAT")]), 6).is_empty());
        m.closed(A);
        assert!(m.is_online("Metin"));
    }

    #[test]
    fn the_store_survives_a_restart() {
        let dir = std::env::temp_dir().join(format!("carbon-messenger-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        {
            let mut m = Messenger::new(Some(dir.clone()));
            m.store.claim("Metin", Some(&digest("a")), 1000);
            m.store.claim("Jan", Some(&digest("b")), 1000);
            m.sign_in(A, "Metin", 1);
            m.sign_in(B, "Jan", 1);
            friends(&mut m);
        }
        let text = fs::read_to_string(dir.join("messenger.json")).unwrap();
        assert!(!text.contains("\"a\"") && text.contains(&hex(&digest("a"))), "only the digest");
        let mut m = Messenger::new(Some(dir.clone()));
        assert!(m.store.are_friends("metin", "jan"));
        assert!(!m.store.claim("Metin", Some(&digest("x")), 2000));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn search_finds_known_names() {
        let mut m = two();
        let out = m.handle(A, &pk(b"USCH", &[("USER", "ja"), ("MAXR", "5"), ("ID", "6")]), 1);
        let a = to(&out, A);
        assert_eq!(a[0], "USCH 0x00000000 {ID=6, SIZE=1}");
        assert!(a[1].contains("USER=Jan"));
    }
}
