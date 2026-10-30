//! Cloud saves (only with CARBON_SAVES_DIR set; otherwise /saves/* answers 501). The start
//! page (source/web/saves.js) seals a save in the browser with a keyphrase the server never
//! sees and stores the sealed bytes under an id it derives from that keyphrase separately; the
//! server keeps opaque blobs only. The routes, status codes and headers are the Node relay's
//! (see its header): GET, HEAD, PUT, DELETE and OPTIONS on /saves/<64 lowercase hex>, ETag =
//! the blob's SHA-256, If-Match / If-None-Match, 1 byte to 8 MiB per save, CARBON_SAVES_MAX_MB
//! for all of them, a per-client rate limit, and writes that a reader never sees half done.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{self, Read, Seek};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime};

use sha2::{Digest, Sha256};
use tokio::io::AsyncWriteExt;

use crate::http::{client, Conn, Request, Url};
use crate::util::{hex, http_date, iso_date, json_str, lock, same_bytes};
use crate::App;

pub const SAVES_MAX: u64 = 8 * 1024 * 1024;
/// Per client IP, all /saves/ requests together: 20 at once, then one per 3 s.
const LIMIT_BURST: f64 = 20.0;
const LIMIT_RATE: f64 = 1.0 / 3.0;
/// Every answer carries these.
const HEADERS: [(&str, &str); 4] = [
    ("access-control-allow-origin", "*"),
    ("access-control-expose-headers", "ETag"),
    ("cross-origin-resource-policy", "cross-origin"),
    ("cache-control", "no-store"),
];
const CHUNK: usize = 64 * 1024;

#[derive(Clone)]
struct Info {
    /// quoted, as sent: "\"<sha-256 hex>\""
    etag: String,
    mtime: SystemTime,
    size: u64,
}

struct Bucket {
    tokens: f64,
    at: Instant,
}

struct State {
    clients: HashMap<String, Bucket>,
    swept: Instant,
    /// ids with a PUT or DELETE in progress
    writing: HashSet<String>,
    /// id -> what HEAD and If-Match need, so they need not hash the blob again (a cache)
    info: HashMap<String, Info>,
    /// bytes of all stored blobs
    used: f64,
}

pub struct Saves {
    pub dir: PathBuf,
    total: f64,
    st: Mutex<State>,
}

fn is_id(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// The size limit for all saves: CARBON_SAVES_MAX_MB as JavaScript's Number() reads it,
/// 1024 when unset, empty, 0 or not a number.
pub fn total_from(env: Option<&str>) -> f64 {
    let mb = env.map(str::trim).filter(|s| !s.is_empty()).and_then(|s| s.parse::<f64>().ok());
    match mb {
        Some(mb) if mb != 0.0 && !mb.is_nan() => mb * 1024.0 * 1024.0,
        _ => 1024.0 * 1024.0 * 1024.0,
    }
}

impl Saves {
    /// Makes the folder, removes temporary files a stop in the middle of an upload left
    /// (never a save), and counts the space used.
    pub fn open(dir: PathBuf, total: f64) -> io::Result<Saves> {
        fs::create_dir_all(&dir)?;
        let mut used = 0.0;
        for entry in fs::read_dir(&dir)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.ends_with(".tmp") {
                let _ = fs::remove_file(entry.path());
            } else if is_id(&name) {
                used += entry.metadata()?.len() as f64;
            }
        }
        let st = State { clients: HashMap::new(), swept: Instant::now(), writing: HashSet::new(), info: HashMap::new(), used };
        Ok(Saves { dir, total, st: Mutex::new(st) })
    }

    pub fn used_mb(&self) -> f64 {
        lock(&self.st).used / 1_048_576.0
    }

    /// Whether an opaque cloud-save id exists. Name claims use this without reading or
    /// changing the sealed save.
    pub async fn exists(&self, id: &str) -> io::Result<bool> {
        if !is_id(id) {
            return Ok(false);
        }
        self.stat(id).await.map(|info| info.is_some())
    }

    /// 0 when the request may go ahead, else the seconds to wait.
    fn wait(&self, who: &str) -> u64 {
        let mut st = lock(&self.st);
        let now = Instant::now();
        let refill = |b: &Bucket| b.tokens + now.duration_since(b.at).as_secs_f64() * LIMIT_RATE;
        if now.duration_since(st.swept) > Duration::from_secs(60) {
            // forget clients that have been quiet long enough
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

    /// One change to an id at a time: None while another is in progress.
    fn begin(&self, id: &str) -> Option<Writing<'_>> {
        lock(&self.st).writing.insert(id.to_string()).then(|| Writing { saves: self, id: id.to_string() })
    }

    fn remember(&self, id: &str, info: Info) {
        let mut st = lock(&self.st);
        if st.info.len() >= 1000 {
            st.info.clear(); // a cache, not a record
        }
        st.info.insert(id.to_string(), info);
    }

    /// The stored blob's {etag, mtime, size}, or None.
    async fn stat(&self, id: &str) -> io::Result<Option<Info>> {
        let path = self.dir.join(id);
        let known = lock(&self.st).info.get(id).cloned();
        let (info, fresh) = blocking(move || {
            let Ok(meta) = fs::metadata(&path) else { return Ok((None, false)) };
            let mtime = meta.modified()?;
            if let Some(k) = known.filter(|k| k.mtime == mtime && k.size == meta.len()) {
                return Ok((Some(k), false));
            }
            let etag = hash_file(&mut fs::File::open(&path)?)?;
            Ok((Some(Info { etag, mtime, size: meta.len() }), true))
        })
        .await?;
        if let (Some(info), true) = (&info, fresh) {
            self.remember(id, info.clone());
        }
        Ok(info)
    }
}

/// Holds an id's write lock; dropping it lets the next change in.
struct Writing<'a> {
    saves: &'a Saves,
    id: String,
}

