// Public encounter messages and native shared-memory commands, without game saves.
import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import fs from "node:fs";

const baseline = process.env.ENCOUNTER_BASELINE === "1";
const version = baseline ? 4 : 5, abi = baseline ? 3 : 4;
const source = fs.readFileSync(process.env.ENCOUNTER_SOURCE || "source/web/encounter.js", "utf8");
function client({ eligible = true, attached = true, sameCrew = false } = {}) {
  let now = 4000, tick;
  const sent = [], listeners = new Map();
  const buffer = new SharedArrayBuffer(2048), v = new DataView(buffer);
  const set = (o, n) => v.setUint32(o, n, true);
  set(0, 0x314e4343); set(4, abi); set(0x314, eligible ? 1 : 0); set(0x320, 99);
  const block = { memory: { buffer }, ptr: 0 };
  const page = {
    TextEncoder, Atomics, DataView, Int32Array, Uint8Array,
    performance: { now: () => now }, setInterval: fn => { tick = fn; }, setTimeout() {},
    addEventListener: (t, fn) => listeners.set(t, fn), dispatchEvent() {}, CustomEvent: class {},
    carbonEncounterBlock: attached ? block : null,
    recompOnlineState: { open: true, me: 2, crewId: "beta", people: [{ id: 1, name: "Rival", crewId: sameCrew ? "beta" : "alpha" }], slotIds: [1] },
    recompOnlineSend: m => sent.push(structuredClone(m)),
  };
  page.window = page;
  vm.runInNewContext(source, page);
  const run = (ms = 100) => { now += ms; tick(); };
  const message = (op, fields = {}) => listeners.get("recomp-online-message")({ detail: { t: "race", k: "encounter", v: version, id: 1, to: 2, s: 77, op, ...fields } });
  run();
  return { sent, set, message, run, page, attach() { page.carbonEncounterBlock = block; }, command: () => v.getUint32(16, true), hasPeer: () => v.getUint32(36, true) };
}

test("a valid rival offer before the periodic capability opens the native prompt", () => {
  const c = client();
  c.message("offer", { car: 99 });
  assert.equal(c.sent.some(m => m.op === "decline"), false);
  assert.equal(c.command(), 1, "request the original prompt, never open the map directly");
});

test("temporary native unavailability waits and retries without declining the player", () => {
  const c = client({ eligible: false });
  c.message("cap", { car: 99, ready: true });
  c.message("offer", { car: 99 });
  c.run(8000);
  assert.equal(c.sent.some(m => ["decline", "cancel"].includes(m.op)), false);
  assert.equal(c.command(), 0, "do not use a busy HUD");
  c.set(0x314, 1); c.run();
  assert.equal(c.command(), 1, "resume when the original HUD becomes available");
});

test("capabilities received before the native block loads are retained", () => {
  const c = client({ attached: false });
  c.message("cap", { car: 99, ready: true });
  c.attach(); c.run();
  assert.equal(c.hasPeer(), 1);
});

test("a verified rival offer can wait for the native block itself to load", () => {
  const c = client({ attached: false });
  c.message("offer", { car: 99 });
  c.attach(); c.run();
  assert.equal(c.command(), 1);
  assert.equal(c.sent.some(m => m.op === "decline"), false);
});

test("same-crew and missing-membership offers cannot open an online rival prompt", () => {
  for (const missing of [false, true]) {
    const c = client({ sameCrew: true });
    if (missing) c.page.recompOnlineState.crewId = null;
    c.message("cap", { car: 99, ready: true }); c.message("offer", { car: 99 });
    assert.equal(c.command(), 0);
  }
});

test("one human acceptance never opens the destination map", () => {
  const c = client();
  c.message("offer", { car: 99 });
  c.set(0x308, 77); c.set(0x304, 9); c.run();
  assert.equal(c.command(), 1);
  assert.equal(c.sent.some(m => m.op === "accept"), true);
  c.message("accept"); c.run();
  assert.equal(c.command(), 5, "the original map is requested only after both accept");
});

test("an older encounter protocol cannot start the new native prompt flow", () => {
  const c = client();
  c.message("offer", { car: 99, v: version - 1 });
  assert.equal(c.command(), 0);
});

test("a deliberate native dismissal is distinct from temporary cancellation", () => {
  for (const deliberate of [false, true]) {
    const c = client();
    c.message("cap", { car: 99, ready: true }); c.message("offer", { car: 99 });
    c.set(0x308, 77); c.set(0x304, 6); c.set(0x31c, deliberate ? 5 : 0); c.run();
    assert.equal(c.sent.some(m => m.op === (deliberate ? "decline" : "cancel")), true);
    assert.equal(c.command(), 4, "the already-ended native session is reset after notifying the peer");
  }
});

test("peer decline requests native separation rearm; transport failure does not", () => {
  for (const op of ["decline", "cancel"]) {
    const c = client();
    c.message("cap", { car: 99, ready: true }); c.message("offer", { car: 99 });
    c.set(0x308, 77); c.set(0x304, 8);
    c.message(op);
    assert.equal(c.command(), op === "decline" ? 7 : 3);
  }
});
