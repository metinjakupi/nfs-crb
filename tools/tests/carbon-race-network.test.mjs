// race.js against a stub page and game block: what happens when the relay drops a race message or
// a player vanishes. Run: node --test tools/tests/carbon-race-network.test.mjs
// CARBON_RACE_SOURCE=<file> runs it on another copy of race.js.
import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import { readFile } from "node:fs/promises";

const source = await readFile(process.env.CARBON_RACE_SOURCE || new URL("../../source/web/race.js", import.meta.url), "utf8");
const PHASE = { idle: 0, holding: 3, racing: 4, done: 5 };

class El {
  constructor() { this.kids = []; this.className = ""; this.hidden = false; this.textContent = ""; this.style = {}; }
  append(...k) { this.kids.push(...k); }
  replaceChildren(...k) { this.kids = k; }
  get childElementCount() { return this.kids.length; }
  setAttribute() {}
  addEventListener(n, f) { this[`on${n}`] = f; }
  find(text) { for (const k of this.kids) { if (k.kids?.some((c) => c.textContent === text)) return k; const f = k.find?.(text); if (f) return f; } return null; }
}

const PEER0_DONE = 0x60 + 12; // the first other player's finish and left bits in the block

function game(me) {
  const listeners = new Map(), intervals = [], later = [], sent = [];
  const mem = new ArrayBuffer(8192), u32 = new Uint32Array(mem), f32 = new Float32Array(mem);
  u32[0] = 0x31435243; u32[1] = 3; u32[0x2cc >> 2] = 3; u32[0x268 >> 2] = 1234;
  const body = new El();
  const ctx = {
    console, Atomics, Uint8Array, Uint32Array, Int32Array, Float32Array, TextEncoder, Math, JSON, Map, Set, Array, String, Number, Object, Error,
    performance: { now: () => ctx.__now }, __now: 1000,
    document: { createElement: () => new El(), body },
    localStorage: { getItem: () => null, setItem() {} },
    addEventListener: (n, f) => { (listeners.get(n) || listeners.set(n, []).get(n)).push(f); },
    dispatchEvent: (e) => { for (const f of listeners.get(e.type) || []) f(e); },
    Event: class { constructor(t) { this.type = t; } },
    CustomEvent: class { constructor(t, o) { this.type = t; this.detail = o?.detail; } },
    setInterval: (f) => intervals.push(f),
    setTimeout: (f, ms = 0) => later.push({ f, at: ctx.__now + ms }),
    carbonRaceBlock: { memory: { buffer: mem }, ptr: 0 },
    recompOnlineState: { me, name: "Me" },
    recompOnlineSend: (m) => sent.push(m),
  };
  ctx.window = ctx; ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(source, ctx);
  const run = (ms = 0) => {
    ctx.__now += ms;
    for (const t of later.splice(0)) { if (t.at <= ctx.__now) t.f(); else later.push(t); }
    intervals[0]();
  };
  return {
    ctx, u32, f32, sent, run, body,
    say: (m) => ctx.dispatchEvent(new ctx.CustomEvent("recomp-online-message", { detail: { t: "race", ...m } })),
    people: (ids) => {
      ctx.recompOnlineState.open = true;
      ctx.recompOnlineState.people = ids.map((id) => ({ id, name: `p${id}` }));
      ctx.dispatchEvent(new ctx.CustomEvent("recomp-online", { detail: ctx.recompOnlineState }));
    },
    state: () => ctx.recompRace.state(),
    phase(n, session) { u32[0x264 >> 2] = n; u32[0x2bc >> 2] = session; u32[0x2cc >> 2] = 6; },
    count: (k) => sent.filter((m) => m.k === k).length,
  };
}

const roster = [{ id: 1, name: "Host", car: 1, grid: 0 }, { id: 2, name: "Guest", car: 1, grid: 1 }];
const start = { k: "start", id: 1, rv: 3, race: 50, key: 9, type: 0, laps: 1, opts: "00".repeat(40), roster };

// A race this game is in, as far as `upTo`.
function inRace(me, upTo = "racing") {
  const g = game(me);
  g.say(start);
  assert.equal(g.state().race?.race, 50);
  g.phase(PHASE.holding, 50);
  g.run(100);
  if (upTo === "loaded") return g;
  g.say({ k: "ready", id: 1, race: 50 });
  g.say({ k: "ready", id: 2, race: 50 });
  g.run(100);
  g.say({ k: "go", id: 1, race: 50 });
  g.phase(PHASE.racing, 50);
  g.run(100);
  return g;
}

test("a guest repeats ready until the go arrives", () => {
  const g = inRace(2, "loaded");
  assert.equal(g.count("ready"), 1);
  for (let i = 0; i < 30; i++) g.run(100); // 3 s: the first ready was lost
  assert.ok(g.count("ready") >= 3, `ready sent ${g.count("ready")} times`);
  g.say({ k: "go", id: 1, race: 50 });
  const before = g.count("ready");
  for (let i = 0; i < 30; i++) g.run(100);
  assert.equal(g.count("ready"), before, "no more ready after the go");
});

