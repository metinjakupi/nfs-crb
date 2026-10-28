//! Problem reports (only with CARBON_REPORTS_DIR set; otherwise /reports answers 501). The
//! game page's "Send report" (source/web/report.js) posts the page's runtime log as text; the
//! user reads the files over ssh (tools/carbon-reports.sh list|get).
//!
//! POST /reports?key=<CARBON_RELAY_KEY>, a UTF-8 text body of 1 byte to 512 KiB
//! (Content-Length required; text/plain keeps it a simple CORS request, without a preflight).
//! Stored as <dir>/<UTC yyyymmdd-hhmmss>-<6 hex>.txt, mode 0600: a first line with the time it
//! arrived, then the body as sent. Never the sender's address: the per-client limit keeps
//! addresses in memory only and forgets quiet ones within minutes. Answer 201
//! {"id":"<name without .txt>"}. Limits: 3 at once per client, then one per 10 minutes (429
//! with Retry-After); CARBON_REPORTS_MAX_MB for all reports together (default 200; 507 when
//! full); 413 over 512 KiB; 400 for a body that is not UTF-8 text. OPTIONS answers a preflight.
//!
//! Rust server only: the Node relay (tools/carbon-net-relay.mjs) runs on the developer's Mac,
//! where the page's own log is at hand, and has no /reports (it answers like any other path).

use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use tokio::io::AsyncWriteExt;

use crate::http::{client, Conn, Request, Url};
use crate::util::{file_stamp, iso_date, lock, same_bytes};
use crate::App;

pub const REPORT_MAX: u64 = 512 * 1024;
/// Per client IP: 3 at once, then one per 10 minutes.
const LIMIT_BURST: f64 = 3.0;
const LIMIT_RATE: f64 = 1.0 / 600.0;
/// Every answer carries these (a page on another origin may read Retry-After only if exposed).
const HEADERS: [(&str, &str); 4] = [
    ("access-control-allow-origin", "*"),
    ("access-control-expose-headers", "Retry-After"),
    ("cross-origin-resource-policy", "cross-origin"),
    ("cache-control", "no-store"),
];

struct Bucket {
    tokens: f64,
    at: Instant,
}

struct State {
    clients: HashMap<String, Bucket>,
    swept: Instant,
    /// bytes of all stored reports
    used: f64,
}

pub struct Reports {
    pub dir: PathBuf,
    total: f64,
    st: Mutex<State>,
}

/// The size limit for all reports: CARBON_REPORTS_MAX_MB, 200 when unset, empty, 0 or not a
/// positive number.
pub fn total_from(env: Option<&str>) -> f64 {
    match env.map(str::trim).and_then(|s| s.parse::<f64>().ok()) {
        Some(mb) if mb > 0.0 && mb.is_finite() => mb * 1024.0 * 1024.0,
        _ => 200.0 * 1024.0 * 1024.0,
    }
}

/// A report's file name (without the folder): "20261025-153012-a1b2c3.txt".
fn report_name(now: SystemTime) -> String {
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let nanos = now.duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    let n = (nanos ^ SEQ.fetch_add(0x9e37_79b9_7f4a_7c15, Ordering::Relaxed).rotate_left(23)) & 0xff_ffff;
    format!("{}-{n:06x}.txt", file_stamp(now))
}

fn is_report(name: &str) -> bool {
    name.len() == 26 && name.ends_with(".txt") && name[..22].bytes().all(|b| b.is_ascii_hexdigit() || b == b'-')
}

impl Reports {
    /// Makes the folder, removes temporary files a stop in the middle left, and counts the
    /// space the reports use.
    pub fn open(dir: PathBuf, total: f64) -> io::Result<Reports> {
        fs::create_dir_all(&dir)?;
        let mut used = 0.0;
        for entry in fs::read_dir(&dir)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.ends_with(".tmp") {
                let _ = fs::remove_file(entry.path());
            } else if is_report(&name) {
                used += entry.metadata()?.len() as f64;
            }
        }
        Ok(Reports { dir, total, st: Mutex::new(State { clients: HashMap::new(), swept: Instant::now(), used }) })
    }

    pub fn used_mb(&self) -> f64 {
        lock(&self.st).used / 1_048_576.0
    }

    /// 0 when the client may send, else the seconds to wait.
    fn wait(&self, who: &str) -> u64 {
        let mut st = lock(&self.st);
        let now = Instant::now();
        let refill = |b: &Bucket| b.tokens + now.duration_since(b.at).as_secs_f64() * LIMIT_RATE;
        if now.duration_since(st.swept) > Duration::from_secs(60) {
            st.swept = now;
            st.clients.retain(|_, b| refill(b) < LIMIT_BURST);
        }
        let b = st.clients.entry(who.to_string()).or_insert(Bucket { tokens: LIMIT_BURST, at: now });
        b.tokens = refill(b).min(LIMIT_BURST);
        b.at = now;
        if b.tokens < 1.0 {
            return ((1.0 - b.tokens) / LIMIT_RATE).ceil() as u64;
        }
        b.tokens -= 1.0;
        0
    }
}

async fn say(conn: &mut Conn, req: &Request, status: u16, text: &str, extra: &[(&str, &str)]) -> io::Result<()> {
    let mut headers = HEADERS.to_vec();
    headers.push(("content-type", "text/plain; charset=utf-8"));
    headers.extend_from_slice(extra);
    conn.reply(req, status, &headers, format!("{text}\n").as_bytes()).await
}

