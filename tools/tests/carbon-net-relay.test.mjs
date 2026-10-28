// Run with Node 22+: node --test tools/tests/carbon-net-relay.test.mjs
// CARBON_RELAY_SCRIPT selects a captured server version for regression checks.
// CARBON_RELAY_CMD runs a server program instead of a Node script, with the same arguments
// and environment, e.g. the Rust server: CARBON_RELAY_CMD=server/target/release/carbon-server
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import net from "node:net";
import { fileURLToPath } from "node:url";
import { setTimeout as delay } from "node:timers/promises";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import http from "node:http";
import crypto from "node:crypto";
import vm from "node:vm";

const script = process.env.CARBON_RELAY_SCRIPT || fileURLToPath(new URL("../carbon-net-relay.mjs", import.meta.url));
const key = "local-relay-test";
const pose = { t: "pose", k: 0x12345678, m: 1, ts: 1500, s: [0, 0, 0, 1, 12, 3, -40, 5, 0, 0, 0, 0, 0] };

async function until(check, description) {
  const end = Date.now() + 2500;
  do {
    const value = check();
    if (value) return value;
    await delay(10);
  } while (Date.now() < end);
  throw new Error(`Timed out: ${description}`);
}

async function start(t, env = {}) {
  const command = process.env.CARBON_RELAY_CMD;
  const child = spawn(command || process.execPath, [...(command ? [] : [script]), "0", "127.0.0.1"], {
    env: { ...process.env, CARBON_RELAY_KEY: key, ...env },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "", stderr = "", ended = false, spawnError;
  child.stdout.on("data", (b) => { stdout = (stdout + b).slice(-8192); });
  child.stderr.on("data", (b) => { stderr = (stderr + b).slice(-8192); });
  child.on("error", (error) => { spawnError = error; });
  const closed = new Promise((resolve) => child.on("close", () => { ended = true; resolve(); }));
  t.after(async () => { child.kill(); await closed; });
  function alive() {
    if (spawnError) throw spawnError;
    assert.equal(ended, false, `relay exited unexpectedly:\n${stderr}`);
  }
  const port = await until(() => {
    alive();
    return Number(stdout.match(/relay on ws:\/\/127\.0\.0\.1:(\d+)\//)?.[1]) || false;
  }, "relay startup");
  return {
    port, alive,
    ws: (room = "lobby", suppliedKey = key) => `ws://127.0.0.1:${port}/${encodeURIComponent(room)}?key=${encodeURIComponent(suppliedKey)}`,
    async stats(room = "lobby") {
      alive();
      const response = await fetch(`http://127.0.0.1:${port}/stats?room=${encodeURIComponent(room)}`, {
        signal: AbortSignal.timeout(2500),
      });
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("access-control-allow-origin"), "*");
      assert.equal(response.headers.get("cache-control"), "no-store");
      return response.json();
    },
  };
}

// Compact poses as the relay header describes them, written out here on their own: binary
// frames decode to {t:"pose"|"where", bin:true, ...} with the whole numbers (n) as sent.
function decodeBinary(b) {
  if (b[0] === 1) {
    const f = b[1], n = [], acks = [];
    for (let i = 0; i < 4; i++) n.push(b.readInt16LE(14 + 2 * i));
    for (let i = 0; i < 3; i++) n.push(b.readInt32LE(22 + 4 * i));
    for (let i = 0; i < 6; i++) n.push(b.readInt16LE(34 + 2 * i));
    for (let j = 0; j < f >> 4; j++) acks.push([b.readUInt32LE(46 + 8 * j), b.readUInt32LE(50 + 8 * j)]);
    return { t: "pose", bin: true, bytes: b, id: b.readUInt32LE(2), k: b.readUInt32LE(6), ts: b.readUInt32LE(10),
             m: f & 1, ghost: (f & 2) !== 0, physics: (f & 4) !== 0, n, acks,
             ...(f & 8 ? { drive: [...b.subarray(b.length - 8)] } : {}) };
  }
  if (b[0] === 2) {
    const p = [];
    for (let at = 1; at < b.length; at += 14) p.push([b.readUInt32LE(at), b.readInt32LE(at + 4), b.readInt32LE(at + 8), b.readUInt16LE(at + 12)]);
    return { t: "where", bin: true, bytes: b, p };
  }
  if (b[0] === 3) return { t: "cops", bin: true, bytes: b, id: b.readUInt32LE(2), n: b[1], ts: b.readUInt32LE(6) };
  return { t: "binary", bin: true, bytes: b };
}

// A compact pose as a page sends it: type 1, flags, id 0, k, ts, then n (4 x i16, 3 x i32, 6 x i16), acks.
function binaryPose({ k = pose.k, ts = 1500, m = 1, ghost = false, n, acks = [], flags, drive }) {
  const b = Buffer.alloc(46 + 8 * acks.length + (drive ? 8 : 0));
  b[0] = 1;
  b[1] = flags ?? (m | (ghost ? 2 : 0) | (drive ? 8 : 0) | (acks.length << 4));
  b.writeUInt32LE(k, 6); b.writeUInt32LE(ts, 10);
  n.forEach((x, i) => (i < 4 ? b.writeInt16LE(x, 14 + 2 * i) : i < 7 ? b.writeInt32LE(x, 22 + 4 * (i - 4)) : b.writeInt16LE(x, 34 + 2 * (i - 7))));
  acks.forEach(([from, seq], j) => { b.writeUInt32LE(from, 46 + 8 * j); b.writeUInt32LE(seq, 50 + 8 * j); });
  if (drive) Buffer.from(drive).copy(b, 46 + 8 * acks.length);
  return b;
}

async function connect(t, relay, room = "lobby", name, physics = false, bin = false, extra = {}) {
  const ws = new WebSocket(relay.ws(room));
  ws.binaryType = "arraybuffer";
  const messages = [];
  ws.addEventListener("message", (e) => messages.push(typeof e.data === "string" ? JSON.parse(e.data) : decodeBinary(Buffer.from(e.data))));
  ws.addEventListener("error", () => {});
  t.after(() => ws.close());
  await until(() => {
    relay.alive();
    assert.notEqual(ws.readyState, WebSocket.CLOSED, "connection closed before opening");
    return ws.readyState === WebSocket.OPEN;
  }, "WebSocket open");
  const client = {
    ws, messages,
    send: (m) => ws.send(JSON.stringify(m)),
    read(type, id) {
      return until(() => {
        relay.alive();
        const i = messages.findIndex((m) => m.t === type && (id === undefined || m.id === id));
        return i >= 0 ? messages.splice(i, 1)[0] : false;
      }, `message ${type}`);
    },
  };
  if (name !== undefined) {
    client.send({ t: "hi", name, ...(physics ? { physics: 1 } : {}), ...(bin ? { bin: 1 } : {}), ...extra });
    client.welcome = await client.read("welcome");
  }
  return client;
}

// Execute the actual worker Blob produced by net.js. Only its browser launch
// and clock are supplied here; WebSocket, shared memory, relay and worker
// message handlers are real. No game data or browser profile is needed.
async function gameLink(t, relay, name, crew = "") {
  let blob;
  const captured = new Error("worker captured");
  const page = vm.createContext({
    location: { search: "" }, recompOnline: { relay: relay.ws(), name },
    window: {}, URLSearchParams, Blob, performance,
    URL: { createObjectURL: (value) => value },
    Worker: class { constructor(value) { blob = value; throw captured; } },
  });
  const clientScript = process.env.CARBON_NET_SCRIPT || new URL("../../source/web/net.js", import.meta.url);
  assert.throws(() => vm.runInContext(fs.readFileSync(clientScript, "utf8"), page), (e) => e === captured);
  const timers = [], sockets = [], posted = [];
  let now = 1000;
  const worker = vm.createContext({
    onmessage: null, postMessage(data) { posted.push(data); }, console, atob, btoa,
    performance: { now: () => now },
    setInterval: (fn) => timers.push(fn), setTimeout() {},
    WebSocket: class extends WebSocket {
      constructor(url) { super(url); sockets.push(this); }
    },
  });
  t.after(() => { for (const socket of sockets) socket.close(); });
  vm.runInContext(await blob.text(), worker);
  const buffer = new SharedArrayBuffer(0x18f0);
  const u32 = new Uint32Array(buffer), i32 = new Int32Array(buffer), f32 = new Float32Array(buffer);
  u32[0] = 0x31504d43; u32[1] = 2;
  worker.onmessage({ data: { t: "block", memory: { buffer }, ptr: 0 } });
  worker.onmessage({ data: { t: "start", relay: relay.ws(), name, crew, ghost: false } });
  const id = await until(() => u32[0x5c / 4] || false, "game link identity");
  return {
    id, u32, i32, f32, posted,
    post(data) { worker.onmessage({ data }); },
    close() { for (const socket of sockets) socket.close(); },
    tick(ms = 40) { now += ms; for (const fn of timers) fn(); },
    publish(ms, s = pose.s, state = 1) {
      Atomics.add(i32, 3, 1);
      u32[4] = state; u32[5] = pose.k; u32[6]++; u32[7] = ms;
      f32.set(s, 8);
      Atomics.add(i32, 3, 1);
    },
    slot(other) {
      for (let i = 0; i < 8; i++) if (u32[(0x60 + i * 0x100) / 4] === other) return i;
      return -1;
    },
  };
}

// A wire client is needed for requests browser WebSocket APIs cannot construct.
function maskedFrame(text, opcode = 1) {
  const payload = Buffer.from(text);
  const n = payload.length;
  assert.ok(n < 65536);
  const header = n < 126 ? Buffer.from([0x80 | opcode, 0x80 | n])
    : Buffer.from([0x80 | opcode, 0xfe, n >> 8, n & 255]);
  const mask = Buffer.from([0x21, 0x43, 0x65, 0x87]);
  for (let i = 0; i < n; i++) payload[i] ^= mask[i % 4];
  return Buffer.concat([header, mask, payload]);
}

async function wire(t, relay, path, { upgrade = true, firstFrame = Buffer.alloc(0) } = {}) {
  const socket = net.createConnection({ host: "127.0.0.1", port: relay.port, allowHalfOpen: true });
  let buffer = Buffer.alloc(0), error;
  socket.on("data", (b) => { buffer = Buffer.concat([buffer, b]); });
  socket.on("error", (e) => { error = e; });
  t.after(() => socket.destroy());
  const headers = [`GET ${path} HTTP/1.1`, `Host: 127.0.0.1:${relay.port}`];
  if (upgrade) headers.push("Upgrade: websocket", "Connection: Upgrade", "Sec-WebSocket-Version: 13",
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==");
  else headers.push("Connection: close");
  socket.write(Buffer.concat([Buffer.from(headers.join("\r\n") + "\r\n\r\n"), firstFrame]));
  const head = await until(() => {
    relay.alive();
    if (error) throw error;
    const end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return false;
    const text = buffer.subarray(0, end).toString();
    buffer = buffer.subarray(end + 4);
    return text;
  }, "HTTP response");
  return {
    socket, status: Number(head.split(" ")[1]),
    readFrame() {
      return until(() => {
        relay.alive();
        if (buffer.length < 2) return false;
        let n = buffer[1] & 127, offset = 2;
        assert.equal(buffer[1] & 128, 0, "server frames must be unmasked");
        assert.notEqual(n, 127, "unexpected large server frame");
        if (n === 126) {
          if (buffer.length < 4) return false;
          n = buffer.readUInt16BE(2); offset = 4;
        }
        if (buffer.length < offset + n) return false;
        const frame = { opcode: buffer[0] & 15, data: buffer.subarray(offset, offset + n) };
        buffer = buffer.subarray(offset + n);
        return frame;
      }, "WebSocket frame");
    },
  };
}

test("existing messages, room isolation, and UI counts survive joins and disconnects", async (t) => {
  const relay = await start(t), room = "friends / café";
  const alice = await connect(t, relay, room, "Alice");
  assert.deepEqual(alice.welcome.peers, []);
  const bob = await connect(t, relay, room, "Bob");
  assert.deepEqual(bob.welcome.peers, [{ id: alice.welcome.id, name: "Alice", crew: "" }]);
  assert.deepEqual(await alice.read("join"), { t: "join", id: bob.welcome.id, name: "Bob", crew: "" });
  const other = await connect(t, relay, "elsewhere", "Other");
  const unnamed = await connect(t, relay, room);
  assert.deepEqual(await relay.stats(room), { players: 3, here: 2 });
  assert.deepEqual(await relay.stats("elsewhere"), { players: 3, here: 1 });
  alice.send(pose);
  assert.deepEqual(await bob.read("pose"), { ...pose, id: alice.welcome.id });
  // A later reply on each ordered connection proves all earlier frames arrived.
  other.send({ t: "hi", name: "Other" });
  await other.read("welcome");
  assert.equal(other.messages.some((m) => m.t === "pose"), false);
  // Nothing reaches a connection before its hi; with it, it gets Alice's newest pose
  // right after the welcome (her car enters its interest set).
  unnamed.send({ t: "hi", name: "Now named" });
  await until(() => unnamed.messages.length, "the welcome");
  assert.equal(unnamed.messages[0].t, "welcome");
  await unnamed.read("welcome");
  assert.deepEqual(await unnamed.read("pose"), { ...pose, id: alice.welcome.id });
  alice.send({ ...pose, m: 0, ts: 1533 });
  assert.equal((await bob.read("pose")).m, 0);
  bob.ws.close();
  assert.deepEqual(await alice.read("left"), { t: "left", id: bob.welcome.id });
  assert.deepEqual(await relay.stats(room), { players: 3, here: 2 });
});

test("authentication and a room limit of eight (CARBON_ROOM_MAX) preserve room capacity", async (t) => {
  const relay = await start(t, { CARBON_ROOM_MAX: "8" });
  const refused = await wire(t, relay, "/lobby?key=wrong");
  assert.equal(refused.status, 403);
  assert.deepEqual(await relay.stats(), { players: 0, here: 0 });
  const clients = [];
  for (let i = 0; i < 8; i++) clients.push(await connect(t, relay, "lobby", `Player ${i}`));
  const ninth = new WebSocket(relay.ws());
  let admitted = false;
  ninth.addEventListener("open", () => { admitted = true; });
  ninth.addEventListener("error", () => {});
  t.after(() => ninth.close());
  await until(() => ninth.readyState === WebSocket.CLOSED, "full room rejection");
  assert.equal(admitted, false);
  assert.deepEqual(await relay.stats(), { players: 8, here: 8 });
  clients[7].ws.close();
  await clients[0].read("left", clients[7].welcome.id);
  const replacement = await connect(t, relay, "lobby", "Replacement");
  assert.equal(replacement.welcome.peers.length, 7);
  assert.deepEqual(await relay.stats(), { players: 8, here: 8 });
});

test("an engine impact reaches its owner once, then the owner pose acknowledges it", async (t) => {
  const relay = await start(t);
  const a = await connect(t, relay, "lobby", "Driver", true);
  const b = await connect(t, relay, "lobby", "Struck driver", true);
  const spectator = await connect(t, relay, "lobby", "Spectator", true);
  a.send(pose); b.send(pose);
  await a.read("pose", b.welcome.id); await b.read("pose", a.welcome.id);
  const hit = { t: "impact", to: b.welcome.id, seq: 1, k: pose.k, ts: pose.ts,
    s: [12, 3, -40, 0.1, 0, 0, 6, 0, 0, 0, 0.4, 0] };
  a.send(hit);
  assert.deepEqual(await b.read("impact"), { t: "impact", id: a.welcome.id,
    seq: 1, k: pose.k, ts: pose.ts, s: hit.s });
  a.send(hit); // a retry must not apply the force a second time
  a.send({ ...pose, ts: 1533 });
  await b.read("pose", a.welcome.id);
  await spectator.read("pose", a.welcome.id);
  // The second pose is an ordering barrier on the spectator connection too.
  await spectator.read("pose", a.welcome.id);
  assert.equal(b.messages.some((m) => m.t === "impact"), false);
  assert.equal(spectator.messages.some((m) => m.t === "impact"), false);
  b.send({ ...pose, ts: 1533, acks: [[a.welcome.id, 1], [a.welcome.id, 99]] });
  const acknowledged = await a.read("pose", b.welcome.id);
  assert.deepEqual(acknowledged.acks, [[a.welcome.id, 1]]);
  assert.equal(a.messages.some((m) => m.t === "impact"), false);
});

test("game workers retain a hit queued before its frame and return the owner's acknowledgement", async (t) => {
  const relay = await start(t);
  const a = await gameLink(t, relay, "A"), b = await gameLink(t, relay, "B");
  a.publish(1500); b.publish(1500); a.tick(); b.tick();
  await until(() => a.slot(b.id) >= 0 && b.slot(a.id) >= 0, "both game poses");
  // C publishes the TX queue first, then the new pose. The worker can poll
  // between those writes. It must wait for that frame without losing the hit.
  const values = [12, 3, -40, 0.125, 0, 0, 6, 0, 0, 0, 0.5, 0];
  const tx = 0x8f0 / 4, rx = 0x10f0 / 4, queue = 0x8e0 / 4;
  a.u32.set([b.id, 1, pose.k, 1517], tx); a.f32.set(values, tx + 4);
  Atomics.store(a.i32, queue, 1);
  a.tick();
  assert.equal(Atomics.load(a.i32, queue + 1), 0, "an unpublished frame must keep its queued hit");
  a.publish(1517); a.tick();
  await until(() => Atomics.load(b.i32, queue + 2) === 1, "impact in the owner's game queue");
  assert.deepEqual(Array.from(b.u32.subarray(rx, rx + 4)), [a.id, 1, pose.k, 1500]);
  assert.deepEqual(Array.from(b.f32.subarray(rx + 4, rx + 16)), values);
  assert.equal(Atomics.load(a.i32, queue + 1), 1);
  // The engine consumes the hit and publishes the response with its receipt.
  Atomics.store(b.i32, queue + 3, 1);
  b.u32[(0x860 + b.slot(a.id) * 16 + 12) / 4] = 1;
  b.publish(1533); b.tick();
  const peer = (0x60 + a.slot(b.id) * 0x100) / 4;
  await until(() => a.u32[peer + 6] === 1, "owner acknowledgement in the source game");
  a.tick(); b.tick();
  assert.equal(Atomics.load(b.i32, queue + 2), 1, "the hit must not be sent twice");
});

test("impacts require one authority, a nearby active owner, and bounded engine changes", async (t) => {
  const relay = await start(t);
  const a = await connect(t, relay, "lobby", "A", true);
  const b = await connect(t, relay, "lobby", "B", true);
  const elsewhere = await connect(t, relay, "other", "Elsewhere", true);
  const legacy = await connect(t, relay, "lobby", "Old client");
  for (const p of [a, b, elsewhere, legacy]) p.send(pose);
  await a.read("pose", b.welcome.id); await b.read("pose", a.welcome.id);
  const hit = { t: "impact", to: b.welcome.id, seq: 1, k: pose.k, ts: pose.ts,
    s: [12, 3, -40, 0.1, 0, 0, 6, 0, 0, 0, 0.4, 0] };
  const invalid = [
    { to: elsewhere.welcome.id }, { to: legacy.welcome.id }, { k: pose.k + 1 },
    { ts: pose.ts - 2000 }, { seq: 0 }, { s: [...hit.s.slice(0, 6), 101, 0, 0, 0, 0, 0] },
    { s: [12, 3, -40, 7, 0, 0, ...hit.s.slice(6)] }, { s: Array(12).fill(null) },
  ];
  for (const bad of invalid) a.send({ ...hit, ...bad });
  a.send({ ...pose, ts: 1533 }); await b.read("pose", a.welcome.id);
  assert.equal(b.messages.some((m) => m.t === "impact"), false);
  elsewhere.send({ t: "hi", name: "Elsewhere", physics: 1 }); await elsewhere.read("welcome");
  assert.equal(elsewhere.messages.some((m) => m.t === "impact"), false);
  b.send({ ...hit, to: a.welcome.id }); // the other browser must not solve the same pair
  b.send({ ...pose, ts: 1533 }); await a.read("pose", b.welcome.id);
  assert.equal(a.messages.some((m) => m.t === "impact"), false);
  for (const state of [{ ghost: true }, { m: 0 }, { s: [0, 0, 0, 1, 1000, 3, -40, 0, 0, 0, 0, 0, 0] }]) {
    b.send({ ...pose, ...state }); await a.read("pose", b.welcome.id);
    a.send(hit); a.send(pose); await b.read("pose", a.welcome.id);
    assert.equal(b.messages.some((m) => m.t === "impact"), false);
  }
  b.send(pose); await a.read("pose", b.welcome.id);
  a.send(hit); assert.equal((await b.read("impact")).seq, 1);
  await delay(1050); // stale parked positions must not admit a delayed hit
  a.send(pose); a.send({ ...hit, seq: 2 }); a.send({ ...pose, ts: 1533 });
  await b.read("pose", a.welcome.id); await b.read("pose", a.welcome.id);
  assert.equal(b.messages.some((m) => m.t === "impact"), false);
});

test("malformed room encoding cannot crash the relay before authentication", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay, "lobby", "Bob");
  for (const path of ["/%zz", `/%E0%A4?key=${key}`]) {
    const response = await wire(t, relay, path);
    assert.equal(response.status, 400);
    assert.deepEqual(await relay.stats(), { players: 2, here: 2 });
    alice.send(pose);
    assert.deepEqual(await bob.read("pose"), { ...pose, id: alice.welcome.id });
  }
});

test("invalid request URLs are rejected on both HTTP and WebSocket routes", async (t) => {
  const relay = await start(t);
  for (const upgrade of [false, true]) {
    const response = await wire(t, relay, "//[", { upgrade });
    assert.equal(response.status, 400);
    assert.deepEqual(await relay.stats(), { players: 0, here: 0 });
  }
});

test("null and other non-message JSON do not stop subsequent position delivery", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay, "lobby", "Bob");
  for (const text of ["null", "[]", "false", "42", '"hi"', "{broken", '{}']) {
    alice.ws.send(text);
    alice.send(pose);
    assert.deepEqual(await bob.read("pose"), { ...pose, id: alice.welcome.id });
  }
  assert.deepEqual(await relay.stats(), { players: 2, here: 2 });
});

