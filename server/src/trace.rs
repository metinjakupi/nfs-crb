//! Phase 1's host at 10.77.0.1 (plans/lan-protocol.md section 5): the trace host. It stands
//! where EA's servers will (the kit resolves *.ea.com to 10.77.0.1) and records what Carbon's
//! Online mode sends there, so the stand-ins can be written from real traffic:
//!
//! - TCP: a SYN on any port is accepted (SYNACK); data is recorded and never answered; a FIN
//!   is answered with our own FIN (we have nothing to send) and the connection is done; an
//!   RST ends it without an answer; DATA or FIN for a connection that does not exist (or no
//!   longer does) is answered with RST, as a real host would.
//! - UDP: datagrams are recorded and never answered (no "port unreachable" either).
//! - Each connection's bytes, and each flow's datagrams (client address, client port, our
//!   port), go to a file of their own under CARBON_TRACE_DIR, if set, as a hex dump with the
//!   printable characters beside it; stdout gets one summary line per connection when it ends
//!   and one per datagram.
//!
//! The files are written on a thread of their own, so a slow disk never holds up the switch.
//! A connection or flow records at most MAX_LOGGED bytes (it still counts the rest).
//! The approach follows nfsu2.xyz's virtual host and trace (MIT; see NOTICE).
//!
//! Phase 4: on the ports of EA's services (FESL 18210, Theater 18215, Messenger 13505) the
//! host answers through the stand-ins in ea.rs. Those connections are recorded a packet at a
//! time instead of a segment at a time, both ways, with passwords masked ('*'), and stdout
//! gets one "ea:" line per packet. A stand-in may answer on another connection than the one
//! that spoke (Theater passes a join request to the game's host), so answers name their
//! connection. UDP to Theater's port (the game's ECHO) is answered too.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::fs;
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::mpsc::{sync_channel, SyncSender, TrySendError};
use std::time::{Instant, SystemTime};

use crate::ea::{masked, theater_udp, ConnKey, Event, Hub, Packet, Service, Session, THEATER_PORT};
use crate::lan::{vip_str, FIN, RST, SYN, SYNACK, DATA};
use crate::util::{file_stamp, iso_date};

const MAX_CONNS: usize = 1024;
const PER_VIP: usize = 64;
const MAX_FLOWS: usize = 1024;
const MAX_LOGGED: u64 = 1024 * 1024;
const PREVIEW: usize = 48;

/// (client vip, client port, our port)
type FlowKey = (u32, u16, u16);

struct Stream {
    name: String,
    file: Option<PathBuf>,
    opened: Instant,
    segments: u64,
    bytes: u64,
    preview: Vec<u8>,
    /// the stand-in answering it, on an EA service's port
    svc: Option<Session>,
    /// bytes the stand-in sent
    sent: u64,
    /// the stand-in closed its side (FESL Goodbye)
    fin_sent: bool,
    /// the last data either way (a quiet stand-in connection gets a ping)
    last: Instant,
}

struct Flow {
    file: Option<PathBuf>,
    first: Instant,
    bytes: u64,
}

pub struct Trace {
    dir: Option<PathBuf>,
    tx: Option<SyncSender<(PathBuf, String)>>,
    conns: HashMap<ConnKey, Stream>,
    flows: HashMap<FlowKey, Flow>,
    seq: u64,
    dropped: u64,
    hub: Hub,
    /// each race host's trace file
    host_files: HashMap<u32, Option<PathBuf>>,
}

/// Something the host sends on a TCP connection: (op, data), data only for DATA (the switch
/// cuts it into frames).
pub struct Answer {
    pub to: ConnKey,
    pub op: u8,
    pub data: Vec<u8>,
}

/// A datagram the host sends: (to vip, from our port, to port, data).
pub type Datagram = (u32, u16, u16, Vec<u8>);

/// Bytes as `hexdump -C` shows them, `base` being the first byte's offset.
pub fn hexdump(out: &mut String, data: &[u8], base: u64) {
    for (row, chunk) in data.chunks(16).enumerate() {
        let _ = write!(out, "  {:08x}  ", base + row as u64 * 16);
        for i in 0..16 {
            match chunk.get(i) {
                Some(b) => {
                    let _ = write!(out, "{b:02x} ");
                }
                None => out.push_str("   "),
            }
            if i == 7 {
                out.push(' ');
            }
        }
        out.push_str(" |");
        out.push_str(&printable(chunk));
        out.push_str("|\n");
    }
}

