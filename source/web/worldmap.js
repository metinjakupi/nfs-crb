// worldmap.js - other players on Need for Speed: Carbon's big world map, the one
// the free-roam pause menu opens. The map itself is the game's: the mod
// (mods/core/nfsc/worldmap.h) puts other players in your online room on it
// as the map's own car arrows, near or far, turned the way they drive and
// tinted in their crew's colour, and shows their name beside the arrow the map's
// cursor is on. All this page does is tell the game who to show and where they
// are. The start page (start.js) loads it with menu.js, chat.js, radar.js and
// race.js when the player presses Play. Like chat.js it wakes up only when the
// game goes online (window.recompOnline, or net.js's first "recomp-online" event)
// and does nothing offline.
//
// WHO. The map has 10 arrows. A room may hold a few hundred players, and the
// relay tells net.js once a second where everyone driving is (the "where" feed:
// window.recompOnlineWhere, "recomp-online-where" events). This file lists the
// 10 to show: crew-mates first, then the nearest; one already listed counts at
// KEEP of its distance, so arrows do not swap back and forth between two
// players at about the same distance. Players whose car is in one of net.js's 8
// slots are also listed when the feed has not named them yet (the game moves
// their arrow with the car itself). A relay older than the feed sends none:
// then the list is the players in the slots, as before.
//
// THE GAME. worldmap.h shares a block of guest memory (globalThis.carbonWorldMapBlock,
// "carbon-worldmap" event; layout WmShared in worldmap.h): magic +0x00, the page's
// sequence number +0x08 (odd while it writes), the game's count of world-map frames
// +0x0c, then the players from +0x10. Version 2 (magic "WMP2"): 10 players of 0x30
// bytes: relay id, crew colour as 0xAARRGGBB (0 = none), name (Latin-1, up to 23
// bytes, NUL-terminated), then floats x, z (where the feed last had their car, in
// the game's physics space; NaN = not known) and fx, fz (the way its nose points);
// the game's part: how many players the map shows +0x1f0, whose name the label
// shows +0x1f4. Version 1 ("WMP1", an older game build): 8 players of 0x20 bytes
// (id, colour, name), shown +0x110, hover +0x114, places from the slots only.
"use strict";
(() => {
  const SEQ = 0x08, UPDATES = 0x0c, FRIENDS = 0x10, NAME = 24;
  const LAYOUTS = new Map([
    [0x31504d57, { count: 8, size: 0x20, shown: 0x110, hover: 0x114, places: false }],
    [0x32504d57, { count: 10, size: 0x30, shown: 0x1f0, hover: 0x1f4, places: true }],
  ]);
  const NEUTRAL = 0xdbe4ee; // no crew: the radar's light grey
  const FEED_FRESH = 5000;  // ms: an older where feed is not used (the relay has stopped sending it)
  const KEEP = 0.75;        // a listed player counts at this much of its distance
  let active = false, told = "", listed = new Set();

  // The block and its layout, if the game has made it.
  let views = null;
  function shared() {
    const b = globalThis.carbonWorldMapBlock;
    if (!b) return null;
    const buf = b.memory.buffer;
    if (!views || views.buf !== buf)
      views = { buf, u8: new Uint8Array(buf), u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
    const layout = LAYOUTS.get(views.u32[b.ptr >> 2]);
    return layout ? { ptr: b.ptr, layout } : null;
  }

  // "#ff66cc" -> 0xff66cc; "" -> the neutral grey.
  function rgb(css) {
    const m = /^#?([0-9a-f]{6})$/i.exec(css || "");
    return m ? parseInt(m[1], 16) : NEUTRAL;
  }
  // The game's text is Latin-1: other letters become "?".
  function latin1(name) {
    const out = [];
    for (const ch of String(name || "").normalize("NFC")) {
      if (out.length >= NAME - 1) break;
      const c = ch.codePointAt(0);
      out.push(c >= 0x20 && c < 0x100 && c !== 0x7f ? c : 0x3f);
    }
    return out;
  }

  // Who to show, at most `count`, best first: [{id, name, crew, x, z, h}] (x NaN: the game knows where).
  function choose(count, places) {
    const s = window.recompOnlineState;
    const people = new Map((s?.people || []).filter((p) => p && p.id).map((p) => [p.id >>> 0, p]));
    const slots = (s?.slotIds || []).filter((id) => id).map((id) => id >>> 0);
    if (!places) return [...people.values()].slice(0, count).map((p) => ({ id: p.id >>> 0, name: p.name, crew: p.crew }));
    const feed = window.recompOnlineWhere;
    const fresh = feed && Array.isArray(feed.p) && performance.now() - feed.at <= FEED_FRESH;
    const me = window.recompOnlineSelf?.() || null;
    const key = (c) => (window.recompCrewKey ? window.recompCrewKey(c) : String(c || "").toLowerCase().replace(/\s+/g, ""));
    const mine = key(s?.crew ?? window.recompOnline?.crew ?? "");
    const all = new Map();
    for (const e of fresh ? feed.p : []) {
      const id = e[0] >>> 0, p = people.get(id);
      if (!p) continue; // not (or no longer) in the room as far as net.js knows
      const d = me ? Math.hypot(e[1] - me[0], e[2] - me[2]) : Infinity;
      all.set(id, { id, name: p.name, crew: p.crew, x: e[1], z: e[2], h: e[3], d });
    }
    // In a slot but not (yet) in the feed: near, and the game has its car.
    for (const id of slots) {
      const p = people.get(id);
      if (p && !all.has(id)) all.set(id, { id, name: p.name, crew: p.crew, x: NaN, z: NaN, h: 0, d: 0 });
    }
    const list = [...all.values()];
    for (const p of list) {
      p.mate = !!mine && key(p.crew) === mine;
      p.rank = listed.has(p.id) ? p.d * KEEP : p.d;
    }
    list.sort((a, b) => b.mate - a.mate || (a.rank < b.rank ? -1 : a.rank > b.rank ? 1 : 0) || a.id - b.id);
    const out = list.slice(0, count);
    listed = new Set(out.map((p) => p.id));
    return out;
  }

  // Who is who and where, into the block, when it changes (and again whenever the game
  // has just made its block).
  function tell(force) {
    const at = shared();
    if (!at) return;
    const { ptr, layout } = at;
    const color = (c) => rgb(c && window.recompCrewColor ? window.recompCrewColor(c) : "");
    const shown = choose(layout.count, layout.places).map((p) => ({ ...p, color: (0xff000000 | color(p.crew)) >>> 0, name: latin1(p.name) }));
    const text = JSON.stringify(shown);
    if (text === told && !force) return;
    told = text;
    const { u8, u32, i32, f32 } = views, b = ptr >> 2;
    Atomics.add(i32, b + SEQ / 4, 1); // odd: being written
    for (let i = 0; i < layout.count; i++) {
      const base = ptr + FRIENDS + i * layout.size, w = base >> 2, p = shown[i];
      u32[w] = p ? p.id : 0;
      u32[w + 1] = p ? p.color : 0;
      u8.fill(0, base + 8, base + 8 + NAME);
      if (p) u8.set(p.name, base + 8);
      if (layout.places) {
        const rad = p ? (p.h * Math.PI) / 180 : 0;
        f32[w + 8] = p ? p.x : NaN;
        f32[w + 9] = p ? p.z : NaN;
        f32[w + 10] = Math.sin(rad);
        f32[w + 11] = Math.cos(rad);
      }
    }
    Atomics.add(i32, b + SEQ / 4, 1); // even again
  }

  // For tests and the console: whether the map is open (it counts its frames),
  // how many players it shows and whose name is up.
  let lastUpdates = 0, lastAt = -1e9;
  function watch() {
    const at = shared();
    if (!at) return null;
    const updates = views.u32[(at.ptr + UPDATES) >> 2];
    if (updates !== lastUpdates) { lastUpdates = updates; lastAt = performance.now(); }
    return at;
  }
  window.recompWorldMap = () => {
    const at = watch();
    if (!at) return null;
    const { u32 } = views;
    return { open: performance.now() - lastAt < 600, updates: lastUpdates, shown: u32[(at.ptr + at.layout.shown) >> 2],
             hover: u32[(at.ptr + at.layout.hover) >> 2], listed: [...listed] };
  };

  function wake() {
    if (active) return;
    active = true;
    addEventListener("recomp-online", () => tell(false));
    addEventListener("recomp-online-where", () => tell(false));
    addEventListener("carbon-worldmap", () => tell(true));
    tell(true);
    // A crew changing its colour (net.js recolours when crews come and go) does
    // not always come with a new state, and a feed that stops must give way to
    // the slots: look again now and then.
    setInterval(() => tell(false), 1000);
    setInterval(watch, 250);
  }
  addEventListener("recomp-online", wake);
  if (window.recompOnline || window.recompOnlineState) wake();
})();