test("object-valued names and pose fields cannot invoke failing coercions", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay);
  const object = { toString: null, valueOf: null };
  bob.send({ t: "hi", name: object });
  const welcome = await bob.read("welcome");
  assert.equal((await alice.read("join", welcome.id)).name, "Player");
  for (const bad of [{ k: object }, { m: object }, { k: -1 }, { m: 2 }, { s: [1, 2] }, { s: Array(13).fill(null) }])
    bob.send({ ...pose, ...bad });
  bob.send(pose);
  assert.deepEqual(await alice.read("pose"), { ...pose, id: welcome.id });
  assert.deepEqual(await relay.stats(), { players: 2, here: 2 });
});

test("a hello delivered with the HTTP upgrade is not lost", async (t) => {
  const relay = await start(t);
  const peer = await wire(t, relay, `/lobby?key=${key}`, {
    firstFrame: maskedFrame(JSON.stringify({ t: "hi", name: "Early hello" })),
  });
  assert.equal(peer.status, 101);
  const frame = await peer.readFrame();
  assert.equal(frame.opcode, 1);
  assert.deepEqual(JSON.parse(frame.data), { t: "welcome", id: 1, peers: [] });
  assert.deepEqual(await relay.stats(), { players: 1, here: 1 });
});

test("partial frames, ping, and oversize rejection leave other players connected", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const peer = await wire(t, relay, `/lobby?key=${key}`);
  const hello = maskedFrame(JSON.stringify({ t: "hi", name: "Wire client" }));
  peer.socket.write(hello.subarray(0, 3));
  await delay(20);
  peer.socket.write(hello.subarray(3));
  const welcome = JSON.parse((await peer.readFrame()).data);
  await alice.read("join", welcome.id);
  peer.socket.write(maskedFrame("test ping", 9));
  const pong = await peer.readFrame();
  assert.equal(pong.opcode, 10);
  assert.equal(pong.data.toString(), "test ping");
  peer.socket.write(maskedFrame("x".repeat(4097)));
  assert.equal((await peer.readFrame()).opcode, 8);
  await alice.read("left", welcome.id);
  assert.deepEqual(await relay.stats(), { players: 1, here: 1 });
});

test("a closed connection cannot rejoin or crash a deleted room", async (t) => {
  const relay = await start(t);
  const peer = await wire(t, relay, `/lobby?key=${key}`);
  peer.socket.write(maskedFrame(JSON.stringify({ t: "hi", name: "Leaving" })));
  assert.equal(JSON.parse((await peer.readFrame()).data).t, "welcome");
  peer.socket.write(maskedFrame("", 8));
  assert.equal((await peer.readFrame()).opcode, 8);
  peer.socket.write(maskedFrame(JSON.stringify({ t: "hi", name: "Too late" })));
  // TCP is allowed to remain writable after the server sends its close frame.
  await delay(30);
  assert.deepEqual(await relay.stats(), { players: 0, here: 0 });
});

test("crews, chat and the horn reach the room, cleaned and rate-limited", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const ws = new WebSocket(relay.ws());
  const got = [];
  ws.addEventListener("message", (e) => got.push(JSON.parse(e.data)));
  await new Promise((resolve) => ws.addEventListener("open", resolve, { once: true }));
  t.after(() => ws.close());
  ws.send(JSON.stringify({ t: "hi", name: "Bob", crew: "  Night<Owls>!  " }));
  assert.deepEqual(await alice.read("join"), { t: "join", id: 2, name: "Bob", crew: "NightOwls" });
  ws.send(JSON.stringify({ t: "chat", text: "Race\u0000 me!\n" + "x".repeat(200) }));
  const chat = await alice.read("chat");
  assert.equal(chat.id, 2);
  assert.ok(chat.text.startsWith("Race  me!"));
  assert.equal(chat.text.length, 120);
  ws.send(JSON.stringify({ t: "horn", on: "yes" }));
  ws.send(JSON.stringify({ t: "horn", on: true }));
  assert.deepEqual(await alice.read("horn"), { t: "horn", id: 2, on: true });
  for (let i = 0; i < 10; i++) ws.send(JSON.stringify({ t: "chat", text: `spam ${i}` }));
  await delay(300);
  const spam = [];
  for (;;) {
    const m = await Promise.race([alice.read("chat"), delay(200).then(() => null)]);
    if (!m) break;
    spam.push(m.text);
  }
  assert.ok(spam.length <= 4, `at most the rest of the burst gets through, got ${spam.length}`);
  assert.ok(got.some((m) => m.t === "welcome" && m.peers[0].crew === ""));
});

// ---- Cloud saves (CARBON_SAVES_DIR): sealed blobs by id over plain HTTP ----
const SAVES_MAX = 8 * 1024 * 1024;
const sha256 = (data) => crypto.createHash("sha256").update(data).digest("hex");
const saveId = (n) => sha256(`save ${n}`);

function savesDir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "carbon-saves-test-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function savesUrl(relay, id, suppliedKey = key) {
  return `http://127.0.0.1:${relay.port}/saves/${id}?key=${encodeURIComponent(suppliedKey)}`;
}

// A request fetch() cannot make: no Content-Length, or a body cut off part way.
function raw(relay, path, { method = "PUT", headers = {}, body = [], cut = false } = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port: relay.port, path, method, headers }, (res) => {
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString() }));
    });
    req.on("error", (error) => (cut ? resolve({ status: 0 }) : reject(error)));
    req.flushHeaders();
    for (const part of body) req.write(part);
    if (cut) setTimeout(() => { req.destroy(); resolve({ status: 0 }); }, 100);
    else if (!headers["content-length"] || body.length) req.end();
  });
}

