// report.js - "Download log" and "Send report" for Need for Speed: Carbon in the browser.
//
// The player page (index.html) keeps every line the game prints in window.recompLog and,
// when the game stops, what stopped it in window.recompFailure. (On this Mac the page also
// posts a failure and ten minutes of loading snapshots to the private server's
// /_runtime-diagnostics, which writes browser-runtime-diagnostics.jsonl; the public site drops
// those posts.) This file turns that into a text a player can save or send:
// - the F9 menu (menu.js's window.recompMenuActions) gets "Download log" and "Send a report";
// - a "game stopped" screen gets the same two buttons (index.html calls
//   window.recompReport.offer, loading this file first if the start page has not);
// - Send report first says in one line what goes (the log, the browser and graphics card name,
//   the player's name, and their own note if they write one), then posts it as text/plain to
//   the online server's POST /reports (server/src/reports.rs; the Rust server only) with the
//   start page's server address and key. The server keeps no address.
// The text: a header (time, page without its query, player name, browser, GPU, screen, what
// stopped, the player's note), the game's log (newest lines kept when it is long), then the
// page scripts' own messages. Server keys in addresses (?key=...) are hidden. A sent report
// stays under the server's 512 KiB; a downloaded log keeps up to 16 MB.
"use strict";
(() => {
  if (window.recompReport) return;
  const reporterBuild = window.recompReportBuild;
  const game = window.recompGame || { id: "nfsc", title: "Need for Speed: Carbon" };
  const SEND_MAX = 500 * 1024, SAVE_MAX = 16 * 1024 * 1024;

  // ---- The page scripts' messages (the game's own lines are in recompLog already) ----
  const pageLines = [];
  for (const level of ["log", "info", "warn", "error"]) {
    const original = console[level];
    console[level] = function (...args) {
      try {
        const log = window.recompLog;
        const own = args.length === 1 && log && log.length && args[0] === log[log.length - 1];
        if (!own) {
          const text = args.map((a) => (a instanceof Error ? a.stack || a.message : typeof a === "string" ? a : safeJson(a))).join(" ");
          pageLines.push(`${new Date().toISOString().slice(11, 23)} ${level} ${text.slice(0, 2000)}`);
          if (pageLines.length > 1500) pageLines.splice(0, pageLines.length - 1500);
        }
      } catch {}
      return original.apply(this, args);
    };
  }
  function safeJson(v) {
    try { return JSON.stringify(v); } catch { return String(v); }
  }
  const hideKeys = (s) => String(s)
    .replace(/([?&]key=)[^&\s"'<>]+/gi, "$1(hidden)")
    .replace(/(\bkey["']?\s*[:=]\s*["'])[^"']+/gi, "$1(hidden)");

  // ---- What the report says ----
  function playerName() {
    const fromGame = window.recompOnline?.name || window.recompLan?.name;
    if (fromGame) return String(fromGame);
    try {
      const slot = window.recompStartPage?.slot || 1;
      const v = JSON.parse(localStorage.getItem(`${game.id}.name.${slot}`) || "null");
      if (v) return String(v);
    } catch {}
    return "(none)";
  }
  async function gpuName() {
    let g = window.carbonGraphicsInfo;
    if (!g) {
      try {
        const a = await navigator.gpu?.requestAdapter({ powerPreference: "high-performance" });
        g = a ? (a.info || {}) : null;
      } catch {}
    }
    if (!g) return navigator.gpu ? "no WebGPU adapter" : "no WebGPU in this browser";
    return [g.vendor, g.architecture, g.device, g.description].filter(Boolean).join(" / ") || "unknown";
  }
  function browserName() {
    const d = navigator.userAgentData;
    const brands = d?.brands?.filter((b) => !/Not.?A.?Brand/i.test(b.brand)).map((b) => `${b.brand} ${b.version}`).join(", ");
    return `${brands || "?"}${d?.platform ? ` on ${d.platform}` : ""} (${navigator.userAgent})`;
  }
  function stopped() {
    const f = window.recompFailure;
    if (f) return `${f.kind}: ${f.message}`;
    const status = document.getElementById("status");
    if (status && !status.hidden && status.textContent.trim()) return status.textContent.trim().split("\n")[0];
    return window.Module ? "no (sent while the game was running)" : "no (sent before the game started)";
  }
  const encoder = new TextEncoder();
  const bytes = (s) => encoder.encode(s).length;
  async function build(budget, note) {
    const canvas = document.getElementById("canvas");
    const head = [
      `${game.title || "Need for Speed: Carbon"} in the browser: problem report`,
      `Made: ${new Date().toISOString()}`,
      `Page: ${location.origin}${location.pathname}`,
      `Page build: ${window.recompPageBuild?.id || "unknown (unstamped page)"}`,
      `Runtime build: ${window.recompRuntimeBuild?.id || "unknown (not loaded or unstamped)"}`,
      `Reporter build: ${reporterBuild?.id || "unknown (unstamped reporter)"}`,
      `Runtime wasm SHA-256: ${window.recompRuntimeBuild?.wasm || "unknown"}`,
      `Player name: ${playerName()}`,
      `Browser: ${browserName()}`,
      `GPU: ${await gpuName()}`,
      `Screen: canvas ${canvas?.width || 0}x${canvas?.height || 0}, window ${innerWidth}x${innerHeight}, ` +
        `display ${screen.width}x${screen.height}, pixel ratio ${devicePixelRatio}; isolated ${crossOriginIsolated}`,
      `Stopped: ${stopped()}`,
      `Startup phase: ${window.recompStartupPhase || "unknown"}`,
    ];
    const f = window.recompFailure;
    if (f?.stack) head.push("Stack:", ...String(f.stack).split("\n").slice(0, 30).map((l) => "  " + l));
    if (window.carbonTestAssets) head.push(`Game files: ${safeJson(window.carbonTestAssets)}`);
    if (note) head.push("", "Player's note:", ...note.slice(0, 2000).split("\n").map((l) => "  " + l));
    const page = pageLines.slice(-400);
    let pageText = page.join("\n");
    while (bytes(pageText) > budget / 6 && page.length) { page.shift(); pageText = page.join("\n"); }
    const log = (window.recompLog || []).map((l) => String(l));
    let room = budget - bytes(head.join("\n")) - bytes(pageText) - 400;
    const kept = [];
    for (let i = log.length - 1; i >= 0 && room > 0; i--) {
      const line = log[i].length > 4000 ? log[i].slice(0, 4000) + " …" : log[i];
      room -= bytes(line) + 1;
      if (room >= 0) kept.push(line);
    }
    kept.reverse();
    const text = [
      ...head,
      "",
      `--- The game's log: ${kept.length === log.length ? `all ${log.length}` : `the last ${kept.length} of ${log.length}`} lines ---`,
      ...kept,
      "",
      `--- The page's messages: the last ${page.length} ---`,
      pageText,
      "",
    ].join("\n");
    return hideKeys(text);
  }

  // ---- Download log ----
  async function download() {
    const text = await build(SAVE_MAX, "");
    const stamp = new Date().toISOString().replace(/[-:]/g, "").replace("T", "-").slice(0, 15);
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "text/plain;charset=utf-8" }));
    a.download = `nfs-carbon-log-${stamp}.txt`;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    return a.download;
  }

  // ---- Send report ----
  function server() {
    const s = (() => { try { return window.recompStartPage?.server?.(); } catch { return null; } })() || window.recompServers?.internet;
    if (!s?.url) return null;
    try {
      const url = new URL(String(s.url).replace(/^ws(s?):/i, "http$1:"));
      return { base: `${url.protocol}//${url.host}`, key: url.searchParams.get("key") || s.key || "" };
    } catch { return null; }
  }
  async function send(note) {
    const s = server();
    if (!s) return { ok: false, text: "This page has no online server to send to. Use Download log instead." };
    const body = await build(SEND_MAX, note);
    let r;
    try {
      r = await fetch(`${s.base}/reports${s.key ? `?key=${encodeURIComponent(s.key)}` : ""}`, {
        method: "POST", body, headers: { "Content-Type": "text/plain;charset=UTF-8" },
        credentials: "omit", cache: "no-store", signal: AbortSignal.timeout(30000),
      });
    } catch {
      return { ok: false, text: "The report could not be sent: the server did not answer. Use Download log instead." };
    }
    if (r.status === 201) {
      const id = await r.json().then((j) => j.id).catch(() => "");
      return { ok: true, id, text: `Sent, thank you. Report ${id || "received"}.` };
    }
    const wait = Number(r.headers.get("retry-after")) || 0;
    const text = {
      429: `A report was sent from here a short while ago. Try again in about ${Math.max(1, Math.ceil(wait / 60))} min, or use Download log.`,
      507: "The server has no room for more reports right now. Use Download log instead.",
      413: "The report is too big to send. Use Download log instead.",
      403: "The server did not accept this page's key. Reload the page, then try again.",
    }[r.status] || "This server does not take reports. Use Download log instead.";
    return { ok: false, text };
  }

  // ---- The dialog ----
  const css = `
  .rp { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --bad: #ff7a90; --line: rgba(95, 228, 242, .3);
        position: fixed; inset: 0; z-index: 40; display: grid; place-items: center; padding: 16px; pointer-events: auto;
        background: rgba(0, 2, 6, .78); font: 15px/1.45 "Avenir Next", "Segoe UI", system-ui, sans-serif; color: var(--ink); text-align: left; }
  .rp * { box-sizing: border-box; }
  .rp-box { width: min(480px, 100%); background: rgba(6, 12, 22, .96); border: 1px solid var(--line); padding: 22px; }
  .rp h2 { margin: 0 0 12px; font: italic 800 15px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
        letter-spacing: .2em; text-transform: uppercase; color: var(--cyan); }
  .rp p { margin: 0 0 12px; }
  .rp .rp-what { color: var(--dim); font-size: 13.5px; }
  .rp textarea { width: 100%; min-height: 76px; resize: vertical; font: inherit; color: var(--ink); background: rgba(0, 0, 0, .45);
        border: 1px solid var(--line); padding: 8px 10px; outline: none; }
  .rp textarea:focus { border-color: var(--cyan); }
  .rp-row { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 14px; justify-content: center; pointer-events: auto; }
  .rp-btn { font: italic 800 13px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif; letter-spacing: .12em;
        text-transform: uppercase; color: var(--ink); background: #0b1626; border: 1px solid var(--line);
        padding: 10px 14px; cursor: pointer; }
  .rp-btn:hover, .rp-btn:focus-visible { border-color: var(--cyan); background: rgba(95, 228, 242, .16); outline: none; }
  .rp-btn.go { color: #021018; background: var(--cyan); border-color: var(--cyan); }
  .rp-btn:disabled { opacity: .5; cursor: default; }
  .rp-say { margin-top: 12px; font-size: 14px; }
  .rp-say.bad { color: var(--bad); }
  .rp-stopped { font: 14px/1.4 system-ui, sans-serif; color: #ddd; }
  .rp-stopped .rp-say { text-align: center; }`;
  let style = null;
  function addStyle() {
    if (style) return;
    style = document.createElement("style");
    style.textContent = css;
    document.head.append(style);
  }
  const make = (tag, cls, text) => {
    const el = document.createElement(tag);
    if (cls) el.className = cls;
    if (text != null) el.textContent = text;
    return el;
  };
  const what = () => `It sends this game's log, your browser and graphics card name, and your player name (${playerName()}),` +
    " plus what you write here. Nothing else, and the server keeps no address.";

  let dialog = null;
  function openDialog() {
    addStyle();
    dialog?.remove();
    dialog = make("div", "rp");
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-label", "Send a report");
    const box = make("div", "rp-box");
    const note = make("textarea");
    note.placeholder = "What happened? (optional)";
    note.maxLength = 2000;
    const say = make("div", "rp-say");
    const sendBtn = make("button", "rp-btn go", "Send report");
    const saveBtn = make("button", "rp-btn", "Download log");
    const closeBtn = make("button", "rp-btn", "Close");
    for (const b of [sendBtn, saveBtn, closeBtn]) b.type = "button";
    const row = make("div", "rp-row");
    row.append(sendBtn, saveBtn, closeBtn);
    box.append(make("h2", "", "Send a report"), make("p", "rp-what", what()), note, row, say);
    dialog.append(box);
    // Keys typed here never reach the game (its listeners are on the window, bubbling).
    for (const type of ["keydown", "keypress", "keyup"]) dialog.addEventListener(type, (e) => {
      e.stopPropagation();
      if (type === "keydown" && e.key === "Escape") closeDialog();
    });
    sendBtn.onclick = async () => {
      sendBtn.disabled = true;
      say.className = "rp-say";
      say.textContent = "Sending…";
      const r = await send(note.value.trim());
      say.className = r.ok ? "rp-say" : "rp-say bad";
      say.textContent = r.text;
      sendBtn.disabled = r.ok;
    };
    saveBtn.onclick = async () => {
      const name = await download();
      say.className = "rp-say";
      say.textContent = `Saved ${name} to your downloads.`;
    };
    closeBtn.onclick = closeDialog;
    document.body.append(dialog);
    note.focus();
  }
  function closeDialog() {
    dialog?.remove();
    dialog = null;
    document.getElementById("canvas")?.focus();
  }
  // While the dialog is open, a key aimed elsewhere (the game's canvas) goes nowhere.
  for (const type of ["keydown", "keypress"]) addEventListener(type, (e) => {
    if (dialog && !dialog.contains(e.target)) {
      e.preventDefault();
      e.stopImmediatePropagation();
      if (type === "keydown" && e.key === "Escape") closeDialog();
      else dialog.querySelector("textarea")?.focus();
    }
  }, true);

  // On a "game stopped" screen (index.html's status line with its Back link).
  function offer(statusEl) {
    if (!statusEl) return;
    addStyle();
    statusEl.querySelector(".rp-stopped")?.remove();
    // The message, the Back link and these buttons one under another, over a darkened game.
    Object.assign(statusEl.style, { flexDirection: "column", gap: "4px", background: "rgba(0, 2, 6, .8)" });
    const wrap = make("div", "rp-stopped");
    const row = make("div", "rp-row");
    const saveBtn = make("button", "rp-btn", "Download log");
    const sendBtn = make("button", "rp-btn", "Send report");
    saveBtn.type = sendBtn.type = "button";
    const say = make("div", "rp-say");
    saveBtn.onclick = async () => { say.textContent = `Saved ${await download()} to your downloads.`; };
    sendBtn.onclick = openDialog;
    row.append(saveBtn, sendBtn);
    wrap.append(row, say);
    statusEl.append(wrap);
  }

  window.recompReport = { build, download, send, openDialog, offer };
  const actions = (window.recompMenuActions ||= []);
  actions.push(
    { id: "report-log", group: "Report a problem", label: "Download log", hint: "a text file of what the game wrote", run: () => { download(); } },
    { id: "report-send", group: "Report a problem", label: "Send a report", hint: "the log, to the game's server", run: openDialog },
  );
})();
