//! The race host: a stand-in for the program EA ran every Carbon race on (game lists named it
//! HN=nfsdevserver, B-version 298_prod_server; the client's status text calls it "rb", the
//! rebroadcaster). Carbon's client only ever joins, so without this nobody can race in the
//! game's own Custom Match. The host lives at 10.77.0.1 on a UDP port of its own and speaks,
//! from the outside in, what the client's code expects of a host (NFSC.exe v1.4; the
//! addresses are the exe's):
//!
//! 1. ProtoTunnel (tunnel.rs): the encrypted envelope on UDP.
//! 2. CommUDP, DirtySock's reliable UDP, on tunnel port index 1 (virtual port 6001). Packets
//!    are [seq u32][ack u32][body], big-endian (_CommUDPWrite 00988fe0):
//!    - seq 1 INIT, 2 CONN, 3 DISC, 4 NAK, 5 POKE, with ack = the connection's id (a hash of
//!      the "#session" part of the address, 00988f20). The joiner sends INIT once a second
//!      (009898a0); a host answers CONN with the same id, and the joiner's link is open
//!      (00989100).
//!    - 0x80..0xff unreliable data, 0x100.. reliable data in order; ack = the last reliable
//!      seq received. A gap is answered with NAK (ack = the seq expected, 009895d0), which
//!      makes the sender resend from there (00989530). A packet with no body only carries
//!      its ack. The top 4 bits of seq count older packets bundled at the end of the body,
//!      each followed by its length byte (009891f0); the receiver takes them oldest first.
//! 3. NetGameLink, one packet per CommUDP data packet: [payload][12-byte sync if kind has
//!    0x40][kind] (0098aff0 send, 0098b160 receive). The sync is [echo u32][time u32]
//!    [latency u16][2 spare]: the sender's clock, and the other end's last clock plus the
//!    time since it arrived, for the round trip. Kind 4 is user data sent reliably, 5
//!    unreliably, 0 a sync alone (sent reliably every 500 ms when idle, 0098b430), 2 a
//!    disconnect. A link that hears nothing for 10 s is lost (0095fc0d).
//! 4. GameManager (FESL SDK 2.9), in kind 4/5 payloads: [system flag][message]. System
//!    messages are GameManager's own, the rest the game's (OLMSG, for the rebroadcaster).
//!    Messages are written with the SDK's encoder (008e7ce0): big-endian integers, signed
//!    ones biased by half their range (i8 + 0x80, i16 + 0x8000, i32/i64 + 2^31/2^63),
//!    strings as an i32 length and the bytes. A message starts with its type as an i8:
//!    0 hello (joiner: version 4, its TICKET), 2 host hello, 3 roster element, 4 roster ack,
//!    5 join announcement, 1 goodbye; handlers registered at 0095cab0 (joiner) and 0095d1c0
//!    (host). The host side below follows the SDK's own host code: hello 0095c800, host
//!    hello and roster 00956b40, roster ack 0095c000.
//!
//! As EA's dedicated hosts did, the game's topology is client-server (0): the players talk
//! only to the host, never to each other, and the host passes the game's messages on.

use std::collections::{BTreeMap, VecDeque};
use std::fmt::Write as _;
use std::net::Ipv4Addr;
use std::time::Instant;

use crate::olmsg;
use crate::stats::{RaceReport, Racer};
use crate::tunnel::{self, Part, Refused, Stream};

/// CommUDP packet types (seq values below the data range).
const INIT: u32 = 1;
const CONN: u32 = 2;
const DISC: u32 = 3;
const NAK: u32 = 4;
const POKE: u32 = 5;
const UNRELIABLE: u32 = 0x80;
const RELIABLE: u32 = 0x100;
/// The CommUDP port index in GameManager's tunnels (virtual port 6001).
const GM_PORT: u8 = 1;
/// GameManager's protocol version (the joiner checks it, 0095c328).
const GM_VERSION: i16 = 4;
/// NetGameLink kinds.
const NGL_USER: u8 = 4;
const NGL_USER_UNRELIABLE: u8 = 5;
const NGL_SYNC: u8 = 0x40;
/// GameManager message types.
const GM_HELLO: i8 = 0;
const GM_GOODBYE: i8 = 1;
const GM_HOST_HELLO: i8 = 2;
const GM_ROSTER: i8 = 3;
const GM_ROSTER_ACK: i8 = 4;
const GM_JOIN: i8 = 5;
/// Player states as GameManager numbers them: 2 said hello, 6 in the game (0095a520).
const PLAYER_HELLO: i8 = 2;
const PLAYER_ACTIVE: i8 = 6;

/// A peer heard from nothing for this long is gone.
const PEER_TIMEOUT_MS: u64 = 30_000;
const RESEND_MS: u64 = 300;
const SYNC_MS: u64 = 400;
const MAX_UNACKED: usize = 256;
const MAX_PEERS: usize = 16;
/// Racer ids per player (GameInitInfo gives player n the ids from 1 + 4n).
const RACERS_PER_PLAYER: u32 = 4;
/// How long the host waits for every player's GameResults before FinalGameResults.
const RESULTS_WAIT_MS: u64 = 10_000;

/// The SDK's encoder, as GameManager uses it.
#[derive(Default)]
pub struct Enc(pub Vec<u8>);

impl Enc {
    pub fn u8(&mut self, v: u8) -> &mut Enc {
        self.0.push(v);
        self
    }
    pub fn i8(&mut self, v: i8) -> &mut Enc {
        self.0.push((v as u8) ^ 0x80);
        self
    }
    pub fn u16(&mut self, v: u16) -> &mut Enc {
        self.0.extend_from_slice(&v.to_be_bytes());
        self
    }
    pub fn i16(&mut self, v: i16) -> &mut Enc {
        self.0.extend_from_slice(&((v as u16) ^ 0x8000).to_be_bytes());
        self
    }
    pub fn u32(&mut self, v: u32) -> &mut Enc {
        self.0.extend_from_slice(&v.to_be_bytes());
        self
    }
    pub fn u64(&mut self, v: u64) -> &mut Enc {
        self.0.extend_from_slice(&v.to_be_bytes());
        self
    }
    pub fn i64(&mut self, v: i64) -> &mut Enc {
        self.0.extend_from_slice(&((v as u64) ^ (1 << 63)).to_be_bytes());
        self
    }
    pub fn str(&mut self, s: &str) -> &mut Enc {
        let n = s.len() as u32;
        self.0.extend_from_slice(&(n ^ 0x8000_0000).to_be_bytes());
        self.0.extend_from_slice(s.as_bytes());
        self
    }
}

/// The SDK's decoder; None once it runs out (the SDK's "Packet encoding error!").
pub struct Dec<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Dec<'a> {
    pub fn new(b: &'a [u8]) -> Dec<'a> {
        Dec { b, at: 0 }
    }
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let s = self.b.get(self.at..self.at + n)?;
        self.at += n;
        Some(s)
    }
    pub fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|s| s[0])
    }
    pub fn i8(&mut self) -> Option<i8> {
        self.u8().map(|v| (v ^ 0x80) as i8)
    }
    pub fn u16(&mut self) -> Option<u16> {
        self.take(2).map(|s| u16::from_be_bytes([s[0], s[1]]))
    }
    pub fn i16(&mut self) -> Option<i16> {
        self.u16().map(|v| (v ^ 0x8000) as i16)
    }
    pub fn str(&mut self) -> Option<String> {
        let n = self.take(4).map(|s| u32::from_be_bytes([s[0], s[1], s[2], s[3]]) ^ 0x8000_0000)? as usize;
        if n > 0x400 {
            return None;
        }
        self.take(n).map(|s| String::from_utf8_lossy(s).into_owned())
    }
}

/// A player Theater sent to this host (EGAM): who to expect on the tunnel.
#[derive(Clone, Debug)]
pub struct Expected {
    pub pid: u16,
    pub user_id: u64,
    pub name: String,
    pub ticket: String,
    pub vip: u32,
}

/// One end of CommUDP: the host's side of a joiner's connection.
#[derive(Default)]
struct Cudp {
    ident: u32,
    open: bool,
    /// next reliable seq to send
    send_seq: u32,
    /// next unreliable seq to send (0x80..0xff)
    unrel_seq: u32,
    /// next reliable seq expected
    recv_seq: u32,
    /// sent and not yet acknowledged: (seq, body)
    unacked: VecDeque<(u32, Vec<u8>)>,
    /// when the oldest unacknowledged packet was last sent
    resent_at: u64,
    /// something arrived that the other end should see acknowledged
    ack_due: bool,
    /// CommUDP packets to put in the next datagrams
    outbox: Vec<Vec<u8>>,
}

impl Cudp {
    fn reset(&mut self, ident: u32) {
        *self = Cudp { ident, open: true, send_seq: RELIABLE, unrel_seq: UNRELIABLE, recv_seq: RELIABLE, ..Cudp::default() };
    }

    fn packet(seq: u32, ack: u32, body: &[u8]) -> Vec<u8> {
        let mut p = Vec::with_capacity(8 + body.len());
        p.extend_from_slice(&seq.to_be_bytes());
        p.extend_from_slice(&ack.to_be_bytes());
        p.extend_from_slice(body);
        p
    }