impl Drop for Writing<'_> {
    fn drop(&mut self) {
        lock(&self.saves.st).writing.remove(&self.id);
    }
}

/// A temporary upload file that is removed unless it became the save.
struct TempFile(Option<PathBuf>);

impl Drop for TempFile {
    fn drop(&mut self) {
        if let Some(path) = self.0.take() {
            let _ = fs::remove_file(path);
        }
    }
}

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> io::Result<T> + Send + 'static) -> io::Result<T> {
    tokio::task::spawn_blocking(f).await.map_err(io::Error::other)?
}

/// "\"<sha-256 hex>\"" of a file, read from where it is.
fn hash_file(file: &mut fs::File) -> io::Result<String> {
    let mut sha = Sha256::new();
    let mut buf = vec![0u8; CHUNK];
    loop {
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        sha.update(&buf[..n]);
    }
    Ok(format!("\"{}\"", hex(&sha.finalize())))
}

/// If-Match / If-None-Match: "*" or a list of quoted etags; never true without a blob.
fn matches(header: &str, info: Option<&Info>) -> bool {
    info.is_some_and(|info| header.split(',').map(str::trim).any(|t| t == "*" || t == info.etag))
}

fn temp_name(id: &str) -> String {
    static SEQ: AtomicU64 = AtomicU64::new(0);
    let nanos = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    let n = (nanos ^ SEQ.fetch_add(0x9e37_79b9_7f4a_7c15, Ordering::Relaxed).rotate_left(17)) & 0xffff_ffff_ffff;
    format!(".{id}.{n:012x}.tmp")
}

/// A short plain-text answer.
async fn say(conn: &mut Conn, req: &Request, status: u16, text: &str, extra: &[(&str, &str)]) -> io::Result<()> {
    let mut headers = HEADERS.to_vec();
    headers.push(("content-type", "text/plain; charset=utf-8"));
    headers.extend_from_slice(extra);
    conn.reply(req, status, &headers, format!("{text}\n").as_bytes()).await
}

async fn internal(conn: &mut Conn, req: &mut Request, error: io::Error) -> io::Result<()> {
    eprintln!("saves: {error}");
    say(conn, req, 500, "The server could not handle this save.", &[]).await
}

