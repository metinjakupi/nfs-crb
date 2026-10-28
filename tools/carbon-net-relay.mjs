#!/usr/bin/env node
// carbon-net-relay.mjs - passes car positions between players' browsers.
//
// Usage: node tools/carbon-net-relay.mjs [port] [host]
//   port 0 (the default) takes any free port; host defaults to 127.0.0.1.
// Players open the game with ?mp=ws://<host>:<port>/<room>; a room holds up
// to CARBON_ROOM_MAX players (default 256; the next is refused), and each player
// sees the players nearest to it (see "Interest" below). Nothing of play is
// stored (only sealed cloud saves, see below), and no game data passes through:
// only names and car poses (up to 30 small messages a second each).
// With CARBON_RELAY_KEY set in the environment, a connection must carry
// ?key=<that key> or it is refused.
//
// Messages are JSON text frames, but poses and the map feed travel as binary
// frames for a page that asks for them in its hi (see "Compact poses" below).
//   browser -> relay: {t:"hi", name, crew, bin, cops, drive}  {t:"pose", k, s, m, ts}
//                     {t:"chat", text}  {t:"horn", on}  {t:"crew", crew}
//                     {t:"follow", ids}  {t:"cophit", to, c, seq, k, ts, s}  a cops frame
//                     {t:"pursuit", to, k, a, b, f}  {t:"lights", s}
//                     {t:"ping", n}  {t:"street", to, k, r, a, f}
//   relay -> browser: {t:"welcome", id, bin, cops, drive, peers:[{id, name, crew}]}  {t:"join", id, name, crew}
//                     {t:"pose", id, k, s, m, ts}  {t:"left", id}
//                     {t:"chat", id, text}  {t:"horn", id, on}  {t:"crew", id, crew}
//                     {t:"where", p:[[id, x, z, heading], ...]}  {t:"pursuit", id, k, a, b, f}
//                     {t:"cophit", id, c, seq, k, ts, s}  a cops frame  {t:"lights", id, s}
//                     {t:"pong", n}  {t:"street", id, k, r, a, f}
//   plus plain HTTP GET /stats, the start page's player count.
// crew is a short shared name ("" for none): players who give the same one
// are a crew (case and spaces do not count: "Night Owls" = "nightowls").
// chat text is at most 120 characters, horn on is true while the player holds
// the horn; both are limited per player (see LIMITS).
// k is the car's model key, s the 13 pose numbers (quaternion, position,
// linear and angular velocity), m what the player is doing (0 none, 1 driving),
// ts the sender's clock in milliseconds.
// welcome, join, left, crew, physics and race go to the whole room: every
// player knows everyone's name and crew. Poses, looks, chat and the horn go
// only to the players near the sender (below).
//
// Interest (rooms of a hundred and more; the idea is nfsu2-web's free-roam hub,
// see server/NOTICE). Every INTEREST_MS (500 ms) the relay picks, for each
// player, the players whose cars it gets: at most CARBON_INTEREST_K (default
// 24) of them, in this order:
//   1. the ones it follows ({t:"follow", ids}: the players racing with it),
//   2. its crew-mates within CREW_RANGE (1000 m), nearest first, CREW_MAX (16) at most,
//   3. everyone else, nearest first.
// "Near" is a player's car position (s[4..6]) carried on 1 s and 4 s by its
// speed (s[7..9], at most 100 m/s), whichever is nearer, so a car being driven
// toward counts before it arrives. Only a player driving now (its newest pose
// is m 1 and at most ACTIVE_MS, 5 s, old) has a place; the others come last
// (those already picked first, then in the order they came; a player that has
// never driven picks this way too). A player already picked counts at a quarter of its
// distance for HOLD_MS (5 s), then at half: a newcomer must be much nearer to
// take its place, so the cars shown do not flap. Rooms of up to K + 1 players
// work as before this: everyone gets everyone.
//   - Poses go only to the players that picked the sender: every one while the
//     two cars are within FULL_RANGE (500 m; the game builds a car within 300 m
//     and removes it beyond 500 m) or the receiver follows the sender; at most
//     one per MID_MS (100 ms) within MID_RANGE (700 m), so a car coming nearer
//     is known well before it is built; beyond, at most one per FAR_MS (250 ms).
//     A pose held back that way is sent once that time has passed (checked every
//     FAR_MS), if no newer one came, so a player's last state arrives.
//   - When a player is picked, it gets the sender's newest look (if any) and,
//     if it is at most ACTIVE_MS old, its newest pose at once, look first, so
//     the car is built in its own look.
//   - {t:"look"} goes to the players that picked the sender; a player who
//     joins gets the looks (and poses) of the players it picks, right after its
//     welcome.
//   - {t:"chat"} and {t:"horn"} go to the players the sender picked, the
//     players that picked it, and its crew-mates anywhere in the room; a horn's
//     "off" also reaches everyone its "on" reached.
//   - {t:"follow", ids}: at most FOLLOW_MAX (8) relay ids (positive integers),
//     the players this one must get whatever the distance (race.js's racers,
//     through net.js); [] = none. Not passed on. Limit: burst 5, then 1 a second.
//   - Every WHERE_MS (1 s) each player gets {t:"where", p}: every player in the
//     room driving now ([id, x, z, heading]; the player itself too), its car's
//     place in whole metres (x and z of s[4..6]; y is up) and the way its nose
//     (the body's +z axis) points, in whole degrees from +z toward +x (0-359).
//     For the world map and the far players' places; sent only while someone
//     is driving.
// A player joining picks at once, and is added to the players that have room
// for more, so its first pose reaches them without waiting for a pick.
//
// Rival encounters (2026-10-27): hi {rivals:1} negotiates welcome {rivals:1}.
//   browser -> relay: {t:"rivals",ts,cars:[{c,k,stamp,s,look?},...]}
//   relay -> audience: the same frame with relay-owned id added.
// At most four distinct actors c; c, model k and appearance stamp are positive
// uint32 values. ts is uint32. s has 13 finite numbers, absolute value <= 1e6,
// with quaternion squared length 0.5..1.5. Optional look is standard base64
// encoding exactly 1076 bytes, or empty for stock appearance. Frames use the
// existing 4096-byte limit, with burst 10 then 15 per second. No persistence.
// The source owns AI, collisions and career results. Receiving free-roam games
// show at most eight visual copies and clear them on an empty frame, disconnect
// or three seconds without updates. The sender refreshes one look per frame,
// every two seconds, to stay within the frame limit. Older relays disable this
// feature through capability negotiation; ordinary player poses are unchanged.
//
// Compact poses (2026-10-25; source/web/net.js). Poses are nearly all of the
// traffic: up to 30 a second from each of up to K cars, about 180 bytes each as
// JSON. A page may ask for them as binary WebSocket frames of 46 bytes instead:
//   hi {bin:1}: the binary format the page reads and writes (a whole number from
//   1; pages from before send no bin). welcome {bin:1}: the format the relay uses
//   with it, the smaller of the two (this relay speaks 1); no bin in the welcome
//   (a relay from before, or one started with CARBON_RELAY_BIN=0) means JSON
//   only, as before. A page that got bin sends its poses as binary frames, and
//   the relay sends it every pose and the map feed as binary frames; all else
//   stays JSON. Both kinds of page share a room: the relay passes each pose on
//   in the kind each receiver reads (it makes each kind once per pose). It also
//   takes JSON poses from a binary page and binary poses from any page after its
//   hi. All numbers are little-endian.
//   POSE, both ways, 46 + 8 * n bytes (+ 8 with flags bit 3, see "Driving"):
//     0  u8      1
//     1  u8      flags: bit 0 m (1 driving), bit 1 ghost, bit 2 physics (relay ->
//                page: the sender has contacts; ghost and acks pass only then),
//                bit 3 drive (8 DRIVE bytes follow the acks; see "Driving"),
//                bits 4-7 n, how many acks follow (0-15)
//     2  u32     id: relay -> page, the sender's relay id; page -> relay, 0 (not read)
//     6  u32     k, the car's model key
//     10 u32     ts, the sender's clock in ms (from a JSON page: its ts >>> 0, the
//                number net.js writes into the game)
//     14 i16 x 4 the quaternion s[0..3], x 32767
//     22 i32 x 3 the position s[4..6] in mm, x 1000
//     34 i16 x 3 the speed s[7..9] in cm/s, x 100 (at most 327.67 m/s)
//     40 i16 x 3 the spin s[10..12] in 1/500 rad/s, x 500 (at most 65.534 rad/s)
//     46 n x {u32 source id, u32 seq}: the acks of a JSON pose (see Shared contacts;
//                the relay keeps the first ACKS_MAX and only those it may pass)
//     Each number is Math.round(x * scale) (halves up) held within +-32767 (i16)
//     or +-(2^31 - 1) (i32). A page quantises its car's own numbers (not the
//     rounded ones it would send as JSON): 1 mm, 1 cm/s, 1/500 rad/s, the turn
//     to about 1/30000 of a radian. A frame of another size or first byte, or
//     one before the hi, is ignored.
//   A JSON page gets a binary page's pose as the JSON pose above, with s = the
//   quaternion to 5 decimals (Math.round(n / 32767 * 1e5) / 1e5, as net.js rounds
//   it), then n / 1000, n / 100 and n / 500 (exact short decimals), ts and k as
//   whole numbers.
//   WHERE, relay -> page, 1 + 14 * count bytes: u8 2, then for each player u32 id,
//   i32 x, i32 z (whole metres), u16 heading (degrees), as in {t:"where"}.
//
// Shared contacts (optional; legacy clients keep the original pose protocol):
//   hi {physics:0|1} -> welcome {physics:1}; join/peers/pose include physics:1
//   for capable players. {t:"physics",on:boolean} changes capability after the
//   game loads; peers receive {t:"physics",id,on}. Pose adds ghost:boolean and
//   acks:[[source id,last applied impact sequence]], at most ACKS_MAX (8)
//   acknowledged sources; the relay only forwards sequences it delivered to that owner.
//   browser -> relay: {t:"impact",to,seq,k,ts,s:[at(3),dp(3),dv(3),dw(3)]}
//   relay -> target only: {t:"impact",id,seq,k,ts,s} (reliable, not coalesced).
//   Only the lower connection id may solve a pair. Both players must support
//   contacts, be driving solid cars in the same room, have poses <=1 s old,
//   and be within 20 m of the impact position. k must match the target car;
//   ts must be within 1 s of the source pose. seq is a strictly increasing
//   positive uint32 per connection. All 12 values must be finite; vector
//   lengths are limited to dp<=6 m, dv<=100 m/s, dw<=50 rad/s. Limit: burst
//   128, then 120 impacts/s per source. The owner applies a sequence once and
//   includes its acknowledgement with the resulting pose.
//
// Police chases (2026-10-26; source/web/net.js, the game side mods/core/nfsc/cops.h). Each game
// runs its own police; these let the players near a chased player see the cops chasing it, and
// let their hits on those cops count in the chase.
//   hi {cops:1}: the page reads and sends them. welcome {cops:1}: this relay passes them, for a
//   page that asked and also got compact poses (bin); without it (a relay from before, or no bin)
//   the page sends none and gets none. Pages that did not ask never get any.
//   COPS, both ways, a binary frame of 10 + 38 * n bytes, little-endian:
//     0  u8   3
//     1  u8   n, cops in this frame: 0 to COPS_MAX (8); 0 = the chase is over
//     2  u32  id: relay -> page, the chased player's relay id; page -> relay, 0 (not read)
//     6  u32  ts, the sender's clock in ms (the clock of its poses)
//     10 n x 38 bytes, one per cop:
//        +0  u8   the cop's number, 1-255, different for each cop in the frame (it keeps it
//                 while it stays in the chase)
//        +1  u8   flags: bit 0 the light bar is on, bit 1 the cop is chasing, bits 4-7 the
//                 game's siren state + 1 (0 = not known); passed on as they came
//        +2  u32  the cop car's model key
//        +6  the cop car's 13 pose numbers as in a POSE frame from its offset 14 (4 x i16
//            quaternion, 3 x i32 position mm, 3 x i16 speed cm/s, 3 x i16 spin 1/500 rad/s)
//   A page sends one while its player is chased, up to about 15 a second, the nearest COPS_MAX
//   cops, and one with n = 0 when the chase ends. The relay passes it on, with the sender's id
//   written in, to the players that picked the sender (Interest) and asked for cops: each frame
//   while the two cars are within MID_RANGE (700 m) or the receiver follows the sender, an
//   n = 0 frame to all of them. A reader that falls behind gets only the newest unsent frame of
//   each sender. A frame of another size, with a number 0 or used twice, or from a page that did
//   not ask, is dropped. Limit: burst 10, then 20 a second. The relay keeps the newest frame's
//   cops (number, model, place) to check hits against for COP_FRESH (1 s).
//   {t:"cophit", to, c, seq, k, ts, s:[at(3), dv(3), dw(3)]}: this player's car hit cop c of
//   player `to` (as shown here): where the cop car was (physics space, metres) and the change
//   its game's own contact solve made to its speed (m/s) and spin (rad/s). The relay passes it
//   to `to` alone as {t:"cophit", id, c, seq, k, ts, s} (reliable, never coalesced) when both
//   asked for cops, `to`'s newest cops frame is at most COP_FRESH old and has cop c with model
//   key k, the sender is driving with a pose at most 1 s old, `at` is within COP_REACH (25 m) of
//   that cop and of the sender's car, |dv| <= 100 and |dw| <= 50, all 9 numbers finite, and seq
//   (a positive uint32) is higher than the sender's last one passed on. Limit: burst 64, then 60
//   a second. The chased player's game applies it to the real cop unless it already saw the
//   sender's car touch that cop itself.
//
// Driving (2026-10-26; source/web/net.js; the game side is mods/core/nfsc/mp_drive.h). A page may
// send its car's controls with each compact pose, so the others' games drive their copy of the car
// as its owner does: wheel spin and tyre smoke, skids, brake and reverse lights, revs and gear
// changes, nitrous flames, the front wheels' steering (the pose still decides where it is).
//   hi {drive:1}: the page reads and sends them. welcome {drive:1}: this relay passes them, for a
//   page that asked and also got compact poses (bin); without it (a relay from before, or no bin)
//   the page sends none and gets none. Pages that did not ask never get any.
//   A POSE frame with flags bit 3 carries 8 DRIVE bytes after its acks (so 54 + 8 * n bytes):
//     +0 i8  steering x127 (-1 full left .. 1 full right)   +1 u8 gas x255
//     +2 u8  brake x255   +3 u8 handbrake x255
//     +4 u8  gear: bits 0-3 the game's gear number (0 reverse, 1 neutral, 2 first, ...), bits 4-7 0
//     +5 u8  flags: bit 0 nitrous on (pressed, with nitrous left); the other bits 0
//     +6 u16 the engine's revs, rpm
//   The relay keeps them with the pose and passes them on as they came (not checked here: the game
//   reads any 8 bytes safely) to the players that get the pose and asked for them; every other
//   binary page gets the same pose without them (bit 3 clear, 46 + 8 * n bytes), JSON pages the
//   JSON pose as before. Bit 3 from a page that did not ask: the pose passes, the controls do not.
//   They add nothing to the limits: they only travel inside poses.
//
// Racing together (source/web/race.js; the game side is mods/core/nfsc/race.h):
//   browser -> relay: {t:"race", k, ...}
//   relay -> the whole room, the sender too: {t:"race", id, k, ...}
//   k is what it is about, a lowercase word of 1-12 letters (race.js uses lobby,
//   join, leave, start, ready, go, progress, finish, cancel); the relay passes the
//   other fields on as they came, but never another t or id. The sender gets its
//   own message back so a "go" reaches every player, its host included, by the
//   same road. A race message is at most RACE_MAX (1024) bytes as sent; limit:
//   burst 20, then 10 a second per player. Nothing is stored.
//
// Car looks (source/web/net.js; the game side is mods/core/nfsc/look.h):
//   browser -> relay: {t:"look", k, data}
//   relay -> the players that picked the sender (see Interest): {t:"look", id, k, data}
//   A player's car paint, parts and vinyls, sent when it changes and on each new
//   connection. k is the car's model key (an integer 0..2^32-1), data standard
//   base64 of 0 to LOOK_MAX (2048) bytes, opaque here ("" = the car has no look of
//   its own: the stock one). The relay keeps each player's newest non-empty look
//   and sends it to each player who picks that player later (a player who joins:
//   right after its welcome); it forgets it when that player leaves. Limit:
//   burst 4, then one per 5 s per player.
//
// Car lights (2026-10-26; source/web/net.js; the game side is mods/core/nfsc/lights.h):
//   browser -> relay: {t:"lights", s}
//   relay -> the players that picked the sender (see Interest): {t:"lights", id, s}
//   s is a whole number 0-15, bits: 1 left turn signal, 2 right turn signal, 4 headlight flash,
//   8 headlights off; 0 = nothing on. Sent when it changes (net.js: at most 4 a second, the
//   newest waiting) and on each new connection when not 0. The relay keeps each player's newest
//   s and sends it, when not 0, to each player who picks that player later (a player who joins:
//   right after its welcome, after the looks); it forgets it when that player leaves (receivers
//   clear it on {t:"left"}). Anything else is refused. Limit: burst 10, then 4 a second per player.
//
// Shared police chase (source/web/net.js; the game side is mods/core/nfsc/pursuit.h, design in
// plans/shared-pursuit.md):
//   browser -> relay: {t:"pursuit", to, k, a, b, f}
//   relay -> browser: {t:"pursuit", id, k, a, b, f}
//   k is a whole number 1-15, a and b whole numbers 0-65535, f four finite numbers within
//   +-1e6 (as sent). to 0: about the sender's own car, to the players that picked the sender
//   (they have its car). to an id: about that player's car, to that player only, and only if the
//   sender picked it (the sender has its car). pursuit.h uses k 1 STATE (to 0: a = status 0 none,
//   1 chase, 2 searching, 3 busted, 4 escaped; b = level; f = heat, bust meter, evade, cops) and
//   k 2 REPORT (to the chased player: a bit 0 = its cops see it, b = how many, f[0] = bust
//   progress in seconds, f[1] = the distance), k 3 ROLE (police/siren bits), and k 4 TRAP
//   (directed to a suspect: a 1 roadblock, 2 spike strip). Kinds 1-15 are reserved; nothing is stored.
//   Limit: burst 40, then 30 a second per player.
//
// Street races (2026-10-27, "Flash to race"; source/web/net.js; the game side is
// mods/core/nfsc/streetrace.h, plans/street-race.md):
//   browser -> relay: {t:"street", to, k, r, a, f}
//   relay -> the player `to` alone: {t:"street", id, k, r, a, f}
//   A challenge between two players and the race that follows: to is the other player's relay id
//   (a positive whole number; that player must be in the sender's room, after its hi, and not the
//   sender), k a whole number 1-15, r the race (a whole number 0 to 2^32 - 1 the challenger makes),
//   a a whole number 0-65535, f four finite numbers within +-1e6; all passed on as sent.
//   streetrace.h uses k 1 CHALLENGE, 2 ACCEPT, 3 START (a = ms to GO, f = the finish x, y, z),
//   4 FINISH (f[0] = the sender's race time, s), 5 QUIT (a = why), 6 ALIVE (every 2 s while
//   racing), 7 BUSY (a = why not). Unlike the shared chase it does not need the sender to have the
//   other's car: racers drive apart. Anything else is refused; nothing is stored. Limit: burst 10,
//   then 5 a second per player.
//
// Ping (2026-10-26; source/web/net.js; the PING line of the kit's performance box):
//   browser -> relay: {t:"ping", n}
//   relay -> the sender alone: {t:"pong", n}
//   n is a whole number 0 to 2^32 - 1, passed back as it came; the page times the answer (net.js
//   asks every 2 s and shows the median of its last 5 round trips). Only after the hi; anything
//   else is refused, and nothing is stored. Limit: burst 5, then 1 a second per player (a ping
//   over it gets no answer). A relay from before answers none, and the page then shows the ping
//   as not known. (These are not the WebSocket protocol's own ping and pong frames, which keep
//   the connection alive and which a page's script can neither send nor time.)
//
// Cloud saves (only with CARBON_SAVES_DIR set; otherwise /saves/* answers 501).
// The start page (source/web/saves.js) seals a save in the browser with a
// keyphrase the server never sees, and stores the sealed bytes under an id it
// derives from that keyphrase separately. The server keeps opaque blobs only.
//   <id>            64 lowercase hex characters; anything else is 400.
//   ?key=           the same CARBON_RELAY_KEY check as the WebSocket route; 403.
//   GET  /saves/<id>   200 the stored bytes (application/octet-stream), 404 none.
//   HEAD /saves/<id>   the same headers, no body.
//   PUT  /saves/<id>   body = the sealed bytes, 1 byte to SAVES_MAX (8 MiB);
//                      Content-Length required (411), over the limit 413.
//                      If-Match: <etag> -> 412 unless the stored blob has it;
//                      If-None-Match: * -> 412 if a blob is already stored.
//                      200 {etag, size, saved} (201 when new). Written to a
//                      temporary file in the same folder, synced, then renamed
//                      over the old one, so a reader sees the old or new blob,
//                      never part of one. A second PUT to an id while one is
//                      being written gets 409.
//   DELETE /saves/<id> removes the stored blob ("Remove from cloud"): 204, or
//                      404 if there is none. If-Match: <etag> -> 412 unless the
//                      stored blob has it. 409 while a PUT to the id is being
//                      written (and a PUT during a DELETE gets 409 too).
//   OPTIONS /saves/<id>  204, the CORS preflight (no key needed).
//   Every answer: Access-Control-Allow-Origin *, Access-Control-Expose-Headers
//   ETag, Cross-Origin-Resource-Policy cross-origin, Cache-Control no-store.
//   GET/HEAD/PUT answers carry ETag = "<sha-256 hex of the blob>" and
//   Last-Modified. Errors are short plain text.
//   Per client IP (X-Real-IP when the connection comes from this machine, i.e.
//   from nginx): SAVES_LIMIT requests, a burst then a steady rate; over it 429
//   with Retry-After. All blobs together stay under CARBON_SAVES_MAX_MB
//   (default 1024): a PUT that would pass it gets 507.
// Problem reports, POST /reports (the game page's "Send report", source/web/report.js): the
// Rust server only (server/src/reports.rs has the rules: key, 512 KiB, 3 per client then one per
// 10 minutes, CARBON_REPORTS_DIR, CARBON_REPORTS_MAX_MB). This relay runs on the developer's
// Mac, where the page's log is at hand ("Download log"), so it has no /reports: a report sent to
// it gets the plain page without CORS headers, and report.js says it could not be sent.
// No dependencies: a minimal WebSocket server (RFC 6455, unfragmented frames).
// Regression checks (Node 22+): node --test tools/tests/carbon-net-relay.test.mjs
import http from "node:http";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

