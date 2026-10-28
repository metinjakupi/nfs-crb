//! The /lan switch and the trace host, through the real binary on a free port: fake clients
//! (a few lines of WebSocket each) exchange the kit's frames as plans/lan-protocol.md
//! describes them.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant};

const KEY: &str = "local-lan-test";
const HOST: u32 = 0x0a4d_0001;
const V16: u32 = 0x0a4d_0010;
const V17: u32 = 0x0a4d_0011;
const V18: u32 = 0x0a4d_0012;

struct Server {
    child: Child,
    port: u16,
    lines: Arc<Mutex<Vec<String>>>,
}

impl Server {
    fn start(env: &[(&str, &str)]) -> Server {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_carbon-server"));
        cmd.args(["0", "127.0.0.1"])
            .env("CARBON_RELAY_KEY", KEY)
            .env_remove("CARBON_SAVES_DIR")
            .env_remove("CARBON_TRACE_DIR")
            .env_remove("CARBON_MESSENGER_DIR")
            .env_remove("CARBON_STATS_DIR")
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit());
        for (k, v) in env {
            cmd.env(k, v);
        }
        let mut child = cmd.spawn().expect("start carbon-server");
        let stdout = child.stdout.take().unwrap();
        let lines = Arc::new(Mutex::new(Vec::new()));
        let (tx, rx) = mpsc::channel();
        let seen = lines.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                if let Some(rest) = line.strip_prefix("relay on ws://127.0.0.1:") {
                    let _ = tx.send(rest.split('/').next().unwrap().parse::<u16>().unwrap());
                }
                seen.lock().unwrap().push(line);
            }
        });
        let port = rx.recv_timeout(Duration::from_secs(10)).expect("the server's first line");
        Server { child, port, lines }
    }

    /// The first stdout line that passes `check`, waiting up to 3 s for it.
    fn line(&self, check: impl Fn(&str) -> bool) -> String {
        let end = Instant::now() + Duration::from_secs(3);
        loop {
            if let Some(l) = self.lines.lock().unwrap().iter().find(|l| check(l)) {
                return l.clone();
            }
            assert!(Instant::now() < end, "no such line in:\n{}", self.lines.lock().unwrap().join("\n"));
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[derive(Debug, PartialEq)]
enum Msg {
    Text(String),
    Bin(Vec<u8>),
    Close(Vec<u8>),
}

struct Client {
    s: TcpStream,
    buf: Vec<u8>,
}

/// The HTTP status of an upgrade, and the connection if it was 101.
fn upgrade(port: u16, path: &str) -> (u16, Option<Client>) {
    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
    write!(
        s,
        "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
         Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
    )
    .unwrap();
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    let end = loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break i;
        }
        let n = s.read(&mut chunk).unwrap();
        assert!(n > 0, "closed before an answer");
        buf.extend_from_slice(&chunk[..n]);
    };
    let head = String::from_utf8_lossy(&buf[..end]).to_string();
    let status: u16 = head.split(' ').nth(1).unwrap().parse().unwrap();
    if status == 101 {
        assert!(head.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "{head}");
    }
    let rest = buf[end + 4..].to_vec();
    (status, (status == 101).then_some(Client { s, buf: rest }))
}

impl Client {
    /// A browser on the network: its welcome checked, its vip returned.
    fn lan(server: &Server) -> (Client, u32) {
        let (status, c) = upgrade(server.port, &format!("/lan?key={KEY}"));
        assert_eq!(status, 101);
        let mut c = c.unwrap();
        let Msg::Text(welcome) = c.read() else { panic!("no welcome") };
        let v: serde_json::Value = serde_json::from_str(&welcome).unwrap();
        assert_eq!(v["t"], "lan");
        assert_eq!(v["netmask"], "255.255.248.0");
        let vip: std::net::Ipv4Addr = v["vip"].as_str().unwrap().parse().unwrap();
        (c, u32::from(vip))
    }