test("cloud saves: PUT and GET round-trip with CORS, the key, ids and conditional writes", async (t) => {
  const dir = savesDir(t);
  const relay = await start(t, { CARBON_SAVES_DIR: dir });
  const id = saveId(1), url = savesUrl(relay, id);
  // Chrome's preflight for a PUT with a binary body and If-Match; it carries no key check.
  const pre = await fetch(`http://127.0.0.1:${relay.port}/saves/${id}`, { method: "OPTIONS", headers: {
    origin: "http://localhost:8098", "access-control-request-method": "PUT",
    "access-control-request-headers": "content-type,if-match" } });
  assert.equal(pre.status, 204);
  assert.equal(pre.headers.get("access-control-allow-origin"), "*");
  assert.match(pre.headers.get("access-control-allow-methods"), /\bPUT\b/);
  assert.match(pre.headers.get("access-control-allow-headers"), /content-type/i);
  assert.match(pre.headers.get("access-control-allow-headers"), /if-match/i);
  let r = await fetch(url);
  assert.equal(r.status, 404);
  assert.equal(r.headers.get("access-control-allow-origin"), "*");
  for (const wrong of ["", "wrong"]) {
    r = await fetch(savesUrl(relay, id, wrong), { method: "PUT", body: "sealed" });
    assert.equal(r.status, 403);
    assert.equal(r.headers.get("access-control-allow-origin"), "*");
  }
  for (const bad of ["abc", id.toUpperCase(), id + "0", `${id.slice(0, 58)}%2e%2e%2f`, ""]) {
    r = await fetch(savesUrl(relay, bad), { method: "PUT", body: "sealed" });
    assert.equal(r.status, 400, `id ${bad}`);
  }
  const one = crypto.randomBytes(5000);
  r = await fetch(url, { method: "PUT", body: one, headers: { "content-type": "application/octet-stream", "if-none-match": "*" } });
  assert.equal(r.status, 201);
  const etag1 = r.headers.get("etag");
  assert.equal(etag1, `"${sha256(one)}"`);
  const put = await r.json();
  assert.equal(put.size, 5000);
  assert.equal(put.etag, etag1);
  r = await fetch(url, { method: "PUT", body: one, headers: { "if-none-match": "*" } });
  assert.equal(r.status, 412, "If-None-Match: * refuses to replace a stored save");
  r = await fetch(url);
  assert.equal(r.status, 200);
  assert.ok(Buffer.from(await r.arrayBuffer()).equals(one));
  assert.equal(r.headers.get("etag"), etag1);
  assert.equal(r.headers.get("content-type"), "application/octet-stream");
  assert.equal(r.headers.get("cross-origin-resource-policy"), "cross-origin");
  assert.equal(r.headers.get("cache-control"), "no-store");
  assert.match(r.headers.get("access-control-expose-headers"), /etag/i);
  assert.ok(Date.parse(r.headers.get("last-modified")) > Date.now() - 60000);
  r = await fetch(url, { method: "HEAD" });
  assert.equal(r.status, 200);
  assert.equal(r.headers.get("etag"), etag1);
  assert.equal(r.headers.get("content-length"), "5000");
  const two = crypto.randomBytes(7000);
  r = await fetch(url, { method: "PUT", body: two, headers: { "if-match": `"${"0".repeat(64)}"` } });
  assert.equal(r.status, 412, "a stale If-Match is refused");
  assert.equal(r.headers.get("etag"), etag1, "and told the current etag");
  r = await fetch(url, { method: "PUT", body: two, headers: { "if-match": etag1 } });
  assert.equal(r.status, 200);
  assert.equal(r.headers.get("etag"), `"${sha256(two)}"`);
  r = await fetch(url);
  assert.ok(Buffer.from(await r.arrayBuffer()).equals(two));
  assert.deepEqual(fs.readdirSync(dir), [id], "one file per save, no temporary files left");
  assert.ok(fs.readFileSync(path.join(dir, id)).equals(two));
  r = await fetch(url, { method: "POST", body: "x" });
  assert.equal(r.status, 405);
  assert.match(r.headers.get("allow"), /\bDELETE\b/);
  assert.deepEqual(await relay.stats(), { players: 0, here: 0 }, "the rest of the relay still answers");
});

test("cloud saves: size limits, cut-off uploads keep the old save, and stale temporary files go", async (t) => {
  const dir = savesDir(t);
  const id = saveId(2), stale = path.join(dir, `.${id}.abc.tmp`);
  fs.writeFileSync(stale, "left by a crash");
  const relay = await start(t, { CARBON_SAVES_DIR: dir });
  assert.equal(fs.existsSync(stale), false);
  const url = savesUrl(relay, id), where = new URL(url);
  const first = crypto.randomBytes(3000);
  let r = await fetch(url, { method: "PUT", body: first });
  assert.equal(r.status, 201);
  const etag = r.headers.get("etag");
  let res = await raw(relay, where.pathname + where.search, { headers: { "content-length": String(SAVES_MAX + 1) } });
  assert.equal(res.status, 413);
  res = await raw(relay, where.pathname + where.search, { headers: { "content-length": "0" } });
  assert.equal(res.status, 413);
  res = await raw(relay, where.pathname + where.search, { body: [Buffer.from("chunked, no length")] });
  assert.equal(res.status, 411);
  // The connection drops after 4000 of 10000 bytes: the old save stays whole.
  await raw(relay, where.pathname + where.search, { headers: { "content-length": "10000" },
    body: [crypto.randomBytes(4000)], cut: true });
  await until(() => fs.readdirSync(dir).length === 1, "temporary file removed");
  r = await fetch(url);
  assert.equal(r.headers.get("etag"), etag);
  assert.ok(Buffer.from(await r.arrayBuffer()).equals(first));
  const biggest = crypto.randomBytes(SAVES_MAX);
  r = await fetch(url, { method: "PUT", body: biggest, headers: { "if-match": etag } });
  assert.equal(r.status, 200, "exactly the limit is accepted");
  r = await fetch(url, { method: "HEAD" });
  assert.equal(r.headers.get("content-length"), String(SAVES_MAX));
  assert.deepEqual(fs.readdirSync(dir), [id]);
});

test("cloud saves: DELETE removes a save, with the key, If-Match, CORS, the write lock and the space it used", async (t) => {
  const dir = savesDir(t);
  // 1 MiB for all saves together: two 600 KB saves do not fit, one does.
  const relay = await start(t, { CARBON_SAVES_DIR: dir, CARBON_SAVES_MAX_MB: "1" });
  const a = saveId(4), b = saveId(5), urlA = savesUrl(relay, a), urlB = savesUrl(relay, b);
  const pre = await fetch(`http://127.0.0.1:${relay.port}/saves/${a}`, { method: "OPTIONS", headers: {
    origin: "http://localhost:8098", "access-control-request-method": "DELETE", "access-control-request-headers": "if-match" } });
  assert.equal(pre.status, 204);
  assert.match(pre.headers.get("access-control-allow-methods"), /\bDELETE\b/);
  assert.match(pre.headers.get("access-control-allow-headers"), /if-match/i);
  const blob = crypto.randomBytes(600 * 1024);
  let r = await fetch(urlA, { method: "PUT", body: blob });
  assert.equal(r.status, 201);
  const etag = r.headers.get("etag");
  r = await fetch(urlB, { method: "PUT", body: blob });
  assert.equal(r.status, 507, "the second save does not fit yet");
  for (const wrong of ["", "wrong"]) {
    r = await fetch(savesUrl(relay, a, wrong), { method: "DELETE" });
    assert.equal(r.status, 403);
    assert.equal(r.headers.get("access-control-allow-origin"), "*");
  }
  r = await fetch(savesUrl(relay, "abc"), { method: "DELETE" });
  assert.equal(r.status, 400);
  r = await fetch(urlA, { method: "DELETE", headers: { "if-match": `"${"0".repeat(64)}"` } });
  assert.equal(r.status, 412, "a stale If-Match removes nothing");
  assert.equal(r.headers.get("etag"), etag, "and is told the current etag");
  assert.deepEqual(fs.readdirSync(dir), [a]);
  // While an upload to the id is being written, a DELETE waits its turn (409).
  const where = new URL(urlA);
  const upload = http.request({ host: "127.0.0.1", port: relay.port, path: where.pathname + where.search, method: "PUT",
                                headers: { "content-length": "10000" } });
  upload.on("error", () => {});
  upload.write(crypto.randomBytes(2000));
  await until(() => fs.readdirSync(dir).some((name) => name.endsWith(".tmp")), "upload in progress");
  r = await fetch(urlA, { method: "DELETE" });
  assert.equal(r.status, 409);
  upload.destroy();
  await until(() => fs.readdirSync(dir).length === 1, "cut-off upload cleaned up");
  r = await fetch(urlA, { method: "DELETE", headers: { "if-match": etag } });
  assert.equal(r.status, 204);
  assert.equal(r.headers.get("access-control-allow-origin"), "*");
  assert.equal(r.headers.get("cache-control"), "no-store");
  assert.equal(await r.text(), "");
  assert.deepEqual(fs.readdirSync(dir), [], "the blob is gone from the disk");
  assert.equal((await fetch(urlA)).status, 404);
  assert.equal((await fetch(urlA, { method: "HEAD" })).status, 404);
  r = await fetch(urlA, { method: "DELETE" });
  assert.equal(r.status, 404, "nothing left to remove");
  assert.equal(r.headers.get("access-control-allow-origin"), "*");
  r = await fetch(urlB, { method: "PUT", body: blob });
  assert.equal(r.status, 201, "the space the removed save used is free again");
  r = await fetch(urlB, { method: "DELETE" });
  assert.equal(r.status, 204, "If-Match is optional");
  r = await fetch(urlA, { method: "PUT", body: blob, headers: { "if-none-match": "*" } });
  assert.equal(r.status, 201, "a removed id can be used again");
  assert.deepEqual(fs.readdirSync(dir), [a]);
});

test("cloud saves: per-client rate limit, and off without CARBON_SAVES_DIR", async (t) => {
  const relay = await start(t, { CARBON_SAVES_DIR: savesDir(t) });
  const url = savesUrl(relay, saveId(3));
  const statuses = [];
  for (let i = 0; i < 30; i++) statuses.push((await fetch(url)).status);
  const passed = statuses.filter((s) => s === 404).length;
  assert.ok(passed >= 20 && passed <= 22, `the burst passes, then the limit holds: ${statuses.join(" ")}`);
  const limited = await fetch(url);
  assert.equal(limited.status, 429);
  assert.ok(Number(limited.headers.get("retry-after")) >= 1);
  assert.equal(limited.headers.get("access-control-allow-origin"), "*");
  // Behind nginx (a connection from this machine) each player counts by X-Real-IP.
  assert.equal((await fetch(url, { headers: { "x-real-ip": "203.0.113.9" } })).status, 404);
  const off = await start(t, { CARBON_SAVES_DIR: "" });
  const r = await fetch(savesUrl(off, saveId(3)));
  assert.equal(r.status, 501);
  assert.equal(r.headers.get("access-control-allow-origin"), "*");
});

test("a player can change crew while connected; everyone learns the cleaned name", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay, "lobby", "Bob");
  bob.send({ t: "crew", crew: " Night<Owls> " });
  assert.deepEqual(await alice.read("crew"), { t: "crew", id: bob.welcome.id, crew: "NightOwls" });
  assert.deepEqual(await bob.read("crew"), { t: "crew", id: bob.welcome.id, crew: "NightOwls" });
  bob.send({ t: "crew", crew: 42 });
  bob.send({ t: "crew", crew: "" });
  assert.deepEqual(await alice.read("crew"), { t: "crew", id: bob.welcome.id, crew: "" });
  const carol = await connect(t, relay, "lobby", "Carol");
  assert.deepEqual(carol.welcome.peers.find((p) => p.id === bob.welcome.id).crew, "");
});

test("race messages reach the whole room, the sender too, size-capped and rate-limited", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay, "lobby", "Bob");
  const carol = await connect(t, relay, "elsewhere", "Carol");
  // Another id or t from the sender never passes; everything else does, as sent.
  alice.send({ t: "race", k: "lobby", id: 99, race: 7, key: 0x8f2c11aa, laps: 2, opts: "02000000", ai: false });
  const lobby = { t: "race", id: alice.welcome.id, k: "lobby", race: 7, key: 0x8f2c11aa, laps: 2, opts: "02000000", ai: false };
  assert.deepEqual(await bob.read("race"), lobby);
  assert.deepEqual(await alice.read("race"), lobby);
  // Refused: no kind, a kind that is not a short lowercase word, over 1024 bytes, before hi.
  alice.send({ t: "race" });
  alice.send({ t: "race", k: "Go!" });
  alice.send({ t: "race", k: "x".repeat(13) });
  alice.send({ t: "race", k: 5 });
  alice.send({ t: "race", k: "progress", pad: "x".repeat(1024) });
  const silent = await connect(t, relay, "lobby");
  silent.send({ t: "race", k: "go" });
  bob.send({ t: "race", k: "ready", race: 7 });
  assert.deepEqual(await alice.read("race"), { t: "race", id: bob.welcome.id, k: "ready", race: 7 });
  assert.deepEqual(await bob.read("race"), { t: "race", id: bob.welcome.id, k: "ready", race: 7 });
  await delay(100);
  assert.equal(alice.messages.filter((m) => m.t === "race").length, 0, "only valid race messages arrive");
  // A burst of 20 per player, then 10 a second: bob has used one.
  for (let i = 0; i < 40; i++) bob.send({ t: "race", k: "progress", n: i });
  await delay(400);
  const got = alice.messages.filter((m) => m.t === "race" && m.k === "progress").length;
  assert.ok(got >= 19 && got <= 24, `the burst and a little more gets through, got ${got}`);
  assert.equal(carol.messages.filter((m) => m.t === "race").length, 0, "other rooms hear nothing");
  assert.equal(silent.messages.filter((m) => m.t === "race").length, 0, "a connection that never said hi hears nothing");
});

test("car looks reach the others, wait for players who join later, and are checked and rate-limited", async (t) => {
  const relay = await start(t);
  const alice = await connect(t, relay, "lobby", "Alice");
  const bob = await connect(t, relay, "lobby", "Bob");
  const olga = await connect(t, relay, "elsewhere", "Olga");
  const silent = await connect(t, relay, "lobby");
  // A look record as look.h sends one: format, part database, 0x42c bytes of customization.
  const record = Buffer.alloc(0x434, 7);
  record.writeUInt32LE(1, 0);
  const data = record.toString("base64"), k = 0x90944519;
  alice.send({ t: "look", k, data });
  assert.deepEqual(await bob.read("look"), { t: "look", id: alice.welcome.id, k, data });
  // Refused: no or a bad model key, data that is not base64 or stands for over 2048 bytes, before hi.
  alice.send({ t: "look", data });
  alice.send({ t: "look", k: -1, data });
  alice.send({ t: "look", k: 1.5, data });
  alice.send({ t: "look", k: 2 ** 32, data });
  alice.send({ t: "look", k: "5", data });
  alice.send({ t: "look", k: 5 });
  alice.send({ t: "look", k: 5, data: 42 });
  alice.send({ t: "look", k: 5, data: "not base64!" });
  alice.send({ t: "look", k: 5, data: "QUJD=" });
  alice.send({ t: "look", k: 5, data: "QQ===" });
  alice.send({ t: "look", k: 5, data: Buffer.alloc(2049).toString("base64") });
  silent.send({ t: "look", k: 5, data });
  const largest = Buffer.alloc(2048, 1).toString("base64");
  alice.send({ t: "look", k: 6, data: largest });
  assert.deepEqual(await bob.read("look"), { t: "look", id: alice.welcome.id, k: 6, data: largest });
  await delay(100);
  assert.equal(bob.messages.filter((m) => m.t === "look").length, 0, "only valid looks arrive");
  assert.equal(alice.messages.filter((m) => m.t === "look").length, 0, "the sender does not get its own back");
  assert.equal(olga.messages.filter((m) => m.t === "look").length, 0, "other rooms hear nothing");
  assert.equal(silent.messages.filter((m) => m.t === "look").length, 0, "a connection that never said hi hears nothing");
  // A stock car: "" passes on but is not kept.
  bob.send({ t: "look", k: 0x12345678, data: "" });
  assert.deepEqual(await alice.read("look"), { t: "look", id: bob.welcome.id, k: 0x12345678, data: "" });
  // A player who joins later gets the newest look of each car here, right after its welcome.
  alice.send({ t: "look", k, data });
  assert.deepEqual(await bob.read("look"), { t: "look", id: alice.welcome.id, k, data });
  const carol = await connect(t, relay, "lobby", "Carol");
  assert.equal(carol.messages[0]?.t, "look", "the looks come right after the welcome");
  assert.deepEqual(await carol.read("look"), { t: "look", id: alice.welcome.id, k, data });
  await delay(100);
  assert.equal(carol.messages.filter((m) => m.t === "look").length, 0, "only kept looks, once each");
  // Alice has used 3 of her 4; then one per 5 s.
  for (let i = 0; i < 6; i++) alice.send({ t: "look", k, data: Buffer.alloc(16, i).toString("base64") });
  await delay(300);
  const passed = bob.messages.filter((m) => m.t === "look");
  assert.equal(passed.length, 1, `a burst of 4, then one per 5 s; got ${passed.length}`);
  assert.equal(passed[0].data, Buffer.alloc(16, 0).toString("base64"));
  // A player who leaves takes the look along.
  alice.ws.close();
  await bob.read("left", alice.welcome.id);
  const dave = await connect(t, relay, "lobby", "Dave");
  await delay(150);
  assert.equal(dave.messages.filter((m) => m.t === "look").length, 0, "no look of a player who left");
});

