# carbon-server

The online server for the browser port of Need for Speed: Carbon, in Rust. One small binary on one port:

- **Free-roam relay**: a drop-in replacement for `tools/carbon-net-relay.mjs`. It has the same command line, first output line,
  environment, WebSocket protocol (hi, pose, chat, horn, crew, physics, impact, race), limits and log lines, plus `GET /stats`.
  The Node relay's header comment stays the protocol reference. Both send poses and the map feed as compact binary frames
  (46 bytes a pose instead of about 180 of JSON) to pages that ask for them in their hi ("Compact poses" there), and JSON
  to the others, byte for byte the same.
- **Cloud saves**: `/saves/<id>` with `CARBON_SAVES_DIR` set. The status codes, CORS/CORP headers, ETags, conditional
  writes, DELETE, size and rate limits, atomic writes and stale-temp cleanup are the same as the Node relay's.
- **Problem reports** (this server only, not the Node relay): `POST /reports?key=<key>` with `CARBON_REPORTS_DIR` set,
  the game page's "Send report" (`source/web/report.js`). The body is UTF-8 text of 1 byte to 512 KiB, stored as
  `<dir>/<UTC yyyymmdd-hhmmss>-<6 hex>.txt` (mode 0600) after a "Report received <time>" line. The answer is 201
  `{"id":"…"}`. Limits: 3 per client, then one per 10 minutes (429 with Retry-After, exposed to pages); all reports
  together under `CARBON_REPORTS_MAX_MB` (default 200; 507 when full); 413, 400 (not UTF-8), 403 (key), 501 when off.
  No address is stored: the per-client limit keeps addresses in memory only. Read them with `tools/carbon-reports.sh`.
  The Node relay runs on the developer's Mac, where the page's log is at hand (Download log), so it has no /reports
  (its header says so).
- **`GET /lan`**: the switch of the virtual network that the game's own Online mode talks through
  (`plans/lan-protocol.md`, section 5). The trace host at 10.77.0.1 records what the game sends to EA's servers, and
  stand-ins for EA's FESL, Theater and Messenger answer on their ports, so the game's own Online mode signs in under
  any name and shows its online menus. With `CARBON_HOST_GAME=1` the server is also the race host EA ran (the client's
  "rb"), so players race each other through the game's own Custom Match. EA Messenger works through the game's own
  screens too: friends, presence, private chat and game invites (`messenger.rs`, below), and so do its Statistics
  screens: Player Stats and the Leaderboard (`stats.rs`, below).

Dependencies: tokio, httparse, serde_json, sha1 and sha2. Everything runs on one thread, plus a few helper threads for
file work. The source is `src/`, with comments at the top of each file:

| File | What it does |
|---|---|
| `main.rs` | Command line, environment, listening |
| `http.rs` | HTTP/1.1: requests, keep-alive, `/stats` |
| `ws.rs` | Upgrades, frames, send queues, the read loop with pings |
| `relay.rs` | Rooms |
| `saves.rs` | Cloud saves |
| `reports.rs` | Problem reports (POST /reports) |
| `lan.rs` | The switch |
| `trace.rs` | The trace host |
| `ea.rs` | The stand-ins for EA's FESL, Theater and Messenger |
| `messenger.rs` | EA Messenger: friends, requests, blocks, presence, chat, game invites; who may use a name |
| `stats.rs` | Player Stats and the Leaderboard: FESL's rank service, and what the race host's races count |
| `tunnel.rs` | DirtySock's ProtoTunnel: RC4 keyed by the game's EKEY, the datagram format |
| `host.rs` | The race host: CommUDP, NetGameLink, GameManager's join, the race messages |
| `olmsg.rs` | The race messages (`OLMSGType_*`): bit streams, types, the few the host builds |

## Build and run

```sh
cd server
cargo build --release
./target/release/carbon-server 0 127.0.0.1     # port 0: any free port
# relay on ws://127.0.0.1:61234/<room>
# lan switch on ws://127.0.0.1:61234/lan; trace host 10.77.0.1: summaries only (set CARBON_TRACE_DIR for files);
#   FESL, Theater and Messenger stand-ins on 18210, 18215, 13505
```

Usage is `carbon-server [port] [host]`; the port defaults to 0 and the host to 127.0.0.1. Players use it as they use the
Node relay: `?mp=ws://127.0.0.1:61234/<room>`. Environment variables (an empty value counts as unset):

