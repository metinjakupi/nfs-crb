//! HTTP/1.1 for the relay's few routes, kept as node:http behaved for the Node relay:
//! request heads (httparse), bodies by Content-Length only, keep-alive (5 s idle, as Node),
//! 100-continue, and the plain answers. A request with Transfer-Encoding is answered and the
//! connection closed (only a save's PUT and a report's POST have a body worth reading, and they
//! must say its length).

use std::io;
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::{Duration, SystemTime};

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::{timeout, timeout_at};

use crate::saves::SAVES_MAX;
use crate::util::{form_decode, http_date, js_slice, lock};
use crate::App;

/// Node's limit for a request head; past it 431.
const MAX_HEAD: usize = 16 * 1024;
const MAX_HEADERS: usize = 100;
/// The first request of a connection may take a while to arrive; an idle keep-alive
/// connection closes after Node's 5 s (its answers say so, and fetch() leaves before that).
const FIRST_HEAD_WAIT: Duration = Duration::from_secs(30);
const KEEP_ALIVE: Duration = Duration::from_secs(5);
/// Any one read or write while a request is in progress.
pub const IO_WAIT: Duration = Duration::from_secs(30);
/// How long a connection being closed may still send (a body nobody read) before it goes.
const LINGER: Duration = Duration::from_secs(2);

pub struct Request {
    pub method: String,
    pub target: String,
    http10: bool,
    /// names in lowercase, values trimmed
    headers: Vec<(String, String)>,
    /// Content-Length, if given
    pub length: Option<u64>,
    /// body bytes not read yet
    pub left: u64,
    /// whether the connection stays open after the answer
    pub keep: bool,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }

    /// All values of a header joined with ", ", as node:http hands over If-Match.
    pub fn joined(&self, name: &str) -> Option<String> {
        let all: Vec<&str> = self.headers.iter().filter(|(k, _)| k == name).map(|(_, v)| v.as_str()).collect();
        if all.is_empty() { None } else { Some(all.join(", ")) }
    }

    fn has_token(&self, name: &str, token: &str) -> bool {
        self.headers
            .iter()
            .filter(|(k, _)| k == name)
            .any(|(_, v)| v.split(',').any(|t| t.trim().eq_ignore_ascii_case(token)))
    }

    /// node:http's 'upgrade' event: an Upgrade header and Connection: upgrade.
    pub fn is_upgrade(&self) -> bool {
        self.header("upgrade").is_some() && self.has_token("connection", "upgrade")
    }
}

pub struct Conn {
    pub stream: TcpStream,
    /// bytes read and not used yet (the next request, a body, or a WebSocket's first frames)
    pub buf: Vec<u8>,
    pub remote: SocketAddr,
}

/// Why no request head came.
enum HeadError {
    /// the connection closed or went quiet: nothing to answer
    Gone,
    /// answer with this status and close
    Status(u16),
}

impl Conn {
    /// Reads more bytes into `buf`, with room for at least `room`; Ok(0) at the end.
    pub async fn fill(&mut self, wait: Duration, room: usize) -> io::Result<usize> {
        if self.buf.capacity() - self.buf.len() < room / 2 {
            self.buf.reserve(room);
        }
        match timeout(wait, self.stream.read_buf(&mut self.buf)).await {
            Ok(r) => r,
            Err(_) => Err(io::ErrorKind::TimedOut.into()),
        }
    }

    async fn read_head(&mut self, wait: Duration) -> Result<Request, HeadError> {
        loop {
            if !self.buf.is_empty() {
                let mut slots = [httparse::EMPTY_HEADER; MAX_HEADERS];
                let mut parsed = httparse::Request::new(&mut slots);
                match parsed.parse(&self.buf) {
                    Ok(httparse::Status::Complete(n)) => {
                        let req = build(&parsed)?;
                        self.buf.drain(..n);
                        return Ok(req);
                    }
                    Ok(httparse::Status::Partial) if self.buf.len() > MAX_HEAD => return Err(HeadError::Status(431)),
                    Ok(httparse::Status::Partial) => {}
                    Err(httparse::Error::TooManyHeaders) => return Err(HeadError::Status(431)),
                    Err(_) => return Err(HeadError::Status(400)),
                }
            }
            match self.fill(wait, 4096).await {
                Ok(n) if n > 0 => {}
                _ => return Err(HeadError::Gone),
            }
        }
    }

