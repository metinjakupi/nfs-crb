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
// (Z on a German keyboard). The Chat button works throughout the online session;
// letter shortcuts work only while driving, so native Messenger and other game
// text fields keep their letters. Neither intercepts browser text fields
// or keys while the F9 menu is open. A window capture listener eats
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
  .cc { --u:clamp(14px,1.2vw,17px); }
  .cc-compose {left:max(20px,env(safe-area-inset-left));top:auto;bottom:max(24px,env(safe-area-inset-bottom));
    width:min(580px,calc(100vw - 40px));height:min(660px,88dvh);max-height:88dvh;display:flex;flex-direction:column;
    overflow:hidden;padding:20px;gap:12px;clip-path:none;border-radius:8px;background:rgba(7,17,28,.97);box-shadow:0 16px 64px #0008;animation:none;}
  .cc-compose::before,.cc-compose::after {display:none;}
  .cc-header {display:flex;align-items:center;justify-content:space-between;gap:12px;}
  .cc-card h2 {margin:0;font-size:17px;letter-spacing:.12em;}
  .cc-card h2::after {display:none;}
  .cc-channel {margin:4px 0 0;color:var(--dim);font-size:13px;font-weight:500;}
  .cc-compose button {min-height:42px;border-radius:4px;font-size:14px;}
  .cc-compose .cc-close {width:42px;padding:0;font-size:24px;line-height:1;background:transparent;}
  .cc-history {flex:1;min-height:72px;height:auto;margin:0;padding:4px 10px 4px 0;scrollbar-width:thin;}
  .cc-history .cc-line {padding:10px 12px;margin:0 0 6px;border-left:2px solid #31516a;background:#10202d55;border-radius:0 4px 4px 0;text-shadow:none;}
  .cc-history .cc-line.mine {border-left-color:var(--cyan);background:#12313955;}
  .cc-history .cc-line.note {font-style:normal;color:#b8cbd9;background:#13212a;border-left-color:#4a6578;}
  .cc-message-meta {display:flex;align-items:baseline;gap:12px;margin-bottom:5px;}
  .cc-message-meta .cc-name {font:700 13px/1.4 "Avenir Next","Segoe UI",system-ui,sans-serif;letter-spacing:.02em;text-transform:none;margin:0;}
  .cc-message-meta time {font-size:11px;color:var(--dim);font-weight:400;}
  .cc-message-text {display:block;font-weight:400;line-height:1.5;overflow-wrap:anywhere;white-space:pre-wrap;}
  .cc-compose label {font-size:13px;color:#c3d4df;}
  .cc-input-row {display:flex;gap:8px;}
  .cc-compose input {min-width:0;flex:1;width:auto;min-height:46px;border-radius:4px;font-size:16px;font-weight:400;}
  .cc-compose .cc-send {background:var(--cyan);color:#071722;border-color:var(--cyan);font-weight:700;padding:0 18px;}
  .cc-compose .cc-send:hover {background:#a1f5fc;}
  .cc-compose button:disabled {opacity:.4;cursor:default;}
  .cc-compose-actions {margin:0;display:flex;justify-content:space-between;align-items:center;gap:10px;}
  .cc-compose-actions span {color:var(--dim);font-size:12px;font-weight:400;}
  .cc-foot {margin:0;min-height:1.4em;font-size:13px;line-height:1.4;}
  .cc-foot.bad {color:#ffb1a4;}
  .cc-suggestions {overflow-y:auto;max-height:32dvh;flex-shrink:0;border:1px solid var(--line);border-radius:4px;background:#0b1d2a;padding:4px;}
  .cc-suggestions button {width:100%;display:flex;text-align:left;align-items:baseline;justify-content:space-between;gap:12px;border:0;background:transparent;padding:9px 10px;}
  .cc-suggestions strong {font:700 14px/1.4 ui-monospace,monospace;color:var(--cyan);white-space:nowrap;}
  .cc-suggestions small {font-size:12px;color:#bed0de;font-weight:400;}
  .cc-open {position:fixed;left:max(20px,env(safe-area-inset-left));bottom:max(24px,env(safe-area-inset-bottom));z-index:7;
    min-height:42px;padding:8px 14px;display:flex;align-items:center;gap:14px;background:#0a1929e8;color:var(--ink);border:1px solid var(--line);border-radius:4px;font:600 14px/1.4 system-ui,sans-serif;cursor:pointer;}
  .cc-open:hover {background:#163549;}.cc-open:focus-visible {outline:2px solid var(--cyan);outline-offset:3px;}
  .cc.composing .cc-open {display:none;}
  .cc-recent .cc-message-meta {margin-bottom:2px;}
  @media(max-width:600px) {.cc-compose {left:10px;bottom:max(10px,env(safe-area-inset-bottom));width:calc(100vw - 20px);padding:14px;height:88dvh;gap:10px;}.cc-suggestions button {align-items:flex-start;flex-direction:column;gap:2px;}.cc-compose-actions span {font-size:11px;}}
  @media(max-height:500px) {.cc-compose {bottom:8px;height:calc(100dvh - 16px);max-height:calc(100dvh - 16px);padding:12px;gap:8px;}.cc-history .cc-line {padding:6px 10px;}.cc-suggestions {max-height:25dvh;}}
  .cc-conversation {position:relative;flex:1;min-height:64px;}
  .cc-conversation .cc-history {height:100%;min-height:0;}
  .cc-conversation .cc-suggestions {position:absolute;inset:0;max-height:none;}
  @media(max-height:500px) {.cc-compose {gap:6px;}.cc-header h2 {font-size:15px;}.cc-channel {font-size:12px;}.cc-compose .cc-close {min-height:36px;height:36px;}.cc-compose-actions button {min-height:36px;}}
  @keyframes cc-in { from { opacity: 0; translate: -8px 0; } }
  @media (prefers-reduced-motion: reduce) {
    .cc-feed, .cc-line { transition: none; } .cc-line, .cc-card { animation: none; }
  }
  `;

  let root, feed, composer, messageInput, composeStatus;
  let composing = false;
  let recent, history;
  let enabled = true;
  try { enabled = localStorage.getItem("nfsc.chatEnabled") !== "false"; } catch {}
  function setEnabled(on) {
    enabled = !!on;
    try { localStorage.setItem("nfsc.chatEnabled", String(enabled)); } catch {}
    if (!enabled) closeComposer();
    if (root) root.hidden = !enabled;
  }
  window.recompMenuToggles ||= [];
  window.recompMenuToggles.push({ id: "chat", label: "Chat", get: () => enabled, set: setEnabled });
  dispatchEvent(new Event("recomp-menu-toggles"));
  // What the player's keyboard prints on a key (Chrome tells; else US names).
  const printed = {};
  const label = (code) => printed[code] || code.slice(3);

  function wake() {
    if (active) return;
    active = true;
    activeAt = performance.now();
    root = document.createElement("div");
    root.className = "cc";
    root.hidden = !enabled;
    root.innerHTML = `
    <style>${css}</style>
    <div class="cc-recent" role="log" aria-live="polite" aria-label="Recent messages"></div>
    <div class="cc-feed" role="log" aria-live="polite" aria-label="Chat"></div>
    <button class="cc-open" type="button" aria-label="Open online chat">Chat <kbd>Y</kbd></button>
    <form class="cc-card cc-compose" role="dialog" aria-modal="true" aria-label="Online chat" hidden>
      <div class="cc-header"><div><h2>Online chat</h2><p class="cc-channel">Everyone in this room</p></div><button class="cc-close" type="button" aria-label="Close chat">×</button></div>
      <div class="cc-conversation"><div class="cc-history" role="log" aria-label="Session messages" tabindex="0"><p class="cc-empty">Say hello to the room. Use Commands to invite friends or send a private message.</p></div>
      <div class="cc-suggestions" aria-label="Chat commands" hidden></div></div>
      <label for="cc-message">Message <span class="cc-count">0 / 120</span></label>
      <div class="cc-input-row"><input id="cc-message" maxlength="120" autocomplete="off" placeholder="Message the room, or type / for commands"><button class="cc-send" type="submit">Send</button></div>
      <div class="cc-foot" role="status"></div>
      <div class="cc-compose-actions"><button class="cc-commands" type="button" aria-expanded="false">Commands</button><span>Enter to send · Esc to return to game</span></div>
    </form>`;
    document.body.append(root);
    recent = root.querySelector(".cc-recent");
    history = root.querySelector(".cc-history");
    feed = root.querySelector(".cc-feed");
    composer = root.querySelector(".cc-compose");
    messageInput = composer.querySelector("input");
    composeStatus = composer.querySelector("[role=status]");
    composer.querySelector(".cc-close").addEventListener("click", closeComposer);
    root.querySelector(".cc-open").addEventListener("click", openComposer);
    composer.querySelector(".cc-commands").addEventListener("click", () => {showCommands = !showCommands; renderCommands(); messageInput.focus();});
    messageInput.addEventListener("input", updateComposer);
    messageInput.addEventListener("keydown", e => {
      const first = composer.querySelector(".cc-suggestions:not([hidden]) button");
      if (first && (e.key === "Tab" || e.key === "ArrowDown") && !e.shiftKey) {e.preventDefault(); if (e.key === "Tab") first.click(); else first.focus();}
    });
    composer.addEventListener("keydown", e => {
      if (e.key !== "Tab" || e.defaultPrevented) return;
      const items = [...composer.querySelectorAll('button:not(:disabled),input,[tabindex="0"]')].filter(el => el.getClientRects().length);
      if (e.shiftKey && document.activeElement === items[0]) {e.preventDefault(); items.at(-1)?.focus();}
      else if (!e.shiftKey && document.activeElement === items.at(-1)) {e.preventDefault(); items[0]?.focus();}
    });
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
    updateComposer();
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
  // Native text fields render on the canvas, so DOM focus cannot identify them.
  // Only a known driving state gives the web overlay ownership of letter keys.
  const drivingShortcuts = () => shared() !== null && driving();
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
    const meta = document.createElement("span"), time = document.createElement("time");
    meta.className = "cc-message-meta"; t.className = "cc-message-text";
    const now = new Date(); time.dateTime = now.toISOString(); time.textContent = now.toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"});
    meta.append(who(name, crew), time); inner.append(meta, t);
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
      if (!enabled) return;
      const text = cleanText(m.text);
      if (m.error) commandReply(m.error === "profanity" ? "Message blocked. Swearing is not allowed." : m.error === "muted" ? `Chat muted until ${new Date(m.until * 1000).toLocaleTimeString()}.` : "Message was not delivered. The player may have left, or you sent messages too quickly.");
      else if (text) say(m.to ? (m.id === window.recompOnlineState?.me ? `To ${m.targetName}` : `Private · ${m.name}`) : m.name, m.crew, text);
      if (composing && (m.error || m.ack || m.to && m.id === window.recompOnlineState?.me)) {
        composeStatus.classList.toggle("bad", !!m.error);
        composeStatus.textContent = m.error === "profanity" ? "Message blocked. Swearing is not allowed." : m.error === "muted" ? "You are muted. See the message above for the end time." : m.error ? "Message was not delivered." : "Message delivered.";
      }
    } else if (m.t === "moderation" && m.k === "disconnect") {
      commandReply(`${m.text} Reload to reconnect.`);
      if (composing) {composeStatus.textContent = "Disconnected. Press Esc to return to the game."; composeStatus.classList.add("bad");updateComposer();}
    } else if (m.t === "moderation" && m.k === "announcement") {
      say("Admin announcement", "", m.text, "note");
    } else if (m.t === "horn") {
      remoteHorn(m);
    }
  }

  function openComposer() {
    if (!active || !enabled || !linkOpen() || menuOpen()) return;
    hornUp();
    composing = true;
    composer.hidden = false;
    root.classList.add("composing");
    globalThis.recompMenuOpen = true; // suppress controller driving while typing
    composeStatus.classList.remove("bad");
    composeStatus.textContent = "Messages stay here until you reload.";
    updateComposer();
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
  let commandBusy = false, showCommands = false;
  const commands = [
    ["help", "Show commands", ""], ["invite", "Invite a friend to your crew", "\"Player Name\""],
    ["crew", "Your crew and members", "[page]"], ["invites", "Your crew invitations", "[page]"],
    ["players", "Players in this room", "[page]"], ["w", "Send a private message", "\"Player Name\" message"],
    ["report", "Report a player to Admin", "\"Player Name\" reason"],
    ["mute", "Mute a player · default 10 minutes", "\"Player Name\" [minutes]", true],
    ["unmute", "Restore a player's chat", "\"Player Name\"", true], ["muted", "Active mutes", "[page]", true],
    ["kick", "Disconnect a player", "\"Player Name\" [reason]", true], ["announce", "Message all rooms", "message", true], ["reports", "Review player reports", "[page]", true],
  ];
  function renderCommands() {
    if (!composer) return;
    const list = composer.querySelector(".cc-suggestions"), prefix = /^\/([a-z]*)$/i.exec(messageInput.value)?.[1]?.toLowerCase();
    const rows = commands.filter(([name,,,adminOnly]) => (!adminOnly || window.recompOnlineState?.admin) && (prefix === undefined || name.startsWith(prefix)));
    const visible = showCommands || prefix !== undefined;
    list.replaceChildren(); list.hidden = !visible || !rows.length;
    history.hidden = !list.hidden;
    composer.querySelector(".cc-commands").setAttribute("aria-expanded", String(!list.hidden));
    for (const [name, description, syntax] of rows) {
      const button = document.createElement("button"), title = document.createElement("strong"), hint = document.createElement("small");
      button.type = "button"; title.textContent = `/${name}`; hint.textContent = description;
      button.title = `/${name}${syntax ? " " + syntax : ""}`; button.append(title, hint);
      button.addEventListener("click", () => {
        messageInput.value = `/${name} `; showCommands = false; updateComposer(); messageInput.focus();
        composeStatus.textContent = `/${name}${syntax ? " " + syntax : ""}`;
      }); list.append(button);
    }
  }
  function updateComposer() {
    if (!composer) return;
    composer.querySelector(".cc-count").textContent = `${messageInput.value.length} / 120`;
    composer.querySelector(".cc-send").disabled = commandBusy || !messageInput.value.trim() || !linkOpen();
    composer.querySelector(".cc-channel").textContent = /^\/w\s/i.test(messageInput.value) ? "Private message · only you and the recipient" : "Everyone in this room";
    renderCommands();
  }

  const commandReply = text => say("System", "", text, "note");
  async function runCommand(text) {
    const match = /^\/(\S+)(?:\s+([\s\S]*))?$/.exec(text);
    if (!match) throw new Error("Type /help for commands.");
    const command = match[1].toLowerCase(), args = (match[2] || "").trim();
    const state = window.recompCrews || {}, crew = state.crew;
    const pageNumber = () => {
      if (args && !/^[1-9]\d{0,5}$/.test(args)) throw new Error(`Use /${command} or /${command} 2 for the next page.`);
      return args ? Number(args) : 1;
    };
    if (command === "help") {
      showCommands = true; renderCommands();
    } else if (["mute", "unmute", "kick", "report", "announce", "muted", "reports"].includes(command)) {
      if (command !== "report" && !window.recompOnlineState?.admin) throw new Error("Only the protected Admin account can use that command.");
      let fields = {};
      if (command === "announce") {
        if (!args) throw new Error("Use /announce message.");
        fields.text = args;
      } else if (command === "muted" || command === "reports") fields.offset = (pageNumber() - 1) * (command === "reports" ? 25 : 50);
      else {
        const parts = /^(?:"([^"\n]+)"|(\S+))(?:\s+(.+))?$/.exec(args);
        if (!parts) throw new Error(`Use /${command} "Player Name"${command === "report" ? " reason" : command === "mute" ? " [minutes]" : ""}.`);
        fields.name = parts[1] || parts[2];
        if (command === "mute") {
          if (parts[3] && !/^[1-9]\d{0,4}$/.test(parts[3])) throw new Error("Use whole minutes from 1 to 10080. The default is 10.");
          fields.minutes = parts[3] ? Number(parts[3]) : 10;
          if (fields.minutes > 10080) throw new Error("Use whole minutes from 1 to 10080.");
        } else if (command === "unmute" && parts[3]) throw new Error('Use /unmute "Player Name".');
        else fields.text = parts[3] || "";
        if (command === "report" && !fields.text) throw new Error('Use /report "Player Name" reason.');
      }
      let result;
      try {result = await window.recompModerationRequest(command, fields);}
      catch (e) {
        const messages = {unavailable:"Moderation needs the updated server to be activated.", forbidden:"Only the protected Admin account can use that command.", target:"That name was not found or is used by several players. Use an exact protected name or an online player name.", busy:"Too many requests. Try again later.", timeout:"The server did not answer. Check the list before retrying."};
        throw new Error(messages[e.message] || e.message);
      }
      if (command === "muted" || command === "reports") {
        commandReply(`${command === "muted" ? "Active mutes" : "Player reports"} · ${result.data.total} total`);
        for (const row of result.data.rows) commandReply(command === "muted" ? `${row.name} · until ${new Date(row.until * 1000).toLocaleString()}` : `#${row.id} · ${row.name}: ${row.text}`);
      } else commandReply(command === "report" ? `Report #${result.data.id} saved for Admin review.` : command === "mute" ? `${fields.name} muted for ${fields.minutes} minutes.` : command === "unmute" ? `${fields.name} unmuted.` : command === "kick" ? `${fields.name} disconnected.` : "Announcement sent.");
    } else if (command === "players") {
      const page = pageNumber(), players = [{name:myName()}, ...(window.recompOnlineState?.people || [])];
      const rows = players.slice((page - 1) * 20, page * 20);
      commandReply(`Online players · page ${page}/${Math.max(1, Math.ceil(players.length / 20))}: ${rows.map(p => p.name).join(", ") || "No players on this page."}`);
    } else if (command === "invite") {
      if (!crew) throw new Error("Join or create a crew first in Online → My crew.");
      if (crew.leader !== state.account) throw new Error("Only the crew leader can invite players.");
      const name = args.startsWith('"') ? /^"([^"\n]+)"$/.exec(args)?.[1] : args;
      if (!name || name.length > 20) throw new Error('Use /invite playername or /invite "Player Name".');
      await window.recompCrewRequest("invite", {crew:crew.id, revision:crew.revision, name});
      commandReply(`Invitation sent to ${name}. They can accept in Online → Invitations.`);
    } else if (command === "crew" || command === "invites") {
      const page = pageNumber();
      if (command === "crew" && !crew) throw new Error("You do not belong to a crew. Open Online → My crew.");
      const result = await window.recompCrewRequest(command === "crew" ? "roster" : "inbox", {offset:(page - 1) * 50});
      const data = result.page;
      commandReply(`${command === "crew" ? crew.name : "Crew invitations"} · page ${page}/${Math.max(1, Math.ceil(data.total / 50))} · ${data.total} total`);
      // Separate lines keep long rosters readable without a membership limit.
      for (let i = 0; i < data.rows.length; i += 10) commandReply(data.rows.slice(i, i + 10).map(p => command === "crew" ? `${p.name}${p.leader ? " (leader)" : ""}` : p.crew).join(", "));
      if (command === "invites") commandReply("Accept or decline in Online → Invitations.");
    } else if (command === "w") {
      if (!window.recompOnlineState?.whispersOk) throw new Error("This server needs an update before private messages are available.");
      const parts = /^(?:"([^"\n]+)"|(\S+))\s+(.+)$/.exec(args);
      if (!parts) throw new Error('Use /w playername message or /w "Player Name" message.');
      const name = parts[1] || parts[2];
      const matches = (window.recompOnlineState?.people || []).filter(p => p.name?.toLowerCase() === name.toLowerCase());
      if (matches.length !== 1) throw new Error(matches.length ? "Several players use that name. Ask them to use a unique name." : "That player is not online in this room.");
      window.recompOnlineSend({t:"chat", to:matches[0].id, text:parts[3]});
    } else throw new Error(`Unknown command /${command}. Type /help.`);
  }
  async function sendTyped() {
    if (!enabled || commandBusy) return;
    let text = cleanText(messageInput.value);
    if (!text) { composeStatus.textContent = "Type a message first."; return; }
    if (!linkOpen()) { composeStatus.textContent = "Not connected. Your message was not sent."; return; }
    if (text.startsWith("/") && !text.startsWith("//")) {
      commandBusy = true;
      composeStatus.classList.remove("bad");
      updateComposer();
      composeStatus.textContent = "Running command…";
      const entered = messageInput.value;
      try {
        await runCommand(text);
        if (messageInput.value === entered) messageInput.value = "";
        composeStatus.textContent = "Command complete · /help for commands";
      } catch (e) {
        composeStatus.classList.add("bad");
        const errors = {reserved:"The Admin name and crew are reserved for the protected Admin account.", protected:"The Admin account cannot be muted or kicked.", duration:"Use whole minutes from 1 to 10080.", text:"Add a short reason or message, up to 120 characters.", profanity:"Message blocked. Swearing is not allowed.", identity:"Protect your player name on the start page first.", target:"That player needs a protected name.", forbidden:"Only the crew leader can do that.", stale:"Your crew changed. Check /crew and try again.", "target-crew":"That player already belongs to a crew.", unavailable:"Protected crews are not available on this server.", busy:"Wait a few seconds and try again.", timeout:"The server did not answer. Check /invites before retrying.", disconnected:"Connection closed. Reconnect and try again."};
        composeStatus.textContent = errors[e.message] || e.message || "Command failed.";
      } finally { commandBusy = false; updateComposer(); }
      return;
    }
    if (text.startsWith("//")) text = text.slice(1);
    if (!chatLimit.take()) { composeStatus.textContent = `Wait ${Math.ceil(chatLimit.wait() / 1000)} seconds before sending again.`; return; }
    window.recompOnlineSend({ t: "chat", text });
    if (!window.recompOnlineState?.chatAckOk) say(myName(), myCrew(), text, "mine");
    messageInput.value = "";
    composeStatus.classList.remove("bad");
    composeStatus.textContent = window.recompOnlineState?.chatAckOk ? "Sending…" : "Message sent.";
    updateComposer();
    messageInput.focus();
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
      if (menuOpen() || typing(e.target) || e.target?.closest?.(".cm") || !drivingShortcuts()) return;
      if (e.code === chatKey && !enabled) return;
      // chatKey / hornKey are null when every key of their list is the game's.
      if ((e.code !== chatKey && e.code !== hornKey) || e.ctrlKey || e.metaKey) return;
      if (e.code === hornKey && window.recompPoliceKey?.(e)) { eat(e); return; } // the police player's siren (net.js)
      eat(e);
      if (e.repeat) return;
      if (e.code === chatKey) openComposer();
      else hornDown(e.code);
    }, true);
    addEventListener("keypress", (e) => {
      if (composing || menuOpen() || typing(e.target) || e.target?.closest?.(".cm") || !drivingShortcuts()) return;
      if (((enabled && e.code === chatKey) || e.code === hornKey) && !e.ctrlKey && !e.metaKey) eat(e);
    }, true);
    addEventListener("keyup", (e) => { if (e.code === mine?.code) hornUp(); }, true); // never stopped
    addEventListener("blur", hornUp);
    document.addEventListener("visibilitychange", () => { if (document.hidden) hornUp(); });
  }

  function tick() {
    if (pickKeys()) hinted = false; // show the new keys (once in a car)
    const inCar = driving(), menu = menuOpen();
    if (enabled && inCar && !hinted) hint();
    if (root) {
      const button = root.querySelector(".cc-open");
      button.hidden = menu || !linkOpen();
      const key = button.querySelector("kbd"); key.hidden = !chatKey || !drivingShortcuts(); if (chatKey) key.textContent = label(chatKey);
    }
    if (composing && menu) closeComposer();
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
