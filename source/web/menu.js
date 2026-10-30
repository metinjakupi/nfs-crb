// menu.js - Need for Speed: Carbon's in-game menu in the browser, the one menu
// there is: F9 or F10 (fn+F9 / fn+F10 on a Mac keyboard), the ☰ button, or the
// controller's settings chord opens it. It holds back to the game, full screen,
// other scripts' switches (window.recompMenuToggles, e.g. radar.js's radar) and
// buttons (window.recompMenuActions, e.g. race.js's race together), who is
// online and your crew (changed here while driving), the settings, and quit to
// the start page. The start page (start.js) loads it when the player presses
// Play; the game's own Esc menu is untouched.
//
// SETTINGS. Three pages, Display & graphics, Controls and Game, with the
// settings the kit's F10 page had: game.toml [settings] rows (frame limit,
// performance overlay, world filtering; its Window row is the browser's full
// screen here) and the mods' mod.toml [settings]. They come from the game
// through the kit's settings block (kit/mods/web_settings.cpp hands it over as
// globalThis.recompSettingsBlock with a "recomp-settings" event): labels, help,
// choices, limits and values. A change goes back the same way and is saved in
// the profile's mod-settings.json exactly as F10 saved it; a "live" setting
// changes the game at once, a "restart" one at the next start (its row says so
// until then). PAGES below orders the known rows; a row it does not name still
// shows, in its page's More section. The kit's own F10 page no longer opens in
// the browser: F10 and the controller's chord (Back + Start, or a DualSense
// touchpad click; the kit sends a "recomp-menu" event) open this menu instead.
//
// While the menu is open the game gets no key presses (a key held before
// still gets its release), so arrows and Enter only move through the menu and
// typing goes into the crew field.
//
// CONTROLLER. The menu reads the controller itself while it is open (padTick):
// D-pad or left stick move, left/right change a setting, ✕ (A) chooses, ○ (B)
// goes back and on the first page closes the menu. Meanwhile the game is given
// a pad with nothing held (the kit asks globalThis.recompMenuOpen), and a
// button still held when the menu closes reaches the game once it is let go.
// The touchpad click, or Create + Options (View + Menu), opens and closes the
// menu through the kit; not the PS button, which macOS keeps for itself.
// Controls → Controller check shows what the browser and the game see of the
// controller (the kit sets globalThis.recompGameController when the game opens
// one), for a player to test theirs.
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

  const css = `
  .cm { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --amber: #f2b45f; --line: rgba(95, 228, 242, .22);
        font: 15px/1.45 "Avenir Next", "Segoe UI", system-ui, sans-serif; color: var(--ink); }
  .cm * { box-sizing: border-box; }
  .cm-open { position: fixed; top: 10px; left: 14px; z-index: 6; width: 40px; height: 32px; cursor: pointer;
        font: 700 17px/1 system-ui, sans-serif; color: var(--ink); background: rgba(6, 12, 22, .62);
        border: 1px solid var(--line); transform: skewX(-12deg); opacity: 0; transition: opacity .3s ease, border-color .15s ease; }
  .cm-open.shown, .cm-open:focus-visible { opacity: .9; }
  .cm-open:hover { border-color: var(--cyan); opacity: 1; }
  .cm-open > span { display: inline-block; transform: skewX(12deg); }
  .cm-shade { position: fixed; inset: 0; z-index: 30; display: grid; place-items: center; padding: 16px;
        background: radial-gradient(80% 80% at 50% 40%, rgba(4, 10, 20, .72), rgba(0, 1, 4, .9)); }
  .cm-panel { width: min(500px, 100%); background: rgba(6, 12, 22, .9); border: 1px solid var(--line); padding: 22px 22px 18px;
        max-height: calc(100vh - 32px); overflow-y: auto; scrollbar-width: thin; scrollbar-color: var(--line) transparent;
        position: relative; clip-path: polygon(0 0, calc(100% - 18px) 0, 100% 18px, 100% 100%, 18px 100%, 0 calc(100% - 18px)); }
  .cm-panel::before, .cm-panel::after { content: ""; position: absolute; width: 26px; height: 1px; background: var(--line);
        transform-origin: left center; transform: rotate(45deg); }
  .cm-panel::before { left: calc(100% - 19px); top: -1px; }
  .cm-panel::after { left: -1px; top: calc(100% - 19px); }
  .cm h2, .cm h3 { margin: 0 0 12px; font: italic 800 15px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
        letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); display: flex; align-items: center; gap: 10px; }
  .cm h3 { font-size: 12px; margin: 16px 0 8px; }
  .cm h2::after, .cm h3::after { content: ""; flex: 1; height: 2px; transform: skewX(-30deg);
        background: linear-gradient(90deg, var(--cyan), transparent); opacity: .5; }
  .cm-item { display: flex; width: 100%; align-items: center; justify-content: space-between; gap: 12px; margin: 0 0 6px;
        font: italic 700 16px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .08em;
        text-transform: uppercase; color: var(--ink); background: rgba(95, 228, 242, .04); border: 1px solid transparent;
        padding: 12px 14px; cursor: pointer; text-align: left; transform: skewX(-8deg); transition: background .12s ease, border-color .12s ease; }
  .cm-item > * { transform: skewX(8deg); }
  .cm-item:hover, .cm-item:focus-visible { outline: none; background: rgba(95, 228, 242, .14); border-color: var(--cyan); }
  .cm-item .cm-val, .cm-item .cm-hint { font: 600 12px/1 system-ui, sans-serif; letter-spacing: .06em; color: var(--dim); text-transform: none; }
  .cm-item .cm-val.on { color: var(--cyan); }
  .cm-item .cm-hint.restart { color: var(--amber); }
  .cm-go::after { content: "›"; font: 700 18px/1 system-ui, sans-serif; color: var(--dim); transform: skewX(8deg); }
  .cm-go:hover::after, .cm-go:focus-visible::after { color: var(--cyan); }
  .cm-go .cm-hint { margin-left: auto; }
  .cm-who { font-size: 13px; color: var(--dim); margin: 0 0 8px; }
  .cm-who b { color: var(--ink); font-weight: 600; }
  .cm-crews { display: grid; gap: 6px; margin: 0 0 10px; max-height: max(120px, calc(100vh - 430px)); overflow-y: auto; padding-left: 4px; }
  .cm-crews.more { -webkit-mask-image: linear-gradient(#000 calc(100% - 26px), transparent); mask-image: linear-gradient(#000 calc(100% - 26px), transparent); }
  .cm-crew { --c: var(--dim); padding: 7px 12px 8px; background: rgba(255, 255, 255, .035); border-left: 3px solid var(--c);
        transform: skewX(-8deg); }
  .cm-crew > * { transform: skewX(8deg); }
  .cm-crew.mine { background: color-mix(in srgb, var(--c) 13%, transparent); box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--c) 45%, transparent); }
  .cm-crew-head { display: flex; align-items: center; gap: 8px; color: var(--c);
        font: italic 800 13px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .14em; text-transform: uppercase; }
  .cm-crew-head::before { content: ""; width: 8px; height: 8px; flex: none; background: var(--c); transform: skewX(-12deg); }
  .cm-crew.none .cm-crew-head { font-weight: 700; }
  .cm-crew.none .cm-crew-head::before { background: transparent; border: 1px solid var(--c); }
  .cm-crew-tag { font: 700 10px/1 system-ui, sans-serif; letter-spacing: .08em; padding: 3px 6px; color: #04101a; background: var(--c); }
  .cm-crew-n { margin-left: auto; font: 600 12px/1 system-ui, sans-serif; letter-spacing: .04em; color: var(--dim); }
  .cm-crew-names { margin-top: 5px; font-size: 13px; color: var(--ink); overflow-wrap: anywhere; }
  .cm-crew-names b { color: var(--c); font-weight: 600; }
  .cm-crew.none .cm-crew-names b { color: var(--ink); }
  .cm-crew-now { display: flex; align-items: center; gap: 10px; min-width: 0; }
  .cm-chip { --c: var(--dim); padding: 3px 7px; border: 1px solid var(--c); color: var(--c); white-space: nowrap;
        background: color-mix(in srgb, var(--c) 14%, transparent); letter-spacing: .1em; text-transform: uppercase;
        font: italic 700 12px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; }
  .cm-chip.none { border-style: dashed; background: transparent; }
  .cm-confirm, .cm-crew-form { margin-top: 8px; padding: 12px; border: 1px solid var(--line); background: rgba(0, 0, 0, .3); }
  .cm-crew-form { margin: -2px 0 6px; }
  .cm-confirm p, .cm-crew-form p { margin: 0 0 10px; font-size: 13px; color: var(--ink); }
  .cm-confirm .cm-item, .cm-crew-form .cm-item { margin: 0; }
  .cm-crew-form .cm-item { width: auto; flex: none; padding: 11px 18px; }
  .cm-crew-form input { flex: 1; min-width: 0; font: 15px/1.2 "Avenir Next", "Segoe UI", system-ui, sans-serif; color: var(--ink);
        background: rgba(0, 0, 0, .45); border: 1px solid var(--line); padding: 9px 11px; border-radius: 0; outline: none; }
  .cm-crew-form input:focus { border-color: var(--cyan); box-shadow: 0 0 0 1px var(--cyan); }
  .cm-crew-form input::placeholder { color: var(--dim); }
  .cm-crew-form .cm-crew-note { margin: 8px 0 0; font-size: 12px; color: var(--dim); }
  .cm-crew-form .cm-crew-note.bad { color: var(--amber); }
  .cm-row { display: flex; gap: 8px; }
  .cm-quit { margin-top: 16px; }
  .cm-quit-yes { border-color: rgba(255, 93, 122, .5); }
  .cm-quit-yes:hover, .cm-quit-yes:focus-visible { background: rgba(255, 93, 122, .16); border-color: #ff5d7a; }
  .cm-foot { margin-top: 12px; font-size: 12px; color: var(--dim); }
  .cm-up { flex: none; width: 30px; height: 26px; margin-left: -4px; cursor: pointer; color: var(--cyan); background: transparent;
        border: 1px solid var(--line); transform: skewX(-12deg); font: 700 17px/1 system-ui, sans-serif; }
  .cm-up > span { display: inline-block; transform: skewX(12deg); }
  .cm-up:hover, .cm-up:focus-visible { outline: none; border-color: var(--cyan); background: rgba(95, 228, 242, .14); }
  .cm-note { margin: -4px 0 10px; font-size: 13px; color: var(--dim); }
  .cm-note.restart { color: var(--amber); }
  .cm-set-row { padding: 10px 12px 10px 14px; font-size: 15px; scroll-margin: 44px 0 150px; } /* clear of the help bar */
  .cm-set-row .cm-label { flex: 1; min-width: 0; line-height: 1.15; }
  .cm-ctl { display: flex; align-items: center; gap: 2px; flex: none; }
  .cm-ctl .cm-val { min-width: 92px; text-align: center; font: 600 13px/1.2 system-ui, sans-serif; letter-spacing: .02em; color: var(--ink); }
  .cm-ctl .cm-val.on { color: var(--cyan); }
  .cm-set-row.pending .cm-val { opacity: .55; }
  .cm-arr { font: 700 17px/1 system-ui, sans-serif; color: var(--dim); padding: 2px 6px; opacity: .35; }
  .cm-set-row:hover .cm-arr, .cm-set-row:focus-visible .cm-arr { opacity: 1; color: var(--cyan); }
  .cm-arr:hover { background: rgba(95, 228, 242, .18); }
  .cm-tag { font: 700 9px/1 system-ui, sans-serif; letter-spacing: .1em; padding: 3px 5px; margin-right: 6px; color: #1a1204;
        background: var(--amber); text-transform: uppercase; }
  .cm-help { position: sticky; bottom: -18px; margin: 12px -22px -18px; padding: 12px 22px 16px 26px; min-height: 76px;
        background: rgb(7, 14, 25); border-top: 1px solid var(--line); box-shadow: 0 -10px 14px rgba(6, 12, 22, .9);
        font-size: 13px; line-height: 1.4; color: var(--ink); }
  .cm-help b { display: block; margin-bottom: 4px; font: italic 700 11px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
        letter-spacing: .16em; text-transform: uppercase; color: var(--cyan); }
  .cm-help b.restart { color: var(--amber); }
  .cm-help .cm-keys { display: block; margin-top: 6px; font-size: 11px; color: var(--dim); }
  .cm-help .cm-restart { display: block; margin-top: 6px; font-size: 12px; color: var(--amber); }
  .cm.cm-pad .cm-item:focus, .cm.cm-pad .cm-up:focus { outline: none; background: rgba(95, 228, 242, .14); border-color: var(--cyan); }
  .cm.cm-pad .cm-set-row:focus .cm-arr { opacity: 1; color: var(--cyan); }
  .cm-check-go::after { content: "›"; font: 700 18px/1 system-ui, sans-serif; color: var(--dim); transform: skewX(8deg); }
  .cm-check-go { margin-bottom: 10px; }
  .cm-pad-name { margin: 0 0 6px; font-size: 16px; color: var(--ink); overflow-wrap: anywhere; }
  .cm-pad-name.none { color: var(--amber); }
  .cm-pad-sub { margin: 0 0 4px; font-size: 13px; color: var(--dim); }
  .cm-pad-sub.good { color: var(--cyan); }
  .cm-pad-sub.bad { color: var(--amber); }
  .cm-pad-keys { display: flex; flex-wrap: wrap; gap: 6px; margin: 14px 0 12px; }
  .cm-pad-key { min-width: 44px; padding: 8px 9px; text-align: center; font: 700 13px/1 system-ui, sans-serif; color: var(--dim);
        border: 1px solid var(--line); background: rgba(255, 255, 255, .03); transform: skewX(-8deg); }
  .cm-pad-key.on { color: #04101a; background: var(--cyan); border-color: var(--cyan); }
  .cm-pad-axes { margin: 0 0 10px; font: 13px/1.6 ui-monospace, Menlo, Consolas, monospace; color: var(--ink); white-space: pre; overflow-x: auto; }
  .cm [hidden] { display: none !important; }
  @media (prefers-reduced-motion: reduce) { .cm-open, .cm-item { transition: none; } }
  `;

  const root = document.createElement("div");
  root.className = "cm";
  root.innerHTML = `
  <style>${css}</style>
  <button class="cm-open" type="button" aria-label="Menu (F9 or F10)" title="Menu (F9 or F10)"><span>☰</span></button>
  <div class="cm-shade" hidden>
    <nav class="cm-panel" role="dialog" aria-modal="true" aria-label="Menu">
      <div class="cm-main">
        <h2>Menu</h2>
        <button class="cm-item cm-back" type="button"><span>Back to the game</span><span class="cm-hint">F9 · F10 · Esc</span></button>
        <button class="cm-item cm-full" type="button"><span>Full screen</span><span class="cm-val"></span></button>
        <div class="cm-toggles"></div>
        <section class="cm-online" hidden>
          <h3>Online</h3>
          <div class="cm-who"></div>
          <div class="cm-crews"></div>
          <button class="cm-item cm-crew-edit" type="button"><span>Your crew</span><span class="cm-crew-now"></span></button>
          <div class="cm-crew-form" hidden>
            <p>Players who type the same crew name are one crew. Leave it empty for no crew.</p>
            <div class="cm-row">
              <input class="cm-crew-in" type="text" maxlength="16" autocomplete="off" spellcheck="false" placeholder="Crew name" aria-label="Crew name">
              <button class="cm-item cm-crew-ok" type="button"><span>Set</span></button>
            </div>
            <p class="cm-crew-note" role="status" hidden></p>
          </div>
        </section>
        <div class="cm-extra"></div>
        <h3>Settings</h3>
        <button class="cm-item cm-go" type="button" data-page="display"><span>Display &amp; graphics</span><span class="cm-hint restart"></span></button>
        <button class="cm-item cm-go" type="button" data-page="controls"><span>Controls</span><span class="cm-hint restart"></span></button>
        <button class="cm-item cm-go" type="button" data-page="game"><span>Game</span><span class="cm-hint restart"></span></button>
        <button class="cm-item cm-quit" type="button"><span>Quit to the start page</span><span class="cm-hint"></span></button>
        <div class="cm-confirm cm-quit-box" hidden>
          <p>The game closes. Your career stays as the game last saved it.</p>
          <div class="cm-row">
            <button class="cm-item cm-quit-yes" type="button"><span>Quit</span></button>
            <button class="cm-item cm-quit-no" type="button"><span>Stay</span></button>
          </div>
        </div>
        <div class="cm-foot">The game's own menu (pause, options) is on Esc while you drive.</div>
      </div>
      <div class="cm-set" hidden>
        <h2><button class="cm-up" type="button" aria-label="Back to the menu" title="Back (Esc)"><span>‹</span></button><span class="cm-set-title"></span></h2>
        <p class="cm-note cm-set-note" hidden></p>
        <div class="cm-rows"></div>
        <button class="cm-item cm-reset" type="button" hidden><span>Restore this page's defaults</span></button>
        <div class="cm-confirm cm-reset-box" hidden>
          <p>Put every setting on this page back to how the game came?</p>
          <div class="cm-row">
            <button class="cm-item cm-reset-yes" type="button"><span>Restore</span></button>
            <button class="cm-item cm-reset-no" type="button"><span>Keep</span></button>
          </div>
        </div>
        <div class="cm-help" role="status"><b></b><span class="cm-help-text"></span>
          <span class="cm-restart" hidden>Changes marked Restart take effect the next time the game starts: Quit to the start page, then Play.</span>
          <span class="cm-keys"></span></div>
      </div>
      <div class="cm-check" hidden>
        <h2><button class="cm-up cm-check-up" type="button" aria-label="Back to Controls" title="Back (Esc)"><span>‹</span></button><span>Controller check</span></h2>
        <p class="cm-pad-name" role="status"></p>
        <p class="cm-pad-sub cm-pad-game"></p>
        <p class="cm-pad-sub cm-pad-info"></p>
        <div class="cm-pad-keys"></div>
        <div class="cm-pad-axes"></div>
        <p class="cm-note cm-pad-how"></p>
      </div>
    </nav>
  </div>`;
  document.body.append(root);
  const $ = (sel) => root.querySelector(sel);
  const shade = $(".cm-shade"), openBtn = $(".cm-open"), canvas = document.getElementById("canvas");
  const mainPage = $(".cm-main"), setPage = $(".cm-set"), checkPage = $(".cm-check");

  // ---- Values ----
  function setVal(btn, on) {
    const v = btn.querySelector(".cm-val");
    v.textContent = on ? "On" : "Off";
    v.classList.toggle("on", on);
  }
  function refresh() {
    setVal($(".cm-full"), !!document.fullscreenElement);
    for (const btn of root.querySelectorAll(".cm-toggle")) {
      let on = false;
      try { on = !!btn.toggle.get(); } catch (e) { console.warn("[menu]", e); }
      setVal(btn, on);
    }
    for (const btn of root.querySelectorAll(".cm-go")) btn.querySelector(".cm-hint").textContent = restartCount(btn.dataset.page) ? "Restart" : "";
    const waiting = restartCount();
    const quitHint = $(".cm-quit .cm-hint");
    quitHint.textContent = waiting ? "Restarts with your new settings" : "";
    quitHint.classList.toggle("restart", !!waiting);
    if (!setPage.hidden) update();
    const s = window.recompOnlineState, online = window.recompOnline;
    $(".cm-online").hidden = !online;
    if (!online) return;
    const who = $(".cm-who"), list = $(".cm-crews");
    who.replaceChildren();
    list.replaceChildren();
    list.classList.remove("more");
    if (shade.hidden) requestAnimationFrame(() => { list.scrollTop = 0; }); // opening: start at the top
    const me = document.createElement("b");
    me.textContent = online.name;
    who.append("You are ", me, online.room ? ` in room "${online.room}"` : " with everyone on the server", ". ");
    if (!s || !s.open) who.append("Connecting…");
    else {
      const people = s.people || s.names.map((n) => ({ name: n, crew: "" }));
      if (!people.length) who.append("Nobody else is here yet.");
      crewList(list, people, online.name, crewNow());
    }
    crewDone();
    showCrew();
    fit();
  }
  // Who is here, one block per crew in the crew's colour: your own crew first and
  // marked, then the others (biggest first), then those in no crew. net.js says
  // which names are one crew and what colour each crew has. A crew typed in two
  // ways ("Speed", "speed") shows the way you typed it, else the way the player
  // who has been here longest did (people come in the order they arrived).
  function crewList(list, people, myName, myCrew) {
    const key = window.recompCrewKey || ((c) => String(c || "").toLowerCase().replace(/\s+/g, ""));
    const color = window.recompCrewColor || (() => "");
    const mine = key(myCrew), groups = new Map(); // crew key -> {crew, people: [{name, me}]}
    if (!mine && !people.length) return; // alone and in no crew: nothing to list
    const add = (crew, name, me) => {
      const k = key(crew);
      if (!groups.has(k)) groups.set(k, { crew: String(crew || "").trim(), people: [] });
      groups.get(k).people.push({ name, me });
    };
    add(myCrew, myName, true);
    for (const p of people) add(p.crew, p.name, false);
    for (const g of groups.values()) g.people.sort((a, b) => b.me - a.me || a.name.localeCompare(b.name));
    const rank = (k) => (k && k === mine ? 0 : k ? 1 : 2);
    const order = [...groups].sort(([a, x], [b, y]) =>
      rank(a) - rank(b) || y.people.length - x.people.length || x.crew.localeCompare(y.crew));
    // A big room (a hundred players or more) lists names only for your own crew;
    // other crews show their size, the biggest twelve, then one line for the rest.
    const big = people.length > 20, shown = big ? order.slice(0, 13) : order;
    const rest = order.slice(shown.length).reduce((n, [, g]) => n + g.people.length, 0);
    for (const [k, g] of shown) {
      const el = document.createElement("div"), head = document.createElement("div"), names = document.createElement("div");
      el.className = "cm-crew" + (!k ? " none" : k === mine ? " mine" : "");
      if (k && color(g.crew)) el.style.setProperty("--c", color(g.crew));
      head.className = "cm-crew-head";
      const title = document.createElement("span");
      title.textContent = k ? g.crew : "No crew";
      head.append(title);
      if (k && k === mine) {
        const tag = document.createElement("span");
        tag.className = "cm-crew-tag";
        tag.textContent = "Your crew";
        head.append(tag);
      }
      const n = document.createElement("span");
      n.className = "cm-crew-n";
      n.textContent = g.people.length === 1 ? "1 player" : `${g.people.length} players`;
      head.append(n);
      names.className = "cm-crew-names";
      if (big && k !== mine) { el.append(head); list.append(el); continue; }
      g.people.forEach((p, i) => {
        if (i) names.append(", ");
        if (!p.me) { names.append(p.name); return; }
        const b = document.createElement("b");
        b.textContent = `${p.name} (you)`;
        names.append(b);
      });
      el.append(head, names);
      list.append(el);
    }
    if (rest) {
      const el = document.createElement("div");
      el.className = "cm-crew none";
      el.textContent = `and ${rest} more ${rest === 1 ? "player" : "players"} in other crews`;
      list.append(el);
    }
    list.onscroll = more;
  }
  // A full room on a short screen scrolls: the list takes the height the rest
  // of the menu leaves on screen (at least 120 px), and a faded edge says there
  // is more below. Measured while the menu shows. Only when even that is too
  // tall (the crew field open on a short screen) does the whole menu scroll.
  function more() {
    const list = $(".cm-crews");
    list.classList.toggle("more", list.scrollTop + list.clientHeight < list.scrollHeight - 1);
  }
  function fit() {
    if (shade.hidden || mainPage.hidden || $(".cm-online").hidden) return;
    const list = $(".cm-crews"), panel = $(".cm-panel");
    const spare = innerHeight - 32 - (panel.scrollHeight + panel.offsetHeight - panel.clientHeight);
    list.style.maxHeight = `${Math.max(120, list.clientHeight + spare)}px`;
    more();
  }
  addEventListener("recomp-online", () => { if (!shade.hidden) refresh(); });
  addEventListener("resize", fit);
  document.addEventListener("fullscreenchange", () => { if (!shade.hidden) refresh(); });

  // ---- Your crew ----
  // Changed while driving through net.js (window.recompOnlineCrew), which tells
  // the relay, keeps the new crew for the start page and recolours the badge and
  // the name tags. The menu shows "Changing…" until the relay has told everyone.
  const crewForm = $(".cm-crew-form"), crewIn = $(".cm-crew-in"), crewNote = $(".cm-crew-note");
  let asked = null, askedTimer = 0; // the crew asked for, until the relay confirms it
  const crewNow = () => window.recompOnlineState?.crew ?? window.recompOnline?.crew ?? "";
  // What the relay keeps: letters, digits, spaces, _ . - (as on the start page).
  const crewClean = (v) => v.replace(/[^\p{L}\p{N} _.-]/gu, "");
  function showCrew() {
    const c = crewNow(), chip = document.createElement("span"), hint = document.createElement("span");
    chip.className = c ? "cm-chip" : "cm-chip none";
    chip.textContent = c || "None";
    const color = c && window.recompCrewColor?.(c);
    if (color) chip.style.setProperty("--c", color);
    hint.className = "cm-hint";
    hint.textContent = asked !== null ? "Changing…" : "Change";
    $(".cm-crew-now").replaceChildren(chip, hint);
  }
  function crewSay(text, bad = false) {
    crewNote.textContent = text;
    crewNote.hidden = !text;
    crewNote.classList.toggle("bad", bad);
    fit();
  }
  function editCrew() {
    crewIn.value = crewNow();
    crewSay("");
    crewForm.hidden = false;
    fit();
    crewIn.focus();
    crewIn.select();
  }
  function hideCrew() {
    crewForm.hidden = true;
    $(".cm-crew-edit").focus();
    fit();
  }
  function setCrew() {
    const want = crewClean(crewIn.value).trim().slice(0, 16);
    crewIn.value = want;
    if (want === crewNow() && asked === null) { hideCrew(); return; }
    const r = window.recompOnlineCrew?.(want);
    if (!r) { crewSay("Crews need the online game.", true); return; }
    if (!r.ok) { crewSay(`Too many changes. Try again in ${r.wait} s.`, true); return; }
    asked = r.crew;
    crewSay("Changing…");
    showCrew();
    clearTimeout(askedTimer);
    askedTimer = setTimeout(() => {
      if (asked === null) return;
      asked = null;
      showCrew();
      if (!crewForm.hidden) crewSay("The server did not answer. Try again in a moment.", true);
    }, 4000);
  }
  // From refresh: once the relay has the crew asked for, the form closes.
  function crewDone() {
    if (asked === null || crewNow() !== asked) return;
    asked = null;
    clearTimeout(askedTimer);
    if (!crewForm.hidden) hideCrew();
  }
  function resetCrew() {
    asked = null;
    clearTimeout(askedTimer);
    crewForm.hidden = true;
    crewSay("");
  }
  crewIn.addEventListener("input", () => {
    const v = crewIn.value, at = crewIn.selectionStart ?? v.length;
    if (crewClean(v) !== v) {
      crewIn.value = crewClean(v);
      const p = crewClean(v.slice(0, at)).length;
      crewIn.setSelectionRange(p, p);
    }
    if (asked === null) crewSay("");
  });
  crewIn.addEventListener("keydown", (e) => {
    if (e.key !== "Enter") return;
    e.preventDefault();
    setCrew();
  });
  $(".cm-crew-ok").addEventListener("click", setCrew);
  $(".cm-crew-edit").addEventListener("click", () => (crewForm.hidden ? editCrew() : hideCrew()));

  // ---- Switches from other scripts ----
  // radar.js and others push {id, label, get: () => boolean, set: (boolean) => void}
  // into window.recompMenuToggles; each shows under Full screen as label + On/Off.
  // Drawn afresh each time the menu opens, so a script that loads after this
  // one still gets its row; the same id twice is one row (the later one).
  window.recompMenuToggles ||= [];
  function drawToggles() {
    const all = new Map();
    for (const t of Array.isArray(window.recompMenuToggles) ? window.recompMenuToggles : [])
      if (t && t.id != null && typeof t.get === "function" && typeof t.set === "function") all.set(String(t.id), t);
    $(".cm-toggles").replaceChildren(...[...all].map(([id, t]) => {
      const btn = document.createElement("button"), label = document.createElement("span"), val = document.createElement("span");
      btn.type = "button";
      btn.className = "cm-item cm-toggle";
      btn.dataset.id = id;
      btn.toggle = t;
      label.textContent = String(t.label || id);
      val.className = "cm-val";
      btn.append(label, val);
      btn.addEventListener("click", () => {
        try { t.set(!t.get()); } catch (e) { console.warn("[menu]", e); }
        refresh();
      });
      return btn;
    }));
  }
  // A script that adds its switch while the menu is open says so (radar.js).
  addEventListener("recomp-menu-toggles", () => {
    if (shade.hidden) return;
    const focused = document.activeElement?.closest?.(".cm-toggle, .cm-action");
    const id = focused?.dataset.id, kind = focused?.classList.contains("cm-action") ? "cm-action" : "cm-toggle";
    drawToggles();
    drawActions();
    refresh();
    if (id != null) root.querySelector(`.${kind}[data-id="${CSS.escape(id)}"]`)?.focus();
  });

  // ---- Buttons from other scripts ----
  // race.js and others push {id, group, label, hint, shown, run, keepOpen} into
  // window.recompMenuActions; label and hint may be strings or functions, shown()
  // false hides the button. Each group gets its own heading above Settings. Drawn
  // afresh each time the menu opens or a script says so (as the switches are);
  // a click runs it and closes the menu unless keepOpen.
  window.recompMenuActions ||= [];
  const text = (v) => { try { return String((typeof v === "function" ? v() : v) ?? ""); } catch { return ""; } };
  function drawActions() {
    const groups = new Map(), seen = new Map();
    for (const a of Array.isArray(window.recompMenuActions) ? window.recompMenuActions : [])
      if (a && a.id != null && typeof a.run === "function") seen.set(String(a.id), a);
    for (const [id, a] of seen) {
      let shown = true;
      try { shown = typeof a.shown !== "function" || !!a.shown(); } catch { shown = false; }
      if (!shown) continue;
      const group = text(a.group) || "More";
      if (!groups.has(group)) groups.set(group, []);
      const btn = document.createElement("button"), label = document.createElement("span"), hint = document.createElement("span");
      btn.type = "button";
      btn.className = "cm-item cm-action";
      btn.dataset.id = id;
      label.textContent = text(a.label) || id;
      hint.className = "cm-hint";
      hint.textContent = text(a.hint);
      btn.append(label, hint);
      btn.addEventListener("click", () => {
        try { a.run(); } catch (e) { console.warn("[menu]", e); }
        if (a.keepOpen) { drawActions(); refresh(); } else close();
      });
      groups.get(group).push(btn);
    }
    $(".cm-extra").replaceChildren(...[...groups].flatMap(([name, btns]) => {
      const h = document.createElement("h3");
      h.textContent = name;
      return [h, ...btns];
    }));
  }

  // ---- Missed boss cars (on this computer only) ----
  // After a crew boss is beaten the game deals face-down bonus cards, and one may be the boss's
  // pink slip. A beaten boss whose car the player never got shows under Career as "Add Kenji's
  // car to My Cars"; mods/core/nfsc/rewards.h gives it through the game's own pink-slip award,
  // in the game's menus or the Safehouse only. Nothing shows when there is nothing to add. Not on
  // the public site (window.recompSite, set by the site's online.js): there none of this runs,
  // and rewards.h stays off, because only this code turns it on. Before adding, the save folder
  // is kept as the Saves panel's undo file, in saves.js's backup format (a stored ZIP, comment
  // {"backup":"nfsc-save",...,"kind":"undo"}), so start page → Saves → Undo puts it back.
  // The game's block (rewards.h has the layout): magic +0x00, the switch +0x08, request number
  // +0x0c, which boss +0x10, handled +0x14, result +0x18, status number +0x1c (odd while the
  // game writes), where +0x20, career cars +0x24 of +0x28, autosave +0x2c, bosses +0x30, then
  // from +0x40 one boss per 0x40 bytes: state, preset, crew, spare, name[24], model[24].
  (() => {
    const MAGIC = 0x31445752, LOCAL = 0x4c41434f, MISSING = 1, SAFE = 1;
    const CARS = { BMWM3GTRE46: "BMW M3 GTR", RX7: "Mazda RX-7", CHARGER69: "Dodge Charger R/T", DB9: "Aston Martin DB9", R8: "Audi Le Mans Quattro" };
    let busy = -1, note = "", noteFor = -1, noteAt = 0;
    const redraw = () => dispatchEvent(new Event("recomp-menu-toggles"));
    function block() {
      const b = globalThis.carbonRewardsBlock;
      if (window.recompSite || !b) return null;
      const u32 = new Uint32Array(b.memory.buffer), p = b.ptr >> 2;
      if (u32[p] !== MAGIC) return null;
      if (u32[p + 2] !== LOCAL) { // turns rewards.h on; its first list follows within a second
        Atomics.store(u32, p + 2, LOCAL);
        setTimeout(redraw, 1200);
      }
      return { u32, u8: new Uint8Array(b.memory.buffer), p, ptr: b.ptr };
    }
    // On as soon as the game is up, so the list is there before the menu first opens; but only
    // once online.js has run (window.recompServers), which is where the site sets recompSite.
    // Without it (no online.js) the first use of the menu turns it on.
    const early = () => { if (window.recompServers) block(); };
    addEventListener("carbon-rewards", early);
    early();
    const latin1 = (u8, at, n) => { let s = ""; for (let i = 0; i < n && u8[at + i]; i++) s += String.fromCharCode(u8[at + i]); return s; };
    const nice = (name) => (/[a-z]/.test(name) ? name : name.charAt(0) + name.slice(1).toLowerCase()); // "KENJI" -> "Kenji"
    function status() {
      const v = block();
      for (let tries = 0; v && tries < 8; tries++) {
        const seq = Atomics.load(v.u32, v.p + 7);
        if (seq & 1) continue;
        const s = { where: v.u32[v.p + 8], garage: v.u32[v.p + 9], garageMax: v.u32[v.p + 10], autosave: v.u32[v.p + 11], bosses: [] };
        for (let i = 0; i < Math.min(v.u32[v.p + 12], 6); i++) {
          const at = v.ptr + 0x40 + 0x40 * i;
          s.bosses.push({ state: v.u32[at >> 2], opening: v.u32[(at >> 2) + 3] === 1, name: nice(latin1(v.u8, at + 16, 24)), model: latin1(v.u8, at + 40, 24) });
        }
        if (Atomics.load(v.u32, v.p + 7) === seq) return s;
      }
      return null;
    }
    const carOf = (b) => CARS[b.model] || b.model || "car";

    // The save folder as saves.js backs it up (same files left out, same ZIP).
    const skipped = (path) => {
      const parts = path.toLowerCase().split("/"), top = parts[0], name = parts[parts.length - 1];
      return name === ".ds_store" || name === "thumbs.db" || name.startsWith("._") || top === "__macosx" || name.endsWith(".crswap") ||
        name.endsWith(".log") || top === "logs" || top === "shaderwarm" || (parts.length === 1 && (top === ".copied" || top === "game-path.txt")) ||
        top === "game" || top.startsWith("launcher-") || path.toLowerCase() === "mods/run.json";
    };
    const CRC = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
    const crc32 = (d) => { let c = 0xffffffff; for (let i = 0; i < d.length; i++) c = CRC[(c ^ d[i]) & 0xff] ^ (c >>> 8); return ~c >>> 0; };
    function zip(files, comment) {
      const enc = new TextEncoder(), parts = [], central = [];
      let offset = 0;
      const header = (size, fields) => { const v = new DataView(new ArrayBuffer(size)); for (const [at, n, bytes] of fields) bytes === 2 ? v.setUint16(at, n, true) : v.setUint32(at, n, true); return new Uint8Array(v.buffer); };
      for (const f of files) {
        const name = enc.encode(f.dir ? `${f.path}/` : f.path), data = f.dir ? new Uint8Array(0) : f.data, crc = crc32(data);
        const d = new Date(f.mtime || Date.now()), year = Math.min(2107, Math.max(1980, d.getFullYear()));
        const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1), date = ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
        parts.push(header(30, [[0, 0x04034b50], [4, 20, 2], [6, 0x0800, 2], [10, time, 2], [12, date, 2], [14, crc], [18, data.length], [22, data.length], [26, name.length, 2]]), name, data);
        central.push(header(46, [[0, 0x02014b50], [4, 20, 2], [6, 20, 2], [8, 0x0800, 2], [12, time, 2], [14, date, 2], [16, crc], [20, data.length],
          [24, data.length], [28, name.length, 2], [38, f.dir ? 0x10 : 0], [42, offset]]), name);
        offset += 30 + name.length + data.length;
      }
      const note = enc.encode(comment), size = central.reduce((n, a) => n + a.length, 0);
      return new Blob([...parts, ...central, header(22, [[0, 0x06054b50], [8, files.length, 2], [10, files.length, 2], [12, size], [16, offset], [20, note.length, 2]]), note]);
    }
    async function backup() {
      const folder = window.recompStartPage?.profileFolder || "profile";
      const nfsc = await (await navigator.storage.getDirectory()).getDirectoryHandle(game.id);
      const files = [];
      async function walk(d, prefix) {
        let any = false;
        for await (const [name, h] of d.entries()) {
          const path = prefix + name;
          if (skipped(path)) continue;
          any = true;
          if (h.kind === "directory") {
            if (!(await walk(h, `${path}/`))) files.push({ path, dir: true, mtime: Date.now() });
          } else {
            const f = await h.getFile();
            files.push({ path, data: new Uint8Array(await f.arrayBuffer()), mtime: f.lastModified });
          }
        }
        return any;
      }
      await walk(await nfsc.getDirectoryHandle(folder), "");
      if (!files.some((f) => /^documents\//i.test(f.path))) throw new Error("no career save found");
      files.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
      const out = await (await nfsc.getFileHandle(`saves-undo-${folder}.zip`, { create: true })).createWritable();
      await out.write(zip(files, JSON.stringify({ backup: "nfsc-save", version: 1, made: new Date().toISOString(), folder, kind: "undo" })));
      await out.close();
    }

    const say = (i, text) => { note = text; noteFor = i; noteAt = Date.now(); redraw(); };
    async function add(i) {
      const s = status(), b = s?.bosses[i];
      if (!b || busy >= 0) return;
      if (b.state !== MISSING) return say(i, "Nothing to add: this car was given already.");
      if (s.where !== SAFE) return say(i, "Not now: open this menu in the game's menus or the Safehouse, not while driving.");
      if (s.garage >= s.garageMax) return say(i, `My Cars is full (${s.garage} cars). Sell a car first.`);
      busy = i;
      say(i, "Backing up your save…");
      try { await backup(); }
      catch (e) { busy = -1; return say(i, `Your save could not be backed up, so nothing was added (${e?.message || e}).`); }
      say(i, "Adding…");
      const v = block();
      if (!v) { busy = -1; return say(i, "The game is not ready yet."); }
      v.u32[v.p + 4] = i;
      const request = Atomics.add(v.u32, v.p + 3, 1) + 1;
      let result = 0;
      for (let t = 0; t < 150 && !result; t++) {
        await new Promise((r) => setTimeout(r, 100));
        if (Atomics.load(v.u32, v.p + 5) === request) result = v.u32[v.p + 6];
      }
      busy = -1;
      const car = b.opening ? "The opening BMW M3 GTR" : `${b.name}'s ${carOf(b)}`;
      say(i, result === 1 ? `${car} is in My Cars (Car Select). ` +
            (s.autosave ? "The game saves it when you next leave the Safehouse (Free Roam or a race)." : "Autosave is off: save the game from its menu to keep it.") +
            " To undo: start page → Saves → Undo the last restore."
        : result === 2 ? "Not now: open this menu in the game's menus or the Safehouse, not while driving."
        : result === 3 ? "Nothing to add: this car was given already."
        : result === 4 ? "My Cars is full. Sell a car first."
        : "The game could not add it; your save was not changed.");
    }

    window.recompMenuActions ||= [];
    let tuneBusy = false, tuneNote = "Packages bought stay owned. Your save is backed up before enabling.";
    window.recompMenuActions.push({
      id: "performance-unlock", group: "Career", keepOpen: true,
      shown: () => { const v = block(); return !!v && v.u32[v.p + 1] >= 2; },
      label: () => { const v = block(); return `Unlock all performance parts: ${v && v.u32[v.p + 13] === 1 ? "On" : "Off"}`; },
      hint: () => tuneNote,
      run: async () => {
        let v = block();
        if (!v || tuneBusy || busy >= 0) return;
        if (v.u32[v.p + 13] === 1) { Atomics.store(v.u32, v.p + 13, 0); redraw(); return; }
        if (status()?.where !== SAFE) { tuneNote = "Open this in the game's menus or the Safehouse."; redraw(); return; }
        tuneBusy = true; busy = 6; tuneNote = "Backing up your save…"; redraw();
        try {
          await backup(); v = block();
          if (v) { Atomics.store(v.u32, v.p + 13, 1); tuneNote = "All performance packages are available in the shop. Prices still apply. Reopen the shop to refresh."; }
        } catch (e) { tuneNote = `Could not back up your save. Parts remain locked (${e?.message || e}).`; }
        finally { tuneBusy = false; busy = -1; redraw(); }
      },
    });
    let visualBusy = false, visualNote = "Paints, visuals and aftermarket parts supported by your car. Your save is backed up before enabling.";
    window.recompMenuActions.push({
      id: "visual-unlock", group: "Career", keepOpen: true,
      shown: () => { const v = block(); return !!v && v.u32[v.p + 1] >= 3; },
      label: () => { const v = block(); return `Unlock paints, visuals & aftermarket: ${v && v.u32[v.p + 14] === 1 ? "On" : "Off"}`; },
      hint: () => visualNote,
      run: async () => {
        let v = block();
        if (!v || visualBusy || busy >= 0) return;
        if (v.u32[v.p + 14] === 1) { Atomics.store(v.u32, v.p + 14, 0); redraw(); return; }
        if (status()?.where !== SAFE) { visualNote = "Open this in the game's menus or the Safehouse."; redraw(); return; }
        visualBusy = true; busy = 7; visualNote = "Backing up your save…"; redraw();
        try {
          await backup(); v = block();
          if (v) { Atomics.store(v.u32, v.p + 14, 1); visualNote = "Paints, visuals and aftermarket parts are unlocked. Prices still apply. Reopen the shop to refresh."; }
        } catch (e) { visualNote = `Could not back up your save. Parts remain locked (${e?.message || e}).`; }
        finally { visualBusy = false; busy = -1; redraw(); }
      },
    });
    let bmwBusy = false, bmwNote = "Double engine torque, taller gearing and extra tyre grip. Full custom ratings. Local only; resets when the game restarts.";
    window.recompMenuActions.push({
      id: "bmw-boost", group: "Career", keepOpen: true,
      shown: () => { const v = block(); return !!v && v.u32[v.p + 1] >= 4; },
      label: () => { const v = block(); return `BMW M3 GTR custom boost: ${v && v.u32[v.p + 15] === 1 ? "On" : "Off"}`; },
      hint: () => bmwNote,
      run: async () => {
        let v = block();
        if (!v || bmwBusy || busy >= 0) return;
        if (v.u32[v.p + 15] === 1) { Atomics.store(v.u32, v.p + 15, 0); redraw(); return; }
        if (status()?.where !== SAFE) { bmwNote = "Open this in the game's menus or the Safehouse."; redraw(); return; }
        bmwBusy = true; busy = 8; bmwNote = "Backing up your save…"; redraw();
        try {
          await backup(); v = block();
          if (v) { Atomics.store(v.u32, v.p + 15, 1); bmwNote = "Custom BMW boost enabled. Reopen Car Select for full ratings. Turning this off restores normal driving settings."; }
        } catch (e) { bmwNote = `Could not back up your save. Boost remains off (${e?.message || e}).`; }
        finally { bmwBusy = false; busy = -1; redraw(); }
      },
    });
    for (let i = 0; i < 6; i++)
      window.recompMenuActions.push({
        id: `boss-car-${i}`, group: "Career", keepOpen: true,
        shown: () => {
          if (window.recompSite) return false;
          if (noteFor === i && (busy === i || Date.now() - noteAt < 120000)) return true;
          return status()?.bosses[i]?.state === MISSING;
        },
        label: () => {
          const b = status()?.bosses[i];
          if (b?.opening) return b.state === MISSING ? "Add opening BMW M3 GTR to My Cars" : "Opening BMW M3 GTR";
          return !b ? "Missed boss car" : b.state === MISSING ? `Add ${b.name}'s ${carOf(b)} to My Cars` : `${b.name}'s ${carOf(b)}`;
        },
        hint: () => {
          if (noteFor === i && (busy === i || Date.now() - noteAt < 120000)) return note;
          const s = status();
          if (!s) return "";
          if (s.where !== SAFE) return "Open this in the game's menus or the Safehouse, not while driving.";
          if (s.garage >= s.garageMax) return `My Cars is full (${s.garage} cars). Sell a car first.`;
          if (s.bosses[i]?.opening) return "The BMW from the opening sequence, with its original appearance. Your save is backed up first.";
          return `${status()?.bosses[i]?.name || "The boss"}'s pink slip from the bonus cards you didn't pick. Your save is backed up first.`;
        },
        run: () => { add(i); },
      });
  })();

  // ---- The game's settings ----
  // The kit's block (kit/mods/web_settings.cpp has the layout): the list is JSON
  // at [+12] (length [+16]), rewritten under an odd generation [+8]; the page
  // writes a request "<id>=<value>" (or "" for a fresh list) into slot n % 8 at
  // +64 + 192 * slot, then adds 1 to [+20]; [+24] counts the handled ones.
  const SET_MAGIC = 0x31425352, SLOTS = 8, SLOT0 = 64, SLOT_BYTES = 192;
  const S = {
    block: null,
    gen: -1,
    list: [],
    byId: new Map(),
    start: new Map(),  // id -> the value the game started with (a restart setting differs until the next start)
    queue: [],         // requests not yet written to the block: {text, want}
    want: new Map(),   // id -> {value, n}: changed here, until request n is handled
    ticking: false,
  };
  // Where each known row goes. A setting none of these name lands in its page's
  // More section (display rows and mod groups "graphics..." on Display &
  // graphics, "controls" on Controls, everything else on Game).
  const PAGES = {
    display: {
      title: "Display & graphics",
      sections: [
        ["Screen", ["browser/fullscreen", "host.display/frame_limit", "host.display/performance_overlay"]],
        ["Picture", ["core.nfsc/car_shine", "host.display/texture_filtering", "core.nfsc/render_scale", "core.nfsc/xenon_effects"]],
        ["Widescreen", ["core.nfsc/widescreen", "core.nfsc/fov_scaling", "core.nfsc/hud_constraint", "core.nfsc/hud_custom_width",
          "core.nfsc/hud_custom_height", "core.nfsc/ui_scale", "core.nfsc/ui_autofit", "core.nfsc/movie_mode",
          "core.nfsc/movie_scale", "core.nfsc/movie_autofit"]],
      ],
    },
    controls: {
      title: "Controls",
      note: "Keys and controller buttons for driving are changed in the game: Options → Controls.",
      sections: [
        ["Controller", ["core.nfsc/console_layout", "core.nfsc/controller_buttons", "core.nfsc/console_prompts"]],
        ["Camera", ["core.nfsc/freelook", "core.nfsc/freelook_invert"]],
        ["Rumble", ["core.nfsc/impact_rumble", "core.nfsc/surface_rumble", "core.nfsc/rumble_floor"]],
      ],
    },
    game: {
      title: "Game",
      sections: [
        ["Start-up", ["core.nfsc/skip_intro", "core.nfsc/hide_online"]],
        ["Online", ["core.nfsc/mp_ghost_cars"]],
        ["Testing", ["core.nfsc/input_trace", "core.nfsc/mp_test"]],
      ],
    },
  };
  const pageOf = (s) => (s.group === "display" || /^graphics/.test(s.group) ? "display" : s.group === "controls" ? "controls" : "game");
  // Not shown: the Window row (the browser's full screen is the Full screen row),
  // cheats (removed from the game in 2026-10; a group of that name never shows),
  // and the test switches (group "diagnostics": Log rumble, Test car beside you)
  // unless the address has ?testing or one of them is already on, so it can be
  // turned off again.
  const TESTING = new URLSearchParams(location.search).has("testing");
  const HIDDEN = (s) => s.id === "host.display/window" || s.group === "cheats" ||
    (s.group === "diagnostics" && !TESTING && !Number(s.value));
  // Rows that only matter with another row's value.
  const custom = () => choiceOf("core.nfsc/hud_constraint") === "Custom";
  const SHOWN_IF = { "core.nfsc/hud_custom_width": custom, "core.nfsc/hud_custom_height": custom };
  // Words the browser needs that the game's own help does not have.
  const RUMBLE_NOTE = " Chrome can rumble PS4 and Xbox controllers, not a PS5 controller (DualSense): Chrome has no way to " +
    "drive its motors yet. Controller check says whether yours can.";
  const WEB_NOTE = {
    "core.nfsc/render_scale": "Game resolution uses the size selected in Options → Video. Choose a lower size there for less graphics work. " +
      "1.5x draws more pixels for smoother edges. Screen follows the browser window's resolution. Restart the game to apply this choice.",
    "core.nfsc/controller_buttons": "Which controller's button names Options → Controls shows. Automatic uses the controller " +
      "you play with: PlayStation names for a PS4 or PS5 controller.",
    "core.nfsc/console_prompts": "Needs XtendedInput's button icons, which the browser version does not have yet, so the " +
      "game's menus show keyboard prompts. Your controller works in them all the same.",
    "core.nfsc/impact_rumble": "How hard the controller shakes when you hit something. 0 turns it off." + RUMBLE_NOTE,
    "core.nfsc/surface_rumble": "Shaking from the road surface under the tyres. 0 turns it off." + RUMBLE_NOTE,
  };
  const TIMING = {
    live: "Changes now", renderer: "Changes now", restart: "After a restart", next_screen: "At the next screen or load",
    next_event: "From the next event", new_career: "New careers only", game_video: "When a detail level is chosen in Options → Video",
  };
  // The browser's own full screen, as a row of the Screen section.
  const FULLSCREEN = {
    id: "browser/fullscreen", label: "Full screen", kind: "bool", min: 0, max: 1, step: 1, def: 0, choices: [], apply: "live",
    help: "The browser's full screen (also on the menu's first page). The start page can start the game in full screen.",
  };

  const setting = (id) => (id === FULLSCREEN.id ? { ...FULLSCREEN, value: document.fullscreenElement ? 1 : 0 } : S.byId.get(id));
  const shownValue = (s) => (S.want.has(s.id) ? S.want.get(s.id).value : s.value);
  function choiceOf(id) {
    const s = setting(id);
    return s && s.choices?.length ? s.choices[shownValue(s) - s.min] ?? "" : "";
  }
  const percent = (s) => /\s%$/.test(s.label);
  const labelOf = (s) => s.label.replace(/\s%$/, "");
  function valueText(s, v) {
    if (s.kind === "bool") return v ? "On" : "Off";
    if (s.choices?.length) return s.choices[v - s.min] || String(v);
    return percent(s) ? `${v}%` : String(v);
  }
  // A restart setting changed since the game started.
  const waitsRestart = (s) => s.apply === "restart" && S.start.has(s.id) && shownValue(s) !== S.start.get(s.id);
  function restartCount(page) {
    let n = 0;
    for (const s of S.list) if (!HIDDEN(s) && (!page || pageOf(s) === page) && waitsRestart(s)) n++;
    return n;
  }

  // The next value: bools and choices go round (as the game's own options do),
  // numbers stop at their ends; a wide percentage moves in fives.
  function stepped(s, v, d) {
    if (s.kind === "bool") return v ? 0 : 1;
    const span = s.max - s.min;
    if (s.choices?.length) {
      let n = v;
      for (let i = 0; i <= span; i++) { // a retired choice (empty name) is passed over
        n += d;
        if (n > s.max) n = s.min;
        if (n < s.min) n = s.max;
        if (s.choices[n - s.min]) break;
      }
      return n;
    }
    const step = Math.max(s.step || 1, span >= 50 && s.max <= 400 ? 5 : 1);
    const n = d > 0 ? Math.floor(v / step) * step + step : Math.ceil(v / step) * step - step;
    return Math.min(s.max, Math.max(s.min, n));
  }

  function attachSettings(b) {
    if (!b || S.block) return;
    try { if (new Uint32Array(b.memory.buffer)[b.ptr >> 2] !== SET_MAGIC) { console.warn("[menu] the game's settings block is not there"); return; } }
    catch { return; }
    S.block = b;
    readList();
    if (!shade.hidden) { ask(""); tick(); }
  }
  addEventListener("recomp-settings", () => attachSettings(globalThis.recompSettingsBlock));
  attachSettings(globalThis.recompSettingsBlock);

  function readList() {
    const b = S.block;
    if (!b) return false;
    const buf = b.memory.buffer, i32 = new Int32Array(buf), at = b.ptr >> 2;
    const gen = Atomics.load(i32, at + 2);
    if (gen === S.gen || gen & 1) return false;
    const ptr = i32[at + 3] >>> 0, len = i32[at + 4] >>> 0;
    const bytes = new Uint8Array(buf, ptr, len).slice(); // a plain copy: TextDecoder refuses shared memory
    if (Atomics.load(i32, at + 2) !== gen) return false; // rewritten while copied: next frame
    let list;
    try { list = JSON.parse(new TextDecoder().decode(bytes)).settings; }
    catch (e) { console.warn("[menu] the game's settings list:", e); return false; }
    S.gen = gen;
    S.list = Array.isArray(list) ? list : [];
    S.byId = new Map(S.list.map((s) => [s.id, s]));
    for (const s of S.list) if (!S.start.has(s.id)) S.start.set(s.id, s.value);
    return true;
  }
  // Ask the game for a change (or, with "", for a fresh list).
  function ask(id, value) {
    if (!id) { if (!S.queue.some((q) => !q.id)) S.queue.push({ text: "" }); kick(); return; }
    const want = { value, n: 0 };
    S.want.set(id, want);
    const waiting = S.queue.find((q) => q.id === id);
    if (waiting) Object.assign(waiting, { text: `${id}=${value}`, want });
    else S.queue.push({ id, text: `${id}=${value}`, want });
    kick();
  }
  function pump() {
    const b = S.block;
    if (!b) return;
    const buf = b.memory.buffer, i32 = new Int32Array(buf), u8 = new Uint8Array(buf), at = b.ptr >> 2;
    while (S.queue.length) {
      const asked = Atomics.load(i32, at + 5), served = Atomics.load(i32, at + 6);
      if (asked - served >= SLOTS) return;
      const q = S.queue.shift(), bytes = new TextEncoder().encode(q.text).slice(0, SLOT_BYTES - 1);
      const slot = b.ptr + SLOT0 + (((asked % SLOTS) + SLOTS) % SLOTS) * SLOT_BYTES;
      u8.fill(0, slot, slot + SLOT_BYTES);
      u8.set(bytes, slot);
      if (q.want) q.want.n = asked + 1;
      Atomics.add(i32, at + 5, 1);
    }
  }
  function kick() {
    if (S.ticking) return;
    S.ticking = true;
    requestAnimationFrame(tick);
  }
  function tick() {
    S.ticking = false;
    const b = S.block;
    if (!b) return;
    pump();
    const i32 = new Int32Array(b.memory.buffer), at = b.ptr >> 2;
    const served = Atomics.load(i32, at + 6), status = Atomics.load(i32, at + 7);
    const fresh = readList(); // the game writes the list before it counts the request handled
    let done = false;
    for (const [id, w] of S.want) if (w.n && served - w.n >= 0) { S.want.delete(id); done = true; }
    if (done && status) console.warn("[menu] the game refused a setting change (status", status + ")");
    if ((fresh || done) && !shade.hidden) {
      if (page && fresh && pageKey(page) !== shownKey) showPage(page, false); // the game's list grew or changed
      refresh();
    }
    if (!shade.hidden || S.queue.length || S.want.size) kick();
  }

  // ---- Settings pages ----
  let page = null;              // the settings page showing, or null for the menu
  const rowEls = new Map();     // id -> its row on the page showing
  let shownKey = "";            // which rows it was built with
  function rowsFor(name) {
    const def = PAGES[name], placed = new Set(def.sections.flatMap(([, ids]) => ids)), out = [];
    for (const [title, ids] of def.sections) out.push([title, ids.map(setting).filter((s) => s && !HIDDEN(s))]);
    out.push(["More", S.list.filter((s) => !HIDDEN(s) && pageOf(s) === name && !placed.has(s.id))]);
    return out.filter(([, list]) => list.length);
  }
  const pageKey = (name) => (S.block ? rowsFor(name).flatMap(([t, list]) => [t, ...list.map((s) => s.id)]).join() : "");
  function showPage(name, focusFirst = true) {
    const keep = focusFirst ? null : document.activeElement?.closest?.(".cm-set-row")?.dataset.id;
    page = name;
    shownKey = pageKey(name);
    checking = false;
    checkPage.hidden = true;
    mainPage.hidden = true;
    setPage.hidden = false;
    $(".cm-reset-box").hidden = true;
    $(".cm-set-title").textContent = PAGES[name].title;
    const note = $(".cm-set-note");
    note.textContent = PAGES[name].note || "";
    note.hidden = !PAGES[name].note;
    rowEls.clear();
    const box = $(".cm-rows"), sections = S.block ? rowsFor(name) : [];
    box.replaceChildren();
    if (name === "controls") box.append(checkGo);
    for (const [title, list] of sections) {
      const h = document.createElement("h3");
      h.textContent = title;
      box.append(h);
      for (const s of list) box.append(makeRow(s));
    }
    if (!sections.length || !S.block) {
      const p = document.createElement("p");
      p.className = "cm-note";
      p.textContent = S.block ? "Nothing to set here." : "The settings appear here once the game has started.";
      box.append(p);
    }
    $(".cm-reset").hidden = !rowEls.size;
    update();
    if (focusFirst) $(".cm-panel").scrollTop = 0;
    if (keep && rowEls.has(keep)) rowEls.get(keep).focus();
    else if (focusFirst || !setPage.contains(document.activeElement)) (box.querySelector(".cm-set-row") || $(".cm-up")).focus();
    help();
  }
  function showMain() {
    const from = page;
    page = null;
    rowEls.clear();
    checking = false;
    checkPage.hidden = true;
    setPage.hidden = true;
    mainPage.hidden = false;
    refresh();
    root.querySelector(`.cm-go[data-page="${from}"]`)?.focus();
  }
  function makeRow(s) {
    const btn = document.createElement("button"), label = document.createElement("span"), ctl = document.createElement("span");
    btn.type = "button";
    btn.className = "cm-item cm-set-row";
    btn.dataset.id = s.id;
    label.className = "cm-label";
    ctl.className = "cm-ctl";
    ctl.innerHTML = `<span class="cm-tag" hidden>Restart</span><i class="cm-arr cm-dec" aria-hidden="true">‹</i>` +
      `<span class="cm-val"></span><i class="cm-arr cm-inc" aria-hidden="true">›</i>`;
    btn.append(label, ctl);
    btn.addEventListener("click", (e) => change(s.id, e.target.closest(".cm-dec") ? -1 : 1));
    btn.addEventListener("focus", help);
    btn.addEventListener("mouseenter", () => help(btn));
    rowEls.set(s.id, btn);
    return btn;
  }
  function change(id, d) {
    const s = setting(id);
    if (!s) return;
    if (id === FULLSCREEN.id) { $(".cm-full").click(); return; }
    const v = stepped(s, shownValue(s), d);
    if (v !== shownValue(s)) ask(id, v);
    update();
    help();
  }
  // A number row also takes typed digits ("For 32:9 enter 32"): digits typed within
  // a second and a half of each other make one number, kept within the row's limits.
  const numeric = (s) => !!s && s.kind === "int" && !s.choices?.length;
  let typed = "", typedId = "", typedAt = 0;
  function typeDigit(id, digit) {
    const s = setting(id), now = performance.now();
    typed = (id === typedId && now - typedAt < 1500 ? typed : "") + digit;
    typedId = id;
    typedAt = now;
    const v = Math.min(s.max, Math.max(s.min, +typed));
    if (v !== shownValue(s)) ask(id, v);
    update();
    help();
  }
  // Rows show the value asked for until the game has it (dimmed meanwhile).
  function update() {
    let restart = false;
    for (const [id, btn] of rowEls) {
      const s = setting(id);
      if (!s) { btn.hidden = true; continue; }
      const v = shownValue(s), val = btn.querySelector(".cm-val"), waits = waitsRestart(s);
      btn.querySelector(".cm-label").textContent = labelOf(s);
      val.textContent = valueText(s, v);
      val.classList.toggle("on", s.kind === "bool" && !!v);
      btn.classList.toggle("pending", S.want.has(id));
      btn.querySelector(".cm-tag").hidden = !waits;
      const shown = !SHOWN_IF[id] || SHOWN_IF[id]();
      if (!shown && document.activeElement === btn) btn.previousElementSibling?.focus?.();
      btn.hidden = !shown;
      btn.setAttribute("aria-label", `${labelOf(s)}: ${valueText(s, v)}`);
      restart ||= waits;
    }
    $(".cm-restart").hidden = !restart;
  }
  // What the row under the pointer, or else the chosen row, does.
  function help(el) {
    if (setPage.hidden) return;
    const row = (el instanceof Element ? el : null) || document.activeElement?.closest?.(".cm-set-row");
    const s = row && setting(row.dataset.id), title = $(".cm-help b"), body = $(".cm-help-text");
    $(".cm-keys").textContent = root.classList.contains("cm-pad")
      ? `D-pad ↑ ↓ choose · ← → change · ${padKind === "sony" ? "✕" : "A"} next choice · ${padKind === "sony" ? "○" : "B"} back`
      : `↑ ↓ choose · ← → change · ${numeric(s) ? "type a number" : "Enter next choice"} · Esc back`;
    if (!s) {
      title.textContent = "";
      body.textContent = PAGES[page]?.note ? "" : "Choose a setting to see what it does.";
      title.className = "";
      return;
    }
    title.textContent = waitsRestart(s) ? "After a restart · not yet in use" : TIMING[s.apply] || "";
    title.className = s.apply === "restart" ? "restart" : "";
    body.textContent = WEB_NOTE[s.id] || s.help || "";
  }
  for (const btn of root.querySelectorAll(".cm-go")) btn.addEventListener("click", () => showPage(btn.dataset.page));
  $(".cm-up").addEventListener("click", showMain);
  setPage.addEventListener("mouseleave", () => help());
  // Restore this page's defaults: every row of the page (hidden ones too) back to the value it was declared with.
  function hideReset() {
    $(".cm-reset-box").hidden = true;
    $(".cm-reset").focus();
  }
  $(".cm-reset").addEventListener("click", () => {
    $(".cm-reset-box").hidden = false;
    $(".cm-reset-no").focus();
  });
  $(".cm-reset-no").addEventListener("click", hideReset);
  $(".cm-reset-yes").addEventListener("click", () => {
    for (const id of rowEls.keys()) {
      const s = setting(id);
      if (s && id !== FULLSCREEN.id && shownValue(s) !== s.def) ask(id, s.def);
    }
    hideReset();
    update();
  });

  // ---- Open and close ----
  function items() { return [...shade.querySelectorAll("button, input")].filter((b) => !b.closest("[hidden]")); }
  function open() {
    if (!shade.hidden) return;
    if (document.pointerLockElement) document.exitPointerLock();
    drawToggles();
    drawActions();
    resetCrew();
    page = null;
    rowEls.clear();
    checking = false;
    checkPage.hidden = true;
    setPage.hidden = true;
    mainPage.hidden = false;
    refresh();
    $(".cm-quit-box").hidden = true;
    shade.hidden = false;
    openBtn.hidden = true;
    fit();
    $(".cm-back").focus();
    if (S.block) ask(""); // a fresh list: F11 or the game may have changed a value
    kick();
    globalThis.recompMenuOpen = true; // the kit gives the game a pad with nothing held
    padStart();
  }
  function close() {
    if (shade.hidden) return;
    shade.hidden = true;
    openBtn.hidden = false;
    checking = false;
    globalThis.recompMenuOpen = false;
    canvas?.focus();
  }
  const toggle = () => (shade.hidden ? open() : close());

  // Keys: F9 and F10 open and close; while open, presses go to the menu only.
  // The controller's chord reaches the kit, which sends "recomp-menu"; a press
  // of F10 the kit also saw (it should not: SDL listens later, in the bubble
  // phase) must not toggle twice.
  let keyAt = -1e9;
  const MENU_KEYS = new Set(["F9", "F10"]);
  addEventListener("keydown", (e) => {
    if (MENU_KEYS.has(e.code)) {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (!e.repeat) { keyAt = performance.now(); toggle(); }
      return;
    }
    if (!shade.hidden && !root.contains(e.target)) {
      e.preventDefault();
      e.stopImmediatePropagation();
      (mainPage.hidden ? setPage.querySelector(".cm-set-row") || $(".cm-up") : $(".cm-back")).focus();
    }
  }, true);
  addEventListener("keypress", (e) => {
    if (MENU_KEYS.has(e.code) || (!shade.hidden && !root.contains(e.target))) {
      e.preventDefault();
      e.stopImmediatePropagation();
    }
  }, true);
  addEventListener("recomp-menu", () => { if (performance.now() - keyAt > 400) toggle(); });
  // Keys inside the menu stop here, before the game's listeners on the window.
  for (const type of ["keydown", "keypress"]) root.addEventListener(type, (e) => e.stopPropagation());
  // One step back: a confirmation, the crew field, the controller check or a settings
  // page; on the first page `closing` (Esc, the controller's ○ / B) closes the menu.
  function back(closing) {
    if (checking) leaveCheck();
    else if (!$(".cm-quit-box").hidden) hideConfirm();
    else if (!crewForm.hidden) hideCrew();
    else if (!$(".cm-reset-box").hidden) hideReset();
    else if (page) showMain();
    else if (closing) close();
  }
  // The next (d = 1) or previous (d = -1) item, round the ends.
  function step(d) {
    const list = items(), at = list.indexOf(document.activeElement);
    const next = list[(at + (d > 0 ? 1 : list.length - 1)) % list.length];
    next?.focus();
    next?.scrollIntoView?.({ block: "nearest" });
  }
  shade.addEventListener("keydown", (e) => {
    const list = items(), at = list.indexOf(document.activeElement);
    const row = document.activeElement?.closest?.(".cm-set-row");
    const inField = e.target instanceof HTMLInputElement;
    if (e.key === "Escape" || (e.key === "Backspace" && !inField)) {
      e.preventDefault();
      back(e.key === "Escape");
    } else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      step(e.key === "ArrowDown" ? 1 : -1);
    } else if ((e.key === "ArrowLeft" || e.key === "ArrowRight") && row) {
      e.preventDefault();
      change(row.dataset.id, e.key === "ArrowRight" ? 1 : -1);
    } else if (/^[0-9]$/.test(e.key) && row && numeric(setting(row.dataset.id))) {
      e.preventDefault();
      typeDigit(row.dataset.id, e.key);
    } else if (e.key === "Tab") {
      e.preventDefault();
      list[(at + (e.shiftKey ? list.length - 1 : 1)) % list.length]?.focus();
    }
  });
  shade.addEventListener("click", (e) => { if (e.target === shade) close(); });

  // ---- Controller ----
  // Chrome's "standard" layout (button 0 bottom, 1 right, 2 left, 3 top, 4/5 shoulders,
  // 6/7 triggers, 8 Create/View, 9 Options/Menu, 10/11 stick clicks, 12-15 d-pad, 16 home,
  // 17 a PlayStation pad's touchpad) for every pad Chrome knows, which includes the PS4 and
  // PS5 controllers on macOS and Windows. A PlayStation pad a browser has no layout for
  // (mapping "") comes in HID order, with the d-pad as a hat on axis 9.
  const SONY = /Vendor: 054c|^0*54c-/i;
  const LOGICAL = ["south", "east", "west", "north", "l1", "r1", "l2", "r2", "back", "start", "l3", "r3",
    "up", "down", "left", "right", "home", "touch"];
  const SONY_RAW = { south: 1, east: 2, west: 0, north: 3, l1: 4, r1: 5, l2: 6, r2: 7, back: 8, start: 9, l3: 10, r3: 11, home: 12, touch: 13 };
  function padRead(gp) {
    const pressed = (i) => { const b = gp.buttons[i]; return !!b && (typeof b === "object" ? !!b.pressed : b === 1); };
    const value = (i) => { const b = gp.buttons[i]; return b ? (typeof b === "object" ? b.value : +b) || 0 : 0; };
    const axis = (i) => (Number.isFinite(gp.axes[i]) ? gp.axes[i] : 0);
    const s = { lx: axis(0), ly: axis(1), rx: axis(2), ry: axis(3), l2v: value(6), r2v: value(7) };
    if (gp.mapping === "standard" || !SONY.test(gp.id)) {
      LOGICAL.forEach((n, i) => { s[n] = pressed(i); });
      return s;
    }
    for (const [n, i] of Object.entries(SONY_RAW)) s[n] = pressed(i);
    const dir = Math.round((axis(9) + 1) * 3.5); // 0 up, 1 up-right ... 7 up-left, 8 centred
    Object.assign(s, { up: dir === 7 || dir === 0 || dir === 1, right: dir >= 1 && dir <= 3, down: dir >= 3 && dir <= 5,
      left: dir >= 5 && dir <= 7, ry: axis(5), l2v: (axis(3) + 1) / 2, r2v: (axis(4) + 1) / 2 });
    return s;
  }
  // Match SDL's browser-name normalization, rather than treating any open
  // controller as proof that the device shown here is used by the game.
  function padDeviceName(name) {
    return String(name || "").replace(/\s*\((STANDARD GAMEPAD|Vendor:|XInput).*$/i, "")
      .replace(/^0*[0-9a-f]+-0*[0-9a-f]+-/i, "").trim().toLowerCase();
  }
  function padMatchesGame(gp) {
    const used = globalThis.recompGameController;
    return !!gp && !!used && padDeviceName(gp.id) === padDeviceName(used.deviceName || used.name);
  }
  function padFind() {
    let pads = [];
    try { pads = [...(navigator.getGamepads?.() || [])]; } catch {}
    const controllers = pads.filter((g) => g && g.connected && !/hyperx cloud/i.test(g.id));
    return controllers.find(padMatchesGame) || controllers[0] || null;
  }

  let padKind = "sony", padHeld = null, padDir = "", padNext = 0, padBackAt = 0, padLoop = 0, checking = false;
  // Controller presses show the menu's focus as keyboard presses do; the mouse or a key hides it.
  function padUsed() {
    if (root.classList.contains("cm-pad")) return;
    root.classList.add("cm-pad");
    help();
  }
  for (const type of ["mousemove", "pointerdown", "keydown"])
    addEventListener(type, () => { if (root.classList.contains("cm-pad")) { root.classList.remove("cm-pad"); help(); } }, true);
  function padStart() {
    padHeld = null; // what is held at the first look (the press that opened the menu) does nothing
    padDir = "";
    if (!padLoop) padLoop = requestAnimationFrame(padTick);
  }
  function padTick(now) {
    padLoop = 0;
    if (shade.hidden) return;
    padLoop = requestAnimationFrame(padTick);
    const gp = padFind(), s = gp ? padRead(gp) : null;
    if (checking) drawCheck(gp, s);
    if (!s) { padHeld = null; return; }
    padKind = SONY.test(gp.id) ? "sony" : "xbox";
    const first = !padHeld, was = padHeld || s;
    padHeld = s;
    const press = (n) => s[n] && !was[n];
    if (checking) {
      // Here every button only lights up; holding ○ (B) for a second goes back.
      if (press("east")) padBackAt = now;
      if (!s.east) padBackAt = 0;
      else if (padBackAt && now - padBackAt > 900) { padBackAt = 0; padUsed(); leaveCheck(); }
      return;
    }
    const dir = s.up || s.ly < -0.6 ? "up" : s.down || s.ly > 0.6 ? "down"
      : s.left || s.lx < -0.6 ? "left" : s.right || s.lx > 0.6 ? "right" : "";
    if (!dir) padDir = "";
    else if (first) { padDir = dir; padNext = now + 400; }
    else if (dir !== padDir || now >= padNext) {
      padNext = now + (dir !== padDir ? 400 : 120); // held: repeats, like a key
      padDir = dir;
      padUsed();
      if (!shade.contains(document.activeElement)) $(".cm-back").focus();
      else if (dir === "up" || dir === "down") step(dir === "down" ? 1 : -1);
      else {
        const row = document.activeElement.closest(".cm-set-row");
        if (row) change(row.dataset.id, dir === "right" ? 1 : -1);
      }
    }
    if (press("south")) {
      padUsed();
      const el = document.activeElement;
      if (el instanceof HTMLButtonElement && shade.contains(el)) el.click();
      else if (!shade.contains(el)) $(".cm-back").focus();
    } else if (press("east")) {
      padUsed();
      back(true);
    }
  }

  // Controls → Controller check: what the browser and the game see of the controller.
  const checkGo = document.createElement("button");
  checkGo.type = "button";
  checkGo.className = "cm-item cm-check-go";
  checkGo.innerHTML = `<span>Controller check</span>`;
  checkGo.addEventListener("click", showCheck);
  $(".cm-check-up").addEventListener("click", leaveCheck);
  function showCheck() {
    page = null;
    rowEls.clear();
    checking = true;
    mainPage.hidden = true;
    setPage.hidden = true;
    checkPage.hidden = false;
    padBackAt = 0;
    drawCheck(padFind(), null);
    $(".cm-check-up").focus();
  }
  function leaveCheck() {
    showPage("controls");
    checkGo.focus();
  }
  const NAMES = {
    sony: { south: "✕", east: "○", west: "□", north: "△", l1: "L1", r1: "R1", l2: "L2", r2: "R2", back: "Create", start: "Options",
      l3: "L3", r3: "R3", up: "↑", down: "↓", left: "←", right: "→", touch: "Touchpad" },
    xbox: { south: "A", east: "B", west: "X", north: "Y", l1: "LB", r1: "RB", l2: "LT", r2: "RT", back: "View", start: "Menu",
      l3: "LS", r3: "RS", up: "↑", down: "↓", left: "←", right: "→" },
  };
  function padTitle(gp) {
    const m = /Vendor: ([0-9a-f]{4}) Product: ([0-9a-f]{4})/i.exec(gp.id) || /^0*([0-9a-f]+)-0*([0-9a-f]+)-/i.exec(gp.id);
    const vendor = m ? parseInt(m[1], 16) : 0, product = m ? parseInt(m[2], 16) : 0;
    if (vendor === 0x054c && product === 0x0ce6) return "PS5 controller (DualSense)";
    if (vendor === 0x054c && product === 0x0df2) return "PS5 controller (DualSense Edge)";
    if (vendor === 0x054c && [0x05c4, 0x09cc, 0x0ba0].includes(product)) return "PS4 controller (DualShock 4)";
    return gp.id.replace(/\s*\((STANDARD GAMEPAD|Vendor:|XInput).*$/, "").replace(/^0*[0-9a-f]+-0*[0-9a-f]+-/i, "") || "Controller";
  }
  let drawnKeys = "";
  function drawCheck(gp, s) {
    const name = $(".cm-pad-name"), game = $(".cm-pad-game"), info = $(".cm-pad-info"), keys = $(".cm-pad-keys");
    const used = globalThis.recompGameController;
    const kind = gp && SONY.test(gp.id) ? "sony" : "xbox";
    const names = { ...NAMES[kind], ...(kind === "sony" && !/0ce6|0df2|-ce6-|-df2-/i.test(gp?.id || "") ? { back: "Share" } : {}) };
    name.classList.toggle("none", !gp);
    name.textContent = gp ? `${padTitle(gp)}: found by the browser` :
      "No controller found yet. Connect it (USB cable or Bluetooth), then press any button on it - not the PS button.";
    const matches = padMatchesGame(gp);
    game.classList.toggle("good", matches);
    game.classList.toggle("bad", !matches && !!gp);
    game.textContent = matches ? "The game uses it." : used
      ? `The game is using another device: ${used.deviceName || used.name}.`
      : gp ? "The game has not picked it up yet. Press a few buttons; if this stays, tell us." : "";
    const layout = gp?.mapping === "standard" ? "standard"
      : kind === "sony" ? "not standard (the game sorts out PlayStation controllers)" : "not standard (some buttons may be in odd places)";
    info.textContent = gp ? `Layout: ${layout}` +
      ` · Rumble: ${gp.vibrationActuator ? "yes" : "not possible in this browser"}` : "";
    const order = LOGICAL.filter((n) => names[n]);
    if (drawnKeys !== kind + order.join()) {
      drawnKeys = kind + order.join();
      keys.replaceChildren(...order.map((n) => {
        const k = document.createElement("span");
        k.className = "cm-pad-key";
        k.dataset.key = n;
        k.textContent = names[n];
        return k;
      }));
    }
    for (const k of keys.children) k.classList.toggle("on", !!s?.[k.dataset.key]);
    const num = (v) => (v >= 0 ? " " : "") + v.toFixed(2);
    const pct = (v) => `${Math.round(Math.max(0, Math.min(1, v)) * 100)}%`.padStart(4);
    $(".cm-pad-axes").textContent = s ? `Left stick   ←→ ${num(s.lx)}   ↑↓ ${num(s.ly)}\n` +
      `Right stick  ←→ ${num(s.rx)}   ↑↓ ${num(s.ry)}\n${names.l2} ${pct(s.l2v)}    ${names.r2} ${pct(s.r2v)}` : "";
    $(".cm-pad-how").textContent = gp
      ? `Press each button: its box lights up. Move both sticks and squeeze ${names.l2} and ${names.r2} slowly. ` +
        `Hold ${names.east} for a second to go back.`
      : "This page shows what your browser sees. Chrome shows a controller to a page only after you press a button on it.";
  }

  // ---- Actions ----
  openBtn.addEventListener("click", open);
  $(".cm-back").addEventListener("click", close);
  $(".cm-full").addEventListener("click", async () => {
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else await document.documentElement.requestFullscreen();
    } catch {}
    store.set("fullscreen", !!document.fullscreenElement);
    refresh();
  });
  function hideConfirm() {
    $(".cm-quit-box").hidden = true;
    $(".cm-quit").focus();
  }
  $(".cm-quit").addEventListener("click", () => {
    $(".cm-quit-box").hidden = false;
    $(".cm-quit-no").focus();
  });
  $(".cm-quit-no").addEventListener("click", hideConfirm);
  $(".cm-quit-yes").addEventListener("click", () => {
    dispatchEvent(new Event("recomp-quit"));
    location.replace(location.pathname);
  });

  // The ☰ button shows while the mouse moves and fades after a few still seconds.
  let idle = 0;
  function nudge() {
    openBtn.classList.add("shown");
    clearTimeout(idle);
    idle = setTimeout(() => openBtn.classList.remove("shown"), 2500);
  }
  addEventListener("mousemove", nudge, { passive: true });
  nudge();
})();