test("game workers send their car's look and hand the game each other player's", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  // look.h LookShared: magic, version, seq, key, size, stamp, parts, db, record at 0x20,
  // then 8 peer entries of 0x460 from 0x460: id, seq, key, size, stamp, 3 spare, record at +0x20.
  function lookBlock(link) {
    const buffer = new SharedArrayBuffer(0x460 * 9), u32 = new Uint32Array(buffer), u8 = new Uint8Array(buffer);
    u32[0] = 0x4b4c4d43; u32[1] = 1;
    link.post({ t: "look-block", memory: { buffer }, ptr: 0 });
    return { u32, u8 };
  }
  const a = lookBlock(ana), b = lookBlock(ben);
  const record = Uint8Array.from({ length: 0x434 }, (_, i) => (i * 7) & 255);
  a.u32[2] = 1; a.u32[3] = 0x90944519; a.u32[4] = record.length; a.u32[5] = 1; a.u8.set(record, 0x20); a.u32[2] = 2;
  ana.tick(500);
  await until(() => b.u32[0x460 / 4] === ana.id, "Ana's look in Ben's game");
  assert.equal(b.u32[0x460 / 4 + 1] % 2, 0, "the entry is not left half written");
  assert.equal(b.u32[0x460 / 4 + 2], 0x90944519);
  assert.equal(b.u32[0x460 / 4 + 3], record.length);
  assert.ok(b.u32[0x460 / 4 + 4] > 0, "a stamp for the game to tell looks apart");
  assert.deepEqual(b.u8.slice(0x480, 0x480 + record.length), record);
  // A player who joins later gets it from the relay, and a stock car ("") empties the bytes.
  const cat = await gameLink(t, relay, "Cat"), c = lookBlock(cat);
  await until(() => c.u32[0x460 / 4] === ana.id && c.u32[0x460 / 4 + 3] === record.length, "Ana's look for Cat");
  a.u32[2] = 3; a.u32[4] = 0; a.u32[5] = 2; a.u32[2] = 4;
  ana.tick(5000);
  await until(() => b.u32[0x460 / 4 + 3] === 0 && b.u32[0x460 / 4] === ana.id, "Ana's stock look");
});

// ---- Rooms of a hundred and more: interest sets, look-on-enter, the map feed, chat scope ----
// A pose standing still at (x, y, z), nose along +z unless q says otherwise.
const still = (x, z = 0, extra = {}, q = [0, 0, 0, 1], y = 0) => ({ ...pose, s: [...q, x, y, z, 0, 0, 0, 0, 0, 0], ...extra });

async function join(t, relay, name, crew = "", room = "lobby") {
  const client = await connect(t, relay, room);
  client.send({ t: "hi", name, crew });
  client.welcome = await client.read("welcome");
  return client;
}

// Proves every frame `from` sent before has reached `to`, if the relay passed it on:
// a race message goes to the whole room in order behind it.
let syncs = 0;
async function sync(from, to) {
  const n = ++syncs;
  from.send({ t: "race", k: "sync", n });
  await until(() => {
    const i = to.messages.findIndex((m) => m.t === "race" && m.n === n);
    return i >= 0 && to.messages.splice(i, 1);
  }, "sync");
}
const posesFrom = (client, from) => client.messages.filter((m) => m.t === "pose" && m.id === from.welcome.id);
const interestTick = () => delay(650); // one pick (every 500 ms) has surely run

// ---- Shared police chase (pursuit.h, net.js; the relay header's "Shared police chase") ----
test("shared police chase: a car's chase state reaches the players that have it, a report only the player it is about", async (t) => {
  const relay = await start(t);
  const ana = await connect(t, relay, "lobby", "Ana");
  const ben = await connect(t, relay, "lobby", "Ben");
  const olga = await connect(t, relay, "elsewhere", "Olga");
  const silent = await connect(t, relay, "lobby");
  const anaId = ana.welcome.id, benId = ben.welcome.id;
  await interestTick(); // each now has the other's car (a pursuit message is never kept for later)
  // STATE about the sender's own car: to the players that get it.
  ana.send({ t: "pursuit", to: 0, k: 1, a: 1, b: 2, f: [3.5, 1.25, 0, 4] });
  assert.deepEqual(await ben.read("pursuit"), { t: "pursuit", id: anaId, k: 1, a: 1, b: 2, f: [3.5, 1.25, 0, 4] });
  // REPORT about another player's car: to that player alone.
  ben.send({ t: "pursuit", to: anaId, k: 2, a: 1, b: 3, f: [0.5, 12.25, 0, 0] });
  assert.deepEqual(await ana.read("pursuit"), { t: "pursuit", id: benId, k: 2, a: 1, b: 3, f: [0.5, 12.25, 0, 0] });
  // Refused: bad k, a, b, to or f; a player in another room; before hi.
  const good = { t: "pursuit", to: 0, k: 1, a: 0, b: 0, f: [1, 0, 0, 0] };
  for (const bad of [{ k: 0 }, { k: 16 }, { k: 1.5 }, { k: "1" }, { a: -1 }, { a: 65536 }, { b: 0.5 }, { b: "2" },
    { f: [1, 0, 0] }, { f: [1, 0, 0, null] }, { f: [1, 0, 0, 2e6] }, { f: "x" }, { to: -1 }, { to: "x" }, { to: undefined },
    { to: olga.welcome.id }])
    ana.send({ ...good, ...bad });
  silent.send(good);
  ben.send({ ...good, to: 99999, k: 2 });
  await sync(ana, ben);
  await delay(100);
  assert.equal(ben.messages.filter((m) => m.t === "pursuit").length, 0, "only valid messages arrive");
  assert.equal(ana.messages.filter((m) => m.t === "pursuit").length, 0, "the sender does not get its own back");
  assert.equal(olga.messages.filter((m) => m.t === "pursuit").length, 0, "other rooms hear nothing");
  assert.equal(silent.messages.filter((m) => m.t === "pursuit").length, 0, "a connection that never said hi hears nothing");
  // A burst of 40, then 30 a second.
  for (let i = 0; i < 60; i++) ana.send({ ...good, a: i });
  await delay(300);
  const passed = ben.messages.filter((m) => m.t === "pursuit").length;
  assert.ok(passed >= 40 && passed <= 52, `a burst of 40, then 30 a second; got ${passed}`);
});

test("game workers send the game's chase records and hand it the others'", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  // pursuit.h PuShared: magic, version, me, spare, tx head/tail, rx head/tail, 16 + 32 records of 0x20.
  function pursuitBlock(link) {
    const buffer = new SharedArrayBuffer(0x620), u32 = new Uint32Array(buffer), f32 = new Float32Array(buffer);
    u32[0] = 0x31535043; u32[1] = 1;
    link.post({ t: "pursuit-block", memory: { buffer }, ptr: 0 });
    return { u32, f32 };
  }
  const a = pursuitBlock(ana), b = pursuitBlock(ben);
  await interestTick();
  ana.tick(50);
  ben.tick(50);
  assert.equal(a.u32[2], ana.id, "the game learns its relay id");
  // Ana's game: a STATE (to 0), then a REPORT about Ben's car.
  a.u32[8] = 0; a.u32[9] = 1; a.u32[10] = 1; a.u32[11] = 3; a.f32.set([4.5, 2, 0.25, 5], 12);
  a.u32[16] = ben.id; a.u32[17] = 2; a.u32[18] = 1; a.u32[19] = 2; a.f32.set([0.75, 30, 0, 0], 20);
  a.u32[4] = 2;
  ana.tick(50);
  assert.equal(a.u32[5], 2, "both records were taken");
  await until(() => b.u32[6] === 2, "both records in Ben's game");
  assert.deepEqual([b.u32[0x220 / 4], b.u32[0x224 / 4], b.u32[0x228 / 4], b.u32[0x22c / 4]], [ana.id, 1, 1, 3]);
  assert.deepEqual(Array.from(b.f32.subarray(0x230 / 4, 0x240 / 4)), [4.5, 2, 0.25, 5]);
  assert.deepEqual([b.u32[0x240 / 4], b.u32[0x244 / 4], b.u32[0x248 / 4], b.u32[0x24c / 4]], [ana.id, 2, 1, 2]);
  assert.deepEqual(Array.from(b.f32.subarray(0x250 / 4, 0x260 / 4)), [0.75, 30, 0, 0]);
  // A ROLE (k 3, a = 1: Ana is the police, police.h): Ben's game gets it, and Ben's page is told
  // for the name tags; a = 0 takes it back.
  a.u32[24] = 0; a.u32[25] = 3; a.u32[26] = 1; a.u32[27] = 0; a.f32.set([0, 0, 0, 0], 28);
  a.u32[4] = 3;
  ana.tick(50);
  await until(() => b.u32[6] === 3, "the role in Ben's game");
  assert.deepEqual([b.u32[0x260 / 4], b.u32[0x264 / 4], b.u32[0x268 / 4]], [ana.id, 3, 1]);
  const told = () => Array.from(ben.posted.filter((m) => m.t === "police").pop()?.ids || ["none"]); // the worker's realm
  assert.deepEqual(told(), [ana.id], "Ben's page: Ana is the police");
  a.u32[32] = 0; a.u32[33] = 3; a.u32[34] = 0; a.u32[35] = 0; a.f32.set([0, 0, 0, 0], 36);
  a.u32[4] = 4;
  ana.tick(50);
  await until(() => b.u32[6] === 4, "the second role in Ben's game");
  assert.deepEqual(told(), [], "Ben's page: nobody is the police");
});

test("rooms take far more than eight players by default", async (t) => {
  const relay = await start(t);
  const clients = [];
  for (let i = 0; i < 30; i++) clients.push(await join(t, relay, `Player ${i}`));
  assert.deepEqual(await relay.stats(), { players: 30, here: 30 });
  assert.equal(clients[29].welcome.peers.length, 29);
  const limited = await start(t, { CARBON_ROOM_MAX: "12" });
  for (let i = 0; i < 12; i++) await join(t, limited, `Player ${i}`);
  const refused = new WebSocket(limited.ws());
  refused.addEventListener("error", () => {});
  t.after(() => refused.close());
  await until(() => refused.readyState === WebSocket.CLOSED, "the thirteenth refused");
  assert.deepEqual(await limited.stats(), { players: 12, here: 12 });
});

test("interest: each player gets its K nearest cars, sets do not flap, and a car picked later arrives with its look and newest pose", async (t) => {
  const relay = await start(t, { CARBON_INTEREST_K: "2" });
  const r = await join(t, relay, "R"), a = await join(t, relay, "A"), b = await join(t, relay, "B"), c = await join(t, relay, "C");
  const look = { t: "look", k: pose.k, data: Buffer.alloc(24, 3).toString("base64") };
  r.send(still(0)); a.send(still(100)); b.send(still(200));
  c.send(look); c.send(still(300));
  await interestTick();
  r.messages.length = 0;
  for (const [p, x] of [[a, 100], [b, 200], [c, 300]]) { p.send(still(x, 0, { ts: 2000 })); await sync(p, r); }
  assert.equal(posesFrom(r, a).length, 1, "the nearest");
  assert.equal(posesFrom(r, b).length, 1, "the second nearest");
  assert.equal(posesFrom(r, c).length, 0, "not the third");
  assert.equal(r.messages.some((m) => m.t === "look"), false, "nor its look");
  // C a little nearer than B (190 m against 200 m): B keeps its place.
  c.send(still(190, 0, { ts: 2100 }));
  await interestTick();
  r.messages.length = 0;
  for (const [p, x] of [[a, 100], [b, 200], [c, 190]]) { p.send(still(x, 0, { ts: 2150 })); await sync(p, r); }
  assert.deepEqual([posesFrom(r, a).length, posesFrom(r, b).length, posesFrom(r, c).length], [1, 1, 0]);
  // C right beside R: it takes B's place at the next pick, and R gets C's look and then the
  // pose C sent last, without C sending anything new.
  r.messages.length = 0;
  c.send(still(20, 0, { ts: 2200 }));
  await until(() => posesFrom(r, c).length, "C's newest pose on entering");
  const order = r.messages.filter((m) => m.id === c.welcome.id).map((m) => m.t);
  assert.deepEqual(order, ["look", "pose"], "look first, so the car is built in it");
  assert.deepEqual(r.messages.find((m) => m.t === "look"), { ...look, id: c.welcome.id });
  assert.deepEqual(posesFrom(r, c)[0], { ...still(20, 0, { ts: 2200 }), id: c.welcome.id });
  r.messages.length = 0;
  for (const [p, x] of [[a, 100], [b, 200]]) { p.send(still(x, 0, { ts: 2300 })); await sync(p, r); }
  assert.deepEqual([posesFrom(r, a).length, posesFrom(r, b).length], [1, 0], "B is out");
  // A look C sends now goes to the players that get C's car: R, not A (whose two are R and B).
  a.messages.length = 0;
  const look2 = { t: "look", k: pose.k, data: Buffer.alloc(24, 4).toString("base64") };
  c.send(look2);
  assert.deepEqual(await r.read("look", c.welcome.id), { ...look2, id: c.welcome.id });
  await sync(c, a);
  assert.equal(a.messages.some((m) => m.t === "look" || m.t === "pose"), false);
});

