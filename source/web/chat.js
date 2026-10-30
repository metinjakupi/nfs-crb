// chat.js - Need for Speed: Carbon's chat and horn, for free roam with
// others. The start page (start.js) loads it with the in-game menu (menu.js)
// when the player presses Play. It wakes up only when the game goes online
// (window.recompOnline, or net.js's first "recomp-online" event: the player
// page sets recompOnline a moment after this file may have run) and does
// nothing offline.
//
// Y opens typed chat (120 characters). Messages use
// the relay's existing chat rate limit and room/nearby routing. Received text
// is displayed with textContent, never interpreted as HTML.
//
// THE HORN (H, held) is made here with Web Audio, no sound files: two buzzy
// tones a major third apart, like a car's twin horns. You hear your own; other
// players hear it louder the closer they are (silent beyond 150 m), panned to
// where the car is on their screen. A horn stops by itself after 4 s, for
// everyone, so a lost "off" cannot leave one sounding.
//
// KEYS. NFS Carbon's default keyboard controls (the bindings template in
// NFSC.exe at 0xa61c88, see mods/core/nfsc/pad.h) put Next song on T and bind
// Y and H to nothing, so chat is on Y and the horn on H. But a player
// can bind any key on Options -> Controls, and a key bound there must stay the
// game's. So the game says which letters its bindings use (MpShared.keys, see
// mp.h; 0 from a mod too old to say, which keeps Y and H) and each feature
// takes the first free key of its own list: chat Y U J K, the horn
// H B N V. All eight are free in the default controls, in the middle of the
// keyboard, where the left hand reaches them from gas, brake, Speedbreaker and
// handbrake (A Z X Space) while the right stays on the arrows; the horn's keep
// to the home and bottom rows, as it is held while driving; and the lists
// share no key, so the two never land on one. A remap while playing moves them
// within a tenth of a second, and the hint shows the new keys again. With all
// four of a list bound, that feature has no key and the hint says which to
// free. Keys are matched by position (KeyboardEvent.code), as the game binds
// them, and hints name them by what the player's keyboard layout prints there
// (Z on a German keyboard). They work only while the player is in a car (the
// game's shared block says so), so the game's own name entry still gets every
// letter, and never while the F9 menu is open. A window capture listener eats
// the presses it uses before the game sees them (SDL3 listens on the window in
// the bubble phase); key-ups always pass, as in menu.js.
"use strict";
(() => {
  const cleanText = (text) => typeof text === "string" ? text.replace(/\p{Cc}/gu, " ").trim().slice(0, 120) : "";
  const CHAT_KEYS = ["KeyY", "KeyU", "KeyJ", "KeyK"]; // first free one wins (KEYS above)
  const HORN_KEYS = ["KeyH", "KeyB", "KeyN", "KeyV"];
  const SHOW_MS = 8000;      // a line in the feed stays this long, then fades
  const MAX_LINES = 2; // temporary hints, separate from conversation
  const HISTORY_LIMIT = 200, RECENT_LIMIT = 5;
  const HORN_MAX_MS = 4000;
  const HEAR_M = 150;        // other players' horns are silent beyond this
  // The relay's limits (LIMITS in tools/carbon-net-relay.mjs), kept a little
  // slower so network jitter never makes a message arrive early and be dropped.
  const CHAT_LIMIT = { burst: 5, every: 2200 };
  const HORN_LIMIT = { burst: 12, every: 190 };
  // The game's shared block (mods/core/nfsc/mp.h MpShared): state at +0x10
  // (1 = driving a car); keys at +0x58 (bit 31: filled in; bit n: the letter
  // A + n is bound in the game's keyboard controls); slots from +0x60, 0x100
  // each {id, seq, key, mode, count, 3 spare, then a ring of 4 poses of
  // {sender ms, 13 floats}, the position at floats 4-6} as net.js writes them;
  // then {x, y, depth, spare} per slot, where the game shows that car on
  // screen (depth 0: not in view).
  const MAGIC = 0x31504d43, SLOTS = 8, SLOT0 = 0x60, SLOT = 0x100, RING = 4, SNAP = 0x38;
  const LABELS = SLOT0 + SLOTS * SLOT, KEYS = 0x58, KEYS_KNOWN = 0x80000000;

  let active = false, activeAt = 0;
  // The keys in use now (null: none free), and the keys word they came from.
  let chatKey = CHAT_KEYS[0], hornKey = HORN_KEYS[0], keysWord = 0;

  const css = `
  .cc { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --line: rgba(95, 228, 242, .22);
        --u: clamp(12px, 1.9vh, 18px); --card: min(calc(var(--u) * 20), calc(100vw - 32px));
        font: 600 var(--u)/1.3 "Avenir Next", "Segoe UI", system-ui, sans-serif; color: var(--ink); }
  .cc * { box-sizing: border-box; }
  .cc-feed { position: fixed; left: 16px; bottom: 42vh; z-index: 7; display: flex; flex-direction: column; align-items: flex-start;
        gap: calc(var(--u) * .3); max-width: min(calc(var(--u) * 30), calc(100vw - 32px)); pointer-events: none;
        transition: transform .18s ease-out, opacity .18s ease-out; }
  .cc-line { max-width: 100%; padding: calc(var(--u) * .3) calc(var(--u) * .8) calc(var(--u) * .3) calc(var(--u) * .65);
        background: linear-gradient(90deg, rgba(6, 12, 22, .8), rgba(6, 12, 22, .5)); border-left: 2px solid var(--cyan);
        transform: skewX(-12deg); text-shadow: 0 1px 2px rgba(0, 0, 0, .8); transition: opacity .6s ease; animation: cc-in .18s ease-out; }
  .cc-line > span { display: block; transform: skewX(12deg); overflow: hidden; text-overflow: ellipsis; white-space: normal; overflow-wrap: anywhere; }
  .cc-line.mine { border-left-color: var(--ink); }
  .cc-line.note { border-left-color: transparent; background: rgba(6, 12, 22, .55); color: var(--dim); font-style: italic; font-weight: 500; }
  .cc-line.out { opacity: 0; }
  .cc-name { font: italic 800 .92em/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .08em;
        text-transform: uppercase; color: var(--cyan); margin-right: .5em; }
  .cc-line.note .cc-name { margin-right: .25em; }
  .cc kbd { display: inline-block; min-width: 1.7em; padding: .2em .4em; text-align: center; font: 700 .78em/1 system-ui, sans-serif;
        color: var(--cyan); background: rgba(95, 228, 242, .1); border: 1px solid var(--line); }
  .cc-card { position: fixed; left: 16px; top: 22vh; z-index: 8; width: var(--card); background: rgba(6, 12, 22, .9);
        border: 1px solid var(--line); padding: calc(var(--u) * .9) calc(var(--u) * .9) calc(var(--u) * .75);
        clip-path: polygon(0 0, calc(100% - 14px) 0, 100% 14px, 100% 100%, 14px 100%, 0 calc(100% - 14px));
        animation: cc-in .16s ease-out; }
  .cc-card::before, .cc-card::after { content: ""; position: absolute; width: 20px; height: 1px; background: var(--line);
        transform-origin: left center; transform: rotate(45deg); }
  .cc-card::before { left: calc(100% - 15px); top: -1px; }
  .cc-card::after { left: -1px; top: calc(100% - 15px); }
  .cc-card h2 { margin: 0 0 calc(var(--u) * .7); font: italic 800 .95em/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
        letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); display: flex; align-items: center; gap: 10px; white-space: nowrap; }
  .cc-card h2::after { content: ""; flex: 1; height: 2px; transform: skewX(-30deg);
        background: linear-gradient(90deg, var(--cyan), transparent); opacity: .5; }
  .cc-foot { margin-top: calc(var(--u) * .5); font-size: .72em; font-weight: 500; color: var(--dim); }
  .cc-compose input { width:100%;padding:.65em;background:#091321;color:var(--ink);border:1px solid var(--line);font:inherit; }
  .cc-compose input:focus-visible, .cc-compose button:focus-visible { outline:2px solid var(--cyan);outline-offset:2px; }
  .cc-compose-actions { display:flex;gap:8px;margin-top:10px; }
  .cc-compose button { padding:.5em .75em;background:#102735;color:var(--ink);border:1px solid var(--line);font:inherit;cursor:pointer; }
  .cc-recent { position:fixed;left:16px;bottom:42vh;z-index:7;display:flex;flex-direction:column;
    align-items:flex-start;gap:4px;width:min(380px,calc(100vw - 32px));pointer-events:none; }
  .cc-recent .cc-line > span { display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical; }
  .cc-line.quiet { background:rgba(6,12,22,.35); }
  .cc-feed { bottom:18vh; }
  .cc.composing .cc-recent, .cc.composing .cc-feed { visibility:hidden; }
  .cc-compose { top:8vh;max-height:84vh;overflow:auto;width:min(460px,calc(100vw - 32px)); }
  .cc-history { height:32vh;min-height:80px;overflow-y:auto;overscroll-behavior:contain;
    margin:8px 0 12px;padding:4px 8px;scrollbar-color:var(--dim) #09121e; }
  .cc-history .cc-line { transform:none;animation:none;margin-bottom:6px;background:transparent;padding:5px 8px; }
  .cc-history .cc-line > span { transform:none; }
  .cc-empty { color:var(--dim);font-size:.85em;font-weight:500; }
  .cc-history:focus-visible { outline:2px solid var(--cyan);outline-offset:2px; }
  .cc-compose button:hover { background:#193d50; }
  .cc-compose button:active { background:#24546a; }
  .cc [hidden] { display: none !important; }
  @keyframes cc-in { from { opacity: 0; translate: -8px 0; } }
  @media (prefers-reduced-motion: reduce) {
    .cc-feed, .cc-line { transition: none; } .cc-line, .cc-card { animation: none; }
  }
  `;

  let root, feed, composer, messageInput, composeStatus;
  let composing = false;
  let recent, history;
  // What the player's keyboard prints on a key (Chrome tells; else US names).
  const printed = {};
  const label = (code) => printed[code] || code.slice(3);

  function wake() {
    if (active) return;
    active = true;
    activeAt = performance.now();
    root = document.createElement("div");
    root.className = "cc";
    root.innerHTML = `
    <style>${css}</style>
    <div class="cc-recent" role="log" aria-live="polite" aria-label="Recent messages"></div>
    <div class="cc-feed" role="log" aria-live="polite" aria-label="Chat"></div>
    <form class="cc-card cc-compose" role="dialog" aria-label="Online chat" hidden>
      <h2>Online chat</h2>
      <div class="cc-history" role="log" aria-label="Session messages" tabindex="0"><p class="cc-empty">No messages yet. Messages stay here until you reload.</p></div>
      <label for="cc-message">Message · 120 characters maximum</label>
      <input id="cc-message" maxlength="120" autocomplete="off" placeholder="Say something to nearby players…">
      <div class="cc-foot" role="status"></div>
      <div class="cc-compose-actions"><button type="submit">Send</button><button type="button">Cancel</button></div>
    </form>`;
    document.body.append(root);
    recent = root.querySelector(".cc-recent");
    history = root.querySelector(".cc-history");
    feed = root.querySelector(".cc-feed");
    composer = root.querySelector(".cc-compose");
    messageInput = composer.querySelector("input");
    composeStatus = composer.querySelector("[role=status]");
    composer.querySelector("button[type=button]").addEventListener("click", closeComposer);
    composer.addEventListener("submit", (e) => { e.preventDefault(); sendTyped(); });
    for (const type of ["keydown", "keypress"]) composer.addEventListener(type, (e) => {
      e.stopPropagation();
      if (type === "keydown" && e.code === "Escape") { e.preventDefault(); closeComposer(); }
    });
    navigator.keyboard?.getLayoutMap?.().then((map) => {
      for (const code of [...CHAT_KEYS, ...HORN_KEYS]) {
        const k = map.get(code);
        if (k && /\S/.test(k)) printed[code] = k.toUpperCase();
      }
    }).catch(() => {});
    pickKeys();
    listen();
    setInterval(tick, 100);
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
  // Whether the player is in a car. A build without the block can't say, so
  // after a minute without one the keys simply work.
  function driving() {
    const ptr = shared();
    if (ptr !== null) return views.u32[(ptr + 0x10) >> 2] === 1;
    return !globalThis.carbonMpBlock && performance.now() - activeAt > 60000;
  }
  // Each feature's key: the first of its list the game's bindings leave free,
  // or the first of the list when the game can't say. Run every tick; true
  // when a key changed. A horn held on the old key still stops on its key-up.
  function pickKeys() {
    const ptr = shared(), word = ptr === null ? 0 : views.u32[(ptr + KEYS) >> 2];
    if (word === keysWord) return false;
    keysWord = word;
    const known = word >= KEYS_KNOWN;
    const free = (code) => !known || !(word & (1 << (code.charCodeAt(3) - 65))); // "KeyY" -> bit 24
    const chat = CHAT_KEYS.find(free) ?? null, horn = HORN_KEYS.find(free) ?? null;
    if (chat === chatKey && horn === hornKey) return false;
    chatKey = chat;
    hornKey = horn;
    return true;
  }
  // Another player's car now: the newest pose in its slot, and how far left or
  // right (-0.8 .. 0.8) it is on screen when in view. null without a slot.
  function locate(id) {
    const ptr = shared();
    if (ptr === null) return null;
    const { u32, i32, f32 } = views;
    for (let i = 0; i < SLOTS; i++) {
      const base = ptr + SLOT0 + i * SLOT, b = base >> 2;
      if (u32[b] !== id) continue;
      const out = { at: null, pan: 0 };
      const seq = Atomics.load(i32, b + 1), count = u32[b + 4];
      if (!(seq & 1) && count) {
        const p = (base + 0x20 + ((count - 1) % RING) * SNAP) >> 2;
        const at = [f32[p + 5], f32[p + 6], f32[p + 7]];
        if (Atomics.load(i32, b + 1) === seq) out.at = at;
      }
      const l = (ptr + LABELS + i * 16) >> 2;
      if (f32[l + 2] > 0 && Number.isFinite(f32[l])) out.pan = Math.max(-0.8, Math.min(0.8, (f32[l] - 0.5) * 1.6));
      return out;
    }
    return null;
  }

  const menuOpen = () => {
    const shade = document.querySelector(".cm-shade");
    return !!shade && !shade.hidden;
  };
  const linkOpen = () => !!window.recompOnlineState?.open;
  const myName = () => window.recompOnlineState?.name || window.recompOnline?.name || "You";
  const myCrew = () => window.recompOnlineState?.crew ?? window.recompOnline?.crew ?? "";

  // A burst of `burst`, then one per `every` ms, as the relay counts.
  function limiter({ burst, every }) {
    let tokens = burst, at = performance.now();
    function fill() {
      const now = performance.now();
      tokens = Math.min(burst, tokens + (now - at) / every);
      at = now;
    }
    return {
      take(n = 1, keep = 0) { fill(); if (tokens < n + keep) return false; tokens -= n; return true; },
      wait() { fill(); return tokens >= 1 ? 0 : (1 - tokens) * every; },
    };
  }
  const chatLimit = limiter(CHAT_LIMIT), hornLimit = limiter(HORN_LIMIT);

  // ---- The feed ----
  // A name in its crew's colour, the one net.js gives the crew on the name
  // tags and the badge (window.recompCrewColor); cyan without a crew.
  function who(name, crew) {
    const b = document.createElement("b");
    b.className = "cc-name";
    b.textContent = name || "Player";
    const color = crew ? window.recompCrewColor?.(crew) : "";
    if (color) {
      b.style.color = color;
      b.title = `Crew ${crew}`;
    }
    return b;
  }
  function kbd(text) {
    const k = document.createElement("kbd");
    k.textContent = text;
    return k;
  }
  function line(parts, kind, ms = SHOW_MS) {
    const el = document.createElement("div");
    el.className = kind ? `cc-line ${kind}` : "cc-line";
    const inner = document.createElement("span");
    inner.append(...parts);
    el.append(inner);
    feed.append(el);
    while (feed.children.length > MAX_LINES) feed.firstElementChild.remove();
    const out = () => {
      el.classList.add("out");
      setTimeout(() => el.remove(), 700);
    };
    setTimeout(out, ms);
    return out;
  }
  function say(name, crew, text, kind) {
    const t = document.createElement("span");
    t.textContent = text;
    const el = document.createElement("div");
    el.className = `cc-line ${kind || ""}`;
    const inner = document.createElement("span");
    inner.append(who(name, crew), t);
    el.append(inner);
    const follow = history.scrollHeight - history.scrollTop - history.clientHeight < 40;
    history.querySelector(".cc-empty")?.remove();
    history.append(el.cloneNode(true));
    while (history.children.length > HISTORY_LIMIT) history.firstElementChild.remove();
    if (composing && follow) history.scrollTop = history.scrollHeight;
    recent.append(el);
    while (recent.children.length > RECENT_LIMIT) recent.firstElementChild.remove();
    setTimeout(() => el.classList.add("quiet"), SHOW_MS);
  }

  // The keys, once the player is in a car and again whenever they move (the
  // old hint goes, so no line names a key that no longer works).
  let hinted = false, unhint = [];
  function hint() {
    hinted = true;
    for (const out of unhint.splice(0)) out();
    const parts = [];
    if (chatKey) parts.push(kbd(label(chatKey)), " Chat");
    if (chatKey && hornKey) parts.push("  ·  ");
    if (hornKey) parts.push(kbd(label(hornKey)), " Horn (hold)");
    if (parts.length) unhint.push(line(parts, "note", 10000));
    const all = (codes) => `${codes.slice(0, -1).map(label).join(", ")} and ${label(codes.at(-1))}`;
    if (!chatKey) unhint.push(line([`Chat off: ${all(CHAT_KEYS)} are all bound in Controls`], "note", 10000));
    if (!hornKey) unhint.push(line([`Horn off: ${all(HORN_KEYS)} are all bound in Controls`], "note", 10000));
  }

  // Who is here, from the online client's list: a horn stops when its player
  // leaves. No "joined"/"left" lines: a room can hold a hundred players or more,
  // and the feed would be nothing else (the user, 2026-10-24).
  let known = null;
  function people(s) {
    if (!s || !active) return;
    if (!s.open) {
      known = null;
      for (const id of [...horns.keys()]) stopRemote(id);
      return;
    }
    const here = new Map((s.people || []).map((p) => [p.id, p]));
    for (const id of [...horns.keys()]) if (!here.has(id)) stopRemote(id);
    known = here;
  }

  function heard(m) {
    if (!m || typeof m !== "object") return;
    if (m.t === "chat") {
      const text = cleanText(m.text);
      if (text) say(m.name, m.crew, text);
    } else if (m.t === "horn") {
      remoteHorn(m);
    }
  }

  function openComposer() {
    if (!active || !driving() || menuOpen()) return;
    hornUp();
    composing = true;
    composer.hidden = false;
    root.classList.add("composing");
    globalThis.recompMenuOpen = true; // suppress controller driving while typing
    composeStatus.textContent = "Enter sends · Esc cancels · Nearby players in this room";
    history.scrollTop = history.scrollHeight;
    messageInput.focus();
  }
  function closeComposer() {
    if (!composing) return;
    composing = false;
    composer.hidden = true;
    root.classList.remove("composing");
    globalThis.recompMenuOpen = menuOpen();
    if (!menuOpen()) document.getElementById("canvas")?.focus();
  }
  function sendTyped() {
    const text = cleanText(messageInput.value);
    if (!text) { composeStatus.textContent = "Type a message first."; return; }
    if (!linkOpen()) { composeStatus.textContent = "Not connected. Your message was not sent."; return; }
    if (!chatLimit.take()) { composeStatus.textContent = `Wait ${Math.ceil(chatLimit.wait() / 1000)} seconds before sending again.`; return; }
    window.recompOnlineSend({ t: "chat", text });
    say(myName(), myCrew(), text, "mine");
    messageInput.value = "";
    closeComposer();
  }

  // ---- The horn ----
  let ac = null, master = null, curve = null;
  function audio() {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!ac && AC) {
      ac = new AC();
      master = ac.createGain();
      master.gain.value = 0.45;
      master.connect(ac.destination);
      // A soft clip: the two tones' sum turns brassy, as a real horn's does.
      curve = new Float32Array(1024);
      for (let i = 0; i < curve.length; i++) curve[i] = Math.tanh(2.5 * (i / 511.5 - 1)) / Math.tanh(2.5);
    }
    if (ac && ac.state === "suspended") ac.resume().catch(() => {});
    return ac;
  }
  // One horn sounding: two sawtooth tones (a low and a high horn, 405 and 510
  // Hz) -> soft clip -> a bump at 1 kHz, rolled off above 2.8 kHz -> a 15 ms
  // attack -> pan -> level.
  function hornVoice(level, pan) {
    const c = audio();
    if (!c) return null;
    const t = c.currentTime;
    const mix = c.createGain(), shape = c.createWaveShaper(), body = c.createBiquadFilter(), tone = c.createBiquadFilter();
    const env = c.createGain(), side = c.createStereoPanner(), out = c.createGain();
    mix.gain.value = 0.32;
    shape.curve = curve;
    shape.oversample = "2x";
    body.type = "peaking";
    body.frequency.value = 1000;
    body.Q.value = 1.1;
    body.gain.value = 5;
    tone.type = "lowpass";
    tone.frequency.value = 2800;
    tone.Q.value = 0.7;
    env.gain.setValueAtTime(0, t);
    env.gain.linearRampToValueAtTime(1, t + 0.015);
    side.pan.value = pan;
    out.gain.value = level;
    mix.connect(shape);
    shape.connect(body);
    body.connect(tone);
    tone.connect(env);
    env.connect(side);
    side.connect(out);
    out.connect(master);
    const oscs = [405, 510].map((f) => {
      const o = c.createOscillator();
      o.type = "sawtooth";
      o.frequency.value = f;
      o.connect(mix);
      o.start(t);
      return o;
    });
    return {
      set(level, pan) {
        const now = c.currentTime;
        out.gain.setTargetAtTime(level, now, 0.06);
        side.pan.setTargetAtTime(pan, now, 0.06);
      },
      stop() {
        const now = c.currentTime;
        if (env.gain.cancelAndHoldAtTime) env.gain.cancelAndHoldAtTime(now);
        else env.gain.cancelScheduledValues(now);
        env.gain.linearRampToValueAtTime(0, now + 0.08);
        for (const o of oscs) o.stop(now + 0.1);
        oscs[0].onended = () => out.disconnect();
      },
    };
  }

  // This player's horn. "on" goes out only with a token to spare for its
  // "off", so the relay never drops the off. It stops on the key-up of the key
  // that started it, should the horn's key move meanwhile.
  let mine = null;
  function hornDown(code) {
    if (mine) return;
    const sent = linkOpen() && hornLimit.take(1, 1);
    if (sent) window.recompOnlineSend({ t: "horn", on: true });
    mine = { code, voice: hornVoice(0.8, 0), sent, timer: setTimeout(hornUp, HORN_MAX_MS) };
  }
  function hornUp() {
    if (!mine) return;
    clearTimeout(mine.timer);
    mine.voice?.stop();
    if (mine.sent && hornLimit.take()) window.recompOnlineSend({ t: "horn", on: false });
    mine = null;
  }

  // Other players' horns: id -> {voice, at, timer}. A repeated "on" does not
  // make one last longer.
  const horns = new Map();
  function hearing(id, at) {
    if (!driving()) return { level: 0, pan: 0 }; // not in the world: no street noise
    const here = window.recompOnlineSelf?.(), car = locate(id), there = car?.at || at;
    if (!here || !there) return { level: 0.3, pan: 0 };
    const d = Math.hypot(there[0] - here[0], there[1] - here[1], there[2] - here[2]);
    if (!Number.isFinite(d)) return { level: 0.3, pan: 0 };
    const near = Math.max(0, 1 - d / HEAR_M);
    return { level: 0.8 * near * near, pan: car?.pan || 0 };
  }
  function remoteHorn(m) {
    if (!m.on) { stopRemote(m.id); return; }
    if (horns.has(m.id)) return;
    const at = Array.isArray(m.at) && m.at.length === 3 ? m.at : null;
    const { level, pan } = hearing(m.id, at);
    horns.set(m.id, { voice: hornVoice(level, pan), at, timer: setTimeout(() => stopRemote(m.id), HORN_MAX_MS) });
  }
  function stopRemote(id) {
    const h = horns.get(id);
    if (!h) return;
    clearTimeout(h.timer);
    h.voice?.stop();
    horns.delete(id);
  }

  // ---- Keys ----
  function eat(e) {
    e.preventDefault();
    e.stopImmediatePropagation();
  }
  const typing = (t) => !!t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName));
  function listen() {
    // Browsers start sound only after a key press or click.
    const unlock = () => {
      audio();
      removeEventListener("keydown", unlock, true);
      removeEventListener("pointerdown", unlock, true);
    };
    addEventListener("keydown", unlock, true);
    addEventListener("pointerdown", unlock, true);

    addEventListener("keydown", (e) => {
      if (e.code === "F9" || e.code === "F10") { closeComposer(); return; } // the menu's key: it takes over
      if (menuOpen() || typing(e.target) || e.target?.closest?.(".cm")) return;
      // chatKey / hornKey are null when every key of their list is the game's.
      if ((e.code !== chatKey && e.code !== hornKey) || e.ctrlKey || e.metaKey || !driving()) return;
      if (e.code === hornKey && window.recompPoliceKey?.(e)) { eat(e); return; } // the police player's siren (net.js)
      eat(e);
      if (e.repeat) return;
      if (e.code === chatKey) openComposer();
      else hornDown(e.code);
    }, true);
    addEventListener("keypress", (e) => {
      if (composing) return;
      if ((e.code === chatKey || e.code === hornKey) && !e.ctrlKey && !e.metaKey && driving() && !menuOpen()) eat(e);
    }, true);
    addEventListener("keyup", (e) => { if (e.code === mine?.code) hornUp(); }, true); // never stopped
    addEventListener("blur", hornUp);
    document.addEventListener("visibilitychange", () => { if (document.hidden) hornUp(); });
  }

  function tick() {
    if (pickKeys()) hinted = false; // show the new keys (once in a car)
    const inCar = driving(), menu = menuOpen();
    if (inCar && !hinted) hint();
    if (composing && (!inCar || menu)) closeComposer();
    if (mine && (!inCar || menu)) hornUp();
    for (const [id, h] of horns) {
      const { level, pan } = hearing(id, h.at);
      h.voice?.set(level, pan);
    }
  }

  addEventListener("recomp-online", (e) => { wake(); people(e.detail); });
  addEventListener("recomp-online-message", (e) => { if (active) heard(e.detail); });
  if (window.recompOnline || window.recompOnlineState) {
    wake();
    people(window.recompOnlineState);
  }
})();