test("the host repeats go, then stops", () => {
  const g = inRace(1, "loaded");
  g.say({ k: "ready", id: 2, race: 50 }); g.say({ k: "ready", id: 1, race: 50 });
  g.run(100);
  assert.equal(g.count("go"), 1);
  for (let i = 0; i < 30; i++) g.run(100);
  assert.ok(g.count("go") >= 3, `go sent ${g.count("go")} times`);
  for (let i = 0; i < 300; i++) g.run(100);
  const settled = g.count("go");
  for (let i = 0; i < 50; i++) g.run(100);
  assert.equal(g.count("go"), settled, "the go stops repeating");
});

test("the host repeats start while a guest has not answered", () => {
  const g = game(1);
  g.ctx.recompRace.host(true);
  g.run(100);
  g.u32[0x270 >> 2] = 77; g.u32[0x26c >> 2] = 1;
  g.run(100);
  const lobby = g.state().lobby;
  assert.ok(lobby);
  g.say({ k: "join", id: 2, name: "Guest", rv: 3, race: lobby.race, car: 1 });
  g.ctx.recompRace.startLobby();
  assert.equal(g.count("start"), 1);
  g.say({ ...g.sent.findLast((m) => m.k === "start"), id: 1 });
  for (let i = 0; i < 30; i++) g.run(100);
  assert.ok(g.count("start") >= 3, `start sent ${g.count("start")} times`);
  g.say({ k: "ready", id: 1, race: lobby.race }); g.say({ k: "ready", id: 2, race: lobby.race });
  g.run(100);
  const settled = g.count("start");
  for (let i = 0; i < 30; i++) g.run(100);
  assert.equal(g.count("start"), settled, "start stops once everyone is ready");
});

test("a duplicate start does not disturb a race in progress", () => {
  const g = inRace(2);
  const before = g.state().race.startedAt;
  g.say(start);
  assert.equal(g.state().race.startedAt, before);
  assert.equal(g.state().race.went, true);
});

test("a finish repeats, then stops", () => {
  const g = inRace(2);
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 61.5; // done, with its time
  g.run(100);
  assert.equal(g.count("finish"), 1);
  for (let i = 0; i < 30; i++) g.run(100);
  assert.ok(g.count("finish") >= 3, `finish sent ${g.count("finish")} times`);
  for (let i = 0; i < 300; i++) g.run(100);
  const settled = g.count("finish");
  for (let i = 0; i < 50; i++) g.run(100);
  assert.equal(g.count("finish"), settled);
});

test("the game hears when a player left, and when they come back", () => {
  const g = inRace(2);
  g.people([2]);
  for (let i = 0; i < 70; i++) g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].left, true);
  assert.equal(g.u32[PEER0_DONE >> 2], 2, "left; not shown as finished (the game would rank them 1st) while we race");
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 61.5; // our finish
  g.run(100);
  assert.equal(g.u32[PEER0_DONE >> 2], 3, "given up on, and left, once we have finished");
  g.people([1, 2]);
  for (let i = 0; i < 3; i++) g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].left, false);
  assert.equal(g.u32[PEER0_DONE >> 2], 1, "still given up on, back on the road");
});

test("a player left out at the start is off the road at once and not ranked before we finish", () => {
  const g = inRace(1, "loaded");
  g.say({ k: "ready", id: 1, race: 50 });
  g.say({ k: "go", id: 1, race: 50, without: [2] });
  g.phase(PHASE.racing, 50);
  g.run(100);
  assert.equal(g.u32[PEER0_DONE >> 2], 2, "off the road, not finished");
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 70;
  g.run(100);
  assert.equal(g.u32[PEER0_DONE >> 2], 3, "last in the results once we have finished");
});

test("a player who finished and then left also leaves the road", () => {
  const g = inRace(2);
  g.say({ k: "finish", id: 1, race: g.state().race?.race, time: 61.5 });
  g.run(100);
  assert.equal(g.u32[PEER0_DONE >> 2] & 1, 1, "their finish arrived");
  g.people([2]);
  for (let i = 0; i < 70; i++) g.run(100);
  assert.equal(g.u32[PEER0_DONE >> 2], 3);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].time, 61.5, "their real time stays");
});

test("a player who leaves a running Quick Race stops holding up the results", () => {
  const g = inRace(2);
  g.people([2]);
  g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].done, false, "not before the grace is over");
  for (let i = 0; i < 70; i++) g.run(100);
  const gone = g.state().standings.find(([id]) => id === 1)[1];
  assert.equal(gone.done, true);
  assert.equal(gone.dnf, true);
  assert.ok(gone.time > 1000, "ranks behind anyone who finished");
});

