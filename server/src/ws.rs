//! WebSockets, shared by the relay's rooms and the /lan switch: the upgrade (routes, key
//! check), frames as the Node relay reads and writes them (RFC 6455, unfragmented frames, a
//! message at most MAX_MESSAGE), each connection's queue of unsent messages and its writer,
//! and the read loop with its pings.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use sha1::{Digest, Sha1};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::Notify;
use tokio::task::JoinHandle;
use tokio::time::{timeout, MissedTickBehavior};

use crate::http::{client, Conn, Request, Url};
use crate::util::{base64, decode_component, js_slice, lock, same_bytes};
use crate::App;

/// A bigger frame closes the connection (as the Node relay; the game's frames are far smaller).
pub const MAX_MESSAGE: usize = 4096;
/// A reader that has taken nothing for this long is removed.
const BLOCKED_TIMEOUT: Duration = Duration::from_secs(15);
/// Pong replies come from the browser itself, so a player idle in a menu or a background tab
/// stays connected; one that stops answering goes.
const PING_INTERVAL: Duration = Duration::from_secs(15);
const PONG_TIMEOUT: Duration = Duration::from_secs(10);
const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const BAD_REQUEST: &[u8] = b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
const FORBIDDEN: &[u8] = b"HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";

/// An upgrade request: GET /lan is the switch, any other path a relay room (the decoded path,
/// at most 40 long, "lobby" when empty). Refusals answer as the Node relay did: 400 for a URL
/// or room it could not decode, 403 for a wrong key; no Sec-WebSocket-Key or a full room
/// closes the connection without an answer.
pub async fn upgrade(app: Arc<App>, mut conn: Conn, req: Request) {
    let Some(url) = Url::parse(&req.target) else { return refuse(conn, BAD_REQUEST).await };
    let lan = url.path == "/lan";
    let room = if lan {
        String::new()
    } else {
        match decode_component(&url.path[1..]) {
            Some(r) if js_slice(&r, 40).is_empty() => "lobby".to_string(),
            Some(r) => js_slice(&r, 40).to_string(),
            None => return refuse(conn, BAD_REQUEST).await,
        }
    };
    let given = url.query("key").unwrap_or_default();
    if !app.key.is_empty() && !same_bytes(given.as_bytes(), app.key.as_bytes()) {
        return refuse(conn, FORBIDDEN).await;
    }
    let accept = match req.header("sec-websocket-key") {
        Some(k) if !k.is_empty() => {
            let mut sha = Sha1::new();
            sha.update(k.as_bytes());
            sha.update(GUID.as_bytes());
            base64(&sha.finalize())
        }
        _ => return,
    };
    // The read buffer may already hold the first frames (a hello sent with the upgrade); they
    // stay, the room the request head needed goes.
    conn.buf.shrink_to_fit();
    if lan {
        let who = client(&conn, &req);
        crate::lan::join(app, conn, who, accept).await
    } else {
        crate::relay::join(app, conn, room, accept).await
    }
}

async fn refuse(mut conn: Conn, answer: &[u8]) {
    let _ = conn.write(answer).await;
    conn.linger().await;
}

/// Answers the upgrade (101) and starts the writer; then the caller reads.
pub async fn open(mut conn: Conn, accept: &str, out: &Arc<Outbox>) -> Option<(OwnedReadHalf, Vec<u8>, JoinHandle<()>)> {
    let answer = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
    );
    if !matches!(timeout(Duration::from_secs(10), conn.stream.write_all(answer.as_bytes())).await, Ok(Ok(()))) {
        return None;
    }
    let (rd, wr) = conn.stream.into_split();
    let writer = tokio::spawn(writer(out.clone(), wr));
    Some((rd, conn.buf, writer))
}

/// One frame from the server (never masked).
pub fn frame(payload: &[u8], opcode: u8) -> Vec<u8> {
    let n = payload.len();
    let mut f = Vec::with_capacity(n + 10);
    f.push(0x80 | opcode);
    if n < 126 {
        f.push(n as u8);
    } else if n < 65536 {
        f.push(126);
        f.extend_from_slice(&(n as u16).to_be_bytes());
    } else {
        f.push(127);
        f.extend_from_slice(&(n as u64).to_be_bytes());
    }
    f.extend_from_slice(payload);
    f
}

/// A close frame with a status code and a short reason.
pub fn close_frame(code: u16, reason: &str) -> Vec<u8> {
    let mut p = code.to_be_bytes().to_vec();
    p.extend_from_slice(reason.as_bytes());
    frame(&p, 8)
}

pub enum Parsed {
    /// wait for more bytes
    More,
    /// over MAX_MESSAGE: the connection closes
    TooBig,
    Frame { op: u8, payload: Vec<u8>, used: usize },
}

