//! GET /leaderboard and /leaderboard/admin through the real binary on a free port, with a seeded
//! stats.json and names.json: the answer's shape, CORS, who is left out (unclaimed names, the
//! game's own boards by default, a name the admin bans), and the admin's authority.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

const KEY: &str = "local-leaderboard-test";
const ADMIN_TOKEN: &str = "abababababababababababababababababababababababababababababababab";

/// identity.rs owner_of: what the server keeps for a cloud-save id.
fn owner_of(token: &str) -> String {
    let mut h = Sha256::new();
    h.update(b"nfsc name claim v1\0");
    h.update(token.as_bytes());
    h.finalize().iter().map(|b| format!("{b:02x}")).collect()
}

struct Server {
    child: Child,
    port: u16,
    dir: PathBuf,
}

impl Server {
    fn start(name: &str, env: &[(&str, &str)]) -> Server {
        let dir = std::env::temp_dir().join(format!("carbon-leaderboard-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let (stats, names) = (dir.join("stats"), dir.join("names"));
        std::fs::create_dir_all(&stats).unwrap();
        std::fs::create_dir_all(&names).unwrap();
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs();
        let week = (now / 86_400 + 3) / 7;
        let t = |v: f64, at: u64| json!([v, "0x8774322C,51.5,72.9,0x00000000", 1, [[2, week, v, "0x8774322C,51.5,72.9,0x00000000"]], at, at]);
        let people = json!({
            // wins: the game's own count (never public), the public count under this reset
            // (shown) and under an earlier one (not shown)
            "metin": {"name": "Metin", "key": null, "seen": 1, "stats": {"Srv_Bst_30F07253": t(103.05, now), "Overall_Solo_On_Wins": [40.0, "", 0, [], now, 0], "Srv_Wins": [4.0, "reset:1000", 0, [[2, week, 4.0, "reset:1000"]], now, now]}},
            "cheat": {"name": "Cheat", "key": null, "seen": 1, "stats": {"Srv_Bst_30F07253": t(101.0, now), "Overall_Solo_On_Wins": [90.0, "", 0, [], now, 0], "Srv_Wins": [9.0, "reset:500", 0, [], now, now]}},
            // not claimed: never on the public boards
            "guest": {"name": "Guest", "key": "0123456789abcdef", "seen": 1, "stats": {"Srv_Bst_30F07253": t(99.0, now), "Evt_Bst_Lap_30F07253": [50.0, "", 1, [[2, week, 50.0, ""]], now, now]}},
            // the game's own word only
            "jan": {"name": "Jan", "key": null, "seen": 1, "stats": {"Evt_Bst_30F07253": t(90.0, now), "Best_Drift_Combo": [5000.0, "", 0, [], 0, 0]}},
        });
        std::fs::write(stats.join("stats.json"), json!({"version": 1, "people": people}).to_string()).unwrap();
        let claim = |n: &str, c: char| json!({"name": n, "owner": c.to_string().repeat(64), "claimed": 1, "seen": 1});
        let claims = json!({"version": 2, "migration": true, "reserved": {}, "names": {"metin": claim("Metin", '1'), "cheat": claim("Cheat", '2'), "jan": claim("Jan", '3')}});
        std::fs::write(names.join("names.json"), claims.to_string()).unwrap();
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_carbon-server"));
        cmd.args(["0", "127.0.0.1"])
            .env("CARBON_RELAY_KEY", KEY)
            .env("CARBON_STATS_DIR", &stats)
            .env("CARBON_MESSENGER_DIR", &names)
            .env("CARBON_ADMIN_OWNERS", owner_of(ADMIN_TOKEN))
            .env_remove("CARBON_SAVES_DIR")
            .env_remove("CARBON_TRACE_DIR")
            .env_remove("CARBON_MODERATION_DIR")
            .env_remove("CARBON_LEADERBOARD_GAME")
            .env_remove("CARBON_LEADERBOARD_SECS")
            .env("CARBON_LEADERBOARD_RESET", "1000")
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

    /// One request on its own connection: (status, head in lowercase, body).
    fn ask(&self, method: &str, target: &str, extra: &[(&str, &str)], body: Option<&str>) -> (u16, String, String) {
        let mut s = TcpStream::connect(("127.0.0.1", self.port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let mut head = format!("{method} {target} HTTP/1.1\r\nHost: test\r\nConnection: close\r\n");
        for (k, v) in extra {
            head += &format!("{k}: {v}\r\n");
        }
        if let Some(b) = body {
            head += &format!("Content-Type: application/json\r\nContent-Length: {}\r\n", b.len());
        }
        head += "\r\n";
        s.write_all(head.as_bytes()).unwrap();
        if let Some(b) = body {
            s.write_all(b.as_bytes()).unwrap();
        }
        let mut out = Vec::new();
        let _ = s.read_to_end(&mut out);
        let text = String::from_utf8_lossy(&out).into_owned();
        let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
        let status = head.split(' ').nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
        (status, head.to_lowercase(), body.to_string())
    }

    fn json(&self, target: &str) -> Value {
        let (status, _, body) = self.ask("GET", target, &[], None);
        assert_eq!(status, 200, "{target}: {body}");
        serde_json::from_str(&body).unwrap()
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

fn board<'a>(v: &'a Value, id: &str) -> Option<&'a Value> {
    v["boards"].as_array().unwrap().iter().find(|b| b["id"] == id)
}

fn names(rows: &Value) -> Vec<&str> {
    rows.as_array().unwrap().iter().map(|r| r[0].as_str().unwrap()).collect()
}

#[test]
fn the_public_answer_has_only_confirmed_results_of_claimed_names() {
    let s = Server::start("public", &[]);
    let (status, head, body) = s.ask("GET", "/leaderboard", &[("Origin", "https://nfscrb.xyz")], None);
    assert_eq!(status, 200);
    assert!(head.contains("access-control-allow-origin: https://nfscrb.xyz"), "{head}");
    assert!(head.contains("cache-control: public, max-age=60"), "{head}");
    assert!(head.contains("content-type: application/json"), "{head}");
    let v: Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["rows"], json!(["name", "value", "car", "when"]));
    let b = board(&v, "Evt_Bst:30F07253").expect("the race host's board");
    assert_eq!((b["family"].as_str(), b["source"].as_str(), b["race"].as_str(), b["low"].as_bool(), b["unit"].as_str()), (Some("Evt_Bst"), Some("race"), Some("30F07253"), Some(true), Some("time")));
    assert_eq!(names(&b["all"]), ["Cheat", "Metin"]);
    assert_eq!(names(&b["week"]), ["Cheat", "Metin"]);
    assert_eq!(b["players"], json!(2));
    assert_eq!(b["all"][1][1], json!(103.05));
    assert_eq!(b["all"][1][2], json!("0x8774322c"));
    assert!(b["all"][1][3].as_u64().is_some());
    // wins since the reset only: Metin's 4, not the game's own 40, nor Cheat's from before
    let wins = board(&v, "Overall_Solo_On_Wins").expect("the public wins");
    assert_eq!(wins["all"], json!([["Metin", 4.0, "", serde_json::Value::from(wins["all"][0][3].as_u64().unwrap())]]));
    assert_eq!(names(&wins["week"]), ["Metin"]);
    // the game's own boards are off by default; no unclaimed name, browser key or owner leaks
    assert!(v["boards"].as_array().unwrap().iter().all(|b| b["source"] == "race"), "{body}");
    for secret in ["Guest", "0123456789abcdef", "1111111111", "Jan"] {
        assert!(!body.contains(secret), "{secret} in {body}");
    }
    // a page elsewhere gets no CORS header
    let (_, head, _) = s.ask("GET", "/leaderboard", &[("Origin", "https://nfscrb.xyz.example.com")], None);
    assert!(!head.contains("access-control-allow-origin"), "{head}");
    let (_, head, _) = s.ask("GET", "/leaderboard", &[("Origin", "http://localhost:8098")], None);
    assert!(head.contains("access-control-allow-origin: http://localhost:8098"), "{head}");
    // one board, one period, one row
    let one = s.json("/leaderboard?board=Evt_Bst:30F07253&period=week&limit=1");
    assert_eq!(one["boards"].as_array().unwrap().len(), 1);
    assert!(one["boards"][0].get("all").is_none());
    assert_eq!(names(&one["boards"][0]["week"]), ["Cheat"]);
    // only reading
    assert_eq!(s.ask("OPTIONS", "/leaderboard", &[("Origin", "https://nfscrb.xyz")], None).0, 204);
    assert_eq!(s.ask("POST", "/leaderboard", &[], Some("{}")).0, 405);
}

#[test]
fn the_games_own_boards_can_be_turned_on() {
    let s = Server::start("game", &[("CARBON_LEADERBOARD_GAME", "1")]);
    let v = s.json("/leaderboard");
    let b = board(&v, "game:Evt_Bst:30F07253").expect("the game's own board");
    assert_eq!((b["source"].as_str(), names(&b["all"])), (Some("game"), vec!["Jan"]));
    assert!(board(&v, "game:Best_Drift_Combo").is_some_and(|b| b["low"] == false && b["unit"] == "points"));
}

#[test]
fn only_the_admin_bans_and_the_board_shows_it_at_once() {
    let s = Server::start("admin", &[]);
    let admin = format!("/leaderboard/admin?key={KEY}");
    let bearer = format!("Bearer {ADMIN_TOKEN}");
    let auth = [("Authorization", bearer.as_str())];
    // no proof, someone else's proof, no server key
    assert_eq!(s.ask("GET", &format!("{admin}&op=pending"), &[], None).0, 401);
    let other = format!("Bearer {}", "c".repeat(64));
    assert_eq!(s.ask("GET", &format!("{admin}&op=pending"), &[("Authorization", other.as_str())], None).0, 401);
    assert_eq!(s.ask("GET", "/leaderboard/admin?op=pending", &auth, None).0, 403);
    // changes only by POST
    assert_eq!(s.ask("GET", &format!("{admin}&op=ban&name=Cheat"), &auth, None).0, 405);
    let (status, _, body) = s.ask("GET", &format!("{admin}&op=pending"), &auth, None);
    assert_eq!(status, 200, "{body}");
    assert_eq!(serde_json::from_str::<Value>(&body).unwrap()["data"]["total"], json!(0));
    assert_eq!(names(&board(&s.json("/leaderboard"), "Evt_Bst:30F07253").unwrap()["all"]), ["Cheat", "Metin"]);
    let (status, _, body) = s.ask("POST", &admin, &auth, Some(r#"{"op":"ban","name":"Cheat","text":"fake times"}"#));
    assert_eq!(status, 200, "{body}");
    assert_eq!(names(&board(&s.json("/leaderboard"), "Evt_Bst:30F07253").unwrap()["all"]), ["Metin"]);
    let (_, _, body) = s.ask("GET", &format!("{admin}&op=audit"), &auth, None);
    let audit: Value = serde_json::from_str(&body).unwrap();
    assert_eq!(audit["data"]["rows"][0]["op"], json!("ban"));
    assert!(audit["data"]["rows"][0]["actor"].as_str().unwrap().starts_with("admin:"));
    assert!(!body.contains(&owner_of(ADMIN_TOKEN)), "the admin's owner digest stays private");
    // kept for the next start
    let saved = std::fs::read_to_string(s.dir.join("stats").join("leaderboard.json")).unwrap();
    assert!(saved.contains("\"cheat\""), "{saved}");
    let (status, _, _) = s.ask("POST", &admin, &auth, Some(r#"{"op":"approve","id":12345}"#));
    assert_eq!(status, 400);
}

#[test]
fn the_in_game_group_is_what_the_game_shows_and_comes_only_when_asked_for() {
    let s = Server::start("ingame", &[]);
    // the plain answer is unchanged: no in-game board in it
    let plain = s.json("/leaderboard");
    assert!(plain["boards"].as_array().unwrap().iter().all(|b| b["source"] == "race"));
    assert!(plain.get("ingame").is_none());
    let (status, head, body) = s.ask("GET", "/leaderboard?source=ingame", &[("Origin", "https://nfscrb.xyz")], None);
    assert_eq!(status, 200);
    assert!(head.contains("access-control-allow-origin: https://nfscrb.xyz") && head.contains("cache-control: public, max-age=60"), "{head}");
    let v: Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["ingame"]["label"], json!("In-game leaderboard"));
    assert!(v["boards"].as_array().unwrap().iter().all(|b| b["source"] == "ingame" && b["label"] == "In-game leaderboard"));
    // the game's own best (Jan's, claimed or not does not matter here) with its two numbers
    let b = board(&v, "ingame:Evt_Bst:30F07253").expect("the in-game race board");
    assert_eq!(names(&b["all"]), ["Jan"]);
    assert_eq!(b["all"][0], json!(["Jan", 90.0, "0x8774322c", b["all"][0][3].clone(), 51.5, 72.9]));
    assert_eq!(names(&b["week"]), ["Jan"]);
    assert!(b["day"].is_array());
    // an unclaimed name is on the in-game board, as in the game
    assert_eq!(names(&board(&v, "ingame:Evt_Bst_Lap:30F07253").unwrap()["all"]), ["Guest"]);
    // the game's own wins (before the reset too), every name
    assert_eq!(names(&board(&v, "ingame:Overall_Solo_On_Wins").unwrap()["all"]), ["Cheat", "Metin"]);
    // game-only keys stay out, and no browser key or owner
    assert!(board(&v, "ingame:Best_Drift_Combo").is_none() && !body.contains("Best_Drift") && !body.contains("Srv_"));
    for secret in ["0123456789abcdef", "1111111111"] {
        assert!(!body.contains(secret), "{secret} in {body}");
    }
    // one in-game board by id, one period; both groups together
    let one = s.json("/leaderboard?board=ingame:Overall_Solo_On_Wins&period=all&limit=1");
    assert_eq!(names(&one["boards"][0]["all"]), ["Cheat"]);
    assert!(one["boards"][0].get("week").is_none() && one["boards"][0].get("day").is_none());
    let all = s.json("/leaderboard?source=all");
    assert!(board(&all, "Evt_Bst:30F07253").is_some() && board(&all, "ingame:Evt_Bst:30F07253").is_some());
    // a banned name leaves the in-game group at once
    let admin = format!("/leaderboard/admin?key={KEY}");
    let bearer = format!("Bearer {ADMIN_TOKEN}");
    let (status, _, body) = s.ask("POST", &admin, &[("Authorization", bearer.as_str())], Some(r#"{"op":"ban","name":"Cheat","text":"fake"}"#));
    assert_eq!(status, 200, "{body}");
    assert_eq!(names(&board(&s.json("/leaderboard?source=ingame"), "ingame:Overall_Solo_On_Wins").unwrap()["all"]), ["Metin"]);
}

#[test]
fn the_in_game_group_can_be_turned_off() {
    let s = Server::start("ingame-off", &[("CARBON_LEADERBOARD_INGAME", "0")]);
    assert_eq!(s.json("/leaderboard?source=ingame")["boards"], json!([]));
    assert!(board(&s.json("/leaderboard"), "Evt_Bst:30F07253").is_some());
}