test("a player back within the grace keeps their place", () => {
  const g = inRace(2);
  g.people([2]);
  for (let i = 0; i < 20; i++) g.run(100);
  g.people([1, 2]);
  for (let i = 0; i < 80; i++) g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].done, false);
});

test("a hung player, still connected, stops holding up the results", () => {
  const g = inRace(2);
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 61.5;
  g.run(100);
  for (let i = 0; i < 190; i++) g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].done, false, "not before the limit");
  for (let i = 0; i < 20; i++) g.run(100);
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].done, true);
});

test("a slow player's real finish replaces the one given up on", () => {
  const g = inRace(2);
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 61.5;
  g.run(100);
  for (let i = 0; i < 210; i++) g.run(100);
  g.say({ k: "finish", id: 1, race: 50, time: 75.25 });
  const s = g.state().standings.find(([id]) => id === 1)[1];
  assert.equal(s.time, 75.25);
  assert.ok(!s.gone);
});

test("a player still reporting is never given up on", () => {
  const g = inRace(2);
  g.u32[0x2a8 >> 2] = 1; g.f32[0x2b0 >> 2] = 61.5;
  g.run(100);
  for (let i = 0; i < 300; i++) {
    g.run(100);
    if (i % 10 === 0) g.say({ k: "progress", id: 1, race: 50, pct: 50, ms: 1000 + i });
  }
  assert.equal(g.state().standings.find(([id]) => id === 1)[1].done, false);
});

test("no invites during a solo race; they come back after it", () => {
  const g = game(2);
  const lobby = { k: "lobby", id: 1, name: "Host", rv: 3, race: 61, key: 9, type: 0, laps: 2, opts: "00".repeat(40), roster: [roster[0]] };
  g.u32[0x2cc >> 2] = 6 | 0x800; // free roam: invites show
  g.say(lobby);
  assert.equal(g.state().invites.length, 1);
  g.u32[0x2cc >> 2] = 6 | 0x200; // a career race starts
  g.run(100);
  assert.equal(g.state().invites.length, 0, "the one showing goes");
  g.say(lobby);
  assert.equal(g.state().invites.length, 0, "repeats stay away");
  g.u32[0x2cc >> 2] = 6 | 0x800; // back in free roam
  g.run(100);
  g.say(lobby);
  assert.equal(g.state().invites.length, 1, "the race still open shows again");
});

test("invites show in the menus after a race, while the game's racing flag is still on", () => {
  const g = game(2);
  const lobby = { k: "lobby", id: 1, name: "Host", rv: 3, race: 62, key: 9, type: 0, laps: 2, opts: "00".repeat(40), roster: [roster[0]] };
  g.u32[0x2cc >> 2] = 3 | 0x200; // the front end, flag not yet cleared
  g.say(lobby);
  assert.equal(g.state().invites.length, 1);
});

test("the host can start without a slow player after 15 s", () => {
  const g = inRace(1, "loaded");
  g.say({ k: "ready", id: 1, race: 50 });
  g.run(100);
  assert.equal(g.body.find("Start without Guest"), null, "not at once");
  for (let i = 0; i < 160; i++) g.run(100);
  const b = g.body.find("Start without Guest");
  assert.ok(b, "offered after 15 s");
  b.onclick({ preventDefault() {} });
  assert.deepEqual([...g.sent.findLast((m) => m.k === "go").without], [2]);
  g.say({ k: "go", id: 1, race: 50, without: [2] });
  assert.equal(g.state().race.went, true);
  assert.equal(g.state().standings.find(([id]) => id === 2)[1].done, true, "the one left out is out of the results");
});

test("the loading timeout starts the race for those ready, not cancels it for all", () => {
  const g = inRace(1, "loaded");
  g.say({ k: "ready", id: 1, race: 50 });
  for (let i = 0; i < 620; i++) g.run(100);
  assert.equal(g.count("cancel"), 0);
  assert.deepEqual([...g.sent.findLast((m) => m.k === "go").without], [2]);
});

test("a go still on its way is not cancelled by the next tick", () => {
  const g = inRace(1, "loaded");
  g.say({ k: "ready", id: 1, race: 50 });
  for (let i = 0; i < 700; i++) g.run(100); // the go's echo never comes
  assert.equal(g.state().race.cancelled, undefined);
});

test("a host that is not loaded either still cancels at the timeout", () => {
  const g = game(1);
  g.say(start);
  for (let i = 0; i < 620; i++) g.run(100);
  assert.equal(g.count("go"), 0);
  assert.ok(g.count("cancel") >= 1);
  assert.equal(g.state().race.cancelled, true);
});

test("a player left out is told, and the race ends for them alone", () => {
  const g = inRace(2, "loaded");
  g.say({ k: "go", id: 1, race: 50, without: [2] });
  assert.equal(g.state().race.cancelled, true);
  assert.equal(g.state().race.went, false);
});