test("interest: crew-mates within 1000 m come first, followed players before everyone", async (t) => {
  const relay = await start(t, { CARBON_INTEREST_K: "1" });
  const r = await join(t, relay, "R", "Night Owls");
  const n = await join(t, relay, "Near"), m = await join(t, relay, "Mate", "nightowls"), f = await join(t, relay, "Far");
  r.send(still(0)); n.send(still(50)); m.send(still(900)); f.send(still(3000, 0, { ts: 1600 }));
  await interestTick();
  r.messages.length = 0;
  for (const [p, x] of [[n, 50], [m, 900]]) { p.send(still(x, 0, { ts: 2000 })); await sync(p, r); }
  assert.deepEqual([posesFrom(r, n).length, posesFrom(r, m).length], [0, 1], "the crew-mate, not the nearer stranger");
  // A crew-mate far away counts by distance.
  m.send(still(9000, 0, { ts: 2100 }));
  await interestTick();
  r.messages.length = 0;
  for (const [p, x] of [[n, 50], [m, 9000]]) { p.send(still(x, 0, { ts: 2200 })); await sync(p, r); }
  assert.deepEqual([posesFrom(r, n).length, posesFrom(r, m).length], [1, 0]);
  // Followed (racing together): first, whatever the distance; its newest pose comes at once.
  r.messages.length = 0;
  for (const bad of [{ ids: "x" }, { ids: [0] }, { ids: [1.5] }, { ids: Array.from({ length: 9 }, (_, i) => i + 1) }])
    r.send({ t: "follow", ...bad });
  r.send({ t: "follow", ids: [f.welcome.id] });
  assert.deepEqual(await r.read("pose", f.welcome.id), { ...still(3000, 0, { ts: 1600 }), id: f.welcome.id });
  n.send(still(50, 0, { ts: 2300 })); await sync(n, r);
  assert.equal(posesFrom(r, n).length, 0, "the stranger made way");
  f.send(still(3000, 0, { ts: 2400 })); await sync(f, r);
  f.send(still(3000, 0, { ts: 2410 })); await sync(f, r);
  assert.deepEqual(posesFrom(r, f).map((p) => p.ts), [2400, 2410], "every pose of a followed player, however far");
  assert.equal(f.messages.some((m) => m.t === "follow"), false, "follow is not passed on");
  r.send({ t: "follow", ids: [] });
  await interestTick();
  r.messages.length = 0;
  n.send(still(50, 0, { ts: 2500 })); await sync(n, r);
  assert.equal(posesFrom(r, n).length, 1, "back to the nearest");
});

test("interest: far cars' poses are thinned out, and the last one still arrives", async (t) => {
  const relay = await start(t);
  const r = await join(t, relay, "R"), near = await join(t, relay, "Near"), mid = await join(t, relay, "Mid");
  const far = await join(t, relay, "Far");
  r.send(still(0)); near.send(still(450)); mid.send(still(600)); far.send(still(800));
  await interestTick();
  r.messages.length = 0;
  for (let i = 0; i < 10; i++) {
    near.send(still(450, 0, { ts: 3000 + i }));
    mid.send(still(600, 0, { ts: 3000 + i }));
    far.send(still(800, 0, { ts: 3000 + i }));
  }
  await sync(near, r); await sync(mid, r); await sync(far, r);
  assert.equal(posesFrom(r, near).length, 10, "within 500 m (the game removes a car beyond): every pose");
  const [some, few] = [posesFrom(r, mid).length, posesFrom(r, far).length];
  assert.ok(some >= 1 && some <= 2, `within 700 m: one per 100 ms, got ${some}`);
  assert.ok(few >= 1 && few <= 2, `beyond 700 m: one per 250 ms, got ${few}`);
  await until(() => posesFrom(r, mid).some((p) => p.ts === 3009), "the middle car's last pose");
  await until(() => posesFrom(r, far).some((p) => p.ts === 3009), "the far car's last pose");
  // Driving on: about ten a second from 500 to 700 m, four beyond.
  r.messages.length = 0;
  const begin = Date.now();
  for (let i = 0; i < 30; i++) {
    mid.send(still(600, 0, { ts: 4000 + i }));
    far.send(still(800, 0, { ts: 4000 + i }));
    await delay(33);
  }
  await sync(mid, r); await sync(far, r);
  const seconds = (Date.now() - begin) / 1000, [tens, fours] = [posesFrom(r, mid).length / seconds, posesFrom(r, far).length / seconds];
  assert.ok(tens >= 6 && tens <= 11, `${tens.toFixed(1)} a second within 700 m`);
  assert.ok(fours >= 2 && fours <= 5, `${fours.toFixed(1)} a second beyond`);
});

test("the map feed: everyone driving, once a second, in whole metres and degrees", async (t) => {
  const relay = await start(t, { CARBON_INTEREST_K: "1" });
  const a = await join(t, relay, "A"), b = await join(t, relay, "B"), c = await join(t, relay, "C");
  const parked = await join(t, relay, "Parked"), watcher = await join(t, relay, "Watcher");
  const quarter = Math.SQRT1_2;
  a.send(still(12.6, -40.5, {}, [0, quarter, 0, quarter], 3)); // nose along +x: 90
  b.send(still(5000, 5000));                                    // along +z: 0
  c.send(still(-7.4, 0.4, {}, [0, -quarter, 0, quarter]));       // along -x: 270
  parked.send(still(30, 30, { m: 0 }));                           // not driving: not listed
  const expected = [[a.welcome.id, 13, -40, 90], [b.welcome.id, 5000, 5000, 0], [c.welcome.id, -7, 0, 270]];
  const feed = await until(() => watcher.messages.find((m) => m.t === "where" && m.p.length === 3), "the map feed");
  assert.deepEqual(feed, { t: "where", p: expected });
  // Players far beyond the receiver's K get it too.
  await until(() => a.messages.find((m) => m.t === "where" && m.p.length === 3), "A's map feed");
  watcher.messages.length = 0;
  const next = await until(() => watcher.messages.find((m) => m.t === "where"), "the next feed");
  assert.deepEqual(next, { t: "where", p: expected });
});

test("chat and the horn reach nearby players and the crew only; race messages the whole room", async (t) => {
  const relay = await start(t, { CARBON_INTEREST_K: "1" });
  // In this order each one's first pick is already the nearest: Far's is Near, Near's and R's
  // each other, Mate's is Far.
  const f = await join(t, relay, "Far");
  f.send(still(3000));
  const n = await join(t, relay, "Near");
  n.send(still(10));
  const r = await join(t, relay, "R", "Owls");
  r.send(still(0));
  const m = await join(t, relay, "Mate", "owls");
  m.send(still(6500));
  await interestTick();
  r.send({ t: "chat", text: "Race me!" });
  assert.equal((await n.read("chat")).text, "Race me!");
  assert.equal((await m.read("chat")).text, "Race me!", "a crew-mate anywhere in the room");
  await sync(r, f);
  assert.equal(f.messages.some((x) => x.t === "chat"), false, "a stranger far away does not");
  r.send({ t: "horn", on: true });
  assert.deepEqual(await n.read("horn"), { t: "horn", id: r.welcome.id, on: true });
  await m.read("horn");
  await sync(r, f);
  assert.equal(f.messages.some((x) => x.t === "horn"), false);
  // Before the horn stops, Near drives off to Mate and another player stops beside R:
  // neither picks the other any more, but the "off" still reaches Near.
  n.send(still(6400));
  const q = await join(t, relay, "Beside");
  q.send(still(5));
  await interestTick();
  r.send({ t: "chat", text: "Where did you go?" });
  assert.equal((await q.read("chat")).text, "Where did you go?");
  await sync(r, n);
  assert.equal(n.messages.some((x) => x.t === "chat"), false, "Near is out of reach now");
  r.send({ t: "horn", on: false });
  assert.deepEqual(await n.read("horn"), { t: "horn", id: r.welcome.id, on: false });
  assert.deepEqual(await q.read("horn"), { t: "horn", id: r.welcome.id, on: false });
});

test("game workers give the 8 car slots to the nearest players, racers and crew-mates first, and swap them slowly", async (t) => {
  const relay = await start(t);
  const g = await gameLink(t, relay, "G", "Owls");
  // The game's own car is at pose.s's place (12, 3, -40); the others stand along +x from it.
  g.publish(1500); g.tick();
  const here = (d, extra) => still(12 + d, -40, extra, undefined, 3);
  let n = 0;
  async function gsync(from) { // as sync(), with the worker's page messages as the receiver
    const k = ++n;
    from.send({ t: "race", k: "sync", n: 1000 + k });
    await until(() => g.posted.some((x) => x.t === "msg" && x.m.t === "race" && x.m.n === 1000 + k), "worker sync");
  }
  const lookBuffer = new SharedArrayBuffer(0x460 * 9), lu32 = new Uint32Array(lookBuffer), lu8 = new Uint8Array(lookBuffer);
  lu32[0] = 0x4b4c4d43; lu32[1] = 1;
  g.post({ t: "look-block", memory: { buffer: lookBuffer }, ptr: 0 });
  const lookEntry = (id) => { for (let i = 0; i < 8; i++) if (lu32[(0x460 + i * 0x460) / 4] === id) return i; return -1; };
  const bots = new Map(); // distance -> client
  for (const d of [90, 80, 70, 60, 50, 40, 30, 20, 10]) bots.set(d, await join(t, relay, `At ${d}`));
  async function drive() { // every bot's pose, fresh in the worker's clock
    for (const [d, b] of bots) { b.send(here(d, { ts: 2000 + n })); await gsync(b); }
  }
  for (const [d, b] of bots) b.send({ t: "look", k: pose.k, data: Buffer.alloc(16, d).toString("base64") });
  await drive();
  const has = (d) => g.slot(bots.get(d).welcome.id) >= 0;
  assert.deepEqual([...bots.keys()].filter(has), [90, 80, 70, 60, 50, 40, 30, 20], "free slots in the order poses came");
  assert.equal(lookEntry(bots.get(10).welcome.id), -1, "8 look entries, all taken");
  g.tick(250);
  assert.equal(has(10), false, "a slot keeps its car for a while");
  g.tick(3000); await drive(); g.tick(250);
  assert.equal(has(10), true, "then the nearest takes the farthest's slot");
  assert.equal(has(90), false);
  const e = lookEntry(bots.get(10).welcome.id);
  assert.ok(e >= 0, "with its look, in the entry of a player without a slot");
  assert.deepEqual(lu8.slice(0x460 + e * 0x460 + 0x20, 0x460 + e * 0x460 + 0x30), new Uint8Array(16).fill(10));
  // Only a much nearer player takes a slot: 75 m does not replace 80 m.
  bots.set(75, await join(t, relay, "At 75"));
  await drive(); g.tick(250);
  assert.equal(has(75), false);
  // A crew-mate within 400 m comes before strangers, however near.
  bots.set(200, await join(t, relay, "Mate", "owls"));
  await drive(); g.tick(250);
  assert.equal(has(200), true, "the crew-mate");
  assert.equal(has(80), false, "in place of the farthest stranger");
  // A player racing with us (race.js) comes first, whatever the distance.
  bots.set(2000, await join(t, relay, "Racer"));
  g.post({ t: "racers", ids: [bots.get(2000).welcome.id] });
  await drive(); g.tick(250);
  assert.equal(has(2000), true, "the racer");
  assert.equal(has(70), false);
  assert.deepEqual([...bots.keys()].filter(has).sort((a, b) => a - b), [10, 20, 30, 40, 50, 60, 200, 2000]);
  // The where feed reaches the page, without this player.
  const where = await until(() => g.posted.find((x) => x.t === "where" && x.p.length === bots.size), "the where feed");
  assert.equal(where.p.some((e) => e[0] === g.id), false);
  assert.deepEqual(Array.from(where.p.find((e) => e[0] === bots.get(10).welcome.id)), [bots.get(10).welcome.id, 22, -40, 0]);
});