| Variable | What it does |
|---|---|
| `CARBON_RELAY_KEY` | WebSockets (rooms and `/lan`) and `/saves` must carry `?key=<it>`; otherwise 403. |
| `CARBON_RELAY_BIN` | `0`: no compact (binary) poses; every page gets JSON, as from a relay before 2026-10-25. Default `1`. |
| `CARBON_SAVES_DIR` | Turns cloud saves on, stored in that folder. Without it, `/saves/*` answers 501. |
| `CARBON_SAVES_MAX_MB` | All saves together, default 1024. |
| `CARBON_MESSENGER_DIR` | Keeps EA Messenger's names, friend lists, pending requests and blocks in `messenger.json`, and explicit name claims in `names.json`, so they survive a restart. Without it they last until the server stops. |
| `CARBON_STATS_DIR` | Keeps Player Stats and the Leaderboard in `stats.json` in that folder, so they survive a restart. Without it they last until the server stops. |
| `CARBON_TRACE_DIR` | Where the trace host writes one file per connection or UDP flow. Without it, the trace host and the stand-ins still answer and print summary lines. |
| `CARBON_HOST_GAME` | `1`: PlayNow (Custom and Quick Match) sends players to a game the server hosts (id 200000 and up, 10.77.0.1, UDP port 19000 and up), which runs the race (`host.rs`). Without it nobody hosts, and Custom Match ends with "Could not create game session". |
| `CARBON_TRACE_GAME` | `1`: when no player's game is open, PlayNow sends players to a game "hosted" by the trace host (id 100000, 10.77.0.1:1042), so what a joining game sends its host is recorded. For tracing only. |

The path `/lan` belongs to the switch, so a free-roam room cannot be named "lan".

## Tests

```sh
cargo test                                   # unit tests, plus tests/lan.rs: the switch, the trace host, a fake
                                             # game signing in to the EA stand-ins, two fake games meeting in a
                                             # Custom Match (PlayNow, CGAM, GLST/GDAT, EGAM -> EGRQ -> EGRS ->
                                             # EGEG, PENT), the trace host's game and the race host's (EGEG,
                                             # the tunnel's first datagram, CommUDP CONN), a game invite's
                                             # lookups (GDAT USER=, EGAM UID=), EA Messenger (requests,
                                             # presence, chat, name claims, a restart), Player Stats and the
                                             # Leaderboard (kept, ranked, a restart), through the binary;
                                             # tests/reports.rs: POST /reports (key, limits, no address stored)
CARBON_RELAY_CMD=$PWD/target/release/carbon-server node --test ../tools/tests/carbon-net-relay.test.mjs
```

`CARBON_RELAY_CMD` makes the relay's own JS tests start this binary instead of the Node script.

**Run the JS tests with Node 26.** With Node 22.20, the test "authentication and the eight-player limit" fails against
both servers. That is a bug in Node 22's built-in WebSocket client (undici 6.21.2): when the server refuses the handshake
it fires `error` but never reaches CLOSED, however the server refuses (closing the connection, or answering 403 or 503).
With Node 26.5 (undici 8.7) all tests pass against both servers.

A server started with port 0 picks a free port, and each test stops its own.

## Cross-build for Linux x86_64 (the droplet) from this Mac

No new software is needed, only a rustup target:

```sh
rustup target add x86_64-unknown-linux-musl
cargo build --release --target x86_64-unknown-linux-musl
file target/x86_64-unknown-linux-musl/release/carbon-server
# ELF 64-bit LSB pie executable, x86-64, static-pie linked, stripped   (about 1.8 MB)
```

How it links:

- The musl target brings its own libc and start files.
- `.cargo/config.toml` links with `rust-lld`, the linker shipped with every Rust toolchain.
- None of the crates compiles C, so no C compiler for Linux is needed.

The result is one static file with no dependencies on the droplet's libraries. Tested on Linux x86_64 (2026-10-25, in
Docker on this Mac, `--platform linux/amd64`): the shared suite passes 29/29 with Node and the binary in one
`node:26-alpine` container (mount `tools/` and `source/web/`, which the game-worker tests load), the same as the Mac
build of the same source; two browsers raced each other through the game's own Custom Match against it; and the deploy
script's on-droplet steps that need no systemd (user, trial start, `/stats`, the key, the listening check) ran in
`ubuntu:24.04`:

