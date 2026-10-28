// Real-socket load and connection-lifecycle checks. Node 22+, macOS or Linux.
// node tools/tests/carbon-net-relay.load.mjs [load|slow|lifecycle|all] [seconds=60] [relay-script]
// CARBON_RELAY_SCRIPT may select a captured relay for before/after comparisons.
// All traffic stays on loopback, in a new relay process on a random port.
import assert from "node:assert/strict";
import { spawn, execFile } from "node:child_process";
import { once } from "node:events";
import net from "node:net";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { setTimeout as delay, setImmediate as yieldLoop } from "node:timers/promises";

const scenario = process.argv[2] || "all";
const seconds = Number(process.argv[3] || 60);
assert.ok(["load", "slow", "lifecycle", "all"].includes(scenario));
assert.ok(Number.isFinite(seconds) && seconds >= 1 && seconds <= 3600);
const script = process.argv[4] || process.env.CARBON_RELAY_SCRIPT || fileURLToPath(new URL("../carbon-net-relay.mjs", import.meta.url));
const key = "local-load-test";
const ps = promisify(execFile);
const pose = { t: "pose", k: 0x12345678, m: 1, ts: 0,
  s: [0, 0, 0.12467, 0.9922, 1250.125, -47.75, 388.625, 43.25, -0.125, 2.375, 0.003, 0.012, -0.001] };

async function until(check, description, timeout = 4000) {
  const end = performance.now() + timeout;
  do {
    const value = await check();
    if (value) return value;
    await delay(10);
  } while (performance.now() < end);
  throw new Error(`Timed out: ${description}`);
}

async function start() {
  const child = spawn(process.execPath, [script, "0", "127.0.0.1"], {
    env: { ...process.env, CARBON_RELAY_KEY: key }, stdio: ["ignore", "pipe", "pipe"],
  });
  let output = "", errors = "", ended = false, spawnError;
  child.stdout.on("data", (b) => { output = (output + b).slice(-8192); });
  child.stderr.on("data", (b) => { errors = (errors + b).slice(-8192); });
  child.on("error", (e) => { spawnError = e; });
  const exited = new Promise((resolve) => child.once("close", () => { ended = true; resolve(); }));
  const sockets = [];
  const alive = () => {
    if (spawnError) throw spawnError;
    assert.equal(ended, false, `relay exited: ${errors}`);
  };
  try {
    const port = await until(() => {
      alive();
      return Number(output.match(/relay on ws:\/\/127\.0\.0\.1:(\d+)\//)?.[1]);
    }, "relay startup");
    return {
      port, sockets, alive,
      url: (room) => `ws://127.0.0.1:${port}/${room}?key=${key}`,
      async stats(room) {
        alive();
        const response = await fetch(`http://127.0.0.1:${port}/stats?room=${room}`, { signal: AbortSignal.timeout(4000) });
        assert.equal(response.status, 200);
        return response.json();
      },
      async resources() {
        alive();
        const { stdout } = await ps("ps", ["-o", "rss=,time=", "-p", String(child.pid)]);
        const [rss, time] = stdout.trim().split(/\s+/);
        const parts = time.split(":").map(Number);
        return { rssMiB: Number(rss) / 1024, cpuSeconds: parts.reduce((sum, part) => sum * 60 + part, 0) };
      },
      async stop() {
        for (const s of sockets) s instanceof WebSocket ? s.close() : s.destroy();
        child.kill();
        await exited;
      },
    };
  } catch (error) {
    child.kill(); await exited; throw error;
  }
}

async function connect(relay, room, name, onMessage = () => {}) {
  const ws = new WebSocket(relay.url(room));
  relay.sockets.push(ws);
  const messages = [];
  ws.addEventListener("message", (e) => {
    const m = JSON.parse(e.data);
    if (m.t !== "pose") messages.push(m);
    onMessage(m);
  });
  ws.addEventListener("error", () => {});
  await until(() => {
    relay.alive();
    assert.notEqual(ws.readyState, WebSocket.CLOSED, "connection refused or closed");
    return ws.readyState === WebSocket.OPEN;
  }, "WebSocket open");
  const client = {
    ws,
    send: (m) => ws.send(JSON.stringify(m)),
    read(type, id, timeout) {
      return until(() => {
        relay.alive();
        const i = messages.findIndex((m) => m.t === type && (id === undefined || m.id === id));
        return i < 0 ? false : messages.splice(i, 1)[0];
      }, type, timeout);
    },
  };
  if (name !== undefined) {
    client.send({ t: "hi", name });
    client.id = (await client.read("welcome")).id;
  }
  return client;
}

function maskedFrame(message, opcode = 1) {
  const payload = Buffer.isBuffer(message) ? Buffer.from(message) : Buffer.from(JSON.stringify(message));
  const n = payload.length;
  const header = n < 126 ? Buffer.from([0x80 | opcode, 0x80 | n]) : Buffer.from([0x80 | opcode, 0xfe, n >> 8, n & 255]);
  const mask = Buffer.from([0x21, 0x43, 0x65, 0x87]);
  for (let i = 0; i < n; i++) payload[i] ^= mask[i & 3];
  return Buffer.concat([header, mask, payload]);
}

async function wire(relay, room, name, onMessage = () => {}) {
  const socket = net.createConnection({ host: "127.0.0.1", port: relay.port });
  relay.sockets.push(socket);
  let buf = Buffer.alloc(0), upgraded = false, welcome, error;
  socket.on("error", (e) => { error = e; });
  socket.on("data", (b) => {
    buf = buf.length ? Buffer.concat([buf, b]) : b;
    if (!upgraded) {
      const end = buf.indexOf("\r\n\r\n");
      if (end < 0) return;
      assert.ok(buf.subarray(0, end).toString().startsWith("HTTP/1.1 101"));
      upgraded = true; buf = buf.subarray(end + 4);
    }
    while (buf.length >= 2) {
      const opcode = buf[0] & 15;
      let n = buf[1] & 127, at = 2;
      if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); at = 4; }
      assert.notEqual(n, 127, "unexpected large server frame");
      if (buf.length < at + n) return;
      const payload = buf.subarray(at, at + n);
      buf = buf.subarray(at + n);
      if (opcode === 1) {
        const m = JSON.parse(payload.toString());
        if (m.t === "welcome") welcome = m;
        onMessage(m);
      }
      // Deliberately do not pong: lifecycle checks need an unresponsive peer.
    }
  });
  socket.write(Buffer.from(`GET /${room}?key=${key} HTTP/1.1\r\nHost: 127.0.0.1:${relay.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n`));
  await until(() => { relay.alive(); if (error) throw error; return upgraded; }, "wire upgrade");
  socket.write(maskedFrame({ t: "hi", name }));
  await until(() => { relay.alive(); return welcome; }, "wire welcome");
  return { socket, id: welcome.id, send: (m) => socket.write(maskedFrame(m)) };
}