test("a player left out in the menus is offered Dismiss, not a reload", () => {
  const g = game(2);
  g.say(start);
  g.say({ k: "go", id: 1, race: 50, without: [2] });
  g.run(100);
  assert.ok(g.body.find("Dismiss"));
  assert.equal(g.body.find("Exit to start page"), null);
  g.body.find("Dismiss").onclick({ preventDefault() {} });
  assert.equal(g.state().race, null);
});

test("the card minimizes while waiting and opens again", () => {
  const g = inRace(1, "loaded");
  g.run(100);
  const min = g.body.find("Minimize");
  assert.ok(min);
  min.onclick({ preventDefault() {} });
  assert.ok(g.body.find("Show"), "a small bar stays");
  assert.equal(g.body.find("Minimize"), null);
  g.body.find("Show").onclick({ preventDefault() {} });
  assert.ok(g.body.find("Minimize"));
});

test("a player in view who goes to a menu is explained, once, and a short pause is not", () => {
  const g = game(2);
  const state = (mode) => { Object.assign(g.ctx.recompOnlineState, { open: true, people: [{ id: 1, name: "Host" }], slotIds: [1], slotModes: [mode] }); g.ctx.dispatchEvent(new g.ctx.CustomEvent("recomp-online", { detail: g.ctx.recompOnlineState })); };
  g.run(100); state(1); state(0);
  for (let i = 0; i < 10; i++) g.run(100);
  assert.equal(g.body.find("Host went to the safehouse or a menu."), null, "1 s is only a pause");
  state(1); state(0);
  for (let i = 0; i < 35; i++) g.run(100);
  assert.ok(g.body.find("Host went to the safehouse or a menu."));
});

test("a player who is gone from the room is reported", () => {
  const g = game(2);
  g.run(100);
  const state = (s) => { Object.assign(g.ctx.recompOnlineState, { open: true }, s); g.ctx.dispatchEvent(new g.ctx.CustomEvent("recomp-online", { detail: g.ctx.recompOnlineState })); };
  state({ people: [{ id: 1, name: "Host" }], slotIds: [1], slotModes: [1] });
  state({ people: [], slotIds: [0], slotModes: [-1] });
  assert.ok(g.body.find("Host left the game."));
});

test("a world race goes on without a player in the safehouse, and says who", () => {
  const g = game(1);
  g.u32[0x2cc >> 2] = 6 | 0x800;
  g.ctx.recompRace.host(true);
  g.run(100);
  g.u32[0x270 >> 2] = 77; g.u32[0x26c >> 2] = 1;
  g.run(100);
  const lobby = g.state().lobby;
  g.say({ k: "join", id: 2, name: "Guest", rv: 3, race: lobby.race, car: 1, world: true });
  g.ctx.recompRace.startLobby();
  g.say({ ...g.sent.findLast((m) => m.k === "start"), id: 1 });
  g.say({ k: "leave", id: 2, race: lobby.race, reason: "menu" });
  assert.equal(g.state().race.cancelled, undefined, "not cancelled for the host");
  assert.equal(g.count("cancel"), 0);
  assert.ok(g.body.find("Guest is in the safehouse or a menu: the race goes on without them."));
  g.phase(PHASE.holding, lobby.race);
  g.run(100);
  g.say({ k: "ready", id: 1, race: lobby.race });
  assert.deepEqual([...g.sent.findLast((m) => m.k === "go").without], [2], "the go leaves the guest out");
});

const three = [...roster, { id: 3, name: "Third", car: 1, grid: 2 }];
const start3 = { ...start, roster: three };
function loading3(me) {
  const g = game(me);
  g.people([1, 2, 3]);
  g.say(start3);
  g.phase(PHASE.holding, 50);
  g.run(100);
  return g;
}

test("a player who leaves the room while loading is dropped; the others race", () => {
  const g = loading3(1);
  g.people([1, 2]);
  assert.equal(g.state().race.cancelled, undefined);
  assert.equal(g.count("cancel"), 0);
  g.say({ k: "ready", id: 1, race: 50 }); g.say({ k: "ready", id: 2, race: 50 });
  assert.deepEqual([...g.sent.findLast((m) => m.k === "go").without], [3]);
  g.say({ k: "go", id: 1, race: 50, without: [3] });
  assert.equal(g.state().race.went, true);
  assert.equal(g.state().standings.find(([id]) => id === 3)[1].done, true, "out of the results");
});

test("a host who leaves while loading hands the race to the next player", () => {
  const two = loading3(2), third = loading3(3);
  for (const g of [two, third]) g.people([2, 3]);
  for (const g of [two, third]) {
    assert.equal(g.state().race.cancelled, undefined);
    assert.equal(g.state().race.host, 2, "both pick the same host");
  }
  for (const g of [two, third]) { g.say({ k: "ready", id: 2, race: 50 }); g.say({ k: "ready", id: 3, race: 50 }); }
  const go = two.sent.findLast((m) => m.k === "go");
  assert.deepEqual([...go.without], [1], "the new host starts it without the old one");
  third.say({ ...go, id: 2 });
  assert.equal(third.state().race.went, true, "the go of the new host counts");
  assert.equal(third.count("go"), 0);
});