/// Printable ASCII as it is, everything else as '.'.
fn printable(data: &[u8]) -> String {
    data.iter().map(|&b| if (0x20..0x7f).contains(&b) { b as char } else { '.' }).collect()
}

fn since(t: Instant) -> f64 {
    t.elapsed().as_secs_f64()
}

impl Trace {
    pub fn new(dir: Option<PathBuf>) -> Trace {
        let dir = dir.filter(|d| match fs::create_dir_all(d) {
            Ok(()) => true,
            Err(e) => {
                eprintln!("trace: cannot use {}: {e}; tracing to stdout only", d.display());
                false
            }
        });
        let tx = dir.as_ref().and_then(|_| {
            let (tx, rx) = sync_channel::<(PathBuf, String)>(4096);
            std::thread::Builder::new()
                .name("trace-files".into())
                .stack_size(256 * 1024)
                .spawn(move || {
                    for (path, text) in rx {
                        let mut options = fs::OpenOptions::new();
                        options.create(true).append(true);
                        #[cfg(unix)]
                        std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
                        if let Err(e) = options.open(&path).and_then(|mut f| f.write_all(text.as_bytes())) {
                            eprintln!("trace: cannot write {}: {e}", path.display());
                        }
                    }
                })
                .ok()
                .map(|_| tx)
        });
        let mut hub = Hub::default();
        hub.trace_game = std::env::var("CARBON_TRACE_GAME").is_ok_and(|v| !v.is_empty() && v != "0");
        hub.host_game = std::env::var("CARBON_HOST_GAME").is_ok_and(|v| !v.is_empty() && v != "0");
        // Messenger's friends, names and blocks are kept here between runs (else for this run).
        let keep = std::env::var("CARBON_MESSENGER_DIR").ok().filter(|v| !v.is_empty()).map(PathBuf::from);
        hub.messenger = crate::messenger::Messenger::new(keep);
        // Player Stats and the Leaderboard, the same way (stats.rs).
        hub.stats = crate::stats::Stats::open(std::env::var("CARBON_STATS_DIR").ok().filter(|v| !v.is_empty()).map(PathBuf::from));
        Trace { dir, tx, conns: HashMap::new(), flows: HashMap::new(), seq: 0, dropped: 0, hub, host_files: HashMap::new() }
    }

    /// For the start-up line.
    pub fn describe(&self) -> String {
        match &self.dir {
            Some(d) => format!("files in {}; FESL, Theater and Messenger stand-ins on 18210, 18215, 13505", d.display()),
            None => "summaries only (set CARBON_TRACE_DIR for files); FESL, Theater and Messenger stand-ins on 18210, 18215, 13505".into(),
        }
    }

    fn new_file(&mut self, what: &str) -> Option<PathBuf> {
        let dir = self.dir.as_ref()?;
        self.seq += 1;
        Some(dir.join(format!("{}-{:05}-{what}.txt", file_stamp(SystemTime::now()), self.seq)))
    }

    fn write(&mut self, file: &Option<PathBuf>, text: String) {
        let (Some(path), Some(tx)) = (file, &self.tx) else { return };
        if text.is_empty() {
            return;
        }
        if let Err(TrySendError::Full(_)) = tx.try_send((path.clone(), text)) {
            self.dropped += 1;
            if self.dropped.is_power_of_two() {
                eprintln!("trace: the disk is behind; {} records not written", self.dropped);
            }
        }
    }