pub async fn handle(app: &App, conn: &mut Conn, req: &mut Request, url: &Url) -> io::Result<()> {
    if req.method == "OPTIONS" {
        let mut headers = HEADERS.to_vec();
        headers.extend_from_slice(&[
            ("access-control-allow-methods", "POST, OPTIONS"),
            ("access-control-allow-headers", "Content-Type"),
            ("access-control-max-age", "600"),
        ]);
        return conn.reply(req, 204, &headers, b"").await;
    }
    let Some(reports) = &app.reports else {
        return say(conn, req, 501, "Reports are not turned on on this server.", &[]).await;
    };
    if req.method != "POST" {
        return say(conn, req, 405, "Send a report with POST.", &[("allow", "POST, OPTIONS")]).await;
    }
    let wait = reports.wait(&client(conn, req));
    if wait > 0 {
        req.keep = false;
        return say(conn, req, 429, "A report was sent from here a moment ago. Try again later.", &[("retry-after", &wait.to_string())]).await;
    }
    let given = url.query("key").unwrap_or_default();
    if !app.key.is_empty() && !same_bytes(given.as_bytes(), app.key.as_bytes()) {
        req.keep = false;
        return say(conn, req, 403, "Wrong server key.", &[]).await;
    }
    let Some(length) = req.length else {
        return say(conn, req, 411, "Content-Length is required.", &[]).await;
    };
    if !(1..=REPORT_MAX).contains(&length) {
        req.keep = false;
        return say(conn, req, 413, &format!("A report is 1 to {REPORT_MAX} bytes."), &[]).await;
    }
    if lock(&reports.st).used + length as f64 > reports.total {
        req.keep = false;
        return say(conn, req, 507, "The server has no room for more reports.", &[]).await;
    }
    let mut body = Vec::with_capacity(length as usize);
    while req.left > 0 {
        let Ok(n) = conn.body_ready(req).await else {
            req.keep = false;
            return say(conn, req, 400, "The report did not arrive whole.", &[]).await;
        };
        body.extend_from_slice(&conn.buf[..n]);
        conn.consume(req, n);
    }
    let Ok(text) = std::str::from_utf8(&body) else {
        return say(conn, req, 400, "A report is UTF-8 text.", &[]).await;
    };
    let now = SystemTime::now();
    let content = format!("Report received {}\n\n{text}", iso_date(now));
    match store(reports, now, content.as_bytes()).await {
        Ok(name) => {
            lock(&reports.st).used += content.len() as f64;
            let id = name.trim_end_matches(".txt");
            let mut headers = HEADERS.to_vec();
            headers.push(("content-type", "application/json"));
            conn.reply(req, 201, &headers, format!(r#"{{"id":"{id}"}}"#).as_bytes()).await
        }
        Err(e) => {
            eprintln!("reports: {e}");
            say(conn, req, 500, "The server could not store this report.", &[]).await
        }
    }
}

/// Written to a temporary file, synced, then renamed: the folder only ever has whole reports.
async fn store(reports: &Reports, now: SystemTime, bytes: &[u8]) -> io::Result<String> {
    for _ in 0..8 {
        let name = report_name(now);
        let path = reports.dir.join(&name);
        if tokio::fs::try_exists(&path).await? {
            continue;
        }
        let tmp = reports.dir.join(format!(".{name}.tmp"));
        let mut out = tokio::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp).await?;
        let written = async {
            out.write_all(bytes).await?;
            out.flush().await?;
            out.sync_all().await
        }
        .await;
        drop(out);
        if let Err(e) = written {
            let _ = tokio::fs::remove_file(&tmp).await;
            return Err(e);
        }
        if let Err(e) = tokio::fs::rename(&tmp, &path).await {
            let _ = tokio::fs::remove_file(&tmp).await;
            return Err(e);
        }
        return Ok(name);
    }
    Err(io::Error::other("no free report name"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        let t = SystemTime::UNIX_EPOCH + Duration::from_secs(1_792_934_412);
        let a = report_name(t);
        let b = report_name(t);
        assert!(is_report(&a), "{a}");
        assert_ne!(a, b);
        assert!(a.starts_with("20261025-"), "{a}");
        assert!(!is_report(".x.tmp") && !is_report("20261025-153012-a1b2c3.json"));
    }

    #[test]
    fn totals() {
        assert_eq!(total_from(None), 200.0 * 1048576.0);
        assert_eq!(total_from(Some("")), 200.0 * 1048576.0);
        assert_eq!(total_from(Some("0")), 200.0 * 1048576.0);
        assert_eq!(total_from(Some("-3")), 200.0 * 1048576.0);
        assert_eq!(total_from(Some(" 5 ")), 5.0 * 1048576.0);
    }

    #[test]
    fn limit() {
        let dir = std::env::temp_dir().join(format!("carbon-reports-unit-{}", std::process::id()));
        let r = Reports::open(dir.clone(), total_from(None)).unwrap();
        assert_eq!((r.wait("a"), r.wait("a"), r.wait("a")), (0, 0, 0));
        assert!(r.wait("a") > 500);
        assert_eq!(r.wait("b"), 0);
        let _ = fs::remove_dir_all(dir);
    }
}
