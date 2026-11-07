// race.js - RACE TOGETHER for Need for Speed: Carbon: friends in the same online
// room race each other in the game's own Quick Races, with the game's own race
// list, minimap dots, start and results. The start page (start.js) loads it with
// menu.js and chat.js when the player presses Play. Like chat.js it wakes up only
// when the game goes online (window.recompOnline, or net.js's first
// "recomp-online" event) and does nothing offline.
//
// HOW A RACE GOES.
// 1. The host turns on "Host a race together" in the F9 menu, then picks a Quick
//    Race the usual way and presses Start. The game does not start it yet: the
//    mod (mods/core/nfsc/race.h) catches the Start with the race and its options,
//    and this file opens the lobby: everyone else in the room sees
//    "Metin started a race: Circuit, 2 laps · Join".
// 2. Players join from the card or the F9 menu, from the game's menus (the race
//    can only be started there). The host presses Start in the lobby: every
//    player's game starts the same race with the host's options, every other
//    player's car is added as a racer, and with no AI unless the host kept them.
// 3. Everyone holds on the loading screen until every player in the race has
//    loaded ("ready"); then the host says "go" and all leave it together.
//    A player who leaves before the go (the room, the safehouse or a menu, or
//    Leave) is dropped, as "Start without" drops a slow one: the others race on
//    (a Canyon Duel, one against one, is cancelled; after the go the player who
//    stays wins it, logs/duel-trucks-20261106). When that player is the
//    host, the first player of the race still in it hosts it from then on;
//    every game keeps the same list, so all pick the same one. A host who
//    leaves the lobby before Start hands it on the same way.
// 4. While racing, each player sends their own place, lap, progress and race
//    time four times a second: the list on the left shows everyone, from what
//    each player sends (a fallback, and a check of the game's own list). Each
//    player's own finish time goes to the others, whose games write it into
//    that player's row, so every results screen has the same order.
//
// OPEN RACES (2026-11-06, logs/race-browser-mockup). Every lobby heard is kept as an
// open race until its host stops repeating it, whether or not this player could join
// it from where they are. The corner shows the ones they could join (invites on, not
// declined, not full, the right place for it): one or two as cards, three or more as
// a small chip with the count, plus a card for a crew mate's race. F9 → Race lists
// them all (menu.js draws openRaces()), with why a race cannot be joined from here.
// "Not now" on a card hides that host's races from the corner for ten minutes.
//
// THE RELAY carries one message type for all of it, {t:"race", k, race, ...}
// (tools/carbon-net-relay.mjs header), sent to everyone in the room, the sender
// too: k is lobby, join, leave, start, ready, go, progress, finish or cancel, and
// race is the session id the host made. net.js passes these to the relay and
// back as "recomp-online-message" events.
//
// RACE START (2026-11-06, logs/race-start-mockup, logs/race-start-20261106). While the players load (every
// game holds on its loading screen, race.h), the page shows a timer at the top ("Race starts in 15") and the
// racers on the right: ready, loading (the step their game has reached: {k:"load", p} 0 menus, 1 track,
// 2 cars; no game reports a percentage), or left. The timer starts when the first player is loaded and lasts
// the host's wait (F9 → Start timer, 15 s by default, sent with the start). Everyone loaded ends it at once:
// the usual go. When it runs out the host's go names the players still loading as late (late, and in without
// for older pages): the others start together with the game's own 3-2-1, and each late player's game lets go
// as soon as it has loaded, behind the grid, on its own 3-2-1 (its race time counts from there), passing
// through the other cars until clear of them (race.h RACE_LATE / RACE_PEER_*). Its row says "late" (not grey);
// only a player who left is grey. A Canyon Duel has no timer: it needs both players.
//
// THE GAME. race.h shares a block of guest memory (globalThis.carbonRaceBlock,
// "carbon-race" event; layout RaceShared in race.h): the page's part from +0x0c
// (session, flags, our id and grid slot, the other players, go, launch with the
// race key and its 40-byte options) and the game's part from +0x1e0 (phase, the
// car this player races, the Start caught while hosting, this player's place,
// laps, progress and time, and the game's racer table). Each side guards its
// part with a sequence number, odd while it writes.
"use strict";
(() => {
  // RaceShared (race.h)
  const MAGIC = 0x31435243, PAGE_SEQ = 0x08, PAGE = 0x0c, PEERS = 0x60, PEER = 0x40, MAX_PEERS = 8;
  const GAME_SEQ = 0x260, ROW0 = 0x2d0, ROW = 0x30, MAX_ROWS = 8;
  const CATCH = 1, KEEP_AI = 2, WORLD = 4, CANCEL = 8;
  let worldHosting = false, pickCommand = 0;
  const PHASE = { idle: 0, launched: 1, setUp: 2, holding: 3, racing: 4, done: 5 };
  // Native GRaceParameters gameplay types; DAL menu categories use different IDs.
  const TYPES = ["Sprint", "Circuit", "Drag", "Lap knockout", "Tollbooth", "Speedtrap", "Checkpoint", "Cash grab",
    "Challenge", "Speedtrap jump", "Milestone jump", "Drift", "Canyon drift", "Canyon duel", "Pursuit tag",
    "Pursuit knockout", "Encounter"];
  const LOBBY_EVERY = 4000;  // the host repeats the lobby, so players who arrive later see it
  const PROGRESS_EVERY = 250;
  // Nobody loaded by then: the race is cancelled (once anyone has loaded, the race start's timer decides). 120 s, as
  // race.h's own hold (RACE_HOLD_MAX): on a busy computer three games took 98-135 s to load a race (2026-11-06,
  // logs/race-start-20261106 run h1), and 60 s cancelled a race all three would have run.
  const LOAD_TIMEOUT = 120000;
  const FORGET = 15000;      // a lobby nobody repeats is gone
  // The relay can drop a race message (rate limit, a connection that is going), and start, ready,
  // go and finish each decide a whole race, so they repeat until they have done their work: every
  // handler ignores a duplicate. The relay's budget is 10 race messages a second.
  const RESEND_EVERY = 1000;
  const RESEND_FOR = 15000;
  const GONE_GRACE = 6000;   // a player missing from the room this long has left a race that has begun
  const GONE_TIME = 5999;    // their finish time: after everyone who really finished
  // RACE START: the timer (seconds after the first player has loaded), the host's choice; a late player's flag.
  const WAITS = [10, 15, 20, 30, 45, 60], LATE = 0x10, LOAD_EVERY = 2000, NAMED_FOR = 4000;
  let waitSecs = 15;
  try {
    const q = Number(new URLSearchParams(location.search).get("raceWait")), v = q || Number(localStorage.getItem("nfsc.raceWait"));
    if (Number.isInteger(v) && v >= 5 && v <= 120) waitSecs = v;
  } catch {}
  const MENU_AFTER = 3000;   // a car stopped this long in a menu: its driver is told about, a short pause is not
  const TOAST_FOR = 6000;
  const SILENT_LIMIT = 20000; // once this player has finished: a hung tab stays in the room but says nothing

  let active = false, me = 0;
  let modeNotice = "", cancelledCardKey = "";
  let cardSmall = false;     // the player minimized the card
  let toastEl = null, toastAt = 0;
  const watched = new Map(); // id -> {name, menuAt, told}: the cars this player can see
  let pendingStart = null;   // the host's start, until everyone is ready: {race, fields, first, at}
  const declined = new Map(); // host id -> until when "Not now" keeps its races off the corner
  const NOT_NOW_FOR = 10 * 60000;
  const CHIP_FROM = 3;       // this many joinable races: the corner shows a chip with the count, not cards
  const MAX_OPEN = 48;       // open races kept at once (a busy room)
  const isDrift = type => type === 11 || type === 12;
  const needsNativeMatch = type => type === 14;
  const CANYON_DUEL = 13;    // one against one: alone it is won at once
  // Races whose finish carries points (and how it ended) that can still change after it: drift's banked points, and a
  // Canyon Duel's points and outcome (logs/duel-trucks-20261106).
  const scored = type => isDrift(type) || type === CANYON_DUEL;
  const placesOf = type => type === CANYON_DUEL ? 2 : MAX_PEERS + 1; // the host takes no more (heard, "join")
  const worldType = type => [0, 1, 5, 11, 12, 13].includes(type);
  function nativeModeNotice() {
    modeNotice = "Pursuit Tag needs Online → Custom Match. Race Together uses Quick Race and cannot start Pursuit Tag.";
    draw();
  }
  let hosting = false;       // F9: the next Quick Race Start opens a lobby
  let lobby = null;          // the host's own lobby: {race, key, type, opts, laps, ai, roster: [{id, name, car}]}
  let invites = new Map();   // race -> {from, name, race, type, laps, ai, roster, at, opened, seen, joined}: every open race heard
  let invitesOn = true;      // F9 "Race invites": off, other players' hosted races are not shown
  try { invitesOn = localStorage.getItem("nfsc.raceInvites") !== "false"; } catch {}
  let race = null;           // the race we are in: {race, host, key, opts, ai, roster, ready: Set, startedAt, went}
  let standings = new Map(); // id -> {name, place, lap, pct, t, done, time, at}
  let lastCaught = -1, lastProgress = 0, finishSent = 0, lastPhase = 0;

  // ---- The game's block ----
  let views = null;
  function block() {
    const b = globalThis.carbonRaceBlock;
    if (!b) return null;
    const buf = b.memory.buffer;
    if (!views || views.buf !== buf)
      views = { buf, u8: new Uint8Array(buf), u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
    return views.u32[b.ptr >> 2] === MAGIC ? b.ptr : null;
  }
  // The game's part, or null while it is being written.
  function game() {
    const p = block();
    if (p === null) return null;
    const { u8, u32, i32, f32 } = views, seq = (p + GAME_SEQ) >> 2;
    const before = Atomics.load(i32, seq);
    if (before & 1) return null;
    const at = (o) => (p + o) >> 2;
    const text = (o, n) => { let s = ""; for (let i = 0; i < n && u8[p + o + i]; i++) s += String.fromCharCode(u8[p + o + i]); return s; };
    const out = {
      worldReady: u32[at(4)] >= 2, driftReady: u32[at(4)] >= 3,
      score: u32[at(4)] >= 3 ? u32[at(0x450)] : 0, dnf: u32[at(4)] >= 3 && u32[at(0x454)] === 1, // drift: overboard / too slow; duel: lost (2 = won)
      // Canyon Duel (race.h race_duel_report): score = this player's duel points; 1 lost, 2 won once it is over.
      duelEnd: u32[at(4)] >= 3 ? u32[at(0x454)] : 0, roam: !!(u32[at(0x2cc)] & 0x800), racing: !!(u32[at(0x2cc)] & 0x200),
      phase: u32[at(0x264)], car: u32[at(0x268)], caught: u32[at(0x26c)], caughtKey: u32[at(0x270)],
      caughtType: u32[at(0x274)], caughtOpts: Array.from(u8.subarray(p + 0x278, p + 0x2a0)),
      place: u32[at(0x2a0)], laps: u32[at(0x2a4)], done: u32[at(0x2a8)], pct: f32[at(0x2ac)], time: f32[at(0x2b0)],
      loaded: u32[at(0x2b8)], session: u32[at(0x2bc)], lapPct: f32[at(0x2c0)], cp: u32[at(0x2c4)], ms: u32[at(0x2c8)],
      flow: u32[at(0x2cc)] & 255, waitingResult: !!(u32[at(0x2cc)] & 256), rows: [], tags: [],
    };
    // Race start: before go, where each other player's name tag goes on the grid (race.h race_tags), page peers order.
    if (u32[at(0x458)] === 0x53474154)
      for (let k = 0; k < MAX_PEERS; k++) out.tags.push({ x: f32[at(0x45c + k * 12)], y: f32[at(0x460 + k * 12)], depth: f32[at(0x464 + k * 12)] });
    for (let i = 0; i < Math.min(u32[at(0x2b4)], MAX_ROWS); i++) {
      const o = ROW0 + i * ROW;
      out.rows.push({ id: u32[at(o)], place: u32[at(o + 4)], laps: u32[at(o + 8)], done: u32[at(o + 12)],
        pct: f32[at(o + 16)], time: f32[at(o + 20)], me: u32[at(o + 24)] === 1, name: text(o + 32, 16),
        ghost: !!(u32[at(o + 28)] & 1), off: !!(u32[at(o + 28)] & 2) }); // race start: passes through, off the road
    }
    return Atomics.load(i32, seq) === before ? out : null;
  }
  // Writes the page's part: f gets a writer for it.
  function page(f) {
    const p = block();
    if (p === null) return false;
    const { u8, u32, i32, f32 } = views, seq = (p + PAGE_SEQ) >> 2;
    Atomics.add(i32, seq, 1);
    try {
      f({
        u32: (o, v) => { u32[(p + PAGE + o) >> 2] = v >>> 0; },
        f32: (o, v) => { f32[(p + PAGE + o) >> 2] = v; },
        bytes: (o, list, n) => { for (let i = 0; i < n; i++) u8[p + PAGE + o + i] = list[i] | 0; },
        text: (o, s, n) => { const b = new TextEncoder().encode(s); for (let i = 0; i < n; i++) u8[p + PAGE + o + i] = i < n - 1 && i < b.length ? b[i] : 0; },
        peer: (i) => PEERS - PAGE + i * PEER,
      });
    } finally {
      Atomics.add(i32, seq, 1);
    }
    return true;
  }
  // Session, flags, us, the other players, launch and go (RacePage in race.h).
  let nativeEncounterOwnsPage=!!globalThis.carbonEncounterSession;
  addEventListener("carbon-encounter-session",e=>{nativeEncounterOwnsPage=e.detail===true;if(!nativeEncounterOwnsPage)tellGame();});
  function tellGame() {
    if(nativeEncounterOwnsPage)return;
    page((w) => {
      const others = race ? race.roster.filter((r) => r.id !== me) : [];
      const mine = race?.roster.find((r) => r.id === me);
      w.u32(0x00, race ? race.race : 0);
      w.u32(0x04, (hosting && !race && !lobby ? CATCH : 0) | (race?.ai ? KEEP_AI : 0) | (race?.world || lobby?.world || worldHosting ? WORLD : 0) | (race?.cancelled ? CANCEL : 0) |
                  (race?.went && race.lateMe ? LATE : 0));
      w.u32(0x08, me);
      // A late start waits behind the grid: two slots past the last player's, a whole row behind them.
      w.u32(0x0c, mine ? race.went && race.lateMe ? Math.max(...race.roster.map((r) => r.grid)) + 2 : mine.grid : 0);
      w.u32(0x10, others.length);
      w.u32(0x14, race?.went || race?.cancelled ? race.race : 0);
      w.u32(0x18, race && !race.cancelled ? race.race : 0);
      w.u32(0x1c, race ? race.key : 0);
      w.bytes(0x20, race ? race.opts : [], 40);
      w.u32(0x48, pickCommand);
      for (let i = 0; i < MAX_PEERS; i++) {
        const o = w.peer(i), r = others[i], s = r && standings.get(r.id);
        w.u32(o, r ? r.id : 0);
        w.u32(o + 4, r ? r.car : 0);
        w.u32(o + 8, r ? r.grid : 0);
        // A player given up on counts as finished (last, GONE_TIME) only once this player has finished:
        // the game ranks a finished racer above everyone still racing, so they showed as 1st.
        const done = s?.done && (!s.gone || finishSent === race?.race);
        // Bit 1: race.h takes their car off the road. Race start: a late player's car stays off it while they load
        // (4), then comes on passing through the others until clear (8).
        const late = !!(race?.went && race.late?.has(r?.id));
        // Before the go, 16: they have loaded, so race.h shows their car parked on its grid slot.
        w.u32(o + 12, (done ? 1 : 0) | (s?.left ? 2 : 0) | (late && !race.ready.has(r.id) ? 4 : 0) | (late ? 8 : 0) |
                      (race && !race.went && race.ready.has(r?.id) ? 16 : 0));
        w.f32(o + 16, done ? s.time : 0);
        // Their progress as their game works it out, at their physics clock: race.h
        // carries it on to the moment their car is drawn at.
        w.f32(o + 20, s ? s.pct : 0);
        w.f32(o + 24, s ? s.lapPct : 0);
        w.u32(o + 28, s ? s.lap : 0);
        w.u32(o + 32, s ? isDrift(race.type) ? ((s.score || 0) | (s.dnf ? 0x80000000 : 0)) >>> 0
          // Canyon Duel: their points, and how their duel ended once it has (bit 31 lost, bit 30 won; neither: an
          // older game that does not say). race.h decides this player's duel from it (race_duel_frame).
          : race.type === CANYON_DUEL ? (((s.score || 0) & 0x3fffffff) | (s.done && s.duel === "lost" ? 0x80000000 : 0) |
                                        (s.done && s.duel === "won" ? 0x40000000 : 0)) >>> 0 : s.cp : 0);
        w.u32(o + 36, s ? s.ms : 0);
        w.f32(o + 40, s ? s.rate : 0);
        w.f32(o + 44, s ? s.lapRate : 0);
        w.text(o + 48, r ? r.name : "", 16);
      }
    });
  }

  // ---- The relay ----
  const myName = () => window.recompOnlineState?.name || window.recompOnline?.name || "Player";
  function send(k, fields = {}) {
    window.recompOnlineSend?.({ ...fields, t: "race", k });
  }
  const hex = (bytes) => bytes.map((b) => (b & 255).toString(16).padStart(2, "0")).join("");
  const unhex = (s) => (typeof s === "string" && /^[0-9a-f]{80}$/.test(s) ? s.match(/../g).map((x) => parseInt(x, 16)) : null);
  const uint = (n) => Number.isInteger(n) && n > 0 && n <= 0xffffffff;
  const what = (l) => `${TYPES[l.type] || "Race"}${l.type === 1 || l.type === 3 ? `, ${l.laps} ${l.laps === 1 ? "lap" : "laps"}` : ""}`;
  function roster(list) {
    if (!Array.isArray(list)) return null;
    const out = [];
    for (const r of list.slice(0, MAX_PEERS + 1))
      if (r && uint(r.id) && !out.some((o) => o.id === r.id))
        out.push({ id: r.id, name: String(r.name || "Player").slice(0, 20), car: uint(r.car) ? r.car : 0,
                   grid: Number.isInteger(r.grid) && r.grid >= 0 && r.grid < 30 ? r.grid : out.length });
    return out;
  }
  function lobbyFields(l) {
    // age: seconds the lobby has been open, so a player who arrives later sees how long (older pages ignore it).
    return { rv: 3, race: l.race, key: l.key, type: l.type, opts: hex(l.opts), laps: l.laps, ai: l.ai, world: l.world === true,
             age: Math.max(0, Math.round((performance.now() - (l.at ?? performance.now())) / 1000)),
             roster: l.roster.map(({ id, name, car }) => ({ id, name, car })) };
  }

  const retired = new Set();
  function retire(id) {
    if (!id) return;
    retired.add(id);
    if (retired.size > 64) retired.delete(retired.values().next().value);
  }
  // Whether "Not now" still keeps this host's races off the corner.
  function declinedHost(id) {
    const until = declined.get(id);
    if (until === undefined) return false;
    if (performance.now() < until) return true;
    declined.delete(id);
    return false;
  }
  // Whether another player's hosted race shows in the corner (card or chip): a race this
  // player could join from where they are. A filter on this page only: the relay and the
  // host see nothing, and F9 → Race still lists every open race. A race we have joined
  // always shows (its start needs the invite).
  function showsInvite(inv, g = game()) {
    if (race?.race === inv.race) return false; // a late repeat of the race we are in
    if (inv.joined) return true;
    if (!invitesOn || race || lobby || nativeEncounterOwnsPage || declinedHost(inv.from)) return false;
    if (inv.roster.length >= placesOf(inv.type)) return false; // full: its host takes nobody else
    // A race of this player's own (career, Challenge Series, solo Quick Race). The game keeps this
    // flag on after a race until free roam, so in the menus (flow 3) it means nothing.
    if (g?.racing && g.flow !== 3) return false;
    // A race hosted in career free roam is joined from career free roam. A player in the menus (a new player
    // may have no career at all) is not shown it: they could not join it from there.
    if (inv.world && !(g?.worldReady && g.flow === 6 && g.roam)) return false;
    for (const other of invites.values()) if (other.joined && other.race !== inv.race) return false;
    return true;
  }
  const shownInvites = (g = game()) => [...invites.values()].filter((inv) => showsInvite(inv, g)); // the game read once
  // A race hosted by a player of this player's crew. Only the crew id says who is in a crew
  // (net.js), so with a relay without protected crews nobody is a crew mate.
  function crewMate(inv) {
    const s = window.recompOnlineState, mine = s?.crewId;
    return !!mine && (s.people || []).some((p) => p.id === inv.from && p.crewId === mine);
  }
  // With several hosts at once, the corner and F9 keep their order: the race we joined, else the newest
  // hosted race (by when it was first heard: every host repeats its lobby, so the last repeat would flip between them).
  const newestFirst = (a, b) => b.at - a.at;
  function shownInvite() {
    const all = shownInvites();
    return all.find((i) => i.joined) || all.sort(newestFirst)[0];
  }
  // What the corner shows: one or two races as cards; from three, a chip with the count and a card only
  // for a crew mate's race (the newest). The joined race alone shows, as a card (showsInvite).
  function corner() {
    const all = shownInvites().sort(newestFirst);
    if (all.length < CHIP_FROM) return { cards: all, chip: 0 };
    const mate = all.find(crewMate);
    return { cards: mate ? [mate] : [], chip: all.length };
  }
  // Whether the corner changed since the last look (a player entered the menus, a race filled, ...).
  let cornerKey = "";
  function cornerChanged() {
    const c = corner(), key = `${c.chip}:${c.cards.map((i) => `${i.race}.${i.joined}.${i.roster.length}`).join()}`;
    if (key === cornerKey) return false;
    cornerKey = key;
    return true;
  }
  function setInvites(on) {
    invitesOn = !!on;
    try { localStorage.setItem("nfsc.raceInvites", String(invitesOn)); } catch {}
    // After menu.js's click handler: drawing here would replace the focused button under it.
    setTimeout(draw);
  }

  // ---- F9 → Race: the open races (menu.js draws the list) ----
  const KIND_COLOR = { 0: "#3fe0e8", 1: "#ffcf3f", 3: "#ffcf3f", 5: "#2ee88a", 11: "#b04dff", 12: "#b04dff", 13: "#e8213a" };
  // The list's filters, in the order shown; menu.js shows only those with a race.
  const FILTERS = [
    { id: "all", label: "All", has: () => true },
    { id: "quick", label: "Quick Race", has: (r) => !r.world },
    { id: "roam", label: "Free roam", has: (r) => r.world },
    { id: "sprint", label: "Sprint", has: (r) => r.type === 0 },
    { id: "circuit", label: "Circuit", has: (r) => r.type === 1 },
    { id: "drift", label: "Drift", has: (r) => isDrift(r.type) },
    { id: "duel", label: "Canyon Duel", has: (r) => r.type === CANYON_DUEL },
  ];
  // Why this player cannot join a race from where they are: the button's word and the reason, or null.
  function joinBlock(inv, g) {
    if (inv.joined) return null;
    if (inv.roster.length >= placesOf(inv.type)) return { button: "Full", why: "This race is full." };
    if (race) return { button: "Join", why: "Finish or leave your own race first." };
    if (lobby) return { button: "Join", why: "Cancel your own lobby first." };
    if (nativeEncounterOwnsPage) return { button: "Join", why: "Finish the street race first." };
    if (!g?.driftReady) return { button: "Join", why: "Reload the game to use the updated Race Together." };
    if (inv.world && !(g.worldReady && g.flow === 6 && g.roam)) return { button: "Free roam", why: "Join from career free roam." };
    if (!inv.world && g.flow !== 3) return { button: "Menus", why: "Join from the game's menus (Quick Race)." };
    return null;
  }
  // Crew mates' races first, then those nearly full (fewest places left; full ones last), then the newest.
  function byOpen(a, b) {
    const left = (r) => r.max - r.players;
    return (b.crew - a.crew) || ((left(a) <= 0) - (left(b) <= 0)) || (left(a) - left(b)) || (b.opened - a.opened);
  }
  // Every open race, joinable from here or not, for F9 → Race: {rows, filters: [{id, label, count}]}.
  function openRaces(filter = "all") {
    const g = game(), now = performance.now();
    const all = [...invites.values()].filter((inv) => race?.race !== inv.race).map((inv) => {
      const block = joinBlock(inv, g), laps = inv.type === 1 || inv.type === 3 ? `${inv.laps} ${inv.laps === 1 ? "lap" : "laps"}` : "";
      return { race: inv.race, host: inv.name || "Player", crew: crewMate(inv), type: inv.type, kind: TYPES[inv.type] || "Race",
               color: KIND_COLOR[inv.type] || "#3fe0e8", world: inv.world,
               event: inv.track || [inv.world ? "Free roam" : "Quick Race", laps].filter(Boolean).join(" · "),
               players: inv.roster.length, max: placesOf(inv.type), ai: inv.ai, opened: inv.opened,
               minutes: Math.max(0, Math.floor((now - inv.opened) / 60000)), joined: !!inv.joined,
               canJoin: !block, button: inv.joined ? "Leave" : block ? block.button : "Join", why: block?.why || "",
               hidden: !inv.joined && (!invitesOn || declinedHost(inv.from)) };
    }).sort(byOpen);
    const filters = FILTERS.map(({ id, label, has }) => ({ id, label, count: all.filter(has).length })).filter((f) => f.id === "all" || f.count);
    const chosen = FILTERS.find((f) => f.id === filter && filters.some((s) => s.id === f.id)) || FILTERS[0];
    return { rows: all.filter(chosen.has), filters, filter: chosen.id, total: all.length };
  }
  // A short line about another player: their car stopping for no visible reason needs explaining.
  function toast(text) {
    if (!root) return;
    if (!toastEl) { toastEl = el("div", "crc-toast"); root.append(toastEl); }
    toastEl.replaceChildren(el("span", null, text));
    toastEl.hidden = false;
    toastAt = performance.now();
  }
  // Cars in view only: a room can hold hundreds of players, and only these were visibly there.
  function watchPlayers(state) {
    const ids = state.slotIds || [], now = performance.now(), here = new Set();
    ids.forEach((id, i) => {
      if (!id) return;
      here.add(id);
      const w = watched.get(id) || { menuAt: 0, told: false };
      watched.set(id, w);
      w.name = state.people?.find((p) => p.id === id)?.name || w.name;
      if (state.slotModes?.[i] !== 0) { w.menuAt = 0; w.told = false; }
      else if (!w.menuAt) w.menuAt = now;
    });
    for (const [id, w] of watched) if (!here.has(id)) {
      if (w.name && !(state.people || []).some((p) => p.id === id)) toast(`${w.name} left the game.`);
      watched.delete(id);
    }
  }
  function watchMenus(now) {
    if (toastEl && !toastEl.hidden && now - toastAt >= TOAST_FOR) toastEl.hidden = true;
    for (const w of watched.values()) if (w.menuAt && !w.told && w.name && now - w.menuAt >= MENU_AFTER) {
      w.told = true;
      toast(`${w.name} went to the safehouse or a menu.`);
    }
  }
  const allFinished = () => !!race && race.roster.every(p => p.id === me ? finishSent === race.race : standings.get(p.id)?.done);
  function onlineChanged(state) {
    if (typeof state?.open !== "boolean") return;
    watchPlayers(state);
    const linked = state.open && (!me || !state.me || state.me === me);
    const present = new Set((state.people || []).map(p => p.id));
    present.add(me);
    if (!linked) {
      hosting = false;
      worldHosting = false;
      retire(lobby?.race);
      lobby = null;
      for (const id of invites.keys()) retire(id);
      invites.clear();
      pendingJoin = 0;
      if (race && (!race.went || race.world) && !allFinished()) cancelLoading();
      tellGame();
      draw();
      return;
    }
    for (const [id, inv] of invites) if (!present.has(inv.from)) {
      if (inv.joined && takeOver(inv, present)) continue;
      invites.delete(id);
      retire(id);
      if (pendingJoin === id) pendingJoin = 0;
    }
    if (lobby) {
      const before = lobby.roster.length;
      lobby.roster = lobby.roster.filter(p => present.has(p.id));
      if (lobby.roster.length !== before) send("lobby", lobbyFields(lobby));
    }
    if (race && !race.went && !race.cancelled)
      for (const p of race.roster) if (!present.has(p.id)) drop(p.id, "left");
    // After the start a race has no host to cancel it: a player who has gone stops counting once
    // the grace is over, or everyone else waits for their finish on the results screen. A player
    // who has finished keeps this too: their car leaves the road either way.
    if (race?.went && !race.cancelled) {
      for (const p of race.roster) {
        const s = standings.get(p.id);
        if (!s || p.id === me) continue;
        if (present.has(p.id)) {
          delete s.goneAt;
          if (s.left) { s.left = false; tellGame(); }
        } else s.goneAt ??= performance.now();
      }
    }
  }

  function heard(m) {
    if (!m || m.t !== "race" || !uint(m.id) || !uint(m.race)) return;
    const from = m.id, self = from === me || (!me && m.self);
    if (retired.has(m.race)) return;
    if (m.k === "lobby") {
      if (lobby && lobby.race === m.race && !me) me = from; // our own lobby came back: that is us
      const r = roster(m.roster), opts = unhex(m.opts);
      if (!r || !opts || !uint(m.key)) return;
      if (lobby && lobby.race === m.race) { draw(); return; }
      if (race?.race === m.race) return; // a late repeat of the race we are in
      const old = invites.get(m.race);
      if (m.rv !== 3 || !game()?.driftReady || m.world === true && !worldType(m.type | 0)) return;
      // A repeat sent before the host heard our leave still lists us: we have left all the same.
      const justLeft = old?.leftAt && performance.now() - old.leftAt < LOBBY_EVERY + 1000;
      const joined = !!old?.joined || (!!me && r.some((x) => x.id === me) && !justLeft);
      // A full duel without us: its host turned our join away, or someone else took the place.
      if ((m.type | 0) === CANYON_DUEL && r.length >= 2 && !(me && r.some((x) => x.id === me))) {
        if (old) {
          if (old.joined) { if (pendingJoin === m.race) pendingJoin = 0; toast(`${m.name || "The host"}'s Canyon Duel is full.`); }
          invites.delete(m.race);
          draw();
        }
        return;
      }
      const type = m.type | 0, now = performance.now();
      if (needsNativeMatch(type)) {
        if (!race && !lobby && showsInvite({ from, race: m.race, type, world: m.world === true, roster: r, joined })) nativeModeNotice(type);
        return;
      }
      if (!old && invites.size >= MAX_OPEN) return;
      // When it opened: the host says how long ago (age, seconds), else when it was first heard here.
      const age = Number.isInteger(m.age) && m.age >= 0 && m.age < 86400 ? m.age * 1000 : 0;
      const fresh = { from, name: m.name, race: m.race, key: m.key, type, laps: m.laps | 0, opts,
                      ai: !isDrift(type) && m.world !== true && m.ai === true, world: m.world === true, roster: r,
                      track: typeof m.track === "string" ? m.track.slice(0, 40) : "",
                      at: old?.at ?? now, opened: old?.opened ?? now - age, seen: now, joined, leftAt: old?.leftAt };
      // In place: a card's buttons keep the object they were drawn with, and a repeat must not leave them a stale copy.
      if (old) Object.assign(old, fresh); else invites.set(m.race, fresh);
      draw();
    } else if (m.k === "join" || m.k === "leave") {
      if (m.k === "join" && pendingJoin === m.race && !me) me = from;
      if (!lobby || lobby.race !== m.race) {
        if (m.k === "leave" && race && race.race === m.race && !race.cancelled && from !== me) {
          if (!race.went) drop(from, m.reason === "menu" ? "is in the safehouse or a menu" : "left");
          else {
            const s = standings.get(from);
            if (s && !s.done) s.goneAt = performance.now() - GONE_GRACE;
            // After the start a duel is not cancelled: the player who stays wins (race.h race_duel_frame).
            if (s && race.type === CANYON_DUEL && !s.duel && finishSent !== race.race) toast(`${s.name} left the duel: you win.`);
          }
        } else if (m.k === "join") draw();
        return;
      }
      if (m.k === "join" && (m.rv !== 3 || lobby.world && m.world !== true)) return;
      lobby.roster = lobby.roster.filter((r) => r.id !== from);
      // A Canyon Duel is one against one: a third player would leave it unable to start.
      if (m.k === "join" && lobby.roster.length < (lobby.type === CANYON_DUEL ? 2 : MAX_PEERS + 1))
        lobby.roster.push({ id: from, name: m.name || "Player", car: uint(m.car) ? m.car : 0 });
      send("lobby", lobbyFields(lobby));
      draw();
    } else if (m.k === "cancel") {
      const inv = invites.get(m.race);
      if (inv && inv.from === from) { invites.delete(m.race); retire(m.race); }
      if (race && race.race === m.race && race.host === from && (!race.went || race.world)) {
        if (typeof m.why === "string") race.why = m.why.slice(0, 120);
        cancelLoading();
      }
      draw();
    } else if (m.k === "start") {
      if(m.rv !== 3 || !game()?.driftReady || m.world === true && !worldType(m.type | 0)) return;
      // A race that starts is no longer open: off the list, whether or not we are in it.
      const opened = invites.get(m.race);
      if (opened && opened.from === from && !opened.joined) { invites.delete(m.race); draw(); }
      if (m.world === true) {
        const inv = invites.get(m.race), own = lobby?.race === m.race && from === me;
        if (!own && !(inv?.from === from && inv.joined)) return;
        const g = game();
        if (!g?.worldReady || g.flow !== 6 || !g.roam) {
          send("leave", {race:m.race, reason:"menu"}); invites.delete(m.race); return;
        }
      }
      // Relay duplicates and another lobby must not replace an in-flight race.
      if (race || nativeEncounterOwnsPage) return;
      invites.delete(m.race);
      const r = roster(m.roster), opts = unhex(m.opts);
      if (!r || !opts || !uint(m.key)) return;
      if (lobby && lobby.race === m.race) { if (!me) me = from; lobby = null; hosting = false; worldHosting = false; }
      if (!r.some((x) => x.id === me)) { draw(); return; } // a race without us
      if (needsNativeMatch(m.type | 0)) { nativeModeNotice(m.type | 0); return; }
      modeNotice = "";
      race = { race: m.race, host: from, key: m.key, type: m.type | 0, laps: m.laps | 0, opts, ai: !isDrift(m.type | 0) && m.world !== true && m.ai === true, world: m.world === true, roster: r,
               ready: new Set(), gone: new Set(), startedAt: performance.now(), went: false, launched: false,
               // Race start: the host's timer, when the first player was loaded, each player's loading step, who starts late.
               wait: (Number.isInteger(m.wait) && m.wait >= 5 && m.wait <= 120 ? m.wait : 15) * 1000, firstReadyAt: 0,
               load: new Map(), late: new Set(), lateMe: false };
      standings = new Map(r.map((x) => [x.id, { name: x.name, score: 0, place: 0, lap: 0, cp: 0, pct: 0, lapPct: 0, ms: 0, rate: 0,
                                                 lapRate: 0, t: 0, done: false, time: 0, at: 0 }]));
      finishSent = 0;
      pendingJoin = 0;
      tellGame();
      draw();
    } else if (m.k === "ready") {
      if (race && !race.cancelled && race.race === m.race && race.roster.some((r) => r.id === from)) {
        const fresh = !race.ready.has(from);
        race.ready.add(from);
        race.firstReadyAt ||= performance.now(); // the timer starts with the first player loaded
        if (fresh) tellGame(); // their car comes: on its grid slot before the go, on the road after it (a late player)
        maybeGo();
        draw();
      }
    } else if (m.k === "load") {
      // Race start: how far a player's game has got (0 in the menus, 1 the track, 2 the cars); older pages ignore it.
      if (race && !race.cancelled && race.race === m.race && race.roster.some((r) => r.id === from) &&
          Number.isInteger(m.p) && m.p >= 0 && m.p <= 2) race.load.set(from, m.p);
    } else if (m.k === "go") {
      if (race && !race.cancelled && race.race === m.race && from === race.host && !race.went) {
        // Race start: late players are in without too, for older pages, which leave them out as before.
        const late = Array.isArray(m.late) ? m.late.filter(uint) : [];
        const without = (Array.isArray(m.without) ? m.without.filter(uint) : []).filter((id) => !late.includes(id));
        if (without.includes(me)) { race.leftOut = true; cancelLoading(); return; }
        race.went = true;
        race.wentAt = performance.now();
        race.late = new Set(late);
        race.lateMe = late.includes(me) && !race.ready.has(me) && !race.readySent;
        // Left out at the start: their car was built for the grid but will never move, so it goes off
        // the road at once (it stood among the others' cars, 2026-11-05), as a player who quits does.
        for (const id of without) { giveUp(id); const s = standings.get(id); if (s) s.left = true; }
        tellGame();
        draw();
      }
    } else if (m.k === "progress" || m.k === "finish") {
      if (!race || race.cancelled || !race.went || race.race !== m.race || !standings.has(from)) return;
      const s = standings.get(from);
      // A late player racing has loaded, even if their ready was lost (it is sent once after the go).
      if (race.late.has(from) && !race.ready.has(from)) { race.ready.add(from); tellGame(); }
      if (isDrift(race.type) && (!Number.isInteger(m.score) || m.score < 0 || m.score > 0x7fffffff)) return;
      // A Canyon Duel's points (an older page sends 0) and, with a finish, how the duel ended.
      const duelPts = race.type === CANYON_DUEL && Number.isInteger(m.score) && m.score >= 0 ? Math.min(m.score, 0x3fffffff) : null;
      const duelEnd = race.type === CANYON_DUEL && m.k === "finish" && (m.duel === "won" || m.duel === "lost") ? m.duel : null;
      // A drift's points can still change after its finish: the game banks the drift running at the line
      // (DriftFinalizeScore) after the finish itself, so the first finish could carry fewer points. Its
      // owner's later reports (a new finish, or progress) keep counting; the finish time stays the first.
      // (2026-11-06: a score sent after a finish was dropped, logs/drift-rows-20261106.)
      if (s.done && !s.gone) {
        if (isDrift(race.type) && !self && (s.score !== m.score || s.dnf !== (m.dnf === true))) {
          s.score = m.score; s.dnf = m.dnf === true; tellGame();
        }
        if (duelPts !== null && !self && (s.score !== duelPts || duelEnd && s.duel !== duelEnd)) {
          s.score = duelPts; if (duelEnd) s.duel = duelEnd; tellGame();
        }
        return;
      }
      if (isDrift(race.type)) {s.score = m.score;s.dnf = m.dnf === true;}
      if (duelPts !== null) s.score = duelPts;
      if (duelEnd) { s.duel = duelEnd; s.dnf = duelEnd === "lost"; }
      if (m.k === "progress") {
        for (const k of ["place", "lap", "cp"]) if (Number.isInteger(m[k]) && m[k] >= 0 && m[k] < 100) s[k] = m[k];
        const pct = Number.isFinite(m.pct) ? Math.max(0, Math.min(100, m.pct)) : s.pct;
        const lapPct = Number.isFinite(m.lp) ? Math.max(0, Math.min(100, m.lp)) : s.lapPct;
        const ms = Number.isInteger(m.ms) && m.ms >= 0 && m.ms <= 0xffffffff ? m.ms : 0;
        // How fast % race and % lap grow, from the last two reports (a new lap restarts % lap).
        if (ms && s.ms && ms > s.ms && ms - s.ms < 3000) {
          const dt = (ms - s.ms) / 1000;
          s.rate = Math.max(0, (pct - s.pct) / dt);
          s.lapRate = lapPct >= s.lapPct ? (lapPct - s.lapPct) / dt : s.lapRate;
        }
        Object.assign(s, { pct, lapPct, ms });
        if (Number.isFinite(m.rt) && m.rt >= 0) s.t = m.rt; // race time ("t" is the message type)
        s.at = performance.now();
        if (!self) tellGame();
      } else if (Number.isFinite(m.time) && m.time > 0 && m.time < 36000 && (!s.done || s.gone)) {
        s.done = true;
        s.gone = false;
        s.time = m.time;
        s.pct = 100;
        if (!self) tellGame();
      }
      syncLocalStanding();
    }
  }
  let pendingJoin = 0;

  // Only the roster's explicit ready messages can release the shared start.
  function maybeGo() {
    if (!race || race.cancelled || race.host !== me || race.wentSent) return;
    if (race.roster.some((r) => !race.ready.has(r.id) && !race.gone.has(r.id))) return;
    race.without = [...race.gone];
    race.wentSent = performance.now();
    send("go", race.without.length ? { race: race.race, without: race.without } : { race: race.race });
  }
  // Starts without whoever is not ready: one slow machine must not cost the others their race.
  const stillLoading = () => (race ? race.roster.filter((r) => !race.ready.has(r.id) && !race.gone.has(r.id)) : []);
  function startWithout() {
    if (!race || race.cancelled || race.went || race.host !== me || race.wentSent || !race.ready.has(me)) return false;
    race.without = [...race.gone, ...stillLoading().map((r) => r.id)];
    race.wentSent = performance.now();
    send("go", { race: race.race, without: race.without });
    return true;
  }
  // Race start: the timer ran out. Everyone loaded starts now; those still loading start late, each as soon as
  // their game has loaded (see the header). The host need not be loaded itself: then it starts late too.
  const timerLeft = (now = performance.now()) => race && race.firstReadyAt ? race.wait - (now - race.firstReadyAt) : null;
  function startLate() {
    if (!race || race.cancelled || race.went || race.host !== me || race.wentSent || race.type === CANYON_DUEL) return false;
    race.lateIds = stillLoading().map((r) => r.id);
    race.without = [...race.gone, ...race.lateIds];
    race.wentSent = performance.now();
    send("go", { race: race.race, without: race.without, late: race.lateIds });
    return true;
  }
  // A player who left before the go is dropped: left out of the go, as a slow one is (see the
  // header). Every game drops the same players, so all of them agree on who hosts now.
  function drop(id, why) {
    const p = race?.roster.find((r) => r.id === id);
    if (!p || race.cancelled || race.went || id === me || race.gone.has(id)) return;
    race.gone.add(id);
    if (race.type === CANYON_DUEL) { race.why = `${p.name} ${why}, so the race was cancelled.`; cancelLoading(); return; }
    const wasHost = race.host === id;
    if (wasHost) race.host = race.roster.find((r) => !race.gone.has(r.id))?.id || me;
    const host = race.host === me ? "you start it" : `${race.roster.find((r) => r.id === race.host)?.name || "the next player"} starts it`;
    toast(`${p.name} ${why}: the race goes on without them${wasHost ? `, and ${host}` : ""}.`);
    maybeGo();
    draw();
  }
  // The host left its lobby (see the header): the first player of its roster still here hosts it now.
  function takeOver(inv, present) {
    const rest = inv.roster.filter((r) => r.id !== inv.from && present.has(r.id));
    if (!rest.some((r) => r.id === me) || !inv.opts) return false;
    const old = inv.roster.find((r) => r.id === inv.from)?.name || "The host";
    if (rest[0].id === me) {
      invites.delete(inv.race);
      lobby = { race: inv.race, key: inv.key, type: inv.type, opts: inv.opts, laps: inv.laps, ai: inv.ai, world: inv.world,
                roster: rest.map(({ id, name, car }) => ({ id, name, car })), sentAt: performance.now(), at: inv.opened };
      send("lobby", lobbyFields(lobby));
      toast(`${old} left: you host this race now.`);
    } else {
      Object.assign(inv, { from: rest[0].id, roster: rest, seen: performance.now() });
      toast(`${old} left: ${rest[0].name} hosts this race now.`);
    }
    draw();
    return true;
  }
  function giveUp(id) {
    const s = standings.get(id), i = race.roster.findIndex((r) => r.id === id);
    if (s && !s.done) Object.assign(s, { done: true, gone: true, dnf: true, time: GONE_TIME + Math.max(i, 0) });
  }

  // ---- Actions ----
  function cancelLoading() {
    if (!race || race.went && !race.world || race.cancelled) return;
    race.cancelled = true;
    retire(race.race);
    if (document.pointerLockElement) document.exitPointerLock?.();
    // Release an already loaded game, but never launch a queued race later.
    // This is an abandoned race; neither client may publish a result for it.
    tellGame();
    draw();
  }
  function host(on) {
    modeNotice = "";
    if (on && nativeEncounterOwnsPage) return;
    const g = game();
    if(on && !g?.driftReady) {modeNotice = "Reload the game to use the updated Race Together.";draw();return;}
    hosting = on;
    worldHosting = !!(on && g?.worldReady && g.flow === 6 && g.roam);
    if (worldHosting) pickCommand++;
    if (!on && lobby) cancelLobby();
    tellGame();
    draw();
  }
  function cancelLobby() {
    if (!lobby) return;
    send("cancel", { race: lobby.race });
    retire(lobby.race);
    if (lobby.world) { hosting = false; worldHosting = false; }
    lobby = null;
    tellGame();
    draw();
  }
  function leaveRace() {
    if (!race?.world || race.cancelled) return;
    send("leave", { race: race.race }); // the others race on; a host hands the race on (see the header)
    cancelLoading();
  }
  function startLobby() {
    if (!lobby || lobby.type === CANYON_DUEL && lobby.roster.length !== 2) return;
    const l = lobby;
    const fields = { ...lobbyFields(l), wait: waitSecs, roster: l.roster.map((r, grid) => ({ id: r.id, name: r.name, car: r.car, grid })) };
    send("start", fields);
    pendingStart = { race: l.race, fields, first: performance.now(), at: performance.now() };
  }
  function canJoin(inv, g = game()) {
    // A car of 0 (the game has none chosen on some menu screens) shows as a copy of the other's own.
    return !!inv && !!g?.driftReady && !nativeEncounterOwnsPage && !race && !lobby && (inv.world
      ? g.worldReady && g.flow === 6 && g.roam : g.flow === 3);
  }
  function join(inv) {
    const g = game(), car = g?.car;
    if (!canJoin(inv, g) || inv.roster.length >= placesOf(inv.type) && !inv.joined) return;
    // One race at a time: joining another from F9's list leaves the one joined before.
    for (const other of invites.values()) if (other.joined && other !== inv) leave(other);
    pendingJoin = inv.race;
    send("join", { rv: 3, race: inv.race, car, world: inv.world === true });
    inv.joined = true;
    delete inv.leftAt;
    draw();
  }
  function leave(inv) {
    send("leave", { race: inv.race });
    inv.joined = false;
    inv.leftAt = performance.now();
    pendingJoin = 0;
    draw();
  }
  // "Not now": this host's races stay off the corner for NOT_NOW_FOR (F9 still lists them).
  function decline(inv) {
    declined.delete(inv.from);
    declined.set(inv.from, performance.now() + NOT_NOW_FOR);
    if (declined.size > 64) declined.delete(declined.keys().next().value);
    if (inv.joined) leave(inv);
  }

  // A cancel is sent three times: a guest that misses it waits out the whole loading timeout.
  function cancelForAll() {
    const fields = { race: race.race, why: race.why };
    send("cancel", fields);
    for (const ms of [RESEND_EVERY, 2 * RESEND_EVERY]) setTimeout(() => send("cancel", fields), ms);
  }

  // The messages that must arrive, and the players who have stopped arriving.
  function resend(now) {
    if (pendingStart) {
      const p = pendingStart, mine = race?.race === p.race ? race : null;
      const everyone = mine && mine.roster.every((r) => mine.ready.has(r.id) || mine.gone.has(r.id));
      if (mine?.went || mine?.cancelled || everyone || retired.has(p.race) || now - p.first > LOAD_TIMEOUT) pendingStart = null;
      else if (now - p.at >= RESEND_EVERY) { p.at = now; send("start", p.fields); }
    }
    if (!race || race.cancelled) return;
    if (race.readySent && !race.went && now - race.readySent >= RESEND_EVERY) {
      race.readySent = now;
      send("ready", { race: race.race });
    }
    if (race.host === me && race.wentSent && now - race.wentSent < RESEND_FOR && now - (race.goAt || race.wentSent) >= RESEND_EVERY) {
      race.goAt = now;
      send("go", race.lateIds?.length ? { race: race.race, without: race.without, late: race.lateIds } : { race: race.race, without: race.without });
    }
    if (race.finish && now - race.finish.first < RESEND_FOR && now - race.finish.at >= RESEND_EVERY) {
      race.finish.at = now;
      send("finish", race.finish.fields);
    }
    if (race.went) {
      let gone = false;
      race.roster.forEach((p, i) => {
        const s = standings.get(p.id);
        if (!s || p.id === me) return;
        const left = s.goneAt && now - s.goneAt >= GONE_GRACE;
        if (left && !s.left) { s.left = true; gone = true; }
        if (s.done) return;
        const silent = race.finish && now - Math.max(s.at || 0, race.finish.first) >= SILENT_LIMIT;
        if (!left && !silent) return;
        Object.assign(s, { done: true, gone: true, dnf: true, time: GONE_TIME + i });
        gone = true;
      });
      if (gone) { tellGame(); syncLocalStanding(); draw(); }
    }
  }

  // ---- Every 100 ms: the game's news ----
  function tick() {
    const g = game();
    const now = performance.now();
    for (const [k, inv] of invites) if (now - (inv.seen ?? inv.at) > FORGET) invites.delete(k);
    if (cornerChanged()) draw(); // we joined a lobby, opened one, a race began, or we moved where others can be joined
    // A stalled native writer must not disable the page's loading timeout.
    // Give the host time to announce its decision before a disconnected guest
    // falls back to local cancellation (still before the native 120 s release).
    const loadLimit = race?.host === me ? LOAD_TIMEOUT : LOAD_TIMEOUT + 30000;
    if (race && !race.went && !race.wentSent && !race.cancelled && now - race.startedAt >= loadLimit) {
      if (!startWithout()) {
        if (race.host === me) cancelForAll();
        cancelLoading();
      }
    }
    // Race start: the host's timer ran out (it starts with the first player loaded; a duel waits for both).
    if (race && race.host === me && !race.went && !race.wentSent && !race.cancelled && race.firstReadyAt &&
        timerLeft(now) <= 0) startLate();
    resend(now);
    watchMenus(now);
    if (!g) return;
    // The host pressed Start in the Quick Race menu: the lobby opens.
    if (lastCaught < 0) lastCaught = g.caught;
    if (g.caught !== lastCaught) {
      lastCaught = g.caught;
      // A cancelled race that never left the menus must not eat the next Start: no lobby, no invite.
      if (race?.cancelled && g.phase === PHASE.idle) { race = null; tellGame(); }
      if (hosting && !lobby && !race && g.caughtKey) {
        if (worldHosting && !worldType(g.caughtType)) {
          hosting = false; worldHosting = false; tellGame(); modeNotice = "Choose Sprint, Circuit, Speedtrap, Drift, Canyon Drift, or Canyon Duel."; draw(); return;
        }
        if (needsNativeMatch(g.caughtType)) {
          hosting = false; worldHosting = false; tellGame(); nativeModeNotice(g.caughtType); return;
        }
        modeNotice = "";
        lobby = { race: ((Math.random() * 0x7ffffffe) | 0) + 1, key: g.caughtKey, type: g.caughtType, opts: g.caughtOpts,
                  laps: g.caughtOpts[4], ai: false, world: worldHosting, roster: [{ id: me || 0, name: myName(), car: g.car }], at: now };
        send("lobby", lobbyFields(lobby));
        lobby.sentAt = now;
        tellGame();
        draw();
      }
    }
    if (lobby) {
      if (me) for (const r of lobby.roster) if (!r.id) r.id = me;
      lobby.roster[0].car = g.car || lobby.roster[0].car;
      if (now - (lobby.sentAt || 0) > LOBBY_EVERY) { lobby.sentAt = now; send("lobby", lobbyFields(lobby)); }
    }
    if (race) {
      if (!race.launched && g.session === race.race && g.phase >= PHASE.launched) race.launched = true;
      if (!race.cancelled && g.phase >= PHASE.holding && !race.readySent && g.session === race.race) {
        race.readySent = now;
        race.firstReadyAt ||= now;
        send("ready", { race: race.race });
      }
      // Race start: until loaded, how far this game has got, for the others' lists (on a change, and every 2 s).
      if (!race.cancelled && !race.readySent) {
        const p = g.session === race.race ? Math.min(Math.max(g.phase, 0), 2) : 0;
        if (p !== race.loadSent || now - (race.loadAt || 0) >= LOAD_EVERY) {
          race.loadSent = p; race.loadAt = now;
          send("load", { race: race.race, p });
        }
      }
      if (race.host === me) maybeGo();
      const current = !race.cancelled && race.went && g.session === race.race && g.phase >= PHASE.racing;
      if (current && now - lastProgress > PROGRESS_EVERY) {
        lastProgress = now;
        send("progress", { race: race.race, place: g.place, lap: g.laps, cp: g.cp, score: g.score, dnf: g.dnf, pct: Math.round(g.pct * 1000) / 1000,
                           lp: Math.round(g.lapPct * 1000) / 1000, rt: Math.round(g.time * 1000) / 1000, ms: g.ms });
      }
      if (current && g.done && g.time > 0 && finishSent !== race.race) {
        finishSent = race.race;
        const fields = { race: race.race, score: g.score, dnf: g.dnf, time: Math.round(g.time * 1000) / 1000 };
        if (race.type === CANYON_DUEL && g.duelEnd) fields.duel = g.duelEnd === 2 ? "won" : "lost";
        send("finish", fields);
        race.finish = { fields, first: now, at: now };
        tellGame(); // the players given up on count as finished now (see tellGame)
      } else if (current && finishSent === race.race && race.finish && scored(race.type) &&
                 (g.score !== race.finish.fields.score || g.dnf !== race.finish.fields.dnf ||
                  race.type === CANYON_DUEL && g.duelEnd && race.finish.fields.duel !== (g.duelEnd === 2 ? "won" : "lost"))) {
        // Points banked after the finish (see heard), or a duel's outcome: the finish again, repeated as the first was.
        race.finish.fields = { ...race.finish.fields, score: g.score, dnf: g.dnf };
        if (race.type === CANYON_DUEL && g.duelEnd) race.finish.fields.duel = g.duelEnd === 2 ? "won" : "lost";
        Object.assign(race.finish, { first: now, at: now });
        send("finish", race.finish.fields);
      }
      // Back in the menus after the race: it is over.
      if (race.launched && g.phase === PHASE.idle && lastPhase !== PHASE.idle) {
        // An unexpected native exit must not leave the other games waiting.
        const complete = allFinished();
        // A Canyon Duel too (logs/duel-trucks-20261106): the other player then wins at once, and this car leaves their road.
        if ((race.world || race.type === CANYON_DUEL) && !race.cancelled && !complete)
          send("leave", { race: race.race });
        race.over = now;
      }
      if (race.over) { race = null; tellGame(); }
      checkList(g);
    }
    const cardState = `${g.flow}:${g.car}:${g.waitingResult}`;
    if (cardState !== lastCardState) { lastCardState = cardState; draw(); }
    lastPhase = g.phase;
    syncLocalStanding(g);
    drawStart(g, now);
  }

  // The game's list against what the players say: a mismatch goes to the console.
  // Only without AI: each game has AI racers of its own, so places differ there.
  let checkedAt = 0;
  function checkList(g) {
    if (!race || race.ai || performance.now() - checkedAt < 2000 || g.phase < PHASE.racing) return;
    checkedAt = performance.now();
    for (const row of g.rows) {
      const s = row.id && standings.get(row.id);
      if (s && s.at && performance.now() - s.at < 1000 && s.pct > 0.5 && s.place && row.place && s.place !== row.place && !s.done)
        console.info(`[race] ${s.name}: place ${row.place} in this game, ${s.place} in theirs`);
    }
  }

  // ---- What the player sees ----
  const css = `
  .crc { --ink:#e6f3f8; --dim:#87a0ad; --faint:#4b5f69; --teal:#3fe0e8; --crew:#2ee88a; --line:rgba(63,224,232,.18); --skew:-14deg;
    --cond:"Saira Condensed","Avenir Next Condensed","Arial Narrow",system-ui,sans-serif;
    --body:"Saira","Avenir Next","Segoe UI",system-ui,sans-serif;
    font:500 12.5px/1.32 var(--body); color:var(--ink); }
  .crc * { box-sizing:border-box; }
  .crc-stack { position:fixed; right:max(16px,env(safe-area-inset-right)); top:max(64px,calc(env(safe-area-inset-top) + 48px)); z-index:8;
    display:flex; flex-direction:column; align-items:flex-end; gap:8px; max-height:calc(100dvh - 88px); pointer-events:none; }
  .crc-stack > * { pointer-events:auto; }
  .crc-card { flex:0 1 auto; min-height:0;
    width:min(300px,calc(100vw - 32px)); overflow:auto; scrollbar-width:thin; scrollbar-color:var(--line) transparent;
    display:flex; flex-direction:column; gap:7px; padding:9px 12px 10px; border:1px solid var(--line);
    background:linear-gradient(160deg,rgba(63,224,232,.08),transparent 45%),rgba(6,11,15,.92);
    clip-path:polygon(0 0,calc(100% - 12px) 0,100% 12px,100% 100%,0 100%); }
  .crc-head { display:flex; align-items:center; justify-content:space-between; gap:10px; min-height:22px; }
  .crc-card h2 { margin:0; min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
    font:italic 700 12px/1 var(--cond); letter-spacing:.26em; text-transform:uppercase; color:var(--teal); }
  .crc-card p { margin:0; color:#b5c9d1; }
  .crc-card b { color:var(--ink); font-weight:600; }
  .crc-summary { margin:0; font:600 13px/1.2 var(--cond); letter-spacing:.04em; color:var(--ink); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .crc-summary span { color:var(--dim); font-weight:500; }
  .crc-who { display:grid; gap:2px; max-height:calc(23px * 8 + 14px); overflow:auto; scrollbar-width:thin; scrollbar-color:var(--line) transparent; }
  .crc-player { display:grid; grid-template-columns:minmax(0,1fr) auto; align-items:center; gap:8px; min-height:23px; padding:2px 8px;
    background:rgba(255,255,255,.035); border-left:2px solid var(--faint); }
  .crc-player.me { border-left-color:var(--teal); background:rgba(63,224,232,.1); }
  .crc-player .player-name { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; font-size:12px; }
  .crc-player .player-state { font:600 10.5px/1 var(--cond); letter-spacing:.1em; color:var(--dim); white-space:nowrap; }
  .crc-player.ready .player-state { color:var(--teal); }
  .crc-card.crc-small { width:auto; flex-direction:row; align-items:center; gap:10px; padding:5px 12px; clip-path:polygon(0 0,calc(100% - 8px) 0,100% 8px,100% 100%,0 100%);
    font:600 12px/1 var(--cond); letter-spacing:.06em; }
  .crc-toast { position:fixed; left:50%; top:max(16px,env(safe-area-inset-top)); z-index:8; transform:translateX(-50%); max-width:min(420px,calc(100vw - 32px));
    padding:6px 14px; border:1px solid var(--line); background:rgba(6,11,15,.92); font:600 13px/1.25 var(--cond); letter-spacing:.04em; color:var(--ink); pointer-events:none; }
  .crc-row { display:flex; gap:6px; flex-wrap:wrap; }
  .crc-btn { flex:1; min-height:30px; padding:6px 12px; border:1px solid var(--line); border-radius:0; cursor:pointer;
    font:italic 700 12px/1 var(--cond); letter-spacing:.12em; text-transform:uppercase; color:var(--ink); background:rgba(0,0,0,.35);
    transform:skewX(var(--skew)); transition:background-color 120ms ease-out,border-color 120ms ease-out,color 120ms ease-out; }
  .crc-btn > span { display:inline-block; transform:skewX(14deg); }
  .crc-btn:hover:not(:disabled),.crc-btn:focus-visible { border-color:var(--teal); background-color:rgba(63,224,232,.14); outline:none; }
  .crc-btn:active:not(:disabled) { background-color:rgba(63,224,232,.26); }
  .crc-btn.go { color:#031317; border-color:var(--teal); background:var(--teal); font-weight:800; }
  .crc-btn.go:hover:not(:disabled),.crc-btn.go:focus-visible { background:#8ff3f8; }
  .crc-btn:disabled { color:var(--faint); border-color:var(--line); background:transparent; cursor:not-allowed; }
  .crc-btn.crc-min { flex:none; min-height:20px; padding:3px 8px; font-size:10px; letter-spacing:.1em; color:var(--dim); }
  .crc-card.crc-small .crc-btn { flex:none; min-height:22px; padding:3px 9px; font-size:10.5px; }
  .crc-card.crew { border-left:3px solid var(--crew); }
  .crc-card.crew h2 { color:var(--crew); }
  .crc-card.crc-two .crc-who { max-height:calc(23px * 3 + 4px); }
  /* Three or more races open: a chip, skewed like the buttons, that opens F9 → Race. */
  .crc-chip { display:flex; align-items:center; gap:9px; padding:6px 13px 6px 10px; border:1px solid var(--line); border-radius:0; cursor:pointer;
    background:rgba(6,11,15,.92); color:var(--ink); transform:skewX(-10deg); transition:border-color 120ms ease-out,background-color 120ms ease-out; }
  .crc-chip > * { transform:skewX(10deg); }
  .crc-chip:hover,.crc-chip:focus-visible { border-color:var(--teal); background-color:rgba(10,26,32,.95); outline:none; }
  .crc-flag { width:20px; height:14px; flex:none; background:conic-gradient(#fff 25%,#0b0f12 0 50%,#fff 0 75%,#0b0f12 0) 0 0/7px 7px; }
  .crc-chip .crc-n { font:italic 800 20px/1 var(--cond); color:var(--teal); }
  .crc-chip .crc-t { font:700 12.5px/1 var(--cond); letter-spacing:.1em; text-transform:uppercase; }
  .crc-chip kbd { font:700 10.5px/1 var(--cond); border:1px solid var(--dim); padding:2px 5px; color:var(--dim); }
  .crc-check { display:flex; align-items:center; gap:7px; margin:0; font-size:12px; color:#b5c9d1; cursor:pointer; }
  .crc-check input { width:14px; height:14px; margin:0; accent-color:var(--teal); }
  .crc-note { color:var(--dim); font-size:11px; line-height:1.35; }
  .crc [hidden] { display:none !important; }
  @media(max-width:600px) { .crc-stack { right:max(12px,env(safe-area-inset-right)); top:max(48px,calc(env(safe-area-inset-top) + 40px)); } .crc-card { width:min(280px,calc(100vw - 24px)); } }
  @media(max-height:500px) { .crc-stack { top:max(40px,calc(env(safe-area-inset-top) + 32px)); max-height:calc(100dvh - 56px); } .crc-who { max-height:calc(23px * 4 + 6px); } }
  @media(prefers-reduced-motion:reduce) { .crc-btn,.crc-chip { transition:none; } }
  /* Race start (logs/race-start-mockup): the timer at the top, the racers on the right, a line at the bottom. */
  .crs,.crs-tag { --ready:#2ee88a; --load:#ffcf3f; --late:#b48bff; --left:#7d8a93; --panel:rgba(5,12,18,.86); pointer-events:none; }
  .crs [hidden] { display:none !important; }
  .crs-timer { position:fixed; left:50%; top:max(4vh,calc(env(safe-area-inset-top) + 8px)); transform:translateX(-50%); z-index:8; display:flex; align-items:center; gap:14px;
    max-width:calc(100vw - 32px); background:var(--panel); border:1px solid var(--line); padding:10px 22px 10px 14px; clip-path:polygon(4% 0,100% 0,96% 100%,0 100%); }
  .crs-ring { width:58px; height:58px; flex:none; position:relative; }
  .crs-ring svg { width:100%; height:100%; transform:rotate(-90deg); }
  .crs-ring circle { fill:none; stroke-width:5; }
  .crs-ring .bg { stroke:rgba(255,255,255,.12); }
  .crs-ring .fg { stroke:var(--teal); stroke-linecap:round; transition:stroke-dashoffset .25s linear; }
  .crs-ring span { position:absolute; inset:0; display:grid; place-items:center; font:italic 900 24px/1 var(--cond); }
  .crs-text { min-width:0; }
  .crs-text small { display:block; font:700 11px/1.3 var(--cond); letter-spacing:.24em; color:var(--teal); text-transform:uppercase; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .crs-text b { display:block; font:italic 900 clamp(18px,2.4vw,28px)/1.05 var(--cond); text-transform:uppercase; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .crs-text em { display:block; font-style:normal; color:var(--dim); font-size:12px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  /* Before the go it covers the game's own list (top right, names only then). */
  .crs-list { position:fixed; right:2%; top:15%; width:min(300px,30vw); z-index:8; display:flex; flex-direction:column; gap:3px; }
  /* In the race the game's own list is at the top right: ours (only while someone loads or passes through) goes under it. */
  .crs-list:not(.inrace) .crs-row { background:rgba(5,12,18,.95); }
  .crs-list.inrace { top:auto; bottom:max(34%,calc(env(safe-area-inset-bottom) + 220px)); width:min(260px,28vw); }
  .crs-list.inrace .crs-row { padding:4px 9px; }
  .crs-row { display:grid; grid-template-columns:22px minmax(0,1fr) auto; align-items:center; gap:8px; background:var(--panel); padding:6px 10px; border-left:3px solid var(--c); }
  .crs-row .n { font:italic 800 14px/1 var(--cond); color:var(--dim); }
  .crs-row .name { font:italic 800 16px/1.15 var(--cond); text-transform:uppercase; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .crs-row .st { font:700 11px/1 var(--cond); letter-spacing:.12em; color:var(--c); text-transform:uppercase; white-space:nowrap; }
  .crs-row .bar { grid-column:2 / 4; height:6px; background:rgba(255,255,255,.1); overflow:hidden; }
  .crs-row .bar i { display:block; height:100%; width:var(--p); background:var(--c); transition:width .4s linear; }
  .crs-row.loading .bar i { animation:crs-pulse 1.2s ease-in-out infinite alternate; }
  .crs-row.me .name::after { content:"YOU"; margin-left:6px; font:700 10px/1 var(--cond); color:#03131a; background:var(--teal); padding:1px 5px; vertical-align:2px; font-style:normal; }
  .crs-row.loading .name { opacity:.7; }
  .crs-row.dim { opacity:.6; }
  .crs-row.left { --c:var(--left); background:rgba(20,24,27,.75); }
  .crs-row.left .name { color:var(--left); }
  .crs-hint { position:fixed; left:50%; bottom:5%; transform:translateX(-50%); z-index:8; max-width:calc(100vw - 32px); background:var(--panel); padding:6px 14px; font:600 13px/1.3 var(--cond); letter-spacing:.06em; color:var(--dim); border:1px solid var(--line); text-align:center; }
  .crs-hint b { color:var(--ink); }
  .crc.crs-on .crc-toast { top:max(calc(4vh + 92px),100px); }
  .crs-tag { position:fixed; left:0; top:0; z-index:7; pointer-events:none; background:var(--panel); border:1px solid var(--c); padding:3px 9px;
    font:700 12px/1.2 var(--cond); letter-spacing:.08em; text-transform:uppercase; white-space:nowrap; color:var(--ink); }
  .crs-tag::after { content:""; position:absolute; left:50%; bottom:-13px; transform:translateX(-50%); border:6px solid transparent; border-top-color:var(--c); }
  .crs-tag b { font-weight:700; }
  .crs-tag span { color:var(--c); margin-left:6px; }
  .crs-tag[hidden] { display:none; }
  @keyframes crs-pulse { from { opacity:.45; } to { opacity:1; } }
  @media(max-width:700px) { .crs-list { width:44vw; top:22%; } .crs-timer { gap:10px; padding:8px 16px 8px 10px; } .crs-ring { width:46px; height:46px; } }
  @media(prefers-reduced-motion:reduce) { .crs-row .bar i,.crs-ring .fg { transition:none; animation:none; } }
  `;
  let root, stack, chip, card, card2, lastCardState = "", cardViewKey = "";
  let startEl = null, startParts = null, startKey = ""; // race start: the timer, the racers and the line at the bottom
  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }
  function button(label, fn, go = false) {
    const b = el("button", go ? "crc-btn go" : "crc-btn");
    b.type = "button";
    b.append(el("span", null, label));
    b.addEventListener("click", (e) => { e.preventDefault(); fn(); });
    b.addEventListener("mousedown", (e) => e.preventDefault()); // the game keeps the keyboard
    return b;
  }
  function names(list, ready = null, hostId = list[0]?.id) {
    const w = el("div", "crc-who");
    list.forEach((r) => {
      const row = el("div", `crc-player${r.id === me ? " me" : ""}${ready?.has(r.id) ? " ready" : ""}`);
      const state = ready ? `${r.id === hostId ? "HOST · " : ""}${ready.has(r.id) ? "READY" : "LOADING"}` : r.id === hostId ? "HOST" : r.id === me ? "YOU" : "JOINED";
      row.setAttribute("aria-label", `${r.name}, ${r.id === me ? "you, " : ""}${state}`);
      row.append(el("span", "player-name", r.name), el("span", "player-state", state));
      w.append(row);
    });
    return w;
  }
  // "Circuit, 3 laps · 2 players"; an invite also says how many it takes and whether AI race too.
  function summary(l, invite = false) {
    const w = el("div", "crc-summary"), n = l.roster.length;
    w.append(what(l), el("span", null, invite ? ` · ${n}/${placesOf(l.type)} players${l.ai ? " · AI" : ""}` : ` · ${n} ${n === 1 ? "player" : "players"}`));
    return w;
  }
  function head(title, small = false) {
    const h = el("div", "crc-head");
    h.append(el("h2", null, title));
    if (small) {
      const b = button("Minimize", () => { cardSmall = true; draw(); });
      b.className += " crc-min";
      h.append(b);
    }
    return h;
  }
  // The card: hosting hint, the host's lobby, an invite, or the race starting.
  function draw() {
    if (!root) return;
    dispatchEvent(new Event("recomp-race-invitations"));
    const shown = race || lobby ? { cards: [], chip: 0 } : corner(), inv = shown.cards[0];
    const g = game();
    // Retain controls across periodic redraws: replacing a pressed/focused
    // button before mouseup loses its click and keyboard focus.
    const cancelledKey = race?.cancelled ? `${race.race}:${!!((g?.flow === 3 || race.world && g?.roam) && g.phase === PHASE.idle)}`
      : modeNotice && !race && !lobby ? `notice:${modeNotice}` : "";
    if (cancelledKey && cancelledCardKey === cancelledKey) return;
    cancelledCardKey = cancelledKey;
    const viewKey = JSON.stringify({ me, hosting, modeNotice,
      race: race && { id: race.race, cancelled: race.cancelled, went: race.went, launched: race.launched, ready: [...race.ready], roster: race.roster, type: race.type, laps: race.laps },
      lobby: lobby && { type: lobby.type, laps: lobby.laps, ai: lobby.ai, roster: lobby.roster },
      invites: shown.cards.map((i) => ({ id: i.race, name: i.name, type: i.type, laps: i.laps, ai: i.ai, joined: i.joined, roster: i.roster, crew: crewMate(i) })),
      chip: shown.chip,
      game: g && { phase: g.phase, flow: g.flow, car: g.car, waitingResult: g.waitingResult },
      small: cardSmall, start: startShown() });
    if (viewKey === cardViewKey) return;
    cardViewKey = viewKey;
    card.className = "crc-card";
    card.replaceChildren();
    card2.replaceChildren();
    card2.hidden = true;
    chip.hidden = !shown.chip;
    if (shown.chip) {
      chip.setAttribute("aria-label", `${shown.chip} races open. Press F9 to see them.`);
      chip.replaceChildren(el("span", "crc-flag"), el("span", "crc-n", String(shown.chip)), el("span", "crc-t", "races open"), el("kbd", null, "F9"));
    }
    const waiting = race ? !race.cancelled && (!race.went || !!g?.waitingResult) : !!lobby;
    if (!waiting) cardSmall = false;
    if (cardSmall) {
      card.hidden = false;
      card.className = "crc-card crc-small";
      const line = race?.went ? "waiting for results" : race ? `${race.ready.size} of ${race.roster.length} ready` : `${lobby.roster.length} ${lobby.roster.length === 1 ? "player" : "players"}`;
      card.append(el("span", null, `Race together · ${line}`), button("Show", () => { cardSmall = false; draw(); }));
      return;
    }
    if (modeNotice && !race && !lobby) {
      card.hidden = false;
      card.append(head("Race together"), el("p", null, modeNotice),
        button("OK", () => { modeNotice = ""; draw(); }));
      return;
    }
    if (race?.cancelled) {
      card.append(head("Race cancelled"));
      card.append(el("p", null, race.why || (race.world ? "No shared result will be recorded." : race.leftOut ? "The others started without you, because your game was still loading." : "Not everyone was ready. No shared result will be recorded.")));
      // Back in the menus or free roam there is nothing to leave: only a game held on the
      // loading screen, which may never accept Esc, needs the exit (F9 Quit's path).
      const safe = (g?.flow === 3 || race?.world && g?.roam) && g.phase === PHASE.idle;
      if (safe) card.append(button("Dismiss", () => { race = null; tellGame(); draw(); }, true));
      else {
        card.append(el("div", "crc-note", race.world ? "Returning to free roam…" : "Exit to the start page, then press Play to return to the game."));
        card.append(button("Exit to start page", () => {
          if (window.recompQuitToStart) return window.recompQuitToStart(); // menu.js: lets the game finish a write first
          dispatchEvent(new Event("recomp-quit"));
          location.replace(location.pathname);
        }, true));
      }
    } else if (race && g?.waitingResult && g.session === race.race) {
      card.append(head("Waiting for results", true));
      card.append(el("p", null, isDrift(race.type) ? "Your drift score is recorded. Waiting for the other players to finish." : "Your finish time is recorded. Waiting for the other players to finish."));
      card.append(el("div", "crc-note", "If a player cannot finish, use F9 → Quit to the start page. No shared winner is confirmed until all results arrive."));
    } else if (race && !race.went) {
      // Race start: the timer and the racers (drawStart) take the card's place while everyone loads.
      if (startShown()) { card.hidden = true; return; }
      const host = race.roster.find((r) => r.id === race.host);
      card.append(head("Race together", true));
      card.append(summary(race), names(race.roster, race.ready, race.host));
      const loading = !race.launched;
      card.append(el("div", "crc-note", loading && g && g.phase === PHASE.idle
        ? race.world ? "Starting from free roam…" : g.flow === 3 ? "Starting… If nothing happens, answer any question on screen or go back to the main menu."
          : "Starting… (the race starts from the game's menus: leave free roam for the main menu)"
        : `Loading · ${race.ready.size} of ${race.roster.length} ready${host && host.id !== me ? ` · ${host.name} starts it` : ""}`));
    } else if (lobby) {
      card.append(head("Race together", true));
      card.append(summary(lobby), names(lobby.roster));
      if (!lobby.world && !isDrift(lobby.type)) {
      const ai = el("label", "crc-check"), box = el("input");
      box.type = "checkbox";
      box.checked = lobby.ai;
      box.addEventListener("change", () => { lobby.ai = box.checked; send("lobby", lobbyFields(lobby)); });
      ai.append(box, "AI opponents too");
      card.append(ai);
      }
      const row = el("div", "crc-row");
      const duel = lobby.type === CANYON_DUEL, duelReady = lobby.roster.length === 2;
      const start = button(duel && !duelReady ? "Needs 2 players" : lobby.roster.length > 1 ? `Start · ${lobby.roster.length} players` : "Start alone", startLobby, true);
      if (duel && !duelReady) start.disabled = true;
      row.append(start, button("Cancel", cancelLobby));
      card.append(row, el("div", "crc-note", lobby.world ? "Practice race · no career cash or race completion." : "Friends in this room can join now, from the game's menus."));
    } else if (inv && !race) {
      const two = shown.cards.length > 1;
      inviteCard(card, inv, g, two);
      if (two) { inviteCard(card2, shown.cards[1], g, two); card2.hidden = false; }
    } else if (hosting) {
      card.append(head("Race together"));
      card.append(el("p", null, worldHosting ? "Choose an event on the world map, then select Jump to Event to open your lobby." : "Pick a Quick Race and press Start: friends in this room can join before it begins."));
      const row = el("div", "crc-row");
      row.append(button("Stop hosting", () => host(false)));
      card.append(row);
    }
    card.hidden = !card.childElementCount;
  }
  // Another player's race: who hosts it, what it is, who is in, Join (or why not here) and Not now.
  // A crew mate's race says so, in the crew colour.
  function inviteCard(box, inv, g, two) {
    const joined = !!inv.joined, mate = crewMate(inv);
    box.className = `crc-card${mate ? " crew" : ""}${two ? " crc-two" : ""}`;
    box.append(head(mate ? "Crew invite" : "Race together"));
    box.append(el("p", null, `${inv.name || "A player"} invites you to race.`), summary(inv, true), names(inv.roster));
    const row = el("div", "crc-row");
    const joinButton = joined ? button("Leave", () => leave(inv)) : button(canJoin(inv, g) ? "Join" : inv.world ? "Enter free roam" : "Return to main menu", () => join(inv), true);
    joinButton.disabled = !joined && !canJoin(inv, g);
    row.append(joinButton,
               button("Not now", () => { decline(inv); draw(); }));
    box.append(row, el("div", "crc-note", joined ? `Joined · waiting for ${inv.name || "the host"} to start`
      : inv.world ? "Join from career free roam; F9 lists it too." : "Join from the game's menus; F9 lists it too."));
  }
  // Keep local result data for the shared protocol. The native leaderboard
  // displays standings; no second leaderboard is drawn by the page.
  function syncLocalStanding(g = game()) {
    if (!race || race.cancelled || !race.went || !g || g.session !== race.race || g.phase < PHASE.racing) return;
    const mine = standings.get(me);
    if (mine) {
      mine.score = g.score; mine.dnf = g.dnf; mine.place = g.place;
      mine.lap = g.laps; mine.pct = g.pct; mine.t = g.time;
      if (g.done) { mine.done = true; mine.time = g.time; }
    }
  }

  // ---- Race start: the timer, the racers, and the line at the bottom (logs/race-start-mockup) ----
  const myStep = (g) => (g?.session === race.race ? Math.min(Math.max(g.phase, 0), 2) : 0);
  const racingNow = (g) => !!g && g.session === race.race && g.phase >= PHASE.racing;
  // One racer's state for the list: ready, loading (with the step their game reported), late, racing or left.
  function racerState(r, g) {
    const s = standings.get(r.id), mine = r.id === me;
    if (race.gone.has(r.id) || s?.left) return { k: "left", text: "left" };
    if (!race.went) {
      if (race.ready.has(r.id) || (mine && race.readySent)) return { k: "ready", text: "ready ✓", p: 100 };
      const p = mine ? myStep(g) : race.load.get(r.id);
      return { k: "loading", p: p === 2 ? 66 : p === 1 ? 33 : 8, text: p === 2 ? "loading cars" : p === 1 ? "loading track" : p === 0 ? "starting…" : "loading…" };
    }
    if (race.late.has(r.id) || (mine && race.lateMe)) {
      if (!(mine ? racingNow(g) : race.ready.has(r.id))) return { k: "loading", text: "loading…", dim: true };
      const row = g?.rows.find((x) => (mine ? x.me : x.id === r.id));
      if (row?.ghost) return { k: "late", text: "late 👻" };
    }
    return { k: "racing", text: s?.done && !s.gone ? "finished" : "" };
  }
  const startShown = () => !!race && !race.cancelled && !!startModel(game(), performance.now());
  function startModel(g, now) {
    if (!race || race.cancelled) return null;
    const rows = race.roster.map((r, i) => ({ n: i + 1, name: r.name, me: r.id === me, ...racerState(r, g) }));
    const waiting = rows.filter((x) => x.k === "loading"), kicker = `Race together · ${what(race).replace(", ", " · ")}`;
    const players = (n) => `${n} ${n === 1 ? "player" : "players"}`;
    const lateNames = race.roster.filter((r) => race.late.has(r.id) && r.id !== me).map((r) => r.name);
    let timer = null, hint = "";
    if (!race.went) {
      const left = timerLeft(now), wait = race.wait / 1000, duel = race.type === CANYON_DUEL;
      const secs = left === null ? wait : Math.max(0, Math.ceil(left / 1000));
      if (!waiting.length) timer = { ring: "✓", frac: 0, head: "Everyone is ready", sub: "Starting together…" };
      else if (duel) timer = { ring: "…", frac: 1, head: `Waiting for ${waiting.length === 1 ? waiting[0].name : players(waiting.length)}`, sub: "A Canyon Duel starts when both players have loaded" };
      else if (left === null) timer = { ring: String(wait), frac: 1, head: "Loading the race", sub: `The timer starts when the first player has loaded · ${wait} s` };
      else if (left > 0) timer = { ring: String(secs), frac: left / race.wait, head: `Race starts in ${secs}`, sub: `Waiting for ${players(waiting.length)} to load` };
      else timer = { ring: "0", frac: 0, head: "Starting now", sub: `${waiting.map((x) => x.name).join(", ")} ${waiting.length === 1 ? "joins" : "join"} late` };
      const mine = rows.find((x) => x.me);
      if (mine?.k === "ready") hint = "Your car is ready · <b>the race starts when everyone is ready or the timer ends</b>";
      else if (g && g.phase === PHASE.idle && !race.launched)
        hint = race.world ? "Starting from free roam…" : g.flow === 3 ? "Starting… <b>answer any question on screen</b>, or go back to the main menu" : "The race starts from the game's menus: <b>leave free roam for the main menu</b>";
    } else if (race.lateMe && !racingNow(g)) {
      timer = { ring: "…", frac: 1, head: "The race has started", sub: "You join late, as soon as your game has loaded" };
      hint = "Your car starts <b>behind the grid</b> and passes through the others until it is clear of them";
    } else if (lateNames.length && now - (race.wentAt || 0) < NAMED_FOR) {
      timer = { ring: "✓", frac: 0, head: "Starting now", sub: `${lateNames.join(", ")} ${lateNames.length === 1 ? "joins" : "join"} late` };
      hint = `<b>${lateNames.map(htmlText).join(", ")}</b> will appear behind the grid, passing through the others until clear`;
    }
    // After the go the list stays only while a racer is loading or still passing through (late).
    const list = !race.went || rows.some((x) => x.k === "loading" || x.k === "late");
    if (!timer && !list) return null;
    return { kicker, timer, rows: list ? rows : null, hint };
  }
  const htmlText = (t) => String(t).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
  const COLOR = { ready: "var(--ready)", loading: "var(--load)", late: "var(--late)", left: "var(--left)", racing: "var(--teal)" };
  function drawStart(g = game(), now = performance.now()) {
    if (!startEl) return;
    const m = race && !race.cancelled ? startModel(g, now) : null;
    root.classList.toggle("crs-on", !!m?.timer);
    const key = JSON.stringify(m);
    if (key === startKey) return;
    startKey = key;
    startEl.hidden = !m;
    const { timer, fg, secs, small, head, sub, list, hint } = startParts;
    timer.hidden = !m?.timer;
    if (m?.timer) {
      const C = 2 * Math.PI * 25;
      if (fg) {
        fg.style.strokeDasharray = String(C);
        fg.style.strokeDashoffset = String(C * (1 - Math.max(0, Math.min(1, m.timer.frac))));
      }
      secs.textContent = m.timer.ring;
      small.textContent = m.kicker; head.textContent = m.timer.head; sub.textContent = m.timer.sub;
      timer.setAttribute("aria-label", `${m.timer.head}. ${m.timer.sub}`);
    }
    list.hidden = !m?.rows;
    list.classList.toggle("inrace", !!race?.went);
    list.replaceChildren(...(m?.rows || []).map((x) => {
      const row = el("div", `crs-row ${x.k}${x.me ? " me" : ""}${x.dim ? " dim" : ""}`), bar = el("div", "bar");
      row.style.setProperty("--c", COLOR[x.k]);
      row.style.setProperty("--p", `${x.k === "loading" ? x.p ?? 8 : 100}%`);
      bar.append(el("i"));
      row.append(el("span", "n", String(x.n)), el("span", "name", x.name), el("span", "st", x.text));
      if (!race.went) row.append(bar);
      row.setAttribute("aria-label", `${x.name}${x.me ? ", you" : ""}${x.text ? `, ${x.text}` : ""}`);
      return row;
    }));
    hint.hidden = !m?.hint;
    hint.innerHTML = m?.hint || "";
  }
  // Tags over the other players' cars on the grid, before the go (race.h race_tags projects them): their name and
  // ready ✓ or loading. Moved every frame, as net.js moves its name tags; net.js's own come with the go.
  const gridTags = [];
  function drawGridTags() {
    requestAnimationFrame(drawGridTags);
    // Only while this game holds on the grid: before that it may still show its loading screen (race.h agrees).
    const g0 = race && !race.cancelled && !race.went ? game() : null, g = g0?.phase === PHASE.holding ? g0 : null;
    const canvas = document.getElementById?.("canvas"), others = g && canvas ? race.roster.filter((r) => r.id !== me) : [];
    const box = others.length ? canvas.getBoundingClientRect() : null;
    for (let k = 0; k < Math.max(gridTags.length, others.length); k++) {
      const r = others[k], t = g?.tags[k];
      let e = gridTags[k];
      if (!r || !t || !(t.depth > 0) || window.recompWorldMapOpen?.()) { if (e) e.hidden = true; continue; }
      if (!e) {
        e = gridTags[k] = el("div", "crs-tag");
        e.append(el("b"), el("span"));
        root.append(e);
      }
      const st = racerState(r, g), label = st.k === "ready" ? "ready ✓" : st.k === "left" ? "left" : "loading";
      if (e.firstChild.textContent !== r.name) e.firstChild.textContent = r.name;
      if (e.lastChild.textContent !== label) e.lastChild.textContent = label;
      e.style.setProperty("--c", COLOR[st.k] || COLOR.loading);
      e.hidden = false;
      e.style.transform = `translate(${box.left + t.x * box.width}px, ${box.top + t.y * box.height}px) translate(-50%, -100%)`;
    }
  }
  // Name tags (net.js): a racer of this race keeps their tag at any distance.
  window.recompRaceRacer = (id) => !!race && !race.cancelled && race.roster.some((r) => r.id === id);
  // Name tags over the cars (net.js): a late player's car says so while it passes through.
  window.recompRaceTagNote = (id) => {
    if (!race || race.cancelled || !race.went || !race.late.has(id)) return "";
    return game()?.rows.find((x) => x.id === id)?.ghost ? "LATE 👻" : "";
  };

  // ---- The F9 menu ----
  // menu.js lists window.recompMenuActions as buttons ({id, group, label, hint, run, shown, keepOpen}).
  function menu() {
    const actions = (window.recompMenuActions ||= []);
    actions.push(
      // menu.js shows this one as the "+ Host a race" button of F9 → Race's open races.
      { id: "race-host", group: "Race together", label: () => (hosting ? "Stop hosting" : "+ Host a race"),
        hint: () => (hosting ? worldHosting ? "On · choose an event on the world map" : "On · pick a Quick Race, press Start" : game()?.worldReady && game()?.roam ? "Choose on world map" : ""), shown: () => !lobby && !race, run: () => host(!hosting) },
      { id: "race-start", group: "Race together", label: "Start the race", shown: () => !!lobby,
        hint: () => (lobby ? `${lobby.roster.length} ${lobby.roster.length === 1 ? "player" : "players"}` : ""), run: startLobby },
      { id: "race-leave-active", group: "Race together", label: () => race?.host === me ? "Cancel race" : "Leave race",
        shown: () => !!race?.world && !race.cancelled, run: leaveRace },
      { id: "race-cancel", group: "Race together", label: "Cancel the race", shown: () => !!lobby, run: cancelLobby },
      // Race start: how long the others wait for whoever is still loading, after the first player has loaded.
      { id: "race-wait", group: "Race together", label: "Start timer", keepOpen: true, shown: () => !race,
        hint: () => `${waitSecs} s`, run: () => {
          waitSecs = WAITS[(WAITS.indexOf(waitSecs) + 1) % WAITS.length];
          try { localStorage.setItem("nfsc.raceWait", String(waitSecs)); } catch {}
        } },
      // Joining is F9 → Race's open races list (menu.js, openRaces()); the single "Join …" button it replaced is gone.
      { id: "race-invites", group: "Race together", label: "Race invites", keepOpen: true,
        hint: () => (invitesOn ? "On" : "Off"), run: () => setInvites(!invitesOn) },
    );
    dispatchEvent(new Event("recomp-menu-toggles"));
  }

  function wake() {
    if (active) return;
    active = true;
    root = el("div", "crc");
    root.innerHTML = `<style>${css}</style>`;
    stack = el("div", "crc-stack");
    chip = el("button", "crc-chip");
    chip.type = "button";
    chip.hidden = true;
    chip.addEventListener("click", (e) => { e.preventDefault(); dispatchEvent(new Event("recomp-open-races")); }); // menu.js opens F9 → Race
    chip.addEventListener("mousedown", (e) => e.preventDefault()); // the game keeps the keyboard
    [card, card2] = [el("div", "crc-card"), el("div", "crc-card")];
    for (const c of [card, card2]) {
      c.setAttribute("role", "dialog");
      c.setAttribute("aria-label", "Race together");
      c.hidden = true;
    }
    stack.append(chip, card, card2);
    startEl = el("div", "crs");
    startEl.hidden = true;
    const timerEl = el("div", "crs-timer"), ring = el("div", "crs-ring"), text = el("div", "crs-text");
    timerEl.setAttribute("role", "status");
    ring.innerHTML = '<svg viewBox="0 0 60 60" aria-hidden="true"><circle class="bg" cx="30" cy="30" r="25"/><circle class="fg" cx="30" cy="30" r="25"/></svg>';
    startParts = { timer: timerEl, fg: ring.querySelector?.(".fg") || null, secs: el("span"), small: el("small"), head: el("b"), sub: el("em"),
                   list: el("div", "crs-list"), hint: el("div", "crs-hint") };
    ring.append(startParts.secs);
    text.append(startParts.small, startParts.head, startParts.sub);
    timerEl.append(ring, text);
    startEl.append(timerEl, startParts.list, startParts.hint);
    root.append(stack, startEl);
    if (typeof requestAnimationFrame === "function") requestAnimationFrame(drawGridTags);
    document.body.append(root);
    menu();
    addEventListener("recomp-online-message", (e) => heard(e.detail));
    addEventListener("carbon-race", () => tellGame());
    addEventListener("recomp-online", (e) => {
      // Reconnection changes relay identity. Cancel the previous session first.
      onlineChanged(e.detail);
      if (Number.isInteger(e.detail?.me) && e.detail.me > 0) me = e.detail.me;
      maybeGo();
      draw();
    });
    if (Number.isInteger(window.recompOnlineState?.me)) me = window.recompOnlineState.me;
    setInterval(tick, 100);
    setInterval(draw, 1000);
    tellGame();
  }
  addEventListener("recomp-online", wake);
  if (window.recompOnline || window.recompOnlineState) wake();
  // For tests: the state, and the actions the buttons run.
  // state().invites: the races the corner offers (joinable from here); open: every open race heard.
  window.recompRace = { state: () => ({ me, hosting, lobby, invites: shownInvites(), open: [...invites.values()], invitesOn, race, standings: [...standings], game: game() }),
                        host, startLobby, cancelLobby, leaveRace, canJoin, setInvites, openRaces, corner,
                        startView: () => startModel(game(), performance.now()), // race start: what the timer and the list show
                        join: (raceId) => { const i = raceId == null ? shownInvite() : invites.get(raceId); if (i) join(i); },
                        leave: (raceId) => { const i = invites.get(raceId); if (i?.joined) leave(i); } };
})();