```sh
docker run --rm --platform linux/amd64 -v "$PWD/../tools:/w/tools:ro" -v "$PWD/../source/web:/w/source/web:ro" \
  -v "$PWD/target/x86_64-unknown-linux-musl/release/carbon-server:/w/carbon-server:ro" \
  -e CARBON_RELAY_CMD=/w/carbon-server node:26-alpine node --test /w/tools/tests/carbon-net-relay.test.mjs
```

If it ever fails on the droplet, the usual alternatives are building there (`cargo build --release`) or
`cargo-zigbuild` (that needs zig installed).

## Memory

RSS measured on macOS (aarch64, release build); expect about the same on Linux with musl:

| Situation | RSS |
|---|---|
| Idle | 2.2 MiB |
| 8 drivers at 30 poses a second, plus 4 LAN browsers sending UDP to each other at 60 Hz and TCP to the trace host | 3.0 MiB |
| After an 8 MiB cloud save is written and read | 3.3 MiB |
| The relay load test's flood scenario (160,000 poses, one stalled reader) | 2.7 MiB |
| The same flood on the Node relay, for comparison | 57–97 MiB |

A save is streamed to and from disk in 64 KiB pieces, never held in memory whole.

## The switch (`/lan`)

The full spec is `plans/lan-protocol.md`; this is what the server does:

- **Addresses.** Each WebSocket gets an address (vip) in 10.77.0.0/21, from 10.77.0.16 upwards. A new connection gets the
  next free address after the last one handed out, so a freed address is not reused at once. An address never ends in .0
  or .255.
- **Welcome.** The first message is `{"t":"lan","vip":"10.77.0.16","netmask":"255.255.248.0"}`.
- **Hello.** The page sends `{"t":"hello","name":..}`; the name is used only in the logs.
- **Frames.** Binary messages carry the 16-byte frame header, the 8-byte LAN header and the data.
  - Kinds 3 (UDP: data, unreachable) and 4 (TCP: SYN, SYNACK, RST, DATA, FIN) are routed by destination.
  - Anything else is dropped: other kinds, frames shorter than 24 bytes or longer than 16 + 1472, and frames to
    addresses nobody holds.
  - The source is always rewritten to the sender's own vip.
- **Broadcasts.** UDP to 10.77.7.255 or 255.255.255.255 goes to every vip except the sender.
- **Departures.** When a vip leaves:
  - each browser it had an open TCP connection with gets a TCP GONE frame per connection;
  - every browser gets `{"t":"gone","vip":..}`.

  The switch tracks connections between browsers from SYN until RST or both FINs, up to 256 per vip.
- **Limits.**
  - At most 8 `/lan` connections per client address. Behind nginx this is X-Real-IP, so locally it is 8 per machine.
  - Per connection, 2000 frames and 1 MiB as a burst, then 1000 frames and 512 KiB a second. A broadcast costs one frame
    per receiver. A sender over its budget is closed with 1008 "too fast".
  - A receiver more than 64 KiB behind loses datagrams. One more than 256 KiB behind is cut off, because TCP segments
    are never dropped.
- **Key and pings.** `CARBON_RELAY_KEY` applies, and so does the relay's ping/pong keep-alive.

### The trace host (10.77.0.1)

It stands in for every EA server: the kit resolves `*.ea.com` to 10.77.0.1.

- **TCP.** A SYN on any port is accepted with SYNACK. On the ports of EA's services (18210, 18215, 13505) the stand-ins
  below answer; on any other port data is recorded and never answered. A FIN is answered with FIN, and the connection
  is done. An RST ends it silently. DATA or FIN for a connection that does not exist gets RST.
- **UDP.** Datagrams are recorded and never answered, except Theater's `ECHO` on 18215 (below).
- **Files.** With `CARBON_TRACE_DIR` there is one file per TCP connection and one per UDP flow (address, client port,
  our port), each named after the time, the address and the ports. They contain a header, one line per segment or
  datagram, a `hexdump -C` style dump (hex plus printable characters), and the reason it closed. At most 1 MiB of bytes
  is written per file; the rest is counted.
- **Stand-in connections** are recorded a packet at a time, both ways (`IN` and `OUT`, each with a one-line summary and
  its hex dump), with password and CD-key values masked as `*`. Fragments of long transactions are listed without
  their data; the joined transaction is noted once.
- **stdout.** One line per connection when it ends, and one per datagram:

  ```
  trace: tcp 10.77.0.16:50123 > 10.77.0.1:18299 [1a2b3c4d] ended by FIN after 1.52 s: 2 segments, 26 bytes "CONN....hello, world!!more" -> 20261024-173856-00001-tcp-10.77.0.16-50123-to-18299-1a2b3c4d.txt
  trace: udp 10.77.0.16:3658 > 10.77.0.1:3658, 6 bytes "udp!.." -> 20261024-173856-00002-udp-10.77.0.16-3658-to-3658.txt
  ```

