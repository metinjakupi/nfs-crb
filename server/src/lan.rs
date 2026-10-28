//! GET /lan: the switch of the virtual network that the game's own Online mode talks through
//! (plans/lan-protocol.md; the design is nfsu2.xyz's, MIT, see NOTICE). Browsers cannot open
//! raw sockets, so the kit answers the game's Winsock calls and every packet travels as a
//! binary WebSocket frame to this switch, which routes it by virtual address:
//!
//! - Each WebSocket gets a virtual address (vip) in 10.77.0.0/21, from 10.77.0.16 upwards (the
//!   next free one after the last handed out, so a just-freed address is not reused at once;
//!   never one ending in .0 or .255), and is told so first: {"t":"lan","vip":..,"netmask":..}.
//! - Frames: [0] kind, [1..3] 0, [4..7] source vip, [8..11] destination vip, [12..13] source
//!   port, [14..15] destination port, [16..] payload = 8-byte LAN header ([0] op, [1] ttl,
//!   [4..7] connection id, little-endian) + data; big-endian; payload at most 1472 bytes.
//!   Kinds 3 (UDP: ops 0 data, 1 unreachable) and 4 (TCP: 1 SYN, 2 SYNACK, 3 RST, 4 DATA,
//!   5 FIN) are routed; anything else, shorter or longer is dropped.
//! - The source is always rewritten to the sender's own vip (no spoofing).
//! - 10.77.7.255 and 255.255.255.255 are broadcasts: UDP only, to every vip but the sender.
//! - 10.77.0.1 is the server's own host: the trace host (trace.rs), with the stand-ins for
//!   EA's services on their ports (ea.rs).
//! - When a vip leaves, every browser it had a TCP connection with through the switch gets a
//!   TCP GONE frame (op 6) per connection, and every browser the {"t":"gone","vip":..} message.
//! - Text frames from the page are JSON: {"t":"hello","name":..,"pk":..} names it in the logs;
//!   pk, the page's player key, claims its name for EA Messenger (only its SHA-256 is kept).
//!
//! Limits: per client address (X-Real-IP behind nginx) PER_CLIENT connections; per connection
//! a budget of frames and bytes a second (a broadcast costs one per receiver), past which it
//! is closed (1008 "too fast"); a receiver more than UDP_SHED behind loses datagrams, and one
//! more than OUT_LIMIT behind is cut off (a TCP segment is never dropped: the kit's streams
//! have no retransmission).

use std::collections::{BTreeMap, HashMap};
use std::net::Ipv4Addr;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use serde_json::Value;

use crate::http::Conn;
use crate::trace::Trace;
use crate::util::{label, lock};
use crate::ws::{self, close_frame, Outbox, Session};
use crate::App;

pub const NETMASK: u32 = 0xffff_f800; // /21: 10.77.0.0 - 10.77.7.255
pub const HOST: u32 = 0x0a4d_0001; // 10.77.0.1
const FIRST: u32 = 0x0a4d_0010; // 10.77.0.16
const LAST: u32 = 0x0a4d_07fe; // 10.77.7.254
const NET_BROADCAST: u32 = 0x0a4d_07ff;
const BROADCAST: u32 = 0xffff_ffff;

/// How often the race hosts' timers run.
pub const TICK_MS: u64 = 20;
pub const KIND_UDP: u8 = 3;
pub const KIND_TCP: u8 = 4;
pub const HDR: usize = 16;
pub const LAN_HDR: usize = 8;
pub const MAX_PAYLOAD: usize = 1472;
pub const SYN: u8 = 1;
pub const SYNACK: u8 = 2;
pub const RST: u8 = 3;
pub const DATA: u8 = 4;
pub const FIN: u8 = 5;
pub const GONE: u8 = 6;

const OUT_LIMIT: usize = 256 * 1024;
const UDP_SHED: usize = 64 * 1024;
const PER_CLIENT: usize = 8;
/// TCP connections followed per vip (for GONE); past it a vip's new ones are not followed and
/// its peers learn of its departure from the gone message alone.
const LINKS_PER_VIP: u32 = 256;
/// Per connection: a burst, then a rate a second. A LAN race sends about 100 frames a second.
const FRAME_BURST: f64 = 2000.0;
const FRAME_RATE: f64 = 1000.0;
const BYTE_BURST: f64 = 1024.0 * 1024.0;
const BYTE_RATE: f64 = 512.0 * 1024.0;