    /// A TCP segment from a browser to our `port`; the answers, in order, mostly on the same
    /// connection.
    pub fn tcp(&mut self, vip: u32, cport: u16, port: u16, op: u8, conn: u32, data: &[u8]) -> Vec<Answer> {
        let key = (vip, cport, port, conn);
        let here = |op: u8| vec![Answer { to: key, op, data: Vec::new() }];
        let name = format!("tcp {}:{cport} > 10.77.0.1:{port} [{conn:08x}]", vip_str(vip));
        match op {
            SYN => {
                if let Some(old) = self.conns.remove(&key) {
                    self.end(key, old, "a new SYN on the same connection".into());
                }
                let mine = self.conns.keys().filter(|k| k.0 == vip).count();
                if self.conns.len() >= MAX_CONNS || mine >= PER_VIP {
                    println!("trace: {name} refused (RST): too many connections");
                    return here(RST);
                }
                let svc = Service::on(port).map(|s| Session::new(s, key));
                let file = self.new_file(&format!("tcp-{}-{cport}-to-{port}-{conn:08x}", vip_str(vip)));
                let by = svc.as_ref().map(|s| format!(" (the {} stand-in answers)", s.service.name())).unwrap_or_default();
                let text = format!(
                    "carbon trace: {name}, opened {}\n+0.000 SYN, answered SYNACK{by}\n",
                    iso_date(SystemTime::now())
                );
                self.write(&file, text);
                let stream = Stream {
                    name,
                    file,
                    opened: Instant::now(),
                    segments: 0,
                    bytes: 0,
                    preview: Vec::new(),
                    svc,
                    sent: 0,
                    fin_sent: false,
                    last: Instant::now(),
                };
                self.conns.insert(key, stream);
                here(SYNACK)
            }
            DATA => {
                let Some(s) = self.conns.get_mut(&key) else {
                    println!("trace: {name} DATA ({} bytes) for no connection, answered RST", data.len());
                    return here(RST);
                };
                let offset = s.bytes;
                s.segments += 1;
                s.last = Instant::now();
                s.bytes += data.len() as u64;
                if s.svc.is_some() {
                    return self.stand_in(key, data);
                }
                let room = PREVIEW.saturating_sub(s.preview.len());
                s.preview.extend_from_slice(&data[..room.min(data.len())]);
                let mut text = format!("+{:.3} DATA {} bytes at {offset}\n", since(s.opened), data.len());
                if offset < MAX_LOGGED {
                    let shown = ((MAX_LOGGED - offset) as usize).min(data.len());
                    hexdump(&mut text, &data[..shown], offset);
                    if shown < data.len() || offset + data.len() as u64 >= MAX_LOGGED {
                        let _ = writeln!(text, "  (the rest of this connection's bytes are counted, not written)");
                    }
                } else {
                    text.clear();
                }
                let file = s.file.clone();
                self.write(&file, text);
                Vec::new()
            }
            FIN => match self.conns.remove(&key) {
                Some(s) => {
                    let file = s.file.clone();
                    // A stand-in that already closed its side has nothing more to say.
                    let answer = if s.fin_sent { Vec::new() } else { here(FIN) };
                    let how = if s.fin_sent { "" } else { ", answered FIN" };
                    self.write(&file, format!("+{:.3} FIN{how}\n", since(s.opened)));
                    self.end(key, s, "FIN".into());
                    answer
                }
                None => {
                    println!("trace: {name} FIN for no connection, answered RST");
                    here(RST)
                }
            },
            RST => {
                if let Some(s) = self.conns.remove(&key) {
                    let file = s.file.clone();
                    self.write(&file, format!("+{:.3} RST\n", since(s.opened)));
                    self.end(key, s, "RST from the client".into());
                }
                Vec::new()
            }
            // SYNACK: we never connect out; GONE never comes from a browser.
            _ => Vec::new(),
        }
    }