// worldmap.js in a context of its own: the page's online state and where feed in, the
// game's block (worldmap.h WmShared version 2) out. No relay needed.
test("the world map shows 10 players: crew-mates first, then the nearest, at the where feed's places", () => {
  let clock = 1000;
  const listeners = new Map();
  const buffer = new SharedArrayBuffer(0x200), u32 = new Uint32Array(buffer), f32 = new Float32Array(buffer), u8 = new Uint8Array(buffer);
  u32[0] = 0x32504d57; u32[1] = 2;
  const people = Array.from({ length: 12 }, (_, i) => ({ id: i + 1, name: `P${i + 1}`, crew: i >= 10 ? "OWLS" : "" }));
  const feed = (moves = {}) => people.map((p) => [p.id, moves[p.id] ?? 100 * p.id, 0, 90]);
  const page = vm.createContext({
    performance: { now: () => clock }, setInterval() {}, Atomics,
    addEventListener: (type, fn) => listeners.set(type, [...(listeners.get(type) || []), fn]),
    carbonWorldMapBlock: { memory: { buffer }, ptr: 0 },
    recompOnlineState: { open: true, crew: "Owls", people, slotIds: [3, 4, 0, 0, 0, 0, 0, 0] },
    recompCrewKey: (c) => String(c || "").toLowerCase().replace(/\s+/g, ""),
    recompCrewColor: (c) => (c ? "#ff66cc" : ""),
    recompOnlineSelf: () => [0, 3, 0],
    recompOnlineWhere: { at: clock, p: feed() },
  });
  page.window = page;
  vm.runInContext(fs.readFileSync(new URL("../../source/web/worldmap.js", import.meta.url), "utf8"), page);
  const fire = (type) => { for (const fn of listeners.get(type) || []) fn({}); };
  const entry = (i) => ({ id: u32[(0x10 + i * 0x30) / 4], color: u32[(0x14 + i * 0x30) / 4],
    name: Buffer.from(u8.slice(0x18 + i * 0x30, 0x18 + i * 0x30 + 24)).toString("latin1").replace(/\0.*/s, ""),
    x: f32[(0x30 + i * 0x30) / 4], z: f32[(0x34 + i * 0x30) / 4], fx: f32[(0x38 + i * 0x30) / 4], fz: f32[(0x3c + i * 0x30) / 4] });
  const ids = () => Array.from({ length: 10 }, (_, i) => entry(i).id);
  fire("recomp-online"); // wakes up and writes
  assert.deepEqual(ids(), [11, 12, 1, 2, 3, 4, 5, 6, 7, 8], "the crew (1100 m, 1200 m) first, then the nearest");
  assert.deepEqual(entry(0), { id: 11, color: 0xffff66cc, name: "P11", x: 1100, z: 0, fx: 1, fz: Math.fround(Math.cos(Math.PI / 2)) });
  assert.equal(entry(2).color, 0xffdbe4ee, "no crew: the neutral grey");
  assert.equal(u32[2] % 2, 0, "the sequence number is even again");
  // 9 a little nearer than 8 (750 m against 800 m): 8 keeps its arrow; at 500 m 9 takes it.
  page.recompOnlineWhere = { at: clock, p: feed({ 9: 750 }) };
  fire("recomp-online-where");
  assert.deepEqual(ids().slice(2), [1, 2, 3, 4, 5, 6, 7, 8]);
  page.recompOnlineWhere = { at: clock, p: feed({ 9: 500 }) };
  fire("recomp-online-where");
  assert.deepEqual(ids().slice(2), [1, 2, 3, 4, 5, 6, 9, 7], "listed ones count at 3/4: 6 (450) and 7 (525) around 9");
  // No feed for a while (an older relay): the players in the car slots, where the game has them.
  clock += 6000;
  fire("recomp-online");
  assert.deepEqual(ids(), [3, 4, 0, 0, 0, 0, 0, 0, 0, 0]);
  assert.ok(Number.isNaN(entry(0).x), "the game takes the place from the car");
  // An older game build (version 1, 8 players, no places) still gets the names.
  u32[0] = 0x31504d57;
  fire("carbon-worldmap");
  assert.deepEqual(Array.from({ length: 8 }, (_, i) => u32[(0x10 + i * 0x20) / 4]), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.equal(page.recompWorldMap().shown, u32[0x110 / 4]);
});

// ---- Compact poses: binary frames for pages that ask in their hi (see the relay header) ----
// A binary pose as a page with relay id `id` gets it from the relay.
const passedOn = (id, fields) => { const b = binaryPose(fields); b.writeUInt32LE(id, 2); return b; };

test("compact poses: asked for in hi, and binary and JSON pages in one room each get every car in their own format", async (t) => {
  const relay = await start(t);
  const json = await connect(t, relay, "lobby", "Old page");
  const a = await connect(t, relay, "lobby", "A", false, true);
  const b = await connect(t, relay, "lobby", "B", false, true);
  assert.equal(json.welcome.bin, undefined, "a page that does not ask gets JSON only");
  assert.equal(a.welcome.bin, 1);
  // Only a whole number from 1 asks; a later format asked for gets this relay's own (1).
  for (const [asked, got] of [[0, undefined], ["1", undefined], [1.5, undefined], [true, undefined], [2, 1]]) {
    const c = await connect(t, relay, "other");
    c.send({ t: "hi", name: "C", bin: asked });
    assert.equal((await c.read("welcome")).bin, got, `bin ${JSON.stringify(asked)}`);
  }
  // A JSON page's pose: 46 bytes for the binary pages, exactly these.
  json.send(pose);
  const want = passedOn(json.welcome.id, { n: [0, 0, 0, 32767, 12000, 3000, -40000, 500, 0, 0, 0, 0, 0] });
  for (const p of [a, b]) {
    const got = await p.read("pose", json.welcome.id);
    assert.equal(got.bytes.toString("hex"), want.toString("hex"));
  }
  // A binary page's pose: passed on as it came to the binary pages (with the sender's id), and as
  // JSON to the others: the quaternion to 5 decimals, mm, cm/s and 1/500 rad/s as exact decimals.
  // Not a contacts page: no ghost or acknowledgements pass.
  const n = [123, -4085, 7, 32767, 12125, 3250, -39875, 4325, -13, 237, 3, -6, 32767];
  a.ws.send(binaryPose({ n, k: 0x90944519, ts: 4000000000, ghost: true, acks: [[json.welcome.id, 1]] }));
  assert.deepEqual(await json.read("pose", a.welcome.id), { t: "pose", id: a.welcome.id, k: 0x90944519, m: 1, ts: 4000000000,
    s: [0.00375, -0.12467, 0.00021, 1, 12.125, 3.25, -39.875, 43.25, -0.13, 2.37, 0.006, -0.012, 65.534] });
  assert.equal((await b.read("pose", a.welcome.id)).bytes.toString("hex"),
    passedOn(a.welcome.id, { n, k: 0x90944519, ts: 4000000000 }).toString("hex"));
  // Not poses: a frame of another type, a size that does not fit its acks, anything before hi.
  const early = await connect(t, relay);
  early.ws.send(binaryPose({ n, ts: 1601 }));
  for (const bad of [Buffer.alloc(0), Buffer.from([2, 1]), binaryPose({ n, ts: 1602 }).subarray(0, 45),
                     Buffer.concat([binaryPose({ n, ts: 1603 }), Buffer.alloc(1)]), binaryPose({ n, ts: 1604, flags: 0x11 }),
                     Buffer.from(binaryPose({ n, ts: 1605 })).fill(3, 0, 1)])
    a.ws.send(bad);
  a.ws.send(binaryPose({ n, ts: 1610, m: 0 }));
  assert.equal((await json.read("pose", a.welcome.id)).ts, 1610);
  assert.equal(json.messages.some((m) => m.t === "pose"), false, "only whole binary poses pass");
  // The map feed: the same places, binary for binary pages.
  const fromJson = await until(() => json.messages.find((m) => m.t === "where" && m.p.length === 1), "the JSON feed");
  const fromBin = await until(() => a.messages.find((m) => m.t === "where" && m.p.length === 1), "the binary feed");
  assert.equal(fromBin.bin, true);
  assert.deepEqual(fromBin.p, fromJson.p);
  assert.deepEqual(fromBin.p, [[json.welcome.id, 12, -40, 0]]);
  assert.equal(fromBin.bytes.length, 1 + 14);
  // Rounding (halves up), the limits, and ts as the game reads it (>>> 0).
  json.send({ ...pose, ts: -5, s: [0.5, -0.5, 2, -1e300, 0.0625, -0.0625, 1e7, 0.125, -0.125, 400, 0.25, 70, -0.0001] });
  const edge = await a.read("pose", json.welcome.id);
  assert.deepEqual(edge.n, [16384, -16383, 32767, -32767, 63, -62, 2147483647, 13, -12, 32767, 125, 32767, 0]);
  assert.equal(edge.ts, 4294967291);
  // CARBON_RELAY_BIN=0: a relay as before compact poses.
  const off = await start(t, { CARBON_RELAY_BIN: "0" });
  const asks = await connect(t, off, "lobby", "New page", false, true), old = await connect(t, off, "lobby", "Old page");
  assert.equal(asks.welcome.bin, undefined);
  old.send(pose);
  assert.deepEqual(await asks.read("pose", old.welcome.id), { ...pose, id: old.welcome.id });
});

test("compact poses carry contacts: the physics flag, ghost and the owner's acknowledgements", async (t) => {
  const relay = await start(t);
  const a = await connect(t, relay, "lobby", "Driver", true);
  const b = await connect(t, relay, "lobby", "Struck driver", true, true);
  const c = await connect(t, relay, "lobby", "Spectator", true, true);
  const n = [0, 0, 0, 32767, 12000, 3000, -40000, 500, 0, 0, 0, 0, 0];
  a.send(pose);
  b.ws.send(binaryPose({ n }));
  assert.equal((await b.read("pose", a.welcome.id)).bytes.toString("hex"),
    passedOn(a.welcome.id, { n, flags: 1 | 4 }).toString("hex"), "a contacts page's pose: the physics flag");
  assert.deepEqual(await a.read("pose", b.welcome.id), { ...pose, id: b.welcome.id, physics: 1, ghost: false, acks: [] });
  a.send({ t: "impact", to: b.welcome.id, seq: 1, k: pose.k, ts: pose.ts, s: [12, 3, -40, 0.1, 0, 0, 6, 0, 0, 0, 0.4, 0] });
  assert.equal((await b.read("impact")).seq, 1, "a binary pose counts as fresh and solid for an impact");
  // Only acknowledgements of impacts the owner received pass, as in a JSON pose.
  const at = (p, ts) => until(() => p.messages.find((m) => m.t === "pose" && m.id === b.welcome.id && m.ts === ts), `pose ${ts}`);
  b.ws.send(binaryPose({ n, ts: 1533, acks: [[a.welcome.id, 1], [a.welcome.id, 99], [0, 1]] }));
  assert.deepEqual((await at(a, 1533)).acks, [[a.welcome.id, 1]]);
  const seen = await at(c, 1533);
  assert.deepEqual([seen.physics, seen.ghost, seen.acks, seen.bytes.length], [true, false, [[a.welcome.id, 1]], 54]);
  b.ws.send(binaryPose({ n, ts: 1566, ghost: true }));
  assert.equal((await at(a, 1566)).ghost, true);
  assert.equal((await at(c, 1566)).ghost, true);
});

test("game workers speak compact poses with a relay that offers them, and JSON with one that does not", async (t) => {
  for (const bin of [1, 0]) {
    const relay = await start(t, { CARBON_RELAY_BIN: String(bin) });
    const g = await gameLink(t, relay, "G");
    const old = await connect(t, relay, "lobby", "Old page"), now = await connect(t, relay, "lobby", "New page", false, true);
    // The game's speed 5.004 m/s: JSON rounds it to 5.004, a compact pose to whole cm/s.
    const s = [...pose.s];
    s[7] = 5.004;
    g.publish(1500, s); g.tick();
    assert.equal((await old.read("pose", g.id)).s[7], bin ? 5 : 5.004, bin ? "sent binary" : "sent JSON");
    assert.equal((await now.read("pose", g.id)).bin, bin ? true : undefined);
    // Either kind of page's car reaches the game's slots, with its own clock.
    old.send({ ...pose, ts: 1700 });
    if (bin) now.ws.send(binaryPose({ n: [0, 0, 0, 32767, 13500, 3000, -40000, 500, 0, 0, 0, 0, 0], ts: 1800 }));
    else now.send({ ...pose, ts: 1800, s: [0, 0, 0, 1, 13.5, 3, -40, 5, 0, 0, 0, 0, 0] });
    for (const [p, ms, x] of [[old, 1700, 12], [now, 1800, 13.5]]) {
      const i = await until(() => { const i = g.slot(p.welcome.id); return i >= 0 ? i + 1 : false; }, "a slot") - 1;
      const base = (0x60 + i * 0x100) / 4;
      await until(() => g.u32[base + 4] > 0, "a pose in the slot");
      const at = base + 8 + ((g.u32[base + 4] - 1) % 4) * 14;
      assert.equal(g.u32[at], ms);
      assert.deepEqual(Array.from(g.f32.subarray(at + 1, at + 14)), [0, 0, 0, 1, x, 3, -40, 5, 0, 0, 0, 0, 0]);
    }
  }
});

test("a hidden or paused game parks its car immediately and restores it without rewinding time", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  ana.publish(1500);
  ana.tick();
  const slot = await until(() => { ben.tick(300); return ben.slot(ana.id) + 1; }, "Ana's car") - 1;
  const mode = () => ben.u32[(0x60 + slot * 0x100) / 4 + 3];
  assert.equal(mode(), 1);

  ana.post({ t: "visibility", hidden: true });
  ana.tick(1);
  await until(() => { ben.tick(1); return mode() === 0; }, "hidden car parked");

  ana.post({ t: "visibility", hidden: false });
  ana.tick(1);
  await until(() => { ben.tick(1); return mode() === 1; }, "visible car restored");

  ana.publish(1500, pose.s, 0);
  ana.tick(1);
  await until(() => { ben.tick(1); return mode() === 0; }, "paused car parked at the same physics time");
});

// ---- Police chases (the relay header's "Police chases"; source/web/net.js, mods/core/nfsc/cops.h) ----
// A cops frame as a page sends it: type 3, n, id 0, ts, then per cop its number, flags, model and
// the 13 whole numbers of a pose (from offset 14 of a POSE frame).
function copsFrame(cops, ts = 2000) {
  const b = Buffer.alloc(10 + 38 * cops.length);
  b[0] = 3; b[1] = cops.length; b.writeUInt32LE(ts, 6);
  cops.forEach(({ c, f = 3, k = 0x0fc0ffee, p = [12, 3, -40] }, j) => {
    const at = 10 + 38 * j;
    b[at] = c; b[at + 1] = f; b.writeUInt32LE(k, at + 2);
    b.writeInt16LE(32767, at + 12); // quaternion w = 1
    p.forEach((x, i) => b.writeInt32LE(Math.round(x * 1000), at + 14 + 4 * i));
    b.writeInt16LE(2500, at + 26); // 25 m/s along x
  });
  return b;
}

async function copsJoin(t, relay, name, hi) {
  const client = await connect(t, relay);
  client.send({ t: "hi", name, ...hi });
  client.welcome = await client.read("welcome");
  return client;
}

test("police chases: cops frames reach nearby players who asked, checked and rate-limited; old pages get none", async (t) => {
  const relay = await start(t);
  const ana = await copsJoin(t, relay, "Ana", { bin: 1, cops: 1 });
  const ben = await copsJoin(t, relay, "Ben", { bin: 1, cops: 1 });
  const old = await copsJoin(t, relay, "Old", { bin: 1 });           // a page from before police chases
  const json = await copsJoin(t, relay, "Json", { cops: 1 });        // no compact poses: no cops either
  assert.equal(ana.welcome.cops, 1);
  assert.equal(ben.welcome.cops, 1);
  assert.equal(old.welcome.cops, undefined);
  assert.equal(json.welcome.cops, undefined);
  for (const c of [ana, ben, old, json]) c.send(still(10));
  await sync(ana, ben);
  const frame = copsFrame([{ c: 1 }, { c: 7, f: 0x21, k: 0xdeadbeef, p: [20, 3, -45] }]);
  ana.ws.send(frame);
  const got = await ben.read("cops");
  assert.equal(got.id, ana.welcome.id);
  assert.equal(got.n, 2);
  assert.deepEqual(got.bytes.subarray(6), frame.subarray(6), "passed on as sent, the sender's id written in");
  // Not to the sender, nor to pages that did not ask; nothing else breaks for them.
  await sync(ana, old);
  await sync(ana, json);
  for (const c of [ana, old, json]) assert.equal(c.messages.filter((m) => m.t === "cops").length, 0);
  // Malformed frames are dropped: a number 0, a number twice, more than 8 cops, a wrong length.
  ana.ws.send(copsFrame([{ c: 0 }]));
  ana.ws.send(copsFrame([{ c: 3 }, { c: 3 }]));
  ana.ws.send(copsFrame(Array.from({ length: 9 }, (_, i) => ({ c: i + 1 }))));
  ana.ws.send(Buffer.concat([copsFrame([{ c: 2 }]), Buffer.alloc(1)]));
  // A page that did not ask sends none either.
  old.ws.send(copsFrame([{ c: 1 }]));
  await sync(ana, ben);
  await sync(old, ben);
  assert.equal(ben.messages.filter((m) => m.t === "cops").length, 0);
  // A friend far away gets no cops, but the end of the chase (n = 0) reaches everyone.
  ben.send(still(900));
  await sync(ben, ana);
  ana.ws.send(copsFrame([{ c: 1 }]));
  await sync(ana, ben);
  assert.equal(ben.messages.filter((m) => m.t === "cops").length, 0);
  ana.ws.send(copsFrame([]));
  assert.equal((await ben.read("cops")).n, 0);
  // Limit: a burst of 10, then 20 a second.
  ben.send(still(10));
  await sync(ben, ana);
  for (let i = 0; i < 30; i++) ana.ws.send(copsFrame([{ c: 1 }], 3000 + i));
  await sync(ana, ben);
  const passed = ben.messages.filter((m) => m.t === "cops").length;
  // Three frames passed above, so 7 of the burst are left (and a few refilled since).
  assert.ok(passed >= 7 && passed <= 12, `burst limit (${passed} passed)`);
});

