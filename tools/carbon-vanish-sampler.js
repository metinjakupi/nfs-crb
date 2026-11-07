// carbon-vanish-sampler.js - in-page sampler for tools/carbon-vanish-check.mjs (logs/no-vanish-20261106): vehicles that
// vanish or pop in while on screen. Evaluate it in a game tab; it runs every 50 ms until evaluated again.
//
// Every car of the game's car list (0x00a9e728) that is shown (physics on: PVehicle +0x160) is projected with the main
// view's view-projection: mp.h writes the view's matrix block to MpMore.spare (+0x18fc of the mp block, from the
// no-vanish build on; an older game leaves it 0 and nothing is counted: "noView"); the matrix is at +0x80, row vectors,
// render space = (z, -x, y) of physics space, the projection mp.h's name tags use. A car is ON SCREEN when it is in
// front (w > 0.5) and its centre is inside the picture (0..1 both ways).
//   VANISHED: on screen within VANISH m (globalThis.__nvRanges.vanish, default 950: inside the far radius) in one sample,
//             and in the next it is gone from the list, not shown (physics off: parked), another car at its address,
//             or farther than its speed explains (10 m + 3 x speed x dt: put somewhere else);
//   APPEARED: on screen within APPEAR m (default 150) now, and in the sample before not in the list, not shown, another
//             car, or jumped (as above). Both are also counted within 300 m ("near300") for comparison with older runs.
// Kinds: own traffic (driver class 1; 4 = trucks' trailers), own police (2), racers (3), and the class 6/7/8 cars the
// mod builds: "friend" when the model is one of globalThis.__nvFriendKeys (the other players' own models), "copy-police"
// when it is a police model (one seen as an own class-2 car, or in globalThis.__nvCopKeys), else "copy-traffic".
// Also: where every new car came from and where every car went (distance, on screen or not), and game frames that took
// longer than 100 ms (MpShared.frame not moving: "stalls").
// DRAW-ONLY CARS (mod drawonly.h, logs/drawonly-merge-20261106): with the mod's draw-only cars on (globalThis.carbonDrawOnly),
// every drawn render connection with one of the mod's world ids (+0x2c >= 0x7d000000; state 3, not hidden, a CarRenderInfo)
// is a "friend-l2" car at its WorldConn entry's place. A friend's full car and a friend-l2 car taking each other's place
// (the other level drawn within HANDOVER m of it in the same sample) is a hand-over, not a vanish or a pop: counted in
// handovers, not in vanished/appeared.
// globalThis.__nvOut() returns it all (the last 400 events). Needs the guest base in globalThis.__gb.
(() => {
  if (globalThis.__nv) clearInterval(globalThis.__nv.timer);
  const R = { vanish: 950, appear: 150, ...(globalThis.__nvRanges || {}) };
  const HANDOVER = 4;
  const T = { since: Date.now(), ranges: R, ticks: 0, noView: 0, vanished: {}, appeared: {}, handovers: { down: 0, up: 0 }, near300: { vanished: {}, appeared: {} },
              onScreen: {}, events: [], spawns: {}, gones: {}, stalls: 0, frames: 0, prev: new Map(), copKeys: new Set(globalThis.__nvCopKeys || []) };
  const add = (o, k) => { o[k] = (o[k] || 0) + 1; };
  let lastFrame = -1, lastFrameAt = 0;
  const tick = () => {
    const B = globalThis.__gb, mp = globalThis.carbonMpBlock; if (B === undefined || !mp) return;
    const m = new DataView(HEAPU8.buffer), r = (a) => m.getUint32(B + a, true) >>> 0, fl = (a) => m.getFloat32(B + a, true);
    const ok = (a) => a > 0x400000 && B + a + 0x100 < m.byteLength;
    const mu = new Uint32Array(mp.memory.buffer), now = performance.now();
    const frame = mu[(mp.ptr + 0x18) >> 2];
    let frameGap = 0; // how long the frame drawn now took to come: a stalled game moves its cars by that much at once
    if (frame !== lastFrame) { frameGap = lastFrame >= 0 ? (now - lastFrameAt) / 1000 : 0; if (lastFrame >= 0 && now - lastFrameAt > 100) T.stalls++; T.frames += frame - lastFrame > 0 && lastFrame >= 0 ? frame - lastFrame : 0; lastFrame = frame; lastFrameAt = now; }
    const view = mu[(mp.ptr + 0x18fc) >> 2] >>> 0;
    if (!view || !ok(view)) { T.noView++; return; }
    const M = []; for (let i = 0; i < 16; i++) M.push(fl(view + 0x80 + 4 * i));
    const project = (p) => {
      const v = [p[2], -p[0], p[1], 1], c = [0, 0, 0, 0];
      for (let j = 0; j < 4; j++) for (let i = 0; i < 4; i++) c[j] += v[i] * M[4 * i + j];
      const w = c[3] || 1e-6;
      return [(c[0] / w + 1) / 2, (1 - c[1] / w) / 2, c[3]];
    };
    const onScreen = (s) => s[2] > 0.5 && s[0] >= 0 && s[0] <= 1 && s[1] >= 0 && s[1] <= 1;
    const key = (c) => { let col = r(c + 0xd8); if (col && (r(c + 0xd8 + 0xc) & 1)) col = r(col + 0xc); return col ? r(col + 0x10) : 0; };
    const state = (c) => { const body = r(c + 0x70), s = body && ok(body) ? r(r(body + 0x30)) : 0; return s && ok(s) ? s : 0; };
    let me = null; const cur = new Map();
    const friends = new Set(globalThis.__nvFriendKeys || []);
    for (let n = r(0xa9e728), i = 0; n && n !== 0xa9e728 && i++ < 256; n = r(n)) {
      const c = n - 0xd0, cls = r(c + 0x148), s = state(c);
      if (!s) continue;
      const p = [fl(s + 0x10), fl(s + 0x14), fl(s + 0x18)], v = Math.hypot(fl(s + 0x20), fl(s + 0x24), fl(s + 0x28));
      if (!me && cls === 0 && r(c + 0x7c)) { me = p; continue; }
      if (!r(c + 0x160) || Math.abs(p[0]) + Math.abs(p[2]) < 5) continue;
      const k = key(c);
      if (cls === 2) T.copKeys.add(k);
      const kind = cls === 1 || cls === 4 ? "traffic" : cls === 2 ? "police" : cls === 3 ? "racer"
                 : cls >= 6 && cls <= 8 ? (friends.has(k) ? "friend" : T.copKeys.has(k) ? "copy-police" : "copy-traffic") : "class" + cls;
      cur.set(c, { p, v, kind, key: k, cls });
    }
    if (!me) return;
    if (globalThis.carbonDrawOnly) { // draw-only cars (friend-l2), keyed apart from the car list's addresses
      const list = r(0xb77db0 + 4), cnt = r(0xb77db0 + 0xc);
      for (let i = 0; list && i < cnt && i < 256; i++) {
        const c = r(list + 4 * i); if (!c || !ok(c) || r(c + 0x2c) < 0x7d000000) continue;
        const e = r(c + 0x30), ve = r(c + 0x34);
        if (r(c + 0x24) !== 3 || m.getUint8(B + c + 0x60) || !r(c + 0x40) || !e || !ok(e)) continue;
        const p = [-fl(e + 0x34), fl(e + 0x38), fl(e + 0x30)]; // render (z, -x, y) -> physics (x, y, z)
        const v = ve && ok(ve) ? Math.hypot(fl(ve), fl(ve + 4), fl(ve + 8)) : 0;
        cur.set("L2:" + c, { p, v, kind: "friend-l2", key: 0, cls: "L2" });
      }
    }
    T.ticks++;
    // this player's own car jumping (the test's driver, or the game putting it back on the road): the other games then see
    // its car jump too, which is not theirs to fix
    if (T.meAt && Math.hypot(me[0] - T.meAt[0], me[2] - T.meAt[2]) > 10 + 3 * 60 * Math.max((now - (T.prevAt || now)) / 1000 || 0.05, frameGap)) { T.selfJumps = (T.selfJumps || 0) + 1; (T.selfJumpAt ||= []).push(Date.now()); if (T.selfJumpAt.length > 500) T.selfJumpAt.shift(); }
    T.meAt = me;
    const look = (x) => { const d = Math.hypot(x.p[0] - me[0], x.p[1] - me[1], x.p[2] - me[2]), s = project(x.p); return { d, s, on: onScreen(s) }; };
    const dt = Math.max(T.prevAt ? (now - T.prevAt) / 1000 : 0.05, frameGap);
    const jumped = (a, b) => Math.hypot(a.p[0] - b.p[0], a.p[1] - b.p[1], a.p[2] - b.p[2]) > 10 + 3 * Math.max(a.v, b.v) * dt;
    const ev = (what, x, why, l) => {
      if (l.d <= 300) add(T.near300[what], x.kind);
      if (l.d > (what === "vanished" ? R.vanish : R.appear)) return;
      add(T[what], x.kind);
      T.events.push({ t: Date.now() - T.since, what, kind: x.kind, cls: x.cls, key: x.key.toString(16), d: Math.round(l.d), x: +l.s[0].toFixed(2), y: +l.s[1].toFixed(2), why });
      if (T.events.length > 400) T.events.shift();
    };
    const note = (o, x, l) => { (o[x.kind] ||= []).push([Math.round(l.d), l.on ? 1 : 0]); if (o[x.kind].length > 400) o[x.kind].shift(); };
    for (const [c, x] of cur) { x.look = look(x); if (x.look.on && x.look.d <= R.vanish) add(T.onScreen, x.kind); }
    // a friend's car whose other level is drawn within HANDOVER m (in map): a hand-over between full and draw-only
    const other = (x, map) => { if (x.kind !== "friend" && x.kind !== "friend-l2") return false; const want = x.kind === "friend" ? "friend-l2" : "friend";
      for (const y of map.values()) if (y.kind === want && Math.hypot(y.p[0] - x.p[0], y.p[1] - x.p[1], y.p[2] - x.p[2]) < HANDOVER + 3 * x.v * dt) return true; return false; };
    for (const [c, a] of T.prev) {
      const b = cur.get(c);
      if (!b) note(T.gones, a, a.look);
      if (!a.look.on) continue;
      if (!b && other(a, cur)) { T.handovers[a.kind === "friend" ? "down" : "up"]++; continue; }
      if (!b) ev("vanished", a, "gone or hidden", a.look);
      else if (b.cls !== a.cls) ev("vanished", a, "another car at its address", a.look);
      else if (jumped(a, b)) ev("vanished", a, "jumped", a.look);
    }
    for (const [c, b] of cur) {
      const a = T.prev.get(c);
      if (!a && T.ticks > 1) note(T.spawns, b, b.look);
      if (!b.look.on) continue;
      if (!a) { if (T.ticks > 1 && !other(b, T.prev)) ev("appeared", b, "new in the list", b.look); }
      else if (a.cls !== b.cls) ev("appeared", b, "another car at its address", b.look);
      else if (jumped(a, b)) ev("appeared", b, "jumped", b.look);
    }
    T.prev = cur; T.prevAt = now;
  };
  T.timer = setInterval(() => { try { tick(); } catch (e) { T.err = String(e); } }, 50);
  globalThis.__nv = T;
  globalThis.__nvOut = () => { const { timer, prev, copKeys, ...o } = globalThis.__nv; return { ...o, copKeys: [...copKeys].map((k) => k.toString(16)), ms: Date.now() - o.since }; };
  return "sampler on";
})()
