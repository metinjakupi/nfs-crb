// Crash reports: where a thread crashed (source/kit/web/crash-capture.js -> report.js "Crash
// location") and a tab that died without a clean exit (start.js's session mark + report.js's
// saved tail, offered on the next start page).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";
import { parseSymbols, symbolize } from "../carbon-crash-symbolize.mjs";

const here = (p) => new URL(p, import.meta.url);
const capture = await readFile(process.env.CARBON_CRASH_CAPTURE_SOURCE || here("../../source/kit/web/crash-capture.js"), "utf8");
const report = await readFile(process.env.CARBON_REPORT_SOURCE || here("../../source/web/report.js"), "utf8");
const start = await readFile(process.env.CARBON_START_SOURCE || here("../../source/web/start.js"), "utf8");

const STACK = "RuntimeError: memory access out of bounds\n" +
  "    at wasm://wasm/0a1b2c3d:wasm-function[42720]:0x1f2e3d\n" +
  "    at wasm://wasm/0a1b2c3d:wasm-function[1234]:0x456\n" +
  "    at invokeEntryPoint (https://example.test/play/nfsc/CarbonRecomp.js:3:9999)";

function memoryStore(init = {}) {
  const m = new Map(Object.entries(init));
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), removeItem: (k) => m.delete(k), map: m };
}

function workerContext(probeText) {
  const listeners = {};
  const printed = [];
  class WorkerGlobalScope {}
  const self = Object.assign(new WorkerGlobalScope(), { name: "em-pthread", addEventListener: (t, f) => { listeners[t] = f; } });
  const buffer = new ArrayBuffer(4096);
  new Uint8Array(buffer).set(new TextEncoder().encode(probeText + "\0"), 100);
  const c = { WorkerGlobalScope, self, WebAssembly, Error, JSON, String, Math, Date, Uint8Array, TextDecoder, console,
    wasmExports: { recomp_crash_probe: () => 100 }, wasmMemory: { buffer }, _pthread_self: () => 0x123450,
    err: (t) => printed.push(t) };
  vm.runInNewContext(capture, c);
  return { listeners, printed, c };
}

test("a worker's trap prints one [crash] line with the stack and the guest registers", () => {
  const { listeners, printed, c } = workerContext("guest thread 1: EIP=0087e926 ESP=0effe000; returns: 0069a1b2");
  const error = new c.WebAssembly.RuntimeError("memory access out of bounds");
  error.stack = STACK;
  listeners.error({ error, message: "Uncaught RuntimeError: memory access out of bounds", filename: "https://example.test/play/nfsc/CarbonRecomp.js", lineno: 3, colno: 14560 });
  assert.equal(printed.length, 1);
  assert.ok(printed[0].startsWith("[crash] {"));
  const r = JSON.parse(printed[0].slice(8));
  assert.equal(r.where, "worker em-pthread");
  assert.equal(r.thread, 0x123450);
  assert.equal(r.message, "memory access out of bounds");
  assert.equal(r.wasm, true);
  assert.match(r.stack, /wasm-function\[42720\]/);
  assert.match(r.guest, /EIP=0087e926/);
  assert.ok(c.Error.stackTraceLimit >= 64, "deeper stacks than the default ten frames");
});

test("a wasm without the probe still reports the stack; repeated errors stop at four", () => {
  const { listeners, printed, c } = workerContext("");
  delete c.wasmExports.recomp_crash_probe;
  for (let i = 0; i < 6; i++) listeners.error({ error: new Error("plain " + i), message: "x" });
  assert.equal(printed.length, 4);
  assert.equal(JSON.parse(printed[0].slice(8)).guest, "");
});

function reportContext({ log = [], fetch, storage = memoryStore() } = {}) {
  const c = { setInterval: () => 1, clearInterval() {}, setTimeout: (f) => { f(); return 1; }, clearTimeout() {},
    sessionStorage: memoryStore(), localStorage: storage, addEventListener() {}, TextEncoder, URLSearchParams, URL, Date, Error, JSON,
    AbortSignal, console: { log() {}, info() {}, warn() {}, error() {} },
    document: { getElementById: () => null, visibilityState: "visible" }, location: new URL("https://example.test/play/nfsc/"),
    navigator: { userAgent: "test" }, screen: { width: 1, height: 1 }, innerWidth: 1, innerHeight: 1, devicePixelRatio: 1,
    crossOriginIsolated: true, carbonGraphicsInfo: { vendor: "test" }, recompLog: log,
    fetch: fetch || (async () => { throw new Error("no fetch expected"); }) };
  c.window = c;
  vm.runInNewContext(report, c);
  return c;
}
const crashLine = "[crash] " + JSON.stringify({ at: "2026-11-06T04:16:34.060Z", where: "worker em-pthread", thread: 0x123450,
  name: "RuntimeError", message: "memory access out of bounds", stack: STACK, source: "https://example.test/play/nfsc/CarbonRecomp.js",
  line: 3, column: 14560, wasm: true, guest: "guest thread 1: EIP=0087e926" });