/// One frame from the front of `buf`. As the Node relay: each frame is a message (FIN and
/// continuation frames are not looked at), masked or not.
pub fn parse(buf: &[u8]) -> Parsed {
    if buf.len() < 2 {
        return Parsed::More;
    }
    let op = buf[0] & 15;
    let masked = buf[1] & 128 != 0;
    let mut len = (buf[1] & 127) as u64;
    let mut at = 2;
    if len == 126 {
        if buf.len() < 4 {
            return Parsed::More;
        }
        len = u16::from_be_bytes([buf[2], buf[3]]) as u64;
        at = 4;
    } else if len == 127 {
        if buf.len() < 10 {
            return Parsed::More;
        }
        len = u64::from_be_bytes(buf[2..10].try_into().unwrap());
        at = 10;
    }
    if len > MAX_MESSAGE as u64 {
        return Parsed::TooBig;
    }
    let mask_at = at;
    if masked {
        at += 4;
    }
    let len = len as usize;
    if buf.len() < at + len {
        return Parsed::More;
    }
    let mut payload = buf[at..at + len].to_vec();
    if masked {
        let m = [buf[mask_at], buf[mask_at + 1], buf[mask_at + 2], buf[mask_at + 3]];
        for (i, b) in payload.iter_mut().enumerate() {
            *b ^= m[i & 3];
        }
    }
    Parsed::Frame { op, payload, used: at + len }
}

/// What waits to be written to one connection, and its state. Messages are queued here and
/// the connection's writer sends everything queued in one write. While a write is stuck
/// (the client reads slower than it is sent to), a newer pose of the same car replaces the
/// queued one and goes to the end, after reliable messages sent meanwhile, and past `limit`
/// queued bytes the client is cut off: the Node relay's rules for a reader that stopped
/// reading.
pub struct Outbox {
    q: Mutex<Queue>,
    wake: Notify,
    kill: Notify,
    limit: usize,
}

struct Queue {
    /// (key, frame): key 0 for a reliable message, else a pose's sender id
    items: VecDeque<(u64, Arc<[u8]>)>,
    bytes: usize,
    /// a write is in progress, i.e. the socket did not take everything at once
    writing: bool,
    state: State,
}

enum State {
    Open,
    /// leaving: nothing more is queued; the writer sends this last frame, if any, and closes
    Finishing(Option<Vec<u8>>),
    /// cut off: the connection is dropped
    Dead,
}

enum Take {
    Write,
    Wait,
    Finish(Option<Vec<u8>>),
    Stop,
}

impl Queue {
    fn remove(&mut self, key: u64) {
        if let Some(i) = self.items.iter().position(|(k, _)| *k == key) {
            if let Some((_, b)) = self.items.remove(i) {
                self.bytes -= b.len();
            }
        }
    }

    fn die(&mut self) {
        self.items.clear();
        self.bytes = 0;
        self.state = State::Dead;
    }
}

impl Outbox {
    pub fn new(limit: usize) -> Arc<Outbox> {
        Arc::new(Outbox {
            q: Mutex::new(Queue { items: VecDeque::new(), bytes: 0, writing: false, state: State::Open }),
            wake: Notify::new(),
            kill: Notify::new(),
            limit,
        })
    }

    /// Queues a frame; `key` != 0 replaces a queued frame with the same key (see above).
    pub fn push(&self, key: u64, bytes: Arc<[u8]>) {
        self.add(key, bytes, None);
    }

    /// Queues a frame that may be lost (a datagram): dropped instead, and false, when more
    /// than `shed` bytes are waiting.
    pub fn push_droppable(&self, bytes: Arc<[u8]>, shed: usize) -> bool {
        self.add(0, bytes, Some(shed))
    }

    fn add(&self, key: u64, bytes: Arc<[u8]>, shed: Option<usize>) -> bool {
        let mut q = lock(&self.q);
        if !matches!(q.state, State::Open) {
            return false;
        }
        // Only behind a stuck write: a reader that keeps up gets every pose, in order.
        if key != 0 && q.writing {
            q.remove(key);
        }
        let total = q.bytes + bytes.len();
        // Only a stuck write counts (the Node relay queued only then: a reader that keeps up
        // loses nothing); the larger figures are a backstop for a writer that has not run.
        if shed.is_some_and(|s| (q.writing && total > s) || total > 4 * s) {
            return false;
        }
        if (q.writing && total > self.limit) || total > 16 * self.limit {
            q.die();
            drop(q);
            self.kill.notify_one();
            self.wake.notify_one();
            return false;
        }
        q.bytes = total;
        q.items.push_back((key, bytes));
        drop(q);
        self.wake.notify_one();
        true
    }