    /// Data for a stand-in: its packets and answers go to the trace a packet at a time (the
    /// game's own bytes with passwords masked) and to stdout a line each. An answer for
    /// another connection (Event::Push) is written to that connection's file.
    fn stand_in(&mut self, key: ConnKey, data: &[u8]) -> Vec<Answer> {
        let Some(s) = self.conns.get_mut(&key) else { return Vec::new() };
        let Some(svc) = s.svc.as_mut() else { return Vec::new() };
        let who = format!("{} {}:{}", svc.service.name(), vip_str(key.0), key.1);
        let result = svc.feed(data, &mut self.hub);
        let closing = svc.closing;
        let mut text = String::new();
        let mut answers = Vec::new();
        let mut pushes = Vec::new();
        let t = since(s.opened);
        let logged = |s: &Stream| s.bytes.saturating_sub(data.len() as u64) + s.sent < MAX_LOGGED;
        match result {
            Ok(events) => {
                for e in events {
                    match e {
                        Event::In(raw, summary) => {
                            println!("ea: {who} > {summary}");
                            let room = PREVIEW.saturating_sub(s.preview.len());
                            s.preview.extend_from_slice(&raw[..room.min(raw.len())]);
                            if logged(s) {
                                let _ = writeln!(text, "+{t:.3} IN {} bytes: {summary}", raw.len());
                                hexdump(&mut text, &raw, 0);
                            }
                        }
                        Event::Fragment(len, summary) => {
                            if logged(s) {
                                let _ = writeln!(text, "+{t:.3} IN {len} bytes, a fragment: {summary}");
                            }
                        }
                        Event::Note(note) => {
                            println!("ea: {who}: {note}");
                            let _ = writeln!(text, "+{t:.3} {note}");
                        }
                        Event::Out(raw, summary) => {
                            println!("ea: {who} < {summary}");
                            if logged(s) {
                                let _ = writeln!(text, "+{t:.3} OUT {} bytes: {summary}", raw.len());
                                hexdump(&mut text, &masked(&raw), 0);
                            }
                            s.sent += raw.len() as u64;
                            answers.push(Answer { to: key, op: DATA, data: raw });
                        }
                        Event::Push(to, raw, summary) => pushes.push((to, raw, summary)),
                        Event::Joined(lines) => {
                            if logged(s) {
                                let _ = write!(text, "{lines}");
                            }
                        }
                    }
                }
                if closing && !s.fin_sent {
                    s.fin_sent = true;
                    let _ = writeln!(text, "+{t:.3} the stand-in closes its side (FIN)");
                    answers.push(Answer { to: key, op: FIN, data: Vec::new() });
                }
                let file = s.file.clone();
                self.write(&file, text);
                for (to, raw, summary) in pushes {
                    let Some(o) = self.conns.get_mut(&to) else {
                        println!("ea: {who}: {summary} for {}:{}, which is gone; dropped", vip_str(to.0), to.1);
                        continue;
                    };
                    let other = format!("{} {}:{}", o.svc.as_ref().map_or("tcp", |v| v.service.name()), vip_str(to.0), to.1);
                    println!("ea: {other} < {summary} (for {who})");
                    let mut text = String::new();
                    if o.bytes + o.sent < MAX_LOGGED {
                        let _ = writeln!(text, "+{:.3} OUT {} bytes, for {who}: {summary}", since(o.opened), raw.len());
                        hexdump(&mut text, &masked(&raw), 0);
                    }
                    o.sent += raw.len() as u64;
                    let file = o.file.clone();
                    self.write(&file, text);
                    answers.push(Answer { to, op: DATA, data: raw });
                }
                answers
            }
            Err(e) => {
                println!("ea: {who}: {e}; reset");
                let _ = writeln!(text, "+{t:.3} {e}; answered RST");
                let file = s.file.clone();
                self.write(&file, text);
                if let Some(s) = self.conns.remove(&key) {
                    self.end(key, s, "a broken stream (RST)".into());
                }
                vec![Answer { to: key, op: RST, data: Vec::new() }]
            }
        }
    }

    /// The end of a connection: the file's last line and the summary on stdout; a stand-in's
    /// games and joins on it end too.
    fn end(&mut self, key: ConnKey, s: Stream, why: String) {
        let took = since(s.opened);
        let plural = if s.segments == 1 { "" } else { "s" };
        let file = s.file.clone();
        let left = s.svc.as_ref().map_or(0, |v| v.pending());
        if left > 0 {
            self.write(&file, format!("{left} bytes of an unfinished packet were left\n"));
        }
        if let Some(svc) = &s.svc {
            for e in self.hub.closed(key) {
                if let Event::Note(n) = e {
                    println!("ea: {} {}:{}: {n}", svc.service.name(), vip_str(key.0), key.1);
                    self.write(&file, format!("+{took:.3} {n}\n"));
                }
            }
        }
        let sent = if s.svc.is_some() { format!(", {} bytes sent", s.sent) } else { String::new() };
        self.write(&file, format!("closed: {why}; {} segment{plural}, {} bytes{sent} in {took:.3} s\n", s.segments, s.bytes));
        let at = s.file.as_ref().and_then(|f| f.file_name()).map(|f| format!(" -> {}", f.to_string_lossy())).unwrap_or_default();
        println!(
            "trace: {} ended by {why} after {took:.2} s: {} segment{plural}, {} bytes{sent} \"{}\"{at}",
            s.name,
            s.segments,
            s.bytes,
            printable(&s.preview)
        );
    }