pub async fn handle(app: &App, conn: &mut Conn, req: &mut Request, url: &Url) -> io::Result<()> {
    if req.method == "OPTIONS" {
        // The browser's preflight for a PUT with a binary body, or a DELETE.
        let mut headers = HEADERS.to_vec();
        headers.extend_from_slice(&[
            ("access-control-allow-methods", "GET, HEAD, PUT, DELETE, OPTIONS"),
            ("access-control-allow-headers", "Content-Type, If-Match, If-None-Match"),
            ("access-control-allow-private-network", "true"),
            ("access-control-max-age", "600"),
        ]);
        return conn.reply(req, 204, &headers, b"").await;
    }
    let Some(saves) = &app.saves else {
        return say(conn, req, 501, "Cloud saves are not turned on on this server.", &[]).await;
    };
    let wait = saves.wait(&client(conn, req));
    if wait > 0 {
        return say(conn, req, 429, "Too many requests. Try again shortly.", &[("retry-after", &wait.to_string())]).await;
    }
    let given = url.query("key").unwrap_or_default();
    if !app.key.is_empty() && !same_bytes(given.as_bytes(), app.key.as_bytes()) {
        return say(conn, req, 403, "Wrong server key.", &[]).await;
    }
    let id = url.path.strip_prefix("/saves/").unwrap_or("");
    if !is_id(id) {
        return say(conn, req, 400, "A save id is 64 lowercase hex characters.", &[]).await;
    }
    match req.method.as_str() {
        "PUT" => put(saves, conn, req, id).await,
        "DELETE" => delete(saves, conn, req, id).await,
        "GET" => get(saves, conn, req, id).await,
        "HEAD" => match saves.stat(id).await {
            Ok(Some(info)) => {
                let modified = http_date(info.mtime);
                conn.send_head(req, 200, &blob_headers(&info.etag, &modified), Some(info.size)).await
            }
            Ok(None) => say(conn, req, 404, "No save with this id.", &[]).await,
            Err(e) => internal(conn, req, e).await,
        },
        _ => say(conn, req, 405, "Use GET, HEAD, PUT or DELETE.", &[("allow", "GET, HEAD, PUT, DELETE, OPTIONS")]).await,
    }
}

/// GET and HEAD of a blob (send_head adds its Content-Length).
fn blob_headers<'a>(etag: &'a str, modified: &'a str) -> Vec<(&'a str, &'a str)> {
    let mut headers = HEADERS.to_vec();
    headers.extend_from_slice(&[("content-type", "application/octet-stream"), ("etag", etag), ("last-modified", modified)]);
    headers
}