    fn send_reliable(&mut self, body: Vec<u8>, now: u64) {
        let seq = self.send_seq;
        self.send_seq = self.send_seq.wrapping_add(1);
        self.outbox.push(Cudp::packet(seq, self.recv_seq.wrapping_sub(1), &body));
        if self.unacked.is_empty() {
            self.resent_at = now;
        }
        self.unacked.push_back((seq, body));
        while self.unacked.len() > MAX_UNACKED {
            self.unacked.pop_front();
        }
        self.ack_due = false;
    }

    fn send_unreliable(&mut self, body: &[u8]) {
        let seq = self.unrel_seq;
        self.unrel_seq = if self.unrel_seq >= 0xff { UNRELIABLE } else { self.unrel_seq + 1 };
        self.outbox.push(Cudp::packet(seq, self.recv_seq.wrapping_sub(1), body));
        self.ack_due = false;
    }

    fn acked(&mut self, ack: u32) {
        while self.unacked.front().is_some_and(|(s, _)| *s <= ack) {
            self.unacked.pop_front();
        }
    }

    /// One CommUDP packet from the joiner: the NetGameLink packets it delivers, in order, and
    /// a note when the connection opened or ended.
    fn receive(&mut self, p: &[u8]) -> (Vec<Vec<u8>>, Option<String>) {
        if p.len() < 8 {
            return (Vec::new(), None);
        }
        let seq = u32::from_be_bytes([p[0], p[1], p[2], p[3]]);
        let ack = u32::from_be_bytes([p[4], p[5], p[6], p[7]]);
        let body = &p[8..];
        match seq {
            INIT => {
                if !self.open || self.ident != ack {
                    self.reset(ack);
                }
                self.outbox.push(Cudp::packet(CONN, ack, &[]));
                return (Vec::new(), Some(format!("CommUDP INIT (connection {ack:08x}): answered CONN")));
            }
            CONN | POKE => return (Vec::new(), None),
            DISC => {
                self.open = false;
                return (Vec::new(), Some("CommUDP DISC: the player closed the connection".into()));
            }
            NAK => {
                self.acked(ack.wrapping_sub(1));
                self.resent_at = 0;
                return (Vec::new(), None);
            }
            _ => {}
        }
        if !self.open {
            return (Vec::new(), None);
        }
        self.acked(ack);
        // Bundled older packets sit at the end, each followed by its length; the oldest is
        // the last one, numbered (seq - count).
        let count = (seq >> 28) as usize;
        let first = seq & 0x0fff_ffff;
        let mut subs: Vec<(u32, &[u8])> = Vec::new();
        let mut end = body.len();
        for k in 0..count {
            if end == 0 {
                break;
            }
            let len = body[end - 1] as usize;
            if len + 1 > end {
                break;
            }
            subs.push((first - count as u32 + k as u32, &body[end - 1 - len..end - 1]));
            end -= len + 1;
        }
        subs.push((first, &body[..end]));
        let mut out = Vec::new();
        for (s, data) in subs {
            if (UNRELIABLE..RELIABLE).contains(&s) {
                if !data.is_empty() {
                    out.push(data.to_vec());
                }
            } else if s < self.recv_seq {
                self.ack_due = true;
            } else if s > self.recv_seq {
                self.outbox.push(Cudp::packet(NAK, self.recv_seq, &[]));
            } else if !data.is_empty() {
                out.push(data.to_vec());
                self.recv_seq += 1;
                self.ack_due = true;
            }
        }
        (out, None)
    }

    /// Resends and acknowledgements due at `now`.
    fn tick(&mut self, now: u64) {
        if !self.open {
            return;
        }
        if !self.unacked.is_empty() && now.saturating_sub(self.resent_at) >= RESEND_MS {
            let ack = self.recv_seq.wrapping_sub(1);
            for (seq, body) in self.unacked.iter().take(16) {
                self.outbox.push(Cudp::packet(*seq, ack, body));
            }
            self.resent_at = now;
            self.ack_due = false;
        }
        if self.ack_due {
            self.outbox.push(Cudp::packet(self.send_seq, self.recv_seq.wrapping_sub(1), &[]));
            self.ack_due = false;
        }
    }
}

/// Where a joiner is in GameManager's join.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Stage {
    /// tunnel and link coming up
    Linking,
    /// its hello was answered with the host hello and the roster
    Rostered,
    /// it acknowledged the roster and everyone was told it joined
    Joined,
    /// it came while a race ran: told so (GameInitInfo refused), kept out of the race
    Refused,
}

/// A joiner, by its address.
struct Peer {
    vip: u32,
    port: u16,
    key: Vec<u8>,
    send: Stream,
    recv: Stream,
    their_client: u32,
    /// the other end said it hears us: the control record can stop
    they_hear: bool,
    cudp: Cudp,
    /// NetGameLink: the other end's clock in its last sync, and our clock then
    their_clock: u32,
    their_clock_at: u64,
    last_sync: u64,
    heard_at: u64,
    who: Option<Expected>,
    stage: Stage,
    /// the player's place in the race (0..), and so its racer ids
    slot: u32,
    /// its car as it last described it (PlayerCarData), for players who join later
    car_data: Option<Vec<u8>>,
    /// it loaded the race and said Ready
    ready: bool,
    /// its GameResults for the race just over
    results: Option<Vec<u8>>,
    /// in the room (not on a race's results): other players' room news can reach it
    in_room: bool,
    /// room news (ActiveGame_Message) held while it is on the results
    held: Vec<Vec<u8>>,
    /// its clock (ClockSyncStart: seconds since its own start) and ours when that came
    clock: Option<(f32, u64)>,
    /// BigMessage pieces being put together, by the sender's sequence number:
    /// (whole length, the bytes so far, how many arrived)
    big: BTreeMap<u32, (usize, Vec<u8>, usize)>,
    /// game messages (non-system) from this player, counted for the logs
    game_msgs: u64,
}

impl Peer {
    fn new(vip: u32, port: u16, key: &[u8], now: u64) -> Peer {
        Peer {
            vip,
            port,
            key: key.to_vec(),
            send: Stream::new(key),
            recv: Stream::new(key),
            their_client: 0,
            they_hear: false,
            cudp: Cudp::default(),
            their_clock: 0,
            their_clock_at: 0,
            last_sync: 0,
            heard_at: now,
            who: None,
            stage: Stage::Linking,
            slot: 0,
            car_data: None,
            ready: false,
            results: None,
            in_room: true,
            held: Vec::new(),
            clock: None,
            big: BTreeMap::new(),
            game_msgs: 0,
        }
    }

    fn ip(&self) -> String {
        Ipv4Addr::from(self.vip).to_string()
    }

    /// The player as the stats store knows it in a race report.
    fn racer(&self) -> Option<Racer> {
        self.who.as_ref().map(|w| Racer { slot: self.slot, name: w.name.clone(), car: crate::stats::vehicle_key(self.car_data.as_deref()) })
    }

    fn label(&self) -> String {
        match &self.who {
            Some(w) => format!("{} ({}, pid {})", w.name, self.ip(), w.pid),
            None => self.ip(),
        }
    }

    /// A NetGameLink packet with our sync, to go out reliably or not.
    fn link_send(&mut self, kind: u8, payload: &[u8], reliable: bool, now: u64) {
        let mut b = Vec::with_capacity(payload.len() + 13);
        b.extend_from_slice(payload);
        let echo = self.their_clock.wrapping_add(now.saturating_sub(self.their_clock_at) as u32);
        b.extend_from_slice(&echo.to_be_bytes());
        b.extend_from_slice(&(now as u32).to_be_bytes());
        b.extend_from_slice(&50u16.to_be_bytes());
        b.extend_from_slice(&[0, 0]);
        b.push(kind | NGL_SYNC);
        if reliable {
            self.cudp.send_reliable(b, now);
        } else {
            self.cudp.send_unreliable(&b);
        }
        self.last_sync = now;
    }

    /// A race message (GameManager game data), reliably or not.
    fn game_send(&mut self, msg: &[u8], reliable: bool, now: u64) {
        let mut b = Vec::with_capacity(msg.len() + 1);
        b.push(0);
        b.extend_from_slice(msg);
        self.link_send(if reliable { NGL_USER } else { NGL_USER_UNRELIABLE }, &b, reliable, now);
    }

    /// A GameManager system message, reliably.
    fn gm_send(&mut self, msg: &[u8], now: u64) {
        let mut b = Vec::with_capacity(msg.len() + 1);
        b.push(1);
        b.extend_from_slice(msg);
        self.link_send(NGL_USER, &b, true, now);
    }

    /// The datagrams to send now: the control record while the other end has not said it
    /// hears us, and the CommUDP packets queued, as many per datagram as fit.
    fn flush(&mut self, my_client: u32) -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        let mut queue: VecDeque<Vec<u8>> = self.cudp.outbox.drain(..).collect();
        while !queue.is_empty() {
            let mut parts: Vec<Part> = Vec::new();
            let mut size = 2;
            if !self.they_hear {
                parts.push(tunnel::control(my_client, true));
                size += 2 + 6;
            }
            while let Some(p) = queue.front() {
                if parts.len() >= 8 || size + 2 + p.len() > tunnel::MAX_DATAGRAM {
                    break;
                }
                size += 2 + p.len();
                parts.push((GM_PORT, queue.pop_front().expect("front")));
            }
            if parts.iter().all(|p| p.0 == tunnel::CONTROL) {
                // A packet too big for a datagram: it cannot go (CommUDP keeps them small).
                queue.pop_front();
                continue;
            }
            out.push(tunnel::seal(&mut self.send, &parts));
        }
        out
    }
}