test("the report names the crashed frames from the release's symbols file", async () => {
  const asked = [];
  const c = reportContext({ log: ["first line", crashLine, "worker sent an error!"], fetch: async (url) => {
    asked.push(String(url));
    return { ok: true, status: 200, text: async () => "0:__cxa_throw\n1234:memcpy\n42720:fn_0087e926\n" };
  } });
  const text = await c.recompReport.build(500 * 1024, "");
  assert.deepEqual(asked, ["https://example.test/play/nfsc/CarbonRecomp.js.symbols"]);
  assert.match(text, /^Crash location /m);
  assert.match(text, /^ {2}#0 wasm 42720 0x1f2e3d fn_0087e926$/m);
  assert.match(text, /^ {2}#1 wasm 1234 0x456 memcpy$/m);
  assert.match(text, /^ {2}#2 js invokeEntryPoint/m);
  assert.match(text, /^ {2}Guest: guest thread 1: EIP=0087e926$/m);
  assert.match(text, /\[crash\] \(see Crash location above\)/);
  assert.doesNotMatch(text.split("--- The game's log")[1], /wasm-function\[42720\]/, "the long line is not repeated in the log");
});

test("without a published symbols file the numbers stay, and the offline tool names them", async () => {
  const c = reportContext({ log: [crashLine], fetch: async () => ({ ok: false, status: 404, text: async () => "<html>" }) });
  const text = await c.recompReport.build(500 * 1024, "");
  assert.match(text, /^ {2}#0 wasm 42720 0x1f2e3d \?$/m);
  assert.match(text, /Names: not available \(HTTP 404/);
  const named = symbolize(text, parseSymbols("42720:fn_0087e926\n1234:memcpy\n"));
  assert.match(named, /^ {2}#0 wasm 42720 0x1f2e3d fn_0087e926$/m);
  assert.match(named, /^ {2}#1 wasm 1234 0x456 memcpy$/m);
});

test("a running session keeps its last lines; the next page reads them once and reports them", async () => {
  const storage = memoryStore();
  const log = Array.from({ length: 500 }, (_, i) => `line ${i}`);
  log.push(crashLine);
  const game = reportContext({ log, storage });
  game.recompReport.watchSession("nfsc.session.v1.1");
  const saved = JSON.parse(storage.getItem("nfsc.session.v1.1.tail"));
  assert.equal(saved.logLines, 501);
  assert.equal(saved.log.at(-1), "line 499");
  assert.equal(saved.crashes.length, 1);
  assert.ok(storage.getItem("nfsc.session.v1.1.tail").length <= 96 * 1024);
  const next = reportContext({ storage, fetch: async () => ({ ok: false, status: 404, text: async () => "" }) });
  const prev = next.recompReport.takeSession("nfsc.session.v1.1");
  assert.equal(storage.getItem("nfsc.session.v1.1.tail"), null, "taken once");
  assert.equal(next.recompReport.takeSession("nfsc.session.v1.1"), null);
  const text = await next.recompReport.build(500 * 1024, "", prev);
  assert.match(text, /^Stopped: the game closed unexpectedly/m);
  assert.match(text, /^Previous session \(what the closed tab saved last\): .*"logLines":501/m);
  assert.match(text, /previous session: the last lines it saved, of 501 written/);
  assert.match(text, /^line 499$/m);
  assert.match(text, /^ {2}#0 wasm 42720 0x1f2e3d \?$/m);
  game.recompReport.endSession();
  assert.equal(storage.getItem("nfsc.session.v1.1.tail"), null);
});

// start.js's session block, run on its own against stand-ins for the page.
function startBlock() {
  const from = start.indexOf("  const sessionKey = () =>"), to = start.indexOf("  Promise.all([slotReady, recoveryReady]).then(() => { if (slot) offerLastSession");
  assert.ok(from > 0 && to > from, "start.js has the session block");
  return start.slice(from, to);
}
function startContext(storage) {
  const events = {}, docEvents = {}, appended = [], dialogs = [], watched = [];
  const el = () => ({ style: {}, children: [], append(...n) { this.children.push(...n); }, setAttribute() {}, remove() { this.removed = true; } });
  const slotEl = el();
  const document = { visibilityState: "visible", createElement: el, addEventListener: (t, f) => { docEvents[t] = f; }, body: { append: (n) => appended.push(n) } };
  const recompReport = { watchSession: (k) => watched.push(k), endSession: () => watched.push("end"),
    takeSession: (k) => { const t = storage.getItem(k + ".tail"); storage.removeItem(k + ".tail"); return t ? JSON.parse(t) : null; },
    openDialog: (prev) => dialogs.push(prev) };
  const run = new Function("game", "slot", "on", "localStorage", "document", "window", "addEventListener", "$", "setTimeout",
    startBlock() + "\nreturn { markRunning, offerLastSession };");
  const api = run({ id: "nfsc" }, 1, { checked: true }, storage, document, { recompReport },
    (t, f) => { (events[t] ||= []).push(f); }, (sel) => (sel === ".cs-slot" ? slotEl : null), () => 0);
  return { api, events, docEvents, document, slotEl, appended, dialogs, watched };
}

test("start.js marks a running game, clears it on a clean exit and keeps it through a hidden tab", async () => {
  const storage = memoryStore();
  const s = startContext(storage);
  s.api.markRunning();
  await new Promise((r) => setImmediate(r));
  assert.equal(JSON.parse(storage.getItem("nfsc.session.v1.1")).state, "running");
  assert.deepEqual(s.watched, ["nfsc.session.v1.1"]);
  s.document.visibilityState = "hidden"; s.docEvents.visibilitychange();
  assert.equal(JSON.parse(storage.getItem("nfsc.session.v1.1")).state, "hidden");
  s.document.visibilityState = "visible"; s.docEvents.visibilitychange();
  assert.equal(JSON.parse(storage.getItem("nfsc.session.v1.1")).state, "running");
  s.events.pagehide.forEach((f) => f({ persisted: false }));
  assert.equal(storage.getItem("nfsc.session.v1.1"), null, "a clean exit leaves no mark");
  assert.equal(s.watched.at(-1), "end");
  s.events.pageshow.forEach((f) => f({ persisted: true }));
  await new Promise((r) => setImmediate(r));
  assert.equal(JSON.parse(storage.getItem("nfsc.session.v1.1")).state, "running", "back from the bfcache: running again");
  assert.deepEqual(s.watched, ["nfsc.session.v1.1", "end", "nfsc.session.v1.1"]);
  s.events["recomp-quit"].forEach((f) => f());
  assert.equal(storage.getItem("nfsc.session.v1.1"), null);
});

test("the next start page offers a report once after a tab died while visible, never after a hidden one", async () => {
  const storage = memoryStore({ "nfsc.session.v1.1": JSON.stringify({ version: 1, state: "running", at: Date.now() - 60000, started: "x", online: true }),
    "nfsc.session.v1.1.tail": JSON.stringify({ version: 1, log: ["last line"], logLines: 9 }) });
  const s = startContext(storage);
  await s.api.offerLastSession(false);
  assert.equal(s.slotEl.children.length, 1);
  const line = s.slotEl.children[0];
  assert.match(line.children[0], /closed unexpectedly last time/);
  line.children[1].onclick();
  assert.deepEqual(s.dialogs[0].log, ["last line"]);
  assert.equal(storage.getItem("nfsc.session.v1.1"), null);
  const again = startContext(storage);
  await again.api.offerLastSession(false);
  assert.equal(again.slotEl.children.length, 0, "offered once");

  const hidden = memoryStore({ "nfsc.session.v1.1": JSON.stringify({ version: 1, state: "hidden", at: Date.now() }),
    "nfsc.session.v1.1.tail": "{}" });
  const h = startContext(hidden);
  await h.api.offerLastSession(false);
  assert.equal(h.slotEl.children.length, 0);
  assert.equal(hidden.getItem("nfsc.session.v1.1"), null);
  assert.equal(hidden.getItem("nfsc.session.v1.1.tail"), null);

  const resumed = memoryStore({ "nfsc.session.v1.1": JSON.stringify({ version: 1, state: "running", at: Date.now() }) });
  const r = startContext(resumed);
  await r.api.offerLastSession(true);
  assert.equal(r.appended.length, 1, "a resuming reload gets the note in a corner");
  r.appended[0].children[1].onclick();
  assert.equal(r.dialogs[0].note, "the closed tab saved no log lines");
});
