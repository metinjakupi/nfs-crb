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
// players at about the same distance. The feed may also name players in free
// roam who are not driving this second (a relay with "Where, version 2": a fifth
// number, the state: 1 driving, 2 paused, 3 away; no fifth number = driving).
// The arrows go to the drivers first. With the page's markers (version 3 below)
// they go to drivers only, and every other player is a dimmed, hollow marker in
// their crew's colour; an older game (version 2, arrows only) gives arrows left
// over to the others. Players whose car is in one of net.js's 8
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
//
// EVERYONE ELSE. Version 3 (still magic "WMP2", version word +0x04 = 3; the first
// 0x200 bytes are version 2's) adds the map's view at +0x200 (WmView in worldmap.h,
// its own magic "WMV3" +0x00 and sequence number +0x04, odd while the game writes;
// frame +0x08 = the map frame count it was written at; flags +0x0c: 1 the player's
// arrow is on the map, 2 another menu is up; +0x10 six floats taking physics x, z to
// the game's FE screen units, fe_w x fe_h +0x28/+0x2c being the whole picture; FE
// scale +0x30; zoom +0x34; the player's arrow size +0x38; the map cursor +0x3c/+0x40
// (NaN: none), shown +0x44; check +0x48; the players with arrows: count +0x54, ids
// +0x58 (10); covers: count +0x80, rectangles x0, y0, x1, y1 from +0x90 (48)). While
// the map shows with the player's arrow, this page draws every other player the feed
// knows - everyone without one of the 10 arrows - as a small marker in their crew's
// colour pointing the way they drive, on a canvas laid over the game's, with the
// game's own menus (covers) cut out of it, and the name of the one under the map's
// cursor (mouse, keys or controller: the game moves the cursor). An older game
// (version 2) gets the 10 arrows only, as before. Debug: globalThis.recompWorldMapDebug
// = true also draws markers for the players with arrows (to line them up), the covers
// and the cursor.
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

  // A feed entry's state: 1 driving (an entry without one too), 2 paused, 3 away.
  const STATE_DRIVING = 1;
  const stateOf = (e) => (e.length >= 5 && Number.isInteger(e[4]) ? e[4] : STATE_DRIVING);

  // Who to show, at most `count`, best first: [{id, name, crew, x, z, h}] (x NaN: the game knows where).
  // markers: the page draws everyone else (version 3), so the arrows are for drivers only.
  function choose(count, places, markers = false) {
    const s = window.recompOnlineState;
    const people = new Map((s?.people || []).filter((p) => p && p.id).map((p) => [p.id >>> 0, p]));
    const slots = (s?.slotIds || []).filter((id) => id).map((id) => id >>> 0);
    if (!places) return [...people.values()].slice(0, count).map((p) => ({ id: p.id >>> 0, name: p.name, crew: p.crew }));
    const feed = window.recompOnlineWhere;
    const fresh = feed && Array.isArray(feed.p) && performance.now() - feed.at <= FEED_FRESH;
    const me = window.recompOnlineSelf?.() || null;
    const mine = s?.crewId;
    const all = new Map();
    for (const e of fresh ? feed.p : []) {
      const id = e[0] >>> 0, p = people.get(id);
      if (!p) continue; // not (or no longer) in the room as far as net.js knows
      const d = me ? Math.hypot(e[1] - me[0], e[2] - me[2]) : Infinity;
      const driving = stateOf(e) === STATE_DRIVING;
      if (!driving && markers) continue; // a dimmed marker of the page's own
      all.set(id, { id, name: p.name, crew: p.crew, crewId: p.crewId, x: e[1], z: e[2], h: e[3], d, driving });
    }
    // In a slot but not (yet) in the feed: near, and the game has its car.
    for (const id of slots) {
      const p = people.get(id);
      if (p && !all.has(id) && !(markers && fresh && feed.p.some((e) => e[0] >>> 0 === id)))
        all.set(id, { id, name: p.name, crew: p.crew, crewId: p.crewId, x: NaN, z: NaN, h: 0, d: 0, driving: true });
    }
    const list = [...all.values()];
    for (const p of list) {
      p.mate = !!mine && p.crewId === mine;
      p.rank = listed.has(p.id) ? p.d * KEEP : p.d;
    }
    list.sort((a, b) => b.driving - a.driving || b.mate - a.mate || (a.rank < b.rank ? -1 : a.rank > b.rank ? 1 : 0) || a.id - b.id);
    const out = list.slice(0, count).map(({ driving, ...p }) => p);
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
    const markers = layout.places && views.u32[(ptr + 4) >> 2] >= 3; // the page draws the others (version 3)
    const shown = choose(layout.count, layout.places, markers).map((p) => ({ ...p, color: (0xff000000 | color(p.crew)) >>> 0, name: latin1(p.name) }));
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
  // Page overlays (name tags, radar) step aside while the map is up.
  window.recompWorldMapOpen = () => { watch(); return performance.now() - lastAt < 600; };
  window.recompWorldMap = () => {
    const at = watch();
    if (!at) return null;
    const { u32 } = views;
    return { open: performance.now() - lastAt < 600, updates: lastUpdates, shown: u32[(at.ptr + at.layout.shown) >> 2],
             hover: u32[(at.ptr + at.layout.hover) >> 2], listed: [...listed], overlay: overlayStats() };
  };

  // ---- Everyone else: the page's markers (version 3) ----
  const VIEW = 0x200, VIEW_MAGIC = 0x33564d57, V_PLAYER = 1, V_MENU = 2, COVERS = 48, ARROWS = 10;
  const REACH = 14;       // FE units: how near the cursor must be to a marker for its name (worldmap.h WM_REACH)
  const STALE_MS = 200;   // the map's frame count standing still this long: it has closed
  let ov = null;          // {cv, ctx, shown, cssW, cssH, dpr}
  let seenUpdates = -1, seenAt = -1e9;
  const stats = { frames: 0, drawn: 0, dimmed: 0, ms: 0, msMax: 0, covers: 0, label: "", hidden: "" };
  function overlayStats() { return { ...stats, ms: +stats.ms.toFixed(3), msMax: +stats.msMax.toFixed(3) }; }
  window.recompWorldMapResetStats = () => { stats.ms = 0; stats.msMax = 0; }; // for measurements

  // The view, whole (the game is not writing it just now), or null.
  function readView(ptr) {
    const { u32, f32 } = views, b = (ptr + VIEW) >> 2;
    if (u32[(ptr + 4) >> 2] < 3 || u32[b] !== VIEW_MAGIC) return null;
    for (let tries = 0; tries < 3; tries++) {
      const seq = Atomics.load(views.i32, b + 1);
      if (seq & 1) continue;
      const v = {
        frame: u32[b + 2], flags: u32[b + 3], m: Array.from(f32.subarray(b + 4, b + 10)),
        feW: f32[b + 10], feH: f32[b + 11], feScale: f32[b + 12], zoom: f32[b + 13], arrow: f32[b + 14],
        cx: f32[b + 15], cy: f32[b + 16], cursorShown: u32[b + 17], check: [f32[b + 18], f32[b + 19]],
        us: u32[b + 20], shown: new Set(Array.from(u32.subarray(b + 22, b + 22 + Math.min(u32[b + 21], ARROWS)))),
        covers: [],
      };
      const n = Math.min(u32[b + 32], COVERS);
      for (let i = 0; i < n; i++) v.covers.push(Array.from(f32.subarray(b + 36 + 4 * i, b + 40 + 4 * i)));
      if (Atomics.load(views.i32, b + 1) === seq) return v;
    }
    return null;
  }

  function ensureOverlay() {
    if (ov) return ov;
    const cv = document.createElement("canvas");
    cv.id = "recomp-worldmap-players";
    // Over the game's picture, under the page's own overlays (radar 4, chat 7) and menus; never takes input.
    cv.style.cssText = "position:fixed;left:0;top:0;pointer-events:none;z-index:3;display:none";
    document.body.append(cv);
    ov = { cv, ctx: cv.getContext("2d"), shown: false, cssW: 0, cssH: 0, dpr: 0 };
    return ov;
  }
  function hideOverlay(why) {
    stats.hidden = why;
    if (ov && ov.shown) { ov.shown = false; ov.cv.style.display = "none"; }
  }

  const cssColor = (c) => { const n = rgb(c && window.recompCrewColor ? window.recompCrewColor(c) : ""); return "#" + n.toString(16).padStart(6, "0"); };

  function drawOverlay() {
    const at = shared();
    if (!at || !at.layout.places) return hideOverlay("no block");
    const { u32 } = views, ptr = at.ptr, now = performance.now();
    const updates = u32[(ptr + UPDATES) >> 2];
    if (updates !== seenUpdates) { seenUpdates = updates; seenAt = now; }
    if (now - seenAt > STALE_MS) return hideOverlay("map closed");
    const v = readView(ptr);
    if (!v) return hideOverlay("no view (older game)");
    if (updates - v.frame > 2) return hideOverlay("view stale");
    if (!(v.flags & V_PLAYER)) return hideOverlay("no player arrow");
    if (v.flags & V_MENU) return hideOverlay("menu up");
    if (!(v.feW > 0 && v.feH > 0)) return hideOverlay("no size");
    if (globalThis.recompWorldMapOverlay === false) return hideOverlay("turned off"); // for A/B measurements
    const feed = window.recompOnlineWhere;
    if (!feed || !Array.isArray(feed.p) || now - feed.at > FEED_FRESH) return hideOverlay("no feed");
    const game = document.getElementById("canvas");
    const box = game ? game.getBoundingClientRect() : { left: 0, top: 0, width: innerWidth, height: innerHeight };
    if (!(box.width > 0 && box.height > 0)) return hideOverlay("no canvas");
    const t0 = performance.now();
    const o = ensureOverlay(), ctx = o.ctx, dpr = window.devicePixelRatio || 1;
    if (o.cssW !== box.width || o.cssH !== box.height || o.dpr !== dpr) {
      o.cssW = box.width; o.cssH = box.height; o.dpr = dpr;
      o.cv.width = Math.round(box.width * dpr); o.cv.height = Math.round(box.height * dpr);
      o.cv.style.width = box.width + "px"; o.cv.style.height = box.height + "px";
    }
    o.cv.style.transform = `translate(${box.left}px, ${box.top}px)`;
    if (!o.shown) { o.shown = true; o.cv.style.display = "block"; }
    stats.hidden = "";
    // FE units -> canvas pixels. A menu size other than 100% scales the FE about the picture's middle (open: not
    // checked on screen).
    const s = v.feScale > 0 && Number.isFinite(v.feScale) ? v.feScale : 1;
    const kx = (o.cv.width / v.feW) * s, ky = (o.cv.height / v.feH) * s;
    const ox = (o.cv.width * (1 - s)) / 2, oy = (o.cv.height * (1 - s)) / 2;
    const [m0, m1, m2, m3, m4, m5] = v.m;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, o.cv.width, o.cv.height);

    const st = window.recompOnlineState;
    const people = new Map((st?.people || []).filter((p) => p && p.id).map((p) => [p.id >>> 0, p]));
    const debug = !!globalThis.recompWorldMapDebug;
    // Marker size: about two thirds of the game's own car arrows (the player's arrow, which the game sizes with the
    // zoom): clearly a car, but a crowded map stays readable.
    const arrow = Number.isFinite(v.arrow) && v.arrow > 0 ? v.arrow : 30;
    const L = Math.max(10 * dpr, Math.min(30 * dpr, arrow * 0.65 * kx));
    const W = L * 0.36, margin = L + 2;
    const byColor = new Map(), dimByColor = new Map(), placed = [], debugMarks = [];
    const W_ = o.cv.width, H_ = o.cv.height;
    for (const e of feed.p) {
      const id = e[0] >>> 0, p = people.get(id);
      if (!p || (v.shown.has(id) && !debug)) continue;
      const fx = m0 * e[1] + m1 * e[2] + m2, fy = m3 * e[1] + m4 * e[2] + m5;
      const x = ox + fx * kx, y = oy + fy * ky;
      if (!(x > -margin && x < W_ + margin && y > -margin && y < H_ + margin)) continue;
      // The way the nose points (physics: degrees from +z toward +x), through the same transform.
      const r = (e[3] * Math.PI) / 180, sx = Math.sin(r), sz = Math.cos(r);
      let dx = (m0 * sx + m1 * sz) * kx, dy = (m3 * sx + m4 * sz) * ky;
      const len = Math.hypot(dx, dy) || 1; dx /= len; dy /= len;
      if (v.shown.has(id)) { // debug only: where this page would put a player the game gives an arrow
        debugMarks.push([x, y, dx, dy]);
        continue;
      }
      // Not driving (paused or away): the same arrow, hollow and dimmed, in the crew's colour.
      const color = cssColor(p.crew), state = e.length >= 5 && Number.isInteger(e[4]) ? e[4] : 1, paths = state === 1 ? byColor : dimByColor;
      let path = paths.get(color);
      if (!path) paths.set(color, (path = new Path2D()));
      const nx = -dy, ny = dx;
      path.moveTo(x + dx * L * 0.62, y + dy * L * 0.62);
      path.lineTo(x - dx * L * 0.38 + nx * W, y - dy * L * 0.38 + ny * W);
      path.lineTo(x - dx * L * 0.16, y - dy * L * 0.16);
      path.lineTo(x - dx * L * 0.38 - nx * W, y - dy * L * 0.38 - ny * W);
      path.closePath();
      placed.push({ id, x, y, fx, fy, name: p.name, state });
    }
    ctx.lineJoin = "round";
    // The others first, so a driver's arrow stays on top: a dark see-through inside, a dark rim, the crew's colour on it.
    for (const [color, path] of dimByColor) {
      ctx.globalAlpha = 0.45; ctx.fillStyle = "#000"; ctx.fill(path);
      ctx.globalAlpha = 0.6; ctx.lineWidth = Math.max(2, 3 * dpr); ctx.strokeStyle = "#000"; ctx.stroke(path);
      ctx.globalAlpha = 0.8; ctx.lineWidth = Math.max(1, 1.5 * dpr); ctx.strokeStyle = color; ctx.stroke(path);
    }
    ctx.globalAlpha = 1;
    ctx.lineWidth = Math.max(1, 1.25 * dpr);
    ctx.strokeStyle = "rgba(0,0,0,0.85)";
    for (const [color, path] of byColor) { ctx.fillStyle = color; ctx.fill(path); ctx.stroke(path); }
    // The game's own menus over the map: cut them out (the markers are under them, as the game's icons are).
    const covered = (fx, fy) => v.covers.some((c) => fx >= c[0] && fx <= c[2] && fy >= c[1] && fy <= c[3]);
    if (v.covers.length) {
      ctx.globalCompositeOperation = "destination-out";
      ctx.fillStyle = "#000";
      for (const c of v.covers) ctx.fillRect(ox + c[0] * kx, oy + c[1] * ky, (c[2] - c[0]) * kx, (c[3] - c[1]) * ky);
      ctx.globalCompositeOperation = "source-over";
    }
    // The name of the marker under the map's cursor, unless the game names one of its arrows there.
    let label = "";
    const hover = u32[(ptr + at.layout.hover) >> 2];
    if (Number.isFinite(v.cx) && Number.isFinite(v.cy) && !hover) {
      let best = null, bestD = REACH * REACH;
      for (const q of placed) {
        const d = (q.fx - v.cx) ** 2 + (q.fy - v.cy) ** 2;
        if (d < bestD && !covered(q.fx, q.fy)) { bestD = d; best = q; }
      }
      if (best) {
        // Like the game's label (worldmap.h wm_label: white, about 14 FE units high, a soft shadow), but above the
        // map cursor's ring, which stays wide over a marker (the game's cursor shrinks only onto its own items):
        // 36 FE units above the marker.
        label = String(best.name || "") + (best.state === 2 ? " · paused" : best.state === 3 ? " · away" : "");
        const size = Math.max(10 * dpr, 14.5 * ky);
        ctx.font = `600 ${size}px 'Avenir Next Condensed','DIN Condensed','Bahnschrift','Arial Narrow',system-ui,sans-serif`;
        ctx.textAlign = "center"; ctx.textBaseline = "middle";
        const half = ctx.measureText(label).width / 2 + 2 * dpr;
        const tx = Math.min(Math.max(best.x, half), W_ - half);
        const ty = Math.max(best.y - 36 * ky, size / 2 + 2 * dpr);
        ctx.shadowColor = "rgba(0,0,0,0.9)"; ctx.shadowBlur = 3 * dpr; ctx.shadowOffsetY = dpr;
        ctx.fillStyle = "#fff";
        ctx.fillText(label, tx, ty);
        ctx.shadowColor = "transparent"; ctx.shadowBlur = 0; ctx.shadowOffsetY = 0;
      }
    }
    if (debug) {
      ctx.lineWidth = dpr; ctx.strokeStyle = "#0f0";
      for (const c of v.covers) ctx.strokeRect(ox + c[0] * kx, oy + c[1] * ky, (c[2] - c[0]) * kx, (c[3] - c[1]) * ky);
      if (Number.isFinite(v.cx)) { const x = ox + v.cx * kx, y = oy + v.cy * ky; ctx.strokeStyle = "#ff0"; ctx.strokeRect(x - 3, y - 3, 6, 6); }
      ctx.strokeStyle = "#ff00ff";
      for (const [x, y, dx, dy] of debugMarks) {
        ctx.beginPath(); ctx.moveTo(x - 4 * dpr, y); ctx.lineTo(x + 4 * dpr, y); ctx.moveTo(x, y - 4 * dpr); ctx.lineTo(x, y + 4 * dpr);
        ctx.moveTo(x, y); ctx.lineTo(x + dx * 16 * dpr, y + dy * 16 * dpr); ctx.stroke();
      }
    }
    const ms = performance.now() - t0;
    stats.frames++; stats.drawn = placed.length; stats.dimmed = placed.filter((q) => q.state !== 1).length; stats.covers = v.covers.length; stats.label = label;
    stats.ms = stats.ms ? stats.ms * 0.95 + ms * 0.05 : ms; stats.msMax = Math.max(stats.msMax, ms);
    stats.view = { zoom: v.zoom, check: v.check, us: v.us, shown: [...v.shown], cursor: [v.cx, v.cy], cursorShown: v.cursorShown, m: v.m, feW: v.feW };
  }
  function overlayLoop() {
    requestAnimationFrame(overlayLoop);
    try { drawOverlay(); } catch (e) { hideOverlay("error " + e.message); }
  }

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
    requestAnimationFrame(overlayLoop);
  }
  addEventListener("recomp-online", wake);
  if (window.recompOnline || window.recompOnlineState) wake();
})();