const rounded = (n) => Math.round(n * 100) / 100;
const percentile = (sorted, p) => rounded(sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * p) - 1)]);

async function load(relay) {
  const clients = [], delays = [], seen = Array.from({ length: 8 }, () => new Map());
  let measuring = false, sent = 0, received = 0, invalid;
  for (let i = 0; i < 8; i++) clients.push(await connect(relay, "load", `Driver ${i}`, (m) => {
    if (!measuring || m.t !== "pose") return;
    if (m.id === clients[i].id || m.ts <= (seen[i].get(m.id) ?? -1)) invalid = "self, duplicate or out-of-order pose";
    seen[i].set(m.id, m.ts);
    delays.push(performance.now() - m.ts); received++;
  }));
  assert.equal((await relay.stats("load")).here, 8);
  await delay(1000);
  const before = await relay.resources(), samples = [before.rssMiB];
  const begin = performance.now();
  let nextSample = begin + 2000, nextTick = begin;
  measuring = true;
  while (performance.now() - begin < seconds * 1000) {
    for (const client of clients) {
      assert.equal(client.ws.readyState, WebSocket.OPEN);
      client.send({ ...pose, ts: performance.now() }); sent++;
    }
    if (performance.now() >= nextSample) { samples.push((await relay.resources()).rssMiB); nextSample += 2000; }
    nextTick += 1000 / 30;
    await delay(Math.max(0, nextTick - performance.now()));
  }
  const elapsed = (performance.now() - begin) / 1000;
  await until(() => { relay.alive(); return received === sent * 7; }, "all eight-player deliveries");
  const after = await relay.resources(); samples.push(after.rssMiB);
  assert.equal(invalid, undefined);
  assert.equal((await relay.stats("load")).here, 8);
  measuring = false;
  delays.sort((a, b) => a - b);
  console.log(JSON.stringify({ scenario: "load", seconds: rounded(elapsed), drivers: 8, posesSent: sent,
    deliveries: received, expectedDeliveries: sent * 7, perDriverHz: rounded(sent / 8 / elapsed),
    delayMs: { p50: percentile(delays, .5), p95: percentile(delays, .95), p99: percentile(delays, .99), max: rounded(delays.at(-1)) },
    relayRssMiB: { start: rounded(before.rssMiB), peak: rounded(Math.max(...samples)), end: rounded(after.rssMiB) },
    relayCpuPercentOfOneCore: rounded((after.cpuSeconds - before.cpuSeconds) / elapsed * 100) }));
  for (let i = 0; i < 20; i++) {
    const old = clients[7]; old.ws.close();
    await clients[0].read("left", old.id);
    clients[7] = await connect(relay, "load", `Replacement ${i}`);
    assert.equal((await relay.stats("load")).here, 8);
  }
  console.log(JSON.stringify({ scenario: "reconnect", cycles: 20, roomPlayers: 8 }));
}