### The EA stand-ins (`ea.rs`)

Just enough of EA's online services for Carbon's own Online mode to sign in, look for a Custom Match and join a game
someone hosts (phase 4 of `plans/lan-protocol.md`). The game connects without SSL: the `core.nfsc` mod sets the FESL
SDK's own `SSL_OVERRIDE_OFF` option in LAN mode (`source/mods/core/nfsc/online_net.h`), and signs in by itself
under the page's player name.

**Players never host.** Carbon's client never creates a game: EA ran every race on its own host program (game lists
show `HN=nfsdevserver`, `B-version=298_prod_server+...`), and the client's PlayNow listeners ignore the "you host"
answer (`CREATE`). So without `CARBON_HOST_GAME`, Custom Match ends with the game's own "Could not create game
session" (PlayNow `NOMATCH`); with it, PlayNow answers `JOIN` with the server's own game (below).

- **Framing** (all three, checked against the game's code and its first packet in our trace): a 4-character type, a
  big-endian word (FESL: the transaction id, top bits 0x8 for a whole transaction and 0xb for a fragment; Theater and
  Messenger: 0), the big-endian length of the whole packet (12 to 0x8000), then `key=value` lines and a NUL. A stream
  with a length outside that range is reset (RST).
- **Optional name claims.** A new name remains open until a player claims it. During migration, every existing
  Messenger or Player Stats name is first reserved with its old browser-key fingerprint. This stops another player
  from typing an old name, claiming it, and inheriting its friends or statistics. The old browser can claim the
  existing name from the start page with a cloud-save keyphrase. The browser sends only the opaque cloud-save id as
  proof; the server never receives the keyphrase and stores only a separate SHA-256 digest in `names.json`. A claim
  protects the same name-keyed records. It does not copy, rename or clear them. The server also checks protection in
  free roam. Passwords and CD keys are dropped when a packet is parsed, never logged and masked in trace files.
- **FESL, TCP 18210:**
  - `fsys`: Hello (the Theater and Messenger addresses, then a server MemCheck with an empty list, as EA's server
    did), MemCheck (the game's answer, not answered), Ping, GetPingSites (three sites at 10.77.0.1,
    `minPingSitesToPing=0`), Goodbye (the stand-in then closes its side).
  - `acct`: Login (`lkey`, `userId`, `profileId`, `displayName`), GetSubAccounts, LoginSubAccount.
  - `dobj` GetObjectInventory (no entitlements), `mtrx` ReportMetrics.
  - `rank`: UpdateStats, GetStats, GetRankedStats, GetStatsForOwners, GetRankedStatsForOwners, GetTopN and
    GetTopNAndStats, answered from `stats.rs` (below).
  - `pnow` (PlayNow, the matchmaker of Custom and Quick Match): Start answers with a session id, then an async Status:
    `JOIN` with an open game (another player's, joins open, room left, the asked game type), else `NOMATCH`. Cancel.
  - Long transactions arrive in fragments (`size=`, `data=` base64 with `=` written `%3d`) and are answered once
    whole.
  - Anything else gets a FESL error (`errorCode=99`) and an `ea:` line on stdout.
- **Theater, TCP 18215:** CONN, USER (the name from the login key), LLST (one lobby, "Internet", id 257, plus its
  LDAT), PCNT (players signed in), and the games:
  - CGAM makes a game (id, `EKEY` tunnel key, UGID, SECRET; a host has one), UGAM/UGDE update its attributes, UBRA
    brackets are answered when they close, RGAM ends it; it also ends when the host's connection does.
  - GLST lists the games (one GDAT each, EA's fields: host name and id, `I`/`P` = the host's vip and game port,
    players, `B-*` attributes); GDAT for one game adds GDET (UGID, `D-*`).
  - Joining: the joiner's EGAM is answered and passed to the host as EGRQ (name, id, PID, TICKET, the joiner's vip and
    ports); the host's EGRS `ALLOWED=1` sends the joiner EGEG (the host's address, PID, TICKET, the game's EKEY and
    UGID), anything else EGEG with a REASON. PENT/PLVT (the host: a player is in / left), ECNL (the joiner gives up).
  - A game invite (EA Messenger) names its sender, not the game: the invited game asks for the sender's game by
    name (GDAT `USER=Metin TYPE=G`) for the invite screen, then joins it by the sender's user id (EGAM
    `UID=... R-UID=... TYPE=G`, no LID or GID). Both find the race host's room the sender is in, or a game it hosts
    or was let into; the join then goes on as any other. The lookup is answered with GDAT (AP = the players listed;
    a race host's room adds its settings as `B-U-*` and its max_online_player as MP), GDET, then one PDAT per player
    (LID, GID, PID, NAME, UID): the game's SDK keeps the request open until it has AP of them (0x952260), and only
    then fills the invite screen (Game Mode, Game Type, Number of Players, Collision Detection, Track Name, Number
    of Laps; "--" for a track left at "Any").
  - UDP `ECHO` on 18215 is answered over UDP with `IP` and `PORT` = the vip and port it came from, `ERR=0`, as EA
    did for NAT; the game's GameManager needs it before PlayNow.
  - Others are logged, not answered.
- **Messenger, TCP 13505:** AUTH (the name from the login key), EPGT/EPST, PING here; everything else in
  `messenger.rs` (next section).
- **Keep-alives.** A connection quiet for 60 s gets a ping, as EA's servers sent them: FESL `fsys` Ping (not a
  transaction, id 0; the game answers with id 0x80000000), Theater and Messenger `PING` (the game answers `PING`).
  The game drops its FESL connection after 180 s without traffic, and a race sends FESL nothing ("You have lost
  your connection to EA Nation" three minutes into a race).
- **Answers to other connections** (EGRQ to the host, EGEG to the joiner) go to that connection's trace file too,
  marked `for <the connection that caused it>`. A joined long transaction is written to the file in full, a line per
  field.
- **stdout:** one line per packet, for example
  `ea: fesl 10.77.0.16:50359 > acct 0x80000002 {TXN=Login, ..., name=carbonfan, password=(not kept), ...}` and
  `ea: fesl 10.77.0.16:50359: carbonfan logged in (userId 804554673)`.

The replies follow EA's own, as the March 2021 captures that ship with xan1242's NFSC_MasterServer show them. They
were used as reference data only; none of that project's GPL-3 code is in this crate.

### EA Messenger (`messenger.rs`)

The game's own Messenger screens (the M key in the online menus, "EA Messenger" in Game Setup), with EA's commands
(FESL SDK 2.9 Communicator; `plans/lan-protocol.md` has the fields):

- **Friends.** Search (USCH) finds names this server knows. A friend request (RADM) shows the other player the
  game's "Metin wants to be your Friend" and a "?" in its list; its answer (RRSP: accept, decline, block) and
  removals (RDEL) reach both players' lists at once (RNOT). Lists come with RGET (ROST: `ATTR=AT` a friend, `RT` a
  request to this player, `ST` one it sent). Block (RADD `LIST=I`, or Block in the request's dialog) also ends a
  friendship; the blocked player's later requests are kept nowhere, and chat and invites need a friendship.
- **Refusals** are the answer's error word, which the game shows as its own text: a friend request to a player who
  blocked the sender `blck` ("User has declined your friend request.", as EA's server answered), to oneself
  `user` ("You cannot Add or Block yourself."), past the limits `full` ("Your Friends list is full." / "Your
  blocked list is full."); chat faster than the limit `rate` ("You cannot send a message that often.").
- **Presence.** What a game sets (PSET) reaches its friends (PGET): "Online", "Online playing Need for Speed Carbon"
  in a game room, "Not Signed In" once its page is closed.
- **Chat** (SEND -> RECV) goes between friends only, live: never stored, and `BODY`/`SUBJ` are shown as
  `(private)` on stdout and in trace files.
- **Game invites.** "Send Game Invite" (offered by the game in an unranked room with space) reaches the friend as
  the game's own "Metin wants to play." and "Read Game Invite"; Accept joins the room through Theater (above),
  Decline shows the sender "Jan has declined your Game Invite."
- **Same name twice.** A second Messenger sign-in under a name already online logs the first off (ADMN DUPL, the
  game's "Multiple log-ins detected"). A reserved old name needs its original browser until it is claimed. A claimed
  name needs the matching cloud-save proof and can then be recovered in another browser with the keyphrase.
- **Display names.** lan.js makes a random player key per browser (localStorage `carbon-player-key`) and sends it in
  the `/lan` hello. The server keeps only its digest. During migration, that digest proves which old browser may
  convert an existing name into an explicit claim. It is not used after that claim. A new name works without browser
  storage, but an old reserved name without its original browser needs manual recovery. The test-only `?testing&pk=`
  sets a key from the address and is never stored.
- **Limits.** 100 friends, 50 open requests and 100 blocks a player; chat 6 messages, then one a second; invites
  3, then one every 5 s; 256 characters a message. 10,000 Messenger names and 10,000 explicit claims are kept.
  A new Messenger name can replace the longest-unseen name that has no recent browser metadata, friends, requests
  or blocks. An explicit claim is kept separately and is not removed by this cleanup.
- **Kept** (with `CARBON_MESSENGER_DIR`, in `messenger.json`, written whole through a temporary file): names, the
  SHA-256 of the latest browser key when supplied (never the key; it is not logged either), the last sign-in, friend
  links, open requests and blocks. No chat, no addresses.

### Player Stats and the Leaderboard (`stats.rs`)

The game's own Statistics screens (STATISTICS in the main menu, signed in): ONLINE PLAYER STATS and LEADERBOARD,
through FESL's `rank` service (`plans/lan-protocol.md` has the fields and addresses):

- **Player Stats** asks `GetStats` for Online_Rep_Level, Overall_Solo/Crew_On_Wins/Losses, Best_Online_Win_Streak,
  Challenge_Series_Beat_Online, DNF and Total_Online_Games_Started/Finished, and shows them as Online XP Level, Total
  Ranked Games, Ranked Wins, Ranked Losses, Win %, Best Winning Streak, Online Challenge Series Won and Total
  Disconnect %. Enter on a leaderboard row shows that player's.
- **The Leaderboard**, per race, race type (Circuit, Sprint, ...) and time period (Weekly, All Time, Daily), asks
  `GetRankedStats` for the player's own rank on `Evt_Bst_<race>`, then `GetTopN` for the rows from that rank and for
  the leader; Friends Only asks `GetRankedStatsForOwners` for the player and its Messenger friends. `<race>` is the
  Attrib key of the race id (`mu.1.1`, Beachfront, is `30F07253`). A row is the time and a text "car, two numbers, 0"
  (a circuit's best lap and top speed, a sprint's top speed and NOS), shown as Total Time, Best Lap, Top Speed and
  Car Name.
- **What counts.** The game never sends the Player Stats keys, and in our unranked rooms it sends no race results:
  EA's servers counted them from what EA's race host reported. So the race host reports each Custom Match race
  (who started it; at the end every game's GameResults) and the server counts it: games started and finished, a win
  for first place and a loss for everyone else, win streaks, wins per game mode (Circuit_On_Wins ..., which the
  online reward cards read), the DNF share, and per race the best time with the car (Evt_Bst_<race>, circuits and
  sprints), the games and the wins. A race needs two players; leaving it or not finishing a sprint or circuit is a
  loss and a DNF. What the game itself sends (`UpdateStats`) is kept only for keys the online screens read (best
  times, Pursuit and Canyon stats); its career upload at sign-in (cars, cash, reward cards) is left out.
- **Who.** Players are kept by display name, as Messenger keeps friends. Claiming a name protects the existing
  record; it does not create a new record or clear statistics. Changing browsers or losing browser storage does not
  clear the name's statistics.
- **Kept** (with `CARBON_STATS_DIR`, in `stats.json`, written whole through a temporary file): per player its name,
  a 16-hex-digit fingerprint of the latest browser key's SHA-256 when supplied, the day it last signed in,
  and its stats (all time, and for leaderboard keys this and last day and week). No addresses. Limits: 10,000
  players and 256 stats each.

Not done: speed trap, Canyon and Pursuit leaderboards from our races (GameResults' values for them are not decoded;
what the game itself sends is kept), NOS on the sprint board (not in GameResults: 0), Online XP Level (0: the game
never sends it, and EA's points per place are unknown; the game's loading tip says a higher place earns more and
level 50 unlocks a Lotus Europa S, so a rule for it would unlock cars), Skill_Level (PlayNow's skill matching: 0).

### The race host (`host.rs`, `CARBON_HOST_GAME=1`)

EA's "rebroadcaster", rebuilt from the game's own code (`plans/lan-protocol.md` has the addresses). PlayNow puts a
player into the first served game with room (a new one when all are full): Theater's EGEG names 10.77.0.1, the
game's UDP port (19000 + n), `HUID` (the host's user id, "RaceHost") and a fresh `EKEY`. The game then talks to that
port through the switch, and the host answers from it. Outside in:

- **ProtoTunnel** (`tunnel.rs`): RC4 keyed by the EKEY's first 16 characters (the game's second try inverts the
  key; both are taken), one stream per direction; a datagram is the sender's stream position, 2-byte sub-packet
  headers, then the data. Port index 1 is GameManager's, 7 the tunnel's control record (client id, "I hear you").
- **CommUDP** (`[seq][ack][body]`): INIT answered with CONN, reliable packets resent after 300 ms and acknowledged,
  NAK for gaps, bundled older packets read.
- **NetGameLink**: `[payload][12-byte sync][kind]`; the host sends a sync every 400 ms, which keeps the game's 10 s
  link timeout away.
- **GameManager**: hello (its ticket must be one Theater gave out), host hello (client-server, the room's size,
  game invites allowed), the roster, roster ack, then a join announcement to everyone.
- **A new room starts unranked.** The first player's game takes the settings it is sent on joining (0x827e30)
  before it sends its own; with none, its settings said game_type 0, so the first room after signing in was
  "Ranked Match" (no game invites) whatever GAME TYPE showed. A new room's settings are the game's own defaults
  for a new room (Sprint, any track, 2 players, NOS and collision on) with game_type 1.
- **Race messages** (`olmsg.rs`, MSB-first bit streams): GameInitInfo (the player's racer ids), clock sync,
  PlayerCarData sent back to everyone with the player's join order (the lowest is the room's leader), the leader's
  GameAttributes with `game_type` made 1 (unranked; ranked needs skill lookups), Start_Timer to everyone, one
  StartLoading to everyone at the first request (a game told twice crashes), Ready counted, then StartRaceSyncBegin
  to each in its own clock. CarState goes on unreliably; anything else goes on as it came. RacerFinished and
  LeaderFinished are logged as results (`result: 1. Jan finished`); once every game has sent its GameResults (or
  10 s after the first) everyone gets one FinalGameResults, which the games show as the race's standings.
- **Pursuit Tag** (game_mode 14): one player is the racer, the others cops. A role change (PursuitTagSync: the racer
  busted, or a minute evaded) swaps both cars in every game as a toggle, and the game that saw it swaps its own
  only when the message comes back. So the host sends each change to everyone, the sender too, keeps track of who
  the racer is, and drops a change from a player who is no longer the racer (a second report of the same bust).
  Without that, each game counted the time as the racer for the wrong player. The racer's own timer messages go
  to the others only. Log: `Pursuit Tag: Metin was busted; Jan is the racer`.
- **Canyon duel** (game_mode 13): two heats. A game's heat ends only when its own "heat over" PursuitTagSync
  ([racer][-4]) comes back, so that one goes to everyone, the sender too; the gap score goes to the other game only.
- **Other modes** need nothing of their own: Circuit, Sprint, Speedtrap and Pursuit Knockout (its eliminations
  and scores are each set by one game and passed on). "Any" is settled by the leader's game, which sends its
  choice of mode and race before loading; should one still be "Any" at StartLoading, the host picks one the room's
  PlayNow offered (Canyon only for two players).
- **Joining while a race runs:** PlayNow gives the player a room of its own (it waits there as its leader). A join
  that still reaches a room whose race is on gets the game's own refusal: its join announcement only, then
  GameInitInfo with "refused" set, and the room screen says "You can not join this Game Room because the match has
  already started." The racers never hear of it.
- **Stats.** When a race starts (StartRaceSyncBegin) and when FinalGameResults goes out, the host reports it to the
  stats store (`stats.rs`, above): who started, and every game's GameResults.
- **After a race** a game is on the results until its game says something only the room says (GameAttributes,
  ActiveGame_Message, Matchmaking/Invites/Joins requests, Start_Timer, PlayerCarData); other players' room news
  (ActiveGame_Message) waits for it until then.
- **stdout:** `host <gid>: ...` lines (joins, the room, loading, the start, results); with `CARBON_TRACE_DIR` a file
  `host-<gid>-udp-<port>` holds every decrypted race message.

Not done: FinalGameResults' host part (written as zeros: no rankings here), voice, joining a race that has started
(players wait in a room of their own instead), and host-side checks of what players send. FinalGameResults is the leader's report as it is (the games'
own reports agree to a few hundredths of a second once Pursuit Tag's changes go to everyone).

## Where it differs from the Node relay

None of these differences show in the tests or in the page's normal use.

- **Request targets.** One that starts with `//` is answered 400 (Node reads what follows as a host name). So is any
  target that is neither a path nor an absolute `http(s)://` URL.
- **Request bodies.** A request with `Transfer-Encoding` is answered and its connection closed: the body is never read.
  A PUT without Content-Length gets the same 411 as before.
- **Names and crews.** Letters are Rust's Alphabetic, which keeps combining vowel signs (Devanagari and others) that
  JavaScript's `\p{L}` drops.
- **JSON.** A message with a number outside the double range (`1e400`) or a lone surrogate escape is ignored as invalid
  JSON. Node would read Infinity, or a lone surrogate.
- **Numbers passed through** (s, ts) are written back as sent. `1e2` comes back as `100.0`, not `100`: the same number
  to `JSON.parse`.
- **A stalled reader** is removed when one write to it takes 15 s, rather than after 15 s of a socket blocked since its
  last drain.
- **Output.** There is one more start-up line (the switch), and the `lan:` and `trace:` lines.

## Deploying to the droplet

`tools/carbon-relay-deploy.sh` (run by the user, from this Mac) installs this binary on the droplet, in place of the
Node relay:

- It builds the Linux binary here (above), checks the droplet is x86_64, copies the binary to
  `/opt/carbon-relay/carbon-server` and tries it once as the `carbon` user on a free loopback port before anything is
  replaced.
- The systemd service keeps its name, `carbon-relay`: `ExecStart=/opt/carbon-relay/carbon-server 8793 127.0.0.1`,
  `CARBON_HOST_GAME=1`, cloud saves in `/var/lib/carbon-relay/saves`, problem reports in
  `/var/lib/carbon-relay/reports`, EA Messenger's names and friend lists in `/var/lib/carbon-relay/messenger`, Player
  Stats and the Leaderboard in `/var/lib/carbon-relay/stats`.
  `CARBON_TRACE_DIR` and `CARBON_TRACE_GAME` are unset (`UnsetEnvironment`, and removed from the key file), so
  nothing a player sends in play is written to disk. Only a save or a report the player chose to send is written,
  Messenger's list of names and friends (no chat, no addresses), and each name's race results and best times.
- The nginx site is `server.nfscrb.xyz` since 2026-10-25 (`DOMAIN` in the script; its DNS record goes in Cloudflare,
  DNS only; Vercel is only the domain's registrar). The earlier `drive.metinjakupi.dev` site and
  certificate are left in place and proxy to the same service, so pages opened before the move keep working.
- `MemoryMax=64M`, `TasksMax=16`, and the sandbox: read-only system, no capabilities, private `/tmp` and devices,
  and `IPAddressDeny=any` with `IPAddressAllow=localhost` (only nginx on the droplet talks to it).
- The key file `/etc/carbon-relay.env`, the saves folder and the nginx site stay as they were. The site's
  `location /` passes WebSocket upgrades, so `/lan` works through it.
- At the end it checks that the service answers `/stats` and listens on `127.0.0.1:8793` and nothing else (it stops
  the service otherwise).

**Ports.** The only socket this program opens is the TCP listener on the given host and port (`main.rs`). EA's
stand-ins (FESL 18210, Theater 18215, Messenger 13505), the trace host and the race host (UDP 19000 and up) are
addresses and ports of the virtual network at 10.77.0.1. They are reached only as frames on a `/lan` WebSocket, so
they are never ports of the machine. Checked on Linux and macOS with `lsof`/`ss`: one TCP listener, no UDP socket.

**The public site** (https://nfscrb.xyz, the user's decision 2026-10-25) is deployed separately by
`tools/carbon-site-deploy.sh` (see `plans/public-site.md`). It serves its own copy of online.js with this server's
address and key, so the key is public now. It no longer keeps anyone off the server, but the per-player limits still
apply. Its pages reach this server cross-origin: the `Access-Control-Allow-Origin: *` and
`Cross-Origin-Resource-Policy: cross-origin` headers (`/stats`, `/saves`, `/reports`) are what make that work under the
site's COEP. Deploy order after a server change: `carbon-relay-deploy.sh`, then `carbon-site-deploy.sh` (it checks the
server from the site's side at the end).

**Going back to the Node relay**, if ever needed: the script keeps the Node relay's unit as
`/opt/carbon-relay/carbon-relay.service.node` (and `relay.mjs` stays in that folder). Copy it to
`/etc/systemd/system/carbon-relay.service`, then `systemctl daemon-reload && systemctl restart carbon-relay`. The Node
relay has no `/lan`: the game's own Online mode needs this server.