    /// Takes a queued frame back (a departed player's last pose must never follow its "left").
    pub fn remove(&self, key: u64) {
        lock(&self.q).remove(key);
    }

    /// Cuts the connection off now.
    pub fn kill(&self) {
        lock(&self.q).die();
        self.kill.notify_one();
        self.wake.notify_one();
    }

    /// Ends the connection in order: what is queued is dropped (the player has left), then
    /// `last` (a close frame) is sent and the socket closed.
    pub fn finish(&self, last: Option<Vec<u8>>) {
        let mut q = lock(&self.q);
        if matches!(q.state, State::Open) {
            q.items.clear();
            q.bytes = 0;
            q.state = State::Finishing(last);
        }
        drop(q);
        self.wake.notify_one();
    }

    pub fn is_open(&self) -> bool {
        matches!(lock(&self.q).state, State::Open)
    }

    pub fn is_dead(&self) -> bool {
        matches!(lock(&self.q).state, State::Dead)
    }

    fn take(&self, chunk: &mut Vec<u8>) -> Take {
        let mut guard = lock(&self.q);
        let q = &mut *guard;
        match &mut q.state {
            State::Dead => Take::Stop,
            State::Finishing(last) => Take::Finish(last.take()),
            State::Open if q.items.is_empty() => Take::Wait,
            State::Open => {
                for (_, b) in q.items.drain(..) {
                    chunk.extend_from_slice(&b);
                }
                q.bytes = 0;
                q.writing = true;
                Take::Write
            }
        }
    }
}

/// Sends what is queued, in one write each time, until the connection ends.
async fn writer(out: Arc<Outbox>, mut w: OwnedWriteHalf) {
    let mut chunk = Vec::new();
    loop {
        match out.take(&mut chunk) {
            Take::Write => {
                let done = timeout(BLOCKED_TIMEOUT, w.write_all(&chunk)).await;
                lock(&out.q).writing = false;
                if !matches!(done, Ok(Ok(()))) {
                    out.kill();
                    return;
                }
                if chunk.capacity() > 64 * 1024 {
                    chunk = Vec::new();
                } else {
                    chunk.clear();
                }
            }
            Take::Wait => out.wake.notified().await,
            Take::Finish(last) => {
                if let Some(bytes) = last {
                    let _ = timeout(Duration::from_secs(5), w.write_all(&bytes)).await;
                }
                let _ = timeout(Duration::from_secs(5), w.shutdown()).await;
                return;
            }
            Take::Stop => return,
        }
    }
}

/// What a connection does with its messages.
pub trait Session {
    fn text(&mut self, text: &str);
    fn binary(&mut self, data: Vec<u8>);
    /// Checked every second; false drops the connection (the relay's hello timeout).
    fn alive(&mut self, now: Instant) -> bool;
    /// The connection is over: leave the room or the network.
    fn leave(&mut self);
}

static PINGS: AtomicU32 = AtomicU32::new(0x6a09_e667);

/// A ping's payload: only has to differ from the last one.
fn next_ping() -> [u8; 4] {
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(0);
    (PINGS.fetch_add(0x9e37_79b9, Ordering::Relaxed) ^ nanos).to_be_bytes()
}

enum End {
    /// a close frame, or a frame over the limit: answer with a close frame
    Close,
    /// the client closed its side
    Eof,
    /// cut off, or the session closed the connection itself
    Gone,
}