test("a Canyon Duel is still cancelled when the other player leaves", () => {
  const g = game(1);
  g.people([1, 2]);
  g.say({ ...start, type: 13 });
  g.people([1]);
  assert.equal(g.state().race.cancelled, true);
});

test("a host who leaves the lobby hands it to the first player who joined", () => {
  const lobbyMsg = (roster) => ({ k: "lobby", id: 1, rv: 3, race: 60, key: 9, type: 0, laps: 2, ai: false, opts: "00".repeat(40), roster });
  const players = [{ id: 1, name: "Host", car: 1 }, { id: 2, name: "Guest", car: 1 }, { id: 3, name: "Third", car: 1 }];
  const two = game(2), third = game(3);
  for (const g of [two, third]) {
    g.u32[0x2cc >> 2] = 3;
    g.people([1, 2, 3]);
    g.say(lobbyMsg(players));
    g.ctx.recompRace.join(60);
    g.run(100);
  }
  for (const g of [two, third]) g.people([2, 3]);
  const lobby = two.state().lobby;
  assert.ok(lobby, "the guest who joined first hosts it now");
  assert.deepEqual([...lobby.roster.map((r) => r.id)], [2, 3]);
  assert.equal(lobby.laps, 2);
  assert.ok(two.sent.some((m) => m.k === "lobby" && m.race === 60), "and tells the others");
  const inv = third.state().invites.find((i) => i.race === 60);
  assert.equal(inv?.from, 2, "the other guest stays in it, under the new host");
  assert.ok(third.body.find("Host left: Guest hosts this race now."));
  two.ctx.recompRace.startLobby();
  assert.equal(two.sent.findLast((m) => m.k === "start")?.race, 60, "the new host can start it");
});

test("a cancelled race left in the menus does not eat the next Start", () => {
  const g = game(1);
  g.say(start);
  g.say({ k: "cancel", id: 1, race: 50 });
  assert.equal(g.state().race.cancelled, true);
  g.ctx.recompRace.host(true);
  g.u32[0x270 >> 2] = 77; g.u32[0x26c >> 2] = 1;
  g.run(100); g.run(100);
  g.u32[0x26c >> 2] = 2;
  g.run(100);
  assert.ok(g.state().lobby, "the lobby opens, so the others get an invite");
  assert.ok(g.sent.some((m) => m.k === "lobby"));
});

test("a Canyon Duel takes exactly two players", () => {
  const g = game(1);
  g.ctx.recompRace.host(true);
  g.run(100);
  g.u32[0x270 >> 2] = 77; g.u32[0x274 >> 2] = 13; g.u32[0x26c >> 2] = 1;
  g.run(100);
  g.ctx.recompRace.startLobby();
  assert.equal(g.count("start"), 0, "not alone");
  g.say({ k: "join", id: 2, name: "Guest", rv: 3, race: g.state().lobby.race, car: 1 });
  g.ctx.recompRace.startLobby();
  assert.equal(g.count("start"), 1);
});

test("a Canyon Duel turns a third player away, so it can still start", () => {
  const g = game(1);
  g.ctx.recompRace.host(true);
  g.run(100);
  g.u32[0x270 >> 2] = 77; g.u32[0x274 >> 2] = 13; g.u32[0x26c >> 2] = 1;
  g.run(100);
  const race = g.state().lobby.race;
  g.say({ k: "join", id: 2, name: "Guest", rv: 3, race, car: 1 });
  g.say({ k: "join", id: 3, name: "Third", rv: 3, race, car: 1 });
  assert.deepEqual([...g.state().lobby.roster.map((r) => r.id)], [1, 2], "the third is not added");
  g.ctx.recompRace.startLobby();
  assert.equal(g.count("start"), 1, "and the duel starts");
});

test("a race hosted in career free roam is offered only to players in career free roam", () => {
  const world = { k: "lobby", id: 1, name: "Host", rv: 3, race: 65, key: 9, type: 0, laps: 1, opts: "00".repeat(40), world: true,
    roster: [{ id: 1, name: "Host", car: 1 }] };
  const menus = game(2);
  menus.people([1, 2]);
  menus.say(world);
  assert.equal(menus.state().invites.length, 0, "a player in the menus (maybe with no career) is not offered it");
  const roam = game(3);
  roam.u32[0x2cc >> 2] = 6 | 0x800;
  roam.people([1, 3]);
  roam.say({ ...world, race: 66 });
  assert.equal(roam.state().invites.length, 1, "a player in free roam is");
});