/// Where the game's race is.
#[derive(Clone, Copy, Debug, PartialEq)]
enum Race {
    /// players in the game room
    Lobby,
    /// StartLoading went out: waiting for every player's Ready
    Loading,
    /// everyone is ready: clocks are being synchronized (ClockSyncRequest sent at the time)
    Syncing(u64),
    /// StartRaceSyncBegin went out
    Racing,
}

/// A race message to a player: whole, or in BigMessage pieces when longer than the client
/// sends whole (0x1e4 bytes, 0082a835), numbered from `seq`.
fn race_send(p: &mut Peer, msg: &[u8], reliable: bool, now: u64, seq: &mut u32) {
    if msg.len() <= olmsg::BIG_PIECE {
        p.game_send(msg, reliable, now);
        return;
    }
    *seq = seq.wrapping_add(1) & 0x7fff;
    for piece in olmsg::big_pieces(*seq, msg) {
        p.game_send(&piece, true, now);
    }
}

/// Something the host did or saw, for stdout and the trace.
pub struct Note(pub String);

/// One game the server hosts.
pub struct Host {
    pub gid: u32,
    /// our UDP port on 10.77.0.1
    pub port: u16,
    pub max_players: u16,
    key: Vec<u8>,
    my_client: u32,
    peers: BTreeMap<(u32, u16), Peer>,
    expected: Vec<Expected>,
    started: Instant,
    /// the race settings a player last sent (GameAttributes), for players who join later
    attributes: Option<Vec<u8>>,
    race: Race,
    /// the host's own BigMessage numbers (a client puts pieces together by number alone,
    /// 00832710, so pieces passed on from two players must not share one)
    big_seq: u32,
    /// the leader's requests (Matchmaking/Invites/Joins On/Off): new players may join
    pub joinable: bool,
    /// the races the game offers per kind (race_type_circuit ...), from its PlayNow
    pub races: BTreeMap<String, Vec<String>>,
    /// the race's end as the games report it: (player id, status), first come first
    finished: Vec<(u32, u32)>,
    /// when the first GameResults came, until FinalGameResults went out
    results_since: Option<u64>,
    /// Pursuit Tag: the racer id of the player who is the racer (everyone else is a cop)
    fugitive: Option<i16>,
    pub notes: Vec<Note>,
    /// what the stats store hears of each race (stats.rs counts it): who started it, then
    /// every game's GameResults when FinalGameResults went out
    pub reports: Vec<RaceReport>,
    /// decrypted traffic, for the trace file
    pub log: String,
}

/// A datagram the host sends: (to vip, to port, data); it comes from 10.77.0.1:port.
pub type Out = (u32, u16, Vec<u8>);

impl Host {
    /// The game `gid` at 10.77.0.1:`port`, keyed by its EKEY; `user_id` is the host
    /// player's (HUID), which the joiner checks in the tunnel's first datagram.
    pub fn new(gid: u32, port: u16, user_id: u64, ekey: &str) -> Host {
        Host {
            gid,
            port,
            max_players: 8,
            key: tunnel::key_from_ekey(ekey),
            my_client: tunnel::client_id(user_id),
            peers: BTreeMap::new(),
            expected: Vec::new(),
            started: Instant::now(),
            // A new room starts unranked. The first player's game takes what it is sent here
            // into its room settings (0x827e30) before it sends its own; without it, its
            // settings say game_type 0 (ranked: "Ranked Match", no game invites) whatever the
            // player chose on GAME TYPE, until it backs out and picks again. It takes the whole
            // list (an attribute left out reads as 0: NOS and collision Off), so these are the
            // game's own defaults for a new room, as its first GameAttributes sent them, with
            // game_type 1. Its own full GameAttributes replace this one.
            attributes: Some(olmsg::game_attributes(&olmsg::new_room_attributes())),
            race: Race::Lobby,
            big_seq: 0,
            joinable: true,
            races: BTreeMap::new(),
            finished: Vec::new(),
            results_since: None,
            fugitive: None,
            notes: Vec::new(),
            reports: Vec::new(),
            log: String::new(),
        }
    }

    fn now(&self) -> u64 {
        self.started.elapsed().as_millis() as u64
    }

    fn note(&mut self, s: String) {
        self.notes.push(Note(s));
    }

    /// Players in the game or joining it, for Theater's lists.
    pub fn players(&self) -> usize {
        self.peers.values().filter(|p| matches!(p.stage, Stage::Rostered | Stage::Joined)).count()
    }

    /// A player in the room, picked by its name and user id (an EA Messenger invite looks
    /// its sender's room up so).
    pub fn has_player(&self, is: &dyn Fn(&str, u64) -> bool) -> bool {
        self.peers.values().any(|p| matches!(p.stage, Stage::Rostered | Stage::Joined) && p.who.as_ref().is_some_and(|w| is(&w.name, w.user_id)))
    }

    /// The players in the room: (GameManager player id, name, user id), for Theater's PDAT.
    pub fn roster(&self) -> Vec<(u32, String, u64)> {
        self.peers
            .values()
            .filter(|p| matches!(p.stage, Stage::Rostered | Stage::Joined))
            .filter_map(|p| p.who.as_ref().map(|w| (u32::from(w.pid), w.name.clone(), w.user_id)))
            .collect()
    }

    /// The room's settings by name, as its leader last sent them (GameAttributes); empty
    /// ones left out. Theater gives them as B-U-* attributes to an invite's lookup, which
    /// the invite screen reads (Game Mode, Game Type, Collision, Track, Laps).
    pub fn settings(&self) -> Vec<(&'static str, String)> {
        let list = self.attributes.as_deref().and_then(olmsg::read_attributes).unwrap_or_default();
        list.into_iter().filter(|(_, v)| !v.is_empty()).filter_map(|(i, v)| Some((*olmsg::ATTRIBUTES.get(i as usize)?, v))).collect()
    }

    /// A race is loading or running: a new player cannot come in until it is over.
    pub fn in_race(&self) -> bool {
        self.race != Race::Lobby
    }

    /// Theater let a player in (EGAM): it will come to our port with this ticket.
    pub fn expect(&mut self, e: Expected) {
        self.expected.retain(|x| x.user_id != e.user_id);
        self.expected.push(e);
        if self.expected.len() > 64 {
            self.expected.remove(0);
        }
    }

    /// A datagram from a joiner's game port.
    pub fn udp(&mut self, vip: u32, port: u16, data: &[u8]) -> Vec<Out> {
        let now = self.now();
        if !self.peers.contains_key(&(vip, port)) && self.peers.len() >= MAX_PEERS {
            return Vec::new();
        }
        let key = self.key.clone();
        let fresh = || Peer::new(vip, port, &key, now);
        let known = self.peers.contains_key(&(vip, port));
        let mut peer = self.peers.remove(&(vip, port)).unwrap_or_else(fresh);
        let parts = match if known { tunnel::open(&mut peer.recv, data) } else { Err(Refused::Malformed) } {
            Ok(p) => Some(p),
            Err(_) => self.rekey(&mut peer, data, known),
        };
        let Some(parts) = parts else {
            if known {
                self.peers.insert((vip, port), peer);
            }
            return Vec::new();
        };
        peer.heard_at = now;
        let mut control_seen = false;
        for (idx, d) in &parts {
            if *idx == tunnel::CONTROL {
                control_seen = true;
                if let Some((c, heard)) = tunnel::read_control(d) {
                    peer.their_client = c;
                    if heard {
                        peer.they_hear = true;
                    }
                }
            }
        }
        if !control_seen {
            peer.they_hear = true;
        }
        for (idx, d) in parts {
            if idx == GM_PORT {
                self.commudp(&mut peer, &d, now);
            } else if idx != tunnel::CONTROL {
                let _ = writeln!(self.log, "+{:.3} {} tunnel port {idx}: {} bytes (not CommUDP's)", now as f64 / 1000.0, peer.label(), d.len());
            }
        }
        let out: Vec<Out> = peer.flush(self.my_client).into_iter().map(|d| (vip, port, d)).collect();
        self.peers.insert((vip, port), peer);
        // Messages for the others (a join announcement, game messages passed on).
        let mut rest = self.flush_all(Some((vip, port)));
        let mut all = out;
        all.append(&mut rest);
        all
    }