async function slow(relay) {
  const count = 160000;
  let healthyCount = 0, slowCount = 0, lastState, leftSeen = false, staleAfterLeft = false, barrier = false;
  const controls = [];
  const healthy = await connect(relay, "slow", "Healthy", (m) => { if (m.t === "pose") healthyCount++; });
  const slowPeer = await wire(relay, "slow", "Slow", (m) => {
    if (m.t === "pose" && m.id !== healthy.id) { slowCount++; if (leftSeen) staleAfterLeft = true; }
    if (m.t === "pose" && m.id === healthy.id) lastState = m;
    if (m.t === "left") leftSeen = true;
    if (m.t === "chat" || m.t === "horn") controls.push(m);
    if (m.t === "welcome" && leftSeen) barrier = true;
  });
  slowPeer.socket.pause();
  const sender = await wire(relay, "slow", "Sender");
  const before = await relay.resources();
  const batch = Buffer.concat(Array.from({ length: 500 }, () => maskedFrame(pose)));
  const begin = performance.now();
  for (let n = 0; n < count; n += 500) {
    relay.alive();
    if (!sender.socket.write(batch)) await once(sender.socket, "drain");
    await yieldLoop();
  }
  await until(() => { relay.alive(); return healthyCount === count; }, "healthy player receives flood", 15000);
  const after = await relay.resources();
  // A departed sender must not be recreated by a retained position after left.
  sender.socket.destroy();
  await healthy.read("left", sender.id);
  healthy.send({ ...pose, m: 0, ts: 1234567 });
  healthy.send({ t: "chat", text: "Still here" });
  healthy.send({ t: "horn", on: true });
  healthy.send({ t: "horn", on: false });
  healthy.send({ t: "hi", name: "Healthy" });
  await healthy.read("welcome");
  slowPeer.send({ t: "hi", name: "Slow recovered" });
  slowPeer.socket.resume();
  await until(() => { relay.alive(); return barrier; }, "slow player catches up", 15000);
  const result = { scenario: "slow", seconds: rounded((performance.now() - begin) / 1000),
    floodPoses: count, healthyDeliveries: healthyCount, slowDeliveries: slowCount,
    discardedObsoletePoses: count - slowCount, latestState: lastState?.m,
    relayRssMiB: { before: rounded(before.rssMiB), after: rounded(after.rssMiB) } };
  console.log(JSON.stringify(result));
  assert.equal(lastState?.ts, 1234567, "latest state must survive a stalled reader");
  assert.equal(lastState?.m, 0);
  assert.equal(staleAfterLeft, false, "queued poses must not resurrect departed players");
  assert.deepEqual(controls, [
    { t: "chat", id: healthy.id, text: "Still here" },
    { t: "horn", id: healthy.id, on: true },
    { t: "horn", id: healthy.id, on: false },
  ], "chat and horn transitions must survive backpressure in order");
  assert.ok(slowCount < count / 2, "a blocked connection must discard obsolete positions instead of replaying the entire backlog");
  assert.equal((await relay.stats("slow")).here, 2);
}

async function lifecycle(relay) {
  const clients = [];
  for (let i = 0; i < 7; i++) clients.push(await connect(relay, "idle", `Idle ${i}`));
  const unresponsive = await wire(relay, "idle", "No pong");
  const unnamed = [];
  for (let i = 0; i < 8; i++) unnamed.push(await connect(relay, "unnamed"));
  // These peers finish the WebSocket upgrade but never introduce themselves.
  // Idle browsers answer WebSocket pings even without sending game positions.
  const [hello, pong] = await Promise.allSettled([
    until(() => { relay.alive(); return unnamed.every((c) => c.ws.readyState === WebSocket.CLOSED); }, "unnamed room slots released", 12000),
    clients[0].read("left", unresponsive.id, 28000),
  ]);
  console.log(JSON.stringify({ scenario: "lifecycle", unnamedSlotsRecovered: hello.status === "fulfilled" ? 8 : 0,
    healthyIdlePlayers: clients.filter((c) => c.ws.readyState === WebSocket.OPEN).length,
    unresponsivePlayerRemoved: pong.status === "fulfilled" }));
  assert.equal(hello.status, "fulfilled", "unnamed connections must release room slots within 12 seconds");
  assert.equal(pong.status, "fulfilled", "a player that never answers pings must release its room slot");
  await connect(relay, "unnamed", "Now admitted");
  for (const c of clients) assert.equal(c.ws.readyState, WebSocket.OPEN, "healthy idle player was disconnected");
  assert.equal((await relay.stats("idle")).here, 7);
  await connect(relay, "idle", "Replacement");
  assert.equal((await relay.stats("idle")).here, 8);
  console.log(JSON.stringify({ scenario: "slot-reuse", unnamedRoomJoin: true, deadSlotReused: true }));
}

for (const [name, run] of Object.entries({ load, slow, lifecycle })) {
  if (scenario !== "all" && scenario !== name) continue;
  const relay = await start();
  try { await run(relay); }
  finally { await relay.stop(); }
}