test("with two hosts at once, their repeats do not swap the race offered", () => {
  const lobbyOf = (id, race, name) => ({ k: "lobby", id, name, rv: 3, race, key: 9, type: 0, laps: 2, opts: "00".repeat(40),
    roster: [{ id, name, car: 1 }] });
  const g = game(3);
  g.people([1, 2, 3]);
  g.say(lobbyOf(1, 70, "First"));
  g.run(100);
  g.say(lobbyOf(2, 71, "Second"));
  g.run(100);
  g.say(lobbyOf(1, 70, "First")); // the first host's 4-second repeat
  assert.equal(g.state().invites.length, 2, "both are listed");
  g.ctx.recompRace.join();
  assert.equal(g.sent.findLast((m) => m.k === "join")?.race, 71, "still the newest hosted race, not the last repeat");
});

test("a full Canyon Duel is not offered, and a player turned away is told", () => {
  const duel = (roster) => ({ k: "lobby", id: 1, name: "Host", rv: 3, race: 64, key: 9, type: 13, laps: 1, opts: "00".repeat(40), roster });
  const host = { id: 1, name: "Host", car: 1 }, guest = { id: 2, name: "Guest", car: 1 };
  const late = game(3);
  late.u32[0x2cc >> 2] = 3;
  late.people([1, 2, 3]);
  late.say(duel([host, guest]));
  assert.equal(late.state().invites.length, 0, "a full duel is not shown");
  const third = game(3);
  third.u32[0x2cc >> 2] = 3;
  third.people([1, 2, 3]);
  third.say(duel([host]));
  third.ctx.recompRace.join(64);
  assert.ok(third.sent.some((m) => m.k === "join"));
  third.say(duel([host, guest]));
  assert.equal(third.state().invites.length, 0, "the invite goes when the place is taken");
  assert.ok(third.body.find("Host's Canyon Duel is full."));
});

test("a player whose car reads as 0 can still join", () => {
  const g = game(2);
  g.u32[0x268 >> 2] = 0;
  g.say({ k: "lobby", id: 1, name: "Host", rv: 3, race: 63, key: 9, type: 0, laps: 2, opts: "00".repeat(40), roster: [roster[0]] });
  g.ctx.recompRace.join();
  assert.ok(g.sent.some((m) => m.k === "join"));
});

test("Not now stays dismissed through the host's repeats", () => {
  const g = game(2);
  const lobby = { k: "lobby", id: 1, name: "Host", rv: 3, race: 60, key: 9, type: 0, laps: 2, opts: "00".repeat(40), roster: [roster[0]] };
  g.say(lobby);
  assert.equal(g.state().invites.length, 1);
  g.run(100);
  const notNow = g.body.find("Not now");
  assert.ok(notNow, "the invite card has a Not now button");
  notNow.onclick({ preventDefault() {} });
  assert.equal(g.state().invites.length, 0);
  g.say(lobby);
  assert.equal(g.state().invites.length, 0, "the repeat does not bring it back");
});

// ---- Open races (2026-11-06): the corner's cards and chip, and F9 → Race's list ----
const open = (id, race, { name = `p${id}`, type = 0, laps = 2, world = false, ai = false, players = 1 } = {}) => ({
  k: "lobby", id, name, rv: 3, race, key: 9, type, laps, ai, world, opts: "00".repeat(40),
  roster: Array.from({ length: players }, (_, i) => ({ id: i ? 500 + race * 10 + i : id, name: i ? `x${i}` : name, car: 1 })) });
// Values from the page's own realm (vm) compared as plain JSON.
const same = (a, b, m) => assert.deepEqual(JSON.parse(JSON.stringify(a)), b, m);
const texts = (e, out = []) => { if (e.textContent) out.push(e.textContent); for (const k of e.kids || []) texts(k, out); return out; };
function hosts(g, ids, crew = {}) {
  g.ctx.recompOnlineState.crewId = crew.mine || null;
  g.people([g.state().me, ...ids]);
  g.ctx.recompOnlineState.people = g.ctx.recompOnlineState.people.map((p) => ({ ...p, crewId: crew[p.id] || null }));
}

test("one or two open races show as cards, three or more as a chip with the count", () => {
  const g = game(9);
  hosts(g, [1, 2, 3, 4]);
  g.say(open(1, 71)); g.run(100);
  g.say(open(2, 72)); g.run(100);
  same(g.ctx.recompRace.corner().cards.map((i) => i.race), [72, 71], "two cards, newest first");
  assert.equal(g.ctx.recompRace.corner().chip, 0);
  g.say(open(1, 71)); g.run(100); // a repeat does not reorder them
  same(g.ctx.recompRace.corner().cards.map((i) => i.race), [72, 71]);
  assert.equal(texts(g.body).filter((t) => t === "Not now").length, 2, "each card has its own Not now");
  g.say(open(3, 73)); g.run(100);
  same(g.ctx.recompRace.corner(), { cards: [], chip: 3 });
  assert.ok(texts(g.body).includes("races open") && texts(g.body).includes("3"), "the chip says 3 races open");
  assert.ok(!texts(g.body).includes("Not now"), "no cards beside the chip");
  g.say(open(4, 74)); g.run(100);
  assert.equal(g.ctx.recompRace.corner().chip, 4, "the count follows");
  for (let i = 0; i < 160; i++) { g.run(100); if (i % 30 === 0) for (const [id, r] of [[1, 71], [2, 72]]) g.say(open(id, r)); }
  same(g.ctx.recompRace.corner().cards.map((i) => i.race), [72, 71], "the races nobody repeats are gone: cards again");
});