/// Reads the client's frames until the connection ends, then leaves.
pub async fn run(mut rd: OwnedReadHalf, mut buf: Vec<u8>, out: Arc<Outbox>, writer: JoinHandle<()>, s: &mut impl Session) {
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    tick.set_missed_tick_behavior(MissedTickBehavior::Delay);
    tick.tick().await;
    let mut ping: Option<[u8; 4]> = None;
    let mut last_ping = Instant::now();
    let end = loop {
        let mut used = 0;
        let mut end = None;
        while end.is_none() {
            match parse(&buf[used..]) {
                Parsed::More => break,
                Parsed::TooBig => end = Some(End::Close),
                Parsed::Frame { op, payload, used: n } => {
                    used += n;
                    match op {
                        1 => s.text(&String::from_utf8_lossy(&payload)),
                        2 => s.binary(payload),
                        8 => end = Some(End::Close),
                        9 if payload.len() <= 125 => out.push(0, frame(&payload, 10).into()),
                        10 if ping.is_some_and(|p| p[..] == payload[..]) => ping = None,
                        _ => {}
                    }
                    // Bytes after a departure are not looked at.
                    if end.is_none() && !out.is_open() {
                        end = Some(End::Gone);
                    }
                }
            }
        }
        if let Some(end) = end {
            break end;
        }
        if used > 0 {
            // Let the receivers' writers send what this read gave them before the next read,
            // as Node's event loop did between two 'data' events: otherwise a sender with a
            // full socket would run alone for a while and pile up its receivers' queues.
            tokio::task::yield_now().await;
        }
        buf.drain(..used);
        if buf.is_empty() && buf.capacity() > 16 * 1024 {
            buf = Vec::new();
        }
        if buf.capacity() - buf.len() < 1024 {
            buf.reserve(4096);
        }
        tokio::select! {
            r = rd.read_buf(&mut buf) => if !matches!(r, Ok(n) if n > 0) { break End::Eof },
            _ = out.kill.notified() => break End::Gone,
            _ = tick.tick() => {
                let now = Instant::now();
                if !s.alive(now) || (ping.is_some() && now.duration_since(last_ping) >= PONG_TIMEOUT) {
                    out.kill();
                    break End::Gone;
                }
                if ping.is_none() && now.duration_since(last_ping) >= PING_INTERVAL {
                    let p = next_ping();
                    ping = Some(p);
                    last_ping = now;
                    out.push(0, frame(&p, 9).into());
                }
            }
        }
    };
    s.leave();
    match end {
        End::Close => out.finish(Some(vec![0x88, 0])),
        End::Eof => out.finish(None),
        End::Gone => {}
    }
    if out.is_dead() {
        writer.abort();
        return;
    }
    if !matches!(end, End::Eof) {
        // Our close frame is on its way: what the client still sends is read and dropped
        // until it closes too (as the Node relay's socket kept reading), so the kernel does
        // not answer unread data with a reset that could destroy the close frame.
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        let mut sink = vec![0u8; 4096];
        while let Ok(Ok(n)) = tokio::time::timeout_at(deadline, rd.read(&mut sink)).await {
            if n == 0 {
                break;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn masked(payload: &[u8], op: u8) -> Vec<u8> {
        let mask = [0x21, 0x43, 0x65, 0x87];
        let mut f = vec![0x80 | op];
        if payload.len() < 126 {
            f.push(0x80 | payload.len() as u8);
        } else {
            f.push(0xfe);
            f.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        }
        f.extend_from_slice(&mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        f
    }

    #[test]
    fn frames_round_trip() {
        let f = masked(b"hello", 1);
        assert!(matches!(parse(&f[..3]), Parsed::More));
        match parse(&f) {
            Parsed::Frame { op, payload, used } => {
                assert_eq!((op, payload.as_slice(), used), (1, &b"hello"[..], f.len()));
            }
            _ => panic!("no frame"),
        }
        let big = masked(&[b'x'; 300], 2);
        assert!(matches!(parse(&big), Parsed::Frame { op: 2, used, .. } if used == big.len()));
        let over = masked(&[b'x'; MAX_MESSAGE + 1], 1);
        assert!(matches!(parse(&over[..4]), Parsed::TooBig));
        assert_eq!(frame(b"hi", 1), vec![0x81, 2, b'h', b'i']);
        assert_eq!(&frame(&[0; 200], 2)[..4], &[0x82, 126, 0, 200]);
        assert_eq!(close_frame(1008, "x"), vec![0x88, 3, 0x03, 0xf0, b'x']);
    }

    #[test]
    fn a_newer_pose_replaces_the_queued_one_behind_a_stuck_write() {
        let out = Outbox::new(100);
        let mut chunk = Vec::new();
        // A reader that keeps up gets every pose.
        out.push(7, Arc::from(&b"pose0"[..]));
        out.push(7, Arc::from(&b"pose1"[..]));
        assert!(matches!(out.take(&mut chunk), Take::Write));
        assert_eq!(chunk, b"pose0pose1");
        chunk.clear();
        // While that write is stuck, the newest pose replaces the queued one and goes last.
        out.push(7, Arc::from(&b"pose2"[..]));
        out.push(0, Arc::from(&b"chat"[..]));
        out.push(7, Arc::from(&b"pose3"[..]));
        lock(&out.q).writing = false;
        assert!(matches!(out.take(&mut chunk), Take::Write));
        assert_eq!(chunk, b"chatpose3");
        // Over the limit while a write is stuck: cut off.
        out.push(0, Arc::from(&[0u8; 60][..]));
        out.push(0, Arc::from(&[0u8; 60][..]));
        assert!(out.is_dead());
    }

    #[test]
    fn datagrams_are_shed_not_fatal() {
        let out = Outbox::new(1000);
        let mut chunk = Vec::new();
        out.push(0, Arc::from(&[0u8; 10][..]));
        assert!(matches!(out.take(&mut chunk), Take::Write));
        assert!(out.push_droppable(Arc::from(&[0u8; 50][..]), 64));
        assert!(!out.push_droppable(Arc::from(&[0u8; 50][..]), 64));
        assert!(out.is_open());
    }
}