    /// At least one byte of the body at the front of `buf` (at most what is left of it);
    /// an error when the body stops short or stalls.
    pub async fn body_ready(&mut self, req: &Request) -> io::Result<usize> {
        if self.buf.is_empty() && self.fill(IO_WAIT, 64 * 1024).await? == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        Ok(self.buf.len().min(req.left as usize))
    }

    pub fn consume(&mut self, req: &mut Request, n: usize) {
        self.buf.drain(..n);
        req.left -= n as u64;
    }

    async fn discard(&mut self, req: &mut Request) -> io::Result<()> {
        while req.left > 0 {
            let n = self.body_ready(req).await?;
            self.consume(req, n);
        }
        Ok(())
    }

    pub async fn write(&mut self, bytes: &[u8]) -> io::Result<()> {
        match timeout(IO_WAIT, self.stream.write_all(bytes)).await {
            Ok(r) => r,
            Err(_) => Err(io::ErrorKind::TimedOut.into()),
        }
    }

    /// The status line and headers; `length` becomes Content-Length.
    pub async fn send_head(&mut self, req: &Request, status: u16, headers: &[(&str, &str)], length: Option<u64>) -> io::Result<()> {
        let head = head_bytes(status, headers, length, req.keep);
        self.write(&head).await
    }

    /// A whole answer. HEAD gets the headers alone; 204 has no body and no length.
    pub async fn reply(&mut self, req: &Request, status: u16, headers: &[(&str, &str)], body: &[u8]) -> io::Result<()> {
        let has_body = status != 204 && status != 304;
        let mut out = head_bytes(status, headers, has_body.then_some(body.len() as u64), req.keep);
        if has_body && req.method != "HEAD" {
            out.extend_from_slice(body);
        }
        self.write(&out).await
    }

    /// Ends the connection after an answer: our side first, then whatever the client still
    /// sends is read and dropped for a moment, so the kernel does not answer that data with
    /// a reset that could destroy the answer before the client has read it.
    pub async fn linger(mut self) {
        let _ = timeout(Duration::from_secs(1), self.stream.shutdown()).await;
        let end = tokio::time::Instant::now() + LINGER;
        let mut sink = vec![0u8; 16 * 1024];
        let mut total = 0usize;
        while total < 16 * 1024 * 1024 {
            match timeout_at(end, self.stream.read(&mut sink)).await {
                Ok(Ok(n)) if n > 0 => total += n,
                _ => break,
            }
        }
    }

    /// Gives back memory a large body needed, once the request is done.
    fn tidy(&mut self) {
        if self.buf.capacity() > 64 * 1024 {
            self.buf.shrink_to(self.buf.len().max(4096));
        }
    }
}

fn build(parsed: &httparse::Request) -> Result<Request, HeadError> {
    let headers: Vec<(String, String)> = parsed
        .headers
        .iter()
        .map(|h| (h.name.to_ascii_lowercase(), String::from_utf8_lossy(h.value).trim().to_string()))
        .collect();
    let mut length = None;
    for (_, v) in headers.iter().filter(|(k, _)| k == "content-length") {
        // Digits only, and every copy the same (llhttp's rules; anything else is 400).
        if v.is_empty() || !v.bytes().all(|b| b.is_ascii_digit()) {
            return Err(HeadError::Status(400));
        }
        let n: u64 = v.parse().map_err(|_| HeadError::Status(400))?;
        if length.is_some_and(|m| m != n) {
            return Err(HeadError::Status(400));
        }
        length = Some(n);
    }
    let chunked = headers.iter().any(|(k, _)| k == "transfer-encoding");
    if chunked && length.is_some() {
        return Err(HeadError::Status(400));
    }
    let mut req = Request {
        method: parsed.method.unwrap_or("").to_string(),
        target: parsed.path.unwrap_or("").to_string(),
        http10: parsed.version == Some(0),
        headers,
        length,
        left: length.unwrap_or(0),
        keep: true,
    };
    req.keep = if req.http10 { req.has_token("connection", "keep-alive") } else { !req.has_token("connection", "close") };
    // A body of unknown length, or one too large to read and drop, ends the connection.
    if chunked || req.left > SAVES_MAX {
        req.keep = false;
    }
    Ok(req)
}