    /// A datagram the current streams cannot open: a joiner's new tunnel (its first, or its
    /// second try with the inverted key), or its own restart. Taken when it opens from the
    /// start of a stream with either key and carries a control record.
    fn rekey(&mut self, peer: &mut Peer, data: &[u8], known: bool) -> Option<Vec<Part>> {
        for key in [self.key.clone(), tunnel::inverted(&self.key)] {
            let mut s = Stream::new(&key);
            let Ok(parts) = tunnel::open(&mut s, data) else { continue };
            let Some((_, ctl)) = parts.iter().find(|p| p.0 == tunnel::CONTROL) else { continue };
            let Some((client, heard)) = tunnel::read_control(ctl) else { continue };
            if known && heard && key == peer.key {
                // Our own stream, just out of step: not a new tunnel.
                continue;
            }
            let which = if key == self.key { "the game's key" } else { "the inverted key (its second try)" };
            self.note(format!("{}:{}: tunnel from client {client:08x} with {which}", Ipv4Addr::from(peer.vip), peer.port));
            peer.key = key.clone();
            peer.recv = s;
            peer.send = Stream::new(&key);
            peer.their_client = client;
            peer.they_hear = false;
            peer.cudp = Cudp::default();
            peer.stage = Stage::Linking;
            return Some(parts);
        }
        None
    }

    fn commudp(&mut self, peer: &mut Peer, p: &[u8], now: u64) {
        let (packets, what) = peer.cudp.receive(p);
        if let Some(what) = what {
            let line = format!("{}: {what}", peer.label());
            let _ = writeln!(self.log, "+{:.3} {line}", now as f64 / 1000.0);
            self.note(line);
            if !peer.cudp.open {
                peer.stage = Stage::Linking;
            }
        }
        for pkt in packets {
            self.link(peer, &pkt, now);
        }
    }

    /// One NetGameLink packet from a joiner.
    fn link(&mut self, peer: &mut Peer, pkt: &[u8], now: u64) {
        let Some((&kind, mut body)) = pkt.split_last() else { return };
        if kind & NGL_SYNC != 0 && body.len() >= 12 {
            let s = &body[body.len() - 12..];
            peer.their_clock = u32::from_be_bytes([s[4], s[5], s[6], s[7]]);
            peer.their_clock_at = now;
            body = &body[..body.len() - 12];
        }
        match kind & !NGL_SYNC {
            0 => {}
            NGL_USER | NGL_USER_UNRELIABLE => {
                let Some((&system, msg)) = body.split_first() else { return };
                if system == 1 {
                    self.gm(peer, msg, now);
                } else if peer.stage == Stage::Joined {
                    self.game(peer, msg, now);
                }
            }
            2 => self.note(format!("{}: NetGameLink disconnect", peer.label())),
            k => {
                let _ = writeln!(self.log, "+{:.3} {} NetGameLink kind {k}: {} bytes", now as f64 / 1000.0, peer.label(), body.len());
            }
        }
    }

    /// A race message (OLMSG) from a player. The host is EA's "rebroadcaster": most
    /// messages go on to the other players as they came; a few drive the race itself.
    fn game(&mut self, peer: &mut Peer, msg: &[u8], now: u64) {
        if olmsg::kind(msg) == Some(olmsg::BIG_MESSAGE) {
            let Some(piece) = olmsg::BigPiece::read(msg) else { return };
            if piece.total > 0x10000 || piece.data.len() > piece.total {
                return;
            }
            let e = peer.big.entry(piece.seq).or_insert_with(|| (piece.total, vec![0; piece.total], 0));
            let end = (piece.offset + piece.data.len()).min(e.1.len());
            if piece.offset < end {
                e.1[piece.offset..end].copy_from_slice(&piece.data[..end - piece.offset]);
            }
            e.2 += piece.data.len();
            if e.2 >= e.0 {
                let (_, whole, _) = peer.big.remove(&piece.seq).expect("there");
                self.game(peer, &whole, now);
            }
            while peer.big.len() > 8 {
                let first = *peer.big.keys().next().expect("non-empty");
                peer.big.remove(&first);
            }
            return;
        }
        peer.game_msgs += 1;
        let kind = olmsg::kind(msg).unwrap_or(u32::MAX);
        if kind != olmsg::CAR_STATE || peer.game_msgs % 64 == 0 {
            let _ = write!(self.log, "+{:.3} {} {} ({} bytes):", now as f64 / 1000.0, peer.label(), olmsg::name(kind), msg.len());
            let shown = if kind == olmsg::CAR_STATE { 160 } else { 4096 };
            for b in msg.iter().take(shown) {
                let _ = write!(self.log, " {b:02x}");
            }
            self.log.push('\n');
        }
        if kind != olmsg::CAR_STATE && (peer.game_msgs <= 40 || peer.game_msgs.is_power_of_two()) {
            self.note(format!("{}: {} ({} bytes)", peer.label(), olmsg::name(kind), msg.len()));
        }
        // A player's game says something only the room says: it is back from the results
        // (what was held for it follows its own message, as when it comes back first).
        let back = !peer.in_room
            && (matches!(kind, olmsg::GAME_ATTRIBUTES | olmsg::ACTIVE_GAME_MESSAGE | olmsg::START_TIMER | olmsg::PLAYER_CAR_DATA)
                || (olmsg::MATCHMAKING_ON..=olmsg::DISABLE_JOINS).contains(&kind));
        self.handle_game(peer, msg, kind, now);
        if back {
            self.back_in_room(peer, now);
        }
    }

    fn handle_game(&mut self, peer: &mut Peer, msg: &[u8], kind: u32, now: u64) {
        // self.peers holds everyone but `peer` while its datagram is handled.
        match kind {
            // A player's car. The host gives it the player's place (join order) at [0xc84],
            // 32 bits at byte 12, and sends it to everyone, the player too: a client takes
            // that number as the player's rank in the room (00831380 -> 00828960) and the
            // lowest as the room's leader (00828e10). Its own entry starts at 9999, so without
            // the echo every client sees another player as the leader.
            olmsg::PLAYER_CAR_DATA => {
                let mut m = msg.to_vec();
                if m.len() >= 16 {
                    m[12..16].copy_from_slice(&peer.slot.to_be_bytes());
                }
                peer.car_data = Some(m.clone());
                self.race_to(peer, &m, true, now);
                self.to_others(&m, true, now);
            }
            // The leader's settings. Its game_type comes from a default (0, ranked) rather
            // than the Game Type the player chose; this server keeps no rankings, so the host
            // makes it 1 (unranked) and sends the settings back to everyone, the leader too:
            // a ranked room's Start first fetches the players' skill (0081f110) and fails.
            olmsg::GAME_ATTRIBUTES => {
                let m = match olmsg::read_attributes(msg) {
                    Some(mut list) => {
                        for (i, v) in list.iter_mut() {
                            if *i == 2 && v != "1" {
                                *v = "1".into();
                            }
                        }
                        olmsg::game_attributes(&list)
                    }
                    None => msg.to_vec(),
                };
                self.attributes = Some(m.clone());
                self.race_to(peer, &m, true, now);
                self.to_others(&m, true, now);
            }
            // A player's game asks to load the race: each game sends StartLoading when the
            // room's timer (Start_Timer) runs out, then waits for the host's. A game loads the
            // race every time it is told to (00831b60), and told twice it crashes (wasm "memory
            // access out of bounds", runs r9 and r10, where the host passed on every player's),
            // so the host answers the first one by telling every player once, the sender too.
            olmsg::START_LOADING => {
                if self.race == Race::Loading {
                    return;
                }
                self.race = Race::Loading;
                self.finished.clear();
                self.results_since = None;
                self.fugitive = None;
                peer.ready = false;
                peer.results = None;
                for p in self.peers.values_mut() {
                    p.ready = false;
                    p.results = None;
                }
                // The race itself: the leader's settings name a game_mode and, per kind, a
                // race or ABSTAIN ("Any"). The leader's game picks one for "Any" itself
                // (it sent "ex.4.2" in run r9); should a race still be open, the host
                // picks one, as EA's did, and tells everyone (the client loads the race
                // its race_type_* attribute names, 008296f0).
                if let Some(list) = self.attributes.as_deref().and_then(olmsg::read_attributes) {
                    let players = 1 + self.peers.values().filter(|p| p.stage == Stage::Joined).count();
                    let seed = now as usize / 7 + self.gid as usize;
                    let (list, picked) = olmsg::settle_race(list, &self.races, players, seed);
                    if !picked.is_empty() {
                        self.note(format!("the race: {picked}"));
                        let attrs = olmsg::game_attributes(&list);
                        self.attributes = Some(attrs.clone());
                        self.race_to(peer, &attrs, true, now);
                        self.to_others(&attrs, true, now);
                    }
                }
                self.race_to(peer, msg, true, now);
                self.to_others(msg, true, now);
                self.note(format!("{}: asked to load the race: everyone told to", peer.label()));
            }
            olmsg::READY => {
                peer.ready = true;
                self.note(format!("{}: ready", peer.label()));
                peer.game_send(&olmsg::clock_sync_request(), true, now);
                self.all_ready(Some(peer), now);
            }
            olmsg::CLOCK_SYNC_START => {
                let mut r = olmsg::Read::new(msg);
                let (_, t) = (r.u32(), r.f32());
                if let Some(t) = t {
                    peer.clock = Some((t, now));
                    let end = olmsg::clock_sync_end(t, now as f32 / 1000.0);
                    peer.game_send(&end, true, now);
                }
            }
            olmsg::CAR_STATE => self.to_others(msg, false, now),
            olmsg::PURSUIT_TAG_SYNC if self.game_mode() == Some(olmsg::MODE_PURSUIT_TAG) => self.pursuit_tag(peer, msg, now),
            olmsg::PURSUIT_TAG_SYNC if self.game_mode() == Some(olmsg::MODE_CANYON) => self.canyon(peer, msg, now),
            // A racer's race ended. Each game reports the racers it sees end (00831c80 and
            // 00831d50, from 008354b0): LeaderFinished for the first, RacerFinished after,
            // as [status u32][the racer's player id u32]; status 1 = finished, 11 = did not
            // finish, 12 = out (busted, knocked out). Passed on as they came; the host keeps
            // the order for its log, the results "reported simply".
            olmsg::LEADER_FINISHED | olmsg::RACER_FINISHED => {
                let mut r = olmsg::Read::new(msg);
                let (_, status, player) = (r.u32(), r.u32().unwrap_or(0), r.u32().unwrap_or(0));
                if !self.finished.iter().any(|(p, _)| *p == player) {
                    self.finished.push((player, status));
                    let what = match status {
                        1 => "finished",
                        11 => "did not finish",
                        12 => "is out",
                        _ => "ended",
                    };
                    let who = self.racer_name(peer, player);
                    self.note(format!("result: {}. {who} {what} (status {status}, as {} saw it)", self.finished.len(), peer.label()));
                }
                self.to_others(msg, true, now);
            }
            // Each game's own view of the race once it is over (007d9400 -> 007d4320). Games
            // ignore GameResults (their handler is a bare `ret`, 0081da20): the host answers
            // with FinalGameResults once everyone's are in (or after a while, tick()).
            olmsg::GAME_RESULTS => {
                self.note(format!("{}: sent its GameResults ({} bytes)", peer.label(), msg.len()));
                peer.results = Some(msg.to_vec());
                if self.race != Race::Lobby {
                    self.results_since.get_or_insert(now);
                }
                if self.results_since.is_some() && self.peers.values().filter(|p| p.stage == Stage::Joined).all(|p| p.results.is_some()) {
                    self.final_results(Some(peer), now);
                }
            }
            // A room timer the leader starts (its FE, 005b5620): everyone shows it, the leader
            // too, counted from the host time in it.
            olmsg::START_TIMER => {
                // a race still marked loading when a new timer starts in the room was given
                // up (a player left while loading): the next StartLoading starts a new one
                if self.race == Race::Loading {
                    self.race = Race::Lobby;
                }
                self.race_to(peer, msg, true, now);
                self.to_others(msg, true, now);
            }
            // A player's news for the room (its state, kind 13 again and again while it waits
            // in the room after a race). A game still on the results that gets one does not
            // go back to the room: the leader's stayed on a car showroom with no menu in runs
            // r15 and r18, where the other player's news came first, and r20 made it happen
            // by having the other player leave the results early. So it waits for the player
            // to be back (the newest of each, in order).
            olmsg::ACTIVE_GAME_MESSAGE => {
                let mut seq = self.big_seq;
                for p in self.peers.values_mut().filter(|p| p.stage == Stage::Joined) {
                    if p.in_room {
                        race_send(p, msg, true, now, &mut seq);
                    } else {
                        p.held.retain(|m| m.as_slice() != msg);
                        p.held.push(msg.to_vec());
                        if p.held.len() > 16 {
                            p.held.remove(0);
                        }
                    }
                }
                self.big_seq = seq;
            }
            olmsg::MATCHMAKING_ON..=olmsg::DISABLE_JOINS => {
                let open = matches!(kind, olmsg::MATCHMAKING_ON | olmsg::ENABLE_JOINS);
                if matches!(kind, olmsg::ENABLE_JOINS | olmsg::DISABLE_JOINS | olmsg::MATCHMAKING_ON | olmsg::MATCHMAKING_OFF) && self.joinable != open {
                    self.joinable = open;
                    self.note(format!("{}: the game is {} to new players", peer.label(), if open { "open" } else { "closed" }));
                }
            }
            _ => self.to_others(msg, true, now),
        }
    }

