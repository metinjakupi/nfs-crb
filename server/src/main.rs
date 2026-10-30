//! carbon-server - the online server for the browser port of Need for Speed: Carbon.
//!
//! Usage: carbon-server [port] [host]
//!   port 0 (the default) takes any free port; host defaults to 127.0.0.1.
//!
//! One process, on one port:
//! - the free-roam relay (relay.rs): players open the game with ?mp=ws://<host>:<port>/<room>;
//!   the same protocol, limits and log lines as tools/carbon-net-relay.mjs, whose header is the
//!   protocol reference, plus GET /stats, the start page's player count;
//! - cloud saves (saves.rs): sealed blobs under /saves/<id>, with CARBON_SAVES_DIR set;
//! - optional name claims (identity.rs): GET/POST /names; a claim protects the existing
//!   name-keyed Messenger and Player Stats records without moving or rewriting them;
//! - problem reports (reports.rs): POST /reports, the game page's log as text, with
//!   CARBON_REPORTS_DIR set (CARBON_REPORTS_MAX_MB for all of them, default 200);
//! - GET /lan (lan.rs): the switch of the virtual network that the game's own Online mode
//!   talks through (plans/lan-protocol.md), with the trace host at 10.77.0.1 (trace.rs).
//!   (So a free-roam room cannot be called "lan".)
//!
//! Environment: CARBON_RELAY_KEY (WebSockets, /lan, /saves and /names must carry ?key=<it>),
//! CARBON_SAVES_DIR, CARBON_SAVES_MAX_MB (default 1024), CARBON_TRACE_DIR (the trace host's
//! files), CARBON_MESSENGER_DIR (Messenger data and names.json claims), CARBON_TRACE_GAME
//! (1: PlayNow sends players to a game the trace host "hosts", to record what a joining game
//! sends its host). Empty counts as unset.
//!
//! Everything runs on one thread, as the Node relay did: rooms of a few hundred players (each
//! getting the cars of the 24 nearest; relay.rs) and a LAN of a few players fit in one core,
//! and one thread keeps the memory small. File work runs on a few helper threads, so a disk
//! never holds up the players.
//!
//! Also: CARBON_ROOM_MAX (players a room holds, default 256), CARBON_INTEREST_K (cars each
//! player gets, default 24).

mod ea;
mod host;
mod http;
mod identity;
mod lan;
mod messenger;
mod olmsg;
mod relay;
mod reports;
mod saves;
mod stats;
mod trace;
mod tunnel;
mod util;
mod ws;

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::net::TcpListener;

pub struct App {
    /// CARBON_RELAY_KEY, "" for none
    pub key: String,
    pub relay: Mutex<relay::Relay>,
    pub identities: Arc<Mutex<identity::Claims>>,
    pub saves: Option<saves::Saves>,
    pub reports: Option<reports::Reports>,
    pub lan: lan::Switch,
}

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port = match args.get(1).map(|p| p.trim().parse::<u16>()) {
        None => 0,
        Some(Ok(p)) => p,
        Some(Err(_)) => {
            eprintln!("carbon-server: the port must be a number from 0 to 65535, not {:?}", args[1]);
            std::process::exit(2);
        }
    };
    let host = args.get(2).cloned().unwrap_or_else(|| "127.0.0.1".into());
    let saves = env("CARBON_SAVES_DIR").map(|dir| {
        let total = saves::total_from(env("CARBON_SAVES_MAX_MB").as_deref());
        saves::Saves::open(PathBuf::from(&dir), total).unwrap_or_else(|e| {
            eprintln!("carbon-server: cannot use the saves folder {dir}: {e}");
            std::process::exit(1);
        })
    });
    let reports = env("CARBON_REPORTS_DIR").map(|dir| {
        let total = reports::total_from(env("CARBON_REPORTS_MAX_MB").as_deref());
        reports::Reports::open(PathBuf::from(&dir), total).unwrap_or_else(|e| {
            eprintln!("carbon-server: cannot use the reports folder {dir}: {e}");
            std::process::exit(1);
        })
    });
    let identities = Arc::new(Mutex::new(
        identity::Claims::open(
            env("CARBON_MESSENGER_DIR").map(PathBuf::from),
            env("CARBON_STATS_DIR").map(PathBuf::from),
        )
        .unwrap_or_else(|e| {
            eprintln!("carbon-server: cannot protect existing names: {e}");
            std::process::exit(1);
        }),
    ));
    let trace = trace::Trace::new(env("CARBON_TRACE_DIR").map(PathBuf::from), identities.clone());
    let app = Arc::new(App {
        key: env("CARBON_RELAY_KEY").unwrap_or_default(),
        relay: Mutex::new(relay::Relay::default()),
        identities,
        saves,
        reports,
        lan: lan::Switch::new(trace),
    });
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .max_blocking_threads(4)
        .thread_stack_size(256 * 1024)
        .thread_keep_alive(Duration::from_secs(10))
        .build()
        .expect("tokio runtime");
    runtime.block_on(serve(app, host, port));
}

async fn serve(app: Arc<App>, host: String, port: u16) {
    let listener = match TcpListener::bind((host.as_str(), port)).await {
        Ok(l) => l,
        Err(e) => {
            eprintln!("carbon-server: cannot listen on {host}:{port}: {e}");
            std::process::exit(1);
        }
    };
    let port = listener.local_addr().map(|a| a.port()).unwrap_or(port);
    println!("relay on ws://{host}:{port}/<room>");
    if let Some(s) = &app.saves {
        println!("cloud saves in {} ({:.1} MB used)", s.dir.display(), s.used_mb());
    }
    if let Some(r) = &app.reports {
        println!("problem reports in {} ({:.1} MB used)", r.dir.display(), r.used_mb());
    }
    println!("lan switch on ws://{host}:{port}/lan; trace host 10.77.0.1: {}", app.lan.describe());
    // The relay's picks, held-back far poses and map feed (relay.rs), on the same thread too.
    let relay = app.clone();
    tokio::spawn(async move {
        let mut every = tokio::time::interval(relay::TICK);
        every.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            every.tick().await;
            util::lock(&relay.relay).tick();
        }
    });
    // The race hosts' timers (CommUDP resends, link syncs), on the same thread as the rest.
    let ticker = app.clone();
    tokio::spawn(async move {
        let mut every = tokio::time::interval(Duration::from_millis(lan::TICK_MS));
        every.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            every.tick().await;
            ticker.lan.tick();
        }
    });
    loop {
        match listener.accept().await {
            Ok((stream, remote)) => {
                tokio::spawn(http::serve(app.clone(), stream, remote));
            }
            Err(e) => {
                // Out of file descriptors, most likely: wait instead of spinning.
                eprintln!("accept: {e}");
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
        }
    }
}