const port = Number(process.argv[2] ?? 0);
const host = process.argv[3] ?? "127.0.0.1";
// A whole number from the environment within [min, max], else the default (unset and "" too).
function envInt(name, fallback, min, max) {
  const n = Number(process.env[name] || NaN);
  return Number.isInteger(n) && n >= min && n <= max ? n : fallback;
}
const MAX_ROOM = envInt("CARBON_ROOM_MAX", 256, 1, 10000);
// Interest (see the header).
const INTEREST_K = envInt("CARBON_INTEREST_K", 24, 1, 1000);
const INTEREST_MS = 500, WHERE_MS = 1000, FAR_MS = 250, MID_MS = 100;
const CREW_RANGE = 1000, CREW_MAX = 16, FOLLOW_MAX = 8, ACKS_MAX = 8;
const ACTIVE_MS = 5000, HOLD_MS = 5000, HELD = 0.25, KEPT = 0.5;
const FULL_RANGE = 500, MID_RANGE = 700, TOP_SPEED = 100, FAR = 1e9, MAP_LIMIT = 1e6;
const MAX_MESSAGE = 4096;
// Bound unsent data per player. Keep only the newest unsent pose per car;
// reliable messages retain their order. A permanently blocked reader is removed.
const MAX_PENDING = 64 * 1024;
const HELLO_TIMEOUT = 10000, BLOCKED_TIMEOUT = 15000;
const PING_INTERVAL = 15000, PONG_TIMEOUT = 10000;
const KEY = process.env.CARBON_RELAY_KEY || "";
const rooms = new Map(); // room name -> Set of peers
let nextId = 1;
// Per player: a burst of `burst` messages, then `rate` a second.
const LIMITS = { chat: { burst: 5, rate: 0.5 }, horn: { burst: 12, rate: 6 }, crew: { burst: 3, rate: 0.2 },
  impact: { burst: 128, rate: 120 }, race: { burst: 20, rate: 10 }, look: { burst: 4, rate: 0.2 },
  follow: { burst: 5, rate: 1 }, cops: { burst: 10, rate: 20 }, cophit: { burst: 64, rate: 60 },
  pursuit: { burst: 40, rate: 30 }, lights: { burst: 10, rate: 4 },
  ping: { burst: 5, rate: 1 }, street: { burst: 10, rate: 5 }, rivals: { burst: 10, rate: 15 } };
