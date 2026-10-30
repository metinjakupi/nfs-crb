//! Optional, recoverable ownership for online names. A player explicitly claims a name with
//! the opaque id of an existing cloud save. That id is derived from the cloud-save keyphrase
//! in the browser; the server never receives the keyphrase. Only a domain-separated SHA-256
//! digest of the id is kept here.
//!
//! Existing names are reserved with their last browser-key fingerprint before claims are
//! enabled. This prevents a different player from claiming an old name and inheriting its
//! Messenger relationships or statistics. A successful claim only adds an access check. It
//! does not rewrite either store or any cloud-save blob.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::ea::clean_name;
use crate::http::{client, Conn, Request, Url};
use crate::util::{json_str, lock, same_bytes};
use crate::App;

const MAX_CLAIMS: usize = 10_000;
const CLAIM_BURST: f64 = 5.0;
const CLAIM_RATE: f64 = 10.0 / 86_400.0;

/// The server-side owner of a name: hex SHA-256, never the cloud-save id itself.
pub type Owner = String;

#[derive(Clone, Debug)]
struct Claim {
    name: String,
    owner: Owner,
    claimed: u64,
    seen: u64,
}

#[derive(Clone, Debug)]
struct Reservation {
    name: String,
    /// SHA-256 of the browser key that used this name before explicit claims existed. None
    /// means that the name needs manual recovery before anybody can claim it.
    key: Option<String>,
}

struct Bucket {
    tokens: f64,
    at: Instant,
}

/// What a name is to the token that asks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Free,
    Yours,
    Taken,
    Legacy,
    Reserved,
    Default,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    Invalid,
    Default,
    Taken,
    Legacy,
    Recovery,
    AlreadyHasName,
    Full,
    Storage,
}

#[derive(Default)]
pub struct Claims {
    names: BTreeMap<String, Claim>,
    owners: BTreeMap<Owner, String>,
    reservations: BTreeMap<String, Reservation>,
    migration_done: bool,
    file: Option<PathBuf>,
    clients: HashMap<String, Bucket>,
}

fn canonical(name: &str) -> String {
    clean_name(name).to_ascii_lowercase()
}

fn valid_token(token: &str) -> bool {
    token.len() == 64 && token.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

pub fn browser_fingerprint(key: &str) -> Option<String> {
    if !(16..=128).contains(&key.len()) || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-') {
        return None;
    }
    Some(Sha256::digest(key.as_bytes()).iter().map(|b| format!("{b:02x}")).collect())
}

pub fn browser_digest(key: &[u8; 32]) -> String {
    key.iter().map(|b| format!("{b:02x}")).collect()
}

fn unix_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs())
}