async fn get(saves: &Saves, conn: &mut Conn, req: &mut Request, id: &str) -> io::Result<()> {
    // One open file for both the bytes and their etag, even if a PUT renames over it meanwhile.
    let path = saves.dir.join(id);
    let known = lock(&saves.st).info.get(id).cloned();
    let opened = blocking(move || {
        let mut file = match fs::File::open(&path) {
            Ok(f) => f,
            Err(_) => return Ok(None),
        };
        let meta = file.metadata()?;
        let mtime = meta.modified()?;
        let info = match known.filter(|k| k.mtime == mtime && k.size == meta.len()) {
            Some(k) => (k, false),
            None => {
                let etag = hash_file(&mut file)?;
                file.rewind()?;
                (Info { etag, mtime, size: meta.len() }, true)
            }
        };
        Ok(Some((file, info)))
    })
    .await;
    let (file, (info, fresh)) = match opened {
        Ok(Some(found)) => found,
        Ok(None) => return say(conn, req, 404, "No save with this id.", &[]).await,
        Err(e) => return internal(conn, req, e).await,
    };
    if fresh {
        saves.remember(id, info.clone());
    }
    let modified = http_date(info.mtime);
    conn.send_head(req, 200, &blob_headers(&info.etag, &modified), Some(info.size)).await?;
    // The bytes go out a piece at a time (a save is up to 8 MiB; the server stays small).
    let mut file = tokio::fs::File::from_std(file);
    let mut buf = vec![0u8; CHUNK];
    let mut left = info.size;
    while left > 0 {
        let want = buf.len().min(left as usize);
        let n = tokio::io::AsyncReadExt::read(&mut file, &mut buf[..want]).await?;
        if n == 0 {
            // shorter than its length said: this answer cannot be finished
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        conn.write(&buf[..n]).await?;
        left -= n as u64;
    }
    Ok(())
}

async fn put(saves: &Saves, conn: &mut Conn, req: &mut Request, id: &str) -> io::Result<()> {
    let Some(length) = req.length else {
        // (a chunked body has no length either; the connection closes after this answer)
        return say(conn, req, 411, "Content-Length is required.", &[]).await;
    };
    if !(1..=SAVES_MAX).contains(&length) {
        req.keep = false;
        return say(conn, req, 413, &format!("A save is 1 to {SAVES_MAX} bytes."), &[]).await;
    }
    let Some(_writing) = saves.begin(id) else {
        return say(conn, req, 409, "This save is being written right now. Try again.", &[]).await;
    };
    let old = match saves.stat(id).await {
        Ok(old) => old,
        Err(e) => return internal(conn, req, e).await,
    };
    let if_match = req.joined("if-match");
    let if_none = req.joined("if-none-match");
    if if_match.is_some_and(|h| !matches(&h, old.as_ref())) || if_none.is_some_and(|h| matches(&h, old.as_ref())) {
        let etag = old.as_ref().map(|o| o.etag.clone());
        let extra: Vec<(&str, &str)> = etag.iter().map(|e| ("etag", e.as_str())).collect();
        return say(conn, req, 412, "The save on the server has changed.", &extra).await;
    }
    let old_size = old.as_ref().map_or(0, |o| o.size) as f64;
    if lock(&saves.st).used - old_size + length as f64 > saves.total {
        return say(conn, req, 507, "The server has no room for more saves.", &[]).await;
    }
    let file = saves.dir.join(id);
    let tmp = saves.dir.join(temp_name(id));
    match receive(conn, req, &tmp, &file).await {
        Ok((etag, meta)) => {
            let mtime = meta.modified()?;
            lock(&saves.st).used += meta.len() as f64 - old_size;
            saves.remember(id, Info { etag: etag.clone(), mtime, size: meta.len() });
            let modified = http_date(mtime);
            let body = format!(r#"{{"etag":{},"size":{},"saved":"{}"}}"#, json_str(&etag), meta.len(), iso_date(mtime));
            let mut headers = HEADERS.to_vec();
            headers.extend_from_slice(&[("content-type", "application/json"), ("etag", &etag), ("last-modified", &modified)]);
            conn.reply(req, if old.is_some() { 200 } else { 201 }, &headers, body.as_bytes()).await
        }
        Err(Upload::Socket) => {
            req.keep = false;
            say(conn, req, 400, "The upload did not arrive whole.", &[]).await
        }
        Err(Upload::File(e)) => {
            eprintln!("saves: PUT failed: {e}");
            req.keep = req.left == 0;
            say(conn, req, 400, "The upload did not arrive whole.", &[]).await
        }
    }
}

enum Upload {
    /// the client stopped sending (cut off, or too slow): nothing to report
    Socket,
    File(io::Error),
}

/// The body into a temporary file in the same folder, synced, then renamed over the old one,
/// so a reader sees the old blob or the new one, never part of one.
async fn receive(conn: &mut Conn, req: &mut Request, tmp: &Path, file: &Path) -> Result<(String, fs::Metadata), Upload> {
    let mut out = tokio::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(tmp).await.map_err(Upload::File)?;
    let mut guard = TempFile(Some(tmp.to_path_buf()));
    let mut sha = Sha256::new();
    while req.left > 0 {
        let n = conn.body_ready(req).await.map_err(|_| Upload::Socket)?;
        sha.update(&conn.buf[..n]);
        out.write_all(&conn.buf[..n]).await.map_err(Upload::File)?;
        conn.consume(req, n);
    }
    out.flush().await.map_err(Upload::File)?;
    out.sync_all().await.map_err(Upload::File)?;
    drop(out);
    tokio::fs::rename(tmp, file).await.map_err(Upload::File)?;
    guard.0 = None;
    let meta = tokio::fs::metadata(file).await.map_err(Upload::File)?;
    Ok((format!("\"{}\"", hex(&sha.finalize())), meta))
}

/// The same rules as a PUT (key, limit, one change at a time); the space it used is free again.
async fn delete(saves: &Saves, conn: &mut Conn, req: &mut Request, id: &str) -> io::Result<()> {
    let Some(_writing) = saves.begin(id) else {
        return say(conn, req, 409, "This save is being written right now. Try again.", &[]).await;
    };
    let old = match saves.stat(id).await {
        Ok(Some(old)) => old,
        Ok(None) => return say(conn, req, 404, "No save with this id.", &[]).await,
        Err(e) => return internal(conn, req, e).await,
    };
    if req.joined("if-match").is_some_and(|h| !matches(&h, Some(&old))) {
        return say(conn, req, 412, "The save on the server has changed.", &[("etag", &old.etag)]).await;
    }
    match tokio::fs::remove_file(saves.dir.join(id)).await {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => return say(conn, req, 404, "No save with this id.", &[]).await,
        Err(e) => return internal(conn, req, e).await,
    }
    {
        let mut st = lock(&saves.st);
        st.used = (st.used - old.size as f64).max(0.0);
        st.info.remove(id);
    }
    conn.reply(req, 204, &HEADERS, b"").await
}