    fn send(&mut self, op: u8, payload: &[u8]) {
        let mask = [0x37, 0xfa, 0x21, 0x3d];
        let mut f = vec![0x80 | op];
        if payload.len() < 126 {
            f.push(0x80 | payload.len() as u8);
        } else {
            f.push(0x80 | 126);
            f.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        }
        f.extend_from_slice(&mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        self.s.write_all(&f).unwrap();
    }

    fn frame(&mut self, f: &[u8]) {
        self.send(2, f);
    }

    fn try_read(&mut self, wait: Duration) -> Option<Msg> {
        self.s.set_read_timeout(Some(wait)).unwrap();
        let mut chunk = [0u8; 4096];
        loop {
            if self.buf.len() >= 2 {
                let (mut n, mut at) = ((self.buf[1] & 127) as usize, 2);
                if n == 126 && self.buf.len() >= 4 {
                    n = u16::from_be_bytes([self.buf[2], self.buf[3]]) as usize;
                    at = 4;
                }
                if (n < 126 || at == 4) && self.buf.len() >= at + n {
                    let op = self.buf[0] & 15;
                    let payload = self.buf[at..at + n].to_vec();
                    self.buf.drain(..at + n);
                    return Some(match op {
                        1 => Msg::Text(String::from_utf8(payload).unwrap()),
                        2 => Msg::Bin(payload),
                        8 => Msg::Close(payload),
                        _ => continue,
                    });
                }
            }
            match self.s.read(&mut chunk) {
                Ok(0) => return None,
                Ok(n) => self.buf.extend_from_slice(&chunk[..n]),
                Err(_) => return None,
            }
        }
    }

    fn read(&mut self) -> Msg {
        self.try_read(Duration::from_secs(3)).expect("a message")
    }

    fn read_frame(&mut self) -> Frame {
        match self.read() {
            Msg::Bin(b) => Frame::parse(&b),
            other => panic!("expected a frame, got {other:?}"),
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
struct Frame {
    kind: u8,
    src: u32,
    dst: u32,
    sport: u16,
    dport: u16,
    op: u8,
    conn: u32,
    data: Vec<u8>,
}

impl Frame {
    #[allow(clippy::too_many_arguments)]
    fn new(kind: u8, src: u32, dst: u32, sport: u16, dport: u16, op: u8, conn: u32, data: &[u8]) -> Frame {
        Frame { kind, src, dst, sport, dport, op, conn, data: data.to_vec() }
    }

    fn udp(dst: u32, sport: u16, dport: u16, data: &[u8]) -> Frame {
        Frame::new(3, 0x0a4d_0063, dst, sport, dport, 0, 0, data) // a made-up source: the switch writes the real one
    }

    fn tcp(dst: u32, sport: u16, dport: u16, op: u8, conn: u32, data: &[u8]) -> Frame {
        Frame::new(4, 0x0a4d_0063, dst, sport, dport, op, conn, data)
    }

    fn bytes(&self) -> Vec<u8> {
        let mut f = vec![0u8; 24];
        f[0] = self.kind;
        f[4..8].copy_from_slice(&self.src.to_be_bytes());
        f[8..12].copy_from_slice(&self.dst.to_be_bytes());
        f[12..14].copy_from_slice(&self.sport.to_be_bytes());
        f[14..16].copy_from_slice(&self.dport.to_be_bytes());
        f[16] = self.op;
        f[17] = 64;
        f[20..24].copy_from_slice(&self.conn.to_le_bytes());
        f.extend_from_slice(&self.data);
        f
    }

    fn parse(b: &[u8]) -> Frame {
        assert!(b.len() >= 24, "short frame {b:?}");
        assert_eq!(&b[1..4], &[0, 0, 0]);
        let be32 = |i: usize| u32::from_be_bytes(b[i..i + 4].try_into().unwrap());
        Frame {
            kind: b[0],
            src: be32(4),
            dst: be32(8),
            sport: u16::from_be_bytes([b[12], b[13]]),
            dport: u16::from_be_bytes([b[14], b[15]]),
            op: b[16],
            conn: u32::from_le_bytes(b[20..24].try_into().unwrap()),
            data: b[24..].to_vec(),
        }
    }

    /// As the receiver sees it: from `src`.
    fn from(mut self, src: u32) -> Frame {
        self.src = src;
        self
    }
}

/// A frame sent to the sender proves that everything sent to it before has arrived.
fn barrier(to: &mut Client, from: &mut Client, from_vip: u32, to_vip: u32) {
    let marker = Frame::udp(to_vip, 9, 9, b"barrier");
    from.frame(&marker.bytes());
    assert_eq!(to.read_frame(), marker.from(from_vip));
}

#[test]
fn vips_and_udp_between_two_browsers() {
    let server = Server::start(&[]);
    let (mut a, va) = Client::lan(&server);
    let (mut b, vb) = Client::lan(&server);
    assert_eq!((va, vb), (V16, V17), "addresses from 10.77.0.16 upwards");
    a.send(1, br#"{"t":"hello","name":"Alice"}"#);
    server.line(|l| l == "lan: 10.77.0.16 is Alice");
    // The source is rewritten to the sender's own address, whatever the sender wrote.
    let ping = Frame::udp(vb, 3658, 3658, b"ping\x00\x01");
    a.frame(&ping.bytes());
    assert_eq!(b.read_frame(), ping.clone().from(va));
    let pong = Frame::udp(va, 3658, 3658, b"pong");
    b.frame(&pong.bytes());
    assert_eq!(a.read_frame(), pong.from(vb));
    // An "unreachable" notice goes back the same way.
    let unreach = Frame::new(3, 0, va, 3658, 3658, 1, 0, b"");
    b.frame(&unreach.bytes());
    assert_eq!(a.read_frame(), unreach.from(vb));
}

#[test]
fn tcp_syn_data_fin_through_the_switch() {
    let server = Server::start(&[]);
    let (mut a, va) = Client::lan(&server);
    let (mut b, vb) = Client::lan(&server);
    let conn = 0xdead_beef;
    let steps = [
        (true, Frame::tcp(vb, 50123, 9900, 1, conn, b"")),
        (false, Frame::tcp(va, 9900, 50123, 2, conn, b"")),
        (true, Frame::tcp(vb, 50123, 9900, 4, conn, b"@dir\x00\x00\x00\x10hello")),
        (false, Frame::tcp(va, 9900, 50123, 4, conn, &[0xab; 1400])),
        (true, Frame::tcp(vb, 50123, 9900, 5, conn, b"")),
        (false, Frame::tcp(va, 9900, 50123, 5, conn, b"")),
    ];
    for (from_a, f) in steps {
        if from_a {
            a.frame(&f.bytes());
            assert_eq!(b.read_frame(), f.from(va));
        } else {
            b.frame(&f.bytes());
            assert_eq!(a.read_frame(), f.from(vb));
        }
    }
    // Closed with both FINs: a leaving now sends b no GONE, only the gone message.
    drop(a);
    assert_eq!(b.read(), Msg::Text(r#"{"t":"gone","vip":"10.77.0.16"}"#.into()));
}

#[test]
fn broadcasts_reach_everyone_but_the_sender() {
    let server = Server::start(&[]);
    let (mut a, va) = Client::lan(&server);
    let (mut b, vb) = Client::lan(&server);
    let (mut c, vc) = Client::lan(&server);
    for dst in [0x0a4d_07ff, 0xffff_ffff] {
        let hello = Frame::udp(dst, 9999, 9999, b"who is there?");
        a.frame(&hello.bytes());
        assert_eq!(b.read_frame(), hello.clone().from(va));
        assert_eq!(c.read_frame(), hello.from(va));
    }
    // TCP is never broadcast; a frame nobody can take is dropped and the sender stays.
    a.frame(&Frame::tcp(0xffff_ffff, 1, 2, 1, 7, b"").bytes());
    a.frame(&Frame::udp(0x0a4d_0020, 1, 2, b"to nobody").bytes());
    a.frame(&Frame::udp(0x0a4d_0002, 1, 2, b"to a reserved address").bytes());
    a.frame(&[3, 0, 0, 0, 0, 0, 0, 0]); // too short
    let mut odd = Frame::udp(vb, 1, 2, b"x");
    odd.kind = 9;
    a.frame(&odd.bytes()); // a kind the switch does not know
    a.frame(&Frame::udp(vb, 1, 2, &[0; 1473]).bytes()); // over the MTU
    // The sender heard none of its own broadcasts; b and c only what they should.
    barrier(&mut a, &mut b, vb, va);
    barrier(&mut b, &mut a, va, vb);
    barrier(&mut c, &mut a, va, vc);
}

#[test]
fn a_departure_resets_its_connections() {
    let server = Server::start(&[]);
    let (mut a, va) = Client::lan(&server);
    let (mut b, vb) = Client::lan(&server);
    let (mut c, _vc) = Client::lan(&server);
    // a has two connections to b (one opened by each side) and none to c.
    let syn1 = Frame::tcp(vb, 50000, 9900, 1, 11, b"");
    a.frame(&syn1.bytes());
    assert_eq!(b.read_frame(), syn1.from(va));
    let syn2 = Frame::tcp(va, 40000, 3658, 1, 22, b"");
    b.frame(&syn2.bytes());
    assert_eq!(a.read_frame(), syn2.from(vb));
    // One more, reset at once: not followed any more.
    let syn3 = Frame::tcp(vb, 50001, 9900, 1, 33, b"");
    a.frame(&syn3.bytes());
    assert_eq!(b.read_frame(), syn3.from(va));
    let rst3 = Frame::tcp(va, 9900, 50001, 3, 33, b"");
    b.frame(&rst3.bytes());
    assert_eq!(a.read_frame(), rst3.from(vb));
    drop(a);
    let mut gone = vec![b.read_frame(), b.read_frame()];
    gone.sort_by_key(|f| f.conn);
    assert_eq!(
        gone,
        vec![
            Frame::new(4, va, vb, 50000, 9900, 6, 11, b""),
            Frame::new(4, va, vb, 3658, 40000, 6, 22, b""),
        ]
    );
    assert_eq!(b.read(), Msg::Text(r#"{"t":"gone","vip":"10.77.0.16"}"#.into()));
    assert_eq!(c.read(), Msg::Text(r#"{"t":"gone","vip":"10.77.0.16"}"#.into()));
    server.line(|l| l == "lan: (10.77.0.16) left, 2 on the network");
    // The next browser gets the next address, not the one just freed.
    let (_d, vd) = Client::lan(&server);
    assert_eq!(vd, V18 + 1);
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("carbon-server-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    dir
}

/// The trace file whose name contains `part`, once it holds `text`.
fn trace_file(dir: &PathBuf, part: &str, text: &str) -> String {
    let end = Instant::now() + Duration::from_secs(3);
    loop {
        for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            if entry.file_name().to_string_lossy().contains(part) {
                let body = std::fs::read_to_string(entry.path()).unwrap();
                if body.contains(text) {
                    return body;
                }
            }
        }
        assert!(Instant::now() < end, "no trace file *{part}* with {text:?}");
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[test]
fn the_trace_host_accepts_records_and_closes() {
    let dir = temp_dir("trace");
    let server = Server::start(&[("CARBON_TRACE_DIR", dir.to_str().unwrap())]);
    let (mut a, va) = Client::lan(&server);
    let conn = 0x1a2b_3c4d;
    // SYN on any port: SYNACK from 10.77.0.1, ports swapped, the same connection id. (Ports
    // without a stand-in: on 18210, 18215 and 13505 the EA stand-ins answer, see below.)
    a.frame(&Frame::tcp(HOST, 50123, 18299, 1, conn, b"").bytes());
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18299, 50123, 2, conn, b""));
    // Data and datagrams are recorded and not answered: the next answer is the FIN's.
    let hello = b"CONN\x00\x00\x00\xc0hello, world!!";
    a.frame(&Frame::tcp(HOST, 50123, 18299, 4, conn, hello).bytes());
    a.frame(&Frame::tcp(HOST, 50123, 18299, 4, conn, b"more").bytes());
    a.frame(&Frame::udp(HOST, 3658, 3658, b"udp!\x01\x02").bytes());
    a.frame(&Frame::tcp(HOST, 50123, 18299, 5, conn, b"").bytes());
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18299, 50123, 5, conn, b""));
    // Data or FIN for a connection that is over, or never was: RST.
    a.frame(&Frame::tcp(HOST, 50123, 18299, 4, conn, b"late").bytes());
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18299, 50123, 3, conn, b""));
    a.frame(&Frame::tcp(HOST, 1, 80, 5, 99, b"").bytes());
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 80, 1, 3, 99, b""));
    // An RST from the client ends a connection without an answer.
    a.frame(&Frame::tcp(HOST, 50124, 18298, 1, 2, b"").bytes());
    assert_eq!(a.read_frame().op, 2);
    a.frame(&Frame::tcp(HOST, 50124, 18298, 3, 2, b"").bytes());
    // A connection still open when its browser leaves ends there.
    a.frame(&Frame::tcp(HOST, 50125, 80, 1, 3, b"").bytes());
    assert_eq!(a.read_frame().op, 2);
    a.frame(&Frame::tcp(HOST, 50125, 80, 4, 3, b"GET /news HTTP/1.0\r\n\r\n").bytes());
    barrier(&mut a, &mut Client::lan(&server).0, 0x0a4d_0011, va);

    let line = server.line(|l| l.contains("10.77.0.16:50123 > 10.77.0.1:18299"));
    assert!(line.starts_with("trace: tcp 10.77.0.16:50123 > 10.77.0.1:18299 [1a2b3c4d] ended by FIN after"), "{line}");
    assert!(line.contains(": 2 segments, 26 bytes \"CONN....hello, world!!more\" -> "), "{line}");
    let udp = server.line(|l| l.starts_with("trace: udp"));
    assert!(udp.starts_with("trace: udp 10.77.0.16:3658 > 10.77.0.1:3658, 6 bytes \"udp!..\""), "{udp}");
    server.line(|l| l.contains("[00000002] ended by RST from the client"));

    let tcp = trace_file(&dir, "tcp-10.77.0.16-50123-to-18299-1a2b3c4d", "closed:");
    assert!(tcp.starts_with("carbon trace: tcp 10.77.0.16:50123 > 10.77.0.1:18299 [1a2b3c4d], opened 20"), "{tcp}");
    assert!(tcp.contains("+0.000 SYN, answered SYNACK\n"), "{tcp}");
    assert!(tcp.contains(" DATA 22 bytes at 0\n  00000000  43 4f 4e 4e 00 00 00 c0  68 65 6c 6c 6f 2c 20 77  |CONN....hello, w|\n"), "{tcp}");
    assert!(tcp.contains("  00000010  6f 72 6c 64 21 "), "{tcp}");
    assert!(tcp.contains(" DATA 4 bytes at 22\n  00000016  6d 6f 72 65 "), "{tcp}");
    assert!(tcp.contains("|more|\n"), "{tcp}");
    assert!(tcp.contains(" FIN, answered FIN\nclosed: FIN; 2 segments, 26 bytes in "), "{tcp}");
    let udp = trace_file(&dir, "udp-10.77.0.16-3658-to-3658", "|udp!..|");
    assert!(udp.contains(" datagram 6 bytes, not answered\n  00000000  75 64 70 21 01 02 "), "{udp}");

    drop(a);
    let left = server.line(|l| l.contains("[00000003] ended by the player leaving"));
    assert!(left.contains("1 segment, 22 bytes \"GET /news HTTP/1.0....\""), "{left}");
    trace_file(&dir, "tcp-10.77.0.16-50125-to-80-00000003", "closed: the player leaving");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_key_is_checked_and_the_relay_still_answers() {
    let server = Server::start(&[]);
    for path in ["/lan", "/lan?key=wrong", "/lan?key=local-lan-tes"] {
        assert_eq!(upgrade(server.port, path).0, 403, "{path}");
    }
    let (_a, va) = Client::lan(&server);
    assert_eq!(va, V16);
    // A relay room on the same port.
    let (status, room) = upgrade(server.port, &format!("/lobby?key={KEY}"));
    assert_eq!(status, 101);
    let mut room = room.unwrap();
    room.send(1, br#"{"t":"hi","name":"Bob"}"#);
    let Msg::Text(welcome) = room.read() else { panic!() };
    assert_eq!(welcome, r#"{"t":"welcome","id":1,"peers":[]}"#);
}

#[test]
fn without_a_key_anyone_may_join_and_a_close_frame_is_answered() {
    let server = Server::start(&[("CARBON_RELAY_KEY", "")]);
    let (status, c) = upgrade(server.port, "/lan");
    assert_eq!(status, 101);
    let mut c = c.unwrap();
    assert!(matches!(c.read(), Msg::Text(t) if t.contains(r#""vip":"10.77.0.16""#)));
    c.send(8, &[0x03, 0xe8]);
    assert_eq!(c.read(), Msg::Close(vec![]));
    server.line(|l| l == "lan: (10.77.0.16) left, 0 on the network");
}

#[test]
fn a_flood_is_cut_off() {
    let server = Server::start(&[]);
    let (mut a, _va) = Client::lan(&server);
    let (mut b, vb) = Client::lan(&server);
    // More than the budget's burst of bytes at once: a is closed with 1008.
    let big = Frame::udp(vb, 1, 2, &[0; 1400]).bytes();
    let mut all = Vec::new();
    for _ in 0..1000 {
        let mask = [0u8; 4];
        all.extend_from_slice(&[0x82, 0x80 | 126]);
        all.extend_from_slice(&(big.len() as u16).to_be_bytes());
        all.extend_from_slice(&mask);
        all.extend_from_slice(&big);
    }
    let _ = a.s.write_all(&all);
    let end = Instant::now() + Duration::from_secs(5);
    loop {
        match a.try_read(Duration::from_secs(1)) {
            Some(Msg::Close(p)) => {
                assert_eq!(p, [&[0x03u8, 0xf0][..], b"too fast"].concat());
                break;
            }
            Some(_) => assert!(Instant::now() < end),
            None => panic!("closed without a close frame"),
        }
    }
    server.line(|l| l == "lan: 10.77.0.16 sent too fast; closed");
    // b got what fitted in the budget (some of it shed if it read too slowly), then the gone message.
    let mut frames = 0;
    loop {
        match b.read() {
            Msg::Bin(_) => frames += 1,
            Msg::Text(t) => {
                assert_eq!(t, r#"{"t":"gone","vip":"10.77.0.16"}"#);
                break;
            }
            other => panic!("{other:?}"),
        }
    }
    assert!(frames > 0 && frames < 1000, "{frames}");
}

/// An Aries packet (FESL, Theater, Messenger): type, word, length, "key=value\n" lines, NUL.
fn aries(kind: &[u8; 4], id: u32, lines: &[(&str, &str)]) -> Vec<u8> {
    let body: String = lines.iter().map(|(k, v)| format!("{k}={v}\n")).collect();
    let mut p = kind.to_vec();
    p.extend_from_slice(&id.to_be_bytes());
    p.extend_from_slice(&((12 + body.len() + 1) as u32).to_be_bytes());
    p.extend_from_slice(body.as_bytes());
    p.push(0);
    p
}

#[derive(Debug)]
struct Reply {
    kind: String,
    id: u32,
    lines: Vec<(String, String)>,
}

impl Reply {
    fn get(&self, key: &str) -> &str {
        self.lines.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str()).unwrap_or_else(|| panic!("no {key} in {self:?}"))
    }
}

/// The next `n` packets a stand-in sends on the connection from `port`, read from its DATA
/// frames (a packet may span frames, a frame may hold several).
fn replies(c: &mut Client, port: u16, n: usize) -> Vec<Reply> {
    let mut stream = Vec::new();
    let mut out = Vec::new();
    while out.len() < n {
        let f = c.read_frame();
        assert_eq!((f.kind, f.src, f.sport, f.op), (4, HOST, port, 4), "{f:?}");
        stream.extend_from_slice(&f.data);
        while stream.len() >= 12 {
            let len = u32::from_be_bytes(stream[8..12].try_into().unwrap()) as usize;
            if stream.len() < len {
                break;
            }
            let p: Vec<u8> = stream.drain(..len).collect();
            assert_eq!(p[len - 1], 0, "a packet ends with a NUL");
            let text = String::from_utf8(p[12..len - 1].to_vec()).unwrap();
            let lines = text.lines().map(|l| l.split_once('=').map(|(k, v)| (k.to_string(), v.to_string())).unwrap()).collect();
            out.push(Reply { kind: String::from_utf8(p[..4].to_vec()).unwrap(), id: u32::from_be_bytes(p[4..8].try_into().unwrap()), lines });
        }
    }
    assert!(stream.is_empty());
    out
}

#[test]
fn the_ea_stand_ins_log_a_player_in() {
    let dir = temp_dir("ea");
    let server = Server::start(&[("CARBON_TRACE_DIR", dir.to_str().unwrap())]);
    let (mut a, va) = Client::lan(&server);
    let tcp = |port: u16, op: u8, conn: u32, data: &[u8]| Frame::tcp(HOST, 50200, port, op, conn, data).bytes();

    // FESL, as the game opens it: SYN, then its Hello (byte for byte from a trace of the game),
    // here cut in two segments.
    let fesl = 0x0f35_1000;
    a.frame(&tcp(18210, 1, fesl, b""));
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18210, 50200, 2, fesl, b""));
    let hello = aries(
        b"fsys",
        0x8000_0001,
        &[
            ("TXN", "Hello"),
            ("clientString", "nfs-pc"),
            ("sku", "America"),
            ("locale", "en_US"),
            ("clientPlatform", "PC"),
            ("clientVersion", "2.0"),
            ("SDKVersion", "2.9.7.1.0"),
            ("protocolVersion", "2.0"),
            ("fragmentSize", "2048"),
            ("clientType", "client"),
        ],
    );
    assert_eq!(hello.len(), 181);
    a.frame(&tcp(18210, 4, fesl, &hello[..17]));
    a.frame(&tcp(18210, 4, fesl, &hello[17..]));
    // The answer, then the server's own MemCheck (id 0x80000000), as EA's server did.
    let r = replies(&mut a, 18210, 2);
    assert_eq!((r[0].kind.as_str(), r[0].id, r[0].get("TXN")), ("fsys", 0x8000_0001, "Hello"));
    assert_eq!(r[0].get("theaterIp"), "nfs-pc.theater.ea.com");
    assert_eq!(r[0].get("theaterPort"), "18215");
    assert_eq!(r[0].get("messengerPort"), "13505");
    assert_eq!(r[0].get("domainPartition.subDomain"), "NFS-2007");
    assert!(r[0].get("curTime").starts_with('"') && r[0].get("curTime").ends_with(" UTC\""), "{:?}", r[0]);
    assert_eq!((r[1].kind.as_str(), r[1].id, r[1].get("TXN"), r[1].get("memcheck.[]")), ("fsys", 0x8000_0000, "MemCheck", "0"));
    a.frame(&tcp(18210, 4, fesl, &aries(b"fsys", 0x8000_0000, &[("TXN", "MemCheck"), ("result", "")])));

    // Login: any name; the password is never looked at, and the trace masks it.
    let login = [("TXN", "Login"), ("returnEncryptedInfo", "0"), ("name", "Night Owl"), ("password", "hunter2"), ("machineId", ""), ("macAddr", "$0a4d00100000")];
    a.frame(&tcp(18210, 4, fesl, &aries(b"acct", 0x8000_0002, &login)));
    let r = replies(&mut a, 18210, 1);
    assert_eq!((r[0].kind.as_str(), r[0].id, r[0].get("TXN")), ("acct", 0x8000_0002, "Login"));
    assert_eq!(r[0].get("displayName"), "NightOwl");
    let lkey = r[0].get("lkey").to_string();
    assert_eq!(lkey.len(), 27);
    assert_eq!(r[0].get("userId"), r[0].get("profileId"));
    assert_eq!(r[0].get("userId").len(), 9);

    a.frame(&tcp(18210, 4, fesl, &aries(b"fsys", 0x8000_0003, &[("TXN", "GetPingSites")])));
    let r = replies(&mut a, 18210, 1);
    assert_eq!((r[0].id, r[0].get("pingSite.[]"), r[0].get("minPingSitesToPing")), (0x8000_0003, "3", "0"));
    assert_eq!(r[0].get("pingSite.0.addr"), "10.77.0.1");

    // A long transaction in fragments (0xb0000000 | id; base64 with '=' written %3d) is
    // answered once whole.
    let text = "TXN=ReportMetrics\nevents.[]=1\nevents.0.props.0.name=SIGNED_IN\n\0";
    let b64 = base64(text.as_bytes());
    let size = b64.len().to_string();
    for piece in [&b64[..30], &b64[30..]] {
        let data = piece.replace('=', "%3d");
        a.frame(&tcp(18210, 4, fesl, &aries(b"mtrx", 0xb000_0004, &[("size", &size), ("data", &data)])));
    }
    let r = replies(&mut a, 18210, 1);
    assert_eq!((r[0].kind.as_str(), r[0].id, r[0].get("TXN")), ("mtrx", 0x8000_0004, "ReportMetrics"));

    // Something no stand-in knows gets a FESL error rather than silence.
    a.frame(&tcp(18210, 4, fesl, &aries(b"acct", 0x8000_0005, &[("TXN", "GetTos")])));
    let r = replies(&mut a, 18210, 1);
    assert_eq!((r[0].get("TXN"), r[0].get("errorCode")), ("GetTos", "99"));

    // Theater knows the player by the login key. (The game sends Theater and Messenger packets
    // with a 0 word, as our trace of it shows.)
    let theater = 0x0f35_2000;
    a.frame(&Frame::tcp(HOST, 50201, 18215, 1, theater, b"").bytes());
    assert_eq!(a.read_frame().op, 2);
    let t = |c: &mut Client, data: Vec<u8>| c.frame(&Frame::tcp(HOST, 50201, 18215, 4, theater, &data).bytes());
    t(&mut a, aries(b"CONN", 0, &[("PROT", "2"), ("PROD", "nfs-pc"), ("VERS", "1.0"), ("PLAT", "PC"), ("LOCALE", "en_US"), ("TID", "1")]));
    let r = replies(&mut a, 18215, 1);
    assert_eq!((r[0].kind.as_str(), r[0].id, r[0].get("TID"), r[0].get("PROT")), ("CONN", 0, "1", "2"));
    t(&mut a, aries(b"USER", 0, &[("CID", ""), ("MAC", "$0a4d00100000"), ("SKU", "America"), ("LKEY", &lkey), ("NAME", "GBPlrINetLAN"), ("TID", "2")]));
    let r = replies(&mut a, 18215, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("NAME"), r[0].get("TID")), ("USER", "NightOwl", "2"));
    t(&mut a, aries(b"LLST", 0, &[("FILTER-FAV-ONLY", "0"), ("TID", "3")]));
    let r = replies(&mut a, 18215, 2);
    assert_eq!((r[0].kind.as_str(), r[0].get("NUM-LOBBIES")), ("LLST", "1"));
    assert_eq!((r[1].kind.as_str(), r[1].get("LID"), r[1].get("NAME"), r[1].get("TID")), ("LDAT", "257", "Internet", "3"));

    // Messenger too.
    let messenger = 0x0f35_3000;
    a.frame(&Frame::tcp(HOST, 50202, 13505, 1, messenger, b"").bytes());
    assert_eq!(a.read_frame().op, 2);
    let m = |c: &mut Client, data: Vec<u8>| c.frame(&Frame::tcp(HOST, 50202, 13505, 4, messenger, &data).bytes());
    m(&mut a, aries(b"AUTH", 0, &[("LKEY", &lkey), ("PROD", "America"), ("VERS", "2.0"), ("PRES", "nfs-pc"), ("RSRC", "/EAGAMES/NFS-2007"), ("ID", "1")]));
    let r = replies(&mut a, 13505, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("ID"), r[0].get("USER")), ("AUTH", "1", "NightOwl@messaging.ea.com/eagames/NFS-2007"));
    m(&mut a, aries(b"RGET", 0, &[("LIST", "B"), ("LRSC", "eagames"), ("PRES", "Y"), ("PEND", "Y"), ("ID", "2")]));
    let r = replies(&mut a, 13505, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("ID"), r[0].get("SIZE")), ("RGET", "2", "0"));

    // Goodbye: the stand-in closes its side, and the game's FIN then gets no answer.
    a.frame(&tcp(18210, 4, fesl, &aries(b"fsys", 0x8000_0006, &[("TXN", "Goodbye"), ("reason", "GOODBYE_CLIENT_NORMAL")])));
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18210, 50200, 5, fesl, b""));
    a.frame(&tcp(18210, 5, fesl, b""));
    // A stream the stand-in cannot follow (a length the game never sends) is reset.
    a.frame(&tcp(18210, 1, fesl + 1, b""));
    assert_eq!(a.read_frame().op, 2);
    a.frame(&tcp(18210, 4, fesl + 1, b"fsys\x80\x00\x00\x07\x00\x00\x00\x02"));
    assert_eq!(a.read_frame(), Frame::new(4, HOST, va, 18210, 50200, 3, fesl + 1, b""));

    server.line(|l| l.contains("ea: fesl 10.77.0.16:50200: NightOwl logged in (userId "));
    server.line(|l| l.starts_with("ea: fesl 10.77.0.16:50200 > acct 0x80000002 {TXN=Login, returnEncryptedInfo=0, name=Night Owl, password=(not kept)"));
    server.line(|l| l.starts_with("ea: theater 10.77.0.16:50201 < USER 0x00000000 {TID=2, NAME=NightOwl}"));
    server.line(|l| l.contains("[0f351000] ended by FIN after"));
    assert!(!server.lines.lock().unwrap().iter().any(|l| l.contains("hunter2")));
    let f = trace_file(&dir, "tcp-10.77.0.16-50200-to-18210-0f351000", "closed:");
    assert!(f.contains("+0.000 SYN, answered SYNACK (the fesl stand-in answers)\n"), "{f}");
    assert!(f.contains(" IN 181 bytes: fsys 0x80000001 {TXN=Hello, clientString=nfs-pc"), "{f}");
    assert!(f.contains(" OUT "), "{f}");
    // The dumped bytes, put back together from the hex columns: the password is masked.
    let dumped: Vec<u8> = f
        .lines()
        .filter(|l| l.starts_with("  0000"))
        .flat_map(|l| l[12..60].split_whitespace().map(|h| u8::from_str_radix(h, 16).unwrap()).collect::<Vec<u8>>())
        .collect();
    assert!(dumped.windows(17).any(|w| w == b"password=*******\n"), "{f}");
    assert!(!dumped.windows(7).any(|w| w == b"hunter2") && !f.contains("hunter2"), "{f}");
    assert!(f.contains("password=(not kept)"), "{f}");
    assert!(f.contains("a fragment: mtrx 0xb0000004 {size="), "{f}");
    assert!(f.contains("the stand-in closes its side (FIN)"), "{f}");
    assert!(f.contains("closed: FIN; "), "{f}");
    let _ = std::fs::remove_dir_all(&dir);
}

/// Standard base64 (the test's own, to build fragments).
fn base64(bytes: &[u8]) -> String {
    const ABC: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in bytes.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            out.push(if i <= c.len() { ABC[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    out
}

/// A player signed in to FESL and Theater through the switch, as the game does it: FESL Login,
/// Theater CONN and USER, and the UDP ECHO from its game port 1042.
struct Player {
    c: Client,
    vip: u32,
    fesl: (u16, u32),
    theater: (u16, u32),
    tid: u32,
}

impl Player {
    fn sign_in(server: &Server, name: &str, cport: u16) -> Player {
        let (mut c, vip) = Client::lan(server);
        let (fport, fconn, tport, tconn) = (cport, 0x5a00_0000 | cport as u32, cport + 1, 0x5b00_0000 | cport as u32);
        c.frame(&Frame::tcp(HOST, fport, 18210, 1, fconn, b"").bytes());
        assert_eq!(c.read_frame().op, 2);
        let login = [("TXN", "Login"), ("name", name), ("password", "x"), ("macAddr", "$0a4d00100000")];
        c.frame(&Frame::tcp(HOST, fport, 18210, 4, fconn, &aries(b"acct", 0x8000_0002, &login)).bytes());
        let lkey = replies(&mut c, 18210, 1)[0].get("lkey").to_string();
        c.frame(&Frame::tcp(HOST, tport, 18215, 1, tconn, b"").bytes());
        assert_eq!(c.read_frame().op, 2);
        let mut p = Player { c, vip, fesl: (fport, fconn), theater: (tport, tconn), tid: 0 };
        p.theater(b"CONN", &[("PROT", "2"), ("PROD", "nfs-pc"), ("VERS", "2.0"), ("PLAT", "PC")]);
        assert_eq!(p.theater_replies(1)[0].kind, "CONN");
        p.theater(b"USER", &[("LKEY", &lkey), ("NAME", "GBPlrINetLAN")]);
        assert_eq!(p.theater_replies(1)[0].get("NAME"), name);
        // ECHO from the game socket: answered over UDP with the address the switch sees.
        let echo = aries(b"ECHO", 0, &[("TID", "1"), ("TYPE", "1"), ("UID", "1")]);
        p.c.frame(&Frame::udp(HOST, 1042, 18215, &echo).bytes());
        let f = p.c.read_frame();
        assert_eq!((f.kind, f.src, f.sport, f.dst, f.dport), (3, HOST, 18215, vip, 1042), "{f:?}");
        let text = String::from_utf8_lossy(&f.data[12..]).to_string();
        assert!(f.data.starts_with(b"ECHO\0\0\0\0"), "{text}");
        let ip = std::net::Ipv4Addr::from(vip).to_string();
        for line in ["TID=1\n", "TXN=ECHO\n", &format!("IP={ip}\n"), "PORT=1042\n", "ERR=0\n", "TYPE=1\n"] {
            assert!(text.contains(line), "{line} in {text}");
        }
        p
    }

    fn theater(&mut self, kind: &[u8; 4], lines: &[(&str, &str)]) {
        self.tid += 1;
        let tid = self.tid.to_string();
        let mut l = lines.to_vec();
        l.push(("TID", &tid));
        let (port, conn) = self.theater;
        self.c.frame(&Frame::tcp(HOST, port, 18215, 4, conn, &aries(kind, 0, &l)).bytes());
    }

    fn theater_replies(&mut self, n: usize) -> Vec<Reply> {
        replies(&mut self.c, 18215, n)
    }

    /// PlayNow Start (Custom Match): the Start answer, then the Status.
    fn play_now(&mut self, id: u32) -> Reply {
        let (port, conn) = self.fesl;
        let start = [
            ("TXN", "Start"),
            ("partition.partition", "/EAGAMES/NFS-2007"),
            ("players.[]", "1"),
            ("players.0.props.{sessionType}", "resetServer"),
            ("players.0.props.{filter-version}", "298_prod_server+22012b18"),
            ("players.0.props.{filterToGame-version}", "version"),
            ("players.0.props.{filter-game_type}", "1"),
            ("players.0.props.{filterToGame-game_type}", "U-game_type"),
            ("players.0.props.{pref-game_mode}", "1"),
            ("players.0.props.{prefToGame-game_mode}", "U-game_mode"),
            ("players.0.props.{pref-max_online_player}", "4"),
        ];
        self.c.frame(&Frame::tcp(HOST, port, 18210, 4, conn, &aries(b"pnow", 0x8000_0000 | id, &start)).bytes());
        let r = replies(&mut self.c, 18210, 2);
        assert_eq!((r[0].kind.as_str(), r[0].id, r[0].get("TXN")), ("pnow", 0x8000_0000 | id, "Start"));
        assert_eq!((r[1].id, r[1].get("TXN"), r[1].get("sessionState")), (0x8000_0000, "Status", "COMPLETE"));
        assert_eq!(r[1].get("id.id"), r[0].get("id.id"));
        r.into_iter().nth(1).unwrap()
    }
}

#[test]
fn two_players_meet_in_a_custom_match() {
    let dir = temp_dir("match");
    let server = Server::start(&[("CARBON_TRACE_DIR", dir.to_str().unwrap())]);
    let mut host = Player::sign_in(&server, "Host", 50300);
    let mut guest = Player::sign_in(&server, "Guest", 50310);

    // Nobody hosts yet: PlayNow finds nothing (Carbon's client never hosts on its own: it
    // ignores PlayNow's CREATE, so the server says NOMATCH and the game says so).
    let s = host.play_now(0x20);
    assert_eq!((s.get("props.{resultType}"), s.get("props.{}")), ("NOMATCH", "1"));

    // CGAM (what a host sends; Carbon's client does not, a future host would): the game
    // exists, with a game id, the tunnel key and a UGID.
    host.theater(
        b"CGAM",
        &[
            ("LID", "257"),
            ("RESERVE-HOST", "0"),
            ("NAME", "Host"),
            ("PORT", "1042"),
            ("HTTYPE", "A"),
            ("TYPE", "G"),
            ("QLEN", "0"),
            ("DISABLE-AUTO-DEQUEUE", "1"),
            ("HXFR", "0"),
            ("INT-PORT", "1042"),
            ("INT-IP", "10.77.0.16"),
            ("MAX-PLAYERS", "4"),
            ("B-maxGameSize", "4"),
            ("JOIN", "O"),
            ("B-version", "298_prod_server+22012b18"),
            ("B-U-game_type", "1"),
            ("B-U-track", "4011"),
        ],
    );
    let r = host.theater_replies(1);
    assert_eq!((r[0].kind.as_str(), r[0].get("TID"), r[0].get("LID"), r[0].get("MAX-PLAYERS"), r[0].get("JOIN")), ("CGAM", "3", "257", "4", "O"));
    let gid = r[0].get("GID").to_string();
    let ekey = r[0].get("EKEY").to_string();
    let ugid = r[0].get("UGID").to_string();
    assert!(ekey.ends_with("%3d%3d") && ekey.len() == 28, "{ekey}");
    assert_eq!(ugid.len(), 36);
    host.theater(b"UGAM", &[("LID", "257"), ("GID", &gid), ("B-U-track", "4012")]);

    // The second player's PlayNow finds it.
    let s = guest.play_now(0x21);
    assert_eq!((s.get("props.{resultType}"), s.get("props.{games}.0.gid"), s.get("props.{games}.0.lid")), ("JOIN", gid.as_str(), "257"));

    // The game list shows it, with the host's address and its attributes.
    guest.theater(b"GLST", &[("LID", "257"), ("TYPE", "G"), ("COUNT", "-1")]);
    let r = guest.theater_replies(2);
    assert_eq!((r[0].kind.as_str(), r[0].get("NUM-GAMES")), ("GLST", "1"));
    assert_eq!((r[1].kind.as_str(), r[1].get("GID"), r[1].get("HN"), r[1].get("I"), r[1].get("P")), ("GDAT", gid.as_str(), "Host", "10.77.0.16", "1042"));
    assert_eq!((r[1].get("B-U-track"), r[1].get("AP"), r[1].get("MP"), r[1].get("TID")), ("4012", "1", "4", "3"));
    guest.theater(b"GDAT", &[("LID", "257"), ("GID", &gid)]);
    let r = guest.theater_replies(2);
    assert_eq!((r[0].kind.as_str(), r[1].kind.as_str(), r[1].get("UGID")), ("GDAT", "GDET", ugid.as_str()));

    // An EA Messenger game invite names its sender, not the game: the invited game reads the
    // sender's game by name (GDAT USER=, as the game sent it) ...
    // ... and its players, one PDAT each (the game's SDK waits for AP of them before the
    // invite screen shows the game's details) ...
    guest.theater(b"GDAT", &[("USER", "host"), ("TYPE", "G")]);
    let r = guest.theater_replies(3);
    assert_eq!((r[0].kind.as_str(), r[0].get("GID"), r[0].get("AP"), r[1].kind.as_str()), ("GDAT", gid.as_str(), "1", "GDET"));
    let host_uid = r[0].get("HU").to_string();
    assert_eq!((r[2].kind.as_str(), r[2].get("TID"), r[2].get("GID"), r[2].get("PID"), r[2].get("NAME"), r[2].get("UID")), ("PDAT", "5", gid.as_str(), "1", "Host", host_uid.as_str()));
    guest.theater(b"GDAT", &[("USER", "Nobody"), ("TYPE", "G")]);
    let r = guest.theater_replies(1);
    assert_eq!((r[0].kind.as_str(), r[0].get("TID"), r[0].lines.len()), ("GDAT", "6", 1), "nobody's game: the TID alone");

    // ... and joins it by the sender's user id (EGAM UID=, no game id). The guest's EGAM
    // becomes the host's EGRQ ...
    guest.theater(b"EGAM", &[("PORT", "1042"), ("R-INT-PORT", "1042"), ("R-INT-IP", "10.77.0.17"), ("UID", &host_uid), ("R-UID", &host_uid), ("TYPE", "G")]);
    let r = guest.theater_replies(1);
    assert_eq!((r[0].kind.as_str(), r[0].get("TID"), r[0].get("GID")), ("EGAM", "7", gid.as_str()));
    let q = host.theater_replies(1);
    assert_eq!((q[0].kind.as_str(), q[0].get("NAME"), q[0].get("IP"), q[0].get("PORT"), q[0].get("GID")), ("EGRQ", "Guest", "10.77.0.17", "1042", gid.as_str()));
    let (pid, ticket) = (q[0].get("PID").to_string(), q[0].get("TICKET").to_string());
    // ... the host lets it in, and the guest gets the host's address and the same tunnel key.
    host.theater(b"EGRS", &[("LID", "257"), ("GID", &gid), ("PID", &pid), ("ALLOWED", "1")]);
    assert_eq!(host.theater_replies(1)[0].kind, "EGRS");
    let e = guest.theater_replies(1);
    assert_eq!((e[0].kind.as_str(), e[0].get("I"), e[0].get("P"), e[0].get("INT-IP"), e[0].get("INT-PORT")), ("EGEG", "10.77.0.16", "1042", "10.77.0.16", "1042"));
    assert_eq!((e[0].get("EKEY"), e[0].get("TICKET"), e[0].get("PID"), e[0].get("UGID")), (ekey.as_str(), ticket.as_str(), pid.as_str(), ugid.as_str()));
    host.theater(b"PENT", &[("LID", "257"), ("GID", &gid), ("PID", &pid)]);
    let r = host.theater_replies(1);
    assert_eq!((r[0].kind.as_str(), r[0].get("PID")), ("PENT", pid.as_str()));

    // From here the two games talk directly, port 1042 to port 1042, through the switch.
    let hello = Frame::udp(guest.vip, 1042, 1042, b"GM hello");
    host.c.frame(&hello.bytes());
    assert_eq!(guest.c.read_frame(), hello.from(host.vip));

    // The host leaves: its game is gone from the list.
    let (port, conn) = host.theater;
    host.c.frame(&Frame::tcp(HOST, port, 18215, 3, conn, b"").bytes());
    server.line(|l| l.contains("closed: its host left"));
    guest.theater(b"GLST", &[("LID", "257"), ("TYPE", "G"), ("COUNT", "-1")]);
    assert_eq!(guest.theater_replies(1)[0].get("NUM-GAMES"), "0");
    server.line(|l| l.contains("Host hosts game"));
    server.line(|l| l.contains("Guest may join game"));
    let f = trace_file(&dir, &format!("tcp-10.77.0.16-50301-to-18215-{:08x}", 0x5b00_0000u32 | 50300), "closed:");
    assert!(f.contains("for theater 10.77.0.17:50311: EGRQ"), "{f}");
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_trace_hosts_game_records_what_a_joining_game_sends() {
    let dir = temp_dir("tracegame");
    let server = Server::start(&[("CARBON_TRACE_DIR", dir.to_str().unwrap()), ("CARBON_TRACE_GAME", "1")]);
    let mut p = Player::sign_in(&server, "Joiner", 50400);
    // With CARBON_TRACE_GAME, PlayNow sends a player to the trace host's game ...
    let s = p.play_now(0x30);
    assert_eq!((s.get("props.{resultType}"), s.get("props.{games}.0.gid")), ("JOIN", "100000"));
    // ... which lets it in at once, with 10.77.0.1:1042 as the host's address.
    p.theater(b"EGAM", &[("PORT", "1042"), ("R-INT-PORT", "1042"), ("R-INT-IP", "10.77.0.16"), ("LID", "257"), ("GID", "100000")]);
    let r = p.theater_replies(2);
    assert_eq!((r[0].kind.as_str(), r[1].kind.as_str()), ("EGAM", "EGEG"));
    assert_eq!((r[1].get("I"), r[1].get("P"), r[1].get("PID"), r[1].get("GID")), ("10.77.0.1", "1042", "2", "100000"));
    // The game's GameManager then talks to its "host": recorded, not answered.
    p.c.frame(&Frame::udp(HOST, 1042, 1042, b"\x00\x00tunnel hello").bytes());
    server.line(|l| l.starts_with("trace: udp 10.77.0.16:1042 > 10.77.0.1:1042, 14 bytes"));
    let f = trace_file(&dir, "udp-10.77.0.16-1042-to-1042", "datagram");
    assert!(f.contains("datagram 14 bytes, not answered"), "{f}");
    let _ = std::fs::remove_dir_all(&dir);
}

/// RC4 as the game's tunnel uses it (one key-schedule pass), for the test's own datagrams.
struct Rc4 {
    s: [u8; 256],
    i: u8,
    j: u8,
}

impl Rc4 {
    fn new(key: &[u8]) -> Rc4 {
        let mut s = [0u8; 256];
        for (i, v) in s.iter_mut().enumerate() {
            *v = i as u8;
        }
        let mut j = 0u8;
        for i in 0..256 {
            j = j.wrapping_add(s[i]).wrapping_add(key[i % key.len()]);
            s.swap(i, j as usize);
        }
        Rc4 { s, i: 0, j: 0 }
    }

    fn apply(&mut self, data: &mut [u8]) {
        for b in data {
            self.i = self.i.wrapping_add(1);
            self.j = self.j.wrapping_add(self.s[self.i as usize]);
            self.s.swap(self.i as usize, self.j as usize);
            *b ^= self.s[(self.s[self.i as usize].wrapping_add(self.s[self.j as usize])) as usize];
        }
    }
}

#[test]
fn the_server_hosts_a_custom_match_game() {
    let server = Server::start(&[("CARBON_HOST_GAME", "1")]);
    let mut p = Player::sign_in(&server, "Racer", 50500);
    // With CARBON_HOST_GAME, PlayNow sends the player to a game the server hosts ...
    let s = p.play_now(0x40);
    assert_eq!((s.get("props.{resultType}"), s.get("props.{games}.0.gid")), ("JOIN", "200000"));
    p.theater(b"EGAM", &[("PORT", "1042"), ("R-INT-PORT", "1042"), ("R-INT-IP", "10.77.0.16"), ("LID", "257"), ("GID", "200000")]);
    let r = p.theater_replies(2);
    assert_eq!((r[0].kind.as_str(), r[1].kind.as_str()), ("EGAM", "EGEG"));
    // ... at 10.77.0.1, UDP port 19000, with its own tunnel key.
    assert_eq!((r[1].get("I"), r[1].get("P"), r[1].get("PID"), r[1].get("GID")), ("10.77.0.1", "19000", "2", "200000"));
    let ekey = r[1].get("EKEY").to_string();
    assert!(ekey.ends_with("%3d%3d") && ekey.len() == 28, "{ekey}");
    server.line(|l| l.contains("Racer joins the race host's game 200000 at 10.77.0.1:19000"));

    // The game's first datagram, as its ProtoTunnel sends it: stream position 0, two headers
    // [len >> 4, (len & 15) << 4 | port index], then the data, all encrypted with the first 16
    // characters of the EKEY: the control record (index 7: 0, client id, "I hear you" 0) and
    // CommUDP's INIT on GameManager's port (index 1: seq 1, ack = the connection id).
    let key = &ekey.as_bytes()[..16];
    let control = [0u8, 0x12, 0x34, 0x56, 0x78, 0];
    let mut init = 1u32.to_be_bytes().to_vec();
    init.extend_from_slice(&0x0bad_cafeu32.to_be_bytes());
    let mut d = vec![0u8, 0, 0, (6 << 4) | 7, 0, (8 << 4) | 1];
    d.extend_from_slice(&control);
    d.extend_from_slice(&init);
    Rc4::new(key).apply(&mut d[2..]);
    p.c.frame(&Frame::udp(HOST, 1042, 19000, &d).bytes());

    // The host answers from its game port: its control record (the HUID's client id, and it
    // hears us) and CommUDP CONN with the same connection id.
    let f = p.c.read_frame();
    assert_eq!((f.kind, f.src, f.sport, f.dst, f.dport), (3, HOST, 19000, p.vip, 1042), "{f:?}");
    let mut b = f.data.clone();
    assert_eq!(&b[..2], &[0, 0], "the host's stream starts at 0");
    Rc4::new(key).apply(&mut b[2..]);
    let heads: Vec<(u8, usize)> = b[2..6].chunks(2).map(|h| (h[1] & 15, ((h[0] as usize) << 4) | (h[1] >> 4) as usize)).collect();
    assert_eq!(heads, vec![(7, 6), (1, 8)], "{b:02x?}");
    assert_eq!((b[6], b[11]), (0, 1), "control record: {b:02x?}");
    assert_eq!(&b[12..20], &[0, 0, 0, 2, 0x0b, 0xad, 0xca, 0xfe], "CONN: {b:02x?}");
    server.line(|l| l.contains("host 200000: 10.77.0.16:1042: tunnel from client 12345678 with the game's key"));
    server.line(|l| l.contains("CommUDP INIT (connection 0badcafe): answered CONN"));
}

/// A page through the switch that says hello with its player key (lan.js), then FESL Login:
/// the client, its vip and Login's answer.
fn keyed_login(server: &Server, name: &str, cport: u16, pk: &str) -> (Client, u32, Reply) {
    let (mut c, vip) = Client::lan(server);
    c.send(1, format!(r#"{{"t":"hello","name":"{name}","pk":"{pk}"}}"#).as_bytes());
    let conn = 0x5c00_0000 | cport as u32;
    c.frame(&Frame::tcp(HOST, cport, 18210, 1, conn, b"").bytes());
    assert_eq!(c.read_frame().op, 2);
    let login = [("TXN", "Login"), ("name", name), ("password", "lan"), ("macAddr", "$0a4d00100000")];
    c.frame(&Frame::tcp(HOST, cport, 18210, 4, conn, &aries(b"acct", 0x8000_0002, &login)).bytes());
    let r = replies(&mut c, 18210, 1).remove(0);
    (c, vip, r)
}

/// Messenger on TCP 13505 for a logged-in player: AUTH with its login key.
fn messenger_auth(c: &mut Client, cport: u16, lkey: &str) -> u32 {
    let conn = 0x5d00_0000 | cport as u32;
    c.frame(&Frame::tcp(HOST, cport, 13505, 1, conn, b"").bytes());
    assert_eq!(c.read_frame().op, 2);
    let auth = [("LKEY", lkey), ("PROD", "America"), ("VERS", "2.0"), ("PRES", "nfs-pc"), ("RSRC", "/EAGAMES/NFS-2007"), ("ID", "1")];
    c.frame(&Frame::tcp(HOST, cport, 13505, 4, conn, &aries(b"AUTH", 0, &auth)).bytes());
    let r = replies(c, 13505, 1);
    assert_eq!(r[0].kind, "AUTH");
    conn
}

fn messenger_send(c: &mut Client, cport: u16, conn: u32, kind: &[u8; 4], lines: &[(&str, &str)]) {
    c.frame(&Frame::tcp(HOST, cport, 13505, 4, conn, &aries(kind, 0, lines)).bytes());
}

#[test]
fn messenger_friends_chat_presence_and_name_claims() {
    let dir = temp_dir("messenger");
    let env = [("CARBON_MESSENGER_DIR", dir.to_str().unwrap())];
    let server = Server::start(&env);
    let (mut metin, _, r) = keyed_login(&server, "Metin", 50600, "metin-browser-key-0001");
    let metin_lkey = r.get("lkey").to_string();
    let (mut jan, _, r) = keyed_login(&server, "Jan", 50610, "jan-browser-key-000001");
    let jan_lkey = r.get("lkey").to_string();
    let mc = messenger_auth(&mut metin, 50601, &metin_lkey);
    let jc = messenger_auth(&mut jan, 50611, &jan_lkey);

    // Metin asks Jan to be friends: Metin sees it sent (ST), Jan sees a request (RT).
    messenger_send(&mut metin, 50601, mc, b"RADM", &[("USER", "Jan"), ("LRSC", "eagames"), ("PRES", "Y"), ("ID", "7")]);
    let r = replies(&mut metin, 13505, 2);
    assert_eq!((r[0].kind.as_str(), r[0].get("ID"), r[1].kind.as_str(), r[1].get("ATTR")), ("RADM", "7", "RNOT", "ST"));
    let r = replies(&mut jan, 13505, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("ATTR"), r[0].get("CHNG"), r[0].get("USER")), ("RNOT", "RT", "A", "Metin@messaging.ea.com"));
    // Jan accepts: both see a friend (AT).
    messenger_send(&mut jan, 50611, jc, b"RRSP", &[("USER", "Metin"), ("ANSW", "Y"), ("ID", "3")]);
    let r = replies(&mut jan, 13505, 2);
    assert_eq!((r[0].kind.as_str(), r[1].kind.as_str(), r[1].get("ATTR")), ("RRSP", "RNOT", "AT"));
    let r = replies(&mut metin, 13505, 1);
    assert_eq!((r[0].get("ATTR"), r[0].get("CHNG"), r[0].get("USER")), ("AT", "X", "Jan@messaging.ea.com"));
    // Presence: Jan in a game room reaches Metin, who watches it (PADD).
    messenger_send(&mut metin, 50601, mc, b"PADD", &[("USER", "Jan"), ("ID", "8")]);
    let r = replies(&mut metin, 13505, 2);
    assert_eq!((r[0].kind.as_str(), r[1].kind.as_str(), r[1].get("SHOW")), ("PADD", "PGET", "DISC"));
    messenger_send(&mut jan, 50611, jc, b"PSET", &[("RSRC", "/eagames/NFS-2007"), ("SHOW", "GAME"), ("ATTR", "J"), ("ID", "5")]);
    assert_eq!(replies(&mut jan, 13505, 1)[0].kind, "PSET");
    let r = replies(&mut metin, 13505, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("SHOW"), r[0].get("ATTR"), r[0].get("CHNG")), ("PGET", "GAME", "J", "1"));
    // Chat goes to Jan, and stays out of the server's output.
    messenger_send(&mut metin, 50601, mc, b"SEND", &[("TYPE", "C"), ("USER", "Jan"), ("SUBJ", ""), ("BODY", "\"see you at the docks\""), ("SECS", "3600"), ("ID", "9")]);
    assert_eq!(replies(&mut metin, 13505, 1)[0].kind, "SEND");
    let r = replies(&mut jan, 13505, 1);
    assert_eq!((r[0].kind.as_str(), r[0].get("TYPE"), r[0].get("BODY")), ("RECV", "C", "\"see you at the docks\""));
    server.line(|l| l.contains("RECV") && l.contains("BODY=(private)"));
    assert!(!server.lines.lock().unwrap().iter().any(|l| l.contains("docks")), "chat is never printed");

    // Another browser cannot sign in as Metin: Login answers as for a wrong password.
    let (_other, _, r) = keyed_login(&server, "metin", 50620, "someone-elses-key-0001");
    assert_eq!((r.get("TXN"), r.get("errorCode")), ("Login", "122"));
    // A page from before player keys may use a free name, but not a claimed one.
    let (_old, _, r) = keyed_login(&server, "Oldpage", 50630, "");
    assert!(!r.get("lkey").is_empty());
    assert!(!server.lines.lock().unwrap().iter().any(|l| l.contains("metin-browser-key")), "keys are never printed");

    // After a restart (the store is in CARBON_MESSENGER_DIR) Jan's roster still has Metin.
    drop(server);
    let server = Server::start(&env);
    let (mut jan, _, r) = keyed_login(&server, "Jan", 50640, "jan-browser-key-000001");
    let jan_lkey = r.get("lkey").to_string();
    let jc = messenger_auth(&mut jan, 50641, &jan_lkey);
    messenger_send(&mut jan, 50641, jc, b"RGET", &[("LIST", "B"), ("LRSC", "eagames"), ("PRES", "Y"), ("PEND", "Y"), ("ID", "2")]);
    let r = replies(&mut jan, 13505, 2);
    assert_eq!((r[0].get("SIZE"), r[1].kind.as_str(), r[1].get("ATTR"), r[1].get("USER")), ("1", "ROST", "AT", "Metin@messaging.ea.com"));
    let (_, _, r) = keyed_login(&server, "Metin", 50650, "someone-elses-key-0001");
    assert_eq!(r.get("errorCode"), "122", "the claim survives the restart");
    let _ = std::fs::remove_dir_all(&dir);
}

/// A FESL rank transaction from a logged-in page (FESL connection `conn` from `cport`), and the
/// answer.
fn rank(c: &mut Client, cport: u16, id: u32, lines: &[(&str, &str)]) -> Reply {
    let conn = 0x5c00_0000 | cport as u32;
    c.frame(&Frame::tcp(HOST, cport, 18210, 4, conn, &aries(b"rank", 0x8000_0000 | id, lines)).bytes());
    let r = replies(c, 18210, 1).remove(0);
    assert_eq!((r.kind.as_str(), r.id), ("rank", 0x8000_0000 | id));
    r
}

#[test]
fn player_stats_and_the_leaderboard_are_kept_and_ranked() {
    let dir = temp_dir("stats");
    let env = [("CARBON_STATS_DIR", dir.to_str().unwrap())];
    let server = Server::start(&env);
    let (mut metin, _, r) = keyed_login(&server, "Metin", 50700, "metin-browser-key-0001");
    let metin_id = r.get("userId").to_string();
    let (mut jan, _, r) = keyed_login(&server, "Jan", 50710, "jan-browser-key-000001");
    let jan_id = r.get("userId").to_string();
    // Each game reports its own best on a race (as 008222f0 writes them; ut 2 = keep the
    // lowest) and a career stat at sign-in, which is not kept.
    let best = |id: &str, time: &str, text: &str| -> Vec<(String, String)> {
        [("TXN", "UpdateStats"), ("u.0.o", id), ("u.0.ot", "1"), ("u.0.s.0.ut", "2"), ("u.0.s.0.k", "Evt_Bst_30F07253"), ("u.0.s.0.v", time), ("u.0.s.0.t", text),
         ("u.0.s.1.ut", "0"), ("u.0.s.1.k", "Total_Cash"), ("u.0.s.1.v", "15000.0000"), ("u.0.s.1.t", ""), ("u.0.s.[]", "2"), ("u.[]", "1")]
            .iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    };
    fn lines(v: &[(String, String)]) -> Vec<(&str, &str)> {
        v.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect()
    }
    let m = best(&metin_id, "103.0500", "0x8774322C,51.250000,72.917000,0x00000000");
    assert_eq!(rank(&mut metin, 50700, 5, &lines(&m)).get("TXN"), "UpdateStats");
    let j = best(&jan_id, "101.5000", "0x7B2CC40B,50.000000,75.000000,0x00000000");
    rank(&mut jan, 50710, 5, &lines(&j));
    server.line(|l| l.contains("Metin: 1 stat(s) kept, 1 left out"));
    // A player cannot write another's stats.
    let fake = best(&jan_id, "1.0000", "");
    rank(&mut metin, 50700, 6, &lines(&fake));

    // The Leaderboard, as the game asks for it: the player's rank, the rows from it, the top.
    let keys = [("keys.0", "Evt_Bst_30F07253"), ("keys.1", "Total_Rnkd_Evt_Games_30F07253"), ("keys.[]", "2")];
    let mut q = vec![("TXN", "GetRankedStats"), ("owner", metin_id.as_str()), ("ownerType", "1"), ("periodId", "2"), ("periodPast", "0")];
    q.extend(keys);
    let r = rank(&mut metin, 50700, 7, &q);
    assert_eq!((r.get("stats.[]"), r.get("stats.0.value"), r.get("stats.0.rank"), r.get("stats.0.text")), ("2", "103.05", "2", "0x8774322C,51.250000,72.917000,0x00000000"));
    assert_eq!((r.get("stats.1.value"), r.get("stats.1.rank")), ("0.0", "0"));
    let r = rank(&mut metin, 50700, 8, &[("TXN", "GetTopN"), ("key", "Evt_Bst_30F07253"), ("ownerType", "1"), ("minRank", "1"), ("maxRank", "12"), ("periodId", "0"), ("periodPast", "0")]);
    assert_eq!((r.get("stats.[]"), r.get("stats.0.name"), r.get("stats.0.rank"), r.get("stats.0.value")), ("2", "Jan", "1", "101.5"));
    assert_eq!((r.get("stats.1.name"), r.get("stats.1.owner"), r.get("stats.1.rank")), ("Metin", metin_id.as_str(), "2"));
    // Last week's board is empty; friends (GetRankedStatsForOwners) get their ranks.
    let r = rank(&mut metin, 50700, 9, &[("TXN", "GetTopN"), ("key", "Evt_Bst_30F07253"), ("minRank", "1"), ("maxRank", "12"), ("periodId", "2"), ("periodPast", "1")]);
    assert_eq!(r.get("stats.[]"), "0");
    let mut q = vec![("TXN", "GetRankedStatsForOwners"), ("owners.[]", "2"), ("owners.0.ownerId", metin_id.as_str()), ("owners.0.ownerType", "1"), ("owners.1.ownerId", jan_id.as_str()), ("owners.1.ownerType", "1"), ("periodId", "2")];
    q.extend(keys);
    let r = rank(&mut metin, 50700, 10, &q);
    assert_eq!((r.get("rankedStats.[]"), r.get("rankedStats.1.ownerId"), r.get("rankedStats.1.rankedStats.0.rank")), ("2", jan_id.as_str(), "1"));
    // Player Stats: what the game never sends is 0 until a race counts it; Total_Cash was not kept.
    let r = rank(&mut jan, 50710, 11, &[("TXN", "GetStats"), ("owner", jan_id.as_str()), ("ownerType", "1"), ("periodId", "0"), ("periodPast", "0"), ("keys.0", "Overall_Solo_On_Wins"), ("keys.1", "Total_Cash"), ("keys.[]", "2")]);
    assert_eq!((r.get("stats.0.value"), r.get("stats.1.value")), ("0.0", "0.0"));

    // After a restart (the store is in CARBON_STATS_DIR) the board is the same.
    drop(server);
    let server = Server::start(&env);
    let (mut metin, _, _) = keyed_login(&server, "Metin", 50720, "metin-browser-key-0001");
    let r = rank(&mut metin, 50720, 3, &[("TXN", "GetTopNAndStats"), ("key", "Evt_Bst_30F07253"), ("minRank", "1"), ("maxRank", "2"), ("periodId", "2"), ("periodPast", "0"), ("keys.0", "Evt_Bst_30F07253"), ("keys.[]", "1")]);
    assert_eq!((r.get("stats.[]"), r.get("stats.0.name"), r.get("stats.1.name"), r.get("stats.1.addStats.0.value")), ("2", "Jan", "Metin", "103.05"));
    let text = std::fs::read_to_string(dir.join("stats.json")).unwrap();
    assert!(!text.contains("browser-key") && !text.contains("Total_Cash"), "{text}");
    let _ = std::fs::remove_dir_all(&dir);
}
