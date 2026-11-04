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
// 4. While racing, each player sends their own place, lap, progress and race
//    time four times a second: the list on the left shows everyone, from what
//    each player sends (a fallback, and a check of the game's own list). Each
//    player's own finish time goes to the others, whose games write it into
//    that player's row, so every results screen has the same order.
//
// THE RELAY carries one message type for all of it, {t:"race", k, race, ...}
// (tools/carbon-net-relay.mjs header), sent to everyone in the room, the sender
// too: k is lobby, join, leave, start, ready, go, progress, finish or cancel, and
// race is the session id the host made. net.js passes these to the relay and
// back as "recomp-online-message" events.
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
  const LOAD_TIMEOUT = 60000;
  const FORGET = 15000;      // a lobby nobody repeats is gone

  let active = false, me = 0;
  let modeNotice = "", cancelledCardKey = "";
  const isDrift = type => type === 11 || type === 12;
  const needsNativeMatch = type => type === 14;
  const worldType = type => [0, 1, 5, 11, 12].includes(type);
  function nativeModeNotice() {
    modeNotice = "Pursuit Tag needs Online → Custom Match. Race Together uses Quick Race and cannot start Pursuit Tag.";
    draw();
  }
  let hosting = false;       // F9: the next Quick Race Start opens a lobby
  let lobby = null;          // the host's own lobby: {race, key, type, opts, laps, ai, roster: [{id, name, car}]}
  let invites = new Map();   // race -> {from, name, race, type, laps, ai, roster, at}
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
      score: u32[at(4)] >= 3 ? u32[at(0x450)] : 0, dnf: u32[at(4)] >= 3 && !!u32[at(0x454)], roam: !!(u32[at(0x2cc)] & 0x800),
      phase: u32[at(0x264)], car: u32[at(0x268)], caught: u32[at(0x26c)], caughtKey: u32[at(0x270)],
      caughtType: u32[at(0x274)], caughtOpts: Array.from(u8.subarray(p + 0x278, p + 0x2a0)),
      place: u32[at(0x2a0)], laps: u32[at(0x2a4)], done: u32[at(0x2a8)], pct: f32[at(0x2ac)], time: f32[at(0x2b0)],
      loaded: u32[at(0x2b8)], session: u32[at(0x2bc)], lapPct: f32[at(0x2c0)], cp: u32[at(0x2c4)], ms: u32[at(0x2c8)],
      flow: u32[at(0x2cc)] & 255, waitingResult: !!(u32[at(0x2cc)] & 256), rows: [],
    };
    for (let i = 0; i < Math.min(u32[at(0x2b4)], MAX_ROWS); i++) {
      const o = ROW0 + i * ROW;
      out.rows.push({ id: u32[at(o)], place: u32[at(o + 4)], laps: u32[at(o + 8)], done: u32[at(o + 12)],
        pct: f32[at(o + 16)], time: f32[at(o + 20)], me: u32[at(o + 24)] === 1, name: text(o + 32, 16) });
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
      w.u32(0x04, (hosting && !race && !lobby ? CATCH : 0) | (race?.ai ? KEEP_AI : 0) | (race?.world || lobby?.world || worldHosting ? WORLD : 0) | (race?.cancelled ? CANCEL : 0));
      w.u32(0x08, me);
      w.u32(0x0c, mine ? mine.grid : 0);
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
        w.u32(o + 12, s?.done ? 1 : 0);
        w.f32(o + 16, s?.done ? s.time : 0);
        // Their progress as their game works it out, at their physics clock: race.h
        // carries it on to the moment their car is drawn at.
        w.f32(o + 20, s ? s.pct : 0);
        w.f32(o + 24, s ? s.lapPct : 0);
        w.u32(o + 28, s ? s.lap : 0);
        w.u32(o + 32, s ? isDrift(race.type) ? ((s.score || 0) | (s.dnf ? 0x80000000 : 0)) >>> 0 : s.cp : 0);
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
    return { rv: 3, race: l.race, key: l.key, type: l.type, opts: hex(l.opts), laps: l.laps, ai: l.ai, world: l.world === true,
             roster: l.roster.map(({ id, name, car }) => ({ id, name, car })) };
  }

  const retired = new Set();
  function retire(id) {
    if (!id) return;
    retired.add(id);
    if (retired.size > 64) retired.delete(retired.values().next().value);
  }
  const allFinished = () => !!race && race.roster.every(p => p.id === me ? finishSent === race.race : standings.get(p.id)?.done);
  function onlineChanged(state) {
    if (typeof state?.open !== "boolean") return;
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
      invites.delete(id);
      retire(id);
      if (pendingJoin === id) pendingJoin = 0;
    }
    if (lobby) {
      const before = lobby.roster.length;
      lobby.roster = lobby.roster.filter(p => present.has(p.id));
      if (lobby.roster.length !== before) send("lobby", lobbyFields(lobby));
    }
    if (race && (!race.went || race.world) && !race.cancelled && !allFinished()) {
      if (!present.has(race.host)) cancelLoading();
      else if (race.roster.some(p => !present.has(p.id))) {
        if (race.host === me) send("cancel", {race:race.race});
        cancelLoading();
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
      const old = invites.get(m.race);
      if (m.rv !== 3 || !game()?.driftReady || m.world === true && !worldType(m.type | 0)) return;
      if (needsNativeMatch(m.type | 0)) { if (!race && !lobby) nativeModeNotice(m.type | 0); return; }
      invites.set(m.race, { from, name: m.name, race: m.race, key: m.key, type: m.type | 0, laps: m.laps | 0,
                            ai: !isDrift(m.type | 0) && m.world !== true && m.ai === true, world: m.world === true, roster: r, at: performance.now(),
                            joined: !!old?.joined || (!!me && r.some((x) => x.id === me)) });
      draw();
    } else if (m.k === "join" || m.k === "leave") {
      if (m.k === "join" && pendingJoin === m.race && !me) me = from;
      if (!lobby || lobby.race !== m.race) {
        if (m.k === "leave" && race?.world && race.race === m.race && race.host === me && !race.cancelled && !allFinished()) {
          if (race.roster.some(p => p.id === from)) {
            send("cancel", {race:race.race});cancelLoading();
          }
        } else if (m.k === "join") draw();
        return;
      }
      if (m.k === "join" && (m.rv !== 3 || lobby.world && m.world !== true)) return;
      lobby.roster = lobby.roster.filter((r) => r.id !== from);
      if (m.k === "join" && lobby.roster.length <= MAX_PEERS)
        lobby.roster.push({ id: from, name: m.name || "Player", car: uint(m.car) ? m.car : 0 });
      send("lobby", lobbyFields(lobby));
      draw();
    } else if (m.k === "cancel") {
      const inv = invites.get(m.race);
      if (inv && inv.from === from) { invites.delete(m.race); retire(m.race); }
      if (race && race.race === m.race && race.host === from && (!race.went || race.world)) cancelLoading();
      draw();
    } else if (m.k === "start") {
      if(m.rv !== 3 || !game()?.driftReady || m.world === true && !worldType(m.type | 0)) return;
      if (m.world === true) {
        const inv = invites.get(m.race), own = lobby?.race === m.race && from === me;
        if (!own && !(inv?.from === from && inv.joined)) return;
        const g = game();
        if (!g?.worldReady || g.flow !== 6 || !g.roam) {
          send("leave", {race:m.race}); invites.delete(m.race); return;
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
               ready: new Set(), startedAt: performance.now(), went: false, launched: false };
      standings = new Map(r.map((x) => [x.id, { name: x.name, score: 0, place: 0, lap: 0, cp: 0, pct: 0, lapPct: 0, ms: 0, rate: 0,
                                                 lapRate: 0, t: 0, done: false, time: 0, at: 0 }]));
      finishSent = 0;
      pendingJoin = 0;
      tellGame();
      draw();
    } else if (m.k === "ready") {
      if (race && !race.cancelled && race.race === m.race && race.roster.some((r) => r.id === from)) {
        race.ready.add(from);
        maybeGo();
        draw();
      }
    } else if (m.k === "go") {
      if (race && !race.cancelled && race.race === m.race && from === race.host && !race.went) { race.went = true; tellGame(); draw(); }
    } else if (m.k === "progress" || m.k === "finish") {
      if (!race || race.cancelled || !race.went || race.race !== m.race || !standings.has(from)) return;
      const s = standings.get(from);
      if (isDrift(race.type) && (!Number.isInteger(m.score) || m.score < 0 || m.score > 0x7fffffff)) return;
      if (s.done) return;
      if (isDrift(race.type)) {s.score = m.score;s.dnf = m.dnf === true;}
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
      } else if (Number.isFinite(m.time) && m.time > 0 && m.time < 36000 && !s.done) {
        s.done = true;
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
    if (race.roster.some((r) => !race.ready.has(r.id))) return;
    race.wentSent = true;
    send("go", { race: race.race });
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
    send(race.host === me ? "cancel" : "leave", {race:race.race});
    cancelLoading();
  }
  function startLobby() {
    if (!lobby) return;
    const l = lobby;
    send("start", { ...lobbyFields(l), roster: l.roster.map((r, grid) => ({ id: r.id, name: r.name, car: r.car, grid })) });
  }
  function canJoin(inv, g = game()) {
    return !!inv && !!g?.driftReady && !nativeEncounterOwnsPage && !race && !lobby && uint(g?.car) && (inv.world
      ? g.worldReady && g.flow === 6 && g.roam : g.flow === 3);
  }
  function join(inv) {
    const g = game(), car = g?.car;
    if (!canJoin(inv, g)) return;
    pendingJoin = inv.race;
    send("join", { rv: 3, race: inv.race, car, world: inv.world === true });
    inv.joined = true;
    draw();
  }
  function leave(inv) {
    send("leave", { race: inv.race });
    inv.joined = false;
    pendingJoin = 0;
    draw();
  }

  // ---- Every 100 ms: the game's news ----
  function tick() {
    const g = game();
    const now = performance.now();
    for (const [k, inv] of invites) if (now - inv.at > FORGET) invites.delete(k);
    // A stalled native writer must not disable the page's loading timeout.
    // Give the host time to announce its decision before a disconnected guest
    // falls back to local cancellation (still before the native 120 s release).
    const loadLimit = race?.host === me ? LOAD_TIMEOUT : LOAD_TIMEOUT + 30000;
    if (race && !race.went && !race.cancelled && now - race.startedAt >= loadLimit) {
      if (race.host === me) send("cancel", { race: race.race });
      cancelLoading();
    }
    if (!g) return;
    // The host pressed Start in the Quick Race menu: the lobby opens.
    if (lastCaught < 0) lastCaught = g.caught;
    if (g.caught !== lastCaught) {
      lastCaught = g.caught;
      if (hosting && !lobby && !race && g.caughtKey) {
        if (worldHosting && !worldType(g.caughtType)) {
          hosting = false; worldHosting = false; tellGame(); modeNotice = "Choose Sprint, Circuit, Speedtrap, Drift, or Canyon Drift."; draw(); return;
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
        race.readySent = true;
        send("ready", { race: race.race });
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
        send("finish", { race: race.race, score: g.score, dnf: g.dnf, time: Math.round(g.time * 1000) / 1000 });
      }
      // Back in the menus after the race: it is over.
      if (race.launched && g.phase === PHASE.idle && lastPhase !== PHASE.idle) {
        // An unexpected native exit must not leave the other games waiting.
        const complete = allFinished();
        if (race.world && !race.cancelled && !complete)
          send(race.host === me ? "cancel" : "leave", {race:race.race});
        race.over = now;
      }
      if (race.over) { race = null; tellGame(); }
      checkList(g);
    }
    const cardState = `${g.flow}:${g.car}:${g.waitingResult}`;
    if (cardState !== lastCardState) { lastCardState = cardState; draw(); }
    lastPhase = g.phase;
    syncLocalStanding(g);
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
  .crc { --ink:#ecf1e9; --dim:#a1b6b1; --cyan:#78d5cc; --select:#e4ef43; --line:rgba(112,209,196,.36);
    --u:clamp(14px,2.3vh,18px); font:500 var(--u)/1.35 "Arial Narrow","Avenir Next Condensed","Segoe UI",sans-serif; color:var(--ink); }
  .crc * { box-sizing:border-box; }
  .crc-card { position:fixed; right:max(20px,env(safe-area-inset-right)); top:max(64px,calc(env(safe-area-inset-top) + 48px)); z-index:8;
    width:min(420px,calc(100vw - 40px)); max-height:calc(100dvh - 100px); overflow:auto; scrollbar-width:thin; scrollbar-color:#41685d transparent;
    padding:18px; border:1px solid var(--line); border-radius:5px;
    background:linear-gradient(120deg,rgba(25,54,51,.45),transparent 65%),rgba(3,10,11,.94);
    box-shadow:0 0 0 3px rgba(16,51,48,.28),0 18px 44px #0006,inset 0 0 28px #315e5215; }
  .crc-card::before { content:"ONLINE / RACE TOGETHER"; display:block; margin:-18px -18px 14px; padding:9px 16px;
    color:var(--cyan); background:linear-gradient(90deg,#244c46b3,#0c1819cc); border-bottom:2px dotted #7fdcd18c;
    font-size:11px; font-weight:800; letter-spacing:.2em; }
  .crc-card h2 { display:flex; align-items:center; gap:10px; margin:0 0 14px; color:#f2f4e9;
    font:italic 800 clamp(22px,3.2vh,28px)/1.1 "Arial Narrow","Avenir Next Condensed","Bahnschrift",sans-serif;
    letter-spacing:.04em; text-transform:uppercase; }
  .crc-card h2::before { content:""; width:22px; height:22px; flex:none; border-left:2px solid var(--ink);
    background:conic-gradient(#e9f3ed 25%,transparent 0 50%,#e9f3ed 0 75%,transparent 0) 4px 1px/8px 8px;
    clip-path:polygon(0 0,100% 0,100% 75%,12% 75%,12% 100%,0 100%); transform:skewY(-7deg); }
  .crc-card p { margin:0 0 13px; color:#d1ddd5; }
  .crc-card b { color:var(--select); font-weight:650; }
  .crc-card.crc-lobby { width:min(680px,calc(100vw - 40px)); }
  .crc-lobby-body { display:grid; grid-template-columns:minmax(0,1.15fr) minmax(0,1fr); gap:20px; }
  .crc-lobby-body .crc-who { margin-bottom:0; }
  .crc-summary { display:grid; grid-template-columns:1fr auto; gap:5px 16px; margin:0 0 14px; padding:10px 12px;
    background:#28483d3b; border-top:1px solid #83b9a139; border-bottom:1px solid #83b9a139; }
  .crc-summary .label { color:var(--dim); font-size:.82em; }
  .crc-summary .value { text-align:right; color:#f4f4e9; font-weight:650; }
  .crc-who { margin:0 0 14px; display:grid; gap:3px; }
  .crc-player { display:grid; grid-template-columns:24px minmax(0,1fr) auto; align-items:center; gap:8px;
    min-height:36px; padding:6px 10px; background:linear-gradient(90deg,#264e455e,#0d1d1a4d); border-left:2px solid #5a928544; }
  .crc-player.me { border-left-color:var(--select); background:linear-gradient(90deg,#889b2038,#3c48140c); }
  .crc-player .slot { font-size:.74em; color:#78968b; font-variant-numeric:tabular-nums; }
  .crc-player .player-name { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; color:#e1ece3; }
  .crc-player.me .player-name { color:var(--select); }
  .crc-player .player-state { display:flex; align-items:center; gap:6px; font-size:10px; font-weight:750; letter-spacing:.09em; color:#91a69c; }
  .crc-player .player-state::before { content:""; width:5px; height:5px; background:#84988c; }
  .crc-player.ready .player-state { color:var(--cyan); }
  .crc-player.ready .player-state::before { background:var(--cyan); box-shadow:0 0 6px #78d5cc55; }
  .crc-row { display:flex; gap:8px; margin-top:14px; flex-wrap:wrap; }
  .crc-btn { flex:1; min-height:42px; padding:10px 14px; border:1px solid #6d9d8a66; border-radius:2px;
    font:700 .87em/1.15 "Arial Narrow","Avenir Next Condensed",sans-serif; letter-spacing:.04em; text-transform:uppercase;
    color:#d5e5da; background:linear-gradient(#294c405c,#172a235c); cursor:pointer;
    transition:background-color 120ms ease-out,border-color 120ms ease-out,color 120ms ease-out; }
  .crc-btn:hover:not(:disabled),.crc-btn:focus-visible { color:#f4f8d7; border-color:var(--select); background-color:#8b9c2138; outline:2px solid #e4ef4366; outline-offset:2px; }
  .crc-btn:active:not(:disabled) { background-color:#dbe95455; }
  .crc-btn.go { color:#ecf56b; border-color:#c8d33f99; background:linear-gradient(90deg,#839a2852,#435d302b); }
  .crc-btn.go::before { content:"›"; margin-right:7px; font-size:1.2em; }
  .crc-btn:disabled { color:#829186; border-color:#56726544; background:#18221c; cursor:not-allowed; }
  .crc-check { display:flex; align-items:center; gap:9px; margin:10px 0 0; font-size:.85em; color:#b6c9ba; cursor:pointer; }
  .crc-check input { width:16px; height:16px; accent-color:var(--select); }
  .crc-note { margin-top:12px; padding-top:11px; border-top:1px solid #72a98a33; color:#9eb6a7; font-size:.78em; line-height:1.45; }
  .crc [hidden] { display:none !important; }
  @media(max-width:600px) { .crc-card.crc-lobby { width:calc(100vw - 24px); } .crc-lobby-body { display:flex; flex-direction:column; gap:14px; } .crc-settings { order:-1; } .crc-card { right:max(12px,env(safe-area-inset-right)); width:min(380px,calc(100vw - 24px)); padding:14px; } .crc-card::before { margin:-14px -14px 12px; } }
  @media(max-height:500px) { .crc-card>.crc-note { position:sticky; bottom:-18px; background:#07120ff5; padding-bottom:10px; } .crc-who { max-height:max(100px,calc(100dvh - 260px)); overflow:auto; scrollbar-width:thin; scrollbar-color:#41685d transparent; } .crc-lobby>.crc-note { display:none; } .crc-card { top:max(46px,env(safe-area-inset-top)); max-height:calc(100dvh - 58px); width:min(380px,calc(100vw - 24px)); } .crc-player { min-height:29px; padding-block:4px; } }
  @media(prefers-reduced-motion:reduce) { .crc-btn { transition:none; } }
  `;
  let root, card, lastCardState = "", cardViewKey = "";
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
    list.forEach((r, i) => {
      const row = el("div", `crc-player${r.id === me ? " me" : ""}${ready?.has(r.id) ? " ready" : ""}`);
      const state = ready ? `${r.id === hostId ? "HOST · " : ""}${ready.has(r.id) ? "READY" : "LOADING"}` : r.id === hostId ? "HOST" : r.id === me ? "YOU" : "JOINED";
      row.setAttribute("aria-label", `${r.name}, ${r.id === me ? "you, " : ""}${state}`);
      row.append(el("span", "slot", String(i + 1).padStart(2, "0")),
        el("span", "player-name", r.name), el("span", "player-state", state));
      w.append(row);
    });
    return w;
  }
  function summary(l) {
    const w = el("div", "crc-summary");
    w.append(el("span", "label", "Game mode"), el("span", "value", what(l)),
      el("span", "label", "Players"), el("span", "value", String(l.roster.length)));
    return w;
  }
  // The card: hosting hint, the host's lobby, an invite, or the race starting.
  function draw() {
    if (!root) return;
    dispatchEvent(new Event("recomp-race-invitations"));
    const inv = [...invites.values()].sort((a, b) => b.at - a.at)[0];
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
      invite: inv && { id: inv.race, name: inv.name, type: inv.type, laps: inv.laps, joined: inv.joined, roster: inv.roster },
      game: g && { phase: g.phase, flow: g.flow, car: g.car, waitingResult: g.waitingResult } });
    if (viewKey === cardViewKey) return;
    cardViewKey = viewKey;
    card.className = lobby && !race ? "crc-card crc-lobby" : "crc-card";
    card.replaceChildren();
    if (modeNotice && !race && !lobby) {
      card.hidden = false;
      card.append(el("h2", null, "Race together"), el("p", null, modeNotice),
        button("OK", () => { modeNotice = ""; draw(); }));
      return;
    }
    if (race?.cancelled) {
      card.append(el("h2", null, "Race cancelled"));
      card.append(el("p", null, race.world ? "No shared result will be recorded." : "Not everyone was ready. No shared result will be recorded."));
      card.append(el("div", "crc-note", race.world ? "Returning to free roam…" : "Exit to the start page, then press Play to return to the game."));
      // Loading may never reach a native menu that can accept Esc. Use the
      // same exit path as F9 Quit, without depending on the game's readiness.
      card.append(button("Exit to start page", () => {
        dispatchEvent(new Event("recomp-quit"));
        location.replace(location.pathname);
      }, true));
      // Hiding the panel must not clear cancellation while a queued launch or
      // loading hold still owns the native session (including an unreadable block).
      if ((g?.flow === 3 || race?.world && g?.roam) && g.phase === PHASE.idle)
        card.append(button("Dismiss", () => { race = null; tellGame(); draw(); }));
    } else if (race && g?.waitingResult && g.session === race.race) {
      card.append(el("h2", null, "Waiting for race results"));
      card.append(el("p", null, isDrift(race.type) ? "Your drift score is recorded. Waiting for the other players to finish." : "Your finish time is recorded. Waiting for the other players to finish."));
      card.append(el("div", "crc-note", "If a player cannot finish, use F9 → Quit to the start page. No shared winner is confirmed until all results arrive."));
    } else if (race && !race.went) {
      const host = race.roster.find((r) => r.id === race.host);
      card.append(el("h2", null, "Race together"));
      card.append(summary(race), names(race.roster, race.ready, race.host));
      const loading = !race.launched;
      card.append(el("div", "crc-note", loading && g && g.phase === PHASE.idle
        ? race.world ? "Starting from free roam…" : "Starting… (the race starts from the game's menus: leave free roam for the main menu)"
        : `Loading · ${race.ready.size} of ${race.roster.length} ready${host && host.id !== me ? ` · ${host.name} starts it` : ""}`));
    } else if (lobby) {
      card.append(el("h2", null, "Race together"));
      const body = el("div", "crc-lobby-body"), settings = el("div", "crc-settings");
      settings.append(summary(lobby));
      if (!lobby.world && !isDrift(lobby.type)) {
      const ai = el("label", "crc-check"), box = el("input");
      box.type = "checkbox";
      box.checked = lobby.ai;
      box.addEventListener("change", () => { lobby.ai = box.checked; send("lobby", lobbyFields(lobby)); });
      ai.append(box, "AI opponents too");
      settings.append(ai);
      }
      body.append(names(lobby.roster), settings); card.append(body);
      const row = el("div", "crc-row");
      row.append(button(lobby.roster.length > 1 ? `Start · ${lobby.roster.length} players` : "Start alone", startLobby, true),
                 button("Cancel", cancelLobby));
      card.append(row, el("div", "crc-note", lobby.world ? "Practice race · no career cash or race completion." : "Friends in this room can join now, from the game's menus."));
    } else if (inv && !race) {
      const joined = !!inv.joined;
      card.append(el("h2", null, "Race together"));
      card.append(el("p", null, `${inv.name || "A player"} invites you to race.`), summary(inv), names(inv.roster));
      const row = el("div", "crc-row");
      const joinButton = joined ? button("Leave", () => leave(inv)) : button(canJoin(inv, g) ? "Join" : inv.world ? "Enter free roam" : "Return to main menu", () => join(inv), true);
      joinButton.disabled = !joined && !canJoin(inv, g);
      row.append(joinButton,
                 button("Not now", () => { invites.delete(inv.race); if (inv.joined) leave(inv); draw(); }));
      card.append(row, el("div", "crc-note", joined ? `Joined · waiting for ${inv.name || "the host"} to start`
        : inv.world ? "Join from career free roam; F9 lists it too." : "Join from the game's menus; F9 lists it too."));
    } else if (hosting) {
      card.append(el("h2", null, "Race together"));
      card.append(el("p", null, worldHosting ? "Choose an event on the world map, then select Jump to Event to open your lobby." : "Pick a Quick Race and press Start: friends in this room can join before it begins."));
      const row = el("div", "crc-row");
      row.append(button("Stop hosting", () => host(false)));
      card.append(row);
    }
    card.hidden = !card.childElementCount;
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

  // ---- The F9 menu ----
  // menu.js lists window.recompMenuActions as buttons ({id, group, label, hint, run, shown, keepOpen}).
  function menu() {
    const actions = (window.recompMenuActions ||= []);
    actions.push(
      { id: "race-host", group: "Race together", label: "Host a race together",
        hint: () => (hosting ? worldHosting ? "On · choose an event on the world map" : "On · pick a Quick Race, press Start" : game()?.worldReady && game()?.roam ? "Choose on world map" : "Off"), shown: () => !lobby && !race, run: () => host(!hosting) },
      { id: "race-start", group: "Race together", label: "Start the race", shown: () => !!lobby,
        hint: () => (lobby ? `${lobby.roster.length} ${lobby.roster.length === 1 ? "player" : "players"}` : ""), run: startLobby },
      { id: "race-leave-active", group: "Race together", label: () => race?.host === me ? "Cancel race" : "Leave race",
        shown: () => !!race?.world && !race.cancelled, run: leaveRace },
      { id: "race-cancel", group: "Race together", label: "Cancel the race", shown: () => !!lobby, run: cancelLobby },
      { id: "race-join", group: "Race together", shown: () => !lobby && !race && invites.size > 0,
        label: () => { const i = [...invites.values()].pop(); return i ? `${i.joined ? "Leave" : "Join"} ${i.name || "the"}'s race` : "Join"; },
        hint: () => { const i = [...invites.values()].pop(); return i ? what(i) : ""; },
        run: () => { const i = [...invites.values()].pop(); if (i) (i.joined ? leave(i) : join(i)); } },
    );
    dispatchEvent(new Event("recomp-menu-toggles"));
  }

  function wake() {
    if (active) return;
    active = true;
    root = el("div", "crc");
    root.innerHTML = `<style>${css}</style>`;
    card = el("div", "crc-card");
    card.setAttribute("role", "dialog");
    card.setAttribute("aria-label", "Race together");
    card.hidden = true;
    root.append(card);
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
  window.recompRace = { state: () => ({ me, hosting, lobby, invites: [...invites.values()], race, standings: [...standings], game: game() }),
                        host, startLobby, cancelLobby, leaveRace, canJoin,
                        join: (raceId) => { const i = raceId == null ? [...invites.values()].pop() : invites.get(raceId); if (i) join(i); },
                        leave: (raceId) => { const i = invites.get(raceId); if (i?.joined) leave(i); } };
})();
