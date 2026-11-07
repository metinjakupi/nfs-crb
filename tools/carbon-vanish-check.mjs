#!/usr/bin/env node
// carbon-vanish-check.mjs - the release check for "nothing vanishes or pops in while visible" (plans/many-cars-design.md,
// "Done when" 1, 2, 4 and 7; logs/no-vanish-20261106). Two real games (or one game and scripted relay bots) drive on
// a busy road in free roam while tools/carbon-vanish-sampler.js counts, in each game, the vehicles of every kind that
// vanish while on screen (within --vanish-range, the draw distance) or appear on screen within --appear-range (150 m).
// Exit code 0 when every count is within its limit, 1 when not, 2 when the run itself failed.
//
// Usage (from the repository root):
//   node tools/carbon-vanish-check.mjs --site <dir> --profile <chrome profile dir> [options]
//     --site <dir>          a site folder with nfsc/ (the page and the runtime to check), served by this tool
//     --profile <dir>       a COPY of a Chrome test profile with the game imported for http://localhost:55257 (the
//                           pages are opened at that origin and every request is answered from --site; nothing is
//                           sent to a server on that port). Chrome locks it: one check per copy. Take a fresh copy
//                           (cp -cR) for every run: the games save as they play, and a save left in the crew-logo
//                           screen stops the menu walk (seen 2026-11-06).
//     --mode two|bots       two: Ana and Ben (saves --saves, default nativeA,nativeB) drive the straight road east of the
//                           safehouse back and forth at --speed, Ben --gap metres behind on Ana's own path; Ana's traffic
//                           and police are shared to Ben (the lower relay id owns them). bots: Ana stands; relay bots drive
//                           off to 1300 m, stall 8 s, leave, pause in front of her (friends' cars only).
//     --minutes 4  --speed 20  --gap 15  --far 315
//     --relay-cmd <exe>     the Rust relay (server/target/release/carbon-server); default: node tools/carbon-net-relay.mjs
//     --max-vanish 0  --max-pop 0  --vanish-range 950  --appear-range 150  --kinds friend,copy-traffic,copy-police,traffic,police
//                           (950 m: just inside the far radius, mp.h MP_VIEW_FAR 1000 m, past which a car may go in view,
//                           plans/many-cars-design.md section 3; there a car is about 3 pixels wide at 1280 x 720)
//     --query <q>           more page query for every game (e.g. RECOMP_AUDIO_STATS=1)
//     --out <dir>           results (check.json, consoles, screenshots); default logs/vanish-check-<time>
// With the mod's draw-only cars (drawonly.h) the sampler also counts them as kind "friend-l2" and leaves hand-overs between a
// friend's full car and its draw-only car out of the vanish / pop counts (check.json: handovers).
// It prints one line per game and kind, and a verdict. A game older than the no-vanish build leaves MpMore.spare 0:
// nothing can be projected, and the check fails ("no view").
import { spawn } from "node:child_process";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";

