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
  const CATCH = 1, KEEP_AI = 2;
  const PHASE = { idle: 0, launched: 1, setUp: 2, holding: 3, racing: 4, done: 5 };
  const TYPES = ["Sprint", "Circuit", "Drag", "Lap knockout", "Tollbooth", "Speedtrap", "Checkpoint", "Cash grab",
    "Challenge", "Speedtrap jump", "Milestone jump", "Drift", "Canyon drift", "Canyon duel", "Pursuit tag",
    "Pursuit knockout", "Encounter"];
  const LOBBY_EVERY = 4000;  // the host repeats the lobby, so players who arrive later see it
  const PROGRESS_EVERY = 250;
  const GO_AFTER = 60000;    // the host lets everyone go even if someone never loads
  const FORGET = 15000;      // a lobby nobody repeats is gone

  let active = false, me = 0;
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
      phase: u32[at(0x264)], car: u32[at(0x268)], caught: u32[at(0x26c)], caughtKey: u32[at(0x270)],
      caughtType: u32[at(0x274)], caughtOpts: Array.from(u8.subarray(p + 0x278, p + 0x2a0)),
      place: u32[at(0x2a0)], laps: u32[at(0x2a4)], done: u32[at(0x2a8)], pct: f32[at(0x2ac)], time: f32[at(0x2b0)],
      loaded: u32[at(0x2b8)], session: u32[at(0x2bc)], lapPct: f32[at(0x2c0)], cp: u32[at(0x2c4)], ms: u32[at(0x2c8)],
      rows: [],
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
  function tellGame() {
    page((w) => {
      const others = race ? race.roster.filter((r) => r.id !== me) : [];
      const mine = race?.roster.find((r) => r.id === me);
      w.u32(0x00, race ? race.race : 0);
      w.u32(0x04, (hosting && !race && !lobby ? CATCH : 0) | (race?.ai ? KEEP_AI : 0));
      w.u32(0x08, me);
      w.u32(0x0c, mine ? mine.grid : 0);
      w.u32(0x10, others.length);
      w.u32(0x14, race?.went ? race.race : 0);
      w.u32(0x18, race ? race.race : 0);
      w.u32(0x1c, race ? race.key : 0);
      w.bytes(0x20, race ? race.opts : [], 40);
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
        w.u32(o + 32, s ? s.cp : 0);
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
    return { race: l.race, key: l.key, type: l.type, opts: hex(l.opts), laps: l.laps, ai: l.ai,
             roster: l.roster.map(({ id, name, car }) => ({ id, name, car })) };
  }

  function heard(m) {
    if (!m || m.t !== "race" || !uint(m.id) || !uint(m.race)) return;
    const from = m.id, self = from === me || (!me && m.self);
    if (m.k === "lobby") {
      if (lobby && lobby.race === m.race && !me) me = from; // our own lobby came back: that is us
      const r = roster(m.roster), opts = unhex(m.opts);
      if (!r || !opts || !uint(m.key)) return;
      if (lobby && lobby.race === m.race) { draw(); return; }
      const old = invites.get(m.race);
      invites.set(m.race, { from, name: m.name, race: m.race, key: m.key, type: m.type | 0, laps: m.laps | 0,
                            ai: m.ai === true, roster: r, at: performance.now(),
                            joined: !!old?.joined || (!!me && r.some((x) => x.id === me)) });
      draw();
    } else if (m.k === "join" || m.k === "leave") {
      if (m.k === "join" && pendingJoin === m.race && !me) me = from;
      if (!lobby || lobby.race !== m.race) { if (m.k === "join") draw(); return; }
      lobby.roster = lobby.roster.filter((r) => r.id !== from);
      if (m.k === "join" && lobby.roster.length <= MAX_PEERS)
        lobby.roster.push({ id: from, name: m.name || "Player", car: uint(m.car) ? m.car : 0 });
      send("lobby", lobbyFields(lobby));
      draw();
    } else if (m.k === "cancel") {
      invites.delete(m.race);
      if (race && race.race === m.race && race.host === from && !race.launched) { race = null; tellGame(); }
      draw();
    } else if (m.k === "start") {
      invites.delete(m.race);
      const r = roster(m.roster), opts = unhex(m.opts);
      if (!r || !opts || !uint(m.key)) return;
      if (lobby && lobby.race === m.race) { if (!me) me = from; lobby = null; hosting = false; }
      if (!r.some((x) => x.id === me)) { draw(); return; } // a race without us
      race = { race: m.race, host: from, key: m.key, type: m.type | 0, laps: m.laps | 0, opts, ai: m.ai === true, roster: r,
               ready: new Set(), startedAt: performance.now(), went: false, launched: false };
      standings = new Map(r.map((x) => [x.id, { name: x.name, place: 0, lap: 0, cp: 0, pct: 0, lapPct: 0, ms: 0, rate: 0,
                                                 lapRate: 0, t: 0, done: false, time: 0, at: 0 }]));
      finishSent = 0;
      pendingJoin = 0;
      tellGame();
      draw();
    } else if (m.k === "ready") {
      if (race && race.race === m.race) {
        race.ready.add(from);
        maybeGo();
        draw();
      }
    } else if (m.k === "go") {
      if (race && race.race === m.race && from === race.host && !race.went) { race.went = true; tellGame(); draw(); }
    } else if (m.k === "progress" || m.k === "finish") {
      if (!race || race.race !== m.race || !standings.has(from)) return;
      const s = standings.get(from);
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
      drawBoard();
    }
  }
  let pendingJoin = 0;

  // The host: everyone is loaded (or the wait is over): go.
  function maybeGo() {
    if (!race || race.host !== me || race.wentSent) return;
    const here = new Set([me, ...(window.recompOnlineState?.people || []).map((p) => p.id)]);
    const waiting = race.roster.filter((r) => here.has(r.id) && !race.ready.has(r.id));
    if (waiting.length && performance.now() - race.startedAt < GO_AFTER) return;
    race.wentSent = true;
    send("go", { race: race.race });
  }

  // ---- Actions ----
  function host(on) {
    hosting = on;
    if (!on && lobby) cancelLobby();
    tellGame();
    draw();
  }
  function cancelLobby() {
    if (!lobby) return;
    send("cancel", { race: lobby.race });
    lobby = null;
    tellGame();
    draw();
  }
  function startLobby() {
    if (!lobby) return;
    const l = lobby;
    send("start", { ...lobbyFields(l), roster: l.roster.map((r, grid) => ({ id: r.id, name: r.name, car: r.car, grid })) });
  }
  function join(inv) {
    pendingJoin = inv.race;
    send("join", { race: inv.race, car: game()?.car || 0 });
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
    if (!g) return;
    // The host pressed Start in the Quick Race menu: the lobby opens.
    if (lastCaught < 0) lastCaught = g.caught;
    if (g.caught !== lastCaught) {
      lastCaught = g.caught;
      if (hosting && !lobby && !race && g.caughtKey) {
        lobby = { race: ((Math.random() * 0x7ffffffe) | 0) + 1, key: g.caughtKey, type: g.caughtType, opts: g.caughtOpts,
                  laps: g.caughtOpts[4], ai: false, roster: [{ id: me || 0, name: myName(), car: g.car }], at: now };
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
      if (g.phase >= PHASE.holding && !race.readySent && g.session === race.race) {
        race.readySent = true;
        send("ready", { race: race.race });
      }
      if (race.host === me) maybeGo();
      if (g.phase >= PHASE.racing && now - lastProgress > PROGRESS_EVERY) {
        lastProgress = now;
        send("progress", { race: race.race, place: g.place, lap: g.laps, cp: g.cp, pct: Math.round(g.pct * 1000) / 1000,
                           lp: Math.round(g.lapPct * 1000) / 1000, rt: Math.round(g.time * 1000) / 1000, ms: g.ms });
      }
      if (g.done && g.time > 0 && finishSent !== race.race) {
        finishSent = race.race;
        send("finish", { race: race.race, time: Math.round(g.time * 1000) / 1000 });
      }
      // Back in the menus after the race: it is over.
      if (race.launched && g.phase === PHASE.idle && lastPhase !== PHASE.idle) {
        race.over = now;
      }
      if (race.over && now - race.over > 20000) { race = null; tellGame(); }
      checkList(g);
    }
    lastPhase = g.phase;
    drawBoard(g);
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
  .crc { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --line: rgba(95, 228, 242, .22);
         --u: clamp(12px, 1.8vh, 17px); font: 600 var(--u)/1.3 "Avenir Next", "Segoe UI", system-ui, sans-serif; color: var(--ink); }
  .crc * { box-sizing: border-box; }
  .crc-card { position: fixed; right: 16px; top: 56px; z-index: 8; width: min(calc(var(--u) * 22), calc(100vw - 32px));
         background: rgba(6, 12, 22, .9); border: 1px solid var(--line); padding: calc(var(--u) * .9);
         clip-path: polygon(0 0, calc(100% - 14px) 0, 100% 14px, 100% 100%, 14px 100%, 0 calc(100% - 14px)); animation: crc-in .16s ease-out; }
  .crc-card h2 { margin: 0 0 calc(var(--u) * .6); font: italic 800 .95em/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
         letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); display: flex; align-items: center; gap: 10px; white-space: nowrap; }
  .crc-card h2::after { content: ""; flex: 1; height: 2px; transform: skewX(-30deg); background: linear-gradient(90deg, var(--cyan), transparent); opacity: .5; }
  .crc-card p { margin: 0 0 calc(var(--u) * .6); }
  .crc-card b { color: var(--cyan); font-weight: 700; }
  .crc-who { margin: 0 0 calc(var(--u) * .6); font-size: .86em; color: var(--dim); }
  .crc-who span { color: var(--ink); }
  .crc-row { display: flex; gap: 8px; flex-wrap: wrap; }
  .crc-btn { flex: 1; font: italic 700 1em/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .1em;
         text-transform: uppercase; color: var(--ink); background: rgba(95, 228, 242, .06); border: 1px solid var(--line);
         padding: calc(var(--u) * .55) calc(var(--u) * .8); cursor: pointer; transform: skewX(-10deg); white-space: nowrap; }
  .crc-btn > span { display: inline-block; transform: skewX(10deg); }
  .crc-btn:hover, .crc-btn:focus-visible { outline: none; background: rgba(95, 228, 242, .16); border-color: var(--cyan); }
  .crc-btn.go { color: #021018; border-color: transparent; background: linear-gradient(100deg, #9ff3fb, var(--cyan) 40%, #2aa4dc); }
  .crc-check { display: flex; align-items: center; gap: 8px; margin: 0 0 calc(var(--u) * .6); font-size: .86em; color: var(--dim); cursor: pointer; }
  .crc-check input { accent-color: var(--cyan); }
  .crc-note { margin-top: calc(var(--u) * .5); font-size: .78em; font-weight: 500; color: var(--dim); }
  .crc-board { position: fixed; left: 16px; top: 52px; z-index: 5; min-width: calc(var(--u) * 13); pointer-events: none;
         background: linear-gradient(90deg, rgba(6, 12, 22, .78), rgba(6, 12, 22, .45)); border-left: 2px solid var(--cyan);
         padding: calc(var(--u) * .45) calc(var(--u) * .7); transform: skewX(-8deg); }
  .crc-board > * { transform: skewX(8deg); }
  .crc-board h3 { margin: 0 0 4px; font: italic 800 .78em/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
         letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); }
  .crc-line { display: grid; grid-template-columns: 1.6em 1fr auto auto; gap: 0 .7em; align-items: baseline; font-size: .9em; white-space: nowrap; }
  .crc-line.me { color: var(--cyan); }
  .crc-line .pos { font: italic 800 1.05em/1.2 "Avenir Next Condensed", "DIN Condensed", sans-serif; }
  .crc-line .lap, .crc-line .gap { font: 500 .85em/1.2 system-ui, sans-serif; color: var(--dim); text-align: right; font-variant-numeric: tabular-nums; }
  .crc-line.me .lap, .crc-line.me .gap { color: var(--cyan); }
  .crc [hidden] { display: none !important; }
  @keyframes crc-in { from { opacity: 0; translate: 8px 0; } }
  @media (prefers-reduced-motion: reduce) { .crc-card { animation: none; } }
  `;
  let root, card, board;
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
  function names(list) {
    const w = el("div", "crc-who");
    list.forEach((r, i) => { if (i) w.append(", "); w.append(el("span", null, r.id === me ? `${r.name} (you)` : r.name)); });
    return w;
  }
  // The card: hosting hint, the host's lobby, an invite, or the race starting.
  function draw() {
    if (!root) return;
    card.replaceChildren();
    const inv = [...invites.values()].sort((a, b) => b.at - a.at)[0];
    const g = game();
    if (race && !race.went) {
      const host = race.roster.find((r) => r.id === race.host);
      card.append(el("h2", null, "Race together"));
      const p = el("p");
      p.append(el("b", null, what(race)), ` with ${race.roster.length - 1} ${race.roster.length === 2 ? "friend" : "friends"}`);
      card.append(p, names(race.roster));
      const loading = !race.launched;
      card.append(el("div", "crc-note", loading && g && g.phase === PHASE.idle
        ? "Starting… (the race starts from the game's menus: leave free roam for the main menu)"
        : `Loading · ${race.ready.size} of ${race.roster.length} ready${host && host.id !== me ? ` · ${host.name} starts it` : ""}`));
    } else if (lobby) {
      card.append(el("h2", null, "Race together"));
      const p = el("p");
      p.append("Your race: ", el("b", null, what(lobby)));
      card.append(p, names(lobby.roster));
      const ai = el("label", "crc-check"), box = el("input");
      box.type = "checkbox";
      box.checked = lobby.ai;
      box.addEventListener("change", () => { lobby.ai = box.checked; send("lobby", lobbyFields(lobby)); });
      ai.append(box, "AI opponents too");
      card.append(ai);
      const row = el("div", "crc-row");
      row.append(button(lobby.roster.length > 1 ? `Start · ${lobby.roster.length} players` : "Start alone", startLobby, true),
                 button("Cancel", cancelLobby));
      card.append(row, el("div", "crc-note", "Friends in this room can join now, from the game's menus."));
    } else if (inv) {
      const joined = !!inv.joined;
      card.append(el("h2", null, "Race together"));
      const p = el("p");
      p.append(el("b", null, inv.name || "A player"), " started a race: ", el("b", null, what(inv)), inv.ai ? " (with AI)" : "");
      card.append(p, names(inv.roster));
      const row = el("div", "crc-row");
      row.append(joined ? button("Leave", () => leave(inv)) : button("Join", () => join(inv), true),
                 button("Not now", () => { invites.delete(inv.race); if (inv.joined) leave(inv); draw(); }));
      card.append(row, el("div", "crc-note", joined ? `Joined · waiting for ${inv.name || "the host"} to start`
        : "Join from the game's menus; F9 lists it too."));
    } else if (hosting) {
      card.append(el("h2", null, "Race together"));
      card.append(el("p", null, "Pick a Quick Race and press Start: friends in this room can join before it begins."));
      const row = el("div", "crc-row");
      row.append(button("Stop hosting", () => host(false)));
      card.append(row);
    }
    card.hidden = !card.childElementCount;
  }
  function fmt(t) {
    const m = Math.floor(t / 60), s = t - m * 60;
    return `${m}:${s.toFixed(2).padStart(5, "0")}`;
  }
  // The shared race list: every player's place, lap and gap, from what each sends.
  let boardKey = "";
  function drawBoard(g = game()) {
    if (!root) return;
    const show = race && race.went && g && g.phase >= PHASE.racing - 1;
    if (!show) { board.hidden = true; boardKey = ""; return; }
    const list = [...standings].map(([id, s]) => ({ id, ...s }));
    const mine = standings.get(me);
    if (mine && g) { mine.place = g.place; mine.lap = g.laps; mine.pct = g.pct; mine.t = g.time; if (g.done) { mine.done = true; mine.time = g.time; } }
    // Finished first, by time; then by progress.
    list.sort((a, b) => (b.done - a.done) || (a.done ? a.time - b.time : b.pct - a.pct));
    const lead = list[0];
    // The leader's average pace (% a second) turns a gap in progress into seconds,
    // once they have gone far enough for it to mean something.
    const rate = lead && !lead.done && lead.t > 5 && lead.pct >= 0.5 ? lead.pct / lead.t : 0;
    const laps = race.type === 1 || race.type === 3 ? race.laps : 0;
    const rows = list.map((s, i) => {
      let gap = "";
      if (s.done) gap = fmt(s.time);
      else if (i && lead.done) gap = "racing";
      else if (i && rate) gap = `+${Math.max(0, (lead.pct - s.pct) / rate).toFixed(1)} s`;
      const lap = laps ? `L${Math.min(laps, s.lap + 1)}/${laps}` : `${Math.floor(s.pct)}%`;
      return { me: s.id === me, pos: String(i + 1), name: s.name, lap: s.done ? "FIN" : lap, gap };
    });
    const key = JSON.stringify(rows);
    if (key === boardKey) return;
    boardKey = key;
    board.replaceChildren(el("h3", null, "Race together"), ...rows.map((r) => {
      const line = el("div", r.me ? "crc-line me" : "crc-line");
      line.append(el("span", "pos", r.pos), el("span", "name", r.name), el("span", "lap", r.lap), el("span", "gap", r.gap));
      return line;
    }));
    board.hidden = false;
  }

  // ---- The F9 menu ----
  // menu.js lists window.recompMenuActions as buttons ({id, group, label, hint, run, shown, keepOpen}).
  function menu() {
    const actions = (window.recompMenuActions ||= []);
    actions.push(
      { id: "race-host", group: "Race together", label: "Host a race together", keepOpen: true,
        hint: () => (hosting ? "On · pick a Quick Race, press Start" : "Off"), shown: () => !lobby && !race, run: () => host(!hosting) },
      { id: "race-start", group: "Race together", label: "Start the race", shown: () => !!lobby,
        hint: () => (lobby ? `${lobby.roster.length} ${lobby.roster.length === 1 ? "player" : "players"}` : ""), run: startLobby },
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
    board = el("div", "crc-board");
    board.setAttribute("aria-live", "polite");
    board.hidden = true;
    root.append(card, board);
    document.body.append(root);
    menu();
    addEventListener("recomp-online-message", (e) => heard(e.detail));
    addEventListener("carbon-race", () => tellGame());
    addEventListener("recomp-online", (e) => {
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
                        host, startLobby, cancelLobby, join: () => { const i = [...invites.values()].pop(); if (i) join(i); } };
})();
