// radar.js - Need for Speed: Carbon's radar for free roam with others: a small
// round radar on the right, between the race standings and the speedometer, that
// turns with your car and shows where the other players are. The start page
// (start.js) loads it with menu.js and chat.js when the player presses Play. Like
// chat.js it wakes up only when the game goes online (window.recompOnline, or
// net.js's first "recomp-online" event: the player page sets recompOnline a
// moment after this file may have run) and does nothing offline.
//
// WHAT IT SHOWS. Up on the radar is where your car points; you are the arrow in
// the middle, and the ticks round the edge and the N (where the game's own minimap
// puts it) turn with the world as you steer. Each other player in the world is a
// dot in their crew's colour (net.js's window.recompCrewColor; light grey without
// a crew); your crewmates' dots are bigger and ringed. Distance from the middle
// grows with the square root of the real distance, so cars close by spread out
// instead of sitting on your arrow: the faint rings are 100 m and 250 m, the edge
// 500 m. A player further away sits on the edge as an arrow pointing their way.
// Under the radar: the nearest crewmate (or, with none in the world, the nearest
// player) and how far.
// It shows only while you drive (the game's shared block says so) and someone else
// is online. The game's pause menu leaves it up: the block can't tell a pause (the
// physics clock runs on, the pose stands still) from a car standing still, and the
// pause bar runs across the middle, clear of it. The F9 menu turns it on and off
// (localStorage nfsc.radar, on by default) through window.recompMenuToggles.
//
// WHERE THINGS ARE. The game's shared block (mods/core/nfsc/mp.h MpShared): this
// player's state at +0x10 (1 = driving), frame +0x18, then the car's pose at
// +0x20: rotation quaternion x, y, z, w, position (+0x30), speed (+0x3c), spin
// (+0x48), floats. Other players' cars are in the slots from +0x60 (0x100 each:
// id, seq, key, mode, count, 3 spare, then a ring of 4 poses {sender ms, the same
// 13 floats}; the newest is (count - 1) % 4), which net.js fills from the relay up
// to 30 times a second. Positions are the game's physics space in metres, y up, so
// the ground is x, z. The car's nose points along its body +z axis turned by the
// quaternion, and its right-hand side is (fz, -fx) for a nose (fx, fz) on the
// ground: both checked in the game (driving forward and turning, the ground speed
// lined up with +z at a cosine of 0.96, every other axis far below; each car whose
// name tag was right of the screen's middle came out right of the arrow, each one
// left of it left). North, the minimap's N, is -x.
"use strict";
(() => {
  const game = window.recompGame || { id: "nfsc" };
  const store = {
    get(key, fallback) {
      try { const v = localStorage.getItem(`${game.id}.${key}`); return v === null ? fallback : JSON.parse(v); }
      catch { return fallback; }
    },
    set(key, value) {
      try { localStorage.setItem(`${game.id}.${key}`, JSON.stringify(value)); } catch {}
    },
  };

  const MAGIC = 0x31504d43, SLOTS = 8, SLOT0 = 0x60, SLOT = 0x100, RING = 4, SNAP = 0x38;
  const RANGE = 500;        // metres at the edge; further players sit on it as arrows
  const RINGS = [100, 250]; // metres
  const NEUTRAL = "#dbe4ee", CYAN = "#5fe4f2";
  const TAU = Math.PI * 2;

  let active = false, enabled = store.get("radar", true) !== false;

  const css = `
  .cr { --s: 132px; --u: 13px; position: fixed; left: 0; top: 0; z-index: 4; width: var(--s); pointer-events: none;
        opacity: 0; transition: opacity .25s ease; }
  .cr.on { opacity: 1; }
  .cr canvas { display: block; width: var(--s); height: var(--s); }
  .cr-near { display: flex; justify-content: center; margin-top: calc(var(--u) * .3); white-space: nowrap; }
  .cr-near > span { max-width: calc(var(--s) + 40px); overflow: hidden; text-overflow: ellipsis; padding: .28em .75em;
        font: italic 700 var(--u)/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", system-ui, sans-serif;
        letter-spacing: .1em; text-transform: uppercase; color: #8fa6bd; background: rgba(6, 12, 22, .62);
        border: 1px solid rgba(95, 228, 242, .25); transform: skewX(-12deg); text-shadow: 0 1px 2px rgba(0, 0, 0, .8); }
  .cr-near b { display: inline-block; transform: skewX(12deg); font-weight: 800; color: #e9f6ff; }
  .cr-near i { display: inline-block; transform: skewX(12deg); font-style: inherit; margin-left: .45em; }
  .cr-near[hidden] { display: none; }
  @media (prefers-reduced-motion: reduce) { .cr { transition: none; } }
  `;

  let root, cv, ctx, near, nearName, nearDist, nearColor = "", gameCanvas;

  function wake() {
    if (active) return;
    active = true;
    root = document.createElement("div");
    root.className = "cr";
    root.setAttribute("aria-hidden", "true"); // the game's picture, in a corner: nothing to read out
    root.innerHTML = `<style>${css}</style><canvas></canvas><div class="cr-near" hidden><span><b></b><i></i></span></div>`;
    document.body.append(root);
    cv = root.querySelector("canvas");
    ctx = cv.getContext("2d");
    near = root.querySelector(".cr-near");
    nearName = near.querySelector("b");
    nearDist = near.querySelector("i");
    gameCanvas = document.getElementById("canvas");
    // The F9 menu's switch (menu.js lists window.recompMenuToggles).
    const toggles = (window.recompMenuToggles ||= []);
    const at = toggles.findIndex((t) => t && t.id === "radar");
    const toggle = { id: "radar", label: "Radar", get: () => enabled, set: (on) => { enabled = !!on; store.set("radar", enabled); } };
    if (at >= 0) toggles[at] = toggle;
    else toggles.push(toggle);
    dispatchEvent(new Event("recomp-menu-toggles"));
    requestAnimationFrame(frame);
  }

  // ---- The game's shared block ----
  let views = null;
  function shared() {
    const b = globalThis.carbonMpBlock;
    if (!b) return null;
    const buf = b.memory.buffer;
    if (!views || views.buf !== buf)
      views = { buf, u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
    return views.u32[b.ptr >> 2] === MAGIC ? b.ptr : null;
  }
  // This player's part, or the last whole one while the game is writing it.
  let mine = null;
  function own(ptr) {
    const { u32, i32, f32 } = views, b = ptr >> 2;
    const seq = Atomics.load(i32, b + 3);
    if (seq & 1) return mine;
    const o = { state: u32[b + 4], frame: u32[b + 6],
                q: [f32[b + 8], f32[b + 9], f32[b + 10], f32[b + 11]], p: [f32[b + 12], f32[b + 13], f32[b + 14]] };
    if (Atomics.load(i32, b + 3) === seq) mine = o;
    return mine;
  }
  // Other players' cars: [{id, at: [x, y, z]}] from the newest pose in each slot
  // whose player is driving. A slot net.js is writing just now keeps its last read.
  const seen = new Array(SLOTS).fill(null); // {id, count, at}
  function others(ptr) {
    const { u32, i32, f32 } = views, out = [];
    for (let i = 0; i < SLOTS; i++) {
      const base = ptr + SLOT0 + i * SLOT, b = base >> 2;
      const seq = Atomics.load(i32, b + 1);
      if (!(seq & 1)) {
        const id = u32[b], mode = u32[b + 3], count = u32[b + 4];
        let read = null;
        if (id && mode === 1 && count) {
          const p = (base + 0x20 + ((count - 1) % RING) * SNAP) >> 2;
          read = { id, count, at: [f32[p + 5], f32[p + 6], f32[p + 7]] };
        }
        if (Atomics.load(i32, b + 1) === seq) seen[i] = read;
      }
      const s = seen[i];
      if (s && s.at.every((x) => Number.isFinite(x) && Math.abs(x) < 1e5)) out.push(s);
    }
    return out;
  }

  // The car's nose on the ground, a unit (x, z): its body +z axis turned by the
  // rotation (x, y, z, w). null when it points straight up or down.
  function nose(q) {
    const [x, y, z, w] = q;
    const fx = 2 * (x * z + w * y), fz = 1 - 2 * (x * x + y * y), len = Math.hypot(fx, fz);
    return len > 1e-3 && Number.isFinite(len) ? [fx / len, fz / len] : null;
  }

  // ---- Who is who ----
  const key = (c) => (window.recompCrewKey ? window.recompCrewKey(c) : String(c || "").toLowerCase().replace(/\s+/g, ""));
  const color = (c) => (c && window.recompCrewColor?.(c)) || "";
  let peopleOf = null, people = new Map();
  function person(id) {
    const s = window.recompOnlineState;
    if (s !== peopleOf) {
      peopleOf = s;
      people = new Map((s?.people || []).map((p) => [p.id, p]));
    }
    return people.get(id);
  }
  const myCrew = () => window.recompOnlineState?.crew ?? window.recompOnline?.crew ?? "";
  const someoneElse = () => {
    const s = window.recompOnlineState;
    return !!s && !!s.open && (s.people || s.names || []).length > 0;
  };

  // ---- Where it goes ----
  // On the right, scaled with the game's height as the game's own HUD is: below the
  // race standings (they end near 42% of the height) and the perf overlay, above the
  // nitrous and speedbreaker bars (from 74%).
  let size = 0, dpr = 0, placedFor = "";
  function place() {
    const box = gameCanvas?.getBoundingClientRect() || { left: 0, top: 0, width: innerWidth, height: innerHeight };
    const h = box.height || innerHeight, w = box.width || innerWidth, ratio = devicePixelRatio || 1;
    const k = `${box.left},${box.top},${w},${h},${ratio}`;
    if (k === placedFor) return false;
    placedFor = k;
    size = Math.round(Math.max(96, Math.min(230, h * 0.19)));
    dpr = ratio;
    cv.width = cv.height = Math.round(size * dpr);
    root.style.setProperty("--s", `${size}px`);
    root.style.setProperty("--u", `${Math.max(11, Math.round(h * 0.018))}px`);
    root.style.left = `${Math.round(box.left + w - size - Math.max(14, h * 0.035))}px`;
    root.style.top = `${Math.round(box.top + h * 0.585 - size / 2)}px`;
    return true;
  }

  // ---- Drawing ----
  // A player's place on the radar: right of you and ahead of you, in metres.
  function relative(me, f, at) {
    const dx = at[0] - me[0], dz = at[2] - me[2];
    return { side: dx * f[1] - dz * f[0], ahead: dx * f[0] + dz * f[1] };
  }
  const reach = (d) => Math.sqrt(Math.min(d, RANGE) / RANGE); // 0..1 of the radius

  function draw(me, f, list) {
    const s = size, c = s / 2, u = s / 140, R = c - 3 * u, inner = R - 4 * u;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, s, s);
    ctx.lineCap = "round";
    ctx.lineJoin = "round";

    // The disc, lighter in the middle.
    const g = ctx.createRadialGradient(c, c, 0, c, c, R);
    g.addColorStop(0, "rgba(10, 26, 42, .5)");
    g.addColorStop(1, "rgba(4, 10, 20, .8)");
    ctx.fillStyle = g;
    ctx.beginPath();
    ctx.arc(c, c, R, 0, TAU);
    ctx.fill();

    // What you are looking at: a soft wedge ahead.
    const cone = ctx.createRadialGradient(c, c, 0, c, c, R);
    cone.addColorStop(0, "rgba(95, 228, 242, .2)");
    cone.addColorStop(1, "rgba(95, 228, 242, 0)");
    ctx.fillStyle = cone;
    ctx.beginPath();
    ctx.moveTo(c, c);
    ctx.arc(c, c, R, -Math.PI / 2 - 0.6, -Math.PI / 2 + 0.6);
    ctx.closePath();
    ctx.fill();

    // Range rings.
    ctx.strokeStyle = "rgba(95, 228, 242, .2)";
    ctx.lineWidth = Math.max(1, u * 0.8);
    ctx.setLineDash([2 * u, 3 * u]);
    for (const m of RINGS) {
      ctx.beginPath();
      ctx.arc(c, c, inner * reach(m), 0, TAU);
      ctx.stroke();
    }
    ctx.setLineDash([]);

    // Ticks fixed to the world, every 30 degrees, longer every 90.
    for (let n = 0; n < 12; n++) {
      const a = (n * TAU) / 12, r = relative([0, 0, 0], f, [Math.sin(a), 0, Math.cos(a)]);
      const x = r.side, y = -r.ahead, big = n % 3 === 0;
      ctx.strokeStyle = big ? "rgba(95, 228, 242, .75)" : "rgba(95, 228, 242, .32)";
      ctx.lineWidth = Math.max(1, u * (big ? 1.6 : 1));
      ctx.beginPath();
      ctx.moveTo(c + x * (R - (big ? 7 : 4.5) * u), c + y * (R - (big ? 7 : 4.5) * u));
      ctx.lineTo(c + x * (R - 1.5 * u), c + y * (R - 1.5 * u));
      ctx.stroke();
    }

    // North.
    const n = relative([0, 0, 0], f, [-1, 0, 0]), nr = R - 13 * u;
    ctx.font = `italic 800 ${Math.round(9.5 * u)}px "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "#9fb6cc";
    ctx.fillText("N", c + n.side * nr, c - n.ahead * nr);

    // The rim, with the HUD's cyan glow.
    ctx.save();
    ctx.shadowColor = "rgba(95, 228, 242, .7)";
    ctx.shadowBlur = 5 * u;
    ctx.strokeStyle = CYAN;
    ctx.lineWidth = Math.max(1.5, 1.8 * u);
    ctx.beginPath();
    ctx.arc(c, c, R, 0, TAU);
    ctx.stroke();
    ctx.restore();

    // The other players: strangers first, crewmates on top, the nearest last.
    const crew = key(myCrew());
    const marks = list.map((o) => {
      const p = person(o.id), r = relative(me, f, o.at), d = Math.hypot(r.side, r.ahead);
      return { ...r, d, name: p?.name || "Player", color: color(p?.crew) || NEUTRAL, mate: !!crew && key(p?.crew) === crew };
    });
    marks.sort((a, b) => a.mate - b.mate || b.d - a.d);
    for (const m of marks) {
      const dirX = m.d > 0.01 ? m.side / m.d : 0, dirY = m.d > 0.01 ? -m.ahead / m.d : 0;
      ctx.fillStyle = m.color;
      ctx.strokeStyle = "rgba(0, 0, 0, .7)";
      ctx.lineWidth = Math.max(1, u);
      if (m.d > RANGE) {
        // On the edge, pointing their way.
        const tip = R - 2.5 * u, back = R - (m.mate ? 11 : 9) * u, half = (m.mate ? 5 : 4) * u;
        ctx.beginPath();
        ctx.moveTo(c + dirX * tip, c + dirY * tip);
        ctx.lineTo(c + dirX * back - dirY * half, c + dirY * back + dirX * half);
        ctx.lineTo(c + dirX * back + dirY * half, c + dirY * back - dirX * half);
        ctx.closePath();
        ctx.stroke();
        ctx.fill();
        continue;
      }
      const r = inner * reach(m.d), x = c + dirX * r, y = c + dirY * r;
      if (m.mate) {
        ctx.save();
        ctx.shadowColor = m.color;
        ctx.shadowBlur = 6 * u;
        ctx.strokeStyle = m.color;
        ctx.lineWidth = Math.max(1.2, 1.5 * u);
        ctx.beginPath();
        ctx.arc(x, y, 7 * u, 0, TAU);
        ctx.stroke();
        ctx.restore();
      }
      ctx.beginPath();
      ctx.arc(x, y, (m.mate ? 4.2 : 3.3) * u, 0, TAU);
      ctx.stroke();
      ctx.fill();
    }

    // You.
    ctx.fillStyle = CYAN;
    ctx.strokeStyle = "rgba(2, 8, 16, .9)";
    ctx.lineWidth = Math.max(1, 1.2 * u);
    ctx.beginPath();
    ctx.moveTo(c, c - 7.5 * u);
    ctx.lineTo(c + 5.5 * u, c + 5.5 * u);
    ctx.lineTo(c, c + 2.5 * u);
    ctx.lineTo(c - 5.5 * u, c + 5.5 * u);
    ctx.closePath();
    ctx.stroke();
    ctx.fill();

    // The nearest crewmate, else the nearest player.
    let pick = null;
    for (const m of marks) if (!pick || m.mate > pick.mate || (m.mate === pick.mate && m.d < pick.d)) pick = m;
    near.hidden = !pick;
    if (pick) {
      const dist = `· ${pick.d < 1000 ? `${Math.round(pick.d)} m` : `${(pick.d / 1000).toFixed(1)} km`}`;
      if (nearName.textContent !== pick.name) nearName.textContent = pick.name;
      if (nearColor !== pick.color) nearName.style.color = (nearColor = pick.color) === NEUTRAL ? "" : pick.color;
      if (nearDist.textContent !== dist) nearDist.textContent = dist;
    }
  }

  // ---- Every frame ----
  let shown = false, drawn = "", told = 0, heading = null;
  function frame() {
    requestAnimationFrame(frame);
    const ptr = shared();
    const o = ptr === null ? null : own(ptr);
    // A car pointing straight up or down for a moment keeps the heading it had.
    if (o && o.state === 1) heading = nose(o.q) || heading;
    const f = o && o.state === 1 ? heading : null;
    const show = enabled && !!f && someoneElse();
    if (show !== shown) {
      shown = show;
      root.classList.toggle("on", show);
    }
    if (!show) return;
    const moved = place();
    const list = others(ptr);
    // Drawn again only when something changed: this car, another, the crews, the size.
    const sig = `${o.frame}|${list.map((s) => s.count).join(",")}|${told}|${enabled}`;
    if (!moved && sig === drawn) return;
    drawn = sig;
    draw(o.p, f, list);
  }

  addEventListener("recomp-online", () => { ++told; wake(); });
  if (window.recompOnline || window.recompOnlineState) wake();
})();