fn reason(status: u16) -> &'static str {
    match status {
        100 => "Continue",
        101 => "Switching Protocols",
        200 => "OK",
        201 => "Created",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        411 => "Length Required",
        412 => "Precondition Failed",
        413 => "Payload Too Large",
        429 => "Too Many Requests",
        431 => "Request Header Fields Too Large",
        501 => "Not Implemented",
        507 => "Insufficient Storage",
        _ => "Internal Server Error",
    }
}

fn head_bytes(status: u16, headers: &[(&str, &str)], length: Option<u64>, keep: bool) -> Vec<u8> {
    let mut s = format!("HTTP/1.1 {status} {}\r\n", reason(status));
    for (k, v) in headers {
        s.push_str(k);
        s.push_str(": ");
        s.push_str(v);
        s.push_str("\r\n");
    }
    s.push_str("date: ");
    s.push_str(&http_date(SystemTime::now()));
    s.push_str("\r\n");
    s.push_str(if keep { "connection: keep-alive\r\nkeep-alive: timeout=5\r\n" } else { "connection: close\r\n" });
    if let Some(n) = length {
        s.push_str(&format!("content-length: {n}\r\n"));
    }
    s.push_str("\r\n");
    s.into_bytes()
}

/// A request target read as the Node relay's `new URL(target, "http://relay")`: a path
/// (origin form) or an absolute http(s) URL; the query is kept for URLSearchParams-style
/// lookups. None where Node's URL parser threw, and also for a target starting with "//"
/// (Node takes what follows for a host name; no client of ours sends one).
pub struct Url {
    pub path: String,
    query: String,
}

impl Url {
    pub fn parse(target: &str) -> Option<Url> {
        let target = target.split('#').next().unwrap_or("");
        let rest = if target.starts_with("//") {
            return None;
        } else if target.starts_with('/') {
            target
        } else {
            let lower = target.get(..8).unwrap_or(target).to_ascii_lowercase();
            let after = if lower.starts_with("http://") {
                &target[7..]
            } else if lower.starts_with("https://") {
                &target[8..]
            } else {
                return None;
            };
            let end = after.find(['/', '?']).unwrap_or(after.len());
            let host = &after[..end];
            let bracket_ok = !host.contains('[') || host.find('[') < host.find(']');
            if host.is_empty() || !bracket_ok || host.bytes().any(|b| b <= b' ' || b == b'\\') {
                return None;
            }
            &after[end..]
        };
        let (path, query) = match rest.split_once('?') {
            Some((p, q)) => (p, q),
            None => (rest, ""),
        };
        let path = if path.is_empty() { "/" } else { path };
        Some(Url { path: path.to_string(), query: query.to_string() })
    }

    /// The first value of a query parameter, decoded as URLSearchParams does.
    pub fn query(&self, name: &str) -> Option<String> {
        self.query.split('&').filter(|p| !p.is_empty()).find_map(|pair| {
            let (k, v) = pair.split_once('=').unwrap_or((pair, ""));
            (form_decode(k) == name).then(|| form_decode(v))
        })
    }
}

/// Who a request comes from, for per-client limits: nginx on the droplet connects from this
/// machine and passes the player's address in X-Real-IP; anyone else counts by their own.
pub fn client(conn: &Conn, req: &Request) -> String {
    let peer = conn.remote.ip().to_string();
    let local = matches!(peer.as_str(), "127.0.0.1" | "::1" | "::ffff:127.0.0.1");
    match req.header("x-real-ip") {
        Some(real) if local && !real.is_empty() => real.to_string(),
        _ => peer,
    }
}