const args = Object.fromEntries(process.argv.slice(2).reduce((a, x, i, all) => (x.startsWith("--") ? [...a, [x.slice(2), all[i + 1] && !all[i + 1].startsWith("--") ? all[i + 1] : "1"]] : a), []));
if (args["bots-for"]) await bots(args["bots-for"], +args.key, JSON.parse(args.at)); // the --mode bots helper (below)
const need = (k) => { if (!args[k]) { console.error(`--${k} is required (see the top of ${path.basename(process.argv[1])})`); process.exit(2); } return args[k]; };
const SITE = path.resolve(need("site")), PROFILE = path.resolve(need("profile"));
const MODE = args.mode || "two", MINUTES = +(args.minutes || 4), SPEED = +(args.speed || 20), GAP = +(args.gap || 15), FAR = +(args.far || 315);
const MAX_VANISH = +(args["max-vanish"] ?? 0), MAX_POP = +(args["max-pop"] ?? 0);
const RANGES = { vanish: +(args["vanish-range"] || 950), appear: +(args["appear-range"] || 150) };
const KINDS = (args.kinds || "friend,friend-l2,copy-traffic,copy-police,traffic,police").split(",");
const SAVES = (args.saves || "nativeA,nativeB").split(",");
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), "..");
const OUT = path.resolve(args.out || path.join(ROOT, "logs", `vanish-check-${new Date().toISOString().replace(/[:.]/g, "-")}`));
fs.mkdirSync(path.join(OUT, "shots"), { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (s) => { const l = `${new Date().toISOString()} ${s}`; console.log(l); fs.appendFileSync(path.join(OUT, "run.log"), l + "\n"); };
const SAMPLER = fs.readFileSync(path.join(ROOT, "tools/carbon-vanish-sampler.js"), "utf8");
const children = [];
const cleanup = () => { for (const c of children) try { c.kill("SIGTERM"); } catch {} };
process.on("exit", cleanup);
process.on("SIGINT", () => process.exit(2));

// ---- the page server (cross-origin isolated, as the site) ----
const TYPES = { ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm", ".html": "text/html", ".json": "application/json", ".css": "text/css", ".png": "image/png", ".jpg": "image/jpeg", ".svg": "image/svg+xml" };
const server = http.createServer((req, res) => {
  const u = decodeURIComponent(new URL(req.url, "http://x").pathname);
  let f = path.join(SITE, u);
  if (!f.startsWith(SITE)) { res.statusCode = 403; return res.end(); }
  try { if (fs.statSync(f).isDirectory()) f = path.join(f, "index.html"); } catch {}
  fs.stat(f, (err, st) => {
    const h = { "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp", "Cross-Origin-Resource-Policy": "cross-origin", "Access-Control-Allow-Origin": "*", "Cache-Control": "no-cache" };
    if (err || !st.isFile()) { res.writeHead(404, h); return res.end(); }
    res.writeHead(200, { ...h, "Content-Type": TYPES[path.extname(f)] || "application/octet-stream", "Content-Length": st.size });
    fs.createReadStream(f).pipe(res);
  });
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const SPORT = server.address().port;

// ---- the relay ----
async function startRelay() {
  const cmd = args["relay-cmd"] ? [path.resolve(args["relay-cmd"]), ["0", "127.0.0.1"]] : [process.execPath, [path.join(ROOT, "tools/carbon-net-relay.mjs"), "0", "127.0.0.1"]];
  const p = spawn(cmd[0], cmd[1], { stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, CARBON_RELAY_KEY: "" } });
  children.push(p);
  const relayLog = fs.createWriteStream(path.join(OUT, "relay.log"));
  let text = "";
  p.stdout.on("data", (d) => { text += d; relayLog.write(d); }); p.stderr.on("data", (d) => relayLog.write(d));
  for (let i = 0; i < 100; i++) { const m = text.match(/ws:\/\/127\.0\.0\.1:(\d+)/); if (m) return +m[1]; await sleep(100); }
  throw new Error("the relay did not say its port");
}
const RPORT = await startRelay();
const ROOM = `check${Date.now() % 100000}`;

// ---- Chrome over the DevTools protocol ----
const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
try { fs.unlinkSync(path.join(PROFILE, "DevToolsActivePort")); } catch {}
for (const f of ["SingletonLock", "SingletonSocket", "SingletonCookie"]) try { fs.unlinkSync(path.join(PROFILE, f)); } catch {}
const chrome = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", `--user-data-dir=${PROFILE}`, "--enable-unsafe-webgpu", "--use-angle=metal",
  "--mute-audio", "--no-first-run", "--no-default-browser-check", "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
  "--disable-backgrounding-occluded-windows", "--window-size=1280,720", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
children.push(chrome);
let dport = null;
for (let i = 0; i < 150 && !dport; i++) { await sleep(100); try { dport = fs.readFileSync(path.join(PROFILE, "DevToolsActivePort"), "utf8").split("\n")[0].trim(); } catch {} }
if (!dport) { log("Chrome did not start"); process.exit(2); }
async function connect(url) {
  const ws = new WebSocket(url); let id = 0; const pending = new Map(), listeners = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { const { res, rej } = pending.get(m.id); pending.delete(m.id); m.error ? rej(new Error(m.error.message)) : res(m.result); } else if (m.method) for (const l of listeners) l(m); };
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  return { ws, on: (f) => listeners.push(f), send: (method, params = {}) => new Promise((res, rej) => { const i = ++id; pending.set(i, { res, rej }); ws.send(JSON.stringify({ id: i, method, params })); }) };
}
const browser = await connect((await (await fetch(`http://127.0.0.1:${dport}/json/version`)).json()).webSocketDebuggerUrl);
await browser.send("Browser.grantPermissions", { origin: "http://localhost:55257", permissions: ["durableStorage"] }).catch(() => {});
const KEYS = { Enter: [13, "\r"], Escape: [27], ArrowRight: [39] };
async function openGame(i, query) {
  const { targetId } = await browser.send("Target.createTarget", { url: "about:blank", newWindow: true });
  const list = await (await fetch(`http://127.0.0.1:${dport}/json/list`)).json();
  const c = await connect(list.find((x) => x.id === targetId).webSocketDebuggerUrl);
  const con = path.join(OUT, `console-${i}.log`); fs.writeFileSync(con, ""); const t0 = Date.now();
  c.on((m) => {
    if (m.method === "Fetch.requestPaused") {
      const from = m.params.request.url, to = from.replace("http://localhost:55257/", `http://127.0.0.1:${SPORT}/`);
      c.send("Fetch.continueRequest", to !== from ? { requestId: m.params.requestId, url: to } : { requestId: m.params.requestId }).catch(() => {});
    } else if (m.method === "Runtime.consoleAPICalled") fs.appendFileSync(con, `${((Date.now() - t0) / 1000).toFixed(2)} [${m.params.type}] ${m.params.args.map((a) => a.value ?? a.description ?? "").join(" ")}\n`);
  });
  await c.send("Page.addScriptToEvaluateOnNewDocument", { source: "Object.defineProperty(Navigator.prototype,'getGamepads',{value:()=>[],configurable:true});" });
  await c.send("Fetch.enable", { patterns: [{ urlPattern: "http://localhost:55257/*", requestStage: "Request" }] });
  await c.send("Runtime.enable"); await c.send("Page.enable");
  await c.send("Emulation.setFocusEmulationEnabled", { enabled: true }).catch(() => {});
  await c.send("Emulation.setDeviceMetricsOverride", { width: 1280, height: 720, deviceScaleFactor: 1, mobile: false });
  await c.send("Page.navigate", { url: `http://localhost:55257/nfsc/?${query}` });
  const ev = async (expression) => { try { const r = await c.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true }); return r.result?.value ?? null; } catch { return null; } };
  const key = async (k) => { const [vk, text] = KEYS[k]; await c.send("Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key: k, code: k, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, ...(text ? { text } : {}) }); await sleep(90); await c.send("Input.dispatchKeyEvent", { type: "keyUp", key: k, code: k, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk }); };
  const shot = async (name) => { const r = await c.send("Page.captureScreenshot", { format: "jpeg", quality: 70 }); fs.writeFileSync(path.join(OUT, "shots", `${name}-${i}.jpg`), Buffer.from(r.data, "base64")); };
  return { i, c, ev, key, shot };
}
// The guest base, this player's car and its model (as logs/trucks-20261106/probe.js).
const PROBE = `(() => { const m = new DataView(HEAPU8.buffer); let B = globalThis.__gb;
  if (B === undefined || m.getUint32(B + 0x400000, true) !== 0x00905a4d) { B = undefined;
    for (let b = 0; b + 0xc00000 < m.byteLength && b < 0x40000000; b += 0x1000) if (m.getUint32(b + 0x400000, true) === 0x00905a4d && m.getUint32(b + 0xa62c48, true) > 0) { B = b; break; }
    globalThis.__gb = B; }
  if (B === undefined) return null; const r = (a) => m.getUint32(B + a, true), fl = (a) => m.getFloat32(B + a, true);
  for (let n = r(0xa9e728), i = 0; n && n !== 0xa9e728 && i++ < 256; n = r(n)) { const c = n - 0xd0;
    if (r(c + 0x148) === 0 && r(c + 0x7c)) { const s = r(r(r(c + 0x70) + 0x30)); let col = r(c + 0xd8); if (col && (r(c + 0xd8 + 0xc) & 1)) col = r(col + 0xc);
      return { me: 1, p: [fl(s + 0x10), fl(s + 0x14), fl(s + 0x18)], key: col ? r(col + 0x10) >>> 0 : 0 }; } }
  return { me: 0 }; })()`;
const MENU = `(()=>{const B=globalThis.__gb;if(B===undefined||!globalThis.carbonMpBlock)return 'boot';const m=new DataView(HEAPU8.buffer),r=a=>m.getUint32(B+a,true);
const f=r(0xa97a78);if(!f)return '';let n=r(r(f)+0xe4);for(let i=0;n&&i<64;i++,n=r(n+8)){if(!r(n+0x20))continue;const s=r(n+0xc);let t='';for(let j=0;s&&j<64;j++){const c=m.getUint8(B+s+j);if(!c)break;t+=String.fromCharCode(c)}return t}return ''})()`;
// To career free roam, the menus as logs/trucks-20261106/run-two.mjs.
async function toFreeRoam(g) {
  let last = "", same = 0, garage = 0, map = 0;
  for (let i = 0; i < 120; i++) {
    const p = await g.ev(PROBE), m = String(await g.ev(MENU));
    if (p && p.me && garage > 0) return true;
    if (m !== last) { last = m; same = 0; log(`game ${g.i}: menu ${m || "(none)"}`); } else same++;
    if (m === "boot" || m === "") { await sleep(2500); continue; }
    if (map && /mainmenu_sub/i.test(m)) { if (garage++ < 4) { await g.key("ArrowRight"); await sleep(1500); await g.key("Enter"); await sleep(10000); } else await sleep(3000); continue; }
    if (/worldmap/i.test(m)) { map = 1; await g.key("Escape"); await sleep(3000); continue; }
    await g.key("Enter"); await sleep(same > 2 ? 8000 : 5000);
  }
  return !!(await g.ev(PROBE))?.me;
}
// In-page driver: the car drives a loop on the road at SPEED: out along the right lane from NEAR to FAR metres past LEAD,
// a half circle (radius LANE / 2) into the other lane, back to NEAR, a half circle back; its place, heading and speed are
// written every 16 ms (so the physics and the poses sent are smooth), lag seconds behind on the loop. stand: it stands.
const LEAD = [-200.1, 50.6, 3255.3, 77.4], LANE = 7;
const DRIVE = (lag, stand) => `(() => { clearInterval(globalThis.__nvDriveTimer); const L = ${JSON.stringify(LEAD)}, NEAR = 15, FAR = ${FAR}, V = ${SPEED}, LAG = ${lag}, T0 = ${Date.now() + 3000}, STAND = ${stand ? 1 : 0}, RAD = ${LANE / 2};
  const h0 = L[3] * Math.PI / 180, f = [Math.sin(h0), Math.cos(h0)], rt = [Math.cos(h0), -Math.sin(h0)], span = FAR - NEAR, arc = Math.PI * RAD, loop = 2 * span + 2 * arc;
  const at = (s) => { s = ((s % loop) + loop) % loop; let d, side, h;
    if (s < span) { d = NEAR + s; side = 0; h = 0; }
    else if (s < span + arc) { const a = (s - span) / RAD; d = FAR + RAD * Math.sin(a); side = RAD - RAD * Math.cos(a); h = -a; }
    else if (s < 2 * span + arc) { d = FAR - (s - span - arc); side = 2 * RAD; h = -Math.PI; }
    else { const a = (s - 2 * span - arc) / RAD; d = NEAR - RAD * Math.sin(a); side = RAD + RAD * Math.cos(a); h = -Math.PI - a; }
    return [L[0] + f[0] * d - rt[0] * side, L[1], L[2] + f[1] * d - rt[1] * side, h0 + h]; };
  globalThis.__nvDriveTimer = setInterval(() => { const B = globalThis.__gb, m = new DataView(HEAPU8.buffer); if (B === undefined) return;
    const r = (a) => m.getUint32(B + a, true), w = (a, v) => m.setFloat32(B + a, v, true); let me = 0;
    for (let n = r(0xa9e728), i = 0; n && n !== 0xa9e728 && i++ < 64; n = r(n)) { const c = n - 0xd0; if (r(c + 0x148) === 0 && r(c + 0x7c)) { me = c; break; } }
    if (!me) return; const st = r(r(r(me + 0x70) + 0x30));
    const t = STAND ? 0 : Math.max(0, (Date.now() - T0) / 1000 - LAG), p = at(t * V), h = p[3], v = STAND ? 0 : V;
    [0, Math.sin(h / 2), 0, Math.cos(h / 2)].forEach((x, k) => w(st + 4 * k, x));
    [p[0], p[1], p[2]].forEach((x, k) => w(st + 0x10 + 4 * k, x));
    [Math.sin(h) * v, 0, Math.cos(h) * v].forEach((x, k) => w(st + 0x20 + 4 * k, x)); for (let k = 0; k < 3; k++) w(st + 0x30 + 4 * k, 0);
  }, 16); return 'driving'; })()`;

let failed = false;
const result = { mode: MODE, minutes: MINUTES, ranges: RANGES, limits: { vanish: MAX_VANISH, pop: MAX_POP }, kinds: KINDS, games: [] };
try {
  const names = MODE === "two" ? ["Ana", "Ben"] : ["Ana"];
  const games = [];
  for (let i = 0; i < names.length; i++) {
    games.push(await openGame(i, `profile=${SAVES[i]}&mp=ws://127.0.0.1:${RPORT}/${ROOM}&name=${names[i]}${args.query ? "&" + args.query : ""}`));
    if (i === 0) await sleep(8000); // Ana first: the lower relay id owns the shared traffic
  }
  log(`${MODE}: page server ${SPORT}, relay ${RPORT}, room ${ROOM}, ${names.length} game(s)`);
  const ok = await Promise.all(games.map(toFreeRoam));
  if (ok.includes(false)) { for (const g of games) await g.shot("not-in-free-roam").catch(() => {}); throw new Error(`game(s) ${ok.map((x, i) => (x ? "" : i)).join(" ")} did not reach free roam`); }
  const keys = await Promise.all(games.map(async (g) => (await g.ev(PROBE)).key));
  for (let i = 0; i < games.length; i++) {
    // bots drive in Ana's own model (--mode bots), so there her own key is a friend's too (her own car is told apart by class)
    const fk = MODE === "bots" ? [keys[0]] : keys.filter((_, j) => j !== i);
    await games[i].ev(`globalThis.__nvFriendKeys=${JSON.stringify(fk)};globalThis.__nvRanges=${JSON.stringify(RANGES)}`);
    await games[i].ev(DRIVE(i * GAP / SPEED, MODE === "bots"));
  }
  await sleep(8000);
  for (const g of games) log(`sampler: ${await g.ev(SAMPLER)}`);
  log("drive start");
  let bots = null;
  if (MODE === "bots") {
    const scen = spawn(process.execPath, [path.join(ROOT, "tools/carbon-vanish-check.mjs"), "--bots-for", `ws://127.0.0.1:${RPORT}/${ROOM}`, "--key", String(keys[0]), "--at", JSON.stringify(LEAD)], { stdio: "inherit" });
    children.push(scen); bots = scen;
  }
  const until = Date.now() + MINUTES * 60000;
  for (let k = 1; Date.now() < until; k++) { await sleep(Math.min(30000, until - Date.now())); for (const g of games) await g.shot(`t${k * 30}s`).catch(() => {}); }
  log("drive end");
  const outs = [];
  for (const g of games) outs.push(await g.ev("globalThis.__nvOut()"));
  for (let i = 0; i < games.length; i++) {
    const o = outs[i];
    // A friend's car that jumps because its own game put it somewhere else (that game's own car jumped: the test's driver
    // after a stall, or the game putting it back on the road) is that game's doing, not this one's: counted apart.
    const theirs = outs.filter((_, j) => j !== i).flatMap((x) => x?.selfJumpAt || []);
    o.senderJumped = { vanished: 0, appeared: 0 };
    for (const e of o.events || []) {
      if (e.kind !== "friend" || e.why !== "jumped") continue;
      const at = o.since + e.t;
      if (theirs.some((s) => at >= s - 300 && at <= s + 1500)) { o.senderJumped[e.what]++; o[e.what].friend--; e.senderJumped = 1; }
    }
    const fps = fs.readFileSync(path.join(OUT, `console-${i}.log`), "utf8").split("\n").map((l) => l.match(/carlimit fps ([\d.]+) \(5 s\)/)).filter(Boolean).map((m) => +m[1]);
    o.fps5s = fps.slice(-Math.ceil(MINUTES * 12));
    result.games.push({ name: names[i], ...o });
    const mins = o.ms / 60000;
    if (o.noView || !o.ticks) { log(`${names[i]}: NO VIEW (${o.noView} samples, ${o.ticks} counted): a game older than the no-vanish build cannot be checked`); failed = true; continue; }
    for (const kind of KINDS) {
      const v = o.vanished[kind] || 0, a = o.appeared[kind] || 0, bad = v > MAX_VANISH || a > MAX_POP;
      failed ||= bad;
      if (kind === "friend" && (o.senderJumped.vanished || o.senderJumped.appeared)) log(`${names[i]} friend       (not counted: ${o.senderJumped.vanished} vanished, ${o.senderJumped.appeared} appeared as the other game's own car jumped)`);
      log(`${names[i]} ${kind.padEnd(12)} on screen ${String(o.onScreen[kind] || 0).padStart(6)} car-samples; vanished ${v} (${(v / mins).toFixed(1)}/min), appeared within ${RANGES.appear} m ${a} (${(a / mins).toFixed(1)}/min)${bad ? "  FAIL" : ""}`);
    }
    const sorted = [...o.fps5s].sort((x, y) => x - y);
    if (o.handovers) log(`${names[i]} hand-overs between a friend's full car and its draw-only car: down ${o.handovers.down}, up ${o.handovers.up}`);
    log(`${names[i]} frames: fps (5 s means) min ${sorted[0] ?? "?"}, median ${sorted[sorted.length >> 1] ?? "?"}; game stalls over 100 ms ${o.stalls}; own car jumps ${o.selfJumps || 0} (the driver or the game putting it back: the others see its car jump too)`);
  }
  if (bots) bots.kill("SIGTERM");
} catch (e) {
  log("RUN FAILED " + (e.stack || e)); fs.writeFileSync(path.join(OUT, "check.json"), JSON.stringify(result, null, 1)); cleanup(); process.exit(2);
}
fs.writeFileSync(path.join(OUT, "check.json"), JSON.stringify(result, null, 1));
log(failed ? "VERDICT: FAIL" : "VERDICT: PASS");
try { await browser.send("Browser.close"); } catch {}
cleanup();
process.exit(failed ? 1 : 0);

// --mode bots: relay members with no game behind them, at 30 poses a second in Ana's own model, in front of her standing
// car, leaving the ways a friend's car leaves, over and over: driving off to 1300 m and back, stalling 2 s and 8 s (past
// the page's 5 s), closing the connection, pausing (m 0) for 12 s. Runs until killed.
async function bots(relay, KEY, LEAD) {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const H = (LEAD[3] * Math.PI) / 180, F = [Math.sin(H), 0, Math.cos(H)], RT = [Math.cos(H), 0, -Math.sin(H)];
  const at = (ahead, side = 4) => [LEAD[0] + F[0] * ahead + RT[0] * side, LEAD[1] + 0.3, LEAD[2] + F[2] * ahead + RT[2] * side];
  let n = 0;
  class Bot {
    constructor() {
      Object.assign(this, { id: 0, ahead: 0, speed: 0, dir: 1, mode: 1, silent: false, ts: 1000, last: Date.now() });
      this.ws = new WebSocket(relay);
      this.ws.addEventListener("open", () => this.ws.send(JSON.stringify({ t: "hi", name: `Bot${++n}`, crew: "" })));
      this.ws.addEventListener("message", (e) => { if (typeof e.data === "string") { try { const m = JSON.parse(e.data); if (m.t === "welcome") this.id = m.id; } catch {} } });
      this.ws.addEventListener("error", () => {});
      this.timer = setInterval(() => {
        const now = Date.now(), dt = (now - this.last) / 1000; this.last = now; this.ahead += this.speed * this.dir * dt; this.ts += dt * 1000;
        if (!this.id || this.silent) return;
        const h = this.dir > 0 ? H : H + Math.PI, v = this.mode === 1 ? [Math.sin(h) * this.speed, 0, Math.cos(h) * this.speed] : [0, 0, 0];
        this.ws.send(JSON.stringify({ t: "pose", k: KEY, m: this.mode, ts: Math.round(this.ts), s: [0, Math.sin(h / 2), 0, Math.cos(h / 2), ...at(this.ahead), ...v, 0, 0, 0] }));
      }, 33);
    }
    async ready() { for (let i = 0; i < 100 && !this.id; i++) await sleep(100); return this; }
    close() { clearInterval(this.timer); try { this.ws.close(); } catch {} }
  }
  const until = async (b, f) => { while (!f(b)) await sleep(50); };
  for (;;) {
    let b = await new Bot().ready(); b.ahead = 30; await sleep(6000);
    b.speed = 35; await until(b, (x) => x.ahead >= 1300); b.speed = 0; await sleep(3000);
    b.dir = -1; b.speed = 35; await until(b, (x) => x.ahead <= 60); b.dir = 1; b.speed = 10; await sleep(2000);
    b.silent = true; await sleep(2000); b.silent = false; await sleep(4000); b.silent = true; await sleep(8000); b.silent = false; await sleep(4000);
    b.close(); await sleep(25000);
    b = await new Bot().ready(); b.ahead = 70; await sleep(6000); b.mode = 0; await sleep(12000); b.mode = 1; await sleep(3000);
    b.speed = 15; await sleep(3000); b.close(); await sleep(25000);
  }
}