test("a crew mate's race keeps a full card next to the chip", () => {
  const g = game(9);
  hosts(g, [1, 2, 3], { mine: "c7", 2: "c7", 3: "other" });
  g.say(open(1, 71)); g.run(100);
  g.say(open(2, 72)); g.run(100);
  g.say(open(3, 73)); g.run(100);
  const c = g.ctx.recompRace.corner();
  assert.equal(c.chip, 3);
  same(c.cards.map((i) => i.race), [72], "only the crew mate's race (not another crew's)");
  assert.ok(texts(g.body).includes("Crew invite"));
  const rows = g.ctx.recompRace.openRaces().rows;
  assert.equal(rows[0].race, 72, "and it leads the F9 list");
  assert.equal(rows[0].crew, true);
});

test("F9's open races: crew first, then nearly full, then the newest", () => {
  const g = game(9);
  hosts(g, [1, 2, 3, 4, 5, 6], { mine: "c1", 6: "c1" });
  g.say(open(1, 81, { players: 2 })); g.ctx.__now += 60000;  // 7 places left, oldest
  g.say(open(2, 82, { players: 2 })); g.ctx.__now += 60000;  // 7 left, newer
  g.say(open(1, 81, { players: 2 })); g.say(open(2, 82, { players: 2 })); // their repeats
  g.say(open(3, 83, { type: 13 })); g.run(100);        // Canyon Duel 1/2: 1 left
  g.say(open(4, 84, { players: 8 })); g.run(100);      // 8/9: 1 left, newest
  g.say(open(5, 85, { players: 9 })); g.run(100);      // full: last
  g.say(open(6, 86)); g.run(100);                      // crew mate, 8 left
  const rows = g.ctx.recompRace.openRaces().rows;
  same(rows.map((r) => r.race), [86, 84, 83, 82, 81, 85]);
  same(rows.map((r) => `${r.players}/${r.max}`), ["1/9", "8/9", "1/2", "2/9", "2/9", "9/9"]);
  assert.equal(rows.at(-1).button, "Full");
  assert.equal(rows.find((r) => r.race === 81).minutes, 2, "minutes open");
  assert.equal(rows.find((r) => r.race === 83).kind, "Canyon duel");
});

test("a host's lobby says how long it has been open, and a late player counts from then", () => {
  const h = game(1);
  h.ctx.recompRace.host(true); h.run(100);
  h.u32[0x270 >> 2] = 77; h.u32[0x26c >> 2] = 1; h.run(100);
  h.run(125000);
  const lobby = h.sent.findLast((m) => m.k === "lobby");
  assert.ok(lobby.age >= 120 && lobby.age <= 130, `age ${lobby.age}`);
  const g = game(9);
  hosts(g, [1]);
  g.say({ ...lobby, id: 1, name: "Host" });
  assert.equal(g.ctx.recompRace.openRaces().rows[0].minutes, 2);
});

test("Not now hides that host's races from the corner for ten minutes; F9 still lists them", () => {
  const g = game(9);
  hosts(g, [1, 2]);
  g.say(open(1, 71)); g.run(100);
  g.say(open(2, 72)); g.run(100);
  const card = g.body.find("Not now"); // the newest card first: host 2
  card.onclick({ preventDefault() {} });
  g.run(100);
  same(g.state().invites.map((i) => i.race), [71]);
  g.say(open(2, 73)); g.run(100);    // the same host opens another race
  same(g.state().invites.map((i) => i.race), [71], "the host's other races stay away too");
  same(g.ctx.recompRace.openRaces().rows.map((r) => r.race).sort(), [71, 72, 73], "F9 lists every race");
  assert.equal(g.ctx.recompRace.openRaces().rows.find((r) => r.race === 72).canJoin, true, "and they can be joined from there");
  g.ctx.__now += 9 * 60000;
  for (const [id, r] of [[1, 71], [2, 72], [2, 73]]) g.say(open(id, r));
  g.run(100);
  same(g.state().invites.map((i) => i.race), [71], "still hidden after 9 minutes");
  g.ctx.__now += 61000;
  for (const [id, r] of [[1, 71], [2, 72], [2, 73]]) g.say(open(id, r));
  g.run(100);
  same(g.state().invites.map((i) => i.race).sort(), [71, 72, 73], "back after 10");
});