    /// When every player in the game has loaded (Ready), the clocks are checked once more
    /// (ClockSyncRequest went to each as it said Ready) and the start follows in tick().
    /// `current` is the player being handled, outside self.peers.
    fn all_ready(&mut self, current: Option<&Peer>, now: u64) {
        if self.race != Race::Loading {
            return;
        }
        let others = self.peers.values().filter(|p| p.stage == Stage::Joined);
        let mut n = 0;
        for p in others.chain(current.into_iter()) {
            if !p.ready {
                return;
            }
            n += 1;
        }
        if n > 0 {
            self.race = Race::Syncing(now);
            self.note(format!("all {n} players loaded: starting"));
        }
    }

    /// A player's game is back in the room after a race: what was held for it goes now.
    fn back_in_room(&mut self, peer: &mut Peer, now: u64) {
        if peer.in_room {
            return;
        }
        peer.in_room = true;
        let held = std::mem::take(&mut peer.held);
        if !held.is_empty() {
            self.note(format!("{}: back in the room; {} held message(s) sent", peer.label(), held.len()));
        }
        for m in held {
            self.race_to(peer, &m, true, now);
        }
    }

    /// game_mode from the room's settings (the leader's GameAttributes).
    fn game_mode(&self) -> Option<u32> {
        let list = olmsg::read_attributes(self.attributes.as_deref()?)?;
        list.iter().find(|(i, _)| *i == 4).and_then(|(_, v)| v.parse().ok())
    }

    /// Pursuit Tag: one player is the racer, the others cops. A role change swaps both cars in
    /// every game, as a toggle, and the game that saw it happen swaps its own only when the
    /// message comes back: so the host sends each change to everyone, the sender too, and
    /// only once. A change from a player who is no longer the racer (it busted or evaded
    /// again before the first one came back, or two games saw the same bust) is dropped:
    /// passed on, it would swap the cars once more in some games and not in others, and
    /// each game would count the time as the racer for the wrong player.
    fn pursuit_tag(&mut self, peer: &mut Peer, msg: &[u8], now: u64) {
        let Some((a, b)) = olmsg::tag_sync(msg) else { return };
        if a == olmsg::TAG_TIMER || b == olmsg::TAG_TIMER {
            // Only the racer's own game sends these; the first one tells the host who
            // started as the racer (every game picks the same one).
            if self.fugitive.is_none() && a >= 0 {
                self.fugitive = Some(a);
                let who = self.racer_name(peer, a as u32);
                self.note(format!("Pursuit Tag: {who} starts as the racer"));
            }
            self.to_others(msg, true, now);
            return;
        }
        // Who hands the role over: the busted racer, the sender for an evasion (only the
        // racer's game evades), or a player who left (any game may say so, once).
        let base = 1 + RACERS_PER_PLAYER * peer.slot;
        let owns = |p: &Peer, r: i16| (1 + RACERS_PER_PLAYER * p.slot..1 + RACERS_PER_PLAYER * (p.slot + 1)).contains(&(r as u32));
        let (from, what) = match a {
            olmsg::TAG_EVADED => match self.fugitive {
                Some(r) if owns(peer, r) => (Some(r), "evaded for a minute"),
                Some(_) => (None, "evaded for a minute"),
                None => (Some(base as i16), "evaded for a minute"),
            },
            olmsg::TAG_LEFT => match self.fugitive {
                Some(r) if !owns(peer, r) && !self.peers.values().any(|p| p.stage == Stage::Joined && owns(p, r)) => (Some(r), "left"),
                _ => (None, "left"),
            },
            _ if a >= 0 => (Some(a), "was busted"),
            _ => {
                self.to_others(msg, true, now);
                return;
            }
        };
        let current = self.fugitive.or(from);
        if from.is_none() || from != current || b < 0 || Some(b) == current {
            let racer = current.map_or_else(|| "nobody known".into(), |r| self.racer_name(peer, r as u32));
            self.note(format!("Pursuit Tag: {}'s change ({a}, {b}) dropped: the racer is {racer}", peer.label()));
            return;
        }
        self.fugitive = Some(b);
        let (old, new) = (self.racer_name(peer, current.unwrap_or(0) as u32), self.racer_name(peer, b as u32));
        self.note(format!("Pursuit Tag: {old} {what}; {new} is the racer"));
        self.race_to(peer, msg, true, now);
        self.to_others(msg, true, now);
    }

    /// Canyon duel: two heats, the leader and the chaser swapping. A game's racer ends a
    /// heat only when its own [racer][-4] comes back: the sender (00616dc7) just notes that
    /// it sent it; the receiving side (00616e10) clears the racer's "in the heat" flag, and the
    /// next heat waits until both racers' flags are clear (00616d90). Passed only to the other
    /// game, each waited on its "Canyon Duel Rank Points ... LOADING" screen for good (run
    /// cy1). So -4 goes to everyone, the sender too; the duel's other messages (-6 the gap
    /// score, -5) go to the other game only. (A host StartLoading, even 6, which 00831b60
    /// loads in mode 3, hung both games there, runs cy2 and cy3; a StartRaceSyncBegin did
    /// nothing, run cy4.)
    fn canyon(&mut self, peer: &mut Peer, msg: &[u8], now: u64) {
        if olmsg::tag_sync(msg).is_some_and(|(_, b)| b == olmsg::HEAT_OVER) {
            self.note(format!("Canyon: {} ended its heat", peer.label()));
            self.race_to(peer, msg, true, now);
        }
        self.to_others(msg, true, now);
    }