pub fn vip_str(v: u32) -> String {
    Ipv4Addr::from(v).to_string()
}

/// A switch frame of a LAN kind.
#[allow(clippy::too_many_arguments)]
pub fn lan_frame(kind: u8, src: u32, dst: u32, sport: u16, dport: u16, op: u8, conn: u32, data: &[u8]) -> Vec<u8> {
    let mut f = vec![0u8; HDR + LAN_HDR + data.len()];
    f[0] = kind;
    f[4..8].copy_from_slice(&src.to_be_bytes());
    f[8..12].copy_from_slice(&dst.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[HDR] = op;
    f[HDR + 1] = 64;
    f[HDR + 4..HDR + 8].copy_from_slice(&conn.to_le_bytes());
    f[HDR + LAN_HDR..].copy_from_slice(data);
    f
}

fn binary(f: &[u8]) -> Arc<[u8]> {
    ws::frame(f, 2).into()
}

/// A TCP connection between two browsers that the switch has seen open: (vip, port) of each
/// end, the lower first, and the connection id.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct Link {
    a: (u32, u16),
    b: (u32, u16),
    conn: u32,
}

struct Peer {
    name: String,
    client: String,
    out: Arc<Outbox>,
    links: u32,
}

struct State {
    peers: BTreeMap<u32, Peer>,
    /// where the search for the next free vip starts
    cursor: u32,
    /// -> (a sent FIN, b sent FIN)
    links: HashMap<Link, (bool, bool)>,
    clients: HashMap<String, usize>,
    trace: Trace,
}

pub struct Switch {
    st: Mutex<State>,
}

/// A connection's frame budget.
pub struct Budget {
    frames: f64,
    bytes: f64,
    at: Instant,
}

impl Budget {
    fn new() -> Budget {
        Budget { frames: FRAME_BURST, bytes: BYTE_BURST, at: Instant::now() }
    }

    fn take(&mut self, frames: usize, bytes: usize) -> bool {
        let now = Instant::now();
        let dt = now.duration_since(self.at).as_secs_f64();
        self.at = now;
        self.frames = (self.frames + dt * FRAME_RATE).min(FRAME_BURST);
        self.bytes = (self.bytes + dt * BYTE_RATE).min(BYTE_BURST);
        if frames as f64 > self.frames || bytes as f64 > self.bytes {
            return false;
        }
        self.frames -= frames as f64;
        self.bytes -= bytes as f64;
        true
    }
}

fn usable(v: u32) -> bool {
    (FIRST..=LAST).contains(&v) && !matches!(v & 0xff, 0 | 255)
}

impl Switch {
    pub fn new(trace: Trace) -> Switch {
        Switch {
            st: Mutex::new(State { peers: BTreeMap::new(), cursor: FIRST, links: HashMap::new(), clients: HashMap::new(), trace }),
        }
    }

    pub fn describe(&self) -> String {
        lock(&self.st).trace.describe()
    }