test("Race invites Off hides the corner, not the F9 list", () => {
  const g = game(9);
  hosts(g, [1, 2, 3]);
  g.ctx.recompRace.setInvites(false);
  for (const [id, r] of [[1, 71], [2, 72], [3, 73]]) g.say(open(id, r));
  g.run(100);
  same(g.ctx.recompRace.corner(), { cards: [], chip: 0 });
  assert.ok(!texts(g.body).includes("races open"));
  assert.equal(g.ctx.recompRace.openRaces().rows.length, 3);
  g.ctx.recompRace.setInvites(true); g.run(100);
  assert.equal(g.ctx.recompRace.corner().chip, 3);
});

test("F9's filters: only the kinds present, and each shows its own", () => {
  const g = game(9);
  hosts(g, [1, 2, 3, 4]);
  g.say(open(1, 71, { type: 0 })); g.say(open(2, 72, { type: 11 })); g.say(open(3, 73, { type: 12 })); g.say(open(4, 74, { type: 1, world: true }));
  const all = g.ctx.recompRace.openRaces();
  same(all.filters.map((f) => `${f.id}:${f.count}`), ["all:4", "quick:3", "roam:1", "sprint:1", "circuit:1", "drift:2"], "no Canyon Duel: none open");
  same(g.ctx.recompRace.openRaces("drift").rows.map((r) => r.race).sort(), [72, 73], "Drift takes Canyon drift too");
  same(g.ctx.recompRace.openRaces("roam").rows.map((r) => r.race), [74]);
  const gone = g.ctx.recompRace.openRaces("duel");
  assert.equal(gone.filter, "all", "a filter with nothing left falls back to All");
  assert.equal(gone.rows.length, 4);
});

test("F9 lists races not joinable from here, with where to join them from", () => {
  const g = game(9);                               // in the menus (flow 3)
  hosts(g, [1, 2]);
  g.say(open(1, 71, { world: true })); g.say(open(2, 72));
  assert.equal(g.state().invites.length, 1, "the corner offers only the Quick Race");
  const menus = Object.fromEntries(g.ctx.recompRace.openRaces().rows.map((r) => [r.race, r]));
  same([menus[71].button, menus[71].canJoin, menus[71].why], ["Free roam", false, "Join from career free roam."]);
  same([menus[72].button, menus[72].canJoin], ["Join", true]);
  g.u32[0x2cc >> 2] = 6 | 0x800;                  // career free roam
  const roam = Object.fromEntries(g.ctx.recompRace.openRaces().rows.map((r) => [r.race, r]));
  same([roam[71].button, roam[71].canJoin], ["Join", true]);
  same([roam[72].button, roam[72].canJoin, roam[72].why], ["Menus", false, "Join from the game's menus (Quick Race)."]);
});

test("joining another race from the list leaves the first; a race that starts leaves the list", () => {
  const g = game(9);
  hosts(g, [1, 2, 3]);
  g.say(open(1, 71)); g.say(open(2, 72)); g.say(open(3, 73));
  g.ctx.recompRace.join(71);
  g.say({ ...open(1, 71), roster: [{ id: 1, name: "p1", car: 1 }, { id: 9, name: "Me", car: 1 }] });
  g.ctx.recompRace.join(72);
  same(g.sent.filter((m) => m.k === "join" || m.k === "leave").map((m) => `${m.k}:${m.race}`), ["join:71", "leave:71", "join:72"]);
  g.say({ ...open(1, 71), roster: [{ id: 1, name: "p1", car: 1 }, { id: 9, name: "Me", car: 1 }] }); // sent before host 1 heard the leave
  same(g.ctx.recompRace.openRaces().rows.filter((r) => r.joined).map((r) => r.race), [72], "one joined race at a time");
  g.say({ k: "start", id: 3, rv: 3, race: 73, key: 9, type: 0, laps: 1, opts: "00".repeat(40), roster: [{ id: 3, name: "p3", car: 1, grid: 0 }] });
  assert.ok(!g.ctx.recompRace.openRaces().rows.some((r) => r.race === 73), "73 began without us: no longer open");
});

test("a card's Leave and Not now still work after the host's repeats (found in the browser, 2026-11-06)", () => {
  const g = game(9);
  hosts(g, [1]);
  g.say(open(1, 71)); g.run(100);
  g.ctx.recompRace.join(71);
  const withMe = { ...open(1, 71), roster: [{ id: 1, name: "p1", car: 1 }, { id: 9, name: "Me", car: 1 }] };
  g.say(withMe); g.run(100);
  const notNow = g.body.find("Not now");         // the card as drawn now
  g.say(withMe); g.say(withMe);                 // repeats after it was drawn
  notNow.onclick({ preventDefault() {} });
  assert.equal(g.sent.findLast((m) => m.k === "leave")?.race, 71);
  g.say(open(1, 71)); g.run(100);               // the host's repeat without us
  same(g.ctx.recompRace.openRaces().rows.map((r) => [r.race, r.joined, r.hidden]), [[71, false, true]]);
  same(g.ctx.recompRace.corner(), { cards: [], chip: 0 });
});