test("police chases: a hit on a cop reaches its chased player only, checked against its newest cops frame", async (t) => {
  const relay = await start(t);
  const ana = await copsJoin(t, relay, "Ana", { bin: 1, cops: 1 });
  const ben = await copsJoin(t, relay, "Ben", { bin: 1, cops: 1 });
  const old = await copsJoin(t, relay, "Old", { bin: 1 });
  for (const c of [ana, ben, old]) c.send(still(12, -40, {}, undefined, 3));
  await sync(ana, ben);
  ana.ws.send(copsFrame([{ c: 4, k: 0xc0c0a, p: [14, 3, -40] }]));
  await ben.read("cops");
  const hit = (extra = {}) => ({ t: "cophit", to: ana.welcome.id, c: 4, seq: 1, k: 0xc0c0a, ts: 2100,
                                 s: [13, 3, -40, 2, 0, 0.5, 0, 0.3, 0], ...extra });
  ben.send(hit());
  const got = await ana.read("cophit");
  assert.deepEqual(got, { t: "cophit", id: ben.welcome.id, c: 4, seq: 1, k: 0xc0c0a, ts: 2100, s: [13, 3, -40, 2, 0, 0.5, 0, 0.3, 0] });
  // Refused: the same seq again, another model, a cop not in the frame, too far from the cop,
  // too big a push, a non-number, a player that did not ask, a hit on oneself.
  ben.send(hit());
  ben.send(hit({ seq: 2, k: 1 }));
  ben.send(hit({ seq: 3, c: 5 }));
  ben.send(hit({ seq: 4, s: [60, 3, -40, 2, 0, 0, 0, 0, 0] }));
  ben.send(hit({ seq: 5, s: [13, 3, -40, 200, 0, 0, 0, 0, 0] }));
  ben.send(hit({ seq: 6, s: [13, 3, -40, 2, 0, 0, 0, 0, "x"] }));
  old.send(hit({ seq: 7 }));
  ana.send(hit({ seq: 8 }));
  await sync(ben, ana);
  await sync(old, ana);
  assert.equal(ana.messages.filter((m) => m.t === "cophit").length, 0);
  assert.equal(old.messages.filter((m) => m.t === "cophit").length + ben.messages.filter((m) => m.t === "cophit").length, 0);
  // A later seq passes; after the chase ends (n = 0) hits on its cops do not.
  ben.send(hit({ seq: 9 }));
  assert.equal((await ana.read("cophit")).seq, 9);
  ana.ws.send(copsFrame([]));
  await ben.read("cops");
  ben.send(hit({ seq: 10 }));
  await sync(ben, ana);
  assert.equal(ana.messages.filter((m) => m.t === "cophit").length, 0);
});

test("game workers send their chase, show a friend's cops in the game's slots, and pass hits both ways", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  // cops.h CopShared: magic, version, page, seq, ms, count, frame, spare, out[8] x 0x40 from 0x20
  // ({number, key, flags, 13 floats}), slots[16] x 0x100 from 0x220 (MpSlot), queue heads at
  // 0x1220 (tx head, tx tail, rx head, rx tail), tx[16] x 0x40 from 0x1230, rx from 0x1630.
  function copBlock(link) {
    const buffer = new SharedArrayBuffer(0x1a30), u32 = new Uint32Array(buffer), f32 = new Float32Array(buffer);
    u32[0] = 0x31504f43; u32[1] = 1;
    link.post({ t: "cop-block", memory: { buffer }, ptr: 0 });
    return { u32, f32 };
  }
  const a = copBlock(ana), b = copBlock(ben);
  const cop = [0, 0, 0, 1, 14, 3, -40, 25, 0, 0, 0, 0, 0];
  ana.publish(1500); ben.publish(1500);
  ana.tick(); ben.tick();
  await until(() => { ana.tick(); return a.u32[2] === 1; }, "Ana's game told its link carries cops");
  a.u32[3] = 1; a.u32[4] = 1500; a.u32[5] = 1;
  a.u32[8] = 3; a.u32[9] = 0xc0c0a; a.u32[10] = 0x21; a.f32.set(cop, 11);
  a.u32[3] = 2;
  ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[0x220 / 4] === ana.id; }, "Ana's cop in Ben's game");
  const slot = 0x220 / 4;
  assert.equal(b.u32[slot + 1] % 2, 0, "the slot is not left half written");
  assert.deepEqual([b.u32[slot + 2], b.u32[slot + 3], b.u32[slot + 5], b.u32[slot + 6]], [0xc0c0a, 1, 3, 0x21]);
  assert.equal(b.u32[slot + 8], 1500, "the pose carries Ana's clock");
  assert.deepEqual(Array.from(b.f32.subarray(slot + 9, slot + 22)).map((x) => Math.round(x * 100) / 100), cop);
  // Ben's game hits that cop: the hit goes to Ana's game (both cars and the chase fresh).
  ana.publish(1540); ben.publish(1540);
  a.u32[3] = 3; a.u32[4] = 1540; a.u32[3] = 4;
  ana.tick(100); ben.tick(10);
  await until(() => { ben.tick(10); return b.u32[slot + 4] === 2; }, "Ana's newer cops frame in Ben's game");
  b.u32[0x1230 / 4] = ana.id; b.u32[0x1230 / 4 + 1] = 3; b.u32[0x1230 / 4 + 2] = 1; b.u32[0x1230 / 4 + 3] = 0xc0c0a;
  b.u32[0x1230 / 4 + 4] = 1540; b.f32.set([13, 3, -40, 2, 0, 0.5, 0, 0.25, 0], 0x1230 / 4 + 5);
  b.u32[0x1220 / 4] = 1;
  ben.tick();
  await until(() => a.u32[0x1220 / 4 + 2] === 1, "the hit in Ana's game");
  assert.deepEqual([a.u32[0x1630 / 4], a.u32[0x1630 / 4 + 1], a.u32[0x1630 / 4 + 2], a.u32[0x1630 / 4 + 3], a.u32[0x1630 / 4 + 4]],
                   [ben.id, 3, 1, 0xc0c0a, 1540], "the arrival in Ana's game clock");
  assert.deepEqual(Array.from(a.f32.subarray(0x1630 / 4 + 5, 0x1630 / 4 + 14)), [13, 3, -40, 2, 0, 0.5, 0, 0.25, 0]);
  assert.equal(b.u32[0x1220 / 4 + 1], 1, "Ben's link took the hit from the queue");
  // The chase ends: one empty frame, and the cop leaves Ben's game.
  a.u32[3] = 5; a.u32[5] = 0; a.u32[3] = 6;
  ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[slot] === 0 && b.u32[slot + 3] === 0; }, "the cop gone from Ben's game");
});

// ---- Car lights (lights.h, net.js; the relay header's "Car lights") ----
test("car lights reach the players that have the car, wait for players who pick it later, and are checked and rate-limited", async (t) => {
  const relay = await start(t);
  const ana = await connect(t, relay, "lobby", "Ana");
  const ben = await connect(t, relay, "lobby", "Ben");
  const olga = await connect(t, relay, "elsewhere", "Olga");
  const silent = await connect(t, relay, "lobby");
  const anaId = ana.welcome.id;
  await interestTick(); // each now has the other's car
  ana.send({ t: "lights", s: 1 });
  assert.deepEqual(await ben.read("lights"), { t: "lights", id: anaId, s: 1 });
  // Refused: not a whole number 0-15; a connection that never said hi.
  for (const s of [-1, 16, 1.5, "3", null, true, undefined, [1]]) ana.send({ t: "lights", s });
  silent.send({ t: "lights", s: 2 });
  await sync(ana, ben);
  await delay(100);
  assert.equal(ben.messages.filter((m) => m.t === "lights").length, 0, "only valid lights arrive");
  assert.equal(ana.messages.filter((m) => m.t === "lights").length, 0, "the sender does not get its own back");
  assert.equal(olga.messages.filter((m) => m.t === "lights").length, 0, "other rooms hear nothing");
  assert.equal(silent.messages.filter((m) => m.t === "lights").length, 0, "a connection that never said hi hears nothing");
  // Every bit, and nothing on, pass on.
  ana.send({ t: "lights", s: 15 });
  assert.deepEqual(await ben.read("lights"), { t: "lights", id: anaId, s: 15 });
  ana.send({ t: "lights", s: 0 });
  assert.deepEqual(await ben.read("lights"), { t: "lights", id: anaId, s: 0 });
  // A player who joins later gets the newest lights of each car here (not 0), once.
  ana.send({ t: "lights", s: 9 });
  assert.deepEqual(await ben.read("lights"), { t: "lights", id: anaId, s: 9 });
  const cat = await connect(t, relay, "lobby", "Cat");
  assert.deepEqual(await cat.read("lights"), { t: "lights", id: anaId, s: 9 });
  await delay(100);
  assert.equal(cat.messages.filter((m) => m.t === "lights").length, 0, "once, and none for cars with nothing on");
  // A burst of 10, then 4 a second.
  await delay(2600);
  for (let i = 0; i < 20; i++) ana.send({ t: "lights", s: i % 16 });
  await delay(300);
  const passed = ben.messages.filter((m) => m.t === "lights").length;
  assert.ok(passed >= 10 && passed <= 12, `a burst of 10, then 4 a second; got ${passed}`);
  // A player who leaves takes the lights along.
  ana.send({ t: "lights", s: 2 });
  await delay(300);
  ana.ws.close();
  await ben.read("left", anaId);
  const dan = await connect(t, relay, "lobby", "Dan");
  await delay(150);
  assert.equal(dan.messages.filter((m) => m.t === "lights").length, 0, "no lights of a player who left");
});

test("game workers send their car's lights and write each slot's player's into the game, cleared when they leave", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  // lights.h LtShared: magic, version, mine (+0x08: 1 left, 2 right, 4 flash, 8 headlights off), seq,
  // remote[8] (+0x10, by car slot), then the game's own values.
  function lightsBlock(link) {
    const buffer = new SharedArrayBuffer(0x58), u32 = new Uint32Array(buffer);
    u32[0] = 0x3154474c; u32[1] = 1;
    link.post({ t: "lights-block", memory: { buffer }, ptr: 0 });
    return { u32 };
  }
  const a = lightsBlock(ana), b = lightsBlock(ben);
  ana.publish(1500); ben.publish(1500);
  ana.tick(); ben.tick();
  const slot = await until(() => { ana.tick(); ben.tick(300); return ben.slot(ana.id) + 1; }, "Ana's car in Ben's game") - 1;
  a.u32[2] = 1 | 4; // left signal, flashing
  ana.tick(300);
  await until(() => { ben.tick(50); return b.u32[4 + slot] === 5; }, "Ana's lights in Ben's slot");
  assert.deepEqual(Array.from(b.u32.subarray(4, 12)).filter((s, i) => i !== slot), [0, 0, 0, 0, 0, 0, 0], "other slots dark");
  a.u32[2] = 8; // headlights off
  ana.tick(300);
  await until(() => { ben.tick(50); return b.u32[4 + slot] === 8; }, "Ana's headlights off in Ben's game");
  // A player who leaves: the slot's lights go with the car. (A raw client, so it can leave.)
  const cal = await join(t, relay, "Cal");
  cal.send({ ...pose, ts: 2000 });
  const calSlot = await until(() => { ben.tick(300); return ben.slot(cal.welcome.id) + 1; }, "Cal's car in Ben's game") - 1;
  cal.send({ t: "lights", s: 2 });
  await until(() => { ben.tick(50); return b.u32[4 + calSlot] === 2; }, "Cal's right signal in Ben's game");
  cal.ws.close();
  await until(() => { ben.tick(50); return b.u32[4 + calSlot] === 0; }, "Cal's lights gone with Cal");
  assert.equal(b.u32[4 + slot], 8, "Ana's stay");
});

// ---- Driving (the relay header's "Driving"; source/web/net.js, mods/core/nfsc/mp_drive.h) ----
// Controls: steering -63/127, full gas, no brake or handbrake, gear 3 (second), nitrous on, 6000 rpm.
const driveCtl = [0xc1, 255, 0, 0, 3, 1, 0x70, 0x17];

test("driving: a car's controls ride in its compact poses to the pages that asked; other pages get the pose as before", async (t) => {
  const relay = await start(t);
  const drv = { drive: 1 };
  const a = await connect(t, relay, "lobby", "A", false, true, drv);
  const b = await connect(t, relay, "lobby", "B", false, true, drv);
  const c = await connect(t, relay, "lobby", "Compact page", false, true);
  const j = await connect(t, relay, "lobby", "JSON page");
  const q = await connect(t, relay, "lobby", "Contacts", true, true, drv);
  assert.deepEqual([a.welcome.drive, b.welcome.drive, c.welcome.drive, j.welcome.drive], [1, 1, undefined, undefined]);
  // Only drive: 1 asks, and only with compact poses.
  for (const [asked, bin, got] of [[1, false, undefined], [true, true, undefined], ["1", true, undefined], [2, true, undefined]]) {
    const x = await connect(t, relay, "other");
    x.send({ t: "hi", name: "X", ...(bin ? { bin: 1 } : {}), drive: asked });
    assert.equal((await x.read("welcome")).drive, got, `drive ${JSON.stringify(asked)} bin ${bin}`);
  }
  const n = [0, 0, 0, 32767, 12000, 3000, -40000, 500, 0, 0, 0, 0, 0];
  a.ws.send(binaryPose({ n, ts: 1500, drive: driveCtl }));
  // To a page that asked: 54 bytes, bit 3, the controls as they came.
  const got = await b.read("pose", a.welcome.id);
  assert.equal(got.bytes.toString("hex"), passedOn(a.welcome.id, { n, ts: 1500, drive: driveCtl }).toString("hex"));
  assert.deepEqual([got.bytes.length, got.drive], [54, driveCtl]);
  // A compact page that did not ask: the pose alone (46 bytes, bit 3 clear); a JSON page: as before.
  assert.equal((await c.read("pose", a.welcome.id)).bytes.toString("hex"), passedOn(a.welcome.id, { n, ts: 1500 }).toString("hex"));
  assert.deepEqual(await j.read("pose", a.welcome.id),
    { t: "pose", id: a.welcome.id, k: pose.k, m: 1, ts: 1500, s: [0, 0, 0, 1, 12, 3, -40, 5, 0, 0, 0, 0, 0] });
  // A contacts page: its acks first, then its controls (an ack of nothing received does not pass).
  q.ws.send(binaryPose({ n, ts: 1550, acks: [[a.welcome.id, 5]], drive: driveCtl }));
  const fromQ = await b.read("pose", q.welcome.id);
  assert.deepEqual([fromQ.physics, fromQ.acks, fromQ.drive, fromQ.bytes.length], [true, [], driveCtl, 54]);
  // Bit 3 from a page that did not ask: the pose passes, the controls do not.
  c.ws.send(binaryPose({ n, ts: 1600, drive: driveCtl }));
  const fromC = await b.read("pose", c.welcome.id);
  assert.deepEqual([fromC.ts, fromC.drive, fromC.bytes.length], [1600, undefined, 46]);
  // Not poses: bit 3 without the 8 bytes, the 8 bytes without bit 3, a byte short.
  for (const bad of [binaryPose({ n, ts: 1601, flags: 1 | 8 }), Buffer.concat([binaryPose({ n, ts: 1602 }), Buffer.from(driveCtl)]),
                     binaryPose({ n, ts: 1603, drive: driveCtl }).subarray(0, 53)])
    a.ws.send(bad);
  // A pose without controls from a driving page passes without them.
  a.ws.send(binaryPose({ n, ts: 1610 }));
  const plain = await b.read("pose", a.welcome.id);
  assert.deepEqual([plain.ts, plain.drive, plain.bytes.length], [1610, undefined, 46], "only whole poses pass");
  // A relay without compact poses has no driving either.
  const off = await start(t, { CARBON_RELAY_BIN: "0" });
  assert.equal((await connect(t, off, "lobby", "New page", false, true, drv)).welcome.drive, undefined);
});