    /// A vip for a new connection, with the welcome already queued (so it is the first thing
    /// the page hears), or None when the client has its share or the network is full.
    fn admit(&self, client: &str, out: Arc<Outbox>) -> Option<u32> {
        let mut st = lock(&self.st);
        if st.clients.get(client).copied().unwrap_or(0) >= PER_CLIENT {
            return None;
        }
        let mut v = st.cursor;
        let vip = loop {
            if usable(v) && !st.peers.contains_key(&v) {
                break v;
            }
            v = if v >= LAST { FIRST } else { v + 1 };
            if v == st.cursor {
                return None;
            }
        };
        st.cursor = if vip >= LAST { FIRST } else { vip + 1 };
        *st.clients.entry(client.to_string()).or_default() += 1;
        let welcome = format!(r#"{{"t":"lan","vip":"{}","netmask":"{}"}}"#, vip_str(vip), vip_str(NETMASK));
        out.push(0, ws::frame(welcome.as_bytes(), 1).into());
        st.peers.insert(vip, Peer { name: String::new(), client: client.to_string(), out, links: 0 });
        println!("lan: {} joined, {} on the network", vip_str(vip), st.peers.len());
        Some(vip)
    }

    fn hello(&self, vip: u32, name: String) {
        let mut st = lock(&self.st);
        if let Some(p) = st.peers.get_mut(&vip) {
            if p.name != name {
                println!("lan: {} is {name}", vip_str(vip));
                p.name = name;
            }
        }
    }

    /// The page's player key: the trace host keeps its digest for FESL Login. Never logged.
    fn player_key(&self, vip: u32, key: &str) {
        lock(&self.st).trace.set_player_key(vip, key);
    }

    /// Routes one frame from `from`; false when the sender is over its budget.
    pub fn route(&self, from: u32, mut f: Vec<u8>, budget: &mut Budget) -> bool {
        if f.len() < HDR + LAN_HDR || f.len() > HDR + MAX_PAYLOAD {
            return true;
        }
        let (kind, op) = (f[0], f[HDR]);
        let known = match kind {
            KIND_UDP => op <= 1,
            KIND_TCP => (SYN..=FIN).contains(&op),
            _ => false,
        };
        if !known {
            return true;
        }
        f[4..8].copy_from_slice(&from.to_be_bytes());
        let dst = u32::from_be_bytes([f[8], f[9], f[10], f[11]]);
        let sport = u16::from_be_bytes([f[12], f[13]]);
        let dport = u16::from_be_bytes([f[14], f[15]]);
        let conn = u32::from_le_bytes([f[HDR + 4], f[HDR + 5], f[HDR + 6], f[HDR + 7]]);
        let mut st = lock(&self.st);
        if dst == NET_BROADCAST || dst == BROADCAST {
            if kind != KIND_UDP {
                return true;
            }
            let n = st.peers.len().saturating_sub(1);
            if !budget.take(n, n * f.len()) {
                return false;
            }
            let bytes = binary(&f);
            for (_, p) in st.peers.iter().filter(|(v, _)| **v != from) {
                p.out.push_droppable(bytes.clone(), UDP_SHED);
            }
            return true;
        }
        if !budget.take(1, f.len()) {
            return false;
        }
        if dst == HOST {
            let data = &f[HDR + LAN_HDR..];
            if kind == KIND_UDP {
                for (to, from_port, to_port, bytes) in st.trace.udp(from, sport, dport, op, data) {
                    if let Some(p) = st.peers.get(&to) {
                        p.out.push_droppable(binary(&lan_frame(KIND_UDP, HOST, to, from_port, to_port, 0, 0, &bytes)), UDP_SHED);
                    }
                }
            } else {
                // Mostly for the sender; Theater's answers can be for another player's
                // connection (a join request to a game's host).
                for a in st.trace.tcp(from, sport, dport, op, conn, data) {
                    let (vip, cport, port, id) = a.to;
                    let Some(p) = st.peers.get(&vip) else { continue };
                    // A stand-in's answer can be longer than one frame carries.
                    let mut pieces: Vec<&[u8]> = a.data.chunks(MAX_PAYLOAD - LAN_HDR).collect();
                    if pieces.is_empty() {
                        pieces.push(&[]);
                    }
                    for piece in pieces {
                        p.out.push(0, binary(&lan_frame(KIND_TCP, HOST, vip, port, cport, a.op, id, piece)));
                    }
                }
            }
            return true;
        }
        let Some(p) = st.peers.get(&dst) else { return true };
        if kind == KIND_UDP {
            p.out.push_droppable(binary(&f), UDP_SHED);
        } else {
            p.out.push(0, binary(&f));
            st.track(from, sport, dst, dport, op, conn);
        }
        true
    }

    /// The race hosts' timers and the stand-ins' keep-alives: called every TICK_MS by the
    /// server's ticker.
    pub fn tick(&self) {
        let mut st = lock(&self.st);
        for (to, from_port, to_port, bytes) in st.trace.tick() {
            if let Some(p) = st.peers.get(&to) {
                p.out.push_droppable(binary(&lan_frame(KIND_UDP, HOST, to, from_port, to_port, 0, 0, &bytes)), UDP_SHED);
            }
        }
        for a in st.trace.keepalive() {
            let (vip, cport, port, id) = a.to;
            if let Some(p) = st.peers.get(&vip) {
                p.out.push(0, binary(&lan_frame(KIND_TCP, HOST, vip, port, cport, a.op, id, &a.data)));
            }
        }
    }

    /// A vip left: its TCP peers get GONE, everyone the gone message, the host forgets it.
    fn leave(&self, vip: u32) {
        let mut st = lock(&self.st);
        let Some(gone) = st.peers.remove(&vip) else { return };
        if let Some(n) = st.clients.get_mut(&gone.client) {
            *n -= 1;
            if *n == 0 {
                st.clients.remove(&gone.client);
            }
        }
        let links: Vec<Link> = st.links.keys().filter(|l| l.a.0 == vip || l.b.0 == vip).copied().collect();
        for l in links {
            st.links.remove(&l);
            let (mine, other) = if l.a.0 == vip { (l.a, l.b) } else { (l.b, l.a) };
            if let Some(p) = st.peers.get_mut(&other.0) {
                p.links -= 1;
                p.out.push(0, binary(&lan_frame(KIND_TCP, vip, other.0, mine.1, other.1, GONE, l.conn, &[])));
            }
        }
        st.trace.gone(vip);
        let msg: Arc<[u8]> = ws::frame(format!(r#"{{"t":"gone","vip":"{}"}}"#, vip_str(vip)).as_bytes(), 1).into();
        for p in st.peers.values() {
            p.out.push(0, msg.clone());
        }
        let name = if gone.name.is_empty() { String::new() } else { format!("{} ", gone.name) };
        println!("lan: {name}({}) left, {} on the network", vip_str(vip), st.peers.len());
    }
}

impl State {
    /// Follows TCP connections between browsers from SYN to RST or both FINs, for GONE.
    fn track(&mut self, src: u32, sport: u16, dst: u32, dport: u16, op: u8, conn: u32) {
        let from_a = (src, sport) <= (dst, dport);
        let (a, b) = if from_a { ((src, sport), (dst, dport)) } else { ((dst, dport), (src, sport)) };
        let link = Link { a, b, conn };
        match op {
            SYN if !self.links.contains_key(&link) => {
                let room = |v: u32| self.peers.get(&v).is_some_and(|p| p.links < LINKS_PER_VIP);
                if room(src) && room(dst) {
                    self.links.insert(link, (false, false));
                    for v in [src, dst] {
                        if let Some(p) = self.peers.get_mut(&v) {
                            p.links += 1;
                        }
                    }
                }
            }
            RST => self.unlink(link),
            FIN => {
                if let Some(fins) = self.links.get_mut(&link) {
                    if from_a { fins.0 = true } else { fins.1 = true }
                    if fins.0 && fins.1 {
                        self.unlink(link);
                    }
                }
            }
            _ => {}
        }
    }

    fn unlink(&mut self, link: Link) {
        if self.links.remove(&link).is_some() {
            for v in [link.a.0, link.b.0] {
                if let Some(p) = self.peers.get_mut(&v) {
                    p.links -= 1;
                }
            }
        }
    }
}

struct Node {
    app: Arc<App>,
    vip: u32,
    out: Arc<Outbox>,
    budget: Budget,
}

impl Session for Node {
    fn text(&mut self, text: &str) {
        let Ok(Value::Object(m)) = serde_json::from_str::<Value>(text) else { return };
        if m.get("t").and_then(Value::as_str) == Some("hello") {
            self.app.lan.hello(self.vip, label(m.get("name"), "Player", 32));
            // a page from before player keys sends none: its name is used, not claimed
            let pk = m.get("pk").and_then(Value::as_str).filter(|k| (16..=128).contains(&k.len())).unwrap_or("");
            self.app.lan.player_key(self.vip, pk);
        }
    }

    fn binary(&mut self, data: Vec<u8>) {
        if !self.app.lan.route(self.vip, data, &mut self.budget) {
            println!("lan: {} sent too fast; closed", vip_str(self.vip));
            self.out.finish(Some(close_frame(1008, "too fast")));
        }
    }

    fn alive(&mut self, _now: Instant) -> bool {
        true
    }

    fn leave(&mut self) {
        self.app.lan.leave(self.vip);
    }
}

/// A browser on the network, from the 101 answer to its departure.
pub async fn join(app: Arc<App>, conn: Conn, client: String, accept: String) {
    let out = Outbox::new(OUT_LIMIT);
    // No place: the connection is closed without an answer, as for a full relay room.
    let Some(vip) = app.lan.admit(&client, out.clone()) else { return };
    let Some((rd, buf, writer)) = ws::open(conn, &accept, &out).await else {
        app.lan.leave(vip);
        return;
    };
    let mut node = Node { app, vip, out: out.clone(), budget: Budget::new() };
    ws::run(rd, buf, out, writer, &mut node).await;
}