    /// A UDP datagram from a browser to our `port` (op 0 data, 1 "unreachable"); the answers
    /// (Theater's ECHO, a race host's game traffic).
    pub fn udp(&mut self, vip: u32, cport: u16, port: u16, op: u8, data: &[u8]) -> Vec<Datagram> {
        if let Some(gid) = self.hub.hosts.values().find(|h| h.port == port).map(|h| h.gid) {
            if op != 0 {
                return Vec::new();
            }
            let out = self.hub.hosts.get_mut(&gid).map(|h| h.udp(vip, cport, data)).unwrap_or_default();
            self.host_notes(gid);
            return out.into_iter().map(|(to, to_port, d)| (to, port, to_port, d)).collect();
        }
        let key = (vip, cport, port);
        if !self.flows.contains_key(&key) {
            if self.flows.len() >= MAX_FLOWS {
                self.flows.clear(); // a flow seen again starts a new file
            }
            let file = self.new_file(&format!("udp-{}-{cport}-to-{port}", vip_str(vip)));
            let text = format!(
                "carbon trace: udp {}:{cport} > 10.77.0.1:{port}, first datagram {}\n",
                vip_str(vip),
                iso_date(SystemTime::now())
            );
            self.write(&file, text);
            self.flows.insert(key, Flow { file, first: Instant::now(), bytes: 0 });
        }
        let echo = if port == THEATER_PORT && op == 0 { theater_udp(vip, cport, data) } else { None };
        let f = self.flows.get_mut(&key).expect("just made");
        let offset = f.bytes;
        f.bytes += data.len() as u64;
        let kind = if op == 1 { " (unreachable notice)" } else { "" };
        let how = if echo.is_some() { "answered" } else { "not answered" };
        let mut text = String::new();
        if offset < MAX_LOGGED {
            let _ = writeln!(text, "+{:.3} datagram {} bytes{kind}, {how}", since(f.first), data.len());
            hexdump(&mut text, &data[..((MAX_LOGGED - offset) as usize).min(data.len())], 0);
        }
        let mut out = Vec::new();
        if let Some((q, r)) = &echo {
            let raw = r.encode();
            if offset < MAX_LOGGED {
                let _ = writeln!(text, "+{:.3} answer {} bytes: {}", since(f.first), raw.len(), r.summary());
                hexdump(&mut text, &raw, 0);
            }
            println!("ea: theater udp {}:{cport} > {}", vip_str(vip), q.summary());
            println!("ea: theater udp {}:{cport} < {}", vip_str(vip), r.summary());
            out.push((vip, port, cport, raw));
        }
        let (file, name) = (f.file.clone(), f.file.as_ref().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().into_owned()));
        self.write(&file, text);
        if echo.is_none() {
            println!(
                "trace: udp {}:{cport} > 10.77.0.1:{port}{kind}, {} bytes \"{}\"{}",
                vip_str(vip),
                data.len(),
                printable(&data[..data.len().min(PREVIEW)]),
                name.map(|n| format!(" -> {n}")).unwrap_or_default()
            );
        }
        out
    }

    /// The race hosts' timers (resends, link syncs, timeouts): what they send now.
    pub fn tick(&mut self) -> Vec<Datagram> {
        let gids: Vec<u32> = self.hub.hosts.keys().copied().collect();
        let mut out = Vec::new();
        for gid in gids {
            let Some(h) = self.hub.hosts.get_mut(&gid) else { continue };
            let port = h.port;
            out.extend(h.tick().into_iter().map(|(to, to_port, d)| (to, port, to_port, d)));
            self.host_notes(gid);
        }
        out
    }

    /// Pings on stand-in connections that have been quiet for a while, as EA's servers sent
    /// them: the game's FESL connection gives up after 180 s without traffic ("conn err
    /// -206", then all three connections close), which a 4-minute race reaches (run r14).
    pub fn keepalive(&mut self) -> Vec<Answer> {
        let mut answers = Vec::new();
        let mut files = Vec::new();
        // Messenger notices with no packet to answer (a friend's connection ended)
        let notices: Vec<(ConnKey, Packet)> = std::mem::take(&mut self.hub.messenger.outbox);
        for (to, p) in notices {
            let Some(s) = self.conns.get_mut(&to) else { continue };
            let raw = p.encode();
            let summary = p.summary();
            println!("ea: messenger {}:{} < {summary}", vip_str(to.0), to.1);
            let mut text = String::new();
            if s.bytes + s.sent < MAX_LOGGED {
                let _ = writeln!(text, "+{:.3} OUT {} bytes: {summary}", since(s.opened), raw.len());
                hexdump(&mut text, &masked(&raw), 0);
            }
            s.sent += raw.len() as u64;
            s.last = Instant::now();
            files.push((s.file.clone(), text));
            answers.push(Answer { to, op: DATA, data: raw });
        }
        for (key, s) in self.conns.iter_mut() {
            let Some(svc) = s.svc.as_ref() else { continue };
            if s.fin_sent || s.last.elapsed().as_secs() < crate::ea::PING_AFTER_SECS {
                continue;
            }
            let (raw, summary) = svc.ping();
            println!("ea: {} {}:{} < {summary} (quiet for {} s)", svc.service.name(), vip_str(key.0), key.1, s.last.elapsed().as_secs());
            let mut text = String::new();
            if s.bytes + s.sent < MAX_LOGGED {
                let _ = writeln!(text, "+{:.3} OUT {} bytes, a keep-alive: {summary}", since(s.opened), raw.len());
                hexdump(&mut text, &raw, 0);
            }
            s.sent += raw.len() as u64;
            s.last = Instant::now();
            files.push((s.file.clone(), text));
            answers.push(Answer { to: *key, op: DATA, data: raw });
        }
        for (file, text) in files {
            self.write(&file, text);
        }
        answers
    }

    /// A race host's notes go to stdout, its decrypted traffic to its own trace file.
    fn host_notes(&mut self, gid: u32) {
        let Some(h) = self.hub.hosts.get_mut(&gid) else { return };
        let open = h.joinable;
        let notes: Vec<String> = h.notes.drain(..).map(|n| n.0).collect();
        let log = std::mem::take(&mut h.log);
        let reports = std::mem::take(&mut h.reports);
        let port = h.port;
        // The leader's DisableJoins only keeps Theater's list honest; PlayNow still sends a
        // second Custom Match player into the leader's room (there are no invites on this
        // server), see playnow_status.
        self.hub.games.set_open(gid, open);
        for n in &notes {
            println!("host {gid}: {n}");
        }
        // the race's results counted for Player Stats and the Leaderboard
        reports.into_iter().flat_map(|r| self.hub.stats.race_report(r)).for_each(|l| println!("{l}"));
        if self.dir.is_none() || (log.is_empty() && notes.is_empty()) {
            return;
        }
        if !self.host_files.contains_key(&gid) {
            let file = self.new_file(&format!("host-{gid}-udp-{port}"));
            self.write(&file, format!("carbon trace: race host of game {gid} on 10.77.0.1:{port} (decrypted), from {}\n", iso_date(SystemTime::now())));
            self.host_files.insert(gid, file);
        }
        let file = self.host_files.get(&gid).cloned().flatten();
        let mut text = log;
        for n in notes {
            text.push_str("note: ");
            text.push_str(&n);
            text.push('\n');
        }
        self.write(&file, text);
    }

    /// A browser left the network: its connections are over.
    /// The page's player key for this vip (lan.js's /lan hello): only its SHA-256 is kept, for
    /// FESL Login's check of the name's claim (messenger.rs). Never logged.
    pub fn set_player_key(&mut self, vip: u32, key: &str) {
        if key.is_empty() {
            self.hub.player_keys.remove(&vip);
        } else {
            self.hub.player_keys.insert(vip, crate::messenger::digest(key));
        }
    }

    pub fn gone(&mut self, vip: u32) {
        self.hub.player_keys.remove(&vip);
        let keys: Vec<ConnKey> = self.conns.keys().filter(|k| k.0 == vip).copied().collect();
        for key in keys {
            if let Some(s) = self.conns.remove(&key) {
                let file = s.file.clone();
                self.write(&file, format!("+{:.3} the player left the network\n", since(s.opened)));
                self.end(key, s, "the player leaving".into());
            }
        }
        self.flows.retain(|k, _| k.0 != vip);
    }
}

#[cfg(test)]
mod tests {
    use super::hexdump;

    #[test]
    fn hexdump_looks_like_hexdump_c() {
        let mut s = String::new();
        hexdump(&mut s, b"CONN\x00\x00\x00\xc0hello, world!!", 16);
        assert_eq!(
            s,
            "  00000010  43 4f 4e 4e 00 00 00 c0  68 65 6c 6c 6f 2c 20 77  |CONN....hello, w|\n\
             \x20 00000020  6f 72 6c 64 21 21                                 |orld!!|\n"
        );
    }
}
