import test from "node:test";
import assert from "node:assert/strict";
import { parseReport, buildPage } from "../carbon-reports-viewer.mjs";

const base = (stopped, extra = "") => `Report received 2026-11-06T21:42:06.608Z

Need for Speed: Carbon in the browser: problem report
Page: https://nfscrb.xyz/releases/20261106-1g-effec484/play/nfsc/
Player name: Tester
Browser: Google Chrome 157, Chromium 157 on Windows (Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36)
Stopped: ${stopped}
Startup phase: starting game code
Environment: {"deviceMemoryGiB":16,"wasmMemoryBytes":1794244608}
Storage estimate (browser quota, not disk capacity): {"quota":1048576000,"usage":0,"free":1048576000}
${extra}
Player's note:
  I got busted

--- The game's log: all 2 lines ---
[mod core.nfsc] core.nfsc: guards: attribute read on a dead collection 00000005 (holder 0, caller 0046348f): not found, skipped
`;

test("an out-of-bounds trap is a game crash with its guest address and a fired guard", () => {
  const r = parseReport("20261106-214221-bd3383.txt", base("error: Uncaught RuntimeError: memory access out of bounds",
    "Crash location (the crashed thread's own stack, newest frame first):\n  Guest: guest thread 1: EIP=00445f65 ESP=0efffe6c EBP=0\n  #0 wasm 7901 0x69986e body_00445c70\n"));
  assert.equal(r.category, "Game crash: memory access out of bounds");
  assert.equal(r.crashEip, "00445f65");
  assert.equal(r.crashTop, "body_00445c70");
  assert.equal(r.device, "Windows");
  assert.equal(r.release, "20261106-1g-effec484");
  assert.equal(r.wasmMB, 1711);
  assert.equal(r.received, "2026-11-06T21:42:21Z");
  assert.match(r.note, /busted/);
  assert.equal(r.guardLines.length, 1);
});

test("categories for the other stop reasons", () => {
  const c = s => parseReport("20261106-000000-aaaaaa.txt", base(s)).category;
  assert.match(c("startup: The operation failed for an unknown transient reason (e.g. out of memory)."), /iPhone\/iPad storage/);
  assert.match(c("startup-storage: The browser does not have enough storage for the game."), /storage full/);
  assert.match(c("startup: This browser could not start WebGPU."), /No WebGPU/);
  assert.match(c("the game closed unexpectedly (the tab ended without a clean exit)"), /Tab closed/);
  assert.match(c("startup: Unterminated string in JSON at position 156"), /manifest/);
  assert.match(c("error: WebAssembly.Memory(): could not allocate memory"), /allocate/);
  assert.equal(c("something new"), "Other");
});

test("the page embeds the data without breaking out of its script tag", () => {
  const page = buildPage([{ ...parseReport("20261106-000000-aaaaaa.txt", base("x")), note: "</script><b>" }], "now");
  assert.ok(!/<\/script><b>/.test(page));
  assert.match(page, /const DATA=\[/);
});