const RACE_MAX = 1024;
const LOOK_MAX = 2048;
// Compact poses (binary frames; see "Compact poses" in the header). BIN: the format this relay
// speaks, CARBON_RELAY_BIN=0 for JSON only (as a relay from before it).
const BIN = envInt("CARBON_RELAY_BIN", 1, 0, 1);
const BIN_POSE = 1, BIN_WHERE = 2, POSE_BYTES = 46, ACK_BYTES = 8, WHERE_BYTES = 14;
// Driving (see the header): flags bit 3 = this many DRIVE bytes follow a pose's acks.
const DRIVE_BIT = 8, DRIVE_BYTES = 8;
// Police chases (see the header).
const BIN_COPS = 3, COPS_HEAD = 10, COP_BYTES = 38, COPS_MAX = 8, COP_FRESH = 1000, COP_REACH = 25;
// s[i] travels as a whole number of 1/SCALE[i], at most LIMIT[i] either way.
const SCALE = [32767, 32767, 32767, 32767, 1000, 1000, 1000, 100, 100, 100, 500, 500, 500];
const LIMIT = SCALE.map((_, i) => (i >= 4 && i < 7 ? 0x7fffffff : 0x7fff));
const SAVES_DIR = process.env.CARBON_SAVES_DIR || "";
const SAVES_MAX = 8 * 1024 * 1024;
const SAVES_TOTAL = (Number(process.env.CARBON_SAVES_MAX_MB) || 1024) * 1024 * 1024;
// Per client IP, all /saves/ requests together: 20 at once, then one per 3 s.
const SAVES_LIMIT = { burst: 20, rate: 1 / 3 };

function frame(text, opcode = 1) {
  const body = Buffer.from(text);
  const n = body.length;
  const head = n < 126 ? Buffer.from([0x80 | opcode, n])
    : n < 65536 ? Buffer.from([0x80 | opcode, 126, n >> 8, n & 255])
    : Buffer.concat([Buffer.from([0x80 | opcode, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(n)); return b; })()]);
  return Buffer.concat([head, body]);
}

