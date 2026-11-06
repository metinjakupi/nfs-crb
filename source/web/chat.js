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
// THE LOOK (plans/chat-redesign, approved 2026-11-04): Carbon's style, as the
// F9 menu (teal on near-black, bars slanted -14 degrees, Saira Condensed
// italic capitals; menu.js loads the fonts). It keeps out of the way while
// driving: at most FEED_LINES messages middle-left, above the game's minimap,
// one row each (name and text together, at most two lines), each fading out
// SHOW_MS after it came, so with nothing new there is nothing but a small chat
// chip beside net.js's online badge at the top (one group with the ☰ and the
// badge), with the key and how many messages came since the chat was last open. The chat key
// opens a compact window that slides up from the bottom edge, at the left
// (history, then the input line with its channel, Send and the status); the command list
// takes the history's place while one is being picked (arrows move, Tab or
// Enter fills it in). Crewmates joining or leaving get a line; other players
// do not (a room can hold a hundred). On touch screens (navigator.maxTouchPoints,
// as touch.js) the feed sits at the top under the chip, clear of the touch
// controls, and the window docks a little above the bottom, above them and
// the on-screen keyboard.
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
  const SHOW_MS = 8000;      // a line in the feed stays this long, then fades out
  const HINT_MS = 10000;     // a hint about the keys stays a little longer
  const FEED_LINES = 3;      // at most this many lines over the game
  const HISTORY_LIMIT = 200;
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
  .cc { --ink: #e6f3f8; --dim: #87a0ad; --faint: #4b5f69; --teal: #3fe0e8; --teal-deep: #0e8f9a; --teal-glow: rgba(63, 224, 232, .45);
        --line: rgba(63, 224, 232, .18); --amber: #f3b54a; --red: #ff5a4e; --skew: -14deg;
        --cond: "Saira Condensed", "Avenir Next Condensed", "Arial Narrow", system-ui, sans-serif;
        --body: "Saira", "Avenir Next", "Segoe UI", system-ui, sans-serif;
        --u: clamp(12.5px, 1.04vw, 15px); font: 500 var(--u)/1.32 var(--body); color: var(--ink); }
  .cc * { box-sizing: border-box; }
  .cc [hidden] { display: none; }
  .cc :where(button, input) { font: inherit; color: inherit; } /* :where keeps it below every rule after it */
  /* The chip: with the chat key and the unread count, right of net.js's online badge (layout() places it). */
  .cc-chip { position: fixed; z-index: 7; left: var(--chip-x, 62px); top: var(--chip-y, 10px);
        height: var(--chip-h, 32px); min-width: 40px; padding: 0 9px; display: flex; align-items: center; gap: 6px; cursor: pointer;
        background: rgba(6, 12, 22, .62); border: 1px solid rgba(63, 224, 232, .22); transform: skewX(-12deg); opacity: .55;
        transition: opacity .3s ease, border-color .15s ease; }
  .cc-chip > * { transform: skewX(12deg); }
  .cc-chip:hover, .cc-chip:focus-visible, .cc-chip.lit { opacity: 1; border-color: var(--teal); outline: none; }
  .cc-chip svg { width: 15px; height: 15px; fill: none; stroke: var(--teal); stroke-width: 1.8; }
  .cc-chip kbd { font: 700 11px/1 var(--cond); color: var(--ink); letter-spacing: .04em; }
  .cc-unread { min-width: 18px; padding: 2px 4px 1px; font: italic 800 11px/1 var(--cond); text-align: center; color: #021015;
        background: var(--teal); box-shadow: 0 0 10px var(--teal-glow); }
  .cc.composing .cc-chip { opacity: 0; pointer-events: none; }
  /* The feed: middle-left, above the game's minimap (its top edge is 37% up the screen). */
  .cc-feed { position: fixed; z-index: 7; left: max(16px, env(safe-area-inset-left)); bottom: 40vh; width: calc(var(--u) * 25); max-width: calc(100vw - 32px);
        display: flex; flex-direction: column; align-items: flex-start; gap: 3px; pointer-events: none; }
  .cc.composing .cc-feed { visibility: hidden; }
  .cc-ln { --c: var(--teal); --sk: var(--skew); position: relative; max-width: 100%; padding: .2em 1em .24em .95em; isolation: isolate;
        text-shadow: 0 1px 2px rgba(0, 0, 0, .85); animation: cc-in .24s cubic-bezier(.2, .8, .2, 1) both; transition: opacity .6s ease, translate .6s ease; }
  .cc-ln::before { content: ""; position: absolute; inset: 0; z-index: -1; transform: skewX(var(--sk));
        background: linear-gradient(90deg, rgba(4, 8, 11, .84), rgba(4, 8, 11, .66) 75%, rgba(4, 8, 11, .4)); border-left: 2px solid var(--c); }
  .cc-ln.cc-new::after { content: ""; position: absolute; inset: 0; z-index: -1; transform: skewX(var(--sk)); pointer-events: none;
        background: linear-gradient(90deg, color-mix(in srgb, var(--c) 34%, transparent), transparent 70%); animation: cc-flash 1.4s ease-out both; }
  .cc-ln.cc-out { opacity: 0; translate: -8px 0; }
  .cc-body { display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; overflow-wrap: anywhere; }
  .cc-nm { font: italic 700 .92em/1 var(--cond); letter-spacing: .06em; text-transform: uppercase; color: var(--c); margin-right: .5em; }
  .cc-tag { display: inline-block; margin-right: .55em; padding: .18em .45em .1em; font: 700 .68em/1 var(--cond); letter-spacing: .14em; text-transform: uppercase;
        font-style: normal; color: var(--tc, var(--teal)); border: 1px solid currentColor; transform: skewX(var(--skew)); vertical-align: .12em; }
  .cc-ln.mine::before { background: linear-gradient(90deg, rgba(14, 60, 66, .78), rgba(4, 8, 11, .62) 75%, rgba(4, 8, 11, .38)); }
  .cc-ln.sys { --c: #5d7682; color: #b5c9d1; }
  .cc-ln.sys .cc-tx { font-style: italic; }
  .cc-ln.admin { --c: var(--amber); --tc: #1b1203; }
  .cc-ln.admin .cc-tag { background: var(--amber); border-color: var(--amber); }
  .cc-ln.admin .cc-nm { color: var(--amber); }
  .cc-ln.pm { --tc: #ff8fd8; }
  .cc-ln.err { --c: var(--red); --tc: var(--red); color: #ffd0cb; }
  .cc-ln kbd { display: inline-grid; place-items: center; min-width: 1.6em; height: 1.45em; padding: 0 .35em; margin: 0 .3em 0 0; border: 1px solid rgba(255, 255, 255, .35);
        border-radius: 3px; font: 700 .82em/1 var(--cond); color: #fff; background: rgba(255, 255, 255, .06); vertical-align: .06em; }
  .cc-gap { display: inline-block; width: 1.2em; }
  /* The chat window: docked to the bottom edge, left, sliding up from below. While it is open it
     covers the game's minimap (bottom-left), which is not needed while typing; it keeps clear of
     the car and the road in the middle, the game's centre prompts and the speedometer. */
  .cc-box { position: fixed; z-index: 8; left: max(16px, env(safe-area-inset-left)); bottom: 0; width: min(calc(var(--u) * 32), calc(100vw - 32px));
        display: flex; flex-direction: column; gap: 7px; padding: 9px 12px calc(8px + env(safe-area-inset-bottom));
        border: 1px solid var(--line); border-bottom: 0; background-color: rgba(6, 11, 15, .92);
        background-image: linear-gradient(160deg, rgba(63, 224, 232, .08), transparent 45%),
          linear-gradient(27deg, rgba(13, 19, 23, .8) 5px, transparent 5px), linear-gradient(207deg, rgba(13, 19, 23, .8) 5px, transparent 5px),
          linear-gradient(27deg, rgba(16, 23, 28, .8) 5px, transparent 5px), linear-gradient(207deg, rgba(16, 23, 28, .8) 5px, transparent 5px),
          linear-gradient(90deg, rgba(12, 18, 22, .8) 10px, transparent 10px);
        background-size: 100% 100%, 20px 20px, 20px 20px, 20px 20px, 20px 20px, 20px 20px;
        background-position: 0 0, 0 5px, 10px 0, 0 10px, 10px 5px, 0 0;
        clip-path: polygon(0 0, calc(100% - 16px) 0, 100% 16px, 100% 100%, 0 100%);
        animation: cc-up .26s cubic-bezier(.2, .8, .2, 1) both; }
  .cc-box.cc-closing { animation: cc-down .2s ease-in both; pointer-events: none; }
  .cc-head { display: flex; align-items: center; gap: 10px; min-height: 24px; }
  .cc-head h2 { margin: 0; font: italic 700 12px/1 var(--cond); letter-spacing: .26em; text-transform: uppercase; color: var(--teal); white-space: nowrap; }
  .cc-channel { font: 500 11px/1.2 var(--body); color: var(--dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
  .cc-rule { flex: 1; min-width: 12px; height: 1px; background: linear-gradient(90deg, var(--line), transparent); }
  .cc-ib { flex: none; display: grid; place-items: center; width: 28px; height: 24px; padding: 0; cursor: pointer; background: transparent;
        border: 1px solid var(--line); transform: skewX(-12deg); font: 700 13px/1 var(--cond); color: var(--dim); }
  .cc-ib > * { transform: skewX(12deg); }
  .cc-ib:hover, .cc-ib:focus-visible, .cc-ib[aria-expanded="true"] { color: var(--teal); border-color: var(--teal); background: rgba(63, 224, 232, .1); outline: none; }
  .cc-history { max-height: min(30vh, 250px); min-height: 60px; overflow-y: auto; overscroll-behavior: contain; display: flex; flex-direction: column; gap: 3px;
        padding: 2px 8px 2px 2px; scrollbar-width: thin; scrollbar-color: var(--teal-deep) transparent;
        -webkit-mask-image: linear-gradient(transparent, #000 12px); mask-image: linear-gradient(transparent, #000 12px); }
  .cc-history:focus-visible { outline: 1px solid var(--teal); outline-offset: 2px; }
  .cc-history .cc-ln { --sk: -9deg; animation: none; text-shadow: none; display: flex; align-items: baseline; gap: 8px; padding-right: .7em; flex: none; }
  .cc-history .cc-ln::before { background: rgba(255, 255, 255, .03); }
  .cc-history .cc-ln.mine::before { background: rgba(63, 224, 232, .08); }
  .cc-history .cc-body { display: block; flex: 1; min-width: 0; }
  .cc-history time { flex: none; margin-left: auto; font: 500 10px/1 var(--body); color: var(--faint); }
  .cc-empty { margin: 6px 4px; color: var(--dim); font-size: .9em; }
  .cc-suggestions { max-height: min(30vh, 250px); overflow-y: auto; overscroll-behavior: contain; display: flex; flex-direction: column; gap: 1px;
        padding: 2px 6px 2px 2px; scrollbar-width: thin; scrollbar-color: var(--teal-deep) transparent; }
  .cc-cmd { position: relative; flex: none; display: grid; grid-template-columns: auto 1fr; align-items: baseline; gap: 10px; padding: 5px 10px 5px 12px;
        border: 0; background: none; text-align: left; cursor: pointer; isolation: isolate; }
  .cc-cmd::before { content: ""; position: absolute; inset: 0; z-index: -1; transform: skewX(var(--skew)); border-left: 2px solid transparent; }
  .cc-cmd:hover::before { background: rgba(63, 224, 232, .07); }
  .cc-cmd.focus::before, .cc-cmd:focus-visible::before { background: linear-gradient(90deg, rgba(63, 224, 232, .26), rgba(63, 224, 232, .05)); border-left-color: var(--teal); }
  .cc-cmd:focus { outline: none; }
  .cc-cmd b { font: italic 700 13px/1.2 var(--cond); letter-spacing: .05em; color: var(--teal); white-space: nowrap; }
  .cc-cmd span { font-size: 12px; color: #c1d4db; min-width: 0; }
  .cc-cmd small { display: block; font-size: 11px; color: var(--faint); }
  .cc-adm { margin-left: 6px; font: normal 700 9px/1 var(--cond); letter-spacing: .14em; text-transform: uppercase; color: var(--amber); }
  .cc-row { display: flex; align-items: stretch; gap: 6px; height: 34px; }
  .cc-chtag { flex: none; display: grid; place-items: center; padding: 0 10px; font: italic 700 11px/1 var(--cond); letter-spacing: .16em; text-transform: uppercase;
        color: var(--teal); background: rgba(63, 224, 232, .1); border: 1px solid var(--line); transform: skewX(var(--skew)); max-width: 40%; }
  .cc-chtag > span { transform: skewX(14deg); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 100%; }
  .cc-chtag.pm { color: #ff8fd8; background: rgba(255, 143, 216, .1); border-color: rgba(255, 143, 216, .35); }
  .cc-field { position: relative; flex: 1; min-width: 0; display: flex; align-items: center; isolation: isolate; }
  .cc-field::before { content: ""; position: absolute; inset: 0; z-index: -1; transform: skewX(var(--skew)); background: rgba(0, 0, 0, .45);
        border: 1px solid var(--line); transition: border-color .15s ease, box-shadow .15s ease; }
  .cc-field:focus-within::before { border-color: var(--teal); box-shadow: 0 0 12px rgba(63, 224, 232, .25); }
  .cc-field input { flex: 1; min-width: 0; height: 100%; margin: 0; padding: 0 6px 0 12px; border: 0; border-radius: 0; outline: none; background: transparent;
        color: var(--ink); font: 400 14px/1 var(--body); }
  .cc-field input::placeholder { color: var(--faint); }
  .cc-count { flex: none; visibility: hidden; padding-right: 12px; font: 600 10.5px/1 var(--cond); letter-spacing: .08em; color: var(--faint); }
  .cc-count.show { visibility: visible; }
  .cc-count.near { color: var(--amber); }
  .cc-send { flex: none; padding: 0 16px; border: 0; cursor: pointer; background: var(--teal); color: #031317; transform: skewX(var(--skew));
        font: italic 800 13px/1 var(--cond); letter-spacing: .14em; text-transform: uppercase; box-shadow: 0 0 14px rgba(63, 224, 232, .3); }
  .cc-send > span { display: inline-block; transform: skewX(14deg); }
  .cc-send:hover { background: #8ff3f8; }
  .cc-send:disabled { background: rgba(63, 224, 232, .18); color: rgba(230, 243, 248, .4); box-shadow: none; cursor: default; }
  .cc-send:focus-visible { outline: 2px solid #fff; outline-offset: 2px; }
  .cc-foot { display: flex; align-items: center; justify-content: space-between; gap: 10px; min-height: 16px; font-size: 11px; color: var(--dim); }
  .cc-status { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .cc-status.bad { color: #ffb1a4; }
  .cc-keys { flex: none; display: flex; gap: 10px; font: 700 10px/1 var(--cond); letter-spacing: .12em; text-transform: uppercase; color: var(--faint); }
  .cc-keys kbd { display: inline-grid; place-items: center; min-width: 18px; height: 16px; padding: 0 4px; margin-right: 4px; border: 1px solid rgba(255, 255, 255, .25);
        border-radius: 3px; font: 700 10px/1 var(--cond); color: var(--dim); }
  /* Touch: the feed at the top (touch.js's controls own the bottom corners), the window docked at the bottom above them. */
  .cc.touch { --u: 13px; }
  .cc.touch .cc-chip { min-width: 46px; opacity: .8; }
  /* Touch: the feed at the top under the chip (layout() places it), clear of touch.js's controls at the bottom. */
  .cc.touch .cc-feed { left: var(--feed-x, 16px); top: var(--feed-y, 110px); bottom: auto; max-width: var(--feed-max, calc(100vw - 32px)); }
  .cc.touch .cc-chip kbd { display: none; }
  .cc.touch .cc-box { left: max(8px, env(safe-area-inset-left)); bottom: max(8px, env(safe-area-inset-bottom)); width: min(560px, calc(100vw - 16px)); z-index: 21; padding: 10px;
        border-bottom: 1px solid var(--line); clip-path: polygon(0 0, calc(100% - 16px) 0, 100% 16px, 100% 100%, 16px 100%, 0 calc(100% - 16px)); }
  .cc.touch .cc-history, .cc.touch .cc-suggestions { max-height: 30dvh; }
  .cc.touch .cc-row { height: 44px; }
  .cc.touch .cc-field input { font-size: 16px; } /* 16 px: iOS does not zoom into the field */
  .cc.touch .cc-send { padding: 0 18px; }
  .cc.touch .cc-ib { width: 40px; height: 34px; font-size: 16px; }
  .cc.touch .cc-keys { display: none; }
  .cc.touch .cc-foot { min-height: 18px; font-size: 12px; }
  /* A phone held sideways: the keyboard leaves room for the message line alone, at the top of what is
     visible. Closing the keyboard brings the whole window back, with its close button. */
  .cc.touch.keyboard .cc-box { padding: 6px; }
  .cc.touch.keyboard .cc-head, .cc.touch.keyboard .cc-history, .cc.touch.keyboard .cc-foot { display: none; }
  .cc.touch.keyboard .cc-suggestions { max-height: var(--kb-room, 64px); }
  @media (max-height: 500px) {
    .cc.touch .cc-history, .cc.touch .cc-suggestions { max-height: max(64px, 32dvh); }
  }
  @keyframes cc-in { from { opacity: 0; translate: -10px 0; } }
  @keyframes cc-flash { to { opacity: 0; } }
  @keyframes cc-up { from { translate: 0 calc(100% + 24px); } }
  @keyframes cc-down { to { translate: 0 calc(100% + 24px); } }
  @media (prefers-reduced-motion: reduce) {
    .cc-ln, .cc-box, .cc-box.cc-closing { animation: none; } .cc-ln.cc-new::after { display: none; } .cc-ln { transition: opacity .2s linear; }
  }
  `;

  let root, feed, chip, composer, messageInput, composeStatus, history;
  let composing = false, unread = 0;
  // Touch screens get the touch layout, as touch.js shows its controls.
  const touch = !!navigator.maxTouchPoints;
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
    root.className = touch ? "cc touch" : "cc";
    root.hidden = !enabled;
    root.innerHTML = `
    <style>${css}</style>
    <button class="cc-chip" type="button" aria-label="Open online chat" title="Chat"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M2.5 3.5h11v7h-6l-3 2.5v-2.5h-2z" stroke-linejoin="round"/></svg><kbd>Y</kbd><b class="cc-unread" hidden>0</b></button>
    <div class="cc-feed" role="log" aria-live="polite" aria-label="Chat"></div>
    <form class="cc-box" role="dialog" aria-modal="true" aria-label="Online chat" autocomplete="off" hidden>
      <div class="cc-head"><h2>Chat</h2><span class="cc-channel">Everyone in this room</span><span class="cc-rule"></span><button class="cc-ib cc-commands" type="button" aria-expanded="false" aria-label="Commands" title="Commands (/)"><span>/</span></button><button class="cc-ib cc-close" type="button" aria-label="Close chat" title="Close (Esc)"><span>✕</span></button></div>
      <div class="cc-history" role="log" aria-label="Session messages" tabindex="0"><p class="cc-empty">Say hello to the room. Type / to invite friends or send a private message.</p></div>
      <div class="cc-suggestions" role="listbox" aria-label="Chat commands" hidden></div>
      <div class="cc-row"><span class="cc-chtag"><span>Room</span></span><label class="cc-field"><input id="cc-message" maxlength="120" autocomplete="off" enterkeyhint="send" placeholder="Message, or / for commands" aria-label="Message"><span class="cc-count" aria-hidden="true">0/120</span></label><button class="cc-send" type="submit" disabled><span>Send</span></button></div>
      <div class="cc-foot"><span class="cc-status" role="status"></span><span class="cc-keys"><span><kbd>↵</kbd>Send</span><span><kbd>Esc</kbd>Back</span></span></div>
    </form>`;
    document.body.append(root);
    addEventListener("resize", layout);
    history = root.querySelector(".cc-history");
    feed = root.querySelector(".cc-feed");
    chip = root.querySelector(".cc-chip");
    composer = root.querySelector(".cc-box");
    messageInput = composer.querySelector("input");
    composeStatus = composer.querySelector("[role=status]");
    composer.querySelector(".cc-close").addEventListener("click", closeComposer);
    chip.addEventListener("click", openComposer);
    composer.querySelector(".cc-commands").addEventListener("click", () => {showCommands = !showCommands; cmdFocus = 0; renderCommands(); messageInput.focus();});
    messageInput.addEventListener("input", () => { cmdFocus = 0; arrowed = false; updateComposer(); });
    // With the command list up: arrows move its highlight, Tab (or Enter on a
    // part-typed command) fills the highlighted one in. Enter on a whole command
    // name (/players) runs it, unless the arrows picked another one.
    messageInput.addEventListener("keydown", e => {
      if (composer.querySelector(".cc-suggestions").hidden) return;
      const rows = commandRows().rows;
      if (!rows.length) return;
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        cmdFocus = (cmdFocus + (e.key === "ArrowDown" ? 1 : rows.length - 1)) % rows.length;
        arrowed = true;
        renderCommands();
        return;
      }
      const row = rows[Math.min(cmdFocus, rows.length - 1)], value = messageInput.value;
      const whole = !arrowed && rows.some(([name]) => value === `/${name}`);
      if ((e.key === "Tab" && !e.shiftKey) || (e.key === "Enter" && (value === "" || /^\/[a-z]*$/i.test(value)) && !whole)) {
        e.preventDefault();
        pickCommand(row[0], row[2]);
      }
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
      if (type !== "keydown" || e.code !== "Escape") return;
      e.preventDefault();
      if (showCommands && !composer.querySelector(".cc-suggestions").hidden) { showCommands = false; renderCommands(); messageInput.focus(); }
      else closeComposer();
    });
    // On a phone the on-screen keyboard covers the bottom: keep the window above it.
    window.visualViewport?.addEventListener("resize", placeComposer);
    window.visualViewport?.addEventListener("scroll", placeComposer);
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

  // ---- The feed and the history ----
  // A name in its crew's colour, the one net.js gives the crew on the name
  // tags and the badge (window.recompCrewColor); teal without a crew. The
  // colour is the line's --c, which also colours its left edge.
  function who(name, crew) {
    const b = document.createElement("b");
    b.className = "cc-nm";
    b.textContent = name;
    if (crew) b.title = `Crew ${crew}`;
    return b;
  }
  function span(cls, text) {
    const s = document.createElement("span");
    s.className = cls;
    s.textContent = text;
    return s;
  }
  function kbd(text) {
    const k = document.createElement("kbd");
    k.textContent = text;
    return k;
  }
  // One line, for the feed or (with its time) the history. item: {kind, tag,
  // name, crew, text, keys: [[key, what]], at}. kind: "" (a message), mine,
  // pm (private), sys (replies, crewmates coming and going), admin, err, hint.
  function build(item, withTime) {
    const el = document.createElement("div");
    el.className = item.kind ? `cc-ln ${item.kind}` : "cc-ln";
    const color = item.crew && ["", "mine", "pm"].includes(item.kind || "") ? window.recompCrewColor?.(item.crew) : "";
    if (color) el.style.setProperty("--c", color);
    const body = span("cc-body", "");
    if (item.tag) body.append(span("cc-tag", item.tag));
    if (item.name) body.append(who(item.name, item.crew));
    for (const [key, what] of item.keys || []) body.append(kbd(key), what, span("cc-gap", ""));
    if (item.text) body.append(span("cc-tx", item.text));
    el.append(body);
    if (withTime) {
      const time = document.createElement("time");
      time.dateTime = item.at.toISOString();
      time.textContent = item.at.toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"});
      el.append(time);
    }
    return el;
  }
  // Over the game: the newest at the bottom, at most FEED_LINES, each fading
  // out on its own. Returns a function that takes the line away early.
  function toFeed(item, ms = SHOW_MS) {
    const el = build(item, false);
    el.classList.add("cc-new");
    feed.append(el);
    while (feed.children.length > FEED_LINES) feed.firstElementChild.remove();
    let gone = false;
    const out = () => {
      if (gone) return;
      gone = true;
      el.classList.add("cc-out");
      setTimeout(() => el.remove(), 650);
    };
    setTimeout(out, ms);
    return out;
  }
  const COUNTED = new Set(["", "pm", "admin"]); // what the chip's unread number counts
  function showUnread() {
    if (!chip) return;
    const badge = chip.querySelector(".cc-unread");
    badge.hidden = !unread;
    badge.textContent = unread > 99 ? "99+" : String(unread);
    chip.classList.toggle("lit", unread > 0);
    chip.setAttribute("aria-label", unread ? `Open online chat, ${unread} new` : "Open online chat");
  }
  // A line for the history (kept until reload) and, while the window is
  // closed, for the feed. Received text goes in with textContent only.
  function say(name, crew, text, kind = "", tag = "") {
    const item = { kind, tag, name, crew, text, at: new Date() };
    const follow = history.scrollHeight - history.scrollTop - history.clientHeight < 40;
    history.querySelector(".cc-empty")?.remove();
    history.append(build(item, true));
    while (history.children.length > HISTORY_LIMIT) history.firstElementChild.remove();
    if (composing) { if (follow) history.scrollTop = history.scrollHeight; return; }
    toFeed(item);
    if (COUNTED.has(kind)) { unread++; showUnread(); }
  }

  // The keys, once the player is in a car and again whenever they move (the
  // old hint goes, so no line names a key that no longer works). Hints are not
  // kept in the history.
  let hinted = false, unhint = [];
  function hint() {
    hinted = true;
    for (const out of unhint.splice(0)) out();
    const keys = [];
    if (chatKey && !touch) keys.push([label(chatKey), "Chat"]);
    if (hornKey) keys.push([label(hornKey), "Horn (hold)"]);
    if (keys.length) unhint.push(toFeed({ kind: "hint", keys }, HINT_MS));
    const all = (codes) => `${codes.slice(0, -1).map(label).join(", ")} and ${label(codes.at(-1))}`;
    if (!chatKey) unhint.push(toFeed({ kind: "sys", text: `Chat off: ${all(CHAT_KEYS)} are all bound in Controls` }, HINT_MS));
    if (!hornKey) unhint.push(toFeed({ kind: "sys", text: `Horn off: ${all(HORN_KEYS)} are all bound in Controls` }, HINT_MS));
  }

  // Who is here, from the online client's list: a horn stops when its player
  // leaves. Only crewmates get a "joined"/"left" line: a room can hold a
  // hundred players or more, and the feed would be nothing else (the user,
  // 2026-10-24; crewmates only, 2026-11-04). The first list after connecting
  // is who was already there, so it says nothing; several at once make one line.
  let known = null;
  const crewKeyOf = (c) => window.recompCrewKey ? window.recompCrewKey(c) : String(c || "").toLowerCase().replace(/\s+/g, "");
  const lastSaid = new Map(); // "name|what" -> when, so a flaky connection does not repeat itself
  function people(s) {
    if (!s || !active) return;
    if (!s.open) {
      known = null;
      for (const id of [...horns.keys()]) stopRemote(id);
      return;
    }
    const here = new Map((s.people || []).map((p) => [p.id, p]));
    for (const id of [...horns.keys()]) if (!here.has(id)) stopRemote(id);
    const crew = crewKeyOf(myCrew());
    if (known && crew && enabled) {
      const mate = (p) => p && p.name && crewKeyOf(p.crew) === crew;
      comings([...here.values()].filter((p) => !known.has(p.id) && mate(p)), "joined the room");
      comings([...known.values()].filter((p) => !here.has(p.id) && mate(p)), "left the room");
    }
    known = here;
  }
  function comings(list, what) {
    const now = performance.now();
    list = list.filter((p) => !(now - (lastSaid.get(`${p.name}|${what}`) ?? -Infinity) < 60000));
    for (const p of list) lastSaid.set(`${p.name}|${what}`, now);
    if (list.length > 2) say("", "", `${list.length} crewmates ${what}`, "sys", "Crew");
    else for (const p of list) say(p.name, p.crew, what, "sys", "Crew");
  }

  function heard(m) {
    if (!m || typeof m !== "object") return;
    if (m.t === "chat") {
      if (!enabled) return;
      const text = cleanText(m.text), me = window.recompOnlineState?.me, fromMe = me != null && m.id === me;
      if (m.error) say("", "", m.error === "profanity" ? "Message blocked. Swearing is not allowed." : m.error === "muted" ? `Chat muted until ${new Date(m.until * 1000).toLocaleTimeString()}.` : "Message was not delivered. The player may have left, or you sent messages too quickly.", "err", m.error === "profanity" ? "Blocked" : m.error === "muted" ? "Muted" : "Not sent");
      else if (text && m.to) say(fromMe ? `To ${m.targetName}` : m.name || "Player", m.crew, text, "pm", "Private");
      else if (text) say(m.name || "Player", m.crew, text, fromMe ? "mine" : "");
      if (composing && (m.error || m.ack || m.to && fromMe)) {
        composeStatus.classList.toggle("bad", !!m.error);
        composeStatus.textContent = m.error === "profanity" ? "Message blocked. Swearing is not allowed." : m.error === "muted" ? "You are muted. See the message above for the end time." : m.error ? "Message was not delivered." : "Message delivered.";
      }
    } else if (m.t === "moderation" && m.k === "disconnect") {
      say("", "", `${m.text} Reload to reconnect.`, "err", "Disconnected");
      if (composing) {composeStatus.textContent = "Disconnected. Press Esc to return to the game."; composeStatus.classList.add("bad");updateComposer();}
    } else if (m.t === "moderation" && m.k === "announcement") {
      say("Announcement", "", String(m.text ?? ""), "admin", "Admin");
    } else if (m.t === "horn") {
      remoteHorn(m);
    }
  }

  function openComposer() {
    if (!active || !enabled || !linkOpen() || menuOpen()) return;
    hornUp();
    composing = true;
    clearTimeout(closing);
    composer.classList.remove("cc-closing");
    composer.hidden = false;
    root.classList.add("composing");
    globalThis.recompMenuOpen = true; // suppress controller driving while typing
    unread = 0;
    showUnread();
    feed.replaceChildren(); // all of it is in the history
    composeStatus.classList.remove("bad");
    composeStatus.textContent = "Messages stay here until you reload.";
    updateComposer();
    placeComposer();
    history.scrollTop = history.scrollHeight;
    messageInput.focus();
  }
  function closeComposer() {
    if (!composing) return;
    composing = false;
    showCommands = false;
    // Slides down, then hides (at once with reduced motion: no animation runs).
    composer.classList.add("cc-closing");
    clearTimeout(closing);
    closing = setTimeout(() => { composer.classList.remove("cc-closing"); if (!composing) composer.hidden = true; },
      matchMedia("(prefers-reduced-motion: reduce)").matches ? 0 : 200);
    root.classList.remove("composing");
    globalThis.recompMenuOpen = menuOpen();
    if (!menuOpen()) document.getElementById("canvas")?.focus();
    if (touch) scrollTo(0, 0); // iOS can leave the page pushed up where the keyboard moved it
  }
  let closing = 0;
  // The chip joins net.js's online badge (top-left, right of the ☰); on touch
  // screens the feed hangs under the chip (elsewhere it sits above the minimap,
  // in CSS). The chip right of the badge, the feed under the chip, both clear of the
  // game's own heat/crew icon at the top left (x 55-250, y 45-95 at 1280x720,
  // measured 2026-11-04; the HUD scales with the screen's height, so: right
  // edge 35% of the height, bottom 13.5%). Where the row has no room (a phone
  // held upright, a narrow window), the chip goes under the ☰ and the feed
  // under both, at the left edge. Run every tick; only a change moves anything.
  let placed = "";
  function layout() {
    if (!root) return;
    const badge = document.querySelector('button[aria-label="Online players (F8)"]');
    const r = badge && badge.getClientRects().length ? badge.getBoundingClientRect() : null;
    const W = innerWidth, H = innerHeight;
    const key = `${W}x${H}:${r ? [r.left, r.top, r.right, r.bottom].map(Math.round) : "-"}`;
    if (key === placed) return;
    placed = key;
    const top = r ? Math.round(r.top) : 10, h = r ? Math.round(r.height) : 32, right = r ? Math.round(r.right) : 54;
    const iconRight = Math.round(H * 0.35) + 8, iconBottom = Math.round(H * 0.135) + 6;
    let chipX = right + 8, chipY = top, feedX = Math.max(chipX, iconRight), feedY = top + h + 6;
    const beside = chipX + 84 <= W - 16;
    if (!beside) { chipX = 14; chipY = top + h + 6; }
    if (!beside || W - feedX - 16 < 240) { feedX = 16; feedY = Math.max((beside ? top : chipY) + h + 6, iconBottom); }
    const set = (name, px) => root.style.setProperty(name, `${px}px`);
    set("--chip-x", chipX); set("--chip-y", chipY); set("--chip-h", h);
    set("--feed-x", feedX); set("--feed-y", feedY); set("--feed-max", W - feedX - 16);
  }
  // Touch: the docked window sits above the on-screen keyboard (what of the
  // layout viewport the visual viewport no longer shows at the bottom). With too
  // little height left for all of it, only the message line shows, measured from
  // the top of the visible area, so what is typed stays in view.
  const KEYBOARD_MIN = 60, ROOM_FOR_ALL = 260;
  function placeComposer() {
    if (!composer || !touch) return;
    const vv = window.visualViewport;
    const covered = vv ? Math.round(innerHeight - vv.height - vv.offsetTop) : 0;
    const tight = !!vv && innerHeight - vv.height > KEYBOARD_MIN && vv.height < ROOM_FOR_ALL;
    root.classList.toggle("keyboard", tight);
    root.style.setProperty("--kb-room", `${vv ? Math.max(40, Math.round(vv.height) - 76) : 64}px`);
    composer.style.top = tight ? `${Math.round(vv.offsetTop) + 6}px` : "";
    composer.style.bottom = tight ? "auto" : covered > 0 ? `calc(${covered}px + 8px)` : "";
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
  let cmdFocus = 0, arrowed = false;
  // The commands the list shows: all of them (the / button, /help) or those
  // starting with what is typed after a lone "/".
  function commandRows() {
    const prefix = /^\/([a-z]*)$/i.exec(messageInput.value)?.[1]?.toLowerCase();
    return { prefix, rows: commands.filter(([name,,,adminOnly]) => (!adminOnly || window.recompOnlineState?.admin) && (prefix === undefined || name.startsWith(prefix))) };
  }
  function renderCommands() {
    if (!composer) return;
    const list = composer.querySelector(".cc-suggestions"), { prefix, rows } = commandRows();
    const visible = (showCommands || prefix !== undefined) && rows.length > 0, was = !list.hidden;
    list.replaceChildren(); list.hidden = !visible;
    history.hidden = visible; // the list takes the history's place
    if (was && !visible) history.scrollTop = history.scrollHeight;
    composer.querySelector(".cc-commands").setAttribute("aria-expanded", String(visible));
    if (!visible) return;
    cmdFocus = Math.min(cmdFocus, rows.length - 1);
    rows.forEach(([name, description, syntax, adminOnly], i) => {
      const button = document.createElement("button"), title = document.createElement("b"), hint = document.createElement("span");
      button.type = "button"; button.className = i === cmdFocus ? "cc-cmd focus" : "cc-cmd";
      button.setAttribute("role", "option"); button.setAttribute("aria-selected", String(i === cmdFocus));
      title.textContent = `/${name}`; hint.textContent = description;
      if (adminOnly) hint.append(span("cc-adm", "Admin"));
      if (syntax) { const small = document.createElement("small"); small.textContent = `/${name} ${syntax}`; hint.append(small); }
      button.title = `/${name}${syntax ? " " + syntax : ""}`; button.append(title, hint);
      button.addEventListener("click", () => pickCommand(name, syntax));
      list.append(button);
    });
    const at = list.children[cmdFocus];
    if (at && (at.offsetTop < list.scrollTop || at.offsetTop + at.offsetHeight > list.scrollTop + list.clientHeight)) list.scrollTop = at.offsetTop - list.clientHeight / 2;
  }
  function pickCommand(name, syntax) {
    messageInput.value = `/${name} `; showCommands = false; cmdFocus = 0; arrowed = false; updateComposer(); messageInput.focus();
    composeStatus.classList.remove("bad");
    composeStatus.textContent = `/${name}${syntax ? " " + syntax : ""}`;
  }
  function updateComposer() {
    if (!composer) return;
    const n = messageInput.value.length, count = composer.querySelector(".cc-count");
    count.textContent = `${n}/120`;
    count.classList.toggle("show", n >= 90); // only near the limit, so the field keeps its width
    count.classList.toggle("near", n >= 110);
    composer.querySelector(".cc-send").disabled = commandBusy || !messageInput.value.trim() || !linkOpen();
    const pm = /^\/w\s+(?:"([^"\n]+)"|(\S+))/i.exec(messageInput.value), tag = composer.querySelector(".cc-chtag");
    tag.classList.toggle("pm", !!pm);
    tag.firstElementChild.textContent = pm ? `To ${pm[1] || pm[2]}` : "Room";
    composer.querySelector(".cc-channel").textContent = /^\/w\s/i.test(messageInput.value) ? "Private · only you and the recipient" : "Everyone in this room";
    renderCommands();
  }

  const commandReply = text => say("", "", text, "sys");
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
      layout();
      chip.hidden = menu || !linkOpen();
      const key = chip.querySelector("kbd"); key.hidden = touch || !chatKey || !drivingShortcuts(); if (chatKey) key.textContent = label(chatKey);
      chip.title = chatKey && !key.hidden ? `Chat (${label(chatKey)})` : "Chat";
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