/// The owner represented by a cloud-save id. The domain separator prevents this digest from
/// being confused with a save ETag or another SHA-256 value kept by the server.
pub fn owner_of(token: &str) -> Option<Owner> {
    if !valid_token(token) {
        return None;
    }
    let mut h = Sha256::new();
    h.update(b"nfsc name claim v1\0");
    h.update(token.as_bytes());
    Some(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

impl Claims {
    /// Claims kept beside Messenger data in `names.json`; in memory only without a folder.
    pub fn open(dir: Option<PathBuf>, stats_dir: Option<PathBuf>) -> Result<Claims, String> {
        let mut out = Claims::default();
        let Some(dir) = dir else { return Ok(out) };
        if let Err(e) = fs::create_dir_all(&dir) {
            return Err(format!("cannot use {}: {e}", dir.display()));
        }
        let file = dir.join("names.json");
        match fs::read_to_string(&file) {
            Ok(text) => match serde_json::from_str::<Value>(&text) {
                Ok(v) => out.load(&v),
                Err(e) => return Err(format!("{} is not readable: {e}", file.display())),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => return Err(format!("cannot read {}: {e}", file.display())),
        }
        out.file = Some(file);
        if !out.migration_done {
            if let Err(e) = out.import_people(&dir.join("messenger.json")) {
                return Err(e);
            }
            if let Some(stats_dir) = &stats_dir {
                if let Err(e) = out.import_stats(&stats_dir.join("stats.json")) {
                    return Err(e);
                }
            }
            out.migration_done = true;
            if !out.save() {
                return Err("cannot save the protected-name migration".into());
            }
        }
        Ok(out)
    }

    fn load(&mut self, value: &Value) {
        self.migration_done = value.get("migration").and_then(Value::as_bool).unwrap_or(false);
        if let Some(names) = value.get("names").and_then(Value::as_object) {
            for (key, value) in names.iter().take(MAX_CLAIMS) {
                let Some(name) = value.get("name").and_then(Value::as_str) else { continue };
                let Some(owner) = value.get("owner").and_then(Value::as_str).filter(|v| valid_token(v)) else { continue };
                let clean = clean_name(name);
                let k = canonical(&clean);
                if k != canonical(key) || k == "player" || self.owners.contains_key(owner) {
                    continue;
                }
                let claimed = value.get("claimed").and_then(Value::as_u64).unwrap_or(0);
                let seen = value.get("seen").and_then(Value::as_u64).unwrap_or(claimed);
                self.names.insert(k.clone(), Claim { name: clean, owner: owner.to_string(), claimed, seen });
                self.owners.insert(owner.to_string(), k);
            }
        }
        if let Some(reserved) = value.get("reserved").and_then(Value::as_object) {
            for (key, value) in reserved.iter().take(MAX_CLAIMS) {
                let Some(name) = value.get("name").and_then(Value::as_str) else { continue };
                let clean = clean_name(name);
                let k = canonical(&clean);
                if k != canonical(key) || k == "player" || self.names.contains_key(&k) {
                    continue;
                }
                let legacy = value.get("key").and_then(Value::as_str).filter(|v| valid_token(v)).map(str::to_string);
                self.reservations.insert(k, Reservation { name: clean, key: legacy });
            }
        }
    }

    fn import_people(&mut self, file: &PathBuf) -> Result<(), String> {
        let text = match fs::read_to_string(file) {
            Ok(text) => text,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(e) => return Err(format!("cannot read {} for name migration: {e}", file.display())),
        };
        let value = serde_json::from_str::<Value>(&text).map_err(|e| format!("{} is not readable for name migration: {e}", file.display()))?;
        let people = value.get("people").and_then(Value::as_object).ok_or_else(|| format!("{} has no people list", file.display()))?;
        self.import_reservations(people);
        Ok(())
    }

    fn import_stats(&mut self, file: &PathBuf) -> Result<(), String> {
        let text = match fs::read_to_string(file) {
            Ok(text) => text,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(e) => return Err(format!("cannot read {} for name migration: {e}", file.display())),
        };
        let value = serde_json::from_str::<Value>(&text).map_err(|e| format!("{} is not readable for name migration: {e}", file.display()))?;
        let people = value.get("people").and_then(Value::as_object).ok_or_else(|| format!("{} has no people list", file.display()))?;
        self.import_reservations(people);
        Ok(())
    }

    /// Snapshot old name records once. Later sign-ins cannot replace this migration proof.
    fn import_reservations(&mut self, people: &serde_json::Map<String, Value>) -> bool {
        let mut changed = false;
        for (key, value) in people.iter().take(MAX_CLAIMS) {
            if self.names.len() + self.reservations.len() >= MAX_CLAIMS {
                break;
            }
            let name = value.get("name").and_then(Value::as_str).unwrap_or(key);
            let clean = clean_name(name);
            let k = canonical(&clean);
            if k == "player" || self.names.contains_key(&k) || self.reservations.contains_key(&k) {
                continue;
            }
            let legacy = value.get("key").and_then(Value::as_str).filter(|v| valid_token(v)).map(str::to_string);
            self.reservations.insert(k, Reservation { name: clean, key: legacy });
            changed = true;
        }
        changed
    }

    fn save(&self) -> bool {
        let Some(file) = &self.file else { return true };
        let names: serde_json::Map<String, Value> = self
            .names
            .iter()
            .map(|(key, claim)| (key.clone(), json!({ "name": claim.name, "owner": claim.owner, "claimed": claim.claimed, "seen": claim.seen })))
            .collect();
        let reserved: serde_json::Map<String, Value> = self
            .reservations
            .iter()
            .map(|(key, reservation)| (key.clone(), json!({ "name": reservation.name, "key": reservation.key })))
            .collect();
        let text = serde_json::to_string(&json!({ "version": 2, "migration": self.migration_done, "names": names, "reserved": reserved })).unwrap_or_default();
        let tmp = file.with_extension("json.tmp");
        let result = (|| {
            let mut options = fs::OpenOptions::new();
            options.create(true).truncate(true).write(true);
            #[cfg(unix)]
            std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
            let mut out = options.open(&tmp)?;
            out.write_all(text.as_bytes())?;
            out.sync_all()?;
            fs::rename(&tmp, file)
        })();
        if let Err(e) = result {
            eprintln!("names: cannot write {}: {e}", file.display());
            return false;
        }
        true
    }

    /// A token may claim at most one name. The first claim does not touch the data kept under
    /// that name in Messenger or Player Stats.
    pub fn claim(&mut self, name: &str, owner: &Owner, browser_key: &str, now: u64) -> Result<String, Refusal> {
        let shown = clean_name(name);
        let key = canonical(&shown);
        if key == "player" {
            return Err(if name.is_empty() { Refusal::Invalid } else { Refusal::Default });
        }
        if let Some(claim) = self.names.get_mut(&key) {
            if &claim.owner != owner {
                return Err(Refusal::Taken);
            }
            if claim.name != shown || claim.seen != now {
                claim.name = shown.clone();
                claim.seen = now;
                self.save();
            }
            return Ok(shown);
        }
        if let Some(reservation) = self.reservations.get(&key) {
            let Some(expected) = reservation.key.as_ref() else { return Err(Refusal::Recovery) };
            if browser_fingerprint(browser_key).as_ref() != Some(expected) {
                return Err(Refusal::Legacy);
            }
        }
        if self.owners.get(owner).is_some_and(|old| old != &key) {
            return Err(Refusal::AlreadyHasName);
        }
        if self.names.len() >= MAX_CLAIMS {
            return Err(Refusal::Full);
        }
        let reservation = self.reservations.remove(&key);
        self.names.insert(key.clone(), Claim { name: shown.clone(), owner: owner.clone(), claimed: now, seen: now });
        self.owners.insert(owner.clone(), key);
        if !self.save() {
            let key = canonical(&shown);
            self.names.remove(&key);
            self.owners.remove(owner);
            if let Some(reservation) = reservation {
                self.reservations.insert(key, reservation);
            }
            return Err(Refusal::Storage);
        }
        Ok(shown)
    }

    pub fn status(&self, name: &str, owner: Option<&Owner>, browser_key: &str) -> (String, Status) {
        let shown = clean_name(name);
        let key = canonical(&shown);
        if key == "player" {
            return (shown, Status::Default);
        }
        match self.names.get(&key) {
            Some(claim) if owner.is_some_and(|o| o == &claim.owner) => (claim.name.clone(), Status::Yours),
            Some(claim) => (claim.name.clone(), Status::Taken),
            None => match self.reservations.get(&key) {
                Some(r) if r.key.as_ref().is_some_and(|expected| browser_fingerprint(browser_key).as_ref() == Some(expected)) => {
                    (r.name.clone(), Status::Legacy)
                }
                Some(r) => (r.name.clone(), Status::Reserved),
                None => (shown, Status::Free),
            },
        }
    }

    /// A new name stays open. An old reserved name needs its old browser key until the player
    /// claims it. A claimed name needs the same keyphrase-derived owner.
    pub fn may_use(&self, name: &str, owner: Option<&Owner>, browser: Option<&str>) -> bool {
        let key = canonical(name);
        if let Some(claim) = self.names.get(&key) {
            return owner.is_some_and(|o| o == &claim.owner);
        }
        self.reservations.get(&key).is_none_or(|r| r.key.as_deref().is_some_and(|expected| browser == Some(expected)))
    }

    /// Per client: five claims at once, then ten a day.
    pub fn allow_claim(&mut self, client: &str) -> bool {
        let now = Instant::now();
        let bucket = self.clients.entry(client.to_string()).or_insert(Bucket { tokens: CLAIM_BURST, at: now });
        bucket.tokens = (bucket.tokens + now.duration_since(bucket.at).as_secs_f64() * CLAIM_RATE).min(CLAIM_BURST);
        bucket.at = now;
        if bucket.tokens < 1.0 {
            return false;
        }
        bucket.tokens -= 1.0;
        true
    }
}

const HEADERS: [(&str, &str); 4] = [
    ("access-control-allow-origin", "*"),
    ("cross-origin-resource-policy", "cross-origin"),
    ("cache-control", "no-store"),
    ("content-type", "application/json"),
];

async fn answer(conn: &mut Conn, req: &Request, status: u16, body: &str) -> std::io::Result<()> {
    conn.reply(req, status, &HEADERS, body.as_bytes()).await
}

async fn body(conn: &mut Conn, req: &mut Request, max: usize) -> std::io::Result<Option<Vec<u8>>> {
    let Some(length) = req.length else { return Ok(None) };
    if length as usize > max {
        return Ok(None);
    }
    let mut out = Vec::with_capacity(length as usize);
    while req.left > 0 {
        let n = conn.body_ready(req).await?;
        out.extend_from_slice(&conn.buf[..n]);
        conn.consume(req, n);
    }
    Ok(Some(out))
}

fn bearer(req: &Request) -> Option<&str> {
    let token = req.header("authorization")?.strip_prefix("Bearer ")?;
    valid_token(token).then_some(token)
}

/// GET/POST /names. The browser sends a cloud-save id as a bearer proof, never its keyphrase.
pub async fn handle(app: &App, conn: &mut Conn, req: &mut Request, url: &Url) -> std::io::Result<()> {
    if req.method == "OPTIONS" {
        let mut headers = HEADERS.to_vec();
        headers.extend_from_slice(&[
            ("access-control-allow-methods", "GET, POST, OPTIONS"),
            ("access-control-allow-headers", "Authorization, Content-Type, X-Carbon-Player-Key"),
            ("access-control-allow-private-network", "true"),
            ("access-control-max-age", "600"),
        ]);
        return conn.reply(req, 204, &headers, b"").await;
    }
    let given = url.query("key").unwrap_or_default();
    if !app.key.is_empty() && !same_bytes(given.as_bytes(), app.key.as_bytes()) {
        return answer(conn, req, 403, r#"{"error":"forbidden"}"#).await;
    }
    let token = bearer(req).map(str::to_string);
    let owner = token.as_deref().and_then(owner_of);
    let browser_key = req.header("x-carbon-player-key").unwrap_or_default().to_string();
    match req.method.as_str() {
        "GET" => {
            let name = url.query("name").unwrap_or_default();
            let (name, state) = lock(&app.identities).status(&name, owner.as_ref(), &browser_key);
            let state = match state {
                Status::Free => "free",
                Status::Yours => "yours",
                Status::Taken => "taken",
                Status::Legacy => "legacy",
                Status::Reserved => "reserved",
                Status::Default => "default",
            };
            answer(conn, req, 200, &format!(r#"{{"name":{},"state":"{state}"}}"#, json_str(&name))).await
        }
        "POST" => {
            let (Some(token), Some(owner)) = (token, owner) else {
                return answer(conn, req, 401, r#"{"error":"keyphrase"}"#).await;
            };
            let who = client(conn, req);
            if !lock(&app.identities).allow_claim(&who) {
                return answer(conn, req, 429, r#"{"error":"busy"}"#).await;
            }
            let Some(bytes) = body(conn, req, 1024).await? else {
                return answer(conn, req, if req.length.is_none() { 411 } else { 413 }, r#"{"error":"invalid"}"#).await;
            };
            let name = serde_json::from_slice::<Value>(&bytes)
                .ok()
                .and_then(|v| v.get("name").and_then(Value::as_str).map(str::to_string))
                .unwrap_or_default();
            let Some(saves) = &app.saves else {
                return answer(conn, req, 501, r#"{"error":"cloud-off"}"#).await;
            };
            if !saves.exists(&token).await.unwrap_or(false) {
                return answer(conn, req, 404, r#"{"error":"keyphrase"}"#).await;
            }
            let claimed = lock(&app.identities).claim(&name, &owner, &browser_key, unix_secs());
            match claimed {
                Ok(name) => answer(conn, req, 200, &format!(r#"{{"name":{},"state":"yours"}}"#, json_str(&name))).await,
                Err(Refusal::Invalid) => answer(conn, req, 400, r#"{"error":"invalid"}"#).await,
                Err(Refusal::Default) => answer(conn, req, 400, r#"{"error":"default"}"#).await,
                Err(Refusal::Taken) => answer(conn, req, 409, r#"{"error":"taken"}"#).await,
                Err(Refusal::Legacy) => answer(conn, req, 403, r#"{"error":"old-browser"}"#).await,
                Err(Refusal::Recovery) => answer(conn, req, 403, r#"{"error":"recovery"}"#).await,
                Err(Refusal::AlreadyHasName) => answer(conn, req, 409, r#"{"error":"one-name"}"#).await,
                Err(Refusal::Full) => answer(conn, req, 507, r#"{"error":"full"}"#).await,
                Err(Refusal::Storage) => answer(conn, req, 503, r#"{"error":"storage"}"#).await,
            }
        }
        _ => answer(conn, req, 405, r#"{"error":"method"}"#).await,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_reserved_name_without_old_browser_proof_cannot_be_taken() {
        let mut claims = Claims::default();
        claims.reservations.insert("arsim".into(), Reservation { name: "Arsim".into(), key: None });
        let owner = owner_of("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").unwrap();

        assert_eq!(claims.status("Arsim", Some(&owner), "").1, Status::Reserved);
        assert!(!claims.may_use("Arsim", Some(&owner), None));
        assert_eq!(claims.claim("Arsim", &owner, "attacker-browser-key-01", 1), Err(Refusal::Recovery));
        assert!(claims.names.is_empty(), "a failed recovery changed ownership");
    }
}
