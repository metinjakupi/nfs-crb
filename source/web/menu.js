// menu.js - Need for Speed: Carbon's in-game menu in the browser, the one menu
// there is: F9 or F10 (fn+F9 / fn+F10 on a Mac keyboard), the ☰ button, or the
// controller's settings chord opens it. It holds back to the game, full screen,
// other scripts' switches (window.recompMenuToggles, e.g. radar.js's radar) and
// buttons (window.recompMenuActions, e.g. race.js's race together), who is
// a link to Online (F8), the settings, and quit to
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
  /* Carbon draws its own pointer. Override SDL's inline cursor resets over the game only. */
  #canvas { cursor: none !important; }
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

  .cm-panel.cm-panel-online { width:min(880px,100%);clip-path:none;padding:26px;max-height:calc(100dvh - 32px); }
  .cm-online-header { display:flex;align-items:center;justify-content:space-between;gap:20px; }
  .cm-online-eyebrow { color:var(--cyan);font:700 10px/1.2 system-ui,sans-serif;letter-spacing:.2em;margin:0 0 8px; }
  .cm-online-header h2 { margin:0;font-size:26px; }
  .cm-online-close,.cm-online-tabs button,.cm-online-more { cursor:pointer;color:var(--ink);background:#10202d;border:1px solid var(--line);padding:10px 14px;min-height:44px;font:600 13px/1.3 system-ui,sans-serif; }
  .cm-online-close span { color:var(--dim);margin-inline-start:12px;font-size:11px; }
  .cm-online-tabs { display:flex;gap:6px;margin:20px 0; }
  .cm-online-tabs button { flex:1;font:italic 700 15px/1.3 "Avenir Next Condensed",system-ui,sans-serif;letter-spacing:.06em; }
  .cm-online-tabs button[aria-pressed=true] { background:var(--cyan);border-color:var(--cyan);color:#04101a; }
  .cm-online-tools { display:grid;grid-template-columns:1fr 160px;gap:16px; }
  .cm-online-tools label { display:grid;gap:6px;color:var(--dim);font-size:12px;min-width:0; }
  .cm-online-tools input,.cm-online-tools select { width:100%;min-width:0;min-height:44px;padding:10px 12px;background:#08121d;border:1px solid var(--line);color:var(--ink);font:500 14px/1.4 system-ui,sans-serif;border-radius:0; }
  .cm-online-count { color:var(--dim);font-size:12px;margin:14px 0 8px; }
  .cm-online-list { overflow-y:auto;max-height:min(46dvh,440px);scrollbar-color:var(--dim) #08121d;overscroll-behavior:contain; }
  .cm-online-player { width:100%;display:grid;grid-template-columns:minmax(0,1fr) minmax(0,.7fr) auto;gap:12px;align-items:center;text-align:start;min-height:54px;padding:10px 14px;background:rgba(95,228,242,.025);border:0;border-bottom:1px solid rgba(95,228,242,.12);color:var(--ink);cursor:pointer;font:500 14px/1.4 system-ui,sans-serif; }
  .cm-online-player strong { overflow-wrap:anywhere;font-weight:650; }
  .cm-online-player .crew { color:var(--dim);overflow-wrap:anywhere;font-size:12px; }
  .cm-online-player .activity { color:var(--cyan);font-size:11px;white-space:nowrap; }
  .cm-online-player[aria-pressed=true],.cm-online-player:hover { background:rgba(95,228,242,.12); }
  .cm-online-more { width:100%;margin-top:8px; }
  .cm-online-detail { padding:14px;margin-top:12px;background:#0c1e2b;border-inline-start:2px solid var(--cyan);overflow-wrap:anywhere; }
  .cm-online-detail p { margin:6px 0 0;color:var(--dim);font-size:13px; }
  .cm-online-empty { margin:0;padding:28px 14px;color:var(--dim);text-align:center;font-size:14px; }
  .cm-online-invite { padding:14px;background:#0c1e2b;margin-top:8px; }
  .cm-online-invite p { margin:0 0 10px;overflow-wrap:anywhere; }
  .cm-online-close:hover,.cm-online-more:hover { border-color:var(--cyan); }
  .cm-online-view :is(button,input,select):focus-visible { outline:2px solid var(--cyan);outline-offset:2px; }
  .cm-online-view button:disabled { opacity:.45;cursor:default; }
  .cm-online-view button:enabled:active { background:rgba(95,228,242,.22);border-color:var(--cyan); }
  .cm-crew-label { display:grid;gap:8px;color:var(--dim);font-size:12px; }
  .cm-crew-label input { width:100%;min-height:44px;background:#08121d;border:1px solid var(--line);padding:10px 12px;color:var(--ink);font:500 14px/1.4 system-ui,sans-serif; }
  .cm-crew-invite-form { padding:16px;background:#0c1e2b;border-inline-start:2px solid var(--cyan);margin:16px 0; }
  .cm-crew-invite-form .cm-item { margin-top:12px; }
  .cm-crew-member { cursor:default; }
  .cm-crew-member-actions { grid-column:1 / -1;flex-wrap:wrap; }
  .cm-crew-member-actions .cm-item { width:auto;min-height:44px;font-size:12px; }
  .cm-crew-note.bad { color:var(--amber); }
  .cm-crew-controls { max-width:240px; }
  .cm-crew-confirm .cm-row { flex-wrap:wrap; }
  @media(max-width:560px) {
    .cm-panel.cm-panel-online { padding:16px; }
    .cm-online-header { gap:10px; }.cm-online-header h2 { font-size:22px; }
    .cm-online-close { font-size:12px;padding:8px; }.cm-online-close span { display:none; }
    .cm-online-tabs { margin:16px 0; }.cm-online-tabs button { padding:10px 6px;font-size:14px; }
    .cm-online-tools { grid-template-columns:minmax(0,1fr) 110px;gap:10px; }
    .cm-online-player { padding:10px 8px;gap:8px;grid-template-columns:minmax(0,1fr) auto; }
    .cm-online-player .crew { grid-column:1; }.cm-online-player .activity { grid-column:2;grid-row:1 / span 2; }
  }
  .cm [hidden] { display: none !important; }
  @media (prefers-reduced-motion: reduce) { .cm-open, .cm-item { transition: none; } }
  `;

  const workspaceCss = `
  /* Carbon F9 workspace: preserve native settings and registered action handlers. */
  .cm-shade { padding:24px; background:radial-gradient(ellipse at 80% 100%,#16343dbb,transparent 70%),#03090de8; }
  .cm-panel,.cm-panel.cm-panel-online { width:min(1180px,100%); height:min(780px,calc(100dvh - 48px)); max-height:calc(100dvh - 48px); padding:0; clip-path:none; overflow:hidden; display:flex; flex-direction:column; background:#0b1318f5; border:0; border-top:2px solid var(--cyan); box-shadow:0 28px 80px #0008; }
  .cm-panel::before,.cm-panel::after { display:none; }
  .cm-header { display:flex; justify-content:space-between; align-items:center; gap:20px; padding:24px 28px; border-bottom:1px solid var(--line); flex-shrink:0; }
  .cm-brand { color:var(--cyan); font-size:10px; font-weight:750; letter-spacing:.25em; margin:0 0 6px; }
  .cm .cm-title { font:italic 800 30px/1.1 "Avenir Next Condensed","DIN Condensed","Bahnschrift",sans-serif; letter-spacing:.03em; color:var(--ink); margin:0; }
  .cm .cm-title::after { display:none; }
  .cm-resume { padding:12px 18px; background:#5fe4f20b; border:1px solid var(--line); color:var(--ink); cursor:pointer; font:700 11px/1.3 system-ui,sans-serif; letter-spacing:.08em; text-transform:uppercase; }
  .cm-resume span { color:var(--cyan); margin-left:18px; }
  .cm-frame { flex:1; min-height:0; display:grid; grid-template-columns:205px minmax(0,1fr) 270px; }
  .cm-rail { padding:26px 0; overflow:auto; border-right:1px solid var(--line); scrollbar-width:thin; }
  .cm-rail-caption { padding:0 22px; color:var(--dim); font:700 10px/1.3 system-ui,sans-serif; letter-spacing:.17em; margin:0 0 15px; }
  .cm .cm-nav-item { width:100%; min-height:44px; padding:14px 22px; border:0; border-left:3px solid transparent; background:transparent; color:var(--dim); font:600 12px/1.5 system-ui,sans-serif; text-align:left; cursor:pointer; transform:none; margin:0; letter-spacing:0; text-transform:none; justify-content:flex-start; }
  .cm .cm-nav-item>* { transform:none; }
  .cm .cm-nav-item::after,.cm .cm-nav-item .cm-hint { display:none; }
  .cm .cm-nav-item[aria-current=page] { color:var(--cyan); border-left-color:var(--cyan); background:linear-gradient(90deg,#5fe4f21a,transparent); }
  .cm .cm-nav-item:hover { color:var(--ink); background:#ffffff05; }
  .cm-work { min-width:0; overflow:auto; padding:24px 26px; scrollbar-width:thin; scrollbar-color:var(--line) transparent; overscroll-behavior:contain; }
  .cm-search-label { display:block; color:var(--dim); font-size:11px; margin-bottom:6px; }
  .cm-search { width:100%; padding:12px; background:#071015; border:1px solid #3b525e; color:var(--ink); font:13px/1.4 system-ui,sans-serif; border-radius:0; }
  .cm-searchbar { margin-bottom:26px; }
  .cm-work h2 { color:var(--ink); font:650 23px/1.3 "Avenir Next","Segoe UI",system-ui,sans-serif; letter-spacing:-.03em; text-transform:none; }
  .cm-work h2::after { display:none; }
  .cm-work h3 { color:var(--dim); font:700 10px/1.4 system-ui,sans-serif; letter-spacing:.16em; margin-top:26px; }
  .cm-work h3::after { display:none; }
  .cm-work .cm-item { transform:none; min-height:56px; padding:14px 10px; margin:0; border:0; border-bottom:1px solid #ffffff12; background:transparent; text-transform:none; font:600 13px/1.4 "Avenir Next","Segoe UI",system-ui,sans-serif; letter-spacing:0; gap:14px; }
  .cm-work .cm-item>* { transform:none; }
  .cm-work .cm-item:hover,.cm-work .cm-item:focus-visible,.cm.cm-pad .cm-work .cm-item:focus { background:#5fe4f20d; box-shadow:inset 2px 0 var(--cyan); }
  .cm-work .cm-item .cm-hint { font-size:11px; line-height:1.5; max-width:52%; text-align:right; }
  .cm-work .cm-set-row { min-height:70px; scroll-margin:12px; }
  .cm-set-meta { display:block; font:10px/1.5 system-ui,sans-serif; color:var(--dim); margin-top:5px; }
  .cm-work .cm-set-row .cm-val { min-width:72px; max-width:145px; color:var(--cyan); font-size:12px; }
  .cm-work .cm-set-row .cm-arr { opacity:.7; }
  .cm-work .cm-set-row .cm-label { line-height:1.45; }
  .cm-work .cm-ctl { gap:2px; max-width:50%; }
  .cm-inspector { min-width:0; overflow:auto; padding:28px 24px; border-left:1px solid var(--line); background:#ffffff02; scrollbar-width:thin; }
  .cm-inspector-label { color:var(--dim); font:10px/1.4 system-ui,sans-serif; letter-spacing:.15em; margin-bottom:26px; }
  .cm-inspector-icon { display:grid; place-items:center; width:38px; height:38px; border:1px solid var(--line); color:var(--cyan); font:italic 22px/1 Georgia,serif; margin-bottom:22px; }
  .cm-guide-title { margin:0 0 14px; font:600 17px/1.45 "Avenir Next","Segoe UI",system-ui,sans-serif; }
  .cm-inspector .cm-help { position:static; margin:0; padding:0; min-height:0; background:none; border:0; box-shadow:none; font-size:12px; line-height:1.8; }
  .cm-inspector .cm-help b { margin-bottom:10px; font:700 10px/1.5 system-ui,sans-serif; }
  .cm-guide-how { margin:22px 0 0; padding-top:18px; border-top:1px solid var(--line); font-size:12px; line-height:1.8; color:var(--dim); }
  .cm-guide-how strong { display:block; color:var(--cyan); font-size:10px; letter-spacing:.1em; margin-bottom:8px; }
  .cm-bottom { padding:16px 28px; border-top:1px solid var(--line); color:var(--dim); font:11px/1.6 system-ui,sans-serif; display:flex; justify-content:space-between; gap:16px; flex-shrink:0; }
  .cm-bottom kbd { font:10px/1.3 system-ui,sans-serif; border:1px solid #405763; padding:2px 5px; margin-right:5px; color:var(--ink); }
  .cm-work .cm-online-header { display:none; }
  .cm-work .cm-online-tabs { gap:5px; margin:12px 0 20px; }
  .cm-work .cm-online-tabs button { padding:10px 5px; font:600 12px/1.4 system-ui,sans-serif; }
  .cm-work .cm-online-tools { grid-template-columns:minmax(0,1fr) 120px; gap:10px; }
  .cm-work .cm-online-player { grid-template-columns:minmax(0,1fr) auto; padding:12px 8px; }
  .cm-work .cm-online-player .crew { grid-column:1; }.cm-work .cm-online-player .activity { grid-column:2; grid-row:1 / span 2; }
  .cm-work .cm-online-list { max-height:340px; }
  .cm-guide-answer { padding:14px 10px 18px; font:12px/1.8 system-ui,sans-serif; color:var(--dim); border-left:2px solid var(--line); }
  .cm-guide-answer ol { margin:0; padding-left:20px; }.cm-guide-answer li { margin-bottom:6px; }.cm-guide-answer p { margin:12px 0 0; }
  .cm-gpu { margin-bottom:18px; }
  .cm-gpu-toggle>span:first-child { min-width:0; }
  .cm-gpu-label { display:block; color:var(--dim); font:11px/1.5 system-ui,sans-serif; }
  .cm-gpu-name { display:block; margin-top:4px; color:var(--cyan); font:600 15px/1.5 system-ui,sans-serif; overflow-wrap:anywhere; }
  .cm-gpu-symbol { color:var(--dim); flex-shrink:0; }
  .cm-gpu-details>p:first-child { margin-top:0; }
  .cm-gpu-details strong { color:var(--ink); }
  .cm-gpu-details ol { margin-top:12px; }
  .cm-help-result { display:block; }.cm-help-result span { display:block; }.cm-help-result small { display:block; color:var(--dim); font-size:10px; font-weight:400; margin-top:4px; }
  .cm-work .cm-up { margin-right:6px; transform:none; }.cm-work .cm-up>span { transform:none; }
  .cm :is(button,input,select):focus-visible { outline:2px solid var(--cyan); outline-offset:2px; }
  @media(max-width:1000px) {
    .cm-frame { grid-template-columns:180px minmax(0,1fr); overflow:auto; }.cm-work { overflow:visible; }.cm-rail { grid-row:1 / span 2; }
    .cm-inspector { grid-column:2; border-left:0; border-top:1px solid var(--line); overflow:visible; padding:22px 26px; }.cm-inspector-icon { display:none; }.cm-inspector-label { margin-bottom:12px; }
  }
  @media(max-width:600px) {
    .cm-shade { padding:8px; }.cm-panel,.cm-panel.cm-panel-online { height:calc(100dvh - 16px); max-height:calc(100dvh - 16px); }.cm-header { padding:18px 16px; }.cm .cm-title { font-size:24px; }.cm-brand { font-size:8px; }.cm-resume { padding:10px; font-size:10px; }.cm-resume span { margin-left:8px; }
    .cm-frame { display:block; overflow:auto; }.cm-rail { display:flex; padding:6px; border-right:0; border-bottom:1px solid var(--line); overflow:auto; position:sticky; top:0; z-index:2; background:#0b1318; }.cm-rail-caption { display:none; }.cm .cm-nav-item { width:auto; flex-shrink:0; padding:12px; font-size:11px; white-space:nowrap; }
    .cm-work { padding:20px 16px; overflow:visible; }.cm-inspector { padding:20px 16px; }.cm-work .cm-item { font-size:12px; }.cm-work .cm-item .cm-hint { font-size:10px; }.cm-work .cm-set-row .cm-val { min-width:60px; max-width:105px; font-size:11px; }.cm-work .cm-ctl { gap:0; }.cm-work .cm-arr { padding:2px 4px; }.cm-bottom { padding:12px 16px; font-size:10px; flex-wrap:wrap; gap:6px; }.cm-online-tools { grid-template-columns:minmax(0,1fr) 110px; }
  }

  `;

  const onlineCss = `
  /* Online uses the workspace for people and actions, rather than generic setting help. */
  .cm-panel-online .cm-frame { grid-template-columns:205px minmax(0,1fr); }
  .cm-panel-online .cm-inspector,.cm-panel-online .cm-searchbar { display:none; }
  .cm-panel-online .cm-work { padding:28px 32px; }
  .cm-online-heading { display:flex; justify-content:space-between; align-items:flex-start; gap:24px; margin-bottom:24px; }
  .cm-online-heading h2 { margin:0 0 6px; font-size:28px; }
  .cm-online-heading p { margin:0; color:#a6b9c2; font:13px/1.6 system-ui,sans-serif; }
  .cm-online-heading .cm-who { text-align:end; max-width:250px; padding-top:5px; font-size:12px; font-variant-numeric:tabular-nums; }
  .cm-online-heading .cm-who::before { content:'●'; color:var(--cyan); margin-inline-end:8px; }
  .cm-online-heading .cm-who[data-connected=false]::before { color:var(--amber); }
  .cm-panel-online .cm-online-tabs { justify-content:flex-start; gap:24px; margin:0 0 24px; border-bottom:1px solid var(--line); }
  .cm-panel-online .cm-online-tabs button { flex:0 1 auto; background:transparent; border:0; border-bottom:2px solid transparent; padding:13px 4px; font:600 14px/1.5 system-ui,sans-serif; color:#a6b9c2; }
  .cm-panel-online .cm-online-tabs button[aria-pressed=true] { color:var(--cyan); background:transparent; border-bottom-color:var(--cyan); }
  .cm-online-players { display:grid; grid-template-columns:minmax(0,1fr) 250px; gap:28px; align-items:start; }
  .cm-player-browser { min-width:0; }
  .cm-panel-online .cm-online-tools { display:grid; grid-template-columns:minmax(0,1fr) 140px; gap:12px; }
  .cm-panel-online .cm-online-tools label,.cm-panel-online .cm-crew-label { font-size:12px; color:#a6b9c2; }
  .cm-panel-online :is(.cm-online-tools input,.cm-online-tools select,.cm-crew-form input,.cm-crew-friend) { font:14px/1.5 system-ui,sans-serif; padding:11px 12px; min-height:44px; background:#071015; border:1px solid #4c6979; }
  .cm-panel-online .cm-online-count { margin:18px 0 10px; color:#a6b9c2; font-size:12px; font-variant-numeric:tabular-nums; }
  .cm-panel-online .cm-online-list { max-height:none; overflow:visible; }
  .cm-panel-online .cm-online-player { grid-template-columns:34px minmax(0,1fr) auto; gap:12px; min-height:76px; padding:14px 10px; background:transparent; font-size:14px; border-bottom:1px solid #ffffff13; }
  .cm-player-avatar { display:grid; place-items:center; width:34px; height:34px; background:#18313c; color:var(--cyan); font:700 12px/1 system-ui,sans-serif; letter-spacing:.02em; }
  .cm-player-identity { min-width:0; }
  .cm-player-identity strong { display:block; overflow-wrap:anywhere; font-weight:650; }
  .cm-panel-online .cm-online-player .crew { display:block; margin-top:3px; color:#a6b9c2; font-size:12px; grid-column:auto; }
  .cm-panel-online .cm-online-player .activity { grid-column:auto; grid-row:auto; color:#b7cbd5; font-size:12px; white-space:normal; text-align:end; max-width:90px; }
  .cm-panel-online button.cm-online-player[aria-pressed=true] { box-shadow:inset 2px 0 var(--cyan); background:#5fe4f20c; }
  .cm-panel-online .cm-online-detail { min-width:0; position:sticky; top:0; padding:22px 0 22px 24px; margin:0; border:0; border-inline-start:1px solid var(--line); background:transparent; overflow-wrap:anywhere; }
  .cm-online-detail h3 { font:650 20px/1.35 system-ui,sans-serif; color:var(--ink); letter-spacing:-.02em; text-transform:none; margin:16px 0 6px; }
  .cm-online-detail p { margin:8px 0 0; font-size:13px; line-height:1.7; color:#a6b9c2; }
  .cm-online-detail .cm-player-avatar { width:48px; height:48px; font-size:17px; }
  .cm-panel-online .cm-online-detail .cm-item { margin-top:22px; }
  .cm-online-detail .cm-detail-label { font-size:11px; text-transform:uppercase; letter-spacing:.12em; margin-bottom:18px; }
  .cm-online-detail .cm-detail-note { margin-top:24px; padding-top:18px; border-top:1px solid var(--line); font-size:12px; }
  .cm-panel-online .cm-online-empty { padding:28px 12px; text-align:start; color:#a6b9c2; font-size:14px; line-height:1.7; }
  .cm-panel-online .cm-online-empty strong { display:block; color:var(--ink); margin-bottom:8px; }
  .cm-panel-online .cm-online-empty .cm-item { margin-top:18px; }
  .cm-panel-online .cm-online-more { background:transparent; color:var(--cyan); border:1px solid #4c6979; font-size:12px; margin-top:16px; }
  .cm-online-crew { max-width:850px; }
  .cm-crew-heading { display:flex; justify-content:space-between; align-items:center; gap:24px; margin-bottom:12px; }
  .cm-crew-heading h3 { margin:0 0 5px; font:650 24px/1.3 system-ui,sans-serif; color:var(--ink); letter-spacing:-.03em; text-transform:none; }
  .cm-crew-role { color:#a6b9c2; font:12px/1.5 system-ui,sans-serif; margin:0; }
  .cm-panel-online .cm-crew-edit { width:auto; padding:10px 16px; min-height:44px; font-size:12px; border:1px solid #4c6979; background:transparent; white-space:nowrap; }
  .cm-panel-online .cm-crew-summary { margin:0 0 22px; color:#a6b9c2; font-size:13px; line-height:1.7; max-width:65ch; }
  .cm-panel-online .cm-crew-form { padding:20px; margin:0 0 24px; border:1px solid #4c6979; background:#101e26; }
  .cm-panel-online .cm-crew-form .cm-row { gap:12px; flex-wrap:wrap; margin-top:12px; }
  .cm-panel-online .cm-crew-form .cm-item { padding:10px 16px; min-height:44px; }
  .cm-panel-online .cm-crew-invite-form { border:0; border-top:1px solid var(--line); padding:22px 0; margin:0 0 12px; background:transparent; display:grid; grid-template-columns:minmax(0,1fr) auto; align-items:end; gap:10px 14px; }
  .cm-crew-invite-form .cm-note { grid-column:1/-1; margin:0; font-size:12px; color:#a6b9c2; }
  .cm-panel-online .cm-crew-invite-form .cm-crew-invite { margin:0; width:auto; padding:12px 18px; min-height:44px; }
  .cm-online-crew .cm-member-heading { margin:24px 0 8px; }
  .cm-panel-online .cm-online-player.cm-crew-member { grid-template-columns:minmax(0,1fr) auto auto; align-items:center; font-size:14px; gap:10px 20px; }
  .cm-member-name { min-width:0; }.cm-member-name strong { display:block; overflow-wrap:anywhere; }
  .cm-panel-online .cm-crew-member .cm-crew-member-actions { grid-column:1/-1; gap:12px; padding-top:4px; }
  .cm-panel-online .cm-crew-member .cm-crew-member-actions .cm-item { width:auto; padding:10px 14px; min-height:44px; font-size:12px; border:1px solid #4c6979; }
  .cm-panel-online .cm-member-manage { background:transparent; border:1px solid #4c6979; min-height:40px; padding:8px 12px; color:#b7cbd5; font:12px/1.4 system-ui,sans-serif; cursor:pointer; }
  .cm-panel-online .cm-crew-controls .cm-item,.cm-panel-online .cm-danger { color:#f4a7a7; }
  .cm-crew-management { border-top:1px solid var(--line); margin-top:28px; padding-top:16px; }
  .cm-panel-online .cm-crew-settings { width:auto; min-height:44px; padding:10px 12px; border:1px solid #4c6979; color:#b7cbd5; }
  .cm-panel-online .cm-crew-controls { padding:8px 0 0; max-width:300px; }
  .cm-panel-online .cm-crew-confirm { border:1px solid #ab6868; background:#271d24; padding:20px; margin:18px 0; }
  .cm-panel-online .cm-crew-confirm .cm-item { width:auto; padding:10px 16px; border:1px solid #80626b; }
  .cm-panel-online .cm-crew-confirm-yes { color:#f4a7a7; }
  .cm-panel-online .cm-crew-confirm p { font-size:14px; line-height:1.7; margin-bottom:18px; }
  .cm-online-invite { background:#101e26; padding:20px; margin:14px 0; border:1px solid var(--line); }
  .cm-panel-online .cm-online-invite p { font-size:14px; line-height:1.6; margin:0 0 14px; }
  .cm-panel-online .cm-online-invite .cm-row { gap:12px; flex-wrap:wrap; }
  .cm-panel-online .cm-online-invite .cm-item { width:auto; min-height:44px; padding:10px 18px; border:1px solid #4c6979; }
  .cm-panel-online .cm-online-invite .cm-note { margin:14px 0 0; font-size:12px; color:#a6b9c2; }
  .cm-panel-online .cm-work .cm-primary { background:var(--cyan); color:#071015; border:1px solid var(--cyan); box-shadow:none; }
  .cm-panel-online .cm-primary:focus-visible { outline-offset:4px; }
  .cm-panel-online button:disabled { opacity:.5; cursor:default; box-shadow:none; }
  .cm-panel-online .cm-work .cm-primary:focus,.cm.cm-pad .cm-panel-online .cm-work .cm-primary:focus { background:var(--cyan); color:#071015; }
  .cm-panel-online .cm-crew-help { width:auto; padding:10px 16px; border:1px solid #4c6979; }
  .cm-panel-online .cm-crew-outbox { margin-top:26px; }
  @media(hover:hover) {
    .cm-panel-online button.cm-online-player:hover { background:#5fe4f20c; }
    .cm-panel-online .cm-primary:not(:disabled):hover { background:#a0f3f8; color:#071015; }
    .cm-panel-online .cm-member-manage:not(:disabled):hover { border-color:var(--cyan); color:var(--cyan); }
  }
  @media(max-width:1050px) {
    .cm-panel-online .cm-frame { grid-template-columns:180px minmax(0,1fr); }
    .cm-online-players { grid-template-columns:minmax(0,1fr) 210px; gap:20px; }
    .cm-panel-online .cm-work { padding:24px; }
  }
  @media(max-width:850px) {
    .cm-online-players { display:block; }
    .cm-panel-online .cm-online-detail { position:static; border-inline-start:0; border-top:1px solid var(--line); padding:24px 0 0; margin-top:24px; }
    .cm-online-heading { flex-direction:column; gap:8px; }.cm-online-heading .cm-who { text-align:start; max-width:none; }
  }
  @media(max-width:600px) {
    .cm-panel-online .cm-frame { display:block; }.cm-panel-online .cm-work { padding:22px 16px; }
    .cm-online-heading h2 { font-size:24px; }.cm-online-heading p { font-size:13px; }
    .cm-panel-online .cm-online-tabs { gap:18px; }.cm-panel-online .cm-online-tabs button { font-size:13px; }
    .cm-panel-online .cm-online-tools { grid-template-columns:minmax(0,1fr); gap:12px; }
    .cm-panel-online :is(.cm-online-tools input,.cm-online-tools select,.cm-crew-form input,.cm-crew-friend) { font-size:16px; }
    .cm-panel-online .cm-online-player { grid-template-columns:30px minmax(0,1fr) auto; padding:14px 6px; gap:10px; }
    .cm-panel-online .cm-online-player .activity { font-size:11px; max-width:64px; }
    .cm-player-avatar { width:30px; height:30px; font-size:11px; }
    .cm-crew-heading { flex-wrap:wrap; gap:14px; }.cm-crew-heading h3 { font-size:22px; }
    .cm-panel-online .cm-crew-invite-form { grid-template-columns:minmax(0,1fr); }
    .cm-panel-online .cm-crew-invite-form .cm-crew-invite { width:100%; justify-content:center; }
    .cm-panel-online .cm-online-player.cm-crew-member { grid-template-columns:minmax(0,1fr) auto; gap:10px; }
    .cm-member-manage { grid-column:2; }.cm-panel-online .cm-crew-member .activity { max-width:100px; }
    .cm-panel-online .cm-crew-form .cm-row { display:flex; }.cm-panel-online .cm-crew-form input { width:100%; flex-basis:100%; }
  }

  `;

  const onlineLayoutCss = `
  .cm-crew-body { display:grid; grid-template-columns:minmax(0,1fr) 260px; gap:28px; align-items:start; }
  .cm-crew-roster,.cm-crew-tools { min-width:0; }
  .cm-roster-note {color:#a6b9c2;font:12px/1.6 system-ui,sans-serif;margin:0 0 8px;}
  .cm-panel-online .cm-roster-refresh {width:auto;font-size:12px;color:var(--cyan);margin-top:12px;min-height:44px;}
  @media(max-width:600px) { .cm-panel-online .cm-frame {scroll-padding-block-start:70px;} .cm-panel-online .cm-work :is(button,input,select) {scroll-margin-block:70px 12px;} }
  .cm-crew-tools { border-inline-start:1px solid var(--line); padding-inline-start:24px; }
  .cm-crew-body .cm-member-heading { margin-top:0; }
  .cm-panel-online .cm-crew-body .cm-crew-invite-form { display:block; border-top:0; margin:0; padding:0; }
  .cm-panel-online .cm-crew-body .cm-crew-invite { width:100%; justify-content:center; margin-top:12px; }
  .cm-panel-online .cm-crew-body .cm-crew-invite-form .cm-note { margin-top:12px; }
  .cm-panel-online .cm-crew-invite-shortcut { display:none; }
  .cm-panel-online .cm-crew-member .activity { max-width:105px; }
  @media(max-width:1050px) { .cm-crew-body { grid-template-columns:minmax(0,1fr); }.cm-crew-tools { border-inline-start:0; border-top:1px solid var(--line); padding-inline-start:0; padding-top:24px; }.cm-panel-online .cm-crew-invite-shortcut { display:flex; width:auto; margin-bottom:24px; padding:12px 18px; }.cm-panel-online .cm-crew-body .cm-crew-invite { width:auto; } }
  @media(max-width:850px) { .cm-panel-online .cm-online-list .cm-online-detail { background:#101e26; border:1px solid var(--line); padding:20px; margin:12px 0; } }
`;

  const root = document.createElement("div");
  root.className = "cm";
  root.innerHTML = `
  <style>${css}${workspaceCss}${onlineCss}${onlineLayoutCss}</style>
  <button class="cm-open" type="button" aria-label="Menu (F9 or F10)" title="Menu (F9 or F10)"><span>☰</span></button>
  <div class="cm-shade" hidden>
    <nav class="cm-panel" role="dialog" aria-modal="true" aria-label="Menu">
      <div class="cm-main">
        <h2>Menu</h2>
        <button class="cm-item cm-back" type="button"><span>Back to the game</span><span class="cm-hint">F9 · F10 · Esc</span></button>
        <button class="cm-item cm-full" type="button"><span>Full screen</span><span class="cm-val"></span></button>
        <div class="cm-toggles"></div>
        <button class="cm-item cm-online-go" type="button"><span>Online</span><span class="cm-hint">F8 · Players &amp; invitations</span></button>
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
      <section class="cm-online cm-online-view" hidden aria-label="Online">
        <header class="cm-online-header"><div><p class="cm-online-eyebrow">CARBON · ONLINE</p><h2>Online</h2></div><button class="cm-online-close" type="button" aria-label="Back to game — close Online">Back to game <span>Esc</span></button></header>
        <p class="cm-who" role="status"></p>
        <p class="cm-crew-note cm-note" role="status" hidden></p>
        <div class="cm-online-tabs" aria-label="Online views">
          <button type="button" data-online-tab="players" aria-pressed="true">Players</button>
          <button type="button" data-online-tab="crew" aria-pressed="false">My crew</button>
          <button type="button" data-online-tab="invites" aria-pressed="false">Invitations</button>
        </div>
        <div class="cm-online-players">
          <div class="cm-online-tools"><label>Find a player<input class="cm-online-search" type="search" autocomplete="off" maxlength="64" placeholder="Player or crew name"></label><label>Show players<select class="cm-online-filter"><option value="all">All online</option><option value="nearby">Nearby</option><option value="crew">My crew</option></select></label></div>
          <p class="cm-online-count" role="status"></p>
          <div class="cm-online-list" aria-label="Players"></div>
          <button class="cm-online-more" type="button" hidden>Show more players</button>
          <div class="cm-online-detail" hidden></div>
        </div>
        <div class="cm-online-crew" hidden>
          <button class="cm-item cm-crew-edit" type="button"><span>Your crew</span><span class="cm-crew-now"></span></button>
          <div class="cm-crew-form" hidden>
            <p>A unique name protects your crew. Friends join only after accepting your invitation.</p>
            <div class="cm-row">
              <input class="cm-crew-in" type="text" maxlength="16" autocomplete="off" spellcheck="false" placeholder="Crew name" aria-label="Crew name">
              <button class="cm-item cm-crew-ok" type="button"><span>Create</span></button>
            </div>
          </div>
          <p class="cm-note cm-crew-summary"></p>
          <div class="cm-crew-controls"></div>
          <div class="cm-crew-confirm cm-confirm" hidden><p></p><div class="cm-row"><button class="cm-item cm-crew-confirm-yes" type="button">Confirm</button><button class="cm-item cm-crew-confirm-no" type="button">Cancel</button></div></div>
          <div class="cm-crew-invite-form" hidden><label class="cm-crew-label">Invite a friend by protected player name<input class="cm-crew-friend" type="text" maxlength="20" autocomplete="off" placeholder="Player name"></label><button class="cm-item cm-crew-invite" type="button"><span>Send invitation</span></button><p class="cm-note">Your friend can accept later. Invitations expire after seven days.</p></div>
          <div class="cm-online-members cm-online-list" aria-label="Crew members"></div>
          <div class="cm-row"><button class="cm-online-more cm-online-crew-prev" type="button" hidden>Previous members</button><button class="cm-online-more cm-online-crew-more" type="button" hidden>Next members</button></div>
          <div class="cm-crew-outbox"></div>
        </div>
        <div class="cm-online-invites" hidden><div class="cm-crew-inbox"></div><div class="cm-online-actions"></div><div class="cm-online-invitations"></div></div>
      </section>
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

  const onlinePage = $(".cm-online-view"), onlineDetail = $(".cm-online-detail");
  const compactOnline = matchMedia("(max-width:850px)");
  compactOnline.addEventListener("change",()=>{onlineSignature="";if(onlineOpen){renderOnline();if(matchMedia("(max-width:600px)").matches)railButtons.get(menuView)?.scrollIntoView({block:"nearest",inline:"nearest"});}});
  let onlineOpen = false, onlineTab = "players", playerLimit = 50, selectedPlayer = null, onlineSignature = "", onlineReturn = null;

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
    $(".cm-online-go").hidden = !window.recompOnline && !window.recompOnlineState;
    showCrew();
    updateRail();
    if (onlineOpen) renderOnline();
  }
  addEventListener("recomp-online", () => { if (!shade.hidden) refresh(); });

  // Escape belongs to the game's pause menu while fullscreen. The browser
  // retains its emergency exit and may deny keyboard lock.
  async function fullscreenKeys() {
    if (!document.fullscreenElement) {
      navigator.keyboard?.unlock?.();
      return;
    }
    try { await navigator.keyboard?.lock?.(["Escape"]); }
    catch (error) { console.warn("[controls] fullscreen keyboard lock unavailable:", error?.message || String(error)); }
    if (!document.fullscreenElement) navigator.keyboard?.unlock?.();
  }
  document.addEventListener("fullscreenchange", () => {
    void fullscreenKeys();
    if (!shade.hidden) refresh();
  });
  if (document.fullscreenElement) void fullscreenKeys();
  addEventListener("keydown", e => {
    if (document.fullscreenElement && e.code === "Escape") e.preventDefault();
  }, true);


  function openOnline() {
    onlineReturn = shade.hidden ? document.activeElement : null;
    if (shade.hidden) open();
    activateMenu("online");
    onlineOpen = true;
    page = null;rowEls.clear();
    mainPage.hidden = setPage.hidden = checkPage.hidden = true;
    checking = false;
    onlinePage.hidden = false;
    $(".cm-panel").classList.add("cm-panel-online");
    $(".cm-panel").setAttribute("aria-label", "Online");
    onlineSignature = "";
    renderOnline();
    void loadCrewPages();
    root.querySelector(`[data-online-tab="${onlineTab}"]`).focus();
  }
  function playerInitials(name) {
    return String(name || "Player").trim().split(/\s+/).slice(0,2).map(part=>[...part][0]||"").join("").toLocaleUpperCase();
  }
  function chooseOnlineTab(tab) {
    onlineTab = tab;
    playerLimit = 50;
    onlineSignature = "";
    renderOnline();
    if (tab !== "players") void loadCrewPages();
  }
  function renderOnline() {
    const state = window.recompOnlineState || {}, config = window.recompOnline || {};
    const me = state.name || config.name || "You", mine = crewNow();
    const peers = state.open ? (state.people || []).filter(p => p.id !== state.me) : [];
    const nearby = new Set((state.slotIds || []).filter(Boolean));
    const query = $(".cm-online-search").value.trim().toLocaleLowerCase();
    const filter = $(".cm-online-filter").value;
    const invitationState = window.recompRace?.state?.();
    const invitations = state.open ? invitationState?.invites || [] : [];
    const signature = JSON.stringify([onlineTab, query, filter, playerLimit, selectedPlayer, mine, state.open, me, compactOnline.matches,
      state.crewId, peers.map(p => [p.id, p.name, p.crew, p.crewId, p.account, p.activity, nearby.has(p.id)]),
      invitations.map(i => [i.race, i.name, i.joined]), invitationState?.game?.flow, invitationState?.game?.car,
      !!invitationState?.race, invitationState?.lobby?.roster?.length, invitationState?.hosting]);
    if (signature === onlineSignature) return;
    onlineSignature = signature;
    $(".cm-who").dataset.connected = String(!!state.open);
    const connectionText = state.open ? `${peers.length + 1} online · You are ${me}` : "Connecting to online play…";
    if ($(".cm-who").textContent !== connectionText) $(".cm-who").textContent = connectionText;
    for (const b of root.querySelectorAll("[data-online-tab]")) b.setAttribute("aria-pressed", String(b.dataset.onlineTab === onlineTab));
    const pending=(crewState().pending||0)+invitations.length;
    root.querySelector('[data-online-tab="invites"]').textContent=pending?`Invitations (${pending})`:"Invitations";
    $(".cm-online-players").hidden = onlineTab !== "players";
    $(".cm-online-crew").hidden = onlineTab !== "crew";
    $(".cm-online-invites").hidden = onlineTab !== "invites";
    if (onlineTab === "crew") {showCrew();return;}
    const empty = (parent, message) => { const p = document.createElement("p");p.className = "cm-online-empty";p.textContent = message;parent.append(p); };
    if (onlineTab === "invites") {
      showCrew();
      const list = $(".cm-online-invitations");
      const focused = document.activeElement?.dataset.raceInvite;
      list.replaceChildren();
      if (!invitations.length) empty(list, "No race invitations. Nearby rival challenges appear in the game.");
      for (const inv of invitations) {
        const item = document.createElement("div"), title = document.createElement("p"), action = document.createElement("button");
        item.className = "cm-online-invite";title.textContent = `${inv.world ? "World map race" : "Quick Race"} · ${inv.name || "Another player"}`;
        action.type = "button";action.className = "cm-item";action.dataset.raceInvite = String(inv.race);
        action.textContent = inv.joined ? "Leave invitation" : "Join race";
        const ready = inv.joined || (window.recompRace?.canJoin?.(inv) ?? (!invitationState?.race && !invitationState?.lobby && invitationState?.game?.flow === 3 && invitationState?.game?.car));
        action.disabled = !ready;
        action.addEventListener("click", () => {
          if (inv.joined) window.recompRace?.leave?.(inv.race); else window.recompRace?.join?.(inv.race);
          onlineSignature = "";renderOnline();drawActions();
        });
        item.append(title, action);
        if (!ready) { const note = document.createElement("p");note.className = "cm-note";note.textContent = inv.world ? "Enter career free roam to join." : "Return to the game's main menu to join.";item.append(note); }
        list.append(item);
      }
      if (focused) [...list.querySelectorAll("button")].find(b => b.dataset.raceInvite === focused)?.focus({preventScroll:true});
      return;
    }
    let matches = peers.filter(p =>
      (filter === "all" || filter === "nearby" && nearby.has(p.id) || filter === "crew" && state.crewId && p.crewId === state.crewId) &&
      (!query || `${p.name} ${p.crew || ""}`.toLocaleLowerCase().includes(query)));
    matches = matches.slice().sort((a, b) => String(a.name).localeCompare(String(b.name)) || a.id - b.id);
    const list = $(".cm-online-players .cm-online-list");
    const scroll = list.scrollTop, focused = document.activeElement?.dataset.playerId;
    $(".cm-online-players").append(onlineDetail);
    list.replaceChildren();
    if (!matches.length) {
      const box=document.createElement("div"),title=document.createElement("strong"),body=document.createElement("span");box.className="cm-online-empty";
      title.textContent=!state.open?"Online connection unavailable":query?"No matching players":filter==="nearby"?"No players nearby":filter==="crew"?"No crew members here":"You are the only player here";
      body.textContent=!state.open?"Check Online and your server selection on the start page.":query?"Try another player name or clear your filters.":filter!=="all"?"Show all online players to find other drivers.":"Ask a friend to join the same server and room.";box.append(title,body);
      if(state.open&&(query||filter!=="all")){const clear=crewButton("Show all players",()=>{$(".cm-online-search").value="";$(".cm-online-filter").value="all";onlineSignature="";renderOnline();$(".cm-online-search").focus();});box.append(clear);}
      list.append(box);
    }
    const activity = p => p.activity === "race" ? "Racing" : p.activity === "roam" ? "Free roam" : p.activity === "self" ? "You" : nearby.has(p.id) ? "Nearby" : "Online";
    for (const player of matches.slice(0, playerLimit)) {
      const row = document.createElement("button"), name = document.createElement("strong"), crew = document.createElement("span"), status = document.createElement("span");
      row.type = "button";row.className = "cm-online-player";row.dataset.playerId = String(player.id);
      row.setAttribute("aria-pressed", String(player.id === selectedPlayer));
      name.textContent = player.name || "Player";crew.className = "crew";crew.textContent = player.crew || "No crew";
      status.className = "activity";status.textContent = activity(player);
      const avatar=document.createElement("span"),identity=document.createElement("span");avatar.className="cm-player-avatar";avatar.setAttribute("aria-hidden","true");avatar.textContent=playerInitials(player.name);identity.className="cm-player-identity";identity.append(name,crew);
      row.append(avatar, identity, status);
      row.addEventListener("click", () => { selectedPlayer = selectedPlayer === player.id ? null : player.id;onlineSignature = "";renderOnline(); });
      list.append(row);
    }
    list.scrollTop = scroll;
    if (focused) {
      const row = [...list.querySelectorAll("button")].find(b => b.dataset.playerId === focused);
      if (row) row.focus({preventScroll:true});else $(".cm-online-search").focus();
    }
    const more = $(".cm-online-more");
    more.hidden = onlineTab !== "players" || matches.length <= playerLimit;
    more.textContent = `Show more · ${Math.max(0, matches.length - playerLimit)} remaining`;
    const countText = `${matches.length} ${matches.length === 1 ? "player" : "players"} · showing ${Math.min(matches.length, playerLimit)}`;
    if ($(".cm-online-count").textContent !== countText) $(".cm-online-count").textContent = countText;
    const detail = onlineDetail, selected = matches.find(p => p.id === selectedPlayer);
    detail.hidden = false;detail.replaceChildren();
    const eyebrow=document.createElement("p");eyebrow.className="cm-detail-label";eyebrow.textContent="Player details";detail.append(eyebrow);
    if (selected) {
      const avatar=document.createElement("span"),name=document.createElement("h3"),description=document.createElement("p"),note=document.createElement("p");
      avatar.className="cm-player-avatar";avatar.setAttribute("aria-hidden","true");avatar.textContent=playerInitials(selected.name);
      name.textContent=selected.name;description.textContent=`${selected.crew || "No crew"} · ${activity(selected)}`;detail.append(avatar,name,description);
      const cs=crewState();note.className="cm-detail-note";
      if (state.open && cs.crew?.leader === cs.account && cs.account && !selected.crewId && selected.account) {
        const invite=crewButton("Invite to crew",()=>{chooseOnlineTab("crew");crewAction("invite",{name:selected.name});},`invite-${selected.id}`);invite.classList.add("cm-primary");detail.append(invite);
        note.textContent=`Invite ${selected.name} to ${cs.crew.name}. They join after accepting.`;
      } else note.textContent=selected.crewId ? `${selected.name} already belongs to a crew.` : !selected.account ? "This player needs to protect their name before receiving crew invitations." : !cs.crew ? "Create or join a crew to play under the same crew name." : "Only your crew leader can invite players.";
      detail.append(note);
    } else {
      const title=document.createElement("h3"),description=document.createElement("p");title.textContent="Choose a player";description.textContent="Select a driver to see their crew, activity, and available actions.";detail.append(title,description);
    }
    if(compactOnline.matches&&selected){const selectedRow=[...list.querySelectorAll("[data-player-id]")].find(el=>el.dataset.playerId===String(selected.id));selectedRow?.after(detail);}
  }
  $(".cm-online-go").addEventListener("click", openOnline);
  $(".cm-online-close").addEventListener("click", close);
  for (const b of root.querySelectorAll("[data-online-tab]")) {
    b.addEventListener("click", () => chooseOnlineTab(b.dataset.onlineTab));
    b.addEventListener("keydown", e => {
      if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(e.key)) return;
      e.preventDefault();e.stopPropagation();
      const tabs = [...root.querySelectorAll("[data-online-tab]")], at = tabs.indexOf(b);
      const next = e.key === "Home" ? tabs[0] : e.key === "End" ? tabs.at(-1) : tabs[(at + (e.key === "ArrowRight" ? 1 : tabs.length - 1)) % tabs.length];
      next.click();next.focus();
    });
  }
  $(".cm-online-search").addEventListener("input", () => { playerLimit = 50;onlineSignature = "";$(".cm-online-players .cm-online-list").scrollTop = 0;renderOnline(); });
  $(".cm-online-filter").addEventListener("change", () => { playerLimit = 50;onlineSignature = "";renderOnline(); });
  $(".cm-online-more").addEventListener("click", () => { playerLimit += 50;onlineSignature = "";renderOnline(); });
  addEventListener("recomp-open-online", openOnline);
  addEventListener("recomp-race-invitations", () => {
    if (!onlineOpen) return;
    const before = onlineSignature, focused = document.activeElement?.dataset.id;
    renderOnline();
    if (before !== onlineSignature) {
      drawActions();
      if (focused) [...root.querySelectorAll(".cm-action")].find(b => b.dataset.id === focused)?.focus({preventScroll:true});
    }
  });

  // ---- Protected crews ----
  const crewForm = $(".cm-crew-form"), crewIn = $(".cm-crew-in"), crewNote = $(".cm-crew-note");
  const crewNow = () => window.recompOnlineState?.crew || "";
  const crewClean = v => v.replace(/[^A-Za-z0-9 _.-]/g, "");
  const crewState = () => window.recompCrews || { supported: false, account: null, crew: null, pending: 0 };
  const crewErrors = {
    reserved: "The Admin name and crew are reserved for the protected Admin account.",
    unavailable: "This server does not support protected crews yet.",
    identity: "Protect your player name on the start page with your cloud keyphrase first.",
    target: "That player needs a protected name before you can invite them.",
    taken: "That crew name is already owned. Choose another name.",
    name: "Use 1–16 A–Z letters, numbers, spaces, underscores, dots or hyphens. Include a letter or number.",
    "one-crew": "You already belong to a crew. Leave it before joining another.",
    "target-crew": "That player already belongs to a crew.",
    forbidden: "Only the current crew leader can do that.",
    invitation: "That invitation is no longer available. It may have expired or been revoked.",
    "leader-leave": "Transfer leadership first, or disband your crew.",
    "no-crew": "You no longer belong to this crew.",
    member: "That member is no longer available for this action.",
    stale: "Your crew changed. The list has been refreshed. Review it and try again.",
    storage: "The server could not save this change. Nothing was changed.",
    busy: "Too many requests. Wait a few seconds and try again.",
    timeout: "The server did not answer. Refresh the list before retrying.",
    disconnected: "The connection closed. Reconnect to check your crew.",
  };
  let crewConfirmReturn = null;
  const expandedCrewMembers = new Set();
  let crewBusy = false, crewGeneration = 0, crewSignature = "", crewRenderSignature = "", crewIdentity = "", crewConfirmation = null;
  const crewLoads = new Set();
  const crewPages = { roster: { rows: [], total: 0, offset: 0 }, inbox: { rows: [], total: 0, offset: 0 }, outbox: { rows: [], total: 0, offset: 0 } };
  function crewSay(text, bad = false) {
    crewNote.textContent = text; crewNote.hidden = !text; crewNote.classList.toggle("bad", bad);
  }
  function crewButton(text, action, key) {
    const b = document.createElement("button"); b.type = "button"; b.className = "cm-item";
    b.textContent = text; if (key) b.dataset.crewAction = key; b.disabled = crewBusy || !window.recompOnlineState?.open;
    b.addEventListener("click", action); return b;
  }
  function crewConfirm(text, op, fields = {}) {
    crewConfirmReturn=document.activeElement;
    const snapshot = crewState().crew;
    crewConfirmation = () => crewAction(op, { crew: snapshot?.id, revision: snapshot?.revision, ...fields });
    $(".cm-crew-confirm-yes").textContent = ({disband:"Disband crew",leave:"Leave crew",transfer:"Make leader",kick:"Remove member"})[op] || "Confirm";
    $(".cm-crew-confirm p").textContent = text;
    $(".cm-crew-confirm").hidden = false; $(".cm-crew-confirm-no").focus();
  }
  function clearCrewConfirm() {
    crewConfirmation = null; $(".cm-crew-confirm").hidden = true;
  }
  $(".cm-crew-confirm-no").addEventListener("click", () => {clearCrewConfirm();if(crewConfirmReturn?.isConnected&&crewConfirmReturn.getClientRects().length)crewConfirmReturn.focus();else root.querySelector('[data-online-tab="crew"]').focus();});
  $(".cm-crew-confirm-yes").addEventListener("click", () => { const act = crewConfirmation; clearCrewConfirm(); act?.(); });
  async function crewAction(op, fields = {}) {
    if (crewBusy) return;
    const c = crewState().crew;
    crewBusy = true; crewSay("Saving…"); showCrew();
    try {
      await window.recompCrewRequest(op, { crew: c?.id, revision: c?.revision, ...fields });
      crewSay(op === "invite" ? "Invitation sent. Your friend can accept in Online → Invitations." : "Saved.");
      crewForm.hidden = true;
      await loadCrewPages(true);
      if (op === "accept") chooseOnlineTab("crew");
    } catch (e) {
      crewSay(crewErrors[e.message] || "The request failed. Refresh the list and try again.", true);
      if (e.message === "stale") await loadCrewPages(true);
    } finally {
      crewBusy = false; showCrew();
      const active = document.activeElement;
      if (!active || active === document.body || active === root || active.closest("[hidden]")) {
        (op === "invite" && !$(".cm-crew-invite-form").hidden ? $(".cm-crew-friend") : root.querySelector('[data-online-tab="crew"]')).focus();
      }
    }
  }
  async function loadCrewPage(kind, direction = false, generation = crewGeneration) {
    if (!crewState().supported || !crewState().account || !window.recompOnlineState?.open) return;
    const offset = Math.max(0, crewPages[kind].offset + (direction === "next" ? 50 : direction === "prev" ? -50 : 0));
    const load = `${kind}-${generation}-${offset}`;
    if (crewLoads.has(load)) return;
    crewLoads.add(load);
    if(kind==="roster")showCrew();
    try {
      const r = await window.recompCrewRequest(kind, { offset });
      if (generation !== crewGeneration) return;
      const p = r.page;
      crewPages[kind] = { rows: p.rows, total: p.total, offset: p.offset };
      if (p.offset >= p.total && p.offset > 0) {
        crewPages[kind].offset = Math.max(0, Math.floor((p.total - 1) / 50) * 50);
        void loadCrewPage(kind, false, generation);
      }
      showCrew();
    } catch (e) {
      if (generation === crewGeneration && !["no-crew", "disconnected"].includes(e.message)) crewSay(crewErrors[e.message] || "Could not load the list.", true);
    } finally {crewLoads.delete(load);showCrew();}
  }
  async function loadCrewPages(reset = false) {
    const generation = ++crewGeneration;
    if (reset) for (const kind of Object.keys(crewPages)) crewPages[kind] = { rows: [], total: 0, offset: 0 };
    if (!crewState().supported || !crewState().account) { showCrew(); return; }
    // Read replies also include the current server summary. Read the inbox first on entry.
    await loadCrewPage("inbox", false, generation);
    if (generation !== crewGeneration) return;
    if (crewState().crew) {
      await loadCrewPage("roster", false, generation);
      if (crewState().crew?.leader === crewState().account) await loadCrewPage("outbox", false, generation);
    }
  }
  function renderCrewInvites(kind, selector) {
    const list = $(selector), page = crewPages[kind], focused = document.activeElement?.dataset.crewAction;
    list.replaceChildren();
    const h = document.createElement("h3"); h.textContent = kind === "inbox" ? "Crew invitations" : "Sent invitations"; list.append(h);
    if (!page.rows.length) { const p = document.createElement("p");p.className = "cm-note";p.textContent = "No pending crew invitations.";list.append(p); }
    for (const inv of page.rows) {
      const item = document.createElement("div"), title = document.createElement("p"), actions = document.createElement("div");
      item.className = "cm-online-invite"; actions.className = "cm-row";
      title.textContent = kind === "inbox" ? `Join ${inv.crew}` : `${inv.name} · ${inv.crew}`;
      if (kind === "inbox") {
        const accept = crewButton("Accept invitation", () => crewAction("accept", { invite: inv.id }), `accept-${inv.id}`);
        accept.classList.add("cm-primary");accept.disabled ||= !!crewState().crew;
        actions.append(accept, crewButton("Decline", () => crewAction("decline", { invite: inv.id }), `decline-${inv.id}`));
      } else actions.append(crewButton("Revoke", () => crewAction("revoke", { invite: inv.id }), `revoke-${inv.id}`));
      const note = document.createElement("p");note.className = "cm-note";
      note.textContent = kind === "inbox" && crewState().crew ? "Leave your current crew before accepting." : `Expires ${new Date(inv.expires * 1000).toLocaleDateString()}`;
      item.append(title, actions, note);list.append(item);
    }
    if (page.offset > 0) list.append(crewButton("Previous invitations", () => loadCrewPage(kind, "prev"), `prev-${kind}`));
    if (page.offset + page.rows.length < page.total) list.append(crewButton("Next invitations", () => loadCrewPage(kind, "next"), `more-${kind}`));
    if (focused) [...list.querySelectorAll("button")].find(b => b.dataset.crewAction === focused)?.focus({ preventScroll: true });
  }
  function showCrew() {
    const s = crewState(), c = s.crew, leader = c && c.leader === s.account;
    const rosterLoading=[...crewLoads].some(key=>key.startsWith(`roster-${crewGeneration}-`));
    const renderSignature = JSON.stringify([s,crewBusy,crewPages,!!window.recompOnlineState?.open,rosterLoading]);
    if (renderSignature === crewRenderSignature) return;
    crewRenderSignature = renderSignature;
    const connected=!!window.recompOnlineState?.open;
    if(c&&!leader)crewForm.hidden=true;
    $(".cm-crew-title").textContent=c?.name || (!s.supported?"Crews unavailable":!s.account?"Protect your player name":"Start your crew");
    $(".cm-crew-role").textContent=c?`${leader?"Crew leader":"Crew member"} · ${c.count} ${c.count===1?"member":"members"}`:"Invitation-only membership";
    const edit=$(".cm-crew-edit");edit.textContent=c?"Rename crew":"Create crew";edit.hidden=!s.supported||!s.account||!!c&&!leader;
    edit.classList.toggle("cm-primary",!c);edit.disabled=crewBusy||!connected;
    $(".cm-crew-ok").disabled=crewBusy||!connected;$(".cm-crew-ok span").textContent=c?"Save name":"Create crew";
    $(".cm-crew-invite").disabled=crewBusy||!connected;$(".cm-crew-invite-form").hidden=!leader;
    $(".cm-crew-summary").textContent=!connected?"You are offline. Reconnect before changing your crew.":!s.supported?"This server does not support crew management. You can still browse online players.":!s.account?"Protect your player name with your cloud keyphrase on the start page. You can then create a crew or accept an invitation.":c?leader?"Invite players below. Members join after accepting your invitation.":"Your crew leader manages invitations and membership. You can view all members here.":"Create a crew with a unique name, or ask a crew leader for an invitation. Find received invitations in the Invitations tab.";
    crewHelp.hidden=!!c;membersHeading.hidden=!c;crewManagement.hidden=!c;crewBody.hidden=!c;inviteShortcut.hidden=!leader;
    const controls = $(".cm-crew-controls"), focus = document.activeElement?.dataset.crewAction;
    controls.replaceChildren();
    if (c) controls.append(leader
      ? crewButton("Disband crew", () => crewConfirm(`Disband ${c.name}? All members will leave and pending invitations will be removed.`, "disband"), "disband")
      : crewButton("Leave crew", () => crewConfirm(`Leave ${c.name}? You will need another invitation to return.`, "leave"), "leave"));
    const list = $(".cm-online-members"), scroll = list.scrollTop;
    list.replaceChildren();
    for (const member of crewPages.roster.rows) {
      const row = document.createElement("div"), title = document.createElement("strong"), role = document.createElement("span"), status = document.createElement("span");
      row.className = "cm-online-player cm-crew-member";title.textContent = member.name;
      role.className = "crew";role.textContent = member.leader ? "Leader" : "Member";
      status.className = "activity";status.textContent = member.online ? member.sameRoom ? "In this room" : "Online · another room" : "Offline";
      const identity=document.createElement("span");identity.className="cm-member-name";identity.append(title,role);row.append(identity,status);
      if (leader && member.id !== s.account) {
        const actions=document.createElement("div");actions.className="cm-row cm-crew-member-actions";actions.id=`cm-member-actions-${list.children.length}`;actions.hidden=!expandedCrewMembers.has(member.id);
        const manage=crewButton("Manage",()=>{actions.hidden=!actions.hidden;if(actions.hidden)expandedCrewMembers.delete(member.id);else expandedCrewMembers.add(member.id);manage.setAttribute("aria-expanded",String(!actions.hidden));},`manage-${member.id}`);
        manage.className="cm-member-manage";manage.setAttribute("aria-label",`Manage ${member.name}`);manage.setAttribute("aria-expanded",String(!actions.hidden));manage.setAttribute("aria-controls",actions.id);
        const transfer=crewButton("Make leader",()=>crewConfirm(`Make ${member.name} the leader of ${c.name}? You will become a member.`,"transfer",{member:member.id}),`transfer-${member.id}`);
        const remove=crewButton("Remove member",()=>crewConfirm(`Remove ${member.name} from ${c.name}? They will need a new invitation to return.`,"kick",{member:member.id}),`kick-${member.id}`);remove.classList.add("cm-danger");
        actions.append(transfer,remove);row.append(manage,actions);
      }
      list.append(row);
    }
    list.scrollTop = scroll;
    rosterNote.textContent=rosterLoading?(crewPages.roster.rows.length?"Refreshing members…":"Loading members…"):crewPages.roster.rows.length?`Showing ${crewPages.roster.offset+1}–${crewPages.roster.offset+crewPages.roster.rows.length} of ${crewPages.roster.total} members`:"Members have not loaded. Refresh members to try again.";
    refreshRoster.disabled=crewBusy||!connected||rosterLoading;
    const more = $(".cm-online-crew-more");more.hidden = !c || crewPages.roster.offset + crewPages.roster.rows.length >= crewPages.roster.total;more.disabled = crewBusy;
    $(".cm-online-crew-prev").hidden = !c || crewPages.roster.offset === 0;$(".cm-online-crew-prev").disabled = crewBusy;
    $(".cm-crew-outbox").hidden = !leader;
    renderCrewInvites("inbox", ".cm-crew-inbox");if (leader) renderCrewInvites("outbox", ".cm-crew-outbox");
    const pending=(s.pending||0)+(window.recompRace?.state?.()?.invites?.length||0);
    const tab = root.querySelector('[data-online-tab="invites"]');tab.textContent = pending ? `Invitations (${pending})` : "Invitations";
    if (focus) [...root.querySelectorAll("[data-crew-action]")].find(b => b.dataset.crewAction === focus)?.focus({ preventScroll: true });
  }
  function editCrew() {
    crewIn.value = crewState().crew?.name || "";crewSay("");crewForm.hidden = false;crewIn.focus();crewIn.select();
  }
  function hideCrew() {crewForm.hidden = true;$(".cm-crew-edit").focus();}
  function resetCrew() {crewForm.hidden = true;clearCrewConfirm();crewSay("");}
  crewIn.addEventListener("input", () => {const v=crewIn.value,at=crewIn.selectionStart||0;crewIn.value=crewClean(v);crewIn.setSelectionRange(crewClean(v.slice(0,at)).length,crewClean(v.slice(0,at)).length);});
  const setCrew = () => crewAction(crewState().crew ? "rename" : "create", { name: crewIn.value.trim() });
  crewIn.addEventListener("keydown", e => {if(e.key === "Enter"){e.preventDefault();setCrew();}});
  $(".cm-crew-ok").addEventListener("click", setCrew);
  $(".cm-crew-edit").addEventListener("click", () => crewForm.hidden ? editCrew() : hideCrew());
  $(".cm-crew-invite").addEventListener("click", () => crewAction("invite", { name: $(".cm-crew-friend").value.trim() }));
  $(".cm-crew-friend").addEventListener("keydown", e => {if(e.key === "Enter"){e.preventDefault();$(".cm-crew-invite").click();}});
  $(".cm-online-crew-more").addEventListener("click", () => loadCrewPage("roster", "next"));
  $(".cm-online-crew-prev").addEventListener("click", () => loadCrewPage("roster", "prev"));
  addEventListener("recomp-crews", () => {
    const s = crewState(), signature = JSON.stringify([s.supported,s.account,s.crew,s.pending]);
    if (signature !== crewSignature) {
      crewSignature = signature;clearCrewConfirm();crewGeneration++;
      const identity = JSON.stringify([s.account,s.crew?.id]);
      if (identity !== crewIdentity) {
        crewIdentity = identity;expandedCrewMembers.clear();
        for (const kind of Object.keys(crewPages)) crewPages[kind] = { rows: [], total: 0, offset: 0 };
      }
      if (onlineOpen) void loadCrewPages();
    }
    showCrew();onlineSignature = "";if (onlineOpen) renderOnline();
  });
  setInterval(() => {if (onlineOpen && !shade.hidden && !crewBusy) void loadCrewPages();}, 15000);

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
      bindFeatureHelp(btn, id, t.label, "");
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
      if (group === "Cops and robbers" || group === "Career" && window.recompSite) continue;
      if (!groups.has(group)) groups.set(group, []);
      const btn = document.createElement("button"), label = document.createElement("span"), hint = document.createElement("span");
      btn.type = "button";
      btn.className = "cm-item cm-action";
      btn.dataset.id = id;
      label.textContent = text(a.label) || id;
      bindFeatureHelp(btn, id, a.label, a.hint);
      hint.className = "cm-hint";
      hint.textContent = text(a.hint);
      btn.append(label, hint);
      btn.addEventListener("click", () => {
        try { a.run(); } catch (e) { console.warn("[menu]", e); }
        if (a.keepOpen) {
          const restoreFocus = document.activeElement === btn;
          drawActions();refresh();
          if (restoreFocus) root.querySelector(`.cm-action[data-id="${CSS.escape(id)}"]`)?.focus();
        } else close();
      });
      groups.get(group).push(btn);
    }
    $(".cm-online-actions").replaceChildren(...(groups.get("Race together") || []));
    groups.delete("Race together");
    $(".cm-career-actions").replaceChildren(...(groups.get("Career") || []));groups.delete("Career");
    $(".cm-report-actions").replaceChildren(...(groups.get("Report a problem") || []));groups.delete("Report a problem");
    displayActions.replaceChildren(...(groups.get("Display") || []));groups.delete("Display");
    $(".cm-extra").replaceChildren(...[...groups].flatMap(([name, btns]) => {
      const h = document.createElement("h3");
      h.textContent = name;
      return [h, ...btns];
    }));
    updateRail();
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
        ["Camera", ["camera/enabled", "core.nfsc/freelook", "core.nfsc/freelook_invert", "camera/height", "camera/forward", "camera/left", "camera/pitch", "camera/light"]],
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
    "host.display/performance_overlay": "Off hides the performance panel. Counters shows numbers; Graph adds frame timing. F11 cycles the modes too, unless the browser intercepts that key. To hide the panel: F9 → Display & graphics → Screen → Performance overlay → Off.",
    "host.display/frame_limit": "Sets a maximum presented frame rate, not a guaranteed speed. The game simulation runs at 60 Hz. 120 FPS is experimental and may affect cinematics, vehicle poses, and animations. Start with 60 FPS.",
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
    id: "browser/fullscreen", label: "Full screen", group: "display", kind: "bool", min: 0, max: 1, step: 1, def: 0, choices: [], apply: "live",
    help: "The browser's full screen (also on the menu's first page). The start page can start the game in full screen.",
  };

  const setting = (id) => (id === FULLSCREEN.id ? { ...FULLSCREEN, value: document.fullscreenElement ? 1 : 0 } : window.carbonCockpitSettings?.get(id) || S.byId.get(id));
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
    const step = s.id.startsWith("camera/") ? s.step || 1 : Math.max(s.step || 1, span >= 50 && s.max <= 400 ? 5 : 1);
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
    if (id?.startsWith("camera/")) { window.carbonCockpitSettings?.set(id, value); return; }
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
    activateMenu(name);
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
    if (name === "display") { refreshGraphics(); box.append(graphicsCard, displayActions); }
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
  addEventListener("carbon-cockpit-settings", () => {
    if (shade.hidden || page !== "controls") return;
    if (pageKey(page) !== shownKey) showPage(page, false);
    else update();
    help();
  });
  function showMain() {
    const from = page;
    activateMenu("quick");
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
      const label = btn.querySelector(".cm-label");
      label.replaceChildren(document.createTextNode(labelOf(s)));
      const meta = document.createElement("span");meta.className="cm-set-meta";
      meta.textContent = waits ? "Restart pending" : TIMING[s.apply] || "";label.append(meta);
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
      $(".cm-guide-title").textContent = PAGES[page]?.title || "Settings";
      $(".cm-guide-how span").textContent = "Focus a setting to read its help. Changes use the game’s existing settings system.";
      title.textContent = "";
      body.textContent = PAGES[page]?.note ? "" : "Choose a setting to see what it does.";
      title.className = "";
      return;
    }
    $(".cm-guide-title").textContent = labelOf(s);
    $(".cm-guide-how span").textContent = s.apply === "restart"
      ? "Quit to the start page, then Play. Closing F9 alone does not restart the game."
      : numeric(s) ? "Use left/right or type a number. The setting stays within its supported limits."
      : "Use left/right to change the value. Enter selects the next value.";
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
  function items() { return [...shade.querySelectorAll("button, input, select")].filter((b) => !b.disabled && !b.closest("[hidden]") && b.getClientRects().length); }
  function open() {
    if (!shade.hidden) return;
    activateMenu("quick");
    if (document.pointerLockElement) document.exitPointerLock();
    drawToggles();
    drawActions();
    resetCrew();
    onlineOpen = false;
    onlinePage.hidden = true;
    $(".cm-panel").setAttribute("aria-label", "Menu");
    $(".cm-panel").classList.remove("cm-panel-online");
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
    onlineOpen = false;
    globalThis.recompMenuOpen = false;
    if (onlineReturn?.isConnected && onlineReturn.getClientRects().length) onlineReturn.focus();
    else canvas?.focus();
    onlineReturn = null;
  }
  const toggle = () => (shade.hidden ? open() : close());

  // Keys: F9 and F10 open and close; while open, presses go to the menu only.
  // The controller's chord reaches the kit, which sends "recomp-menu"; a press
  // of F10 the kit also saw (it should not: SDL listens later, in the bubble
  // phase) must not toggle twice.
  let keyAt = -1e9;
  const MENU_KEYS = new Set(["F9", "F10"]);
  addEventListener("keydown", (e) => {
    if (globalThis.carbonHudPickerOpen) return;
    if (e.code === "F8" && (window.recompOnline || window.recompOnlineState)) {
      e.preventDefault();e.stopImmediatePropagation();
      if (!e.repeat) { if (onlineOpen && !shade.hidden) close(); else openOnline(); }
      return;
    }
    if (MENU_KEYS.has(e.code)) {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (!e.repeat) { keyAt = performance.now(); toggle(); }
      return;
    }
    if (!shade.hidden && !root.contains(e.target)) {
      e.preventDefault();
      e.stopImmediatePropagation();
      (items()[0] || $(".cm-resume")).focus();
    }
  }, true);
  addEventListener("keypress", (e) => {
    if (globalThis.carbonHudPickerOpen) return;
    if (MENU_KEYS.has(e.code) || (!shade.hidden && !root.contains(e.target))) {
      e.preventDefault();
      e.stopImmediatePropagation();
    }
  }, true);
  addEventListener("recomp-menu", () => {
    if (globalThis.carbonHudPickerOpen) { globalThis.carbonCustomHud?.closePicker(); return; }
    if (performance.now() - keyAt > 400) toggle();
  });
  // Keys inside the menu stop here, before the game's listeners on the window.
  for (const type of ["keydown", "keypress"]) root.addEventListener(type, (e) => e.stopPropagation());
  // One step back: a confirmation, the crew field, the controller check or a settings
  // page; on the first page `closing` (Esc, the controller's ○ / B) closes the menu.
  function back(closing) {
    if (menuView === "search") {$(".cm-search").value="";showMain();$(".cm-back").focus();return;}
    if (menuView === "support" || menuView === "career") {showMain();$(".cm-back").focus();return;}
    if (onlineOpen) {
      if (!$(".cm-crew-confirm").hidden) {clearCrewConfirm();root.querySelector('[data-online-tab="crew"]').focus();}
      else if (!crewForm.hidden) hideCrew();
      else close();
    }
    else if (checking) leaveCheck();
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
    if (e.target instanceof HTMLSelectElement && e.key !== "Escape" && e.key !== "Tab") return;
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
    const dir = Number.isFinite(gp.axes[9]) ? Math.round((gp.axes[9] + 1) * 3.5) : 8; // 0 up, 1 up-right ... 7 up-left, 8 centred
    Object.assign(s, { up: dir === 7 || dir === 0 || dir === 1, right: dir >= 1 && dir <= 3, down: dir >= 3 && dir <= 5,
      left: dir >= 5 && dir <= 7, ry: axis(5), l2v: Number.isFinite(gp.axes[3]) ? (gp.axes[3] + 1) / 2 : value(6), r2v: Number.isFinite(gp.axes[4]) ? (gp.axes[4] + 1) / 2 : value(7) });
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
      if (!shade.contains(document.activeElement)) (items()[0] || $(".cm-resume")).focus();
      else if (onlineOpen && document.activeElement instanceof HTMLSelectElement && (dir === "left" || dir === "right")) {
        const select = document.activeElement;
        select.selectedIndex = (select.selectedIndex + (dir === "right" ? 1 : select.options.length - 1)) % select.options.length;
        select.dispatchEvent(new Event("change", {bubbles:true}));
      }
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
      else if (!shade.contains(el)) (items()[0] || $(".cm-resume")).focus();
    } else if (press("east")) {
      padUsed();
      back(true);
    }
  }

  // Use the adapter captured by the player for its actual device. A new probe
  // could choose a different adapter and would not identify the running game.
  const graphicsCard = document.createElement("section");
  graphicsCard.className = "cm-gpu";
  graphicsCard.innerHTML = `
    <button type="button" class="cm-item cm-gpu-toggle" aria-expanded="false" aria-controls="cm-gpu-details">
      <span><span class="cm-gpu-label">Graphics card in use</span><span class="cm-gpu-name"></span></span>
      <span class="cm-gpu-symbol" aria-hidden="true">+</span>
    </button>
    <div class="cm-guide-answer cm-gpu-details" id="cm-gpu-details" hidden>
      <p class="cm-gpu-status"></p>
      <div class="cm-gpu-windows" hidden>
        <p><strong>Choose a GPU on Windows</strong></p>
        <p>If your PC has another graphics card, Windows can assign it to your browser.</p>
        <ol>
          <li>Open Windows Settings → System → Display → Graphics.</li>
          <li>Select the browser you use for the game, such as Chrome or Edge. Add the browser app if it is missing.</li>
          <li>Open Options, choose High performance, then Save. Check the GPU name shown by Windows.</li>
          <li>Finish your game session, then fully close and reopen the browser.</li>
          <li>Start the game and check Graphics card in use again.</li>
        </ol>
        <p>The game requests high performance. Windows and the browser choose the GPU; this menu cannot switch it directly.</p>
      </div>
    </div>`;
  const graphicsToggle = graphicsCard.querySelector(".cm-gpu-toggle");
  const graphicsDetails = graphicsCard.querySelector(".cm-gpu-details");
  function refreshGraphics() {
    const info = window.carbonGraphicsInfo;
    const active = info?.source === "active device adapter";
    const field = (key) => active && typeof info[key] === "string" ? info[key].trim() : "";
    const vendor = field("vendor"), architecture = field("architecture"), description = field("description");
    const brands = { amd: "AMD", nvidia: "NVIDIA", intel: "Intel", apple: "Apple", qualcomm: "Qualcomm", arm: "ARM" };
    const family = [brands[vendor.toLowerCase()] || vendor, architecture].filter(Boolean).join(" · ");
    const software = active && (info.fallback === true || /swiftshader/i.test([vendor, architecture, description, field("device")].join(" ")));
    graphicsCard.querySelector(".cm-gpu-name").textContent = software ? "Software renderer" : description || family || (active ? "Name unavailable" : "Not available yet");
    graphicsCard.querySelector(".cm-gpu-status").textContent = !active
      ? "The game has not provided graphics details for this session. Open this page again after the game starts."
      : software ? "The browser selected software rendering. Enable graphics acceleration in your browser settings, then restart the browser."
      : description ? "Reported by the graphics adapter selected for this game."
      : family ? "This is the GPU manufacturer or family reported for this game. The browser did not share a model name."
      : "The browser did not share a graphics card name. This does not mean graphics acceleration is off.";
    graphicsCard.querySelector(".cm-gpu-windows").hidden = !/win/i.test(navigator.userAgentData?.platform || navigator.platform || navigator.userAgent);
  }
  function explainGraphics() {
    guide("Graphics card in use", "Shows the graphics adapter selected for this game. The browser may share only its manufacturer or family.", "Open the row for details. On Windows, it also explains how to choose a GPU for your browser.");
  }
  graphicsToggle.addEventListener("click", () => {
    refreshGraphics();
    graphicsDetails.hidden = !graphicsDetails.hidden;
    graphicsToggle.setAttribute("aria-expanded", String(!graphicsDetails.hidden));
    graphicsCard.querySelector(".cm-gpu-symbol").textContent = graphicsDetails.hidden ? "+" : "−";
    explainGraphics();
  });
  graphicsToggle.addEventListener("focus", explainGraphics);
  graphicsToggle.addEventListener("mouseenter", explainGraphics);

  // Controls → Controller check: what the browser and the game see of the controller.
  const checkGo = document.createElement("button");
  checkGo.type = "button";
  checkGo.className = "cm-item cm-check-go";
  checkGo.setAttribute("aria-label", "Controller check");
  checkGo.innerHTML = `<span>Controller check</span>`;
  checkGo.addEventListener("click", showCheck);
  $(".cm-check-up").addEventListener("click", leaveCheck);
  function showCheck() {
    activateMenu("controls");
    guide("Controller check", "Check the name and inputs reported by the browser and the game.", "Connect your controller and press a button. A generic name alone does not mean input is broken.");
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
      else await document.documentElement.requestFullscreen({ keyboardLock: "browser" });
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

  const MENU_GUIDES = [
    [
      "Hide the performance overlay",
      [
        "Open F9 → Display & graphics → Screen in the current game menu.",
        "Set Performance overlay to Off.",
        "Close F9 to return to the game."
      ],
      "F11 also cycles Off, Counters, and Graph in the game. A browser can intercept F11. On a Mac, try Fn with a function key."
    ],
    [
      "Improve slow gameplay",
      [
        "Choose Render resolution → Game resolution.",
        "Quit to the start page, then press Play to apply the restart setting.",
        "Choose a lower size in the game’s Options → Video.",
        "Compare the same track and camera with the performance overlay visible."
      ],
      "If the lower resolution does not help, report that result. A higher frame limit does not guarantee a higher frame rate."
    ],
    [
      "Controller name is missing or wrong",
      [
        "Open Controls → Controller check.",
        "Check the browser name and whether buttons and sticks respond.",
        "If the name is wrong, report the model, USB or Bluetooth connection, browser, and name shown."
      ],
      "A generic device name does not by itself show whether controller input works. Button names is a separate setting for labels in Options → Controls."
    ],
    [
      "Controller works but keyboard prompts remain",
      [
        "Check Button names in Controls.",
        "Use Automatic or choose your controller family.",
        "Check the labels in the game’s Options → Controls."
      ],
      "The browser version lacks the menu button-icon assets. Some game menus can still show keyboard prompts even when controller input works."
    ],
    [
      "Controller buttons do not respond",
      [
        "Connect the controller and press a button.",
        "Open Controller check.",
        "Check whether the browser sees button and stick movement.",
        "Report whether the device is missing, detected without movement, or detected but ignored by the game."
      ],
      "Driving bindings are changed in the game’s Options → Controls."
    ],
    [
      "Black screen, flashes, or a freeze",
      [
        "Note whether the problem occurs while loading, in the garage, or while driving.",
        "Record your browser, device, and graphics settings.",
        "Note whether sound continues and whether the game recovers.",
        "Open Help & support → Send a report."
      ],
      "Include repeatable steps and the performance overlay if possible. Use Download log if you need a separate diagnostics file."
    ],
    [
      "Create a crew or accept an invitation",
      [
        "Protect your player name on the start page with your cloud keyphrase.",
        "Open Online → My crew to create a crew, or Online → Invitations to accept one.",
        "Ask the leader for an invitation by protected player name."
      ],
      "Copying a crew name does not grant membership. The server must support protected crews. Invitations expire after seven days."
    ],
    [
      "Why is an action missing?",
      [
        "Check that the feature supports your current game state.",
        "For races, check the current lobby and invitations.",
        "For career tools, use a local session and open the game menus or Safehouse."
      ],
      "Unavailable actions stay hidden. Race actions depend on the lobby and invitations. Career tools are local-only."
    ],
    [
      "Apply changes that need a restart",
      [
        "Change the setting.",
        "Use Quit to the start page. Your career stays as the game last saved it.",
        "Press Play again."
      ],
      "Closing F9 alone does not restart the game. Live settings apply immediately in the game."
    ],
    [
      "Save or undo a recovered career car",
      [
        "Use the eligible recovery action in the menus or Safehouse.",
        "With autosave enabled, the game saves when you next leave the Safehouse. Otherwise save through the game menu.",
        "To undo, use start page → Saves → Undo the last restore."
      ],
      "Recovery requires a successful backup and a free garage slot. Backups are managed on the start page."
    ]
  ];

  // ---- Wider F9 workspace (2026-11-02) ----
  // The settings block and script registrations remain authoritative. No sample values.
  let menuView = "quick";
  const menuPanel = $(".cm-panel"), work = document.createElement("div"), rail = document.createElement("div");
  const header = document.createElement("div"), frame = document.createElement("div"), inspector = document.createElement("aside"), bottom = document.createElement("div");
  header.className = "cm-header";
  header.innerHTML = `<div><p class="cm-brand">NEED FOR SPEED · CARBON</p><h2 class="cm-title">GAME MENU</h2></div><button class="cm-resume" type="button">Resume <span>ESC ↗</span></button>`;
  frame.className = "cm-frame";rail.className = "cm-rail";rail.setAttribute("aria-label", "Menu categories");work.className = "cm-work";
  inspector.className = "cm-inspector";inspector.setAttribute("aria-label", "Feature guide");
  inspector.innerHTML = `<div class="cm-inspector-label">FEATURE GUIDE</div><div class="cm-inspector-icon" aria-hidden="true">i</div><p class="cm-guide-title"></p>`;
  inspector.append($(".cm-help"));
  const how = document.createElement("p");how.className = "cm-guide-how";how.innerHTML = `<strong>HOW TO USE</strong><span></span>`;inspector.append(how);
  bottom.className = "cm-bottom";bottom.innerHTML = `<span><kbd>↑ ↓</kbd>Navigate &nbsp; <kbd>← →</kbd>Change &nbsp; <kbd>Enter</kbd>Select</span><span><kbd>Esc</kbd>Back &nbsp; <kbd>F9</kbd>Close menu</span>`;
  const searchBar = document.createElement("div");searchBar.className = "cm-searchbar";
  searchBar.innerHTML = `<label class="cm-search-label" for="cm-feature-search">Find a setting or get help</label><input class="cm-search" id="cm-feature-search" type="search" placeholder="Try overlay, controller, graphics, or crew…" autocomplete="off">`;
  const careerPage = document.createElement("section"), supportPage = document.createElement("section"), resultsPage = document.createElement("section"), displayActions = document.createElement("div");
  careerPage.className = "cm-career";supportPage.className = "cm-support";resultsPage.className = "cm-search-results";displayActions.className = "cm-display-actions";
  careerPage.hidden = supportPage.hidden = resultsPage.hidden = true;
  careerPage.innerHTML = `<h2>Career tools · Local only</h2><p class="cm-note">Available on this computer only. Save backups and eligibility checks apply.</p><div class="cm-career-actions"></div>`;
  supportPage.innerHTML = `<h2>Help & support</h2><p class="cm-note">Choose a topic for steps, or send a report with the game log.</p><div class="cm-report-actions"></div><h3>Step-by-step help</h3><div class="cm-guides"></div>`;
  work.append(searchBar, mainPage, setPage, checkPage, onlinePage, careerPage, supportPage, resultsPage);
  frame.append(rail, work, inspector);menuPanel.append(header, frame, bottom);
  $(".cm-main h2").textContent = "Quick actions";
  // These are the existing settings buttons, with their original event handlers.
  const settingsHeading = [...mainPage.querySelectorAll("h3")].find(el => el.textContent === "Settings");
  if (settingsHeading) settingsHeading.hidden = true;
  const caption = document.createElement("p");caption.className = "cm-rail-caption";caption.textContent = "YOUR GAME";rail.append(caption);
  const railButtons = new Map();
  function navButton(view, label, existing) {
    const b = existing || document.createElement("button");b.type = "button";
    b.classList.add("cm-nav-item");b.dataset.view = view;
    if (!existing) {b.textContent = label;b.addEventListener("click", () => routeMenu(view));}
    rail.append(b);railButtons.set(view, b);return b;
  }
  navButton("quick", "Quick actions");
  for (const name of ["display", "controls", "game"]) navButton(name, PAGES[name].title, root.querySelector(`.cm-go[data-page="${name}"]`));
  navButton("online", "Online & crews", $(".cm-online-go"));
  $(".cm-online-go span").textContent = "Online & crews";
  navButton("race", "Race together");navButton("career", "Career tools · Local only");navButton("support", "Help & support");
  $(".cm-resume").addEventListener("click", close);
  for (const [selector,title,body,instructions] of [
    [".cm-back","Back to the game","Closes the F9 browser menu.","F9 and F10 also close this menu. Esc steps back through pages first."],
    [".cm-full","Full screen","Uses the whole display for the game.","Change this here or on Display & graphics. The browser can intercept full-screen shortcuts."],
    [".cm-quit","Quit to the start page","Closes the running game. Your career stays as the game last saved it.","Confirm Quit to leave. For restart settings, press Play again on the start page."],
  ]) {const b=$(selector);for(const event of ["focus","mouseenter"])b.addEventListener(event,()=>guide(title,body,instructions));}
  // Online is a people workspace. Keep the generic settings guide out of this view.
  const onlineHeading = document.createElement("div");onlineHeading.className = "cm-online-heading";
  const onlineTitle = document.createElement("div");onlineTitle.innerHTML = `<h2>Online & crews</h2><p>Players in your session, crew members, and invitations.</p>`;
  onlinePage.insertBefore(onlineHeading, $(".cm-who"));onlineHeading.append(onlineTitle, $(".cm-who"));
  const playerBrowser = document.createElement("div");playerBrowser.className = "cm-player-browser";
  for (const selector of [".cm-online-tools", ".cm-online-count", ".cm-online-players .cm-online-list", ".cm-online-more"]) playerBrowser.append($(selector));
  $(".cm-online-players").prepend(playerBrowser);
  const crewHeading = document.createElement("div");crewHeading.className = "cm-crew-heading";
  const crewHeadingIdentity = document.createElement("div");crewHeadingIdentity.innerHTML = `<h3 class="cm-crew-title">My crew</h3><p class="cm-crew-role"></p>`;
  $(".cm-online-crew").prepend(crewHeading);crewHeading.append(crewHeadingIdentity, $(".cm-crew-edit"));
  const crewNameLabel=document.createElement("label");crewNameLabel.className="cm-crew-label";crewNameLabel.htmlFor="cm-crew-name";crewNameLabel.textContent="Crew name";
  crewIn.id="cm-crew-name";crewIn.placeholder="Night Shift";crewForm.insertBefore(crewNameLabel,crewForm.querySelector(".cm-row"));
  const cancelCrew=document.createElement("button");cancelCrew.type="button";cancelCrew.className="cm-item cm-crew-cancel";cancelCrew.textContent="Cancel";cancelCrew.addEventListener("click",hideCrew);crewForm.querySelector(".cm-row").append(cancelCrew);
  $(".cm-crew-ok").classList.add("cm-primary");$(".cm-crew-invite").classList.add("cm-primary");
  $(".cm-crew-friend").placeholder="Alex";
  $(".cm-crew-invite-form .cm-crew-label").firstChild.textContent="Invite a player by name";
  $(".cm-crew-invite-form .cm-note").textContent="Use their protected player name. Invitations expire after seven days.";
  const crewHelp=document.createElement("button");crewHelp.type="button";crewHelp.className="cm-item cm-crew-help";crewHelp.textContent="How crews work";
  crewHelp.addEventListener("click",()=>{routeMenu("support");const topic=guideButtons.find(g=>g.title==="Create a crew or accept an invitation");if(topic){if(topic.answer.hidden)topic.button.click();topic.button.focus();}});
  $(".cm-crew-summary").after(crewHelp);
  const membersHeading=document.createElement("h3");membersHeading.className="cm-member-heading";membersHeading.textContent="Members";$(".cm-online-members").before(membersHeading);
  const crewManagement=document.createElement("div");crewManagement.className="cm-crew-management";
  const crewSettings=document.createElement("button");crewSettings.type="button";crewSettings.className="cm-item cm-crew-settings";crewSettings.textContent="Crew settings";crewSettings.setAttribute("aria-expanded","false");crewSettings.setAttribute("aria-controls","cm-crew-controls");
  $(".cm-crew-controls").id="cm-crew-controls";$(".cm-crew-controls").hidden=true;
  crewSettings.addEventListener("click",()=>{const box=$(".cm-crew-controls");box.hidden=!box.hidden;crewSettings.setAttribute("aria-expanded",String(!box.hidden));});
  crewManagement.append(crewSettings,$(".cm-crew-controls"));
  const crewBody=document.createElement("div"),crewRoster=document.createElement("section"),crewTools=document.createElement("aside"),crewPaging=document.createElement("div");
  crewBody.className="cm-crew-body";crewRoster.className="cm-crew-roster";crewTools.className="cm-crew-tools";crewTools.setAttribute("aria-label","Crew invitations and settings");crewPaging.className="cm-row";
  const rosterNote=document.createElement("p");rosterNote.className="cm-roster-note";rosterNote.setAttribute("role","status");
  const refreshRoster=document.createElement("button");refreshRoster.type="button";refreshRoster.className="cm-item cm-roster-refresh";refreshRoster.textContent="Refresh members";refreshRoster.addEventListener("click",()=>loadCrewPage("roster"));
  crewPaging.append($(".cm-online-crew-prev"),$(".cm-online-crew-more"));crewRoster.append(membersHeading,rosterNote,$(".cm-online-members"),crewPaging,refreshRoster);
  crewTools.append($(".cm-crew-invite-form"),$(".cm-crew-outbox"),crewManagement);crewBody.append(crewRoster,crewTools);$(".cm-online-crew").append(crewBody);
  const inviteShortcut=document.createElement("button");inviteShortcut.type="button";inviteShortcut.className="cm-item cm-primary cm-crew-invite-shortcut";inviteShortcut.textContent="Invite a player";inviteShortcut.addEventListener("click",()=>{$(".cm-crew-friend").focus();$(".cm-crew-invite-form").scrollIntoView({block:"nearest"});});$(".cm-crew-summary").after(inviteShortcut);

  const panels = [mainPage, setPage, checkPage, onlinePage, careerPage, supportPage, resultsPage];
  function guide(title, body, instructions = "", timing = "") {
    $(".cm-guide-title").textContent = title;
    $(".cm-help b").textContent = timing;$(".cm-help b").className = "";
    $(".cm-help-text").textContent = body;$(".cm-guide-how span").textContent = instructions;
    $(".cm-restart").hidden = true;$(".cm-keys").textContent = "";
  }
  function activateMenu(view) {
    menuView = view;
    panels.forEach(el => {el.hidden = true;});
    onlineOpen = false;checking = false;
    $(".cm-quit-box").hidden = $(".cm-reset-box").hidden = true;
    menuPanel.classList.remove("cm-panel-online");menuPanel.setAttribute("aria-label", "Game menu");
    if (view !== "search") $(".cm-search").value = "";
    for (const [name, b] of railButtons) {
      if (name === view) b.setAttribute("aria-current", "page");else b.removeAttribute("aria-current");
    }
    work.scrollTop = 0;
    if(matchMedia("(max-width:600px)").matches)railButtons.get(view)?.scrollIntoView({block:"nearest",inline:"nearest"});
    const descriptions = {
      quick: ["Quick actions", "Common switches and session actions.", "Choose a category on the left. F9 or F10 closes this menu. Esc steps back."],
      online: ["Online & crews", "See players, manage your protected crew, and respond to invitations.", "Crew actions use your protected player identity. Only the crew leader can manage members."],
      race: ["Race together", "Use the existing shared Quick Race controls and invitations.", "Enable Host a race together, then choose a Quick Race. Race controls appear here when available."],
      career: ["Career tools · Local only", "These tools use the existing save backup and eligibility checks.", "Use the game menus or Safehouse. Shop prices still apply. Purchased packages remain owned."],
      support: ["Help & support", "Find practical steps for graphics, controllers, online play, and saves.", "Open a topic below. Download log saves a text file; Send a report opens the report form."],
    };
    if (descriptions[view]) guide(...descriptions[view]);
  }
  function routeMenu(view) {
    if (["display", "controls", "game"].includes(view)) {showPage(view);return;}
    if (view === "quick") {showMain();$(".cm-back").focus();return;}
    if (view === "online" || view === "race") {
      openOnline();
      if (view === "race") {menuView = "race";for (const [name,b] of railButtons) {if(name === "race") b.setAttribute("aria-current","page");else b.removeAttribute("aria-current");}chooseOnlineTab("invites");guide("Race together", "Existing Quick Race controls and invitations.", "Enable hosting, choose a Quick Race, and press Start. Invitations appear when another player hosts a race.");}
      return;
    }
    if (view === "career" && window.recompSite) return;
    activateMenu(view);page = null;rowEls.clear();
    const panel = view === "career" ? careerPage : supportPage;panel.hidden = false;
    (panel.querySelector("button") || railButtons.get(view))?.focus();
  }
  function updateRail() {
    railButtons.get("career").hidden = !!window.recompSite || !$(".cm-career-actions").querySelector("button");
    const hasOnline = !!window.recompOnline || !!window.recompOnlineState;
    railButtons.get("online").hidden = !hasOnline;railButtons.get("race").hidden = !hasOnline;
    if (menuView === "career" && railButtons.get("career").hidden) routeMenu("quick");
  }
  const featureHelp = {
    radar: ["Shows nearby online players on the radar.", "Turn Off to hide the radar. Your choice is saved in this browser."],
    chat: ["Shows room chat during driving, the garage, and the main menu.", "Press Y to open chat. Turn Off here to hide the chat interface."],
    "world-hud": ["Enables the custom gauges.", "Turn Off to restore the original Carbon gauges. Choose HUD style under Display & graphics to change the design."],
    "hud-style": ["Choose and preview a gauge design in the existing HUD picker.", "Choose a style, then apply it. Original Carbon restores the original gauges."],
    "race-host": ["Host a shared Quick Race using the existing race system.", "Enable hosting, choose a Quick Race, and press Start. Available when no shared race or lobby is active."],
    "race-start": ["Starts the current race lobby.", "Check the player count before starting."],
    "race-cancel": ["Cancels the current race lobby.", "Players return from the pending lobby. This does not start a free-roam race."],
    "race-join": ["Join or leave the selected player's race invitation.", "Available when an invitation exists and no other race or lobby is active."],
    "report-log": ["Downloads the game log as a text file.", "Use this when asked to share diagnostics. A log download alone does not send a report."],
    "report-send": ["Opens the existing report form with the game log.", "Describe the steps, what happened, and what you expected. Submit the form to send the report."],
    "performance-unlock": ["Makes performance packages available in the shop. Prices still apply.", "Local only. Use menus or Safehouse. A successful save backup is required before enabling. Reopen the shop to refresh."],
    "visual-unlock": ["Unlocks supported paints, visuals, and aftermarket parts. Prices still apply.", "Local only. A successful save backup is required before enabling. Reopen the shop to refresh."],
    "bmw-boost": ["Adds engine torque, taller gearing, and extra grip to the BMW M3 GTR.", "Local only. A save backup is required. Resets when the game restarts. Turning Off restores normal driving settings."],
  };
  function explainFeature(id, title, hint) {
    const entry = featureHelp[id];
    guide(title, entry?.[0] || hint || "Use this action from the current game session.", entry?.[1] || (id.startsWith("boss-car-") ? "Local only. Use the menus or Safehouse. A save backup and a free garage slot are required. Already awarded cars cannot be added again." : "Availability depends on the current game state."));
  }
  function bindFeatureHelp(btn, id, label, hint) {
    const show = () => explainFeature(id, text(label) || id, text(hint));
    btn.addEventListener("focus", show);btn.addEventListener("mouseenter", show);
  }
  const guideButtons = [];
  function addGuide(title, steps, note) {
    const button = document.createElement("button"), answer = document.createElement("div");
    button.type = "button";button.className = "cm-item cm-guide-button";
    const label = document.createElement("span"), symbol = document.createElement("span");label.textContent = title;symbol.textContent = "+";symbol.setAttribute("aria-hidden", "true");button.append(label,symbol);
    answer.className = "cm-guide-answer";answer.hidden = true;answer.id = `cm-guide-${guideButtons.length}`;
    button.setAttribute("aria-expanded", "false");button.setAttribute("aria-controls", answer.id);
    const list = document.createElement("ol");
    for (const step of steps) {const li=document.createElement("li");li.textContent=step;list.append(li);}
    const p=document.createElement("p");p.textContent=note;answer.append(list,p);
    button.addEventListener("click", () => {answer.hidden = !answer.hidden;button.setAttribute("aria-expanded", String(!answer.hidden));symbol.textContent = answer.hidden ? "+" : "−";});
    const show=()=>guide(title,note,steps.join(" "));button.addEventListener("focus",show);button.addEventListener("mouseenter",show);
    $(".cm-guides").append(button,answer);guideButtons.push({title,steps,note,button,answer});
  }
  for (const entry of MENU_GUIDES) addGuide(...entry);
  function searchMenu() {
    const query = $(".cm-search").value.trim().toLocaleLowerCase();
    if (!query) {showMain();$(".cm-search").focus();return;}
    activateMenu("search");page=null;rowEls.clear();resultsPage.hidden=false;
    resultsPage.replaceChildren();const heading=document.createElement("h2");heading.textContent="Search results";resultsPage.append(heading);
    let count=0;
    function result(title,category,openResult) {
      count++;const b=document.createElement("button");b.type="button";b.className="cm-item cm-help-result";
      const label=document.createElement("span"),small=document.createElement("small");label.textContent=title;small.textContent=category;b.append(label,small);b.addEventListener("click",openResult);resultsPage.append(b);
    }
    const matches=(...parts)=>parts.join(" ").toLocaleLowerCase().includes(query);
    const settings=[setting(FULLSCREEN.id),...S.list,...(window.carbonCockpitSettings?.list() || [])];
    for (const s of settings) {
      if (HIDDEN(s) || SHOWN_IF[s.id] && !SHOWN_IF[s.id]()) continue;
      if(matches(s.label,s.help,WEB_NOTE[s.id])) result(labelOf(s),PAGES[pageOf(s)]?.title || "Display & graphics",()=>{showPage(s.id===FULLSCREEN.id?"display":pageOf(s));rowEls.get(s.id)?.focus();});
    }
    for(const btn of new Set([...root.querySelectorAll(".cm-toggle,.cm-action"), ...displayActions.querySelectorAll(".cm-action")])) {
      const owner=btn.closest(".cm-career-actions")?"career":btn.closest(".cm-online-actions")?"race":btn.closest(".cm-report-actions")?"support":btn.closest(".cm-display-actions")?"display":"quick";
      if(owner==="career"&&window.recompSite)continue;
      if(matches(btn.textContent,...(featureHelp[btn.dataset.id]||[])))result(btn.querySelector("span")?.textContent||btn.textContent,owner==="career"?"Career tools · Local only":owner==="race"?"Race together":"Menu action",()=>{routeMenu(owner);btn.focus();});
    }
    if(matches("graphics card gpu adapter video nvidia amd intel windows high performance"))result("Graphics card in use","Display & graphics",()=>{showPage("display");graphicsToggle.focus();});
    if(matches("controller check device name input sticks rumble"))result("Controller check","Controls",showCheck);
    for(const topic of guideButtons)if(matches(topic.title,...topic.steps,topic.note))result(topic.title,"Help & support",()=>{routeMenu("support");topic.answer.hidden=false;topic.button.setAttribute("aria-expanded","true");topic.button.lastElementChild.textContent="−";topic.button.focus();topic.button.scrollIntoView({block:"nearest"});});
    const p=document.createElement("p");p.className="cm-note";p.textContent=count?`${count} ${count===1?"result":"results"}. Choose one to open its page.`:"No matches. Try overlay, controller, graphics, or crew.";heading.after(p);
    guide("Search", "Find settings, currently available actions, and help topics.", "Choose a result to open its page. Local career tools are excluded on the public site.");
  }
  $(".cm-search").addEventListener("input",searchMenu);
  activateMenu("quick");mainPage.hidden=false;guide("Quick actions","Common switches and session actions.","Choose a category on the left. F9 closes the menu.");

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