test("game workers send their car's controls with its poses and hand the game each other player's, by car slot", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  // mp_drive.h DrvShared: magic, version, seq, ms, own[8] (+0x10, this car's), page (+0x18), spare,
  // then 8 slots from +0x20 (by car slot), 0x70 bytes each: id, seq, count, spare, 8 x {ms, 8 bytes}.
  function driveBlock(link) {
    const buffer = new SharedArrayBuffer(0x3a0), u32 = new Uint32Array(buffer), u8 = new Uint8Array(buffer);
    u32[0] = 0x31565244; u32[1] = 1;
    link.post({ t: "drive-block", memory: { buffer }, ptr: 0 });
    return { u32, u8 };
  }
  const a = driveBlock(ana), b = driveBlock(ben);
  assert.deepEqual([a.u32[6], b.u32[6]], [1, 1], "the page tells the game the relay passes controls");
  const newest = (blk, slot) => {
    const base = 0x20 + slot * 0x70, count = blk.u32[base / 4 + 2], at = base + 0x10 + ((count - 1) % 8) * 12;
    return count ? { id: blk.u32[base / 4], ms: blk.u32[at / 4], c: [...blk.u8.subarray(at + 4, at + 12)] } : { id: blk.u32[base / 4] };
  };
  a.u8.set(driveCtl, 0x10);
  ana.publish(1500);
  ana.tick(); ben.tick();
  const slot = await until(() => { ana.tick(); ben.tick(300); return ben.slot(ana.id) + 1; }, "Ana's car in Ben's game") - 1;
  await until(() => { ben.tick(50); return newest(b, slot).ms === 1500; }, "Ana's controls in Ben's game");
  assert.deepEqual(newest(b, slot), { id: ana.id, ms: 1500, c: driveCtl });
  // Each pose its own: the newest controls with the newest pose's time.
  a.u8.set([0, 0, 255, 255, 0, 0, 0x20, 0x03], 0x10); // reversing: brake, handbrake, 800 rpm
  ana.publish(1533);
  ana.tick();
  await until(() => { ben.tick(50); return newest(b, slot).ms === 1533; }, "the next controls");
  assert.deepEqual(newest(b, slot).c, [0, 0, 255, 255, 0, 0, 0x20, 0x03]);
  // The game writing its part (odd seq): the last whole controls go instead.
  a.u32[2] = 1;
  a.u8.set([9, 9, 9, 9, 9, 9, 9, 9], 0x10);
  ana.publish(1566);
  ana.tick();
  await until(() => { ben.tick(50); return newest(b, slot).ms === 1566; }, "a pose while the game writes");
  assert.deepEqual(newest(b, slot).c, [0, 0, 255, 255, 0, 0, 0x20, 0x03]);
  a.u32[2] = 2;
  // A car whose page sends no controls: its slot names it and holds none; when it leaves, its
  // slot is emptied.
  const old = await join(t, relay, "Old page");
  old.send({ ...pose, ts: 2000 });
  const oldSlot = await until(() => { ben.tick(300); return ben.slot(old.welcome.id) + 1; }, "the old page's car") - 1;
  await until(() => { ben.tick(50); return newest(b, oldSlot).id === old.welcome.id; }, "its slot named");
  assert.deepEqual(newest(b, oldSlot), { id: old.welcome.id });
  old.ws.close();
  await until(() => { ben.tick(50); return newest(b, oldSlot).id === 0; }, "the slot emptied with the car");
  assert.equal(newest(b, slot).id, ana.id, "Ana's stays");
});

// ---- Ping (net.js, the kit's performance box; the relay header's "Ping") ----
test("ping: the relay answers the sender alone with its n, only after the hi, checked and rate-limited", async (t) => {
  const relay = await start(t);
  const ana = await connect(t, relay, "lobby", "Ana");
  const ben = await connect(t, relay, "lobby", "Ben");
  const silent = await connect(t, relay, "lobby");
  ana.send({ t: "ping", n: 0 });
  assert.deepEqual(await ana.read("pong"), { t: "pong", n: 0 });
  ana.send({ t: "ping", n: 4294967295 });
  assert.deepEqual(await ana.read("pong"), { t: "pong", n: 4294967295 });
  // Refused: not a whole number 0 to 2^32 - 1; a connection that never said hi.
  for (const n of [-1, 4294967296, 1.5, "3", null, true, undefined, [1], { n: 1 }]) ana.send({ t: "ping", n });
  silent.send({ t: "ping", n: 7 });
  await sync(ana, ana);
  await sync(ana, ben);
  await delay(100);
  assert.equal(ana.messages.filter((m) => m.t === "pong").length, 0, "only valid pings are answered");
  assert.equal(ben.messages.filter((m) => m.t === "pong").length, 0, "to the sender alone");
  assert.equal(silent.messages.filter((m) => m.t === "pong").length, 0, "not before the hi");
  // A burst of 5, then 1 a second; the answers keep their order.
  await delay(2100);
  for (let i = 0; i < 12; i++) ana.send({ t: "ping", n: 100 + i });
  await sync(ana, ana);
  await delay(100);
  const answered = ana.messages.filter((m) => m.t === "pong").map((m) => m.n);
  assert.ok(answered.length >= 5 && answered.length <= 6, `a burst of 5, then 1 a second; got ${answered.length}`);
  assert.deepEqual(answered, answered.map((_, i) => 100 + i), "in order, the first ones");
});

test("game workers time the relay's answers to their pings and hand the page the median of the last five", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana");
  const shown = () => ana.posted.filter((x) => x.t === "ping").map((x) => x.ms);
  // The worker's clock moves only in tick(): a ping goes at a tick, and its answer cannot be
  // read before the next await, so a second tick right after sets the round trip it measures.
  async function round(rtt, expect) {
    ana.tick(2000); // the next ping goes
    ana.tick(rtt);  // its answer comes this much later
    await until(() => shown().at(-1) === expect, `ping ${expect} after a ${rtt} ms round trip`);
  }
  ana.tick(1);    // linked: the first ping goes at once
  assert.deepEqual(shown(), [null], "linked, nothing timed yet");
  ana.tick(37);   // (no second ping: 2 s have not passed)
  await until(() => shown().at(-1) === 37, "the first ping timed");
  await round(121, 79);  // 37, 121: the middle two's mean, rounded
  await round(40, 40);   // 37, 40, 121
  await round(300, 81);  // 37, 40, 121, 300: 80.5
  await round(44, 44);   // 37, 40, 44, 121, 300
  await delay(1100);     // the relay answers a burst of 5, then 1 a second
  await round(50, 50);   // the last five: 40, 44, 50, 121, 300 (37 has gone)
  // Pings left unanswered (six at once: over the relay's limit; a relay from before answers none):
  // after 6 s without an answer the round trip is not known, until the next answer.
  for (let i = 0; i < 6; i++) ana.tick(2000);
  await until(() => shown().at(-1) === null, "not known after 6 s without an answer");
  await delay(300);
  assert.equal(shown().at(-1), null, "an answer that comes after 6 s does not count");
  await delay(1100);
  await round(60, 60);
});

// Street messages must reach exactly the named opponent, even before cars enter interest.
test("street races: directed delivery, validation, room isolation and rate limit", async (t) => {
  const relay = await start(t);
  const ana = await connect(t, relay, "lobby", "Ana");
  const ben = await connect(t, relay, "lobby", "Ben");
  const observer = await connect(t, relay, "lobby", "Observer");
  const other = await connect(t, relay, "elsewhere", "Other");
  const silent = await connect(t, relay, "lobby");
  const good = { t: "street", to: ben.welcome.id, k: 3, r: 0xffffffff, a: 4500, f: [123, -25.5, 999, 0] };
  ana.send(good);
  assert.deepEqual(await ben.read("street"), { t: "street", id: ana.welcome.id, k: 3, r: 0xffffffff, a: 4500, f: good.f });
  for (const bad of [{ k: 0 }, { k: 16 }, { k: 1.5 }, { r: -1 }, { r: 0x100000000 }, { r: "1" },
    { a: 65536 }, { a: -1 }, { f: [1, 2, 3] }, { f: [0, null, 0, 0] }, { f: [1e7, 0, 0, 0] },
    { to: 0 }, { to: ana.welcome.id }, { to: other.welcome.id }]) ana.send({ ...good, ...bad });
  silent.send(good);
  await sync(ana, ben);
  await delay(100);
  for (const peer of [ana, ben, observer, other, silent])
    assert.equal(peer.messages.filter(m => m.t === "street").length, 0, "invalid and unrelated messages stay private");
  for (let i = 0; i < 30; i++) ana.send({ ...good, r: i });
  await delay(300);
  const count = ben.messages.filter(m => m.t === "street").length;
  assert.ok(count >= 9 && count <= 13, `street burst limited, got ${count}`);
});

test("street race workers transfer records, names and identity through shared memory", async (t) => {
  const relay = await start(t);
  const ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  function block(link) {
    const buffer = new SharedArrayBuffer(0x640), u32 = new Uint32Array(buffer), f32 = new Float32Array(buffer);
    u32[0] = 0x31525453; u32[1] = 1;
    link.post({ t: "street-block", memory: { buffer }, ptr: 0 });
    return { u32, f32, u8: new Uint8Array(buffer) };
  }
  const a = block(ana), b = block(ben);
  ana.tick(); ben.tick();
  assert.equal(a.u32[2], ana.id);
  a.u32.set([ben.id, 3, 42, 4500], 0x30 / 4);
  a.f32.set([100.25, 2, -500, 0], 0x40 / 4);
  a.u32[8] = 1;
  ana.tick();
  await until(() => b.u32[10] === 1, "street start reaches opponent game");
  assert.equal(a.u32[9], 1);
  assert.deepEqual(Array.from(b.u32.subarray(0x1b0 / 4, 0x1c0 / 4)), [ana.id, 3, 42, 4500]);
  assert.deepEqual(Array.from(b.f32.subarray(0x1c0 / 4, 0x1d0 / 4)), [100.25, 2, -500, 0]);
  assert.equal(new TextDecoder().decode(b.u8.subarray(0x1d0, 0x1d3)), "Ana");
});

// Visual encounters have a separate transport: no career/race messages, spoofed owner,
// cross-room delivery, or unbounded actor list. Existing player/cops tests do not cover it.
test("rivals: owner identity, appearance, removal, validation and room isolation", async (t) => {
  const relay = await start(t), a = await connect(t, relay, "lobby", "Ana"), b = await connect(t, relay, "lobby", "Ben");
  const other = await connect(t, relay, "elsewhere", "Other"), silent = await connect(t, relay, "lobby");
  a.send(pose); b.send(pose);
  await b.read("pose"); await a.read("pose");
  const car = {c: 12, k: pose.k, stamp: 99, s: pose.s, look: Buffer.alloc(0x434, 7).toString("base64")};
  const good = {t: "rivals", id: other.welcome.id, ts: 1500, cars: [car]};
  a.send(good);
  assert.deepEqual(await b.read("rivals"), {...good, id: a.welcome.id});
  for (const change of [{c: 0}, {k: -1}, {stamp: 1.5}, {s: [0, 0, 0, 0, ...pose.s.slice(4)]},
    {s: [0, 0, 0, 1, 1e7, ...pose.s.slice(5)]}, {s: []}, {look: "?"}, {look: Buffer.alloc(10).toString("base64")}])
    a.send({...good, cars: [{...car, ...change}]});
  a.send({...good, cars: [car, car]}); a.send({...good, cars: Array.from({length: 5}, (_, i) => ({...car, c: i + 1, look: ""}))});
  a.send({...good, ts: -1}); silent.send(good);
  a.send({...good, ts: 1600, cars: []});
  assert.deepEqual(await b.read("rivals"), {t: "rivals", id: a.welcome.id, ts: 1600, cars: []});
  await delay(100);
  for (const p of [a, b, other, silent]) assert.equal(p.messages.filter(x => x.t === "rivals").length, 0);
});

test("rival workers preserve appearance and clear ended, stale and disconnected encounters", async (t) => {
  const relay = await start(t), ana = await gameLink(t, relay, "Ana"), ben = await gameLink(t, relay, "Ben");
  function block(link) {
    const buffer = new SharedArrayBuffer(0x3be0), u32 = new Uint32Array(buffer), f32 = new Float32Array(buffer), u8 = new Uint8Array(buffer);
    u32[0] = 0x31564952; u32[1] = 1;
    link.post({t: "rival-block", memory: {buffer}, ptr: 0});
    return {u32, f32, u8};
  }
  const a = block(ana), b = block(ben), slot = 0x1200 / 4;
  ana.publish(1500); ben.publish(1500); ana.tick(); ben.tick();
  await until(() => { ana.tick(100); ben.tick(100); return ana.slot(ben.id) >= 0; }, "players visible");
  a.u32[4] = 1500; a.u32[5] = 1;
  a.u32.set([12, pose.k, 0x434, 99], 8); a.f32.set(pose.s, 12);
  a.u8.fill(7, 0x64, 0x64 + 0x434);
  ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[slot] === ana.id; }, "rival reaches receiving game");
  assert.deepEqual(Array.from(b.u32.subarray(slot + 2, slot + 4)), [pose.k, 1]);
  assert.equal(b.u32[slot + 5], 12);
  assert.equal(b.u32[slot + 64], 0x434);
  assert.equal(b.u8[0x1200 + 0x108 + 1075], 7);
  assert.deepEqual(Array.from(b.f32.subarray(slot + 9, slot + 22)), pose.s);
  a.u32[4] = 1600; a.f32[16] = 14; ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[slot + 4] >= 2; }, "pose without repeated appearance");
  assert.equal(b.u8[0x1200 + 0x108 + 1075], 7);
  // An actor can change model while its physics clock is paused.
  a.u32[9] = pose.k + 1; ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[slot + 2] === pose.k + 1; }, "replacement model at unchanged timestamp");
  a.u32[5] = 0; ana.tick(100);
  await until(() => { ben.tick(10); return b.u32[slot] === 0; }, "encounter ends");
  a.u32[5] = 1; a.u32[4] = 1700; ana.tick(2100);
  await until(() => { ben.tick(10); return b.u32[slot] === ana.id; }, "new encounter appears");
  ben.tick(3100);
  assert.equal(b.u32[slot], 0, "stale owner cannot leave cars behind");
  a.u32[4] = 1800; ana.tick(2100);
  await until(() => { ben.tick(10); return b.u32[slot] === ana.id; }, "fresh appearance restores copy");
  ana.close();
  await until(() => { ben.tick(10); return b.u32[slot] === 0; }, "disconnect clears copy");
});
