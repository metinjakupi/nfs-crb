// Mobile controls use Carbon's standard keyboard bindings through SDL's existing input path.
//
// Two layouts follow the game's own "driving a car" state (MpShared +0x10, as chat.js reads it;
// mp.h writes 0 while paused): a D-pad with OK and Back in the game's menus and pause screen,
// steering and pedals in the car. Until the block exists the game is booting: the menu layout.
//
// Steering is one of three modes, saved per browser: Thumb (drag anywhere on the left), Tilt
// (turn the phone like a wheel) and Buttons. The game only sees keys, so Thumb and Tilt give a
// proportional turn by holding the arrow key for that share of each tick.
(() => {
  'use strict';
  if (document.getElementById('carbon-touch') || !navigator.maxTouchPoints) return;
  const canvas = document.getElementById('canvas');
  if (!canvas) return;
  const LEFT = ['ArrowLeft','ArrowLeft',37], RIGHT = ['ArrowRight','ArrowRight',39];
  const UP = ['ArrowUp','ArrowUp',38], DOWN = ['ArrowDown','ArrowDown',40];
  const MP_MAGIC = 0x31504d43, MP_STATE = 0x10;
  const GHOST_CLICK_MS = 450;  // the browser's click after a finger lifts, once the control under it has gone
  const MIN_PRESS_MS = 120;    // a quick tap must span a frame, and a phone can be at 15 FPS
  const STEER_TICK_MS = 33;    // about one game frame at 30 FPS
  const STEER_DEAD = 0.1;      // a resting thumb or a slightly crooked phone does not steer
  const STEER_FULL = 0.95;     // from here the key is simply held
  const TILT_RANGE_DEG = 24;   // degrees from the rest position to full lock
  const MODES = ['thumb', 'tilt', 'buttons'];
  const MODE_LABEL = { thumb: 'Thumb', tilt: 'Tilt', buttons: 'Buttons' };
  const MODE_KEY = 'carbon-touch-steering';
  const SHOW_KEY = 'carbon-touch-show';
  const LAYOUT_KEY = 'carbon-touch-layout';
  const SIZE_MIN = 0.7, SIZE_MAX = 1.6, SIZE_STEP = 0.1;
  const PAD_MOVED = 0.4;       // a stick resting off centre is not a player
  const BADGE = 'button[aria-label="Online players (F8)"]';
  // Drawn here, 24x24, in the text colour: sharp at any size, and nothing to download.
  const line = 'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
  const ICONS = {
    gas: `<path ${line} d="M6 13l6-6 6 6M6 19l6-6 6 6"/>`,
    brake: `<circle ${line} cx="12" cy="12" r="6"/><path ${line} d="M4 6a11 11 0 0 0 0 12M20 6a11 11 0 0 1 0 12M12 9v3.5M12 15.2v.1"/>`,
    nos: `<path fill="currentColor" d="M12.5 2c.6 3.6-1 5.4-2.6 7.2C8.300 11 6.500 12.900 6.500 15.700 6.500 19.300 9 22 12.200 22c3.300 0 5.800-2.600 5.800-6.100 0-2.600-1.300-4.400-2.500-5.900-.2 1.400-.8 2.400-1.900 3 .4-3.900-.1-8.100-1.600-11z"/>`,
    slide: `<path ${line} d="M8 21c0-5 8-5 8-10 0-2.500-2-4-4-4"/><path ${line} d="M15 3.500L11.500 7l3.500 3.500"/>`,
    // The view of your car changing, not a photo.
    camera: `<path ${line} stroke-width="1.8" d="M6 14l1.5-4.5h9L18 14M4.5 19v-3.5A1.5 1.5 0 0 1 6 14h12a1.5 1.5 0 0 1 1.5 1.5V19M4.5 19H8M16 19h3.5"/><path ${line} stroke-width="1.8" d="M5 6.5A9 9 0 0 1 19 6.5M19 2.8v3.7h-3.7"/>`,
    crew: `<circle ${line} cx="9" cy="8.500" r="3.500"/><path ${line} d="M3 20c0-3.800 2.600-6 6-6s6 2.200 6 6M15.500 5.300a3.500 3.500 0 0 1 0 6.400M18 14.500c1.900.8 3 2.700 3 5.500"/>`,
    speed: `<circle ${line} cx="12" cy="13.5" r="7.5"/><path ${line} d="M9.5 2.5h5M12 2.5V6M12 13.5V9.5M18 6.5l1.3 1.3"/>`,
    menu: `<path ${line} d="M4 7h16M4 12h16M4 17h16"/>`,
    pause: `<path fill="currentColor" d="M7 5h3.500v14H7zM13.500 5H17v14h-3.500z"/>`,
    online: `<path ${line} d="M3 9.500a13 13 0 0 1 18 0M6.500 13.200a8 8 0 0 1 11 0M10 16.800a3 3 0 0 1 4 0"/><circle fill="currentColor" cx="12" cy="19.500" r="1.200"/>`,
    up: `<path ${line} d="M6 15l6-6 6 6"/>`, down: `<path ${line} d="M6 9l6 6 6-6"/>`,
    left: `<path ${line} d="M15 6l-6 6 6 6"/>`, right: `<path ${line} d="M9 6l6 6-6 6"/>`,
    ok: `<path ${line} d="M5 12.500l4.500 4.500L19 7.500"/>`, back: `<path ${line} d="M9 7L4.500 11.500 9 16M5 11.500h9a5 5 0 0 1 5 5V18"/>`,
  };
  const icon = name => `<svg viewBox="0 0 24 24" aria-hidden="true">${ICONS[name]}</svg>`;
  const style = document.createElement('style');
  style.textContent = `
    html, body { overscroll-behavior:none; }
    #canvas { touch-action:none; -webkit-touch-callout:none; -webkit-user-select:none; user-select:none; }
    #carbon-touch { position:fixed; inset:0; z-index:20; pointer-events:none; user-select:none; -webkit-user-select:none; -webkit-touch-callout:none; font:600 12px system-ui,sans-serif; }
    #carbon-touch [hidden] { display:none!important; }
    /* Other pages' parts, smaller on a phone. The Menu button below does what the ☰ does. */
    .cm-open { display:none!important; }
    ${BADGE} { left:max(8px,env(safe-area-inset-left))!important; top:max(8px,env(safe-area-inset-top))!important; min-height:28px!important; padding:3px 8px!important; gap:5px!important; font-size:10px!important; letter-spacing:.05em!important; max-width:34vw; overflow:hidden; white-space:nowrap; opacity:.85; }
    ${BADGE} > span:nth-child(3) { display:none!important; }
    html .cc.touch { --u:11px; }
    html .cc.touch .cc-feed { max-width:min(var(--feed-max,46vw),46vw); }
    .crc-card { width:min(220px,46vw)!important; top:max(46px,calc(env(safe-area-inset-top) + 38px))!important; right:max(8px,env(safe-area-inset-right))!important; max-height:calc(100dvh - 150px)!important; padding:5px 8px 6px!important; gap:4px!important; font-size:11px!important; }
    /* The F9 menu on a phone: a slim bar, categories on the left, settings in all the rest. */
    html .cm .cm-shade { padding:0!important; background:#03090df2!important; }
    html .cm .cm-panel, html .cm .cm-panel.cm-panel-online { width:100%!important; height:100dvh!important; max-height:100dvh!important; }
    html .cm .cm-brand, html .cm .cm-searchbar, html .cm .cm-bottom, html .cm .cm-inspector, html .cm .cm-resume span { display:none!important; }
    html .cm .cm-header { min-height:40px; padding:3px max(10px,env(safe-area-inset-right)) 3px max(10px,env(safe-area-inset-left))!important; align-items:center!important; border-bottom:1px solid var(--line)!important; }
    html .cm .cm-title { font-size:17px!important; }
    html .cm .cm-resume { min-height:32px; padding:6px 14px!important; font-size:11px!important; }
    html .cm .cm-frame { flex:1; min-height:0; display:grid!important; grid-template-columns:clamp(100px,15vw,132px) minmax(0,1fr)!important; grid-template-rows:minmax(0,1fr)!important; padding:0 0 0 env(safe-area-inset-left)!important; column-gap:0!important; overflow:hidden!important; }
    html .cm .cm-rail { grid-column:1!important; grid-row:1!important; flex-direction:column!important; align-items:stretch!important; gap:0!important; margin:0!important; padding:2px 0!important; overflow:hidden auto!important; border:0!important; border-right:1px solid var(--line)!important; touch-action:pan-y; }
    html .cm .cm-rail .cm-nav-item { width:100%!important; min-height:38px!important; padding:8px 10px!important; font-size:12px!important; letter-spacing:.03em!important; text-align:left!important; justify-content:flex-start!important; white-space:normal!important; }
    html .cm .cm-rail .cm-nav-item::after { display:none!important; }
    html .cm .cm-rail .cm-nav-item[aria-current=page] { background:#5fe4f222!important; box-shadow:inset 3px 0 var(--cyan)!important; }
    html .cm .cm-work { grid-column:2!important; grid-row:1!important; min-height:0; overflow:hidden auto!important; overscroll-behavior:contain; -webkit-overflow-scrolling:touch; touch-action:pan-y; padding:4px max(10px,env(safe-area-inset-right)) 14px 10px!important; }
    html .cm .cm-work h2 { font-size:15px!important; margin:2px 0 4px!important; }
    html .cm .cm-work h3 { font-size:10px!important; margin:10px 0 3px!important; letter-spacing:.14em!important; }
    html .cm .cm-work .cm-item { min-height:40px!important; padding:4px 8px 4px 12px!important; font-size:12px!important; touch-action:pan-y; }
    html .cm .cm-work .cm-item .cm-hint { font-size:10px!important; }
    html .cm .cm-work .cm-set-row { min-height:40px!important; padding-top:3px!important; padding-bottom:3px!important; }
    html .cm .cm-work .cm-set-row .cm-val { min-width:70px!important; max-width:120px!important; font-size:12px!important; }
    /* Every tab as tight as Quick: one line a row. */
    html .cm .cm-work .cm-up { display:none!important; }
    html .cm .cm-work .cm-set-title { margin:0!important; }
    html .cm .cm-work .cm-note, html .cm .cm-work .cm-foot, html .cm .cm-work p { margin:1px 0 4px!important; font-size:11px!important; line-height:1.3!important; }
    html .cm .cm-work .cm-set-row .cm-label { display:block!important; line-height:1.15!important; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
    html .cm .cm-work .cm-set-row .cm-set-meta[data-ct="now"] { display:none!important; }
    html .cm .cm-work .cm-set-row .cm-set-meta[data-ct="restart"] { display:inline!important; margin:0 0 0 8px!important; padding:1px 4px; border:1px solid #f2b45f66; border-radius:3px; color:#f2b45f!important; font:600 9px/1 system-ui,sans-serif!important; }
    html .cm .cm-work .cm-gpu { margin:0 0 2px!important; }
    html .cm .cm-work .cm-gpu-toggle { min-height:34px!important; }
    html .cm .cm-work .cm-gpu-label { display:none!important; }
    html .cm .cm-work .cm-gpu-name { margin:0!important; font-size:12px!important; }
    html .cm .cm-work .cm-career-actions .cm-hint { max-width:58%!important; font:500 9px/1.2 system-ui,sans-serif!important; letter-spacing:0!important; text-transform:none!important; }
    html .cm .cm-work .cm-ctl { align-items:center!important; }
    html .cm .cm-work .cm-arr { display:grid; place-items:center; min-width:38px; min-height:34px; padding:0!important; font-size:22px!important; line-height:1!important; opacity:1!important; }
    /* Online and crews. Text boxes stay 16px: below that, iOS zooms the page on focus. */
    html .cm .cm-online-header { margin:0 0 2px!important; padding:0!important; }
    html .cm .cm-online-eyebrow { display:none!important; }
    html .cm .cm-online-tabs { gap:14px!important; margin:0 0 5px!important; }
    html .cm .cm-online-tabs button { min-height:32px; padding:4px 2px!important; font-size:12px!important; }
    html .cm .cm-online-tools { gap:6px!important; }
    html .cm .cm-online-tools label { font-size:10px!important; }
    html .cm .cm-online-tools input, html .cm .cm-online-tools select, html .cm .cm-crew-form input, html .cm .cm-crew-friend { min-height:32px!important; padding:3px 8px!important; font-size:16px!important; }
    html .cm .cm-online-count { margin:4px 0!important; font-size:10px!important; }
    html .cm .cm-online-heading { gap:10px!important; margin:0 0 2px!important; align-items:center!important; }
    html .cm .cm-online-heading h2 { margin:0!important; font-size:15px!important; }
    html .cm .cm-online-heading p:not(.cm-who) { display:none!important; }
    html .cm .cm-online-heading .cm-who { padding:0!important; font-size:10px!important; }
    html .cm .cm-online-players { grid-template-columns:minmax(0,1fr) 190px!important; gap:10px!important; }
    html .cm .cm-online-list { max-height:none!important; overflow:visible!important; }
    html .cm .cm-online-detail { margin-top:4px!important; padding:6px!important; }
    /* Players and crew members: one line a person. */
    html .cm .cm-panel-online .cm-online-player { grid-template-columns:26px minmax(0,1fr) auto!important; gap:2px 8px!important; min-height:38px!important; padding:3px 6px!important; font-size:12px!important; }
    html .cm .cm-panel-online .cm-online-player.cm-crew-member { grid-template-columns:minmax(0,1fr) auto auto!important; }
    html .cm .cm-player-avatar { width:26px!important; height:26px!important; font-size:10px!important; }
    html .cm .cm-player-identity strong, html .cm .cm-member-name strong { display:inline!important; }
    html .cm .cm-panel-online .cm-online-player .crew { display:inline!important; margin:0 0 0 7px!important; font-size:10px!important; }
    html .cm .cm-panel-online .cm-online-player .activity { max-width:92px!important; font-size:10px!important; }
    html .cm .cm-panel-online .cm-member-manage { grid-column:auto!important; grid-row:auto!important; min-height:30px!important; padding:3px 9px!important; font-size:11px!important; }
    html .cm .cm-panel-online .cm-crew-member .cm-crew-member-actions { gap:6px!important; padding-top:2px!important; }
    html .cm .cm-panel-online .cm-crew-member .cm-crew-member-actions .cm-item { min-height:32px!important; padding:3px 9px!important; font-size:11px!important; }
    html .cm .cm-crew-heading { gap:8px!important; margin:0 0 3px!important; }
    html .cm .cm-crew-heading h3 { margin:0!important; font-size:14px!important; }
    html .cm .cm-panel-online .cm-crew-summary { margin:0 0 4px!important; font-size:11px!important; line-height:1.3!important; }
    html .cm .cm-online-crew .cm-member-heading { margin:6px 0 2px!important; }
    html .cm .cm-crew-body { grid-template-columns:minmax(0,1fr) minmax(170px,220px)!important; gap:10px!important; }
    html .cm .cm-crew-tools { padding-inline-start:10px!important; }
    html .cm .cm-crew-management { margin-top:6px!important; padding-top:5px!important; }
    html .cm .cm-crew-steps { margin-top:5px!important; padding-top:5px!important; gap:3px!important; }
    html .cm .cm-crew-steps li { gap:6px!important; font-size:11px!important; }
    html .cm .cm-crew-steps b { width:18px!important; height:18px!important; }
    html .cm .cm-panel-online .cm-crew-outbox { margin-top:6px!important; }
    html .cm .cm-panel-online .cm-online-invite { margin:3px 0!important; padding:4px 6px!important; }
    html .cm .cm-panel-online .cm-online-invite .cm-item, html .cm .cm-panel-online .cm-crew-invite { min-height:32px!important; padding:3px 9px!important; font-size:11px!important; margin-top:3px!important; }
    @media (orientation:portrait) {
      html .cm .cm-work .cm-set-row .cm-label { white-space:normal; }
      html .cm .cm-online-players, html .cm .cm-crew-body { grid-template-columns:minmax(0,1fr)!important; }
      html .cm .cm-crew-tools { padding-inline-start:0!important; }
      html .cm .cm-frame { grid-template-columns:minmax(0,1fr)!important; grid-template-rows:auto minmax(0,1fr)!important; }
      html .cm .cm-rail { flex-direction:row!important; overflow:auto hidden!important; border-right:0!important; border-bottom:1px solid var(--line)!important; touch-action:pan-x; }
      html .cm .cm-rail .cm-nav-item { width:auto!important; flex:none!important; }
      html .cm .cm-work { grid-column:1!important; grid-row:2!important; }
    }
    html[data-ct-badge="off"] ${BADGE} { display:none!important; }
    html[data-ct-chat="off"] .cc-feed, html[data-ct-chat="off"] .cc-chip { display:none!important; }
    #carbon-touch .ct-group { position:absolute; display:flex; gap:8px; align-items:end; }
    #carbon-touch button { touch-action:none; pointer-events:auto; min-width:40px; min-height:40px; padding:6px 8px; border:1px solid #b8e8ec50; border-radius:10px; background:#07151e8c; color:#e7faff; font:inherit; box-shadow:0 3px 12px #0005; -webkit-tap-highlight-color:transparent; }
    #carbon-touch button svg { display:block; width:22px; height:22px; margin:auto; }
    #carbon-touch button span { display:block; margin-top:1px; font-size:9px; letter-spacing:.04em; text-align:center; opacity:.85; }
    #carbon-touch .ct-top button { display:grid; place-items:center; min-width:36px; }
    #carbon-touch .ct-top button svg { width:18px; height:18px; }
    #carbon-touch .ct-nos { left:max(14px,env(safe-area-inset-left)); bottom:calc(max(18px,env(safe-area-inset-bottom)) + 44vh); z-index:1; }
    #carbon-touch .ct-nos button { width:clamp(58px,10vw,82px); height:clamp(46px,16vh,62px); padding:3px; }
    #carbon-touch .ct-nos button.ct-crew { border-color:#8fe0a0; background:#123f268c; color:#cdf7d6; }
    #carbon-touch .ct-nos button.ct-crew[data-held] { background:#8fe0a0d9; color:#04210e; border-color:#e0ffe6; }
    #carbon-touch .ct-nos button.ct-speed { border-color:#7fb6ff; background:#12305a8c; color:#cfe3ff; }
    #carbon-touch .ct-nos button.ct-speed[data-held] { background:#7fb6ffd9; color:#04152e; border-color:#e2eeff; }
    #carbon-touch button.ct-boost { border-color:#ffb066; background:#5a2a0a8c; color:#ffd9b0; }
    #carbon-touch button.ct-boost[data-held] { background:#ff9a3cd9; color:#2a1200; border-color:#ffe2bf; }
    #carbon-touch .ct-gas button, #carbon-touch button.ct-gas { color:#b8fbff; }
    #carbon-touch button:focus-visible { outline:2px solid #83edf0; outline-offset:3px; }
    #carbon-touch button[data-held] { background:#53cbd9d9; color:#03141b; border-color:#b3ffff; }
    #carbon-touch .ct-top { top:max(12px,env(safe-area-inset-top)); right:max(12px,env(safe-area-inset-right)); }
    #carbon-touch .ct-top { gap:6px; }
    #carbon-touch .ct-top button { min-height:32px; min-width:0; padding:4px 9px; font-size:11px; border-radius:8px; }
    #carbon-touch .ct-panel { position:absolute; z-index:1; top:calc(max(12px,env(safe-area-inset-top)) + 38px); right:max(12px,env(safe-area-inset-right)); width:176px; padding:4px; display:flex; flex-direction:column; gap:3px; border:1px solid #b8e8ec50; border-radius:10px; background:#07151ee0; pointer-events:auto; }
    #carbon-touch .ct-panel button { display:flex; justify-content:space-between; align-items:center; width:100%; min-height:32px; padding:4px 9px; font-size:12px; background:transparent; border-color:transparent; box-shadow:none; }
    #carbon-touch .ct-panel button[aria-checked="true"] i { color:#83edf0; }
    #carbon-touch .ct-panel button i { font-style:normal; color:#8aa2ad; }
    #carbon-touch .ct-panel hr { width:100%; margin:1px 0; border:0; border-top:1px solid #b8e8ec30; }
    #carbon-touch .ct-steer { bottom:max(18px,env(safe-area-inset-bottom)); left:max(14px,env(safe-area-inset-left)); }
    #carbon-touch .ct-steer button { width:clamp(52px,9vw,72px); height:clamp(50px,18vh,68px); font-size:18px; }
    #carbon-touch .ct-pedals { bottom:max(18px,env(safe-area-inset-bottom)); right:max(14px,env(safe-area-inset-right)); }
    #carbon-touch .ct-pedals button { width:clamp(58px,10vw,82px); height:clamp(54px,20vh,78px); padding:3px; }
    #carbon-touch .ct-gas { border-color:#67dfe5; background:#164c6099; }
    #carbon-touch .ct-actions { bottom:calc(max(18px,env(safe-area-inset-bottom)) + clamp(62px,calc(20vh + 8px),86px)); right:max(14px,env(safe-area-inset-right)); }
    #carbon-touch .ct-actions button { width:clamp(58px,10vw,82px); height:clamp(46px,17vh,66px); padding:2px; }
    #carbon-touch .ct-wheel { position:absolute; left:0; bottom:0; width:42vw; height:70vh; pointer-events:auto; touch-action:none; }
    #carbon-touch .ct-rail { position:absolute; left:max(14px,env(safe-area-inset-left)); bottom:calc(max(18px,env(safe-area-inset-bottom)) + 34px); width:clamp(120px,20vw,180px); height:6px; border-radius:3px; background:#07151e80; border:1px solid #b8e8ec40; }
    #carbon-touch .ct-rail { opacity:0; transition:opacity .12s; }
    #carbon-touch .ct-wheel[data-held] .ct-rail { opacity:1; }
    #carbon-touch .ct-knob { position:absolute; left:50%; top:50%; width:40px; height:40px; margin:-20px 0 0 -20px; border-radius:50%; background:#164c6099; border:1px solid #67dfe5; box-shadow:0 3px 12px #0005; }
    #carbon-touch .ct-wheel[data-held] .ct-knob { background:#53cbd9d9; border-color:#b3ffff; }
    #carbon-touch .ct-tilt { position:absolute; left:50%; bottom:calc(max(18px,env(safe-area-inset-bottom)) + 4px); width:clamp(120px,22vw,200px); height:6px; margin-left:calc(clamp(120px,22vw,200px) / -2); border-radius:3px; background:#07151e99; }
    #carbon-touch .ct-tilt i { position:absolute; left:50%; top:-3px; width:12px; height:12px; margin-left:-6px; border-radius:50%; background:#83edf0; }
    #carbon-touch .ct-dpad { display:grid; align-items:stretch; grid-template:repeat(3,clamp(38px,14vh,48px)) / repeat(3,clamp(42px,7.5vw,52px)); gap:3px; left:max(14px,env(safe-area-inset-left)); bottom:max(14px,env(safe-area-inset-bottom)); }
    #carbon-touch .ct-dpad button { min-width:0; min-height:0; padding:0; font-size:15px; }
    #carbon-touch .ct-dpad button:nth-child(1) { grid-area:1/2; } #carbon-touch .ct-dpad button:nth-child(2) { grid-area:2/1; }
    #carbon-touch .ct-dpad button:nth-child(3) { grid-area:2/3; } #carbon-touch .ct-dpad button:nth-child(4) { grid-area:3/2; }
    #carbon-touch .ct-confirm { right:max(14px,env(safe-area-inset-right)); bottom:max(14px,env(safe-area-inset-bottom)); }
    #carbon-touch .ct-confirm button { width:clamp(56px,9vw,74px); height:clamp(46px,17vh,62px); padding:3px; }
    /* Arranging: over the F9 menu, which stays open underneath so no key reaches the game. */
    #carbon-touch[data-arranging] { z-index:60; }
    #carbon-touch[data-arranging]::before { content:""; position:absolute; inset:0; background:#03090dcc; pointer-events:auto; }
    #carbon-touch[data-arranging] .ct-group button { border-style:dashed; }
    #carbon-touch[data-arranging] .ct-group button[data-picked] { border:2px solid #83edf0; background:#164c60e6; }
    #carbon-touch .ct-arrange { position:absolute; z-index:2; top:max(8px,env(safe-area-inset-top)); left:50%; transform:translateX(-50%); box-sizing:border-box; width:max-content; max-width:calc(100vw - 16px); display:flex; flex-wrap:wrap; align-items:center; justify-content:center; gap:6px; padding:5px 8px; border:1px solid #b8e8ec30; border-radius:10px; background:#07151ef2; color:#e7faff; }
    #carbon-touch .ct-arrange p { margin:0 4px 0 0; min-width:150px; font-size:12px; line-height:1.25; }
    #carbon-touch .ct-arrange small { display:block; color:#a9c4cd; font-size:10px; font-weight:500; }
    #carbon-touch .ct-arrange button { min-width:44px; min-height:40px; padding:4px 10px; font-size:13px; }
    #carbon-touch .ct-arrange .ct-size { font-size:20px; line-height:1; }
    #carbon-touch .ct-arrange .ct-done { border-color:#67dfe5; background:#164c60; color:#b8fbff; }
    #carbon-touch .ct-rotate { position:absolute; top:40%; left:0; right:0; text-align:center; color:#e7faff; font-size:16px; text-shadow:0 1px 4px #000; }
  `;
  document.head.append(style);
  const root = document.createElement('div');
  root.id = 'carbon-touch'; root.setAttribute('aria-label', 'Touch controls');
  const held = new Map();
  const keys = new Map();
  const event = (type, binding) => {
    const [key, code, keyCode] = binding;
    canvas.dispatchEvent(new KeyboardEvent(type, {key, code, keyCode, which:keyCode, bubbles:true, cancelable:true, shiftKey:code === 'ShiftRight'}));
  };
  // A binding is one key, or a list of keys held together.
  const each = binding => Array.isArray(binding[0]) ? binding : [binding];
  function drop(item) {
    for (const binding of each(item.binding)) {
      const count = keys.get(binding[1]) - 1;
      if (count) keys.set(binding[1], count);
      else { keys.delete(binding[1]); event('keyup', binding); }
    }
    if (item.button && ![...held.values()].some(v => v.button === item.button)) delete item.button.dataset.held;
  }
  // The new keys go down before the old ones lift: Gas stays held on the way to NOS + Gas.
  function press(id, button, binding) {
    const old = held.get(id);
    held.set(id, {button, binding, since:performance.now()});
    if (button) button.dataset.held = '';
    for (const key of each(binding)) {
      const count = keys.get(key[1]) || 0; keys.set(key[1], count + 1);
      if (!count) event('keydown', key);
    }
    if (old) drop(old);
  }
  function release(id) {
    const item = held.get(id); if (!item) return;
    held.delete(id); drop(item);
  }
  // A finger lifted: the key stays down until the press has lasted long enough for the game to see it.
  let ghostUntil = 0;
  addEventListener('click', e => {
    if (performance.now() < ghostUntil && !e.target.closest?.('#carbon-touch')) { e.preventDefault(); e.stopPropagation(); }
  }, true);
  function lift(id) {
    ghostUntil = performance.now() + GHOST_CLICK_MS;
    const item = held.get(id); if (!item || item.lifting) return;
    const left = MIN_PRESS_MS - (performance.now() - item.since);
    if (left <= 0) return release(id);
    item.lifting = true;
    setTimeout(() => { if (held.get(id) === item) release(id); }, left);
  }
  function releaseAll() { for (const id of [...held.keys()]) release(id); setSteer(0); }

  // ---- Proportional steering: a share of each tick on the arrow key ----
  let steerValue = 0, steerAcc = 0, steerTimer = 0;
  const STEER_ID = 'steer';
  function steerTick() {
    const amount = Math.abs(steerValue);
    let on = amount >= STEER_FULL;
    if (on) steerAcc = 0;
    else { steerAcc += amount; if (steerAcc >= 1) { steerAcc -= 1; on = amount > 0; } }
    const wanted = on ? (steerValue < 0 ? LEFT : RIGHT) : null;
    if (held.get(STEER_ID)?.binding !== wanted) wanted ? press(STEER_ID, null, wanted) : release(STEER_ID);
    if (!amount) { clearInterval(steerTimer); steerTimer = 0; steerAcc = 0; }
  }
  function setSteer(raw) {
    const size = Math.abs(raw), sign = Math.sign(raw);
    // Fine control near the centre, full lock at the end of the travel.
    const eased = size < STEER_DEAD ? 0 : ((size - STEER_DEAD) / (1 - STEER_DEAD)) ** 1.5;
    steerValue = sign * Math.min(eased, 1);
    if (steerValue && !steerTimer) steerTimer = setInterval(steerTick, STEER_TICK_MS);
    if (!steerValue) steerTick();
  }

  const bindings = new Map();
  let arranging = false;
  function group(name, entries, into = root) {
    const box = document.createElement('div'); box.className = `ct-group ct-${name}`;
    for (const [label, title, binding, className] of entries) {
      const button = document.createElement('button');
      button.type = 'button'; button.setAttribute('aria-label', title);
      // 'icon' alone, or 'icon Caption'.
      const [name, ...caption] = label.split(' ');
      button.innerHTML = icon(name) + (caption.length ? `<span>${caption.join(' ')}</span>` : '');
      if (className) button.className = className;
      button.dataset.ct = name;
      bindings.set(button, binding);
      button.addEventListener('pointerdown', e => {
        if (e.pointerType === 'mouse' && e.button !== 0) return;
        e.preventDefault();
        if (arranging) return arrangeDown(e, button);
        canvas.focus({preventScroll:true});
        button.setPointerCapture(e.pointerId);
        ghostUntil = performance.now() + GHOST_CLICK_MS;
        press(e.pointerId, button, binding);
        navigator.vibrate?.(8);
      });
      // A thumb slides between Gas, Brake, NOS + Gas and Slide without lifting.
      button.addEventListener('pointermove', e => {
        if (arranging) return arrangeMove(e);
        const item = held.get(e.pointerId); if (!item) return;
        const over = document.elementFromPoint(e.clientX, e.clientY)?.closest?.('button');
        if (over && over !== item.button && bindings.has(over)) press(e.pointerId, over, bindings.get(over));
      });
      for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) button.addEventListener(type, e => arranging ? arrangeUp(e) : lift(e.pointerId));
      button.addEventListener('contextmenu', e => e.preventDefault());
      box.append(button);
    }
    into.append(box); return box;
  }
  const steerButton = document.createElement('button');
  const top = group('top', [['camera','Change camera',['c','KeyC',67]], ['menu','Open Carbon menu',['F9','F9',120]], ['pause','Pause or back',['Escape','Escape',27]]]);
  top.prepend(steerButton);
  const cameraButton = top.querySelector('[aria-label="Change camera"]');

  // ---- Online: what shows on screen, in one small panel ----
  const onlineButton = document.createElement('button');
  onlineButton.type = 'button'; onlineButton.innerHTML = icon('online'); onlineButton.setAttribute('aria-label', 'Online options');
  onlineButton.setAttribute('aria-expanded', 'false');
  cameraButton.after(onlineButton);
  const panel = document.createElement('div'); panel.className = 'ct-panel'; panel.hidden = true;
  panel.setAttribute('aria-label', 'Online options');
  let shown = {badge: true, chat: true};
  try { shown = {...shown, ...JSON.parse(localStorage.getItem(SHOW_KEY))}; } catch {}
  const invitesOn = () => globalThis.recompRace?.state?.().invitesOn !== false;
  const switches = [];
  function addSwitch(label, get, set) {
    const button = document.createElement('button'), state = document.createElement('i');
    button.type = 'button'; button.setAttribute('role', 'switch'); button.textContent = label; button.append(state);
    const paint = () => { const on = get(); button.setAttribute('aria-checked', String(on)); state.textContent = on ? 'On' : 'Off'; };
    button.addEventListener('click', () => { set(!get()); paint(); });
    switches.push(paint); panel.append(button);
  }
  function applyShown() {
    document.documentElement.dataset.ctBadge = shown.badge ? 'on' : 'off';
    document.documentElement.dataset.ctChat = shown.chat ? 'on' : 'off';
    try { localStorage.setItem(SHOW_KEY, JSON.stringify(shown)); } catch {}
  }
  addSwitch('Online badge', () => shown.badge, on => { shown.badge = on; applyShown(); });
  addSwitch('Chat', () => shown.chat, on => { shown.chat = on; applyShown(); });
  addSwitch('Race invitations', invitesOn, on => globalThis.recompRace?.setInvites?.(on));
  panel.append(document.createElement('hr'));
  const openOnlineTab = tab => { dispatchEvent(new Event('recomp-open-online')); document.querySelector(`[data-online-tab="${tab}"]`)?.click(); };
  for (const [label, run] of [['Players', () => openOnlineTab('players')], ['My crew', () => openOnlineTab('crew')],
                              ['Send a message', () => document.querySelector('.cc-chip')?.click()]]) {
    const button = document.createElement('button');
    button.type = 'button'; button.textContent = label;
    button.addEventListener('click', () => { showPanel(false); run(); });
    panel.append(button);
  }
  root.append(panel);
  function showPanel(on) {
    panel.hidden = !on; onlineButton.setAttribute('aria-expanded', String(on));
    if (on) for (const paint of switches) paint();
  }
  onlineButton.addEventListener('click', () => showPanel(panel.hidden));
  addEventListener('pointerdown', e => { if (!panel.hidden && !e.target.closest?.('.ct-panel,[aria-label="Online options"]')) showPanel(false); }, true);
  applyShown();
  const steerZone = document.createElement('div'); steerZone.className = 'ct-wheel';
  steerZone.setAttribute('role', 'slider'); steerZone.setAttribute('aria-label', 'Steering: drag left or right');
  const rail = document.createElement('div'); rail.className = 'ct-rail';
  const knob = document.createElement('div'); knob.className = 'ct-knob';
  rail.append(knob); steerZone.append(rail); root.append(steerZone);
  const tiltBar = document.createElement('div'); tiltBar.className = 'ct-tilt';
  const tiltDot = document.createElement('i'); tiltBar.append(tiltDot); root.append(tiltBar);
  const steerKeys = group('steer', [['left','Steer left',LEFT], ['right','Steer right',RIGHT]]);
  const pedals = [
    group('pedals', [['brake Brake','Brake or reverse',DOWN], ['gas Gas','Accelerate',UP,'ct-gas']]),
    // NOS + Gas sits above Gas: the thumb slides up to it and the throttle never lifts.
    group('actions', [['slide Slide','Handbrake',[' ','Space',32]], ['nos NOS + Gas','Nitrous and accelerate',[UP,['Shift','ShiftRight',16]],'ct-boost']]),
    // On the left hand's side: the right thumb is on the pedals when these are wanted.
    // G is the game's Speedbreaker (GAME_ACTION_GAMEBREAKER: X or G); Crew is GAME_ACTION_CREWDEFENSIVE (Right Ctrl).
    group('nos', [['crew Crew','Activate crew member',['Control','ControlRight',17],'ct-crew'], ['speed G','Speedbreaker',['g','KeyG',71],'ct-speed']]),
  ];
  const menus = [
    group('dpad', [['up','Menu up',UP], ['left','Menu left',LEFT], ['right','Menu right',RIGHT], ['down','Menu down',DOWN]]),
    group('confirm', [['back Back','Back',['Escape','Escape',27]], ['ok OK','Confirm',['Enter','Enter',13]]]),
  ];
  const rotate = document.createElement('div'); rotate.className = 'ct-rotate'; rotate.textContent = 'Rotate your phone to landscape';
  root.append(rotate);
  document.body.append(root);

  // ---- Arrange: every driving button can move and change size ----
  // A transform moves a button without shifting its neighbours in the row. Offsets are shares of
  // the screen, so a layout still fits when a phone or tablet is turned.
  const layout = new Map();
  const shareOf = v => Math.min(1, Math.max(-1, v));
  const sizeOf = v => Math.min(SIZE_MAX, Math.max(SIZE_MIN, Math.round(v * 10) / 10));
  try {
    for (const [id, c] of Object.entries(JSON.parse(localStorage.getItem(LAYOUT_KEY)) || {}))
      if (Number.isFinite(c?.x) && Number.isFinite(c?.y) && Number.isFinite(c?.s)) layout.set(id, {x:shareOf(c.x), y:shareOf(c.y), s:sizeOf(c.s)});
  } catch {}
  const arrangeable = () => [...pedals, steerKeys].flatMap(box => [...box.children]);
  const spot = button => layout.get(button.dataset.ct) || {x:0, y:0, s:1};
  function place(button) {
    const {x, y, s} = spot(button);
    button.style.transform = x || y || s !== 1 ? `translate(${(x * innerWidth).toFixed(1)}px,${(y * innerHeight).toFixed(1)}px) scale(${s})` : '';
  }
  const placeAll = () => arrangeable().forEach(place);
  const saveLayout = () => {
    try { layout.size ? localStorage.setItem(LAYOUT_KEY, JSON.stringify(Object.fromEntries(layout))) : localStorage.removeItem(LAYOUT_KEY); } catch {}
  };
  const arrangeBar = document.createElement('div'); arrangeBar.className = 'ct-arrange'; arrangeBar.hidden = true;
  const arrangeLine = document.createElement('p'); arrangeLine.setAttribute('aria-live', 'polite');
  arrangeBar.append(arrangeLine); root.append(arrangeBar);
  let picked = null, drag = null;
  function paintArrange() {
    const what = document.createElement('b');
    what.textContent = picked ? `${picked.getAttribute('aria-label')}: ${Math.round(spot(picked).s * 100)}%` : 'All buttons';
    const how = document.createElement('small');
    how.textContent = picked ? 'Drag to move, − or + to resize' : 'Drag to move, tap one to resize only it';
    arrangeLine.replaceChildren(what, how);
  }
  function pick(button) {
    if (picked) delete picked.dataset.picked;
    picked = button;
    if (picked) picked.dataset.picked = '';
    paintArrange();
  }
  // Fully on the screen and clear of the bar, where a button could no longer be reached.
  function keepOnScreen(button, c) {
    const r = button.getBoundingClientRect(), top = arrangeBar.getBoundingClientRect().bottom + 4;
    if (!r.width) return; // hidden: steering buttons outside Buttons steering
    const dx = r.left < 0 ? -r.left : r.right > innerWidth ? innerWidth - r.right : 0;
    const dy = r.bottom > innerHeight ? innerHeight - r.bottom : r.top < top ? top - r.top : 0;
    if (dx || dy) { c.x += dx / innerWidth; c.y += dy / innerHeight; place(button); }
  }
  function arrangeDown(e, button) {
    button.setPointerCapture(e.pointerId);
    pick(button);
    const c = {...spot(button)};
    layout.set(button.dataset.ct, c);
    drag = {id:e.pointerId, button, c, startX:e.clientX, startY:e.clientY, fromX:c.x, fromY:c.y};
  }
  function arrangeMove(e) {
    if (e.pointerId !== drag?.id) return;
    const {button, c} = drag;
    c.x = shareOf(drag.fromX + (e.clientX - drag.startX) / innerWidth);
    c.y = shareOf(drag.fromY + (e.clientY - drag.startY) / innerHeight);
    place(button); keepOnScreen(button, c);
  }
  function arrangeUp(e) {
    if (e.pointerId === drag?.id) { drag = null; saveLayout(); }
  }
  function resizeButtons(step) {
    for (const button of picked ? [picked] : arrangeable()) {
      const c = {...spot(button)};
      c.s = sizeOf(c.s + step);
      layout.set(button.dataset.ct, c);
      place(button); keepOnScreen(button, c);
    }
    saveLayout(); paintArrange();
  }
  function arrange(on) {
    arranging = on;
    root.toggleAttribute('data-arranging', on);
    arrangeBar.hidden = !on;
    drag = null; pick(null);
    releaseAll(); sync();
  }
  for (const [text, title, className, run] of [
    ['−', 'Smaller', 'ct-size', () => resizeButtons(-SIZE_STEP)],
    ['+', 'Bigger', 'ct-size', () => resizeButtons(SIZE_STEP)],
    ['Reset', 'Put every button back', '', () => { layout.clear(); saveLayout(); placeAll(); pick(null); }],
    ['Done', 'Finish arranging', 'ct-done', () => arrange(false)],
  ]) {
    const button = document.createElement('button');
    button.type = 'button'; button.textContent = text; button.setAttribute('aria-label', title);
    if (className) button.className = className;
    button.addEventListener('click', run);
    arrangeBar.append(button);
  }
  root.addEventListener('pointerdown', e => { if (arranging && e.target === root) pick(null); });
  addEventListener('resize', placeAll);
  placeAll();
  // keepOpen: the menu stays under the editor and the game keeps getting a pad with nothing held.
  (globalThis.recompMenuActions ||= []).push({id:'touch-layout', group:'Controls', label:'Move and resize touch buttons', keepOpen:true, run:() => arrange(true)});

  // ---- Thumb: a floating stick, so the centre is wherever the thumb lands ----
  let thumb = null;
  const thumbRange = () => Math.min(Math.max(innerWidth * 0.09, 48), 90);
  const showKnob = v => { knob.style.transform = `translateX(${(v * rail.clientWidth / 2).toFixed(1)}px)`; };
  function endThumb(e) {
    if (e.pointerId !== thumb?.id) return;
    thumb = null; delete steerZone.dataset.held; showKnob(0); setSteer(0);
  }
  steerZone.addEventListener('pointerdown', e => {
    e.preventDefault(); canvas.focus({preventScroll:true});
    steerZone.setPointerCapture(e.pointerId);
    thumb = {id:e.pointerId, centre:e.clientX}; steerZone.dataset.held = '';
  });
  steerZone.addEventListener('pointermove', e => {
    if (e.pointerId !== thumb?.id) return;
    const range = thumbRange();
    // Past full lock the centre follows the thumb, so turning back needs no long travel.
    thumb.centre = Math.min(Math.max(thumb.centre, e.clientX - range), e.clientX + range);
    const v = (e.clientX - thumb.centre) / range;
    showKnob(v); setSteer(v);
  });
  for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) steerZone.addEventListener(type, endThumb);
  steerZone.addEventListener('contextmenu', e => e.preventDefault());

  // ---- Tilt ----
  let tiltZero = null, tiltOn = false;
  // window.orientation first: on phones it is the rotation of the screen in both browsers.
  function turned() {
    const a = typeof window.orientation === 'number' ? window.orientation : screen.orientation?.angle ?? 0;
    return (a + 360) % 360;
  }
  function onTilt(e) {
    const a = turned();
    const value = a === 90 ? e.beta : a === 270 ? -e.beta : a === 180 ? -e.gamma : e.gamma;
    if (value == null || !tiltOn) return;
    if (tiltZero === null) tiltZero = value;
    const v = Math.min(Math.max((value - tiltZero) / TILT_RANGE_DEG, -1), 1);
    tiltDot.style.transform = `translateX(${(v * tiltBar.clientWidth / 2).toFixed(1)}px)`;
    setSteer(v);
  }
  function setTilt(on) {
    if (on === tiltOn) return;
    tiltOn = on; tiltZero = null; setSteer(0);
    if (on) addEventListener('deviceorientation', onTilt); else removeEventListener('deviceorientation', onTilt);
  }

  // ---- Steering mode ----
  let mode = 'thumb';
  try { const saved = localStorage.getItem(MODE_KEY); if (MODES.includes(saved)) mode = saved; } catch {}
  function setMode(next, save = true) {
    mode = next;
    if (save) try { localStorage.setItem(MODE_KEY, next); } catch {}
    steerButton.textContent = `Steer: ${MODE_LABEL[next]}`;
    releaseAll(); sync();
  }
  steerButton.type = 'button';
  steerButton.setAttribute('aria-label', 'Change steering: thumb, tilt or buttons');
  steerButton.addEventListener('click', async () => {
    let next = MODES[(MODES.indexOf(mode) + 1) % MODES.length];
    if (next === 'tilt') {
      // iOS asks once, and only from a tap. Refused or missing: go on to Buttons.
      try {
        const ask = globalThis.DeviceOrientationEvent?.requestPermission;
        if (!globalThis.DeviceOrientationEvent || (ask && await ask.call(DeviceOrientationEvent) !== 'granted')) next = 'buttons';
      } catch { next = 'buttons'; }
    }
    setMode(next);
  });

  // ---- A controller takes over ----
  // Browsers announce a pad only after a press on it, so 'gamepadconnected' is already a player.
  // Touching the screen brings the buttons back, in case the pad is flat or was never wanted:
  // some Android phones list their fingerprint sensor as a gamepad.
  let padInUse = false;
  const realPads = () => {
    try { return [...(navigator.getGamepads?.() || [])].filter(g => g?.connected && !/hyperx cloud/i.test(g.id)); }
    catch { return []; }
  };
  const padTouched = g => g.buttons.some(b => b.pressed) || g.axes.some(a => Math.abs(a) > PAD_MOVED);
  const usePad = on => { if (padInUse !== on) { padInUse = on; sync(); } };
  addEventListener('gamepadconnected', e => { if (!/hyperx cloud/i.test(e.gamepad.id)) usePad(true); });
  addEventListener('gamepaddisconnected', () => { if (!realPads().length) usePad(false); });
  addEventListener('pointerdown', e => { if (e.pointerType === 'touch') usePad(false); }, true);
  setInterval(() => { if (!padInUse && realPads().some(padTouched)) usePad(true); }, 100);

  // ---- Which layout ----
  function inCar() {
    const b = globalThis.carbonMpBlock;
    const memory = b?.memory?.buffer;
    if (!memory || !b.ptr) return null;
    const u32 = new Uint32Array(memory, b.ptr & ~3, 8);
    return u32[0] === MP_MAGIC ? u32[MP_STATE >> 2] === 1 : null;
  }
  const portrait = matchMedia('(orientation: portrait)');
  let wasDriving = false;
  // HTML menus own touch input. Native game menus use the D-pad and OK.
  function sync() {
    const typing = !!document.activeElement?.closest('input,textarea,select,[contenteditable="true"]');
    const blocked = typing || !!globalThis.recompMenuOpen || document.hidden || padInUse;
    const driving = !blocked && !arranging && inCar() === true, menu = !blocked && !arranging && !driving;
    const show = (el, on) => { if (el.hidden === on) el.hidden = !on; };
    for (const box of pedals) show(box, driving || arranging);
    for (const box of menus) show(box, menu);
    show(steerKeys, (driving || arranging) && mode === 'buttons');
    show(steerZone, driving && mode === 'thumb');
    show(tiltBar, driving && mode === 'tilt');
    show(steerButton, driving); show(cameraButton, driving);
    const online = !blocked && !arranging && !!document.querySelector(BADGE);
    show(onlineButton, online);
    if (!online && !panel.hidden) showPanel(false);
    show(rotate, (driving || arranging) && portrait.matches && Math.min(screen.width, screen.height) <= 600);
    show(top, !blocked && !arranging);
    // Only a restart note is worth a phone's row: the menu writes the same element for both.
    if (globalThis.recompMenuOpen) for (const meta of document.querySelectorAll('.cm-set-meta:not([data-ct])')) {
      meta.dataset.ct = /restart/i.test(meta.textContent) ? 'restart' : 'now';
      if (meta.dataset.ct === 'restart') meta.textContent = 'Restart';
    }
    setTilt(driving && mode === 'tilt');
    // Leaving the car with a finger down must not leave a key held in the game's menu.
    if (driving !== wasDriving) { wasDriving = driving; releaseAll(); }
    else if (blocked && (held.size || steerValue)) releaseAll();
  }
  const observer = new MutationObserver(sync);
  observer.observe(document.body, {subtree:true, attributes:true, attributeFilter:['hidden','open','class']});
  addEventListener('focusin', sync); addEventListener('focusout', sync);
  addEventListener('blur', releaseAll); addEventListener('pagehide', releaseAll);
  addEventListener('visibilitychange', sync);
  // Not on every resize: Safari's toolbars resize the page mid-race, and that must not lift the gas.
  portrait.addEventListener('change', () => { releaseAll(); tiltZero = null; sync(); });
  addEventListener('keydown', e => { if (['F9','F10','Escape'].includes(e.key)) queueMicrotask(sync); });
  // iOS pinch zoom ignores touch-action on the page itself.
  for (const type of ['gesturestart', 'gesturechange']) document.addEventListener(type, e => e.preventDefault());
  // Android: full screen is the only state that can hold landscape. iOS has neither call on iPhone.
  document.addEventListener('fullscreenchange', () => {
    if (document.fullscreenElement) screen.orientation?.lock?.('landscape').catch(() => {});
  });
  // The game changes state without touching the page.
  setInterval(sync, 250);
  setMode(mode, false);
})();