    /// FinalGameResults to everyone, once per race. EA's host made them from the players'
    /// reports; this one takes the leader's GameResults (the lowest place that sent one)
    /// as they are: every game's report has a row for every racer.
    fn final_results(&mut self, mut current: Option<&mut Peer>, now: u64) {
        let Some(since) = self.results_since.take() else { return };
        let mut from: Vec<(u32, String, Vec<u8>)> = self.peers.values().filter_map(|p| p.results.clone().map(|r| (p.slot, p.label(), r))).collect();
        if let Some(p) = current.as_deref() {
            if let Some(r) = p.results.clone() {
                from.push((p.slot, p.label(), r));
            }
        }
        from.sort_by_key(|f| f.0);
        let Some((_, who, results)) = from.into_iter().next() else { return };
        let Some(m) = olmsg::final_game_results(&results) else {
            self.note(format!("{who}'s GameResults are {} bytes, not {}: no FinalGameResults", results.len(), olmsg::GAME_RESULTS_LEN));
            return;
        };
        if let Some(p) = current.as_deref_mut() {
            self.race_to(p, &m, true, now);
        }
        self.to_others(&m, true, now);
        self.race = Race::Lobby;
        // everyone is on the results screen now, until its game says otherwise
        if let Some(p) = current.as_deref_mut() {
            p.in_room = false;
        }
        for p in self.peers.values_mut() {
            p.in_room = false;
        }
        self.note(format!("FinalGameResults to everyone ({who}'s GameResults, {} s after the first)", now.saturating_sub(since) / 1000));
        let joined = self.peers.values().chain(current.as_deref()).filter(|p| p.stage == Stage::Joined);
        let players = joined.clone().filter_map(Peer::racer).collect();
        let results = joined.filter_map(|p| Some((p.slot, p.results.clone()?))).collect();
        self.reports.push(RaceReport::Ended { gid: self.gid, settings: self.settings(), players, results });
    }

    /// A player id from a race message as a name: a joined player's GameManager pid, or
    /// a racer id within one player's range (GameInitInfo).
    fn racer_name(&self, current: &Peer, id: u32) -> String {
        let all = self.peers.values().filter(|p| p.stage == Stage::Joined).chain(std::iter::once(current));
        for p in all {
            let base = 1 + RACERS_PER_PLAYER * p.slot;
            let pid = p.who.as_ref().map(|w| u32::from(w.pid));
            if pid == Some(id) || (base..base + RACERS_PER_PLAYER).contains(&id) {
                return p.who.as_ref().map_or_else(|| p.ip(), |w| w.name.clone());
            }
        }
        format!("player {id}")
    }

    /// A race message to every player in the game but the one being handled.
    fn to_others(&mut self, msg: &[u8], reliable: bool, now: u64) {
        let mut seq = self.big_seq;
        for p in self.peers.values_mut().filter(|p| p.stage == Stage::Joined) {
            race_send(p, msg, reliable, now, &mut seq);
        }
        self.big_seq = seq;
    }

    /// A race message to one player, cut into BigMessage pieces when it is long.
    fn race_to(&mut self, p: &mut Peer, msg: &[u8], reliable: bool, now: u64) {
        let mut seq = self.big_seq;
        race_send(p, msg, reliable, now, &mut seq);
        self.big_seq = seq;
    }

    /// Once every player's clock is known (or after a while), the start: each player gets
    /// the moment the race clock began, in its own clock (StartRaceSyncBegin 00825b70; the
    /// client takes its own time minus it as the time since the start, 0081d9b0).
    fn start_race(&mut self, now: u64) {
        // seconds from this message to the green light, the same for everyone (the client
        // counts it from when the message arrives, 007aa5a0)
        let countdown = 3.0f32;
        for p in self.peers.values_mut().filter(|p| p.stage == Stage::Joined) {
            // the player's clock now: its time when it asked, plus what has passed since
            let theirs_ms = p.clock.map_or(0, |(t, at)| (t * 1000.0) as u64 + now.saturating_sub(at));
            let m = olmsg::start_race_sync_begin(theirs_ms as u32, countdown, 0.0);
            p.game_send(&m, true, now);
        }
        self.race = Race::Racing;
        self.note("race started (StartRaceSyncBegin)".into());
        let players = self.peers.values().filter(|p| p.stage == Stage::Joined).filter_map(Peer::racer).collect();
        self.reports.push(RaceReport::Started { gid: self.gid, players });
    }

    /// A GameManager system message from a joiner.
    fn gm(&mut self, peer: &mut Peer, msg: &[u8], now: u64) {
        let mut d = Dec::new(msg);
        let Some(kind) = d.i8() else { return };
        let mut hex = String::new();
        for b in msg.iter().take(64) {
            let _ = write!(hex, " {b:02x}");
        }
        let _ = writeln!(self.log, "+{:.3} {} GameManager message {kind}:{hex}", now as f64 / 1000.0, peer.label());
        match kind {
            GM_HELLO => {
                let (Some(version), Some(ticket)) = (d.i16(), d.str()) else {
                    self.note(format!("{}: a hello we cannot read", peer.label()));
                    return;
                };
                let who = self.expected.iter().find(|e| e.ticket == ticket).or_else(|| self.expected.iter().rev().find(|e| e.vip == peer.vip)).cloned();
                let Some(who) = who else {
                    self.note(format!("{}: hello with ticket {ticket}, which Theater never gave out: ignored", peer.label()));
                    return;
                };
                peer.who = Some(who.clone());
                self.note(format!("{}: hello (GameManager version {version}, ticket {ticket})", peer.label()));
                self.welcome(peer, now);
            }
            GM_ROSTER_ACK => {
                if peer.stage != Stage::Rostered {
                    return;
                }
                let Some(who) = peer.who.clone() else { return };
                // A race is on: the game's own answer is GameInitInfo with "refused" set
                // (00831710 marks the session refused, [mgr+4]), and its room screen then says
                // "You can not join this Game Room because the match has already started."
                // (005cbac0, string 0xbf05727a). The others never hear of it.
                if self.in_race() {
                    peer.stage = Stage::Refused;
                    // its own join announcement only, so its game leaves "Joining game session..."
                    // for the room screen, where the refusal shows (run st1: without it, it stayed)
                    let element = self.element(&who, peer.vip, peer.port, PLAYER_ACTIVE);
                    let mut m = Enc::default();
                    m.i8(GM_JOIN).u16(who.pid);
                    m.0.extend_from_slice(&element);
                    peer.gm_send(&m.0, now);
                    peer.game_send(&olmsg::game_init_info(1, now as f32 / 1000.0, 1, 0), true, now);
                    self.note(format!("{}: came while a race runs; refused (the match has already started)", peer.label()));
                    return;
                }
                peer.stage = Stage::Joined;
                self.note(format!("{}: roster acknowledged; joined, told everyone", peer.label()));
                let element = self.element(&who, peer.vip, peer.port, PLAYER_ACTIVE);
                let mut m = Enc::default();
                m.i8(GM_JOIN).u16(who.pid);
                m.0.extend_from_slice(&element);
                peer.gm_send(&m.0, now);
                for other in self.peers.values_mut().filter(|p| p.stage == Stage::Joined) {
                    other.gm_send(&m.0, now);
                }
                // The race layer starts with GameInitInfo: until it comes the client waits
                // ("Joining game session...", 0082abc0 only looks for it). It gives the
                // player its racer ids.
                let used: Vec<u32> = self.peers.values().filter(|p| p.stage == Stage::Joined).map(|p| p.slot).collect();
                peer.slot = (0..8).find(|s| !used.contains(s)).unwrap_or(0);
                let base = 1 + RACERS_PER_PLAYER * peer.slot;
                let init = olmsg::game_init_info(base, now as f32 / 1000.0, 0, 0);
                peer.game_send(&init, true, now);
                // Its clock against ours: the room's timers (Start_Timer) are in host time.
                peer.game_send(&olmsg::clock_sync_request(), true, now);
                // What the others already said about themselves and the race.
                if let Some(a) = self.attributes.clone() {
                    self.race_to(peer, &a, true, now);
                }
                let cars: Vec<Vec<u8>> = self.peers.values().filter(|p| p.stage == Stage::Joined).filter_map(|p| p.car_data.clone()).collect();
                for c in cars {
                    self.race_to(peer, &c, true, now);
                }
                let _ = writeln!(self.log, "+{:.3} {} < GameInitInfo (racer ids from {base})", now as f64 / 1000.0, peer.label());
                self.note(format!("{}: sent GameInitInfo (slot {}, racer ids from {base})", peer.label(), peer.slot));
            }
            GM_GOODBYE => {
                self.note(format!("{}: goodbye", peer.label()));
                peer.stage = Stage::Linking;
            }
            k => self.note(format!("{}: GameManager message {k} ({} bytes), not handled yet", peer.label(), msg.len())),
        }
    }

