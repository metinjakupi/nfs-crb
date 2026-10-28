// start.js - Need for Speed: Carbon's start page. The player page loads it
// when its address carries no developer settings, and this page starts the
// game (window.recompBoot) when the player presses Play.
//
// Everything the address used to carry is kept in this browser instead:
// the player's name, online on or off (free roam with others and the game's
// own Online mode, Custom Match), the room, the server.
// A second window of the game on the same computer gets its own save slot by
// itself (a Web Lock per slot), so two players can share one computer.
//
// The look follows the game's menus. The "Need for Speed Carbon" logo and the
// night sky behind the page are read from the player's own copy of the game
// (GLOBAL/UniqueBootTextures.bin, GLOBAL/GLOBALB.BUN), so no art of the game's
// is part of this file or of the site.
"use strict";
(() => {
  const game = window.recompGame || { id: "nfsc", executable: "NFSC.exe" };
  let recovery = null;
  const recoveryReady = new Promise(resolve => {
    let finished = false;
    const script = document.createElement("script");
    script.src = "resume.js";
    const done = loaded => {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      if (loaded) recovery = new CarbonRecovery(game);
      resolve();
    };
    const timeout = setTimeout(() => done(false), 5000);
    script.onload = () => done(true);
    script.onerror = () => done(false); // a previous deployment can still start normally
    document.body.append(script);
  });
  const SERVERS = {
    internet: { label: "Internet", url: "wss://drive.metinjakupi.dev", key: "" },
    local: { label: "This computer (testing)", url: "ws://127.0.0.1:57254", key: "" },
  };
  // online.js, beside this page, is written by tools/carbon-relay-deploy.sh with
  // the internet server's address and key:
  //   window.recompServers = { internet: { url: "wss://...", key: "..." } };
  // It holds the key, so it is not in git; without it (a 404) the defaults above
  // stay. SERVERS is read when it is needed (player count, Play, cloud saves);
  // the first player count, Play and the start page's add-ons wait for this.
  const serversReady = new Promise((resolve) => {
    let late = false;
    const script = document.createElement("script");
    script.src = "online.js";
    script.onload = () => {
      const s = window.recompServers?.internet;
      if (s && typeof s.url === "string" && /^wss?:\/\/\S+$/i.test(s.url)) SERVERS.internet.url = s.url.replace(/\/+$/, "");
      if (s && typeof s.key === "string") SERVERS.internet.key = s.key;
      resolve();
      if (late) { siteOnly(); count(); } // the count shown so far was for the address above
    };
    script.onerror = () => { script.remove(); resolve(); };
    setTimeout(() => { late = true; resolve(); }, 4000); // a server that never answers must not hold up the page
    document.body.append(script);
  });
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
  .cs { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --blue: #1f86c8; --deep: #0b3a66;
        --violet: #6a2a8a; --panel: rgba(6, 12, 22, .78); --line: rgba(95, 228, 242, .22);
        position: fixed; inset: 0; z-index: 10; overflow-y: auto; color: var(--ink);
        font: 15px/1.45 "Avenir Next", "Segoe UI", system-ui, sans-serif;
        background: radial-gradient(120% 70% at 50% 0%, #142a4a 0%, #070c16 55%, #020409 100%); }
  .cs::before { content: ""; position: fixed; left: 0; right: 0; top: 0; height: 72vh; pointer-events: none; opacity: 0;
        background: var(--sky, none) center bottom / cover no-repeat; transition: opacity 1.2s ease;
        -webkit-mask-image: linear-gradient(#000 0%, #000 55%, transparent 100%); mask-image: linear-gradient(#000 0%, #000 55%, transparent 100%); }
  .cs.sky::before { opacity: 1; }
  .cs::after { content: ""; position: fixed; inset: 0; pointer-events: none; opacity: .5;
        background: repeating-linear-gradient(45deg, rgba(255,255,255,.018) 0 2px, transparent 2px 4px),
                    repeating-linear-gradient(-45deg, rgba(0,0,0,.25) 0 2px, transparent 2px 4px); }
  .cs * { box-sizing: border-box; }
  .cs-wrap { position: relative; z-index: 1; max-width: 1180px; margin: 0 auto; padding: 28px 16px 40px; }
  .cs-head { display: flex; align-items: flex-end; justify-content: space-between; gap: 16px; flex-wrap: wrap; margin-bottom: 26px; }
  .cs-logo { margin: 0; width: min(520px, 100%); aspect-ratio: 4 / 1; display: grid; align-items: center; }
  .cs-logo canvas { width: 100%; height: auto; filter: drop-shadow(0 0 18px rgba(106, 42, 138, .55)); }
  .cs-word { font: italic 900 clamp(34px, 7vw, 64px)/.9 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", "Arial Narrow", sans-serif;
        letter-spacing: .04em; text-transform: uppercase; background: linear-gradient(#bff6ff, #3fb6e6 55%, #1360a8);
        -webkit-background-clip: text; background-clip: text; color: transparent; }
  .cs-word small { display: block; font-size: .32em; letter-spacing: .3em; color: var(--ink); -webkit-text-fill-color: var(--ink); }
  .cs-tag { color: var(--dim); font-size: 12px; letter-spacing: .18em; text-transform: uppercase; }
  .cs-grid { display: grid; grid-template-columns: minmax(280px, .78fr) minmax(0, 1.22fr); gap: 18px; }
  .cs-col { display: grid; gap: 18px; align-content: start; }
  .cs-panel { position: relative; background: var(--panel); border: 1px solid var(--line); padding: 18px 20px 20px;
        clip-path: polygon(0 0, calc(100% - 18px) 0, 100% 18px, 100% 100%, 18px 100%, 0 calc(100% - 18px));
        backdrop-filter: blur(6px); -webkit-backdrop-filter: blur(6px); }
  .cs-panel::before, .cs-panel::after { content: ""; position: absolute; width: 26px; height: 1px; background: var(--line);
        transform-origin: left center; transform: rotate(45deg); }
  .cs-panel::before { left: calc(100% - 19px); top: -1px; }
  .cs-panel::after { left: -1px; top: calc(100% - 19px); }
  .cs-panel h2 { margin: 0 0 14px; font: italic 800 15px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
        letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); display: flex; align-items: center; gap: 10px; }
  .cs-panel h2::after { content: ""; flex: 1; height: 2px; transform: skewX(-30deg);
        background: linear-gradient(90deg, var(--cyan), transparent); opacity: .5; }
  .cs-play { display: grid; gap: 14px; padding: 26px 24px 24px; }
  .cs-state { display: flex; align-items: center; gap: 10px; font-weight: 600; }
  .cs-dot { width: 10px; height: 10px; border-radius: 50%; background: var(--dim); box-shadow: 0 0 10px currentColor; }
  .cs-state.ready .cs-dot { background: var(--cyan); color: var(--cyan); }
  .cs-state.bad .cs-dot { background: #ff5d7a; color: #ff5d7a; }
  .cs-note { color: var(--dim); font-size: 13px; }
  .cs-mode { color: var(--ink); font-size: 14px; }
  .cs-mode b { color: var(--cyan); font-weight: 600; }
  .cs-btn { font: italic 800 15px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .14em;
        text-transform: uppercase; color: var(--ink); background: rgba(95, 228, 242, .06); border: 1px solid var(--line);
        padding: 11px 18px; cursor: pointer; transform: skewX(-12deg); transition: background .15s ease, border-color .15s ease, box-shadow .15s ease;
        text-decoration: none; display: inline-block; }
  .cs-btn > span { display: inline-block; transform: skewX(12deg); }
  .cs-btn:hover { background: rgba(95, 228, 242, .14); border-color: var(--cyan); }
  .cs-btn:focus-visible { outline: none; box-shadow: 0 0 0 2px var(--cyan); }
  .cs-btn.cs-discord { color: #fff; background: #5865f2; border-color: #8c95ff; min-height: 44px; }
  .cs-btn.cs-discord > span { display: inline-flex; align-items: center; gap: 9px; }
  .cs-discord svg { width: 22px; height: 22px; fill: currentColor; flex: none; }
  .cs-btn.cs-discord:hover { background: #4752c4; border-color: #b3b9ff; }
  .cs-btn.cs-discord:active { background: #3c45a5; }
  .cs-btn.cs-discord:focus-visible { outline: 2px solid #fff; outline-offset: 3px; }
  .cs-btn:disabled { opacity: .4; cursor: default; }
  .cs-btn.big { font-size: 30px; padding: 18px 34px; color: #021018; border-color: transparent;
        background: linear-gradient(100deg, #9ff3fb, var(--cyan) 40%, #2aa4dc); box-shadow: 0 0 28px rgba(95, 228, 242, .35); }
  .cs-btn.big:hover { box-shadow: 0 0 40px rgba(95, 228, 242, .6); background: linear-gradient(100deg, #c8f9fd, #7deaf5 40%, #36b2e6); }
  .cs-btn.big:disabled { box-shadow: none; filter: grayscale(1); }
  .cs-state[hidden], .cs-tag[hidden] { display: none; }
  .cs-key { font: 600 11px/1 system-ui, sans-serif; letter-spacing: .06em; opacity: .7; margin-left: 10px; vertical-align: middle; }
  .cs-row { display: grid; gap: 6px; margin-bottom: 14px; }
  .cs-row:last-child { margin-bottom: 0; }
  .cs-label { font-size: 12px; letter-spacing: .14em; text-transform: uppercase; color: var(--dim); }
  .cs input[type=text], .cs select { font: inherit; color: var(--ink); background: rgba(0, 0, 0, .45); border: 1px solid var(--line);
        padding: 9px 11px; width: 100%; border-radius: 0; outline: none; }
  .cs input[type=text]:focus, .cs select:focus { border-color: var(--cyan); box-shadow: 0 0 0 1px var(--cyan); }
  .cs-switch { display: flex; align-items: center; justify-content: space-between; gap: 12px; cursor: pointer; margin-bottom: 14px; }
  .cs-switch input { appearance: none; -webkit-appearance: none; width: 46px; height: 24px; flex: none; margin: 0; cursor: pointer;
        background: rgba(255, 255, 255, .1); border: 1px solid var(--line); transform: skewX(-12deg); position: relative; transition: background .15s ease; }
  .cs-switch input::after { content: ""; position: absolute; top: 3px; left: 3px; width: 16px; height: 16px; background: var(--dim); transition: left .15s ease, background .15s ease; }
  .cs-switch input:checked { background: rgba(95, 228, 242, .25); border-color: var(--cyan); }
  .cs-switch input:checked::after { left: 25px; background: var(--cyan); }
  .cs-switch input:focus-visible { box-shadow: 0 0 0 2px var(--cyan); }
  .cs-online[data-off] .cs-when-on { opacity: .45; }
  .cs-count { font-size: 13px; color: var(--dim); margin-top: 10px; }
  .cs-count b { color: var(--cyan); }
  .cs details summary { cursor: pointer; color: var(--dim); font-size: 13px; }
  .cs details[open] summary { margin-bottom: 10px; }
  .cs-keys { display: grid; grid-template-columns: auto 1fr; gap: 6px 14px; font-size: 13px; margin: 0; }
  .cs-keys dt { color: var(--cyan); font-weight: 700; white-space: nowrap; }
  .cs-keys dd { margin: 0; color: var(--ink); }
  .cs-actions { display: flex; flex-wrap: wrap; gap: 10px; }
  .cs-foot { margin-top: 26px; color: var(--dim); font-size: 12px; text-align: center; }
  .cs-foot-links { margin-bottom: 8px; font-size: 13px; letter-spacing: .06em; }
  .cs-foot-links a { color: var(--cyan); }
  .cs-dialog { position: fixed; inset: 0; z-index: 20; display: grid; place-items: center; padding: 16px; background: rgba(0, 2, 6, .7); }
  .cs-dialog .cs-panel { width: min(440px, 100%); padding: 24px; }
  .cs-dialog p { margin: 0 0 16px; color: var(--ink); }
  .cs-dialog .cs-actions { margin-top: 18px; }
  .cs [hidden] { display: none !important; }
  .cs-ext:empty { display: none; }
  .cs-credits { margin: 0; padding-left: 18px; display: grid; gap: 5px; color: var(--dim); font-size: 13px; }
  .cs-credits a, .cs-licences a, .cs-community a { color: var(--cyan); }
  @media (max-width: 820px) {
    .cs-grid { grid-template-columns: 1fr; } .cs-col { display: contents; } .cs-btn.big { width: 100%; }
    .cs-panel { order: 3; } .cs-panel.cs-play { order: 1; } .cs-panel.cs-online { order: 2; } .cs-panel.cs-about { order: 4; }
  }
  @media (prefers-reduced-motion: reduce) { .cs::before, .cs-btn, .cs-switch input, .cs-switch input::after { transition: none; } }
  `;

  const root = document.createElement("div");
  root.className = "cs";
  root.innerHTML = `
  <style>${css}</style>
  <div class="cs-wrap">
    <header class="cs-head">
      <h1 class="cs-logo"><span class="cs-word"><small>Need for Speed</small>Carbon</span></h1>
      <div class="cs-tag">Browser edition</div>
    </header>
    <div class="cs-grid">
      <div class="cs-col">
        <section class="cs-panel cs-play">
          <div class="cs-state"><span class="cs-dot"></span><span class="cs-state-text">Checking your game files…</span></div>
          <div><button class="cs-btn big cs-go" disabled><span>Play<span class="cs-key">Enter</span></span></button></div>
          <div class="cs-mode"></div>
          <div class="cs-note cs-slot"></div>
        </section>
        <section class="cs-panel">
          <h2>Game files</h2>
          <div class="cs-note cs-files">…</div>
          <div class="cs-actions" style="margin-top:12px">
            <a class="cs-btn cs-manage" href="${game.streamAssets ? "../manage.html" : "../"}"><span>${game.downloadManifest ? "Manage game files" : "Import or manage"}</span></a>
          </div>
        </section>
        <div class="cs-ext" data-for="saves"></div>
        <section class="cs-panel">
          <h2>Controls</h2>
          <dl class="cs-keys">
            <dt>Options → Controls</dt><dd>change keys and controller buttons, in the game</dd>
            <dt>Esc</dt><dd>pause, menus back</dd>
            <dt>F9 or F10</dt><dd>the menu: settings, full screen, online, quit (fn+F9 on a Mac)</dd>
            <dt>Controller</dt><dd>PS5, PS4 or Xbox: press any button. The touchpad (or Create + Options) opens the menu</dd>
            <dt>Y</dt><dd>quick chat, when online</dd>
            <dt>H</dt><dd>horn, hold it, when online</dd>
          </dl>
          <label class="cs-switch" style="margin:14px 0 0"><span>Full screen when the game starts</span><input type="checkbox" class="cs-full"></label>
        </section>
      </div>
      <div class="cs-col">
        <section class="cs-panel cs-online">
          <h2>Online</h2>
          <label class="cs-switch"><span>Play online</span><input type="checkbox" class="cs-on"></label>
          <div class="cs-note" style="margin:-6px 0 14px">Drive with other players. Choose Online → Custom Match in the game to race together.</div>
          <div class="cs-when-on">
            <label class="cs-row"><span class="cs-label">Your name</span>
              <input type="text" class="cs-name" maxlength="20" autocomplete="nickname" spellcheck="false" placeholder="Name others see"></label>
            <label class="cs-row"><span class="cs-label">Crew</span>
              <input type="text" class="cs-crew" maxlength="16" autocomplete="off" spellcheck="false" placeholder="Optional: same name = same crew"></label>
            <label class="cs-row"><span class="cs-label">Room</span>
              <input type="text" class="cs-room" maxlength="40" autocomplete="off" spellcheck="false" placeholder="Empty: everyone on the server"></label>
            <div class="cs-note">Friends who type the same room name drive together. Leave it empty to meet everyone.
              Custom Match races are with anyone on the server.</div>
            <details style="margin-top:12px"><summary>Server</summary>
              <label class="cs-row"><span class="cs-label">Server</span><select class="cs-server"></select></label>
            </details>
            <div class="cs-count"></div>
          </div>
        </section>
        <div class="cs-ext" data-for="preferences"></div>
        <section class="cs-panel cs-about">
          <h2>About</h2>
          <div class="cs-note">Play solo or join other drivers online. Your saves stay on this device.</div>
          <div class="cs-actions" style="margin-top:12px">
            <a class="cs-btn cs-discord" href="https://discord.gg/UP3fhwbB2" target="_blank" rel="noopener"><span><svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M20.317 4.3698a19.7913 19.7913 0 00-4.8851-1.5152.0741.0741 0 00-.0785.0371c-.211.3753-.4447.8648-.6083 1.2495-1.8447-.2762-3.68-.2762-5.4868 0-.1636-.3933-.4058-.8742-.6177-1.2495a.077.077 0 00-.0785-.037 19.7363 19.7363 0 00-4.8852 1.515.0699.0699 0 00-.0321.0277C.5334 9.0458-.319 13.5799.0992 18.0578a.0824.0824 0 00.0312.0561c2.0528 1.5076 4.0413 2.4228 5.9929 3.0294a.0777.0777 0 00.0842-.0276c.4616-.6304.8731-1.2952 1.226-1.9942a.076.076 0 00-.0416-.1057c-.6528-.2476-1.2743-.5495-1.8722-.8923a.077.077 0 01-.0076-.1277c.1258-.0943.2517-.1923.3718-.2914a.0743.0743 0 01.0776-.0105c3.9278 1.7933 8.18 1.7933 12.0614 0a.0739.0739 0 01.0785.0095c.1202.099.246.1981.3728.2924a.077.077 0 01-.0066.1276 12.2986 12.2986 0 01-1.873.8914.0766.0766 0 00-.0407.1067c.3604.698.7719 1.3628 1.225 1.9932a.076.076 0 00.0842.0286c1.961-.6067 3.9495-1.5219 6.0023-3.0294a.077.077 0 00.0313-.0552c.5004-5.177-.8382-9.6739-3.5485-13.6604a.061.061 0 00-.0312-.0286zM8.02 15.3312c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9555-2.4189 2.157-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.9555 2.4189-2.1569 2.4189zm7.9748 0c-1.1825 0-2.1569-1.0857-2.1569-2.419 0-1.3332.9554-2.4189 2.1569-2.4189 1.2108 0 2.1757 1.0952 2.1568 2.419 0 1.3332-.946 2.4189-2.1568 2.4189Z"/></svg>Join Discord</span></a>
            <a class="cs-btn cs-source" href="https://github.com/metinjakupi/nfs-crb" target="_blank" rel="noopener"><span>Source code on GitHub</span></a>
          </div>
          <div class="cs-note cs-community" style="margin-top:12px">Find players, ask for help, or report a bug in <a href="https://discord.com/channels/1565102068673286255/1565102215327387658" target="_blank" rel="noopener">#bug-reports</a> after joining. Include your browser, graphics card, and steps to reproduce the problem. Use F9 → Send report for game logs.</div>
          <h2 style="margin-top:18px">Credits</h2>
          <ul class="cs-credits">
            <li><a href="https://github.com/ThirteenAG/WidescreenFixesPack" target="_blank" rel="noopener">ThirteenAG</a> — widescreen improvements.</li>
            <li><a href="https://github.com/veritr1x/populous-recomp" target="_blank" rel="noopener">veritr1x</a> — porting tools.</li>
            <li><a href="https://github.com/elforeign/nfs-most-wanted-mac" target="_blank" rel="noopener">elforeign</a> — the Most Wanted Mac port.</li>
            <li><a href="https://github.com/th0mk/nfsu2-web" target="_blank" rel="noopener">th0mk</a>, with Talha Müderrisoğlu — online play.</li>
            <li><a href="https://github.com/xan1242/NFS-XtendedInput" target="_blank" rel="noopener">Lovro Plese (Xan/Tenjoin) and Berkay Yigit</a>,
              controller support.</li>
            <li><a href="https://github.com/nlgxzef/Darius" target="_blank" rel="noopener">MaxHwoy / nlgxzef</a>, visual improvements.</li>
          </ul>
          <div class="cs-note cs-licences" style="margin-top:8px" hidden><a href="/notice.txt" target="_blank" rel="noopener">All credits and licences</a></div>
        </section>
      </div>
    </div>
    <div class="cs-foot">Fan project. Not affiliated with or endorsed by Electronic Arts. "Need for Speed" and "Carbon" are trademarks of Electronic Arts Inc.</div>
  </div>
  <div class="cs-dialog" hidden>
    <form class="cs-panel" method="dialog">
      <h2>Play online?</h2>
      <p>Choose a name for online play.</p>
      <label class="cs-row"><span class="cs-label">Your name</span>
        <input type="text" class="cs-ask-name" maxlength="20" autocomplete="nickname" spellcheck="false" required></label>
      <div class="cs-actions">
        <button class="cs-btn big cs-yes" type="submit"><span>Turn on</span></button>
        <button class="cs-btn cs-no" type="button"><span>Not now</span></button>
      </div>
    </form>
  </div>`;
  document.body.append(root);
  // Keep player controls visible on the same homepage as Play.
  if (game.streamAssets) {
    root.querySelector(".cs-tag").hidden = true;
    root.querySelector(".cs-state").hidden = true;
    const [left, right] = root.querySelectorAll(".cs-col");
    right.prepend(root.querySelector('.cs-ext[data-for="saves"]'));
    left.append(root.querySelector(".cs-about"));
    right.append(root.querySelector(".cs-files").closest("section"));
  }
  const $ = (sel) => root.querySelector(sel);

  // Which save slot this window uses: the first free one. Slot 1 is the
  // game's own profile; others start as a copy of it (the player page makes it).
  let slot = 1;
  const slotReady = (async () => {
    await recoveryReady;
    if (recovery) { slot = await recovery.acquireSlot(); return; }
    if (!navigator.locks) return;
    for (let n = 1; n <= 4; n++) {
      const got = await new Promise((resolve) => {
        navigator.locks.request(`${game.id}-slot-${n}`, { ifAvailable: true }, (lock) => {
          resolve(!!lock);
          return lock ? new Promise(() => {}) : undefined; // held while this window is open
        });
      });
      if (got) { slot = n; return; }
    }
    slot = 0;
  })();

  const nameKey = () => `name.${slot}`;
  const online = store.get("online", true); // New players start online; keep a saved choice.
  const on = $(".cs-on"), nameEl = $(".cs-name"), roomEl = $(".cs-room"), serverEl = $(".cs-server"), fullEl = $(".cs-full");
  const crewEl = $(".cs-crew");
  for (const [id, s] of Object.entries(SERVERS)) serverEl.append(new Option(s.label, id));
  on.checked = !!online;
  roomEl.value = store.get("room", "");
  crewEl.value = store.get("crew", "");
  serverEl.value = SERVERS[store.get("server", "internet")] ? store.get("server", "internet") : "internet";
  fullEl.checked = store.get("fullscreen", false);
  // On the public site (tools/carbon-site-deploy.sh sets window.recompSite in its copy of
  // online.js) the player is not on this Mac: the Internet server is the only one, and the
  // site's notice.txt (all credits and licences) is linked. Nothing changes on the Mac.
  function siteOnly() {
    if (!window.recompSite || !SERVERS.local) return;
    delete SERVERS.local;
    serverEl.querySelector('option[value="local"]')?.remove();
    serverEl.value = "internet";
    serverEl.closest("details").hidden = true;
    $(".cs-licences").hidden = false;
    // The guide and FAQ pages (tools/site/), and the site's own title (the player page sets the
    // game's; this one is the same as tools/site/seo-head.html's).
    document.title = "NFS Carbon Online – Need for Speed: Carbon multiplayer in your browser";
    const links = document.createElement("div");
    links.className = "cs-foot-links";
    links.innerHTML = '<a href="/online/">How to play online</a> · <a href="/faq/">FAQ</a>';
    $(".cs-foot").prepend(links);
    reflect();
  }
  serversReady.then(siteOnly);
  // The panel greys out what only matters online, and Play says how you'll play.
  function reflect() {
    $(".cs-online").toggleAttribute("data-off", !on.checked);
    const mode = $(".cs-mode"), name = nameEl.value.trim(), room = roomEl.value.trim(), crew = crewEl.value.trim();
    mode.replaceChildren();
    if (!on.checked) { mode.textContent = "Single player"; return; }
    const b = document.createElement("b");
    b.textContent = name || "a name you'll choose";
    mode.append("Online as ", b, crew ? ` · crew "${crew}"` : "", room ? ` · room "${room}"` : " · everyone on the server",
                serverEl.value === "internet" ? "" : ` · ${SERVERS[serverEl.value].label.toLowerCase()}`);
  }
  reflect();

  slotReady.then(() => {
    nameEl.value = store.get(nameKey(), "");
    reflect();
    $(".cs-slot").textContent = !slot ? "Four game windows are already open. Close one to play here." : slot > 1
      ? `This is window ${slot} on this computer: it plays with its own save, a copy of the main one.`
      : "";
    serversReady.then(count);
  });

  on.addEventListener("change", () => { store.set("online", on.checked); reflect(); count(); });
  nameEl.addEventListener("input", reflect);
  roomEl.addEventListener("input", reflect);
  nameEl.addEventListener("change", () => store.set(nameKey(), nameEl.value.trim()));
  roomEl.addEventListener("change", () => { store.set("room", roomEl.value.trim()); count(); });
  // A crew name keeps only what the relay keeps (letters, digits, spaces, _ . -),
  // so the player sees here the name everyone else will see. Saved as it is typed,
  // so a click on Play right after typing already uses it.
  crewEl.addEventListener("input", () => {
    const v = crewEl.value, at = crewEl.selectionStart ?? v.length, clean = (s) => s.replace(/[^\p{L}\p{N} _.-]/gu, "");
    if (clean(v) !== v) {
      crewEl.value = clean(v);
      const p = clean(v.slice(0, at)).length;
      crewEl.setSelectionRange(p, p);
    }
    store.set("crew", crewEl.value.trim());
    reflect();
  });
  crewEl.addEventListener("change", () => { crewEl.value = crewEl.value.trim(); store.set("crew", crewEl.value); });
  serverEl.addEventListener("change", () => { store.set("server", serverEl.value); reflect(); count(); });
  fullEl.addEventListener("change", () => store.set("fullscreen", fullEl.checked));

  // For the start page's add-ons (saves.js draws its panel into .cs-ext[data-for=saves]):
  // this window's save slot and the folder the player page will use for it.
  // After online.js, so server() already has the internet server's key.
  Promise.all([slotReady, serversReady]).then(() => {
    window.recompStartPage = {
      slot, store, panel: (id) => root.querySelector(`.cs-ext[data-for="${id}"]`),
      profileFolder: slot > 1 ? `profile-${slot}` : "profile", server: () => SERVERS[serverEl.value],
      gameFolder, entry,
      async prepareProfile() {
        await (await navigator.storage.getDirectory()).getDirectoryHandle(game.id, { create: true });
        return profileFolder(game.id, slot > 1 ? String(slot) : null);
      },
    };
    dispatchEvent(new Event("recomp-start-page"));
    const saves = document.createElement("script");
    saves.src = "saves.js";
    saves.onerror = () => saves.remove();
    document.body.append(saves);
    if (game.streamAssets) {
      const preferences = document.createElement("script");
      preferences.src = "home-settings.js";
      document.body.append(preferences);
    }
  });

  // How many are on the server now (the relay's /stats).
  let counting = 0;
  async function count() {
    const el = $(".cs-count"), mine = ++counting;
    if (!on.checked) { el.textContent = ""; return; }
    const server = SERVERS[serverEl.value];
    const http = server.url.replace(/^ws/, "http");
    try {
      const r = await fetch(`${http}/stats?room=${encodeURIComponent(roomEl.value.trim())}`, { cache: "no-store" });
      const s = await r.json();
      if (mine !== counting) return;
      el.innerHTML = "";
      const b = document.createElement("b");
      b.textContent = `${s.players} ${s.players === 1 ? "player" : "players"}`;
      el.append("Online now: ", b, roomEl.value.trim() ? ` · ${s.here} in your room` : "");
    } catch {
      if (mine === counting) el.textContent = "Online play is unavailable. You can still play solo.";
    }
  }

  // The game's files in this browser.
  let ready = false;
  async function gameFolder() {
    let dir = await navigator.storage.getDirectory();
    for (const p of [game.id, "game"]) dir = await dir.getDirectoryHandle(p);
    return dir;
  }
  async function entry(dir, ...parts) { // without regard to case
    for (const part of parts) {
      let found = null;
      for await (const [name, handle] of dir.entries())
        if (name.toLowerCase() === part.toLowerCase()) { found = handle; break; }
      if (!found) return null;
      dir = found;
    }
    return dir;
  }
  const filesReady = (async () => {
    const stateEl = $(".cs-state"), text = $(".cs-state-text"), files = $(".cs-files");
    if (game.streamAssets) {
      ready = true;
      stateEl.classList.add("ready");
      text.textContent = "Ready to play";
      files.textContent = "Files load automatically.";
      $(".cs-go").disabled = false;
      hostedLogo().catch(error => console.warn("[start] logo:", error));
      return;
    }
    try {
      const dir = await gameFolder();
      await dir.getFileHandle(game.executable);
      const stamp = await (await (await dir.getFileHandle(".stamp")).getFile()).text();
      if (game.sha256 && stamp.trim() !== game.sha256) throw new Error("Game files are incomplete");
      ready = true;
      stateEl.classList.add("ready");
      text.textContent = "Ready";
      files.textContent = "The game is ready in this browser.";
      $(".cs-go").disabled = false;
      art(dir);
    } catch {
      stateEl.classList.add("bad");
      text.textContent = "Game files needed";
      files.textContent = game.downloadManifest
        ? "Download the game once from the launcher. This browser keeps completed files so an interrupted download can continue."
        : "Import Need for Speed: Carbon (the folder it is installed in) once; this browser keeps it.";
    }
  })();

  // ---- The game's own art, from the player's files ----
  function chunks(d, o, end, each) {
    while (o + 8 <= end) {
      const id = d.getUint32(o, true), size = d.getUint32(o + 4, true);
      if (each(id, o + 8, size) === false) return;
      o += 8 + size;
    }
  }
  function findTexture(buf, wanted) {
    const d = new DataView(buf), u8 = new Uint8Array(buf);
    let out = null;
    chunks(d, 0, u8.length, (cid, o, s) => {
      if (cid !== 0xb3300000) return;
      let info = null, fmt = null, data = -1;
      chunks(d, o, o + s, (c, p, sz) => {
        if (c === 0xb3310000) chunks(d, p, p + sz, (c2, p2, s2) => {
          if (c2 === 0x33310004) info = [p2, s2];
          else if (c2 === 0x33310005) fmt = [p2, s2];
        });
        else if (c === 0xb3320000) chunks(d, p, p + sz, (c2, p2) => { if (c2 === 0x33320002) data = (p2 + 0x7f) & ~0x7f; });
      });
      if (!info || !fmt || data < 0) return;
      for (let p = info[0], end = info[0] + info[1], i = 0; p + 0x59 <= end; i++) {
        const n = u8[p + 0x58];
        let name = "";
        for (let k = 0; k < n && u8[p + 0x59 + k]; k++) name += String.fromCharCode(u8[p + 0x59 + k]);
        if (name === wanted) {
          const off = d.getUint32(p + 20, true), size = d.getUint32(p + 28, true);
          const f = fmt[0] + 24 * i + 12;
          out = { w: d.getUint16(p + 40, true), h: d.getUint16(p + 42, true),
                  fmt: String.fromCharCode(u8[f], u8[f + 1], u8[f + 2], u8[f + 3]),
                  raw: u8.subarray(data + off, data + off + size) };
          return false;
        }
        p += 0x59 + n;
      }
      return out ? false : undefined;
    });
    return out;
  }
  function decode(t) {
    const { w, h, raw } = t, px = new Uint8ClampedArray(w * h * 4);
    if (t.fmt === "\x15\0\0\0") {
      for (let i = 0; i < w * h; i++) {
        px[4 * i] = raw[4 * i + 2]; px[4 * i + 1] = raw[4 * i + 1]; px[4 * i + 2] = raw[4 * i]; px[4 * i + 3] = raw[4 * i + 3];
      }
    } else if (t.fmt === "DXT1") {
      const rgb = (c) => [(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31];
      let o = 0;
      for (let by = 0; by < h; by += 4) for (let bx = 0; bx < w; bx += 4, o += 8) {
        const c0 = raw[o] | raw[o + 1] << 8, c1 = raw[o + 2] | raw[o + 3] << 8;
        const idx = (raw[o + 4] | raw[o + 5] << 8 | raw[o + 6] << 16 | raw[o + 7] << 24) >>> 0;
        const a = rgb(c0), b = rgb(c1);
        const cols = c0 > c1 ? [a, b, a.map((x, k) => (2 * x + b[k]) / 3), a.map((x, k) => (x + 2 * b[k]) / 3)]
                             : [a, b, a.map((x, k) => (x + b[k]) / 2), [0, 0, 0]];
        for (let i = 0; i < 16; i++) {
          const x = bx + (i & 3), y = by + (i >> 2);
          if (x >= w || y >= h) continue;
          const k = idx >>> (2 * i) & 3, q = 4 * (y * w + x), c = cols[k];
          px[q] = c[0]; px[q + 1] = c[1]; px[q + 2] = c[2]; px[q + 3] = c0 <= c1 && k === 3 ? 0 : 255;
        }
      }
    } else {
      return null;
    }
    const canvas = document.createElement("canvas");
    canvas.width = w; canvas.height = h;
    canvas.getContext("2d").putImageData(new ImageData(px, w, h), 0, 0);
    return canvas;
  }
  async function texture(dir, path, name) {
    const handle = await entry(dir, ...path);
    if (!handle) return null;
    const t = findTexture(await (await handle.getFile()).arrayBuffer(), name);
    return t && decode(t);
  }
  async function hostedLogo() {
    if (!game.downloadManifest) return;
    const url = new URL("files/GLOBAL/UniqueBootTextures.bin", game.downloadManifest);
    const response = await fetch(url);
    if (!response.ok) return;
    const texture = findTexture(await response.arrayBuffer(), "NFS07_LOGO");
    const logo = texture && decode(texture);
    if (!logo || !root.isConnected) return;
    logo.setAttribute("role", "img");
    logo.setAttribute("aria-label", "Need for Speed Carbon");
    $(".cs-logo").replaceChildren(logo);
  }
  async function art(dir) {
    try {
      const logo = await texture(dir, ["GLOBAL", "UniqueBootTextures.bin"], "NFS07_LOGO");
      if (logo) {
        logo.setAttribute("role", "img");
        logo.setAttribute("aria-label", "Need for Speed Carbon");
        $(".cs-logo").replaceChildren(logo);
      }
      const sky = await texture(dir, ["GLOBAL", "GLOBALB.BUN"], "SKY_NIGHT_BASE_01_D");
      if (sky) sky.toBlob((blob) => {
        if (!blob) return;
        root.style.setProperty("--sky", `url("${URL.createObjectURL(blob)}")`);
        root.classList.add("sky");
      }, "image/jpeg", 0.9);
    } catch (error) {
      console.warn("[start] the game's art could not be read:", error);
    }
  }

  // ---- Play ----
  const dialog = $(".cs-dialog"), askName = $(".cs-ask-name");
  function ask() {
    askName.value = nameEl.value || store.get("name.1", "") || "";
    dialog.hidden = false;
    askName.focus();
    askName.select();
  }
  $(".cs-dialog form").addEventListener("submit", (e) => {
    e.preventDefault();
    const name = askName.value.trim();
    if (!name) { askName.focus(); return; }
    nameEl.value = name;
    store.set(nameKey(), name);
    on.checked = true;
    store.set("online", true);
    reflect();
    dialog.hidden = true;
    play();
  });
  $(".cs-no").addEventListener("click", () => {
    on.checked = false;
    store.set("online", false);
    reflect();
    dialog.hidden = true;
    play();
  });

  let starting = false;
  async function play(restoring = false) {
    if (!ready || starting || $(".cs-go").disabled || window.recompStartPage?.homeBusy) return;
    await slotReady;
    await serversReady;
    if (!slot || starting) return;
    try { await window.recompStartPage?.flushSettings?.(); } catch { return; }
    const name = nameEl.value.trim();
    if (on.checked && !name) { ask(); return; }
    starting = true;
    if (!restoring && fullEl.checked && document.documentElement.requestFullscreen)
      document.documentElement.requestFullscreen().catch(() => {});
    const server = SERVERS[serverEl.value];
    const room = roomEl.value.trim() || "lobby";
    const key = server.key ? `?key=${encodeURIComponent(server.key)}` : "";
    const relay = `${server.url}/${encodeURIComponent(room)}${key}`;
    await recovery?.begin({ online: on.checked, name, room: roomEl.value.trim(),
      crew: crewEl.value.trim(), server: serverEl.value }, restoring);
    root.remove();
    // Online gives both: free roam with others (net.js, the relay room) and the game's own
    // Online mode (lan.js: the server's virtual LAN at /lan; the game signs in under the name).
    window.recompBoot({
      profile: slot > 1 ? String(slot) : null,
      online: on.checked ? { relay, name, room: roomEl.value.trim(), crew: crewEl.value.trim() } : null,
      lan: on.checked ? { url: `${server.url}/lan${key}`, name } : null,
    });
    for (const src of ["menu.js", "chat.js", "race.js", "worldmap.js", "report.js"]) { // the in-game menu (F9), quick chat and horn, the radar, racing together, friends on the world map, Download log / Send report
      const script = document.createElement("script");
      script.src = src;
      document.body.append(script);
    }
  }
  $(".cs-go").addEventListener("click", () => play());
  Promise.all([slotReady, serversReady, filesReady]).then(() => {
    if (!slot) { $(".cs-go").disabled = true; return; }
    const saved = recovery?.pending?.config;
    if (!saved || !ready) return;
    // Use this tab's previous room/name, even if another window changed the
    // homepage preferences. The server address/key always comes from online.js.
    if (!SERVERS[saved.server]) { recovery.clear(); return; }
    on.checked = saved.online;
    nameEl.value = saved.name;
    roomEl.value = saved.room;
    crewEl.value = saved.crew;
    serverEl.value = saved.server;
    reflect();
    play(true);
  });
  addEventListener("keydown", function enter(e) {
    if (!root.isConnected) { removeEventListener("keydown", enter); return; }
    if (e.key !== "Enter" || !dialog.hidden || e.target.closest?.("input, select, button, a, summary, textarea")) return;
    e.preventDefault();
    play();
  });
})();