// One frame from the front of buf, or null until more bytes arrive.
function parse(buf) {
  if (buf.length < 2) return null;
  const op = buf[0] & 15, masked = buf[1] & 128;
  let len = buf[1] & 127, at = 2;
  if (len === 126) { if (buf.length < 4) return null; len = buf.readUInt16BE(2); at = 4; }
  else if (len === 127) { if (buf.length < 10) return null; len = Number(buf.readBigUInt64BE(2)); at = 10; }
  const mask = masked ? buf.subarray(at, at + 4) : null;
  if (masked) at += 4;
  if (len > MAX_MESSAGE) return { op: -1, used: buf.length };
  if (buf.length < at + len) return null;
  const payload = Buffer.from(buf.subarray(at, at + len));
  if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
  return { op, payload, used: at + len };
}

function removePending(peer, key) {
  const old = peer.pending.get(key);
  if (!old) return;
  peer.pendingBytes -= old.length;
  peer.pending.delete(key);
}

function disconnect(peer) {
  leave(peer);
  peer.socket.destroy();
}

function write(peer, bytes, key = Symbol()) {
  if (peer.closed || peer.socket.destroyed || peer.socket.writableEnded) return;
  if (peer.blockedAt !== null) {
    // Reinsert a replacement at the end, after any intervening reliable events.
    removePending(peer, key);
    if (peer.pendingBytes + bytes.length > MAX_PENDING) { disconnect(peer); return; }
    peer.pending.set(key, bytes);
    peer.pendingBytes += bytes.length;
  } else if (!peer.socket.write(bytes)) peer.blockedAt = performance.now();
}

function flush(peer) {
  if (peer.closed) return;
  peer.blockedAt = null;
  for (const [key, bytes] of peer.pending) {
    removePending(peer, key);
    write(peer, bytes, key);
    if (peer.blockedAt !== null) break;
  }
}

function send(peer, message) {
  // net.js creates a player for each pose, so a queued old pose must never
  // follow that player's departure. Bytes already handed to TCP stay ordered.
  if (message.t === "left") { removePending(peer, message.id); removePending(peer, `c${message.id}`); }
  write(peer, frame(JSON.stringify(message)), message.t === "pose" ? message.id : Symbol());
}

function others(peer, message) {
  for (const p of rooms.get(peer.room) ?? []) if (p !== peer && p.named) send(p, message);
}

function leave(peer) {
  // The close frame and TCP close can both arrive; remove a peer only once.
  // Ignore further bytes on the half-closed socket after its room is gone.
  if (peer.closed) return;
  peer.closed = true;
  peer.pending.clear();
  peer.pendingBytes = 0;
  const room = rooms.get(peer.room);
  if (!room || !room.delete(peer)) return;
  if (!room.size) rooms.delete(peer.room);
  for (const r of peer.audience.keys()) r.interest.delete(peer);
  for (const s of peer.interest.keys()) s.audience.delete(peer);
  peer.audience.clear();
  peer.interest.clear();
  others(peer, { t: "left", id: peer.id });
  for (const p of room) p.receipts.delete(peer.id);
  console.log(`${peer.room}: ${peer.name} (${peer.id}) left, ${room.size} remain`);
}

function allow(peer, kind) {
  const { burst, rate } = LIMITS[kind], now = Date.now();
  const b = (peer.buckets[kind] ??= { tokens: burst, at: now });
  b.tokens = Math.min(burst, b.tokens + ((now - b.at) / 1000) * rate);
  b.at = now;
  if (b.tokens < 1) return false;
  b.tokens -= 1;
  return true;
}

// A name or crew: letters, digits, space, _ . - only.
function label(value, fallback, max) {
  return (typeof value === "string" ? value : "").replace(/[^\p{L}\p{N} _.-]/gu, "").trim().slice(0, max) || fallback;
}

function number13(s) {
  return Array.isArray(s) && s.length === 13 && s.every((x) => typeof x === "number" && Number.isFinite(x));
}

// The bytes a look's data stands for (standard base64, at most LOOK_MAX), or -1.
function lookBytes(data) {
  if (typeof data !== "string" || data.length % 4 || !/^[A-Za-z0-9+/]*={0,2}$/.test(data)) return -1;
  const n = (data.length / 4) * 3 - (data.endsWith("==") ? 2 : data.endsWith("=") ? 1 : 0);
  return n <= LOOK_MAX ? n : -1;
}

function identity(p) {
  return { id: p.id, name: p.name, crew: p.crew, ...(p.physics ? { physics: 1 } : {}) };
}

function uint(n) { return Number.isInteger(n) && n > 0 && n <= 0xffffffff; }

// ---- Compact poses (see the header) ----
// The Rust server does the same sums in the same order, so both send the same bytes.
const clampTo = (x, limit) => (x > limit ? limit : x < -limit ? -limit : x) || 0; // NaN and -0: 0
// A pose's 13 numbers as the whole numbers a binary pose carries.
const quantise = (s) => s.map((x, i) => clampTo(Math.round(x * SCALE[i]), LIMIT[i]));
// What they stand for, for the relay's own use (interest, impacts, the map feed).
const unquantise = (n) => n.map((x, i) => x / SCALE[i]);
// And as a JSON page gets them from a binary sender: the quaternion to 5 decimals (as net.js
// rounds it for JSON), the rest exactly (whole mm, cm/s and 1/500 rad/s are short decimals).
const jsonNumbers = (n) => n.map((x, i) => (i < 4 ? Math.round((x / SCALE[i]) * 1e5) / 1e5 : x / SCALE[i]));

// A binary pose from a player (browser -> relay; id 0 there), or null if it is not one.
function readPose(b) {
  if (b.length < POSE_BYTES || b[0] !== BIN_POSE ||
      b.length !== POSE_BYTES + ACK_BYTES * (b[1] >> 4) + (b[1] & DRIVE_BIT ? DRIVE_BYTES : 0)) return null;
  const n = [];
  for (let i = 0; i < 4; i++) n.push(clampTo(b.readInt16LE(14 + 2 * i), LIMIT[i]));
  for (let i = 4; i < 7; i++) n.push(clampTo(b.readInt32LE(22 + 4 * (i - 4)), LIMIT[i]));
  for (let i = 7; i < 13; i++) n.push(clampTo(b.readInt16LE(34 + 2 * (i - 7)), LIMIT[i]));
  const acks = [];
  for (let j = 0; j < b[1] >> 4; j++) acks.push([b.readUInt32LE(46 + 8 * j), b.readUInt32LE(50 + 8 * j)]);
  return { k: b.readUInt32LE(6), ts: b.readUInt32LE(10), m: b[1] & 1, ghost: (b[1] & 2) !== 0, n, acks,
           drive: b[1] & DRIVE_BIT ? Buffer.from(b.subarray(b.length - DRIVE_BYTES)) : null };
}

// A player's pose as a binary page gets it (relay -> browser).
function poseBinary(id, pose, withDrive = false) {
  const acks = pose.physics ? pose.acks : [], n = pose.n;
  const drive = withDrive && pose.drive ? pose.drive : null;
  const b = Buffer.alloc(POSE_BYTES + ACK_BYTES * acks.length + (drive ? DRIVE_BYTES : 0));
  b[0] = BIN_POSE;
  b[1] = pose.m | (pose.physics ? 4 | (pose.ghost ? 2 : 0) : 0) | (drive ? DRIVE_BIT : 0) | (acks.length << 4);
  b.writeUInt32LE(id, 2);
  b.writeUInt32LE(pose.k, 6);
  b.writeUInt32LE(pose.ts >>> 0, 10);
  for (let i = 0; i < 4; i++) b.writeInt16LE(n[i], 14 + 2 * i);
  for (let i = 4; i < 7; i++) b.writeInt32LE(n[i], 22 + 4 * (i - 4));
  for (let i = 7; i < 13; i++) b.writeInt16LE(n[i], 34 + 2 * (i - 7));
  acks.forEach(([from, seq], j) => { b.writeUInt32LE(from, 46 + 8 * j); b.writeUInt32LE(seq, 50 + 8 * j); });
  if (drive) drive.copy(b, POSE_BYTES + ACK_BYTES * acks.length);
  return b;
}

// s's newest pose as r reads it, each kind made once per pose (and kept for players who pick s later).
function poseFrame(s, r) {
  const pose = s.pose;
  if (r.bin && r.drive && pose.drive) return (pose.driveFrame ??= frame(poseBinary(s.id, pose, true), 2));
  if (r.bin) return (pose.binFrame ??= frame(poseBinary(s.id, pose), 2));
  return (pose.jsonFrame ??= frame(JSON.stringify({ t: "pose", id: s.id, k: pose.k, s: pose.js, m: pose.m, ts: pose.ts,
    ...(pose.physics ? { physics: 1, ghost: pose.ghost, acks: pose.acks } : {}) })));
}