    /// A player as the roster describes it (Player serialize 009828c0): id, name, user id,
    /// state, a spare u64, no Xbox address, its address pair (type 0: external, internal),
    /// and a flag.
    fn element(&self, who: &Expected, vip: u32, port: u16, state: i8) -> Vec<u8> {
        let mut e = Enc::default();
        e.u16(who.pid)
            .str(&who.name)
            .i64(who.user_id as i64)
            .i8(state)
            .u64(0)
            .u8(0)
            .i8(0)
            .u32(vip)
            .u16(port)
            .u32(vip)
            .u16(port)
            .u8(0);
        e.0
    }

    /// The answer to a hello: the host hello, then a roster element for each player in the
    /// game and for the joiner itself (00956b40). A dedicated host has no player of its own.
    fn welcome(&mut self, peer: &mut Peer, now: u64) {
        let Some(me) = peer.who.clone() else { return };
        let others: Vec<(Expected, u32, u16)> = self
            .peers
            .values()
            .filter(|p| p.stage == Stage::Joined)
            .filter_map(|p| p.who.clone().map(|w| (w, p.vip, p.port)))
            .collect();
        let mut h = Enc::default();
        h.i8(GM_HOST_HELLO)
            .i16(GM_VERSION)
            .i8(0) // topology: client-server
            .i8(0) // VOIP off
            // game invites allowed ([game+0x2c], 1 in GameManager's own defaults, 009543c0):
            // with 0 the game never offers "Send Game Invite" (its check, 009544e0)
            .i8(1)
            .u16(0)
            .u8(0)
            .u8(0)
            .u8(0)
            .u8(0)
            .i8(0)
            .u8(0)
            .u64(0)
            .u16(self.max_players)
            .u16(others.len() as u16 + 1)
            .u8(0); // no host player
        peer.gm_send(&h.0, now);
        for (w, vip, port) in &others {
            let mut m = Enc::default();
            m.i8(GM_ROSTER);
            m.0.extend_from_slice(&self.element(w, *vip, *port, PLAYER_ACTIVE));
            peer.gm_send(&m.0, now);
        }
        let mut m = Enc::default();
        m.i8(GM_ROSTER);
        m.0.extend_from_slice(&self.element(&me, peer.vip, peer.port, PLAYER_HELLO));
        peer.gm_send(&m.0, now);
        peer.stage = Stage::Rostered;
        self.note(format!("{}: sent the host hello and a roster of {}", peer.label(), others.len() + 1));
    }

    /// Datagrams queued for peers other than `except`.
    fn flush_all(&mut self, except: Option<(u32, u16)>) -> Vec<Out> {
        let mut out = Vec::new();
        for (k, p) in self.peers.iter_mut() {
            if Some(*k) == except || p.cudp.outbox.is_empty() {
                continue;
            }
            for d in p.flush(self.my_client) {
                out.push((p.vip, p.port, d));
            }
        }
        out
    }

