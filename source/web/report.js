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
  const rendererEvents = [];
  let rendererResets = 0, rendererFaults = 0;
  function recordRendererEvent(kind, extra = {}) {
    const canvas = document.getElementById('canvas');
    rendererEvents.push({at:new Date().toISOString(),kind,...extra,
      windowWidth:innerWidth,windowHeight:innerHeight,canvasWidth:canvas?.width,canvasHeight:canvas?.height,
      visibility:document.visibilityState});
    if (rendererEvents.length > 80) rendererEvents.shift();
  }
  function recordRendererLine(text) {
    const reset = /\bd3d9: Reset to (\d+)x(\d+)/.exec(text);
    if (reset) { ++rendererResets; recordRendererEvent('device reset',{width:Number(reset[1]),height:Number(reset[2])}); }
    // The presenter can latch warnings, so this counts reports, not every stall.
    if (text.includes('presenter: FAULT: target pool stalled')) { ++rendererFaults; recordRendererEvent('presentation stall'); }
  }
  addEventListener('resize',() => recordRendererEvent('window resize'));
  document.addEventListener?.('fullscreenchange',() => recordRendererEvent('fullscreen change',{fullscreen:!!document.fullscreenElement}));
  for (const level of ["log", "info", "warn", "error"]) {
    const original = console[level];
    console[level] = function (...args) {
      try {
        if (args.length === 1 && typeof args[0] === 'string') {
          recordRendererLine(args[0]);
          if (args[0].startsWith(CRASH)) setTimeout(() => checkpoint(true));
          if (args[0].includes('audio sink:')) window.recompPerformance?.audio(args[0]);
        }
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
    .replace(/([?&](?:key|token|claim|pk)=)[^&\s"'<>]+/gi, "$1(hidden)")
    .replace(/(\b(?:key|token|claim|pk)["']?\s*[:=]\s*["'])[^"']+/gi, "$1(hidden)");

  // Bounded, passive observations. Browser animation timing is NOT game FPS.
  const samples = [], longTasks = [], inputHistory = [], errors = [];
  let lastAnimation = null, inputSignature = "";
  const now = () => performance.now();
  function shortError(error) {
    if (error == null) return { message: String(error) };
    return { name: String(error.name || typeof error), message: String(error.message || safeJson(error) || error).slice(0, 2000),
      stack: String(error.stack || "").slice(0, 5000) };
  }
  function rememberError(kind, error, extra = {}) {
    errors.push({ at: new Date().toISOString(), kind, ...shortError(error), ...extra });
    if (errors.length > 20) errors.shift();
    if (sessionKey) setTimeout(() => checkpoint(true)); // a tab that dies next keeps what stopped it
  }
  addEventListener("error", e => rememberError("error", e.error || e.message || "Resource failed to load",
    { source: cleanUrl(e.filename || e.target?.src || e.target?.href), line: e.lineno || 0, column: e.colno || 0 }), true);
  addEventListener("unhandledrejection", e => rememberError("rejection", e.reason));
  function cleanUrl(value) {
    try { const u = new URL(value, location.href); return u.origin + u.pathname; } catch { return ""; }
  }
  function animationTick(at) {
    if (!document.hidden) {
      if (lastAnimation !== null && at > lastAnimation) samples.push({ at, ms: at - lastAnimation });
      while (samples.length && (samples.length > 1800 || samples[0].at < at - 30000)) samples.shift();
      lastAnimation = at;
    } else lastAnimation = null;
    requestAnimationFrame(animationTick);
  }
  if (typeof requestAnimationFrame === "function") requestAnimationFrame(animationTick);
  document.addEventListener?.("visibilitychange", () => { lastAnimation = null; recordRendererEvent('visibility change'); });
  try {
    const observer = new PerformanceObserver(list => {
      for (const e of list.getEntries()) longTasks.push({ at: e.startTime, ms: e.duration });
      while (longTasks.length > 60) longTasks.shift();
    });
    observer.observe({ type: "longtask", buffered: true });
  } catch {}
  function controllerSnapshot() {
    try { return Array.from(navigator.getGamepads?.() || []).filter(Boolean).slice(0, 8).map(g => ({
      id: String(g.id).slice(0, 180), index: g.index, connected: g.connected, mapping: g.mapping,
      timestamp: g.timestamp, axes: Array.from(g.axes || []).slice(0, 16).map(v => Number.isFinite(v) ? Math.round(v * 1000) / 1000 : null),
      buttons: Array.from(g.buttons || []).slice(0, 24).map(b => ({ pressed: !!b.pressed, value: Math.round((Number(b.value) || 0) * 1000) / 1000 })),
    })); } catch (e) { return { unavailable: shortError(e) }; }
  }
  function recordInput() {
    const pads = controllerSnapshot();
    const signature = safeJson(Array.isArray(pads) ? pads.map(({ timestamp, ...g }) => g) : pads);
    if (signature === inputSignature) return;
    inputSignature = signature;
    inputHistory.push({ at: new Date().toISOString(), pads });
    if (inputHistory.length > 24) inputHistory.shift();
  }
  function timingSnapshot() {
    const recent = samples.filter(s => s.at >= now() - 30000).map(s => s.ms).sort((a, b) => a - b);
    const percentile = q => recent.length ? Math.round(recent[Math.min(recent.length - 1, Math.floor(recent.length * q))] * 100) / 100 : null;
    return { source: "browser animation callbacks; not rendered game FPS", samples: recent.length,
      p50ms: percentile(.5), p95ms: percentile(.95), p99ms: percentile(.99), maxMs: percentile(1),
      gapsOver50ms: recent.filter(ms => ms > 50).length,
      longTasks: longTasks.filter(t => t.at >= now() - 30000), visibility: document.visibilityState };
  }
  async function storageSnapshot() {
    if (!navigator.storage?.estimate) return { unavailable: true };
    let timer;
    try { return await Promise.race([
      navigator.storage.estimate().then(e => ({ quota: e.quota ?? null, usage: e.usage ?? null,
        usageDetails: e.usageDetails || null, free: Number.isFinite(e.quota) && Number.isFinite(e.usage) ? Math.max(0, e.quota - e.usage) : null })),
      new Promise(resolve => { timer = setTimeout(() => resolve({ timedOut: true }), 1500); }),
    ]); } catch (e) { return { unavailable: shortError(e) }; }
    finally { clearTimeout(timer); }
  }

  // ---- Where a game thread crashed ----
  // The runtime's crash-capture.js (a --pre-js of CarbonRecomp.js) prints one "[crash] {json}" line
  // into the game's log when a thread traps: its own stack (wasm-function[N] frames) and the guest's
  // register file (crash_probe.cpp). The release's CarbonRecomp.js.symbols ("N:name" per line) names
  // the frames; a release that does not publish it keeps the numbers, which
  // tools/carbon-crash-symbolize.mjs names later from the build's own symbols file.
  const CRASH = "[crash] {";
  function crashRecords(lines) {
    const out = [];
    for (const l of lines || []) {
      const text = String(l);
      if (!text.startsWith(CRASH)) continue;
      try { const c = JSON.parse(text.slice(8)); if (c && typeof c === "object") out.push(c); } catch {}
      if (out.length >= 8) break;
    }
    return out;
  }
  const symbolTables = new Map();
  function symbolTable(source) {
    let url;
    try { url = new URL("CarbonRecomp.js.symbols", source || location.href).href; } catch { return Promise.resolve(null); }
    if (!symbolTables.has(url)) symbolTables.set(url, (async () => {
      try {
        const r = await fetch(url, { cache: "no-cache", credentials: "omit", signal: AbortSignal.timeout?.(8000) });
        if (!r.ok) return { url: cleanUrl(url), missing: `HTTP ${r.status}` };
        const text = await r.text();
        if (!/^\d+:/.test(text)) return { url: cleanUrl(url), missing: "not a symbols file" };
        const names = new Map();
        for (const line of text.split("\n")) {
          const i = line.indexOf(":");
          if (i > 0) names.set(Number(line.slice(0, i)), line.slice(i + 1).trim());
        }
        return { url: cleanUrl(url), names };
      } catch (e) { return { url: cleanUrl(url), missing: String(e?.name || e) }; }
    })());
    return symbolTables.get(url);
  }
  // One line per frame: "wasm N 0xOFFSET name" for wasm frames, the JS frame otherwise.
  function crashFrames(stack, table) {
    const frames = [];
    for (const raw of String(stack || "").split("\n")) {
      const line = raw.trim();
      if (!line || /^(?:Uncaught )?\w*Error\b/.test(line)) continue;
      const m = /wasm-function\[(\d+)\](?::(0x[0-9a-f]+))?/i.exec(line);
      if (m) {
        const n = Number(m[1]);
        const own = /^at ([^\s(]+) \(/.exec(line)?.[1] || /^([^@\s]+)@/.exec(line)?.[1] || "";
        const name = table?.names?.get(n) || (own && !own.startsWith("wasm") && !own.includes("wasm-function") ? own : "?");
        frames.push(`wasm ${n}${m[2] ? " " + m[2] : ""} ${name}`);
      } else frames.push(line.replace(/^at /, "js ").slice(0, 300));
      if (frames.length >= 64) break;
    }
    return frames;
  }
  async function crashSection(records) {
    if (!records.length) return [];
    const out = ["Crash location (the crashed thread's own stack, newest frame first; wasm N = wasm-function[N]):"];
    for (const c of records.slice(0, 4)) {
      const table = /wasm-function\[\d+\]/.test(String(c.stack)) ? await symbolTable(c.source) : null;
      const frames = crashFrames(c.stack, table);
      out.push(`  ${c.where || "?"}${c.thread ? ` (pthread ${Number(c.thread).toString(16)})` : ""} at ${c.at || "?"}: ` +
        `${c.name ? c.name + ": " : ""}${String(c.message || "").slice(0, 300)}`);
      if (c.guest) out.push(`  Guest: ${String(c.guest).slice(0, 1500)}`);
      if (Number(c.memoryBytes) > 0) out.push(`  WebAssembly memory then: ${c.memoryBytes} bytes`);
      out.push(`  Names: ${!table ? "none needed" : table.names ? `from ${table.url}` : `not available (${table.missing}, ${table.url}); name the numbers with tools/carbon-crash-symbolize.mjs`}`);
      if (!frames.length) out.push("  (the browser gave no stack)");
      frames.forEach((f, i) => out.push(`  #${i} ${f}`));
    }
    return out;
  }

  // ---- The last lines, kept in case the tab itself dies ----
  // start.js marks a running game in localStorage (its key) and clears the mark on a clean exit;
  // this keeps the end of the log beside it (key + ".tail"), every 10 s while the log grows, when
  // the tab is hidden and right after an error or a crash line. A tab the browser kills (out of
  // memory, a renderer crash) leaves both, and the next start page offers to send them.
  let sessionKey = "", sessionTimer = 0, sessionLines = -1, sessionAt = 0;
  const TAIL_MAX = 96 * 1024;
  function sessionSnapshot(logLines, pageKeep) {
    const log = (window.recompLog || []).map(String);
    return { version: 1, at: new Date().toISOString(), page: location.origin + location.pathname,
      pageBuild: window.recompPageBuild?.id || "", runtimeBuild: window.recompRuntimeBuild?.id || "",
      wasm: window.recompRuntimeBuild?.wasm || "", reporterBuild: reporterBuild?.id || "",
      phase: window.recompStartupPhase || "", stopped: stopped(), failure: window.recompFailure || null,
      wasmMemoryBytes: window.Module?.HEAPU8?.buffer?.byteLength ?? globalThis.wasmMemory?.buffer?.byteLength ?? null, visibility: document.visibilityState,
      race: raceSnapshot(), raceHistory: raceHistory.slice(-8), errors: errors.slice(-6), crashes: crashRecords(log),
      logLines: log.length, log: log.filter(l => !l.startsWith(CRASH)).slice(-logLines).map(l => l.slice(0, 400)),
      pageLines: pageLines.slice(-pageKeep).map(l => l.slice(0, 400)) };
  }
  function checkpoint(force) {
    if (!sessionKey) return;
    const lines = window.recompLog?.length || 0;
    if (!force && lines === sessionLines && Date.now() - sessionAt < 60000) return;
    sessionLines = lines; sessionAt = Date.now();
    try {
      let keep = 160, text;
      do { text = safeJson(sessionSnapshot(keep, keep / 2)); keep >>= 1; } while (text.length > TAIL_MAX && keep >= 10);
      if (text.length <= TAIL_MAX) localStorage.setItem(sessionKey + ".tail", text);
    } catch { /* storage full or blocked: the mark alone still says the tab died */ }
  }
  function watchSession(key) {
    if (!key) return;
    sessionKey = String(key);
    clearInterval(sessionTimer);
    sessionTimer = setInterval(() => checkpoint(false), 10000);
    checkpoint(true);
  }
  function endSession() {
    const key = sessionKey;
    sessionKey = "";
    clearInterval(sessionTimer);
    try { if (key) localStorage.removeItem(key + ".tail"); } catch {}
  }
  // The tail a previous page left under key (start.js's mark), removed as it is read.
  function takeSession(key) {
    let tail = null;
    try {
      const text = localStorage.getItem(String(key) + ".tail");
      localStorage.removeItem(String(key) + ".tail");
      const parsed = text && text.length <= TAIL_MAX * 2 ? JSON.parse(text) : null;
      if (parsed?.version === 1 && Array.isArray(parsed.log)) tail = parsed;
    } catch {}
    return tail;
  }
  document.addEventListener?.("visibilitychange", () => { if (document.visibilityState === "hidden") checkpoint(true); });

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
    let probed = false, timer;
    if (!g) {
      probed = true;
      try {
        const a = await Promise.race([navigator.gpu?.requestAdapter({ powerPreference: "high-performance" }),
          new Promise(resolve => { timer = setTimeout(() => resolve(null), 1500); })]);
        g = a ? (a.info || {}) : null;
      } catch {} finally { clearTimeout(timer); }
    }
    if (!g) return navigator.gpu ? "no WebGPU adapter" : "no WebGPU in this browser";
    const label = [g.vendor, g.architecture, g.device, g.description].filter(Boolean).join(" / ") || "unknown";
    return label + (probed ? " (probe only; no active game adapter recorded)" : "");
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
  function diagnosticJson(value) {
    const text = safeJson(value);
    return bytes(text) <= 24000 ? text : safeJson({ truncated: true, detail: text.slice(0, 5500) });
  }
  function raceSnapshot() {
    const block = window.carbonRaceBlock;
    if (!block?.memory || !Number.isInteger(block.ptr)) return null;
    try {
      const view = new DataView(block.memory.buffer), p = block.ptr;
      const u = offset => view.getUint32(p + offset, true);
      const seq = u(0x260);
      if (seq & 1) return { busy: true };
      const count = Math.min(u(0x2b4), 30), rows = [];
      for (let i = 0; i < count; i++) {
        const at = 0x2d0 + i * 48;
        rows.push({ id: u(at), place: u(at + 4), finished: u(at + 12), local: u(at + 24) });
      }
      const result = { phase: u(0x264), flow: u(0x2cc) & 255, car: u(0x268), session: u(0x2bc), loaded: u(0x2b8), count, rows };
      return seq === u(0x260) ? result : { busy: true };
    } catch { return { unavailable: true }; }
  }
  // Preserve only roster transitions, not frame-by-frame progress or player names.
  // sessionStorage retains the last transitions after a same-tab restart.
  const raceHistoryKey = 'carbon.race-diagnostics.v1', raceRun = Date.now();
  let raceHistory = [], raceSignature = '';
  try {
    const saved = sessionStorage.getItem(raceHistoryKey);
    if (saved && saved.length < 64000) {
      const parsed = JSON.parse(saved);
      if (Array.isArray(parsed)) raceHistory = parsed.slice(-40);
    }
  } catch {}
  function recordRace() {
    const race = raceSnapshot();
    if (!race || race.busy || race.unavailable) return;
    // Places change while driving; that does not explain a missing grid opponent.
    const signature = JSON.stringify({ ...race, rows: race.rows.map(({ place, ...row }) => row) });
    if (signature === raceSignature) return;
    raceSignature = signature;
    raceHistory.push({ at: new Date().toISOString(), run: raceRun,
      runtime: window.recompRuntimeBuild?.id || 'unknown', ...race });
    raceHistory = raceHistory.slice(-40);
    try { sessionStorage.setItem(raceHistoryKey, JSON.stringify(raceHistory)); } catch {}
  }
  setInterval(() => { recordRace(); recordInput(); }, 500);
  const bytes = (s) => encoder.encode(s).length;
  async function build(budget, note, previous = null) {
    const prev = previous && typeof previous === "object" ? previous : null;
    const canvas = document.getElementById("canvas");
    recordInput();
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
      `Stopped: ${prev ? "the game closed unexpectedly (the tab ended without a clean exit; sent from the next start page)" : stopped()}`,
      `Startup phase: ${window.recompStartupPhase || "unknown"}`,
    ];
    head.push(`Environment: ${safeJson({ secure: globalThis.isSecureContext, isolated: crossOriginIsolated,
      online: navigator.onLine, cores: navigator.hardwareConcurrency ?? null, deviceMemoryGiB: navigator.deviceMemory ?? null,
      wasmMemoryBytes: window.Module?.HEAPU8?.buffer?.byteLength ?? globalThis.wasmMemory?.buffer?.byteLength ?? null })}`);
    head.push(`Storage estimate (browser quota, not disk capacity): ${diagnosticJson(await storageSnapshot())}`);
    head.push(`Active graphics adapter: ${diagnosticJson(window.carbonGraphicsInfo || { unavailable: true })}`);
    head.push(`Browser timing (last 30 seconds): ${diagnosticJson(timingSnapshot())}`);
    head.push(`Renderer events (observed since reporter loaded; not game FPS): ${diagnosticJson({resets:rendererResets,presentationStallReports:rendererFaults,recent:rendererEvents})}`);
    head.push(`Game controller: ${diagnosticJson(window.recompGameController || null)}`);
    head.push(`Controller history (sampled every 500 ms): ${diagnosticJson(inputHistory)}`);
    head.push(`Recent errors: ${diagnosticJson(errors)}`);
    if (window.carbonDiagnostics) head.push(`Startup diagnostics: ${diagnosticJson(window.carbonDiagnostics)}`);
    if (prev) {
      const { log: _log, pageLines: _page, crashes: _crashes, ...about } = prev;
      head.push(`Previous session (what the closed tab saved last): ${diagnosticJson(about)}`);
    }
    try {
      const resources = performance.getEntriesByType("resource").slice(-30).map(r => ({ url: cleanUrl(r.name),
        type: r.initiatorType, durationMs: Math.round(r.duration), transferBytes: r.transferSize,
        encodedBytes: r.encodedBodySize, decodedBytes: r.decodedBodySize, status: r.responseStatus ?? null }));
      head.push(`Recent resource timing (zero may mean browser restriction): ${diagnosticJson(resources)}`);
    } catch {}
    const f = prev ? prev.failure : window.recompFailure;
    if (f) head.push(`Failure details: ${diagnosticJson(f)}`);
    if (f?.stack) head.push("Stack:", ...String(f.stack).split("\n").slice(0, 30).map((l) => "  " + l));
    const allLog = (prev ? prev.log : window.recompLog || []).map((l) => String(l));
    head.push(...await crashSection(prev ? (Array.isArray(prev.crashes) ? prev.crashes : []) : crashRecords(allLog)));
    if (window.carbonTestAssets) head.push(`Game files: ${safeJson(window.carbonTestAssets)}`);
    try { if (window.carbonCockpitDiagnostics) head.push(`Cockpit diagnostics: ${safeJson(window.carbonCockpitDiagnostics())}`); } catch {}
    const race = raceSnapshot();
    if (race) head.push(`Race roster: ${safeJson(race)}`);
    recordRace();
    if (raceHistory.length) head.push(`Race history: ${safeJson(raceHistory)}`);
    if (note) head.push("", "Player's note:", ...note.slice(0, 2000).split("\n").map((l) => "  " + l));
    const page = (prev ? (Array.isArray(prev.pageLines) ? prev.pageLines.map(String) : []) : pageLines).slice(-400);
    let pageText = page.join("\n");
    while (bytes(pageText) > budget / 6 && page.length) { page.shift(); pageText = page.join("\n"); }
    // A crash line is in the section above, whole; the log keeps its place, not its 24 KB.
    const log = allLog.map((l) => l.startsWith(CRASH) ? "[crash] (see Crash location above)" : l);
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
      `--- The game's log${prev ? ` (previous session: the last lines it saved, of ${Number(prev.logLines) || "?"} written)` : ""}: ${kept.length === log.length ? `all ${log.length}` : `the last ${kept.length} of ${log.length}`} lines ---`,
      ...kept,
      "",
      `--- The page's messages: the last ${page.length} ---`,
      pageText,
      "",
    ].join("\n");
    const redacted = hideKeys(text);
    if (bytes(redacted) <= budget) return redacted;
    const suffix = "\n[Report truncated to the upload limit]\n";
    // Trim on Unicode code points so multi-byte player notes remain valid UTF-8.
    if (budget <= bytes(suffix)) return "";
    const chars = Array.from(redacted);
    while (bytes(chars.join("")) + bytes(suffix) > budget) chars.splice(Math.floor(chars.length * .95));
    return chars.join("") + suffix;
  }

  // ---- Download log ----
  async function download(previous = null) {
    const text = await build(SAVE_MAX, "", previous);
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
  async function send(note, previous = null) {
    const s = server();
    if (!s) return { ok: false, text: "This page has no online server to send to. Use Download log instead." };
    const body = await build(SEND_MAX, note, previous);
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
    " plus recent timing, controller input, storage and error details, and what you write here. The server keeps no address.";

  let dialog = null;
  function openDialog(previous = null) {
    if (previous instanceof Event) previous = null; // called as a click handler
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
      const r = await send(note.value.trim(), previous);
      say.className = r.ok ? "rp-say" : "rp-say bad";
      say.textContent = r.text;
      sendBtn.disabled = r.ok;
    };
    saveBtn.onclick = async () => {
      const name = await download(previous);
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
    sendBtn.onclick = () => openDialog();
    row.append(saveBtn, sendBtn);
    wrap.append(row, say);
    statusEl.append(wrap);
  }

  // Native callbacks carry acknowledged NEW frames, not requestAnimationFrame ticks.
  // Constant-size counters on the frame path; one summary per second; no full logs.
  function installPerformanceReports() {
    const override = new URLSearchParams(location.search).get('RECOMP_PERF_REPORTS');
    if (override === '0' || (!window.recompSite && override !== '1')) return;
    const LIMIT = 2, COOLDOWN = 10 * 60 * 1000, STORE = 'carbon.performance.auto.v1';
    let epoch = null, width = 0, height = 0, started = 0, lastFrame = 0, lastNew = 0;
    let active = false, lowSeconds = 0, sending = false, nextSend = 0, attempts = 0;
    let healthySent = false, lowSent = false, audio = null, stopped = false;
    const normalSample = Math.random() < .1, rows = [], gpu = [];
    const bins = new Uint32Array(101);
    let frames = 0, repeats = 0, intervals = 0, maxGap = 0;
    let renderFrames = 0, encode = 0, proxyWait = 0, presenterWait = 0;
    let slotWait = 0, yielded = 0, blocked = 0, readyWait = 0, sleep = 0, overrun = 0, objectWait = 0;
    let bucketAt = performance.now();
    try { attempts = Math.min(LIMIT, Math.max(0, Number(sessionStorage.getItem(STORE)) || 0)); } catch {}
    if (attempts >= LIMIT) return;
    const round = v => Math.round(v * 100) / 100;
    function clearBucket() {
      bins.fill(0); frames = repeats = intervals = maxGap = 0;
      renderFrames = encode = proxyWait = presenterWait = 0;
      slotWait = yielded = blocked = readyWait = sleep = overrun = objectWait = 0;
    }
    function reset() {
      active = false; started = lastNew = lastFrame = 0; lowSeconds = 0;
      rows.length = gpu.length = 0; bucketAt = performance.now(); clearBucket();
    }
    const visible = () => !document.hidden && !window.recompFailure && !window.recompMenuOpen;
    function stopCollection() {
      stopped = true;
      window.recompPerformance.enabled = false;
      clearInterval(timer);
      // Stop native clocks and GPU queries too, not just the JavaScript receiver.
      window.Module?._host_performance_reports_stop?.();
    }
    // D3D9 frames have no renderer scene classification. Read Carbon's existing
    // sequence-guarded state four times per second. The general clock also moves
    // during pause: require a live driving state or advancing race timer too.
    let gameViews = null, gameProbeAt = -Infinity, gameMs = null, raceClock = null, gameActive = false;
    function gameplay(time, cls) {
      if (cls >= 0) return cls === 2;
      if (time - gameProbeAt < 250) return gameActive;
      gameProbeAt = time; gameActive = false;
      const b = window.carbonRaceBlock, buffer = b?.memory?.buffer, ptr = b?.ptr;
      if (!buffer || !Number.isInteger(ptr) || ptr < 0 || (ptr & 3) || ptr + 0x2d0 > buffer.byteLength) {
        gameMs = null; return false;
      }
      if (gameViews?.buffer !== buffer)
        gameViews = {buffer, u:new Uint32Array(buffer), i:new Int32Array(buffer)};
      const {u,i} = gameViews, at = offset => (ptr + offset) >>> 2;
      const seq = Atomics.load(i, at(0x260));
      if ((seq & 1) || u[at(0)] !== 0x31435243 || u[at(4)] !== 1) { gameMs = null; return false; }
      const flow = u[at(0x2cc)] & 255, car = u[at(0x268)], ms = u[at(0x2c8)], race = u[at(0x2b0)];
      if (Atomics.load(i, at(0x260)) !== seq) { gameMs = null; return false; }
      const mp = window.carbonMpBlock, mpPtr = mp?.ptr;
      let driving = false;
      if (mp?.memory?.buffer === buffer && Number.isInteger(mpPtr) && mpPtr >= 0 && !(mpPtr & 3) && mpPtr + 0x20 <= buffer.byteLength) {
        const base = mpPtr >>> 2, mpSeq = Atomics.load(i, base + 3);
        driving = !(mpSeq & 1) && u[base] === 0x31504d43 && (u[base+1] === 1 || u[base+1] === 2) &&
          u[base+4] === 1 && Atomics.load(i, base+3) === mpSeq;
      }
      // The free-roam block intentionally reports idle during native online races.
      // Their timer advances while driving and stops on the native pause screen.
      gameActive = flow === 6 && car !== 0 && gameMs !== null && ms !== gameMs &&
        (driving || (raceClock !== null && race !== raceClock));
      gameMs = flow === 6 && car !== 0 ? ms : null;
      raceClock = gameMs === null ? null : race;
      return gameActive;
    }
    function frame(time, cls, generation, w, h, repeat, slot, yieldMs, blockedMs, readyMs, sleepMs, overMs, objectMs) {
      if (stopped) return;
      // Threaded Emscripten uses timeOrigin + performance.now() across workers.
      time -= performance.timeOrigin;
      // A class transition or resize invalidates the comparison window.
      if (!visible() || !gameplay(time, cls) || w <= 0 || h <= 0) { if (active) reset(); return; }
      if (epoch !== generation || width !== w || height !== h) {
        reset(); epoch = generation; width = w; height = h;
      }
      if (!active) { active = true; started = bucketAt = time; }
      lastFrame = time;
      if (repeat) { ++repeats; return; }
      ++frames;
      if (lastNew && time > lastNew) {
        const gap = time - lastNew;
        ++bins[Math.min(100, Math.ceil(gap / 5))]; ++intervals;
        maxGap = Math.max(maxGap, gap);
      }
      lastNew = time;
      slotWait += slot; yielded += yieldMs; blocked += blockedMs; readyWait += readyMs;
      sleep += sleepMs; overrun += overMs; objectWait += objectMs;
    }
    function render(time, encodeMs, waitMs, deferMs) {
      if (stopped || !active || !visible()) return;
      ++renderFrames; encode += encodeMs; proxyWait += waitMs; presenterWait += deferMs;
    }
    function gpuSample(at, ms, passes) {
      at -= performance.timeOrigin;
      if (stopped || !active || !visible() || at < started || at < performance.now() - 30000 ||
          !Number.isFinite(ms) || ms < 0 || ms > 10000) return;
      gpu.push({at, ms:round(ms), passes});
      if (gpu.length > 30) gpu.shift();
    }
    function audioLine(text) {
      if (stopped) return;
      const m = /audio sink: (\d+) pulls, (\d+) late .*?, (\d+) starved, max gap ([\d.]+) ms, max render ([\d.]+) ms, period ([\d.]+) ms/.exec(text);
      if (m) audio = {at:performance.now(), pulls:+m[1], lateCallbacks:+m[2], maxGapMs:+m[4], maxRenderMs:+m[5], periodMs:+m[6]};
      // "starved" counts empty SDL queues at pull time, not audible dropouts.
    }
    function summary(reason) {
      const span = rows.reduce((n,r) => n+r.seconds, 0);
      const count = rows.reduce((n,r) => n+r.newFrames, 0);
      const g = gpu.filter(r => r.at >= bucketAt - 30000);
      const info = window.carbonGraphicsInfo || {};
      const clean = value => typeof value === 'string' ? value.slice(0,120) : null;
      return {
        kind:'carbon-performance', version:1, reason,
        build:{page:clean(window.recompPageBuild?.id), runtime:clean(window.recompRuntimeBuild?.id),
          wasm:clean(window.recompRuntimeBuild?.wasm), reporter:clean(reporterBuild?.id)},
        environment:{browser: String(navigator.userAgent || '').slice(0,250), cores:navigator.hardwareConcurrency || null,
          gpuVendor:clean(info.vendor), gpuArchitecture:clean(info.architecture), fallback:info.fallback ?? null,
          drawableWidth:width, drawableHeight:height, devicePixelRatio,
          renderScale:clean(window.Module?.ENV?.RECOMP_D3D9_SCALE || 'auto')},
        browserLongTasks:longTasks.filter(t => t.at >= bucketAt-30000).map(t => ({ms:round(t.ms)})),
        frameSource:'acknowledged new gameplay frames; repeats excluded', fps:span ? round(count/span) : null,
        seconds:round(span), rows:rows.slice(),
        gpu:{source:'sampled D3D9 render-pass timestamps; excludes copies and final composition',
          status:g.length ? 'measured' : 'unavailable', samples:g.length,
          meanPasses:g.length ? round(g.reduce((n,r)=>n+r.passes,0)/g.length) : null,
          meanMs:g.length ? round(g.reduce((n,r)=>n+r.ms,0)/g.length) : null,
          maxMs:g.length ? Math.max(...g.map(r=>r.ms)) : null},
        audio:audio && performance.now()-audio.at < 45000 ? {...audio, ageMs:round(performance.now()-audio.at), at:undefined,
          scope:'cumulative since audio sink start; late callbacks are not measured audible dropouts'} : null,
        timingScope:'CPU encoding is wall time. Waits overlap and cover displayed frames only. CPU execution time is unavailable; do not add these columns.',
      };
    }
    async function upload(reason) {
      if (sending || attempts >= LIMIT || Date.now() < nextSend) return;
      const s = server(); if (!s) return;
      const generation = started;
      const body = 'Carbon automatic performance v1\n' + JSON.stringify(summary(reason));
      if (bytes(body) > 24000) return;
      sending = true;
      try {
        const claim = async () => {
          let last = 0;
          try { last = Number(localStorage.getItem(STORE)) || 0; } catch {}
          const now = Date.now();
          if (now - last < COOLDOWN) { nextSend = last + COOLDOWN; return false; }
          // Reserve before the network call. Failures cannot create retry storms.
          nextSend = now + COOLDOWN; ++attempts;
          try { localStorage.setItem(STORE, String(now)); sessionStorage.setItem(STORE, String(attempts)); } catch {}
          return true;
        };
        const claimed = navigator.locks?.request ? await navigator.locks.request(STORE, claim) : await claim();
        if (!claimed || !active || !visible() || started !== generation) return;
        const response = await fetch(`${s.base}/reports${s.key ? `?key=${encodeURIComponent(s.key)}` : ''}`, {
          method:'POST', body, headers:{'Content-Type':'text/plain;charset=UTF-8'},
          credentials:'omit', cache:'no-store', signal:AbortSignal.timeout(10000),
        });
        if (response.status === 429) {
          const seconds = Number(response.headers.get('Retry-After'));
          if (Number.isFinite(seconds) && seconds > 0) nextSend = Math.max(nextSend, Date.now()+seconds*1000);
        }
        if (response.status === 201) {
          if (reason === 'sustained-low-fps') lowSent = true;
          else healthySent = true;
        }
      } catch { /* Reporting must never interrupt the game. No immediate retries. */ }
      finally { sending = false; if (lowSent || attempts >= LIMIT) stopCollection(); }
    }
    function tick() {
      if (stopped) return;
      const now = performance.now(), seconds = (now-bucketAt)/1000;
      if (!active || !visible() || now-lastFrame > 3000 || seconds > 3) { reset(); return; }
      if (seconds < .75) return;
      // Ignore the first ten seconds after loading, a menu, visibility change or resize.
      if (now-started < 10000) { bucketAt=now; clearBucket(); return; }
      let n = 0, p95 = null;
      for (let i=0;i<bins.length;i++) { n+=bins[i]; if (intervals && n>=Math.ceil(intervals*.95)) { p95=i===100 ? Math.max(500,maxGap) : i*5; break; } }
      const fps = frames/seconds;
      rows.push({seconds:round(seconds), newFrames:frames, repeats,
        p95UpperMs:p95, maxGapMs:round(maxGap), encodeMs:round(encode), renderFrames,
        proxyWaitMs:round(proxyWait), presenterWaitMs:round(presenterWait), slotWaitMs:round(slotWait),
        guestYieldMs:round(yielded), guestBlockedMs:round(blocked), guestReadyWaitMs:round(readyWait),
        guestSleepRequestedMs:round(sleep), guestSleepOverrunMs:round(overrun), guestObjectWaitMs:round(objectWait)});
      if (rows.length>30) rows.shift();
      lowSeconds = fps < 25 ? lowSeconds+seconds : 0;
      bucketAt=now; clearBucket();
      if (!lowSent && lowSeconds>=10) void upload('sustained-low-fps');
      else if (normalSample && !healthySent && !lowSent && rows.length===30 && lowSeconds===0) void upload('normal-sample');
    }
    window.recompPerformance = {enabled:true, frame, render, gpu:gpuSample, audio:audioLine};
    document.addEventListener('visibilitychange', reset);
    addEventListener('pagehide', reset);
    const timer = setInterval(tick, 1000);
  }
  installPerformanceReports();

  window.recompReport = { build, download, send, openDialog, offer, watchSession, endSession, takeSession, checkpoint };
  const actions = (window.recompMenuActions ||= []);
  actions.push(
    { id: "report-log", group: "Report a problem", label: "Download log", hint: "a text file of what the game wrote", run: () => { download(); } },
    { id: "report-send", group: "Report a problem", label: "Send a report", hint: "the log, to the game's server", run: () => openDialog() },
  );
})();
