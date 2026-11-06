// The start page's new design: tabs, a hero picture and Play always in reach.
// start.js, saves.js and home-settings.js still own every control: this file moves their real
// elements into the new layout and never copies their logic, so Play, saves, cloud sync, name
// protection and settings work as before. If this file fails to load, start.js's page shows.
(() => {
  'use strict';
  const root = document.querySelector('.cs');
  if (!root || root.querySelector('.ph')) return;
  const $ = (s, r = root) => r.querySelector(s);
  const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html) e.innerHTML = html; return e; };
  const touch = matchMedia('(pointer: coarse)').matches;
  const css = `
  .cs:has(.ph) { overflow:hidden; background:#03080e; font-family:"Saira","Segoe UI",system-ui,sans-serif; }
  .cs:has(.ph)::before, .cs:has(.ph)::after { display:none; }
  .ph { --cyan:#3fe0e8; --soft:#a9bdc8; --line:rgba(63,224,232,.18); --skew:-14deg; --cond:"Saira Condensed","Arial Narrow",system-ui,sans-serif; --body:"Saira","Segoe UI",system-ui,sans-serif;
    position:absolute; inset:0; display:flex; flex-direction:column; isolation:isolate; overflow:hidden; color:var(--ink); }
  .ph-art { position:absolute; z-index:-2; inset:12% 0 auto 34%; aspect-ratio:16/9; background:url("play-home/hero-carbon.webp") center/cover no-repeat;
    -webkit-mask-image:linear-gradient(90deg,transparent,#000 22%),linear-gradient(180deg,transparent,#000 16%,#000 78%,transparent); -webkit-mask-composite:source-in;
    mask-image:linear-gradient(90deg,transparent,#000 22%),linear-gradient(180deg,transparent,#000 16%,#000 78%,transparent); mask-composite:intersect; }
  .ph-shade { position:absolute; z-index:-1; inset:0; pointer-events:none;
    background:linear-gradient(90deg,#03080e 0%,#03080e 30%,rgba(3,8,14,.6) 46%,transparent 66%); }
  .ph-top { flex:none; display:flex; align-items:center; justify-content:space-between; gap:12px; padding:clamp(10px,2.4vh,22px) clamp(16px,4vw,64px) 4px; }
  .ph-logo { margin:0; line-height:0; } .ph-logo img { display:block; width:clamp(230px,23vw,340px); height:auto; filter:drop-shadow(0 4px 18px rgba(63,224,232,.22)); }
  .ph-links { display:flex; align-items:center; gap:8px; flex:none; }
  .ph .ph-links .cs-btn { display:inline-flex; align-items:center; justify-content:center; min-height:38px; padding:7px 14px; font-size:12px; white-space:nowrap; flex:none; }
  .ph .ph-links .cs-discord { color:#fff; background:#5865f2; border-color:#8c95ff; }
  .ph .ph-links .cs-discord > span { display:inline-flex; align-items:center; gap:8px; }
  .ph .ph-links .cs-discord svg { width:18px; height:18px; fill:currentColor; flex:none; }
  .ph .ph-links .cs-support { padding-inline:12px; }
  .ph .ph-links .cs-support svg { width:15px; height:15px; }
  .ph-tabs { flex:none; display:flex; margin:6px clamp(16px,4vw,64px) 0; border-bottom:1px solid var(--line); }
  .ph-tab { position:relative; padding:12px clamp(12px,1.5vw,22px) 11px; border:0; background:none; cursor:pointer; color:#9db2bd; font:italic 700 15px/1 var(--cond); letter-spacing:.14em; text-transform:uppercase; }
  .ph-tab:hover { color:var(--ink); }
  .ph-tab::after { content:""; position:absolute; left:10px; right:10px; bottom:-1px; height:3px; background:var(--cyan); transform:skewX(var(--skew)) scaleX(0); transition:transform .18s cubic-bezier(.2,.8,.2,1); }
  .ph-tab[aria-selected=true] { color:#fff; text-shadow:0 0 18px rgba(63,224,232,.55); }
  .ph-tab[aria-selected=true]::after { transform:skewX(var(--skew)) scaleX(1); }
  .ph-frame { flex:1; min-height:0; display:grid; grid-template-columns:minmax(300px,440px) minmax(0,1fr); column-gap:clamp(30px,7vw,120px); padding:0 clamp(16px,4vw,64px); }
  .ph-work { min-height:0; overflow:hidden auto; padding:16px 8px 18px 2px; scrollbar-width:thin; scrollbar-color:var(--line) transparent; overscroll-behavior:contain; }
  .ph-page { display:none; } .ph-page.on { display:block; animation:ph-in .2s cubic-bezier(.2,.8,.2,1) both; }
  @keyframes ph-in { from { opacity:0; transform:translateY(5px); } }
  .ph-page > h2 { margin:0 0 3px; font:italic 700 26px/1.05 var(--cond); letter-spacing:.03em; text-transform:uppercase; }
  .ph-sub { margin:0 0 12px; color:var(--soft); font-size:14px; line-height:1.45; }
  .ph-section { margin:16px 0 5px; display:flex; align-items:center; gap:12px; color:var(--cyan); font:700 12px/1 var(--cond); letter-spacing:.24em; text-transform:uppercase; }
  .ph-section::after { content:""; flex:1; height:1px; background:linear-gradient(90deg,var(--line),transparent); }
  .ph-choice { display:grid; grid-template-columns:1fr 1fr; gap:12px; margin:6px 5px 12px; }
  .ph-pick { position:relative; isolation:isolate; min-height:84px; padding:10px 18px; border:0; background:none; cursor:pointer; color:var(--ink); text-align:left; }
  .ph-pick::before { content:""; position:absolute; z-index:-1; inset:0; transform:skewX(var(--skew)); border:1px solid var(--line); background:rgba(255,255,255,.03); transition:background .12s,border-color .12s; }
  .ph-pick:hover::before { border-color:var(--cyan); }
  .ph-pick[aria-checked=true]::before { border-color:var(--cyan); background:linear-gradient(100deg,rgba(63,224,232,.26),rgba(63,224,232,.07)); box-shadow:0 0 18px rgba(63,224,232,.2); }
  .ph-pick b { display:block; font:italic 700 21px/1.1 var(--cond); letter-spacing:.03em; text-transform:uppercase; }
  .ph-pick small { display:block; margin-top:3px; color:var(--soft); font:400 13px/1.25 var(--body); }
  .ph-row { position:relative; isolation:isolate; display:flex; align-items:center; justify-content:space-between; gap:14px; width:100%; min-height:48px; margin:2px 0; padding:7px 18px 7px 22px; border:0; background:none; cursor:pointer; color:var(--ink); text-align:left; font:500 15px/1.25 var(--body); }
  .ph-row::before { content:""; position:absolute; z-index:-1; inset:0; transform:skewX(var(--skew)); background:rgba(255,255,255,.03); }
  .ph-row:hover::before { background:rgba(63,224,232,.08); }
  .ph-row small { display:block; margin-top:1px; color:var(--soft); font-size:13px; font-weight:400; }
  .ph-career { position:relative; isolation:isolate; display:block; width:100%; margin:0 0 6px; padding:11px 22px 12px; border:0; background:none; cursor:pointer; color:var(--ink); text-align:left; }
  .ph-career::before { content:""; position:absolute; z-index:-1; inset:0; transform:skewX(var(--skew)); border:1px solid var(--line); background:rgba(255,255,255,.04); }
  .ph-career:hover::before { border-color:var(--cyan); }
  .ph-career-label { display:block; color:var(--cyan); font:700 12px/1 var(--cond); letter-spacing:.2em; text-transform:uppercase; }
  .ph-career strong { display:block; margin:5px 90px 2px 0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; font:italic 700 25px/1.1 var(--cond); letter-spacing:.03em; }
  .ph-career small { display:block; color:var(--soft); font:400 13px/1.3 var(--body); }
  .ph-career-go { position:absolute; right:22px; top:50%; transform:translateY(-50%); color:var(--cyan); font:500 14px/1 var(--body); }
  .ph .ph-links .cs-count { display:flex; align-items:center; gap:8px; min-height:38px; margin:0; padding:0 8px; color:transparent; font:600 13px/1 var(--body); font-size:0; white-space:nowrap; }
  .ph .ph-links .cs-count:empty { display:none; }
  .ph .ph-links .cs-count b { color:var(--ink); font-size:13px; font-weight:500; }
  .ph .ph-links .cs-count b::before { content:""; display:inline-block; width:8px; height:8px; margin-right:7px; border-radius:50%; background:#49e38a; box-shadow:0 0 9px #49e38a; }
  /* start.js's panels, without their boxes */
  .ph .cs-panel { padding:0; border:0; background:none; box-shadow:none; clip-path:none; backdrop-filter:none; -webkit-backdrop-filter:none; }
  .ph .cs-panel::before, .ph .cs-panel::after { display:none; }
  .ph .cs-panel > h2 { display:none; }
  .ph .cp > .cs-note:first-of-type { display:none; }
  .ph .cs-online > .cs-switch, .ph .cs-online > .cs-note:first-of-type { display:none; }
  /* The crew is changed in the game. The field stays in the page, hidden: start.js reads it at Play. */
  .ph .cs-row:has(.cs-crew), .ph #cs-crew-help { display:none; }
  .ph-online { display:none; margin:0 2px 8px; } .ph.is-online .ph-online { display:block; }
  .ph .cs-note, .ph .sv .cs-note { color:var(--soft); font-size:13px; line-height:1.4; }
  .ph .cs-label { color:#c4d4dc; font:500 13px/1.2 var(--body); letter-spacing:0; text-transform:none; }
  .ph .cs-row { gap:3px; margin-bottom:9px; }
  .ph .cs-row input, .ph .cs-row select, .ph .cp-row select, .ph .cp-row input[type=number] { min-height:42px; padding:6px 12px; border:1px solid var(--line); background:rgba(4,10,15,.8); color:var(--ink); font:400 16px/1.2 var(--body); }
  .ph .cs-row input::placeholder { color:#7f96a3; }
  .ph .cs-switch { min-height:44px; margin:0; padding:6px 0; border-bottom:1px solid #ffffff12; font:500 15px/1.25 var(--body); }
  .ph .cp-row { padding:7px 0; border-bottom:1px solid #ffffff12; font:500 15px/1.25 var(--body); }
  .ph .cp h3, .ph .sv-sub { margin:16px 0 5px; color:var(--cyan); font:700 12px/1 var(--cond); letter-spacing:.24em; text-transform:uppercase; }
  .ph .cs-actions .cs-btn:not(.big) { min-height:40px; font-size:13px; }
  .ph .cs-keys { display:grid!important; gap:6px 16px; font-size:14px; } .ph .cs-keys dt { font-weight:500; }
  .ph details summary { min-height:38px; display:flex; align-items:center; color:var(--cyan); cursor:pointer; }
  /* Saves: what do you want to do? One bright button a choice. */
  .ph .sv > .sv-sub, .ph .sv > .sv-gap, .ph .sv > .sv-local, .ph .sv > .cs-note, .ph .sv-up { display:none; }
  .ph-careercard { margin-bottom:8px; cursor:default; } .ph-careercard .ph-career-go { display:none; }
  .ph-acc { margin:6px 2px; border:1px solid var(--line); background:rgba(255,255,255,.03); }
  .ph-acc[open] { border-color:var(--cyan); background:rgba(63,224,232,.06); }
  .ph .ph-acc > summary { display:grid; grid-template-columns:32px minmax(0,1fr) auto; align-items:center; column-gap:10px; min-height:62px; padding:9px 12px; cursor:pointer; list-style:none; }
  .ph-acc > summary::-webkit-details-marker { display:none; }
  .ph-acc > summary:hover { background:rgba(63,224,232,.06); }
  .ph-acc > summary svg { grid-column:1; grid-row:1 / span 2; width:26px; height:26px; fill:none; stroke:var(--cyan); stroke-width:1.8; stroke-linecap:round; stroke-linejoin:round; }
  .ph-acc > summary b { grid-column:2; grid-row:1; font:500 16px/1.2 var(--body); color:var(--ink); }
  .ph-acc > summary small { grid-column:2; grid-row:2; margin-top:2px; color:var(--soft); font:400 13px/1.25 var(--body); }
  .ph-acc > summary i { grid-column:3; grid-row:1 / span 2; color:var(--cyan); font:500 13px/1 var(--body); }
  .ph-acc > summary i::after { content:"›"; display:inline-block; margin-left:7px; font-size:20px; line-height:0; vertical-align:-2px; transition:transform .15s; }
  .ph-acc[open] > summary i::after { transform:rotate(90deg); }
  .ph-acc-body { padding:2px 12px 14px 54px; }
  .ph-acc-cloud .ph-acc-body { padding-left:12px; }
  .ph-acc-body p { margin:0 0 10px; color:var(--soft); font:400 13px/1.45 var(--body); }
  .ph-acc-body .cs-actions { display:flex; flex-wrap:wrap; gap:8px; margin:0; }
  .ph .ph-acc-body .cs-btn { min-height:44px; padding:10px 18px; border-color:var(--line); background:none; color:var(--ink); font:500 14px/1.2 var(--body); letter-spacing:0; text-transform:none; transform:none; clip-path:none; box-shadow:none; }
  .ph .ph-acc-body .cs-btn > span { transform:none; }
  .ph .ph-acc-body .ph-main { width:100%; border-color:transparent; background:linear-gradient(100deg,#c8f9fd,#5fe4f2 45%,#1aa7d8); color:#021018; font-weight:500; }
  .ph .ph-acc-body .ph-main:hover { filter:brightness(1.07); }
  .ph .ph-acc-body .ph-quiet { min-height:36px; padding:6px 0; border:0; color:var(--cyan); text-decoration:underline; text-underline-offset:3px; }
  .ph .ph-acc-body .sv-cloud .cs-actions .cs-btn:first-child { width:100%; border-color:transparent; background:linear-gradient(100deg,#c8f9fd,#5fe4f2 45%,#1aa7d8); color:#021018; }
  .ph .ph-acc-body .sv-cloud .cs-actions .cs-btn:not(:first-child) { min-height:36px; padding:6px 0; border:0; color:var(--cyan); text-decoration:underline; text-underline-offset:3px; }
  .ph .ph-acc-body .sv-cloud .cs-actions .sv-have, .ph .ph-acc-body .sv-cloud .sv-phrase .cs-actions .cs-btn { flex:1 1 150px; width:auto; min-height:44px; padding:10px 14px; border:1px solid var(--cyan); background:none; color:var(--ink); text-decoration:none; }
  .ph .ph-acc-body .sv-cloud .cs-actions .sv-have { width:100%; flex-basis:100%; }
  .ph .ph-acc-body .sv-cloud .sv-phrase .cs-actions .sv-ack { flex-basis:100%; border-color:transparent; background:linear-gradient(100deg,#fff0c9,#ffd27a); color:#1a1000; }
  .ph-acc-cloud.ph-key > summary i { color:#ffd27a; }
  .ph .sv > .sv-msg:not(:empty) { margin:6px 2px 8px; padding:9px 12px; border-left:3px solid var(--cyan); background:rgba(63,224,232,.08); color:var(--ink); font:400 14px/1.35 var(--body); }
  .ph .sv > .sv-undo { display:block; margin:0 2px 8px; padding:4px 0; border:0; background:none; color:var(--cyan); cursor:pointer; text-align:left; font:400 13px/1.3 var(--body); text-decoration:underline; text-underline-offset:3px; }
  .ph .sv > .sv-undo[hidden] { display:none; }
  /* the hero */
  .ph-hero { min-height:0; display:flex; flex-direction:column; gap:10px; padding:14px 0 16px; }
  .ph-caption { margin-bottom:auto; text-shadow:0 2px 14px #000; }
  .ph-caption span { display:block; color:#dcebf1; font:600 12px/1.2 var(--cond); letter-spacing:.26em; text-transform:uppercase; }
  .ph-caption strong { display:block; margin-top:6px; font:italic 700 clamp(34px,4vw,58px)/1 var(--cond); letter-spacing:.03em; text-transform:uppercase; }
  .ph-caption small { display:block; margin-top:8px; color:#dcebf1; font-size:14px; }
  .ph .cs-state { margin:0; color:var(--ink); font:italic 700 13px/1.2 var(--cond); letter-spacing:.16em; text-transform:uppercase; }
  .ph .cs-actions.ph-go { display:block; margin:0; }
  .ph .cs-go.big { position:relative; isolation:isolate; display:block; width:100%; min-height:78px; padding:8px 18px; border:0; clip-path:none; transform:none; background:none; box-shadow:none; color:#021018; font:italic 700 36px/1 var(--cond); letter-spacing:.12em; text-transform:uppercase; cursor:pointer; }
  .ph .cs-go.big > span { transform:none; }
  .ph .cs-go.big::before { content:""; position:absolute; z-index:-1; inset:0; transform:skewX(var(--skew)); background:linear-gradient(100deg,#c8f9fd,#5fe4f2 45%,#1aa7d8); box-shadow:0 0 34px rgba(63,224,232,.42); transition:box-shadow .15s,filter .15s; }
  .ph .cs-go.big::after { display:none; }
  .ph .cs-go.big:hover::before { box-shadow:0 0 54px rgba(63,224,232,.7); filter:brightness(1.07); }
  .ph .cs-go.big:disabled { color:#28343b; cursor:default; } .ph .cs-go.big:disabled::before { filter:grayscale(1) brightness(.6); box-shadow:none; }
  .ph .cs-go .cs-key { color:#021018; opacity:.7; }
  .ph-info { display:flex; flex-wrap:wrap; gap:4px 24px; color:#dcebf1; font-size:13px; line-height:1.3; }
  .ph .ph-info > * { margin:0; color:inherit; font-size:13px; }
  .ph .ph-info:empty { display:none; }
  .ph-foot { flex:none; display:flex; gap:18px; padding:9px clamp(16px,4vw,64px); border-top:1px solid var(--line); color:#9db2bd; font:600 12px/1 var(--cond); letter-spacing:.12em; text-transform:uppercase; }
  .ph-foot kbd { display:inline-grid; place-items:center; min-width:24px; height:22px; padding:0 6px; margin-right:6px; border:1px solid rgba(255,255,255,.3); border-radius:3px; color:var(--ink); font:700 11px/1 var(--cond); }
  /* narrow screens and phones: one column, Play under the thumb */
  @media (max-width:820px), (pointer:coarse) and (orientation:portrait) {
    .ph-logo img { width:clamp(88px,25vw,150px); }
    .ph-top { padding:max(8px,env(safe-area-inset-top)) 12px 2px; gap:8px; }
    .ph .ph-links .cs-discord > span, .ph .ph-links .cs-support > span { font-size:0; gap:0; letter-spacing:0; }
    .ph .ph-links .cs-btn { min-height:36px; min-width:64px; padding:5px 10px; }
    .ph .ph-links .cs-count { padding:0 2px; } .ph .ph-links .cs-count b { font-size:12px; }
    .ph-tabs { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); margin:4px 12px 0; }
    .ph-tab { padding:10px 2px 9px; font-size:14px; letter-spacing:.05em; text-align:center; }
    .ph-art { inset:102px 0 auto; height:min(46vw,210px); aspect-ratio:auto; background-position:center 62%; -webkit-mask-image:linear-gradient(180deg,#000 80%,transparent); mask-image:linear-gradient(180deg,#000 80%,transparent); -webkit-mask-composite:source-over; mask-composite:add; }
    .ph:not(.on-home) .ph-art { display:none; }
    .ph-shade { display:none; }
    .ph-frame { display:flex; flex-direction:column; padding:0 12px; }
    .ph.on-home .ph-work { padding-top:calc(min(46vw,210px) + 4px); }
    .ph-work { flex:1; padding:8px 2px; }
    .ph-hero { flex:none; gap:6px; padding:8px 0 max(10px,env(safe-area-inset-bottom)); border-top:1px solid var(--line); background:#03080e; }
    .ph-caption { display:none; }
    .ph .cs-go.big { min-height:54px; font-size:26px; }
    .ph-foot, .ph .cs-go .cs-key { display:none; }
  }
  @media (pointer:coarse) { .ph .cs-local, .ph .cs-go .cs-key, .ph-foot { display:none; } }
  /* phones held sideways: settings left, Play under the right thumb */
  @media (pointer:coarse) and (orientation:landscape) {
    .ph-top { position:absolute; z-index:2; top:calc(max(2px,env(safe-area-inset-top)) + 44px); right:max(12px,env(safe-area-inset-right)); width:clamp(190px,27vw,250px); flex-direction:column-reverse; gap:4px; padding:0; }
    .ph-logo img { width:clamp(140px,21vw,190px); }
    .ph .ph-links .cs-discord > span, .ph .ph-links .cs-support > span { font-size:0; gap:0; letter-spacing:0; }
    .ph .ph-links .cs-btn { min-height:34px; min-width:64px; padding:4px 9px; }
    .ph .ph-links .cs-count { padding:0 2px; } .ph .ph-links .cs-count b { font-size:12px; }
    .ph-links { gap:6px; }
    .ph-tabs { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); margin:max(2px,env(safe-area-inset-top)) max(12px,env(safe-area-inset-right)) 0 max(12px,env(safe-area-inset-left)); }
    .ph-tab { padding:10px 2px 9px; font-size:14px; letter-spacing:.05em; text-align:center; }
    .ph-frame { grid-template-columns:minmax(0,1fr) clamp(190px,27vw,250px); column-gap:12px; padding:0 max(12px,env(safe-area-inset-right)) 0 max(12px,env(safe-area-inset-left)); }
    .ph-art { inset:0 0 0 35%; height:auto; aspect-ratio:auto; background-position:center; -webkit-mask-image:linear-gradient(90deg,transparent,#000 30%); mask-image:linear-gradient(90deg,transparent,#000 30%); -webkit-mask-composite:source-over; mask-composite:add; }
    .ph:not(.on-home) .ph-art { display:block; }
    .ph-shade { display:block; background:linear-gradient(90deg,#03080e 34%,rgba(3,8,14,.55) 60%,transparent 76%); }
    .ph:not(.on-home) .ph-shade { background:linear-gradient(90deg,#03080e 62%,rgba(3,8,14,.7) 70%,transparent 80%); }
    .ph.on-home .ph-work { padding-top:6px; }
    .ph-caption { display:none; }
    .ph-hero { justify-content:flex-end; gap:6px; padding:6px 0 max(8px,env(safe-area-inset-bottom)); }
    .ph .cs-go.big { min-height:54px; font-size:26px; }
    .ph-work { padding:6px 2px 8px; }
    .ph-page > h2 { font-size:19px; }
    .ph-pick { min-height:66px; padding:6px 12px; } .ph-pick b { font-size:18px; } .ph-pick small { font-size:12px; }
  }
  @media (pointer:coarse) {
    .ph-page > h2 { font-size:19px; } .ph-sub { margin-bottom:6px; font-size:13px; }
    .ph-choice { gap:10px; margin:5px 3px 8px; } .ph-pick { min-height:66px; padding:6px 12px; } .ph-pick b { font-size:18px; } .ph-pick small { font-size:12px; }
    .ph .cs-note { font-size:12px; } .ph .cs-label { font-size:12px; } .ph-info, .ph .ph-info > * { font-size:12px; }
    .ph-row { min-height:46px; padding:4px 8px 4px 12px; font-size:14px; } .ph-row small { font-size:12px; }
    .ph .cs-switch, .ph .cp-row { font-size:14px; }
    .ph-acc > summary { min-height:58px; padding:7px 10px; } .ph-acc > summary b { font-size:15px; } .ph-acc > summary small { font-size:12px; }
    .ph-acc-body { padding:2px 10px 12px; } .ph-acc-body p { font-size:13px; }
  }
  @media (prefers-reduced-motion:reduce) { .ph *, .ph *::before, .ph *::after { animation:none!important; transition:none!important; } }
  `;
  const style = document.createElement('style'); style.textContent = css; root.prepend(style);

  // ---- The loading screens ----
  // The player page owns the panel, its progress and its messages; this only dresses them to
  // match the page above, so pressing Play changes the panel and leaves the picture and logo where
  // they were. It goes in <head> because Play removes the start page and all it holds.
  const loading = document.createElement('style');
  loading.textContent = `
  @font-face { font-family:"Saira Condensed"; font-weight:500; font-display:swap; src:url("fonts/SairaCondensed-500.woff2") format("woff2"); }
  @font-face { font-family:"Saira Condensed"; font-weight:600; font-display:swap; src:url("fonts/SairaCondensed-600.woff2") format("woff2"); }
  @font-face { font-family:"Saira Condensed"; font-weight:700 900; font-display:swap; src:url("fonts/SairaCondensed-700.woff2") format("woff2"); }
  @font-face { font-family:"Saira"; font-weight:400 500; font-display:swap; src:url("fonts/Saira.woff2") format("woff2"); }
  #status, #runtime-loader { --c:#3fe0e8; --ink:#e9f6ff; --soft:#a9bdc8; --line:rgba(63,224,232,.2); --skew:-14deg; --pad:clamp(16px,4vw,64px); --cond:"Saira Condensed","Arial Narrow",system-ui,sans-serif; --body:"Saira","Segoe UI",system-ui,sans-serif;
    isolation:isolate; background:#03080e; color:var(--ink); font:400 15px/1.45 var(--body); }
  #status::before, #runtime-loader::before { content:""; position:fixed; z-index:-2; inset:12% 0 auto 34%; aspect-ratio:16/9;
    background:linear-gradient(90deg,#03080e 0,rgba(3,8,14,.5) 22%,transparent 46%),url("play-home/hero-carbon.webp") center/cover no-repeat;
    -webkit-mask-image:linear-gradient(180deg,transparent,#000 16%,#000 78%,transparent); mask-image:linear-gradient(180deg,transparent,#000 16%,#000 78%,transparent); }
  #status::after, #runtime-loader::after { content:""; position:fixed; z-index:-1; left:max(var(--pad),env(safe-area-inset-left)); top:max(clamp(10px,2.4vh,22px),env(safe-area-inset-top)); width:clamp(230px,23vw,340px); aspect-ratio:3;
    background:url("play-home/carbon-logo.webp") left top/contain no-repeat; filter:drop-shadow(0 4px 18px rgba(63,224,232,.22)); }
  #status { justify-content:flex-start; padding:0 max(16px,50vw) 0 max(var(--pad),env(safe-area-inset-left)); text-align:left; font:italic 700 clamp(22px,2.6vw,30px)/1.2 var(--cond); letter-spacing:.02em; text-shadow:0 2px 14px #000; }
  /* a stopped game: report.js makes this a column and darkens it, so both are set again here */
  #status:has(a) { flex-direction:column!important; justify-content:center; align-items:flex-start; gap:6px; padding-top:calc(clamp(10px,2.4vh,22px) + 110px); padding-bottom:16px; padding-right:max(16px,52vw); box-sizing:border-box; overflow:auto; pointer-events:auto; background:#03080e!important; font:400 15px/1.5 var(--body); letter-spacing:0; text-shadow:none; }
  #status .rp-row { justify-content:flex-start; }
  #status a { color:var(--c); font-weight:500; }
  #runtime-loader { place-items:center start; padding-left:max(var(--pad),env(safe-area-inset-left)); }
  #runtime-loader .cl-panel { width:min(100%,460px); margin-top:clamp(40px,9vh,90px); padding:0; background:none; border:0; box-shadow:none; }
  #runtime-loader .cl-kicker { display:flex; align-items:center; gap:10px; margin:0 0 8px; color:var(--c); font:600 13px/1 var(--cond); letter-spacing:.3em; }
  #runtime-loader .cl-kicker::before { content:""; flex:none; width:24px; height:18px; background:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 18'%3E%3Cpath d='M2 2l10 6 10-6v5L12 13 2 7zM2 9.5l10 6 10-6V13l-10 5L2 13z' fill='%233fe0e8'/%3E%3C/svg%3E") center/contain no-repeat; }
  #runtime-loader h1 { margin:0 0 8px; color:#f4fbff; font:italic 700 clamp(34px,4.2vw,54px)/1 var(--cond); letter-spacing:.01em; text-transform:uppercase; text-shadow:0 2px 20px rgba(0,0,0,.8),0 0 24px rgba(63,224,232,.2); }
  #runtime-loader #cl-message { min-height:2.9em; margin:0 0 14px; color:var(--soft); font-size:14px; line-height:1.45; }
  #runtime-loader .cl-label, #runtime-loader .cl-prepare { color:var(--ink); font:italic 600 16px/1.2 var(--cond); letter-spacing:.09em; text-transform:uppercase; }
  #runtime-loader .cl-label { margin:16px 0 8px; }
  #runtime-loader .cl-label span:last-child, #runtime-loader .cl-prepare span:last-child { color:var(--soft); font:500 13px/1.2 var(--body); letter-spacing:0; text-transform:none; font-variant-numeric:tabular-nums; }
  #runtime-loader .cl-panel progress { height:12px; background:rgba(63,224,232,.13); transform:skewX(var(--skew)); -webkit-mask-image:repeating-linear-gradient(90deg,#000 0 15px,transparent 15px 18px); mask-image:repeating-linear-gradient(90deg,#000 0 15px,transparent 15px 18px); }
  #runtime-loader .cl-panel progress::-webkit-progress-bar { background:transparent; }
  #runtime-loader .cl-panel progress::-webkit-progress-value { background:linear-gradient(90deg,#1aa7d8,#5fe4f2 62%,#c8f9fd); }
  #runtime-loader .cl-panel progress::-moz-progress-bar { background:linear-gradient(90deg,#1aa7d8,#5fe4f2 62%,#c8f9fd); }
  #runtime-loader .cl-panel progress:indeterminate { background:linear-gradient(90deg,rgba(63,224,232,.13) 20%,#5fe4f2 50%,rgba(63,224,232,.13) 80%); background-size:200% 100%; }
  #runtime-loader .cl-prepare { margin-top:18px; padding-top:13px; border-top:1px solid var(--line); }
  #runtime-loader .cl-footer { margin:12px 0 0; color:#9db2bd; font-size:13px; }
  #runtime-loader #cl-reload { min-height:44px; margin-top:14px; padding:10px 24px; border:0; background:linear-gradient(100deg,#c8f9fd,#5fe4f2 45%,#1aa7d8); color:#021018; font:italic 700 17px/1 var(--cond); letter-spacing:.06em; text-transform:uppercase; transform:skewX(var(--skew)); }
  #runtime-loader #cl-reload:hover { filter:brightness(1.07); }
  #runtime-loader #cl-reload[hidden] { display:none; }
  @media (max-width:820px), (pointer:coarse) and (orientation:portrait) {
    #status::before, #runtime-loader::before { inset:64px 0 auto; height:min(46vw,210px); aspect-ratio:auto; background:url("play-home/hero-carbon.webp") center 62%/cover no-repeat; -webkit-mask-image:linear-gradient(180deg,#000 80%,transparent); mask-image:linear-gradient(180deg,#000 80%,transparent); }
    #status::after, #runtime-loader::after { left:12px; top:max(8px,env(safe-area-inset-top)); width:clamp(120px,36vw,170px); }
    #status { align-items:flex-end; padding:0 16px max(32px,env(safe-area-inset-bottom)); }
    #status:has(a) { justify-content:flex-end; align-items:flex-start; padding:calc(min(46vw,210px) + 76px) 16px max(20px,env(safe-area-inset-bottom)); }
    #runtime-loader { place-items:end stretch; padding-left:16px; padding-right:16px; }
    #runtime-loader .cl-panel { width:100%; margin-top:0; }
    #runtime-loader h1 { font-size:34px; }
  }
  /* a phone on its side: everything in one short column beside the picture */
  @media (max-height:500px) {
    #status::before, #runtime-loader::before { inset:0 0 0 38%; height:auto; aspect-ratio:auto; background:linear-gradient(90deg,#03080e 0,rgba(3,8,14,.55) 24%,transparent 52%),url("play-home/hero-carbon.webp") center/cover no-repeat; -webkit-mask-image:none; mask-image:none; }
    #status::after, #runtime-loader::after { left:max(16px,env(safe-area-inset-left)); top:max(8px,env(safe-area-inset-top)); width:112px; }
    #status { align-items:center; padding:48px 45vw 0 max(16px,env(safe-area-inset-left)); }
    #status:has(a) { justify-content:flex-start; align-items:flex-start; padding:50px 36vw 10px max(16px,env(safe-area-inset-left)); font-size:13px; line-height:1.35; }
    #runtime-loader { place-items:center start; padding:44px 16px 6px max(16px,env(safe-area-inset-left)); }
    #runtime-loader .cl-panel { width:min(100%,380px); }
    #runtime-loader .cl-kicker { display:none; }
    #runtime-loader h1 { margin-bottom:2px; font-size:24px; }
    #runtime-loader #cl-message { min-height:0; margin-bottom:0; font-size:12px; line-height:1.3; }
    #runtime-loader .cl-label { margin:7px 0 4px; font-size:13px; }
    #runtime-loader .cl-panel progress { height:8px; }
    #runtime-loader .cl-prepare { margin-top:8px; padding-top:6px; font-size:13px; }
    #runtime-loader .cl-footer { margin-top:4px; font-size:11px; line-height:1.25; }
    #runtime-loader #cl-reload { min-height:36px; margin-top:6px; padding:6px 18px; }
  }
  `;
  document.head.append(loading);
  // Pages without the panel print raw byte counts; a percentage says the same in plain words.
  const line = document.getElementById('status');
  if (line) new MutationObserver(() => {
    const bytes = /^Downloading data\.{3}\s*\((\d+)\/(\d+)\)$/.exec(line.textContent);
    if (bytes && +bytes[2]) line.textContent = `Downloading game data… ${Math.min(100, Math.floor(bytes[1] / bytes[2] * 100))}%`;
  }).observe(line, { childList: true, characterData: true, subtree: true });

  // ---- The frame ----
  const ph = el('div', 'ph on-home');
  ph.innerHTML = `<div class="ph-art" role="img" aria-label="A red car and a blue car on a floodlit canyon bend at night, with the lights of the city across the water."></div><div class="ph-shade"></div>
    <header class="ph-top"><h1 class="ph-logo"><img src="play-home/carbon-logo.webp" alt="Need for Speed Carbon"></h1><div class="ph-links"></div></header>
    <nav class="ph-tabs" role="tablist" aria-label="Start page"></nav>
    <div class="ph-frame"><main class="ph-work"></main><aside class="ph-hero">
      <div class="ph-caption"><span>Street racing in Palmont City</span><strong>Own the city</strong><small>Build your crew. Take every territory. Settle it in the canyons.</small></div></aside></div>
    <footer class="ph-foot"><div><kbd>Enter</kbd>Play</div><div><kbd>Q E</kbd>Change tab</div></footer>`;
  const work = $('.ph-work', ph), hero = $('.ph-hero', ph), tabsBox = $('.ph-tabs', ph);
  const pages = new Map(), tabs = [];
  const go = name => {
    for (const t of tabs) t.setAttribute('aria-selected', String(t.dataset.t === name));
    for (const [id, p] of pages) p.classList.toggle('on', id === name);
    ph.classList.toggle('on-home', name === 'home'); work.scrollTop = 0;
  };
  for (const [id, label] of [['home', 'Home'], ['saves', 'Saves'], ['settings', 'Settings'], ['help', 'Help']]) {
    const t = el('button', 'ph-tab'); t.type = 'button'; t.setAttribute('role', 'tab'); t.dataset.t = id; t.textContent = label;
    t.addEventListener('click', () => go(id)); tabs.push(t); tabsBox.append(t);
    const p = el('section', 'ph-page'); p.dataset.p = id; pages.set(id, p); work.append(p);
  }
  const row = (html, to) => { const b = el('button', 'ph-row', html); b.type = 'button'; b.addEventListener('click', () => go(to)); return b; };
  const panelNamed = name => [...root.querySelectorAll('.cs-panel')].find(p => $('h2', p)?.textContent.trim() === name);

  // ---- Home ----
  const home = pages.get('home');
  home.innerHTML = `<h2>Ready to race</h2><p class="ph-sub">Choose how you want to play, then press Play.</p>
    <div class="ph-choice" role="radiogroup" aria-label="How do you want to play?">
      <button type="button" class="ph-pick" role="radio" data-online="0"><b>Play alone</b><small>Career and quick races, just you</small></button>
      <button type="button" class="ph-pick" role="radio" data-online="1"><b>Play online</b><small>Drive and race with other players</small></button></div>
    <div class="ph-online"></div>`;
  const switchOn = $('.cs-on');
  $('.ph-online', home).append($('.cs-online'));
  const careerCard = el('div', 'ph-career ph-careercard', '<span class="ph-career-label">Your career</span><strong>Looking at your save…</strong><small></small>');
  const career = el('button', 'ph-career', '<span class="ph-career-label">Your career</span><strong>Looking at your save…</strong><small></small><span class="ph-career-go">Saves ›</span>');
  career.type = 'button'; career.addEventListener('click', () => go('saves'));
  home.append(career, row('<span>New here? See how to play</span><span>›</span>', 'help'));
  const paintOnline = () => {
    const on = !!switchOn?.checked; ph.classList.toggle('is-online', on);
    for (const b of home.querySelectorAll('.ph-pick')) b.setAttribute('aria-checked', String((b.dataset.online === '1') === on));
  };
  // The real switch, so everything start.js does on a change still happens.
  home.querySelectorAll('.ph-pick').forEach(b => b.addEventListener('click', () => { if (switchOn && switchOn.checked !== (b.dataset.online === '1')) switchOn.click(); paintOnline(); }));
  switchOn?.addEventListener('change', paintOnline);

  // ---- Saves, Settings, Help: the real panels ----
  const saves = pages.get('saves');
  saves.innerHTML = '<h2>Saves</h2><p class="ph-sub">What would you like to do?</p>';
  saves.append($('.cs-ext[data-for="saves"]'));
  // saves.js looks its elements up inside its own panel, so they are rearranged there, never taken out.
  const ICON = {
    backup: '<path d="M12 3v12M7 10.5 12 15.5l5-5M4 20h16"/>',
    bring: '<path d="M12 15.5V3.5M7 8.5 12 3.5l5 5M4 15v5h16v-5"/>',
    cloud: '<path d="M7.5 18.5a4.5 4.5 0 0 1-.6-8.96A6 6 0 0 1 18.5 11a3.8 3.8 0 0 1-.5 7.5z"/>',
  };
  const choice = (key, title, note, body) => {
    const d = el('details', `ph-acc ph-acc-${key}`);
    d.name = 'ph-saves';
    d.innerHTML = `<summary><svg viewBox="0 0 24 24" aria-hidden="true">${ICON[key]}</svg><b>${title}</b><small>${note}</small><i></i></summary><div class="ph-acc-body"></div>`;
    $('.ph-acc-body', d).append(...body);
    // Its main button must be in view once it opens.
    d.addEventListener('toggle', () => { if (d.open) requestAnimationFrame(() => d.scrollIntoView({ block: 'nearest' })); });
    return d;
  };
  const arrangeSaves = view => {
    if (!view || view.dataset.ph) return;
    view.dataset.ph = '1';
    const pick = s => $(s, view);
    const copy = el('div', 'cs-actions'), down = pick('.sv-down'); if (down) { down.classList.add('ph-main'); copy.append(down); }
    const bring = el('div', 'cs-actions'), file = pick('.sv-import-file'), dir = pick('.sv-import-dir');
    if (file) { file.classList.add('ph-main'); bring.append(file); }
    if (dir) { dir.classList.add('ph-quiet'); bring.append(dir); }
    const backup = choice('backup', 'Back up my career', 'Keep a copy so you never lose it', [el('p', '', 'Saves a file to this device, with your career and your settings.'), copy]);
    const incoming = choice('bring', 'Bring in a career', 'From a backup, or from the PC game', [el('p', '', 'Choose a backup you made here, or a career from the PC game version 1.4. A .zip is fine. On Windows it is in Documents, in the NFS Carbon folder.'), bring]);
    const cloud = choice('cloud', 'Cloud save', 'Use your career on other devices', [pick('.sv-cloud')]);
    view.prepend(careerCard);
    careerCard.after(pick('.sv-msg'), pick('.sv-undo'), backup, incoming, cloud);
    paintCareer();
  };
  const settings = pages.get('settings');
  settings.innerHTML = '<h2>Settings</h2><p class="ph-sub">Changes start the next time you press Play.</p>';
  const full = $('.cs-full')?.closest('label'); if (full) { full.removeAttribute('style'); settings.append(full); }
  settings.append($('.cs-ext[data-for="preferences"]'));
  const help = pages.get('help');
  help.innerHTML = '<h2>Help</h2><p class="ph-sub">How to play, your game files and the community.</p><div class="ph-section">How to play</div>';
  const controls = panelNamed('Controls');
  if (touch) help.append(el('dl', 'cs-keys', '<dt>Steer</dt><dd>Drag your left thumb. You can also tilt the phone or use buttons</dd><dt>Gas, Brake</dt><dd>Right thumb. Slide between them without lifting</dd><dt>NOS, G</dt><dd>Left thumb: speed boost and slow motion</dd><dt>Slide</dt><dd>Handbrake, above the pedals</dd><dt>Top row</dt><dd>Steering type, camera, online, menu, pause</dd>'));
  else if (controls) help.append(controls);
  const files = panelNamed('Game files'), about = $('.cs-about');
  const local = $('.cs-local'), localStatus = $('.cs-local-status');
  if (files) { help.append(el('div', 'ph-section', 'Game files'), files); if (local) $('.cs-actions', files)?.prepend(local); if (localStatus) files.append(localStatus); }
  if (about) help.append(el('div', 'ph-section', 'Community'), about);

  // ---- Header and hero: the real Play button and status ----
  const links = $('.ph-links', ph), support = $('.cs-support'), discord = $('.cs-discord');
  const live = $('.cs-count', ph); if (live) links.append(live); // the live player count, always in view
  if (support) {
    support.hidden = false; links.append(support);
    const words = [...$('span', support).childNodes].find(n => n.nodeType === 3 && n.nodeValue.trim());
    if (words) words.nodeValue = 'Buy me a coffee';
  }
  // Local pages have no About link to copy, so the header makes its own.
  const chat = discord ? discord.cloneNode(true) : el('a', 'cs-btn cs-discord', `<span><svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z"/></svg>Discord</span>`);
  chat.href = 'https://discord.gg/UP3fhwbB2'; chat.target = '_blank'; chat.rel = 'noopener'; chat.hidden = false; chat.setAttribute('aria-label', 'Join us on Discord');
  links.append(chat);
  // The leaderboards page sits at the site's root, beside the game (/leaderboards/).
  const boards = el('a', 'cs-btn cs-boards', '<span>Leaderboards</span>');
  boards.href = '/leaderboards/'; boards.setAttribute('aria-label', 'Leaderboards: the fastest players');
  links.insertBefore(boards, chat);
  const go2 = el('div', 'cs-actions ph-go'); go2.append($('.cs-go'));
  const info = el('div', 'ph-info');
  for (const s of ['.cs-mode', '.cs-slot']) { const e = $(s); if (e) info.append(e); }
  const state = $('.cs-state'); if (state) { state.hidden = false; hero.append(state); }
  hero.append(go2, info);

  // ---- Plain words for what the other files draw, whenever they draw it ----
  const WORDS = new Map([['Download backup', 'Download a copy'], ['Choose a career file or zip', 'Choose a save file'], ['Choose the NFS Carbon folder', 'Or choose the whole NFS Carbon folder'], ['Frame rate', 'Smoothness'],
    ['Frame counter', 'Show speed of the game'], ['Texture filtering', 'Sharpness of road and walls'], ['How much you see to the sides', 'How wide you see'],
    ['UI size %', 'Menu size %'], ['Console controller layout', 'Console-style buttons'], ['Button names', 'Controller button names'],
    ['Ghost mode (see-through cars)', "Drive through other players' cars"], ['Full screen when the game starts', 'Start in full screen'],
    ['Your name', 'Your driver name'], ['Room', 'Private room (optional)'], ['Name others see', 'The name other players see'], ['Empty: everyone on the server', 'Leave empty to meet everyone']]);
  const plain = () => {
    arrangeSaves($('.sv', ph));
    const w = document.createTreeWalker(work, NodeFilter.SHOW_TEXT);
    while (w.nextNode()) { const t = WORDS.get(w.currentNode.nodeValue.trim()); if (t) w.currentNode.nodeValue = t; }
    for (const i of work.querySelectorAll('input[placeholder]')) { const t = WORDS.get(i.placeholder); if (t) i.placeholder = t; }
    paintCareer();
  };

  // The career card says one plain thing: which career, and when it was saved.
  const paintCareer = () => {
    const line = $('.sv-local'), action = $('.ph-career-go', career);
    if (!line) return;
    const names = [...($('b', line)?.textContent || '').matchAll(/“([^”]+)”/g)].map(m => m[1]);
    const saved = /saved (.+)$/.exec(line.textContent)?.[1];
    const title = names.length ? names.join(', ') : 'No career yet';
    const note = names.length ? (saved ? `Last saved ${saved}` : 'Saved in this browser') : 'Press Play to start one, or bring it from the PC game.';
    const go = names.length ? 'Saves ›' : 'Import ›';
    for (const card of [career, careerCard]) {
      const big = $('strong', card), small = $('small', card);
      if (big.textContent !== title) big.textContent = title;
      if (small.textContent !== note) small.textContent = note;
    }
    if (action.textContent !== go) action.textContent = go;
    const back = $('.ph-acc-backup', ph); if (back) back.hidden = !names.length; // nothing to back up yet
    paintCloud();
  };

  // The keyphrase is the only way back to a cloud save, so a new one is never left behind a closed section.
  let keyShown = false;
  const paintCloud = () => {
    const box = $('.ph-acc-cloud', ph); if (!box) return;
    const on = !!$('.sv-cloud-state'), unsaved = !!$('.sv-unsaved');
    const state = unsaved ? 'Write it down' : on ? 'On' : '';
    const note = unsaved ? 'Your keyphrase is not written down yet' : on ? 'Your keyphrase is here. You need it on other devices' : 'Use your career on other devices';
    const label = $('summary i', box), small = $('summary small', box);
    if (label.textContent !== state) label.textContent = state;
    if (small.textContent !== note) small.textContent = note;
    box.classList.toggle('ph-key', unsaved);
    if (unsaved && !keyShown && !box.open) box.open = true;
    keyShown = unsaved;
  };

  // ---- In ----
  const wrap = $('.cs-wrap');
  if (wrap) wrap.hidden = true;
  root.append(ph);
  go('home'); paintOnline(); plain();
  new MutationObserver(plain).observe(work, { subtree: true, childList: true, characterData: true });
  const poll = setInterval(() => { if (!root.isConnected) return clearInterval(poll); paintOnline(); }, 400); // start.js restores the switch without an event
  addEventListener('keydown', e => {
    if (!root.isConnected || e.target.closest?.('input,select,textarea,dialog')) return;
    const k = e.key.toLowerCase(), at = tabs.findIndex(t => t.getAttribute('aria-selected') === 'true');
    if (k === 'q') go(tabs[(at + tabs.length - 1) % tabs.length].dataset.t);
    if (k === 'e') go(tabs[(at + 1) % tabs.length].dataset.t);
  });
})();