/// One TCP connection: requests one after another, or a WebSocket upgrade.
pub async fn serve(app: Arc<App>, stream: TcpStream, remote: SocketAddr) {
    let _ = stream.set_nodelay(true);
    let mut conn = Conn { stream, buf: Vec::new(), remote };
    let mut wait = FIRST_HEAD_WAIT;
    loop {
        let mut req = match conn.read_head(wait).await {
            Ok(req) => req,
            Err(HeadError::Gone) => return,
            Err(HeadError::Status(status)) => {
                let _ = conn.write(format!("HTTP/1.1 {status} {}\r\nConnection: close\r\n\r\n", reason(status)).as_bytes()).await;
                conn.linger().await;
                return;
            }
        };
        if req.is_upgrade() {
            crate::ws::upgrade(app, conn, req).await;
            return;
        }
        // node:http answers 100-continue itself, before the request handler sees it.
        if req.left > 0 && !req.http10 && req.header("expect").is_some_and(|e| e.eq_ignore_ascii_case("100-continue"))
            && conn.write(b"HTTP/1.1 100 Continue\r\n\r\n").await.is_err()
        {
            return;
        }
        if route(&app, &mut conn, &mut req).await.is_err() {
            return;
        }
        if !req.keep {
            conn.linger().await;
            return;
        }
        // A body the answer did not need is read and dropped, so the next request starts in
        // the right place.
        if conn.discard(&mut req).await.is_err() {
            return;
        }
        conn.tidy();
        wait = KEEP_ALIVE;
    }
}

async fn route(app: &App, conn: &mut Conn, req: &mut Request) -> io::Result<()> {
    let Some(url) = Url::parse(&req.target) else {
        return conn.reply(req, 400, &[("content-type", "text/plain")], b"Invalid request URL.\n").await;
    };
    if url.path == "/saves" || url.path.starts_with("/saves/") {
        return crate::saves::handle(app, conn, req, &url).await;
    }
    if url.path == "/reports" {
        return crate::reports::handle(app, conn, req, &url).await;
    }
    if url.path == "/stats" {
        // {players, here}: how many are on the relay and in that room (empty: the lobby),
        // for the start page. Counts only, no names.
        let room = url.query("room").map(|r| js_slice(&r, 40).to_string()).filter(|r| !r.is_empty());
        let (players, here) = lock(&app.relay).stats(room.as_deref().unwrap_or("lobby"));
        let body = format!(r#"{{"players":{players},"here":{here}}}"#);
        return conn
            .reply(req, 200, &[("content-type", "application/json"), ("cache-control", "no-store"),
                ("access-control-allow-origin", "*"), ("cross-origin-resource-policy", "cross-origin")], body.as_bytes())
            .await;
    }
    conn.reply(req, 200, &[("content-type", "text/plain")], b"Carbon online relay. Players connect with WebSockets.\n").await
}

#[cfg(test)]
mod tests {
    use super::Url;

    #[test]
    fn targets() {
        let u = Url::parse("/friends%20%2F%20caf%C3%A9?key=a+b%21&room=x&key=second").unwrap();
        assert_eq!(u.path, "/friends%20%2F%20caf%C3%A9");
        assert_eq!(u.query("key").as_deref(), Some("a b!"));
        assert_eq!(u.query("room").as_deref(), Some("x"));
        assert_eq!(u.query("none"), None);
        assert!(Url::parse("//[").is_none());
        assert!(Url::parse("//host/stats").is_none());
        assert!(Url::parse("*").is_none());
        assert_eq!(Url::parse("http://relay").unwrap().path, "/");
        assert_eq!(Url::parse("HTTP://relay:80/stats?room=a").unwrap().path, "/stats");
        assert!(Url::parse("http://[/").is_none());
        assert_eq!(Url::parse("/stats#frag").unwrap().path, "/stats");
        assert_eq!(Url::parse("/lan?key=").unwrap().query("key").as_deref(), Some(""));
    }
}
