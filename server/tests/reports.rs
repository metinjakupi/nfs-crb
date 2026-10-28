//! POST /reports through the real binary on a free port: the key, the limits, what is stored
//! (and that the sender's address is not), and the answers a browser page reads.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::Duration;

const KEY: &str = "local-reports-test";

struct Server {
    child: Child,
    port: u16,
    dir: PathBuf,
}

impl Server {
    fn start(name: &str, env: &[(&str, &str)]) -> Server {
        let dir = std::env::temp_dir().join(format!("carbon-reports-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_carbon-server"));
        cmd.args(["0", "127.0.0.1"])
            .env("CARBON_RELAY_KEY", KEY)
            .env("CARBON_REPORTS_DIR", &dir)
            .env_remove("CARBON_REPORTS_MAX_MB")
            .env_remove("CARBON_SAVES_DIR")
            .env_remove("CARBON_TRACE_DIR")
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit());
        for (k, v) in env {
            cmd.env(k, v);
        }
        let mut child = cmd.spawn().expect("start carbon-server");
        let stdout = child.stdout.take().unwrap();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                if let Some(rest) = line.strip_prefix("relay on ws://127.0.0.1:") {
                    let _ = tx.send(rest.split('/').next().unwrap().parse::<u16>().unwrap());
                }
            }
        });
        let port = rx.recv_timeout(Duration::from_secs(10)).expect("the server's first line");
        Server { child, port, dir }
    }

    /// One request on its own connection: (status, headers in lowercase, body).
    fn ask(&self, method: &str, target: &str, ip: &str, body: Option<&[u8]>) -> (u16, String, String) {
        let mut s = TcpStream::connect(("127.0.0.1", self.port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut head = format!("{method} {target} HTTP/1.1\r\nHost: test\r\nX-Real-IP: {ip}\r\nConnection: close\r\n");
        if let Some(b) = body {
            head += &format!("Content-Type: text/plain;charset=UTF-8\r\nContent-Length: {}\r\n", b.len());
        }
        head += "\r\n";
        s.write_all(head.as_bytes()).unwrap();
        if let Some(b) = body {
            let _ = s.write_all(b);
        }
        let mut out = Vec::new();
        let _ = s.read_to_end(&mut out);
        let text = String::from_utf8_lossy(&out).into_owned();
        let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
        let status = head.split(' ').nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
        (status, head.to_lowercase(), body.to_string())
    }

    fn files(&self) -> Vec<PathBuf> {
        let mut all: Vec<PathBuf> = std::fs::read_dir(&self.dir).unwrap().map(|e| e.unwrap().path()).collect();
        all.sort();
        all
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn id_of(body: &str) -> String {
    let start = body.find(r#""id":""#).expect("an id") + 6;
    body[start..].split('"').next().unwrap().to_string()
}

#[cfg(unix)]
fn mode(path: &Path) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(path).unwrap().permissions().mode() & 0o777
}

#[test]
fn reports_are_stored_without_the_sender() {
    let server = Server::start("store", &[]);
    let key = format!("/reports?key={KEY}");
    // The browser's preflight (not needed for text/plain, answered anyway) and a wrong method.
    let (status, head, _) = server.ask("OPTIONS", "/reports", "203.0.113.1", None);
    assert_eq!(status, 204);
    assert!(head.contains("access-control-allow-origin: *"), "{head}");
    assert_eq!(server.ask("GET", &key, "203.0.113.1", None).0, 405);
    // The key.
    assert_eq!(server.ask("POST", "/reports", "203.0.113.2", Some(b"x")).0, 403);
    assert_eq!(server.ask("POST", "/reports?key=nope", "203.0.113.2", Some(b"x")).0, 403);
    // A report.
    let text = "NFS Carbon in the browser: problem report\nPlayer name: Tester\n[recomp] a log line ü\n";
    let (status, head, body) = server.ask("POST", &key, "203.0.113.3", Some(text.as_bytes()));
    assert_eq!(status, 201, "{body}");
    assert!(head.contains("access-control-allow-origin: *") && head.contains("cross-origin-resource-policy: cross-origin"), "{head}");
    let id = id_of(&body);
    let files = server.files();
    assert_eq!(files.len(), 1);
    assert_eq!(files[0].file_name().unwrap().to_string_lossy(), format!("{id}.txt"));
    let stored = std::fs::read_to_string(&files[0]).unwrap();
    assert!(stored.starts_with("Report received 20"), "{stored}");
    assert!(stored.ends_with(text), "{stored}");
    assert!(!stored.contains("203.0.113") && !stored.contains("127.0.0.1"), "an address was stored: {stored}");
    #[cfg(unix)]
    assert_eq!(mode(&files[0]), 0o600);
    // Not text, empty, too big.
    assert_eq!(server.ask("POST", &key, "203.0.113.4", Some(&[0xff, 0xfe, 0x00])).0, 400);
    assert_eq!(server.ask("POST", &key, "203.0.113.5", Some(b"")).0, 413);
    let big = vec![b'a'; 512 * 1024 + 1];
    assert_eq!(server.ask("POST", &key, "203.0.113.6", Some(&big)).0, 413);
    let max = vec![b'a'; 512 * 1024];
    assert_eq!(server.ask("POST", &key, "203.0.113.6", Some(&max)).0, 201);
    assert_eq!(server.files().len(), 2);
}

#[test]
fn per_client_limit() {
    let server = Server::start("limit", &[]);
    let key = format!("/reports?key={KEY}");
    for _ in 0..3 {
        assert_eq!(server.ask("POST", &key, "198.51.100.7", Some(b"one")).0, 201);
    }
    let (status, head, _) = server.ask("POST", &key, "198.51.100.7", Some(b"four"));
    assert_eq!(status, 429);
    assert!(head.contains("retry-after:"), "{head}");
    assert!(head.contains("access-control-expose-headers: retry-after"), "a page could not read Retry-After: {head}");
    assert_eq!(server.ask("POST", &key, "198.51.100.8", Some(b"someone else")).0, 201);
    assert_eq!(server.files().len(), 4);
}

#[test]
fn total_size_limit_and_off() {
    // 0.002 MB = 2097 bytes for all reports together.
    let server = Server::start("full", &[("CARBON_REPORTS_MAX_MB", "0.002")]);
    let key = format!("/reports?key={KEY}");
    let report = vec![b'r'; 1500];
    assert_eq!(server.ask("POST", &key, "192.0.2.1", Some(&report)).0, 201);
    assert_eq!(server.ask("POST", &key, "192.0.2.2", Some(&report)).0, 507);
    assert_eq!(server.files().len(), 1);
    // Without CARBON_REPORTS_DIR: 501.
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_carbon-server"));
    cmd.args(["0", "127.0.0.1"]).env("CARBON_RELAY_KEY", KEY).env_remove("CARBON_REPORTS_DIR")
        .env_remove("CARBON_SAVES_DIR").env_remove("CARBON_TRACE_DIR").stdout(Stdio::piped()).stderr(Stdio::inherit());
    let mut child = cmd.spawn().unwrap();
    let mut out = BufReader::new(child.stdout.take().unwrap());
    let mut first = String::new();
    out.read_line(&mut first).unwrap();
    // Keep draining: the server prints more lines, and a closed pipe would kill it.
    std::thread::spawn(move || std::io::copy(&mut out, &mut std::io::sink()));
    let port: u16 = first.trim().strip_prefix("relay on ws://127.0.0.1:").unwrap().split('/').next().unwrap().parse().unwrap();
    let off = Server { child, port, dir: std::env::temp_dir().join("carbon-reports-none") };
    assert_eq!(off.ask("POST", &key, "192.0.2.3", Some(b"x")).0, 501);
}