// The map feed as binary pages get it.
function whereBinary(p) {
  const b = Buffer.alloc(1 + WHERE_BYTES * p.length);
  b[0] = BIN_WHERE;
  p.forEach(([id, x, z, h], j) => {
    b.writeUInt32LE(id, 1 + 14 * j); b.writeInt32LE(x, 5 + 14 * j); b.writeInt32LE(z, 9 + 14 * j); b.writeUInt16LE(h, 13 + 14 * j);
  });
  return b;
}
function distance(a, b) { return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

// ---- Interest: who gets whose car (see the header) ----
// The sums are written out (not Math.hypot) so the Rust server, which runs the same
// tests, picks exactly the same players.
const crewKey = (crew) => crew.toLowerCase().replace(/\s+/g, "");
const length3 = (x, y, z) => Math.sqrt(x * x + y * y + z * z);

// A pose's speed, at most TOP_SPEED.
function speed(s) {
  const n = length3(s[7], s[8], s[9]);
  return n > TOP_SPEED ? [s[7] * (TOP_SPEED / n), s[8] * (TOP_SPEED / n), s[9] * (TOP_SPEED / n)] : [s[7], s[8], s[9]];
}

// Driving now: its newest pose says so and is fresh.
function active(p, now) {
  return !!p.pose && p.pose.m === 1 && now - p.pose.at <= ACTIVE_MS;
}

// How far c's car counts from r's: the nearer of the two cars 1 s and 4 s on at their
// speeds. FAR when c is not driving now or r has never driven (r's last place stays
// while it is in a menu: its picks do not change then).
function apart(r, c, now) {
  if (!r.at || !active(c, now)) return FAR;
  const rv = active(r, now) ? r.vel : [0, 0, 0];
  let best = FAR;
  for (const t of [1, 4]) {
    const d = length3(r.at[0] + rv[0] * t - c.at[0] - c.vel[0] * t, r.at[1] + rv[1] * t - c.at[1] - c.vel[1] * t,
                      r.at[2] + rv[2] * t - c.at[2] - c.vel[2] * t);
    if (d < best) best = d; // NaN (from huge numbers) never is
  }
  return best;
}

const byDistance = (a, b) => (a.d < b.d ? -1 : a.d > b.d ? 1 : 0) || a.c.id - b.c.id;

// r starts getting s's car: s's look and newest pose go to r at once.
function enter(r, s, now) {
  if (r.closed) return;
  r.interest.set(s, now);
  const edge = { sent: -Infinity, owed: 0 };
  s.audience.set(r, edge);
  if (s.look) send(r, { t: "look", id: s.id, ...s.look });
  if (s.lights) send(r, { t: "lights", id: s.id, s: s.lights });
  if (s.pose && now - s.pose.at <= ACTIVE_MS) {
    edge.sent = now;
    write(r, poseFrame(s, r), s.id);
  }
}

// Picks whose cars r gets now and applies the change.
function pick(r, now) {
  const room = rooms.get(r.room);
  if (!room || !r.named || r.closed) return;
  const list = [];
  for (const c of room) {
    if (c === r || !c.named) continue;
    const since = r.interest.get(c);
    let d = apart(r, c, now);
    if (since !== undefined) d *= now - since < HOLD_MS ? HELD : KEPT;
    const group = r.follow.has(c.id) ? 0 : r.crewKey && c.crewKey === r.crewKey && d <= CREW_RANGE ? 1 : 2;
    list.push({ c, group, d });
  }
  list.sort((a, b) => a.group - b.group || byDistance(a, b));
  const first = [], rest = [];
  let crew = 0;
  for (const x of list) {
    if (x.group === 0 || (x.group === 1 && crew < CREW_MAX)) {
      first.push(x);
      if (x.group === 1) crew++;
    } else rest.push(x); // a crew past CREW_MAX counts by distance like everyone
  }
  rest.sort(byDistance);
  const chosen = first.concat(rest).slice(0, INTEREST_K);
  const keep = new Set(chosen.map((x) => x.c));
  for (const s of [...r.interest.keys()]) if (!keep.has(s)) { r.interest.delete(s); s.audience.delete(r); }
  for (const { c } of chosen) {
    if (r.closed) return;
    if (!r.interest.has(c)) enter(r, c, now);
  }
}

// The least time between two of s's poses to r: none within FULL_RANGE or for a player r
// follows (every pose), MID_MS within MID_RANGE, otherwise FAR_MS.
function every(s, r) {
  if (r.follow.has(s.id) || !s.at || !r.at) return 0;
  const d = length3(s.at[0] - r.at[0], s.at[1] - r.at[1], s.at[2] - r.at[2]);
  return d <= FULL_RANGE ? 0 : d <= MID_RANGE ? MID_MS : FAR_MS;
}

// s's newest pose to the players that picked it.
function forward(s, now) {
  for (const [r, edge] of s.audience) {
    if (r.closed) continue;
    const gap = every(s, r);
    if (gap && now - edge.sent < gap) { edge.owed = gap; continue; }
    edge.sent = now;
    edge.owed = 0;
    write(r, poseFrame(s, r), s.id);
  }
}

// A far player's pose held back is sent once the time it waited for (edge.owed) has passed.
function flushOwed(now) {
  for (const room of rooms.values()) for (const s of room) {
    for (const [r, edge] of s.audience) {
      if (!edge.owed || now - edge.sent < edge.owed || r.closed) continue;
      edge.sent = now;
      edge.owed = 0;
      write(r, poseFrame(s, r), s.id);
    }
  }
}

// Whom chat and the horn reach: the players p picked, those that picked p, p's crew.
function hearers(p) {
  const out = new Set([...p.interest.keys(), ...p.audience.keys()]);
  if (p.crewKey) for (const q of rooms.get(p.room) ?? []) if (q !== p && q.crewKey === p.crewKey) out.add(q);
  return [...out].filter((q) => q.named && !q.closed);
}

// Whole degrees from +z toward +x of the way the nose (+z of the body) points.
function heading(s) {
  const [qx, qy, qz, qw] = s;
  const h = Math.round((Math.atan2(2 * (qx * qz + qw * qy), 1 - 2 * (qx * qx + qy * qy)) * 180) / Math.PI);
  return ((h % 360) + 360) % 360;
}

// The map feed: every player driving now, to everyone in the room.
function where(now) {
  for (const room of rooms.values()) {
    const p = [];
    for (const x of room) {
      if (!x.named || !active(x, now)) continue;
      const px = Math.round(x.at[0]), pz = Math.round(x.at[2]);
      if (Math.abs(px) <= MAP_LIMIT && Math.abs(pz) <= MAP_LIMIT) p.push([x.id, px, pz, heading(x.pose.s)]);
    }
    if (!p.length) continue;
    let json = null, bin = null; // each kind made once, if a page here reads it
    for (const x of room) {
      if (x.named) write(x, x.bin ? (bin ??= frame(whereBinary(p), 2)) : (json ??= frame(JSON.stringify({ t: "where", p }))));
    }
  }
}

// One browser solves a pair (the lower connection id), using Carbon's own
// contact solver. Only the struck car's owner receives the resulting changes.
// A reliable event is never coalesced like a position update.
function impact(peer, m) {
  if (!peer.named || !peer.physics || !uint(m.to) || !uint(m.seq) || m.seq <= peer.impactSeq ||
      !Array.isArray(m.s) || m.s.length !== 12 || !m.s.every(Number.isFinite) ||
      !Number.isFinite(m.ts) || !allow(peer, "impact")) return;
  const target = [...(rooms.get(peer.room) || [])].find((p) => p.id === m.to && p.named && p.physics);
  const now = Date.now();
  if (!target || peer.id >= target.id || !peer.pose || !target.pose ||
      peer.pose.m !== 1 || target.pose.m !== 1 || peer.pose.ghost || target.pose.ghost ||
      now - peer.pose.at > 1000 || now - target.pose.at > 1000 ||
      m.k !== target.pose.k || Math.abs(m.ts - peer.pose.ts) > 1000 ||
      distance(m.s, peer.pose.s.slice(4, 7)) > 20 || distance(m.s, target.pose.s.slice(4, 7)) > 20 ||
      Math.hypot(...m.s.slice(3, 6)) > 6 || Math.hypot(...m.s.slice(6, 9)) > 100 ||
      Math.hypot(...m.s.slice(9, 12)) > 50) return;
  peer.impactSeq = m.seq;
  target.receipts.set(peer.id, m.seq);
  send(target, { t: "impact", id: peer.id, seq: m.seq, k: m.k, ts: m.ts, s: m.s });
}

// A player's new pose (JSON or binary): kept, and passed on to the players that get its car.
// Its frames are made when first needed (poseFrame), one for every receiver of a kind.
function posed(peer, pose) {
  pose.at = Date.now();
  pose.physics = peer.physics;
  peer.pose = pose;
  if (pose.m === 1) {
    peer.at = [pose.s[4], pose.s[5], pose.s[6]];
    peer.vel = speed(pose.s);
  }
  forward(peer, pose.at);
}

// A binary frame: a compact pose, checked as a JSON one is, or a cops frame.
function handleBinary(peer, bytes) {
  if (peer.named && bytes[0] === BIN_COPS) return copsFrame(peer, bytes);
  const b = peer.named ? readPose(bytes) : null;
  if (!b) return;
  const acks = peer.physics ? b.acks.slice(0, ACKS_MAX).filter(([from, seq]) =>
    from && seq && seq <= (peer.receipts.get(from) || 0)) : [];
  posed(peer, { k: b.k, s: unquantise(b.n), js: jsonNumbers(b.n), n: b.n, m: b.m, ts: b.ts, ghost: b.ghost, acks,
                drive: peer.drive ? b.drive : null });
}

// ---- Police chases (see the header) ----
// The cops of a cops frame ([{c, k, p}]: number, model, place in metres), or null if it is not one.
function readCops(b) {
  if (b.length < COPS_HEAD || b[0] !== BIN_COPS || b[1] > COPS_MAX || b.length !== COPS_HEAD + COP_BYTES * b[1]) return null;
  const list = [];
  for (let j = 0, at = COPS_HEAD; j < b[1]; j++, at += COP_BYTES) {
    if (!b[at] || list.some((x) => x.c === b[at])) return null;
    list.push({ c: b[at], k: b.readUInt32LE(at + 2),
                p: [b.readInt32LE(at + 14) / 1000, b.readInt32LE(at + 18) / 1000, b.readInt32LE(at + 22) / 1000] });
  }
  return list;
}

// Whether r gets s's cops now: it follows s, or their cars are within MID_RANGE (or not known yet).
function copsReach(s, r) {
  return r.follow.has(s.id) || !s.at || !r.at || length3(s.at[0] - r.at[0], s.at[1] - r.at[1], s.at[2] - r.at[2]) <= MID_RANGE;
}

function copsFrame(peer, bytes) {
  const list = peer.cops ? readCops(bytes) : null;
  if (!list || !allow(peer, "cops")) return;
  peer.chase = { at: Date.now(), list };
  const out = Buffer.from(bytes);
  out.writeUInt32LE(peer.id, 2);
  const f = frame(out, 2);
  for (const r of peer.audience.keys())
    if (r.named && !r.closed && r.cops && (!list.length || copsReach(peer, r))) write(r, f, `c${peer.id}`);
}

// A hit on another player's cop, to that player alone (see the header).
function copHit(peer, m) {
  if (!peer.named || !peer.cops || !uint(m.to) || !Number.isInteger(m.c) || m.c < 1 || m.c > 255 || !uint(m.seq) ||
      m.seq <= peer.copHitSeq || !Number.isInteger(m.k) || m.k < 0 || m.k > 0xffffffff || !Number.isFinite(m.ts) ||
      !Array.isArray(m.s) || m.s.length !== 9 || !m.s.every((x) => typeof x === "number" && Number.isFinite(x)) ||
      !allow(peer, "cophit")) return;
  const now = Date.now();
  const target = [...(rooms.get(peer.room) || [])].find((p) => p.id === m.to && p.named && p.cops);
  const cop = target && target.chase && now - target.chase.at <= COP_FRESH ? target.chase.list.find((x) => x.c === m.c) : null;
  if (!cop || target === peer || cop.k !== m.k || !peer.pose || peer.pose.m !== 1 || now - peer.pose.at > COP_FRESH ||
      distance(m.s, cop.p) > COP_REACH || distance(m.s, peer.pose.s.slice(4, 7)) > COP_REACH ||
      length3(m.s[3], m.s[4], m.s[5]) > 100 || length3(m.s[6], m.s[7], m.s[8]) > 50) return;
  peer.copHitSeq = m.seq;
  send(target, { t: "cophit", id: peer.id, c: m.c, seq: m.seq, k: m.k, ts: m.ts, s: m.s });
}

// Shared police chase (see the header): about the sender's car to the players that have it, or
// about another player's car to that player alone, if the sender has that car.
function pursuitMessage(peer, m) {
  const whole = (n, max) => Number.isInteger(n) && n >= 0 && n <= max;
  if (!whole(m.k, 15) || m.k < 1 || !whole(m.a, 65535) || !whole(m.b, 65535) || !whole(m.to, 0xffffffff) ||
      !Array.isArray(m.f) || m.f.length !== 4 || !m.f.every((x) => Number.isFinite(x) && Math.abs(x) <= 1e6)) return;
  let to = [];
  if (m.to === 0) to = [...peer.audience.keys()];
  else {
    const target = [...peer.interest.keys()].find((p) => p.id === m.to);
    if (target) to = [target];
  }
  if (!to.length || !allow(peer, "pursuit")) return;
  const bytes = frame(JSON.stringify({ t: "pursuit", id: peer.id, k: m.k, a: m.a, b: m.b, f: m.f }));
  for (const p of to) if (p.named && !p.closed) write(p, bytes);
}

// Rival AI cars are visual only. All battle/career state stays with their owner.
function rivalsMessage(peer, m) {
  if (!Number.isInteger(m.ts) || m.ts < 0 || m.ts > 0xffffffff || !Array.isArray(m.cars) || m.cars.length > 4) return;
  const seen = new Set(), cars = [];
  for (const x of m.cars) {
    if (!x || !uint(x.c) || !uint(x.k) || !uint(x.stamp) || seen.has(x.c) || !number13(x.s) ||
        !x.s.every((v) => Math.abs(v) <= 1e6)) return;
    const q = x.s.slice(0, 4).reduce((n, v) => n + v * v, 0);
    if (q < 0.5 || q > 1.5) return;
    if (x.look !== undefined && ![0, 0x434].includes(lookBytes(x.look))) return;
    seen.add(x.c);
    cars.push({c: x.c, k: x.k, stamp: x.stamp, s: x.s, ...(x.look === undefined ? {} : {look: x.look})});
  }
  if (!allow(peer, "rivals")) return;
  const bytes = frame(JSON.stringify({t: "rivals", id: peer.id, ts: m.ts, cars}));
  for (const target of peer.audience.keys()) if (target.named && !target.closed) write(target, bytes);
}

// Street races (see the header): to one player of the sender's room alone.
function streetMessage(peer, m) {
  const whole = (n, max) => Number.isInteger(n) && n >= 0 && n <= max;
  if (!whole(m.k, 15) || m.k < 1 || !whole(m.r, 0xffffffff) || !whole(m.a, 65535) || !uint(m.to) ||
      !Array.isArray(m.f) || m.f.length !== 4 || !m.f.every((x) => Number.isFinite(x) && Math.abs(x) <= 1e6)) return;
  const target = [...(rooms.get(peer.room) || [])].find((p) => p.id === m.to);
  if (!target || target === peer || !target.named || target.closed || !allow(peer, "street")) return;
  send(target, { t: "street", id: peer.id, k: m.k, r: m.r, a: m.a, f: m.f });
}

function handle(peer, text) {
  let m;
  try { m = JSON.parse(text); } catch { return; }
  if (!m || typeof m !== "object" || Array.isArray(m)) return;
  if (m.t === "hi") {
    // JSON objects can shadow toString/valueOf, so do not coerce client fields.
    peer.name = label(m.name, "Player", 20);
    peer.crew = label(m.crew, "", 16);
    peer.crewKey = crewKey(peer.crew);
    peer.physics = m.physics === 1;
    // Compact poses: a page that reads binary poses says which format (1 now); 0 = JSON only.
    peer.bin = BIN && Number.isInteger(m.bin) && m.bin >= 1 ? BIN : 0;
    // Police chases: binary frames, so only for a page that also got compact poses.
    peer.cops = !!peer.bin && m.cops === 1;
    // Driving: controls inside compact poses, so only for a page that also got them.
    peer.drive = !!peer.bin && m.drive === 1;
    send(peer, { t: "welcome", id: peer.id, ...(m.rivals === 1 ? {rivals: 1} : {}), ...(m.physics === 0 || m.physics === 1 ? { physics: 1 } : {}),
      ...(peer.bin ? { bin: peer.bin } : {}), ...(peer.cops ? { cops: 1 } : {}), ...(peer.drive ? { drive: 1 } : {}),
      peers: [...rooms.get(peer.room)].filter((p) => p !== peer && p.named).map(identity) });
    if (peer.closed) return;
    peer.named = true;
    others(peer, { t: "join", ...identity(peer) });
    if (peer.closed) return;
    // The cars it gets, at once: their looks (so they are built in them from the start) and poses.
    const now = Date.now();
    pick(peer, now);
    if (peer.closed) return;
    // Players with room for more take the newcomer now, not at their next pick.
    for (const p of rooms.get(peer.room))
      if (p !== peer && p.named && !p.closed && !p.interest.has(peer) && p.interest.size < INTEREST_K) enter(p, peer, now);
    if (peer.closed) return;
    console.log(`${peer.room}: ${peer.name} (${peer.id}) joined, ${rooms.get(peer.room).size} here`);
  } else if (m.t === "pose" && peer.named && number13(m.s) &&
             Number.isInteger(m.k) && m.k >= 0 && m.k <= 0xffffffff &&
             (m.m === 0 || m.m === 1)) {
    const acks = peer.physics && Array.isArray(m.acks) ? m.acks.slice(0, ACKS_MAX).filter((a) =>
      Array.isArray(a) && a.length === 2 && uint(a[0]) && uint(a[1]) && a[1] <= (peer.receipts.get(a[0]) || 0)) : [];
    // JSON pages get s and ts as sent, binary pages the same numbers quantised.
    posed(peer, { k: m.k, s: m.s, js: m.s, n: quantise(m.s), m: m.m, ts: Number.isFinite(m.ts) ? m.ts : 0,
                  ghost: m.ghost === true, acks });
  } else if (m.t === "follow" && peer.named && Array.isArray(m.ids) && m.ids.length <= FOLLOW_MAX &&
             m.ids.every(uint) && allow(peer, "follow")) {
    // The players racing with this one: picked first from now on.
    peer.follow = new Set(m.ids);
    pick(peer, Date.now());
  } else if (m.t === "physics" && peer.named && typeof m.on === "boolean") {
    peer.physics = m.on;
    others(peer, { t: "physics", id: peer.id, on: m.on });
  } else if (m.t === "impact") {
    impact(peer, m);
  } else if (m.t === "cophit") {
    copHit(peer, m);
  } else if (m.t === "chat" && peer.named && typeof m.text === "string") {
    const text = m.text.replace(/\p{Cc}/gu, " ").trim().slice(0, 120);
    if (text && allow(peer, "chat")) {
      const bytes = frame(JSON.stringify({ t: "chat", id: peer.id, text }));
      for (const p of hearers(peer)) write(p, bytes);
    }
  } else if (m.t === "crew" && peer.named && typeof m.crew === "string" && allow(peer, "crew")) {
    // A player changing crew while driving; everyone else, and the sender, learn the cleaned name.
    peer.crew = label(m.crew, "", 16);
    peer.crewKey = crewKey(peer.crew);
    send(peer, { t: "crew", id: peer.id, crew: peer.crew });
    others(peer, { t: "crew", id: peer.id, crew: peer.crew });
  } else if (m.t === "horn" && peer.named && typeof m.on === "boolean" && allow(peer, "horn")) {
    // A horn's "off" also reaches those its "on" reached, even if they have gone out of reach.
    const to = hearers(peer);
    if (m.on) for (const p of to) peer.hornTo.add(p);
    else {
      for (const p of peer.hornTo) if (!to.includes(p) && p.named && !p.closed) to.push(p);
      peer.hornTo.clear();
    }
    const bytes = frame(JSON.stringify({ t: "horn", id: peer.id, on: m.on }));
    for (const p of to) write(p, bytes);
  } else if (m.t === "look" && peer.named && Number.isInteger(m.k) && m.k >= 0 && m.k <= 0xffffffff &&
             lookBytes(m.data) >= 0 && allow(peer, "look")) {
    // A car's look, to the players that get this car; the newest is kept for those who pick it later.
    peer.look = m.data ? { k: m.k, data: m.data } : null;
    const bytes = frame(JSON.stringify({ t: "look", id: peer.id, k: m.k, data: m.data }));
    for (const p of peer.audience.keys()) if (p.named && !p.closed) write(p, bytes);
  } else if (m.t === "lights" && peer.named && Number.isInteger(m.s) && m.s >= 0 && m.s <= 15 && allow(peer, "lights")) {
    // A car's lights, to the players that get this car; the newest is kept for those who pick it later.
    peer.lights = m.s;
    const bytes = frame(JSON.stringify({ t: "lights", id: peer.id, s: m.s }));
    for (const p of peer.audience.keys()) if (p.named && !p.closed) write(p, bytes);
  } else if (m.t === "pursuit" && peer.named) {
    pursuitMessage(peer, m);
  } else if (m.t === "rivals" && peer.named) {
    rivalsMessage(peer, m);
  } else if (m.t === "street" && peer.named) {
    streetMessage(peer, m);
  } else if (m.t === "ping" && peer.named && Number.isInteger(m.n) && m.n >= 0 && m.n <= 0xffffffff &&
             allow(peer, "ping")) {
    // The page timing its link (see "Ping"): straight back, to the sender alone.
    send(peer, { t: "pong", n: m.n });
  } else if (m.t === "race" && peer.named && typeof m.k === "string" && /^[a-z]{1,12}$/.test(m.k) &&
             Buffer.byteLength(text) <= RACE_MAX && allow(peer, "race")) {
    // The lobby, start, progress and finish of a race together: to everyone here, the sender too.
    const { t, id, ...rest } = m;
    const out = { t: "race", id: peer.id, ...rest };
    send(peer, out);
    others(peer, out);
  }
}

// ---- Cloud saves: sealed blobs by id (see the header) ----
const savesClients = new Map(); // client IP -> {tokens, at}
const savesWriting = new Set(); // ids with a PUT in progress
const savesInfo = new Map();    // id -> {etag, mtimeMs, size}, so HEAD and If-Match need not rehash
let savesUsed = 0;              // bytes of all stored blobs
let savesSwept = Date.now();
const SAVES_HEADERS = {
  "access-control-allow-origin": "*", "access-control-expose-headers": "ETag",
  "cross-origin-resource-policy": "cross-origin", "cache-control": "no-store",
};

if (SAVES_DIR) {
  fs.mkdirSync(SAVES_DIR, { recursive: true });
  for (const name of fs.readdirSync(SAVES_DIR)) {
    const file = path.join(SAVES_DIR, name);
    // A temporary file left by a stop in the middle of an upload is never a save.
    if (name.endsWith(".tmp")) fs.rmSync(file, { force: true });
    else if (/^[0-9a-f]{64}$/.test(name)) savesUsed += fs.statSync(file).size;
  }
}

function savesReply(res, status, text, headers = {}) {
  res.writeHead(status, { ...SAVES_HEADERS, "content-type": "text/plain; charset=utf-8", ...headers });
  res.end(text + "\n");
}

// nginx on the droplet connects from this machine and passes the player's
// address in X-Real-IP; anyone else is counted by their own address.
function savesClient(req) {
  const peer = req.socket.remoteAddress || "";
  const real = req.headers["x-real-ip"];
  const local = peer === "127.0.0.1" || peer === "::1" || peer === "::ffff:127.0.0.1";
  return local && typeof real === "string" && real.trim() ? real.trim() : peer;
}

// 0 when the request may go ahead, else the seconds to wait.
function savesWait(ip) {
  const { burst, rate } = SAVES_LIMIT, now = Date.now();
  const full = (b) => b.tokens + ((now - b.at) / 1000) * rate >= burst;
  if (now - savesSwept > 60000) { // forget clients that have been quiet long enough
    savesSwept = now;
    for (const [k, b] of savesClients) if (full(b)) savesClients.delete(k);
  }
  const b = savesClients.get(ip) ?? { tokens: burst, at: now };
  b.tokens = Math.min(burst, b.tokens + ((now - b.at) / 1000) * rate);
  b.at = now;
  savesClients.set(ip, b);
  if (b.tokens < 1) return Math.ceil((1 - b.tokens) / rate);
  b.tokens -= 1;
  return 0;
}

function savesKeyOk(url) {
  const given = Buffer.from(url.searchParams.get("key") || "");
  return !KEY || (given.length === Buffer.byteLength(KEY) && crypto.timingSafeEqual(given, Buffer.from(KEY)));
}

function savesRemember(id, info) {
  if (savesInfo.size >= 1000) savesInfo.clear(); // a cache, not a record
  savesInfo.set(id, info);
  return info;
}

// {etag, mtimeMs, size} of a stored blob, or null.
async function savesStat(id) {
  let st;
  try { st = await fs.promises.stat(path.join(SAVES_DIR, id)); } catch { return null; }
  const known = savesInfo.get(id);
  if (known && known.mtimeMs === st.mtimeMs && known.size === st.size) return known;
  const data = await fs.promises.readFile(path.join(SAVES_DIR, id));
  return savesRemember(id, { etag: `"${crypto.createHash("sha256").update(data).digest("hex")}"`,
                             mtimeMs: st.mtimeMs, size: st.size });
}

// If-Match / If-None-Match: "*" or a list of quoted etags.
function savesMatches(header, info) {
  return !!info && header.split(",").map((s) => s.trim()).some((t) => t === "*" || t === info.etag);
}

async function savesPut(req, res, id) {
  const file = path.join(SAVES_DIR, id);
  const length = Number(req.headers["content-length"]);
  if (req.headers["content-length"] === undefined || !Number.isSafeInteger(length))
    return savesReply(res, 411, "Content-Length is required.");
  if (length < 1 || length > SAVES_MAX)
    return savesReply(res, 413, `A save is 1 to ${SAVES_MAX} bytes.`, { connection: "close" });
  if (savesWriting.has(id)) return savesReply(res, 409, "This save is being written right now. Try again.");
  savesWriting.add(id);
  const tmp = path.join(SAVES_DIR, `.${id}.${crypto.randomBytes(6).toString("hex")}.tmp`);
  let out = null;
  try {
    const old = await savesStat(id);
    const ifMatch = req.headers["if-match"], ifNone = req.headers["if-none-match"];
    if ((ifMatch !== undefined && !savesMatches(ifMatch, old)) || (ifNone !== undefined && savesMatches(ifNone, old)))
      return savesReply(res, 412, "The save on the server has changed.", old ? { etag: old.etag } : {});
    if (savesUsed - (old?.size ?? 0) + length > SAVES_TOTAL)
      return savesReply(res, 507, "The server has no room for more saves.");
    out = await fs.promises.open(tmp, "wx", 0o600);
    const hash = crypto.createHash("sha256");
    let got = 0;
    for await (const chunk of req) {
      got += chunk.length;
      hash.update(chunk);
      await out.write(chunk);
    }
    if (got !== length) throw new Error("upload ended early");
    await out.sync();
    await out.close();
    out = null;
    await fs.promises.rename(tmp, file); // atomic: readers get the old blob or the new one
    const st = await fs.promises.stat(file);
    savesUsed += st.size - (old?.size ?? 0);
    const info = savesRemember(id, { etag: `"${hash.digest("hex")}"`, mtimeMs: st.mtimeMs, size: st.size });
    const saved = new Date(st.mtimeMs);
    res.writeHead(old ? 200 : 201, { ...SAVES_HEADERS, "content-type": "application/json",
                                     etag: info.etag, "last-modified": saved.toUTCString() });
    res.end(JSON.stringify({ etag: info.etag, size: st.size, saved: saved.toISOString() }));
  } catch (error) {
    if (out) await out.close().catch(() => {});
    await fs.promises.rm(tmp, { force: true }).catch(() => {});
    if (!res.headersSent && !res.destroyed) savesReply(res, 400, "The upload did not arrive whole.");
    if (!/ended early|aborted|ECONNRESET/.test(String(error?.message ?? error))) console.warn("saves: PUT failed:", error);
  } finally {
    savesWriting.delete(id);
  }
}

// The same rules as a PUT (key, limit, one change at a time); the space it used is free again.
async function savesDelete(req, res, id) {
  if (savesWriting.has(id)) return savesReply(res, 409, "This save is being written right now. Try again.");
  savesWriting.add(id);
  try {
    const old = await savesStat(id);
    if (!old) return savesReply(res, 404, "No save with this id.");
    const ifMatch = req.headers["if-match"];
    if (ifMatch !== undefined && !savesMatches(ifMatch, old))
      return savesReply(res, 412, "The save on the server has changed.", { etag: old.etag });
    try { await fs.promises.unlink(path.join(SAVES_DIR, id)); }
    catch (error) { if (error?.code === "ENOENT") return savesReply(res, 404, "No save with this id."); throw error; }
    savesUsed = Math.max(0, savesUsed - old.size);
    savesInfo.delete(id);
    res.writeHead(204, SAVES_HEADERS);
    res.end();
  } finally {
    savesWriting.delete(id);
  }
}

async function saves(req, res, url) {
  if (req.method === "OPTIONS") { // the browser's preflight for a PUT with a binary body, or a DELETE
    res.writeHead(204, { ...SAVES_HEADERS, "access-control-allow-methods": "GET, HEAD, PUT, DELETE, OPTIONS",
                         "access-control-allow-headers": "Content-Type, If-Match, If-None-Match",
                         "access-control-allow-private-network": "true", "access-control-max-age": "600" });
    res.end();
    return;
  }
  if (!SAVES_DIR) return savesReply(res, 501, "Cloud saves are not turned on on this server.");
  const wait = savesWait(savesClient(req));
  if (wait) return savesReply(res, 429, "Too many requests. Try again shortly.", { "retry-after": String(wait) });
  if (!savesKeyOk(url)) return savesReply(res, 403, "Wrong server key.");
  const id = url.pathname.slice("/saves/".length);
  if (!/^[0-9a-f]{64}$/.test(id)) return savesReply(res, 400, "A save id is 64 lowercase hex characters.");
  if (req.method === "PUT") return savesPut(req, res, id);
  if (req.method === "DELETE") return savesDelete(req, res, id);
  if (req.method !== "GET" && req.method !== "HEAD")
    return savesReply(res, 405, "Use GET, HEAD, PUT or DELETE.", { allow: "GET, HEAD, PUT, DELETE, OPTIONS" });
  let data, st;
  if (req.method === "HEAD") {
    st = await savesStat(id);
  } else {
    // One open file for both the bytes and their etag, even if a PUT renames over it meanwhile.
    let fh;
    try { fh = await fs.promises.open(path.join(SAVES_DIR, id), "r"); } catch { st = null; }
    if (fh) {
      try {
        const s = await fh.stat();
        data = await fh.readFile();
        st = savesRemember(id, { etag: `"${crypto.createHash("sha256").update(data).digest("hex")}"`,
                                 mtimeMs: s.mtimeMs, size: data.length });
      } finally { await fh.close(); }
    }
  }
  if (!st) return savesReply(res, 404, "No save with this id.");
  res.writeHead(200, { ...SAVES_HEADERS, "content-type": "application/octet-stream", "content-length": String(st.size),
                       etag: st.etag, "last-modified": new Date(st.mtimeMs).toUTCString() });
  res.end(data);
}

// GET /stats?room=<name>: {players, here}, how many are on the relay and in
// that room (empty: the lobby), for the start page. Counts only, no names.
const server = http.createServer((req, res) => {
  let url;
  try { url = new URL(req.url, "http://relay"); }
  catch {
    res.writeHead(400, { "content-type": "text/plain" });
    res.end("Invalid request URL.\n");
    return;
  }
  if (url.pathname === "/saves" || url.pathname.startsWith("/saves/")) {
    saves(req, res, url).catch((error) => {
      console.warn("saves:", error);
      if (!res.headersSent) savesReply(res, 500, "The server could not handle this save.");
      else res.destroy();
    });
    return;
  }
  if (url.pathname === "/stats") {
    const named = (set) => [...(set ?? [])].filter((p) => p.named).length;
    let players = 0;
    for (const set of rooms.values()) players += named(set);
    const here = named(rooms.get(url.searchParams.get("room")?.slice(0, 40) || "lobby"));
    res.writeHead(200, { "content-type": "application/json", "cache-control": "no-store",
                         "access-control-allow-origin": "*", "cross-origin-resource-policy": "cross-origin" });
    res.end(JSON.stringify({ players, here }));
    return;
  }
  res.writeHead(200, { "content-type": "text/plain" });
  res.end("Carbon online relay. Players connect with WebSockets.\n");
});

server.on("upgrade", (req, socket, head) => {
  // Rejected upgrades also need an error handler if the remote closes early.
  socket.on("error", () => socket.destroy());
  const key = req.headers["sec-websocket-key"];
  let url, room;
  try {
    url = new URL(req.url, "http://relay");
    room = decodeURIComponent(url.pathname.slice(1)).slice(0, 40) || "lobby";
  } catch {
    socket.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    return;
  }
  const given = Buffer.from(url.searchParams.get("key") || "");
  if (KEY && (given.length !== Buffer.byteLength(KEY) || !crypto.timingSafeEqual(given, Buffer.from(KEY)))) {
    socket.end("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
    return;
  }
  if (!key || (rooms.get(room)?.size ?? 0) >= MAX_ROOM) { socket.destroy(); return; }
  const accept = crypto.createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
  socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
               `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
  socket.setNoDelay(true);
  const now = performance.now();
  // interest: the players whose cars this one gets -> since when; audience: the players
  // that get this one's car -> {sent, owed: ms a held-back pose waits for} (see forward); at, vel: from its newest driving pose.
  const peer = { id: nextId++, socket, room, name: "Player", crew: "", crewKey: "", named: false, closed: false, buckets: {},
    pending: new Map(), pendingBytes: 0, blockedAt: null, joinedAt: now, lastPing: now, ping: null,
    physics: false, bin: 0, pose: null, impactSeq: 0, receipts: new Map(), look: null, lights: 0, cops: false, drive: false, chase: null, copHitSeq: 0,
    at: null, vel: null, interest: new Map(), audience: new Map(), follow: new Set(), hornTo: new Set() };
  if (!rooms.has(room)) rooms.set(room, new Set());
  rooms.get(room).add(peer);
  let buf = Buffer.alloc(0);
  const receive = (data) => {
    if (peer.closed) return;
    buf = buf.length ? Buffer.concat([buf, data]) : data;
    for (let f; (f = parse(buf));) {
      buf = buf.subarray(f.used);
      if (f.op === 1) handle(peer, f.payload.toString());
      else if (f.op === 2) handleBinary(peer, f.payload);
      else if (f.op === 9 && f.payload.length <= 125) write(peer, frame(f.payload, 10));
      else if (f.op === 10 && peer.ping && f.payload.equals(peer.ping)) peer.ping = null;
      else if (f.op === 8 || f.op === -1) { leave(peer); socket.end(Buffer.from([0x88, 0])); return; }
      if (peer.closed) return;
    }
  };
  socket.on("data", receive);
  socket.on("drain", () => flush(peer));
  socket.on("close", () => leave(peer));
  socket.on("end", () => { leave(peer); socket.end(); });
  // Node may read the first WebSocket bytes alongside the HTTP headers.
  if (head.length) receive(head);
});

// WebSocket pong replies come from the browser itself. A player may stay idle
// in a menu or a background tab without publishing any car positions.
setInterval(() => {
  const now = performance.now();
  for (const room of rooms.values()) for (const peer of room) {
    if ((!peer.named && now - peer.joinedAt >= HELLO_TIMEOUT) ||
        (peer.blockedAt !== null && now - peer.blockedAt >= BLOCKED_TIMEOUT) ||
        (peer.ping && now - peer.lastPing >= PONG_TIMEOUT)) {
      disconnect(peer);
    } else if (!peer.ping && now - peer.lastPing >= PING_INTERVAL) {
      peer.ping = crypto.randomBytes(4);
      peer.lastPing = now;
      write(peer, frame(peer.ping, 9));
    }
  }
}, 1000).unref();

// Interest: owed far poses every FAR_MS, picks every INTEREST_MS, the map feed every WHERE_MS.
let interestTicks = 0;
setInterval(() => {
  const now = Date.now();
  interestTicks++;
  flushOwed(now);
  if (interestTicks % (INTEREST_MS / FAR_MS) === 0) for (const room of rooms.values()) for (const r of room) pick(r, now);
  if (interestTicks % (WHERE_MS / FAR_MS) === 0) where(now);
}, FAR_MS).unref();

server.listen(port, host, () => {
  const { port: p } = server.address();
  console.log(`relay on ws://${host}:${p}/<room>`);
  if (SAVES_DIR) console.log(`cloud saves in ${SAVES_DIR} (${(savesUsed / 1048576).toFixed(1)} MB used)`);
});