    /// Timers: resends, acknowledgements, link syncs; peers silent too long are dropped.
    pub fn tick(&mut self) -> Vec<Out> {
        let now = self.now();
        let gone: Vec<(u32, u16)> = self.peers.iter().filter(|(_, p)| now.saturating_sub(p.heard_at) > PEER_TIMEOUT_MS).map(|(k, _)| *k).collect();
        for k in gone {
            if let Some(p) = self.peers.remove(&k) {
                self.note(format!("{}: silent for {} s; dropped", p.label(), PEER_TIMEOUT_MS / 1000));
            }
        }
        // a race everyone left is over: the room takes new players again
        if self.race != Race::Lobby && !self.peers.values().any(|p| p.stage == Stage::Joined) {
            self.race = Race::Lobby;
        }
        self.all_ready(None, now);
        // results from a player who left (or never ends the race) are not waited for
        if self.results_since.is_some_and(|t| now.saturating_sub(t) > RESULTS_WAIT_MS) {
            self.final_results(None, now);
        }
        if let Race::Syncing(since) = self.race {
            let synced = self.peers.values().filter(|p| p.stage == Stage::Joined).all(|p| p.clock.is_some_and(|(_, at)| at >= since));
            if synced || now.saturating_sub(since) > 3000 {
                self.start_race(now);
            }
        }
        for p in self.peers.values_mut() {
            if p.cudp.open && now.saturating_sub(p.last_sync) >= SYNC_MS {
                p.link_send(0, &[], true, now);
            }
            p.cudp.tick(now);
        }
        self.flush_all(None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EKEY: &str = "AAAAAAAAAAAAAAAAAAAAAA%3d%3d";
    const JOINER: u32 = 0x0a4d_0010;

    /// The joiner's side, as the game's code does it, for the tests.
    struct Game {
        send: Stream,
        recv: Stream,
        seq: u32,
        got: u32,
        /// race messages (game data) from the host
        races: Vec<Vec<u8>>,
    }

    impl Game {
        fn new() -> Game {
            let key = tunnel::key_from_ekey(EKEY);
            Game { send: Stream::new(&key), recv: Stream::new(&key), seq: RELIABLE, got: RELIABLE - 1, races: Vec::new() }
        }
        fn datagram(&mut self, cudp: Vec<u8>) -> Vec<u8> {
            tunnel::seal(&mut self.send, &[tunnel::control(tunnel::client_id(424_415_906), false), (GM_PORT, cudp)])
        }
        fn gm(&mut self, msg: &[u8]) -> Vec<u8> {
            let mut b = vec![1];
            b.extend_from_slice(msg);
            b.push(NGL_USER);
            let p = Cudp::packet(self.seq, self.got, &b);
            self.seq += 1;
            self.datagram(p)
        }
        /// The CommUDP packets in the host's datagrams, and the GameManager messages in them.
        fn read(&mut self, out: &[Out]) -> (Vec<(u32, u32)>, Vec<Vec<u8>>) {
            let (mut heads, mut msgs) = (Vec::new(), Vec::new());
            for (_, _, d) in out {
                for (idx, data) in tunnel::open(&mut self.recv, d).unwrap() {
                    if idx != GM_PORT {
                        continue;
                    }
                    let seq = u32::from_be_bytes(data[0..4].try_into().unwrap());
                    let ack = u32::from_be_bytes(data[4..8].try_into().unwrap());
                    heads.push((seq, ack));
                    let body = &data[8..];
                    if seq >= RELIABLE && !body.is_empty() {
                        self.got = self.got.max(seq);
                        let kind = *body.last().unwrap();
                        assert_eq!(kind & NGL_SYNC, NGL_SYNC);
                        let payload = &body[..body.len() - 13];
                        if kind & !NGL_SYNC == NGL_USER {
                            if payload[0] == 1 {
                                msgs.push(payload[1..].to_vec());
                            } else {
                                self.races.push(payload[1..].to_vec());
                            }
                        }
                    }
                }
            }
            (heads, msgs)
        }
    }

    #[test]
    fn a_joiner_gets_connected_welcomed_and_announced() {
        let mut host = Host::new(100_001, 19_001, 354_668_344, EKEY);
        host.expect(Expected { pid: 2, user_id: 424_415_906, name: "Metin".into(), ticket: "1258340126".into(), vip: JOINER });
        let mut g = Game::new();
        // INIT (the game's own bytes: connection id 219ead5e) is answered with CONN.
        let out = host.udp(JOINER, 1042, &g.datagram(Cudp::packet(INIT, 0x219e_ad5e, &[])));
        assert_eq!(g.read(&out).0, vec![(CONN, 0x219e_ad5e)]);
        // Its hello: version 4 and its ticket.
        let mut hello = Enc::default();
        hello.i8(GM_HELLO).i16(4).str("1258340126");
        let out = host.udp(JOINER, 1042, &g.gm(&hello.0));
        let (_, msgs) = g.read(&out);
        assert_eq!(msgs.len(), 2, "host hello and one roster element");
        let mut d = Dec::new(&msgs[0]);
        assert_eq!((d.i8(), d.i16()), (Some(GM_HOST_HELLO), Some(4)));
        let mut d = Dec::new(&msgs[1]);
        assert_eq!((d.i8(), d.u16(), d.str()), (Some(GM_ROSTER), Some(2), Some("Metin".into())));
        // The roster ack: everyone (here only the joiner) hears it joined.
        let out = host.udp(JOINER, 1042, &g.gm(&[(GM_ROSTER_ACK as u8) ^ 0x80]));
        let (_, msgs) = g.read(&out);
        let mut d = Dec::new(&msgs[0]);
        assert_eq!((d.i8(), d.u16(), d.u16()), (Some(GM_JOIN), Some(2), Some(2)));
        assert_eq!(host.players(), 1);
        // ... and the race layer begins: GameInitInfo with the table's hash and racer ids.
        let mut r = olmsg::Read::new(&g.races[0]);
        assert_eq!((r.u32(), r.u32(), r.u32()), (Some(olmsg::GAME_INIT_INFO), Some(olmsg::VERSION_HASH), Some(1)));
    }

    #[test]
    fn a_joiner_during_a_race_is_refused() {
        let mut host = Host::new(100_001, 19_001, 354_668_344, EKEY);
        host.expect(Expected { pid: 2, user_id: 424_415_906, name: "Metin".into(), ticket: "1258340126".into(), vip: JOINER });
        host.race = Race::Racing;
        assert!(host.in_race());
        let mut g = Game::new();
        let d = g.datagram(Cudp::packet(INIT, 0x219e_ad5e, &[]));
        let out = host.udp(JOINER, 1042, &d);
        g.read(&out);
        let mut hello = Enc::default();
        hello.i8(GM_HELLO).i16(4).str("1258340126");
        let d = g.gm(&hello.0);
        let out = host.udp(JOINER, 1042, &d);
        g.read(&out);
        let d = g.gm(&[(GM_ROSTER_ACK as u8) ^ 0x80]);
        let out = host.udp(JOINER, 1042, &d);
        let (_, msgs) = g.read(&out);
        // Its own join announcement only, not counted, and GameInitInfo says "refused".
        assert_eq!(msgs.len(), 1);
        assert_eq!(Dec::new(&msgs[0]).i8(), Some(GM_JOIN));
        assert_eq!(host.players(), 0);
        let mut r = olmsg::Read::new(&g.races[0]);
        let (kind, hash, _base, _clock, refused) = (r.u32(), r.u32(), r.u32(), r.f32(), r.u32());
        assert_eq!((kind, hash, refused), (Some(olmsg::GAME_INIT_INFO), Some(olmsg::VERSION_HASH), Some(1)));
        assert_eq!(g.races.len(), 1, "nothing else of the room");
    }

    /// A joined player for the race-message tests: its slot gives its racer ids (1 + 4 slot).
    fn joined(vip: u32, slot: u32, name: &str) -> Peer {
        let mut p = Peer::new(vip, 1042, &tunnel::key_from_ekey(EKEY), 0);
        p.stage = Stage::Joined;
        p.slot = slot;
        p.who = Some(Expected { pid: slot as u16 + 2, user_id: 1, name: name.into(), ticket: String::new(), vip });
        p
    }

    /// How many race messages a player was sent since the last call.
    fn sent(p: &mut Peer) -> usize {
        let n = p.cudp.outbox.len();
        p.cudp.outbox.clear();
        n
    }

    fn tag(a: i16, b: i16) -> Vec<u8> {
        let mut m = olmsg::PURSUIT_TAG_SYNC.to_be_bytes().to_vec();
        m.extend_from_slice(&a.to_be_bytes());
        m.extend_from_slice(&b.to_be_bytes());
        m.extend_from_slice(&[0; 8]);
        m
    }

    #[test]
    fn pursuit_tag_changes_go_to_everyone_once() {
        let mut host = Host::new(100_001, 19_001, 354_668_344, EKEY);
        host.attributes = Some(olmsg::game_attributes(&[(4, "14".into())]));
        let (mut metin, jan) = (joined(JOINER, 0, "Metin"), joined(JOINER + 1, 1, "Jan"));
        host.peers.insert((JOINER + 1, 1042), jan);
        let jan_key = (JOINER + 1, 1042);
        // The racer's game says so every 10 frames: passed on, and the host learns who it is.
        host.game(&mut metin, &tag(1, olmsg::TAG_TIMER), 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap()), host.fugitive), (0, 1, Some(1)));
        // Metin busted by Jan: everyone, Metin too, swaps; Jan is the racer.
        host.game(&mut metin, &tag(1, 5), 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap()), host.fugitive), (1, 1, Some(5)));
        // Metin's game saw the bust again before its swap came back: dropped.
        host.game(&mut metin, &tag(1, 5), 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap()), host.fugitive), (0, 0, Some(5)));
        // Only the racer evades: from Metin (a cop now) it is dropped, from Jan it hands over.
        host.game(&mut metin, &tag(olmsg::TAG_EVADED, 1), 0);
        assert_eq!((sent(&mut metin), host.fugitive), (0, Some(5)));
        let mut jan = host.peers.remove(&jan_key).unwrap();
        host.peers.insert((JOINER, 1042), metin);
        host.game(&mut jan, &tag(olmsg::TAG_EVADED, 1), 0);
        assert_eq!((sent(&mut jan), sent(host.peers.get_mut(&(JOINER, 1042)).unwrap()), host.fugitive), (1, 1, Some(1)));
        // In other modes PursuitTagSync is only passed on.
        host.attributes = Some(olmsg::game_attributes(&[(4, "15".into())]));
        host.game(&mut jan, &tag(5, 1), 0);
        assert_eq!((sent(&mut jan), sent(host.peers.get_mut(&(JOINER, 1042)).unwrap())), (0, 1));
    }

    #[test]
    fn a_canyon_duel_heat_end_goes_back_to_its_sender_too() {
        let mut host = Host::new(100_001, 19_001, 354_668_344, EKEY);
        host.attributes = Some(olmsg::game_attributes(&[(4, "13".into())]));
        let (mut metin, jan) = (joined(JOINER, 0, "Metin"), joined(JOINER + 1, 1, "Jan"));
        let jan_key = (JOINER + 1, 1042);
        host.peers.insert(jan_key, jan);
        // The heat's end: to both games.
        host.game(&mut metin, &tag(1, olmsg::HEAT_OVER), 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap())), (1, 1));
        // The gap score: to the other one only.
        host.game(&mut metin, &tag(1, -6), 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap())), (0, 1));
    }

    #[test]
    fn room_news_waits_for_a_player_still_on_the_results() {
        let mut host = Host::new(100_001, 19_001, 354_668_344, EKEY);
        let (mut metin, jan) = (joined(JOINER, 0, "Metin"), joined(JOINER + 1, 1, "Jan"));
        let jan_key = (JOINER + 1, 1042);
        host.peers.insert(jan_key, jan);
        host.race = Race::Racing;
        // Both games' results: FinalGameResults goes to both, who are now on the results.
        let mut results = olmsg::GAME_RESULTS.to_be_bytes().to_vec();
        results.resize(olmsg::GAME_RESULTS_LEN, 0);
        let mut jan = host.peers.remove(&jan_key).unwrap();
        host.peers.insert((JOINER, 1042), metin);
        host.game(&mut jan, &results, 0);
        metin = host.peers.remove(&(JOINER, 1042)).unwrap();
        host.peers.insert(jan_key, jan);
        host.game(&mut metin, &results, 0);
        assert_eq!((sent(&mut metin), sent(host.peers.get_mut(&jan_key).unwrap())), (1, 1));
        // Jan is back in the room first: its news (twice the same) waits for Metin ...
        let news = [0, 0, 0, 0x1c, 0, 3, b'J', b'a', b'n', 0, 0, 0, 13];
        let mut jan = host.peers.remove(&jan_key).unwrap();
        host.peers.insert((JOINER, 1042), metin);
        host.game(&mut jan, &news, 0);
        host.game(&mut jan, &news, 0);
        metin = host.peers.remove(&(JOINER, 1042)).unwrap();
        assert_eq!((sent(&mut metin), metin.held.len()), (0, 1));
        // ... until Metin's game says something only the room says.
        host.peers.insert(jan_key, jan);
        host.game(&mut metin, &olmsg::game_attributes(&[(4, "14".into())]), 0);
        assert!(metin.in_room && metin.held.is_empty());
        assert_eq!(sent(&mut metin), 2, "its settings back, then Jan's news");
    }

    #[test]
    fn lost_packets_are_resent_and_gaps_are_nakked() {
        let mut c = Cudp::default();
        c.reset(7);
        c.send_reliable(b"one".to_vec(), 0);
        c.send_reliable(b"two".to_vec(), 0);
        c.outbox.clear();
        // Nothing acknowledged: both go again after RESEND_MS.
        c.tick(RESEND_MS);
        assert_eq!(c.outbox.len(), 2);
        c.outbox.clear();
        // An ack for the first leaves the second.
        assert!(c.receive(&Cudp::packet(RELIABLE, RELIABLE, &[])).0.is_empty());
        assert_eq!(c.unacked.len(), 1);
        // A packet from ahead of what we expect is answered with a NAK.
        c.receive(&Cudp::packet(RELIABLE + 1, RELIABLE, b"x"));
        assert_eq!(c.outbox.last().unwrap()[..8], Cudp::packet(NAK, RELIABLE, &[])[..]);
        // In order, with an older one bundled at the end: both arrive, oldest first.
        let mut body = b"new".to_vec();
        body.extend_from_slice(b"old");
        body.push(3);
        let got = c.receive(&Cudp::packet((1 << 28) | (RELIABLE + 1), RELIABLE, &body)).0;
        assert_eq!(got, vec![b"old".to_vec(), b"new".to_vec()]);
        assert_eq!(c.recv_seq, RELIABLE + 2);
    }

    #[test]
    fn the_encoder_biases_signed_values() {
        let mut e = Enc::default();
        e.i8(0).i8(-1).i16(4).str("ab").i64(1).u16(6001);
        assert_eq!(e.0, vec![0x80, 0x7f, 0x80, 0x04, 0x80, 0, 0, 2, b'a', b'b', 0x80, 0, 0, 0, 0, 0, 0, 1, 0x17, 0x71]);
        let mut d = Dec::new(&e.0);
        assert_eq!((d.i8(), d.i8(), d.i16(), d.str(), d.u8()), (Some(0), Some(-1), Some(4), Some("ab".into()), Some(0x80)));
    }
}
