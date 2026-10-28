// Free roam at scale: many simulated players driving around one room, against a relay on
// loopback, with the relay's CPU and memory and each player's traffic measured.
// Node 22+. node tools/tests/carbon-net-relay.roam-load.mjs [city|crowd|both] [players=120] [seconds=60] [bin|json|mix]
// CARBON_RELAY_CMD runs a server program (the Rust server: server/target/release/carbon-server)
// instead of tools/carbon-net-relay.mjs. Each scenario starts a relay of its own on a free port.
//   city:  players spread over a SIDE x SIDE m square (5 km), each driving its own way at
//          15-45 m/s, turning now and then; a fifth of them in crews of 4 driving in convoy.
//   crowd: everyone within 300 m of one spot: every player's interest set is full (the worst case).
// Each bot does what net.js does: hi with physics, its look, 30 poses a second (rounded as
// net.js rounds them), a quick chat now and then, and answers the relay's pings. The bots run
// on raw sockets in a few worker threads, so they keep up with what the relay sends.
// The last argument is what the pages speak: bin (the default; net.js from 2026-10-25 with a
// relay that offers compact poses), json (pages and relays from before), or mix (every other
// bot an old page). "byFormat" in the report splits each player's traffic by its format.
import assert from "node:assert/strict";
import { spawn, execFile } from "node:child_process";
import net from "node:net";
import os from "node:os";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { setTimeout as delay } from "node:timers/promises";

const KEY = "local-roam-load";
const SIDE = 5000, CROWD = 300, HZ = 30, CREW_SIZE = 4;
// Compact poses (the relay header): s as whole numbers of 1/SCALE.
const SCALE = [32767, 32767, 32767, 32767, 1000, 1000, 1000, 100, 100, 100, 500, 500, 500];
const U32 = 4294967296;

// ---- The bots (a worker thread each runs some of them) ----
function bots({ port, room, list, warmMs, measureMs, crowd, seed }) {
  let rand = seed >>> 0 || 1; // xorshift: the same drive every run
  const random = () => ((rand ^= rand << 13, rand ^= rand >>> 17, rand ^= rand << 5) >>> 0) / 4294967296;
  const look = Buffer.alloc(0x434, 7).toString("base64");
  const all = [];
  let measuring = false, measureStart = 0;
  for (const b of list) {
    const bot = { ...b, socket: null, buf: Buffer.alloc(0), up: 0, down: 0, frames: {}, senders: new Map(), delays: [],
                  open: false, id: 0, x: 0, z: 0, heading: 0, speed: 0, turnAt: 0, ms: 0 };
    if (crowd) {
      const a = random() * Math.PI * 2, r = Math.sqrt(random()) * CROWD;
      bot.x = 2500 + Math.cos(a) * r; bot.z = 2500 + Math.sin(a) * r;
    } else {
      bot.x = random() * SIDE; bot.z = random() * SIDE;
    }
    bot.heading = random() * Math.PI * 2;
    bot.speed = crowd ? 3 + random() * 8 : 15 + random() * 30;
    all.push(bot);
  }
  // Crew convoys: every member after the first drives behind its leader.
  const leaders = new Map();
  for (const bot of all) if (bot.crew && !leaders.has(bot.crew)) leaders.set(bot.crew, bot);
  function frame(text, opcode = 1) {
    const payload = Buffer.from(text), n = payload.length;
    // Clients must mask; a zero mask key is valid and leaves the bytes as they are.
    const head = n < 126 ? Buffer.from([0x80 | opcode, 0x80 | n, 0, 0, 0, 0])
      : Buffer.from([0x80 | opcode, 0x80 | 126, n >> 8, n & 255, 0, 0, 0, 0]);
    return Buffer.concat([head, payload]);
  }
  function send(bot, text, opcode) {
    const f = frame(text, opcode);
    if (measuring) bot.up += f.length;
    bot.socket.write(f);
  }
  function received(bot, op, payload, size) {
    if (op === 9) { send(bot, payload, 10); return; } // ping: pong
    if (op !== 1 && op !== 2) return;
    let t, id, ts;
    if (op === 2) { // compact: a pose (id at 2, ts at 10) or the map feed
      t = payload[0] === 1 ? "pose" : payload[0] === 2 ? "where" : "binary";
      if (t === "pose") { id = payload.readUInt32LE(2); ts = payload.readUInt32LE(10); }
    } else {
      const text = payload.toString("latin1");
      t = text.startsWith('{"t":"pose"') ? "pose" : text.startsWith('{"t":"where"') ? "where" : JSON.parse(text).t;
      if (t === "welcome") {
        const w = JSON.parse(text);
        bot.id = w.id;
        bot.bin = w.bin === 1;
        bot.open = true;
        send(bot, JSON.stringify({ t: "look", k: 0x12345678, data: look }));
      }
      if (t === "pose") {
        const at = text.indexOf('"id":') + 5, at2 = text.indexOf('"ts":') + 5;
        id = parseInt(text.slice(at, at + 12), 10);
        ts = parseInt(text.slice(at2, at2 + 16), 10);
      }
    }
    if (!measuring) return;
    bot.down += size;
    const f = (bot.frames[t] ??= { n: 0, bytes: 0 });
    f.n++; f.bytes += size;
    if (t === "pose") {
      bot.senders.set(id, (bot.senders.get(id) || 0) + 1);
      // ts is Date.now(); a compact pose carries it >>> 0, so the delay is counted modulo 2^32.
      if (bot.delays.length < 4000 && Math.random() < 0.05) bot.delays.push((((Date.now() - ts) % U32) + U32) % U32);
    } else if (t === "where") {
      bot.whereBytes = size;
    }
  }
  // A pose as net.js sends it to a relay that speaks compact poses.
  function binaryPose(s, ts) {
    const b = Buffer.alloc(46);
    b[0] = 1; b[1] = 1;
    b.writeUInt32LE(0x12345678, 6); b.writeUInt32LE(ts >>> 0, 10);
    s.forEach((x, i) => {
      const limit = i >= 4 && i < 7 ? 0x7fffffff : 0x7fff, n = Math.max(-limit, Math.min(limit, Math.round(x * SCALE[i])));
      if (i < 4) b.writeInt16LE(n, 14 + 2 * i); else if (i < 7) b.writeInt32LE(n, 22 + 4 * (i - 4)); else b.writeInt16LE(n, 34 + 2 * (i - 7));
    });
    return b;
  }
  function read(bot, chunk) {
    bot.buf = bot.buf.length ? Buffer.concat([bot.buf, chunk]) : chunk;
    if (!bot.upgraded) {
      const end = bot.buf.indexOf("\r\n\r\n");
      if (end < 0) return;
      bot.upgraded = true;
      bot.buf = bot.buf.subarray(end + 4);
      send(bot, JSON.stringify({ t: "hi", name: bot.name, crew: bot.crew, physics: 1, ...(bot.format === "bin" ? { bin: 1 } : {}) }));
    }
    while (bot.buf.length >= 2) {
      let n = bot.buf[1] & 127, at = 2;
      if (n === 126) { if (bot.buf.length < 4) return; n = bot.buf.readUInt16BE(2); at = 4; }
      else if (n === 127) { if (bot.buf.length < 10) return; n = Number(bot.buf.readBigUInt64BE(2)); at = 10; }
      if (bot.buf.length < at + n) return;
      const op = bot.buf[0] & 15, payload = bot.buf.subarray(at, at + n);
      bot.buf = bot.buf.subarray(at + n);
      received(bot, op, payload, at + n);
    }
  }
  for (const bot of all) {
    const socket = net.createConnection({ host: "127.0.0.1", port });
    bot.socket = socket;
    socket.setNoDelay(true);
    socket.on("data", (chunk) => read(bot, chunk));
    socket.on("error", (e) => { bot.error = e.message; });
    socket.on("close", () => { bot.closed = true; });
    socket.write(`GET /${encodeURIComponent(room)}?key=${KEY} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n` +
                 "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
  }
  const round = (x, d) => Math.round(x * d) / d;
  const start = Date.now();
  let last = start;
  setInterval(() => {
    const now = Date.now(), dt = (now - last) / 1000;
    last = now;
    for (const bot of all) {
      if (!bot.open || bot.closed) continue;
      const leader = bot.crew ? leaders.get(bot.crew) : null;
      if (leader && leader !== bot && !crowd) {
        // In convoy: 15 m behind the one in front, a little to the side.
        const k = all.indexOf(bot) - all.indexOf(leader);
        bot.heading = leader.heading;
        bot.speed = leader.speed;
        bot.x = leader.x - Math.sin(leader.heading) * 15 * k + Math.cos(leader.heading) * 3 * (k % 2 ? 1 : -1);
        bot.z = leader.z - Math.cos(leader.heading) * 15 * k;
      } else {
        if (now >= bot.turnAt) { bot.heading += (random() - 0.5) * 1.6; bot.turnAt = now + 4000 + random() * 8000; }
        bot.x += Math.sin(bot.heading) * bot.speed * dt;
        bot.z += Math.cos(bot.heading) * bot.speed * dt;
        const lo = crowd ? 2500 - CROWD : 0, hi = crowd ? 2500 + CROWD : SIDE;
        if (bot.x < lo || bot.x > hi) { bot.heading = -bot.heading; bot.x = Math.min(hi, Math.max(lo, bot.x)); }
        if (bot.z < lo || bot.z > hi) { bot.heading = Math.PI - bot.heading; bot.z = Math.min(hi, Math.max(lo, bot.z)); }
      }
      const h = bot.heading / 2, v = bot.speed;
      const raw = [0, Math.sin(h), 0, Math.cos(h), bot.x, 12.5, bot.z, Math.sin(bot.heading) * v, 0, Math.cos(bot.heading) * v, 0, 0.01, 0];
      if (bot.bin) send(bot, binaryPose(raw, now), 2);
      else send(bot, JSON.stringify({ t: "pose", k: 0x12345678, s: raw.map((x, i) => round(x, i < 4 ? 1e5 : 1e3)), m: 1, ts: now,
                                      ghost: false, acks: [] }));
      if (random() < 1 / (HZ * 20)) send(bot, JSON.stringify({ t: "chat", text: "Nice car!" })); // about every 20 s
    }
  }, 1000 / HZ);
  setTimeout(() => {
    measuring = true;
    measureStart = Date.now();
    setTimeout(() => {
      measuring = false;
      const seconds = (Date.now() - measureStart) / 1000;
      parentPort.postMessage(all.map((b) => ({ name: b.name, id: b.id, format: b.bin ? "bin" : "json", open: b.open && !b.closed, error: b.error, seconds,
        up: b.up, down: b.down, frames: b.frames, senders: b.senders.size, whereBytes: b.whereBytes || 0, delays: b.delays,
        posesPerSender: [...b.senders.values()] })));
      // The sockets stay open until every thread has reported (the main thread ends them all),
      // so no thread counts another one's players leaving.
    }, measureMs);
  }, warmMs);
}

// ---- The relay, the workers, the report ----
async function main() {
  const scenario = process.argv[2] || "both";
  const players = Number(process.argv[3] || 120), seconds = Number(process.argv[4] || 60), format = process.argv[5] || "bin";
  assert.ok(["city", "crowd", "both"].includes(scenario));
  assert.ok(["bin", "json", "mix"].includes(format));
  assert.ok(Number.isInteger(players) && players >= 2 && players <= 5000);
  const results = [];
  for (const name of scenario === "both" ? ["city", "crowd"] : [scenario]) results.push(await run(name, players, seconds, format));
  console.log(JSON.stringify(results, null, 2));
}

async function start() {
  const command = process.env.CARBON_RELAY_CMD;
  const script = fileURLToPath(new URL("../carbon-net-relay.mjs", import.meta.url));
  const child = spawn(command || process.execPath, [...(command ? [] : [script]), "0", "127.0.0.1"], {
    env: { ...process.env, CARBON_RELAY_KEY: KEY }, stdio: ["ignore", "pipe", "pipe"],
  });
  let out = "", ended = false;
  child.stdout.on("data", (b) => { out = (out + b).slice(-4096); });
  child.stderr.on("data", (b) => { out = (out + b).slice(-4096); });
  child.on("close", () => { ended = true; });
  for (let i = 0; i < 400 && !/relay on ws:\/\/127\.0\.0\.1:(\d+)\//.test(out); i++) {
    assert.equal(ended, false, out);
    await delay(10);
  }
  const port = Number(out.match(/relay on ws:\/\/127\.0\.0\.1:(\d+)\//)[1]);
  const ps = promisify(execFile);
  return {
    port, child, program: command ? "rust" : "node",
    async resources() {
      const { stdout } = await ps("ps", ["-o", "rss=,time=", "-p", String(child.pid)]);
      const [rss, time] = stdout.trim().split(/\s+/);
      return { rssMiB: Number(rss) / 1024, cpuSeconds: time.split(":").map(Number).reduce((a, x) => a * 60 + x, 0) };
    },
  };
}

const pct = (sorted, p) => sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))];
const r1 = (x) => Math.round(x * 10) / 10;

async function run(scenario, players, seconds, format) {
  const relay = await start();
  const list = Array.from({ length: players }, (_, i) => {
    const crewed = i < Math.floor(players / 5 / CREW_SIZE) * CREW_SIZE;
    return { name: `Bot ${i}`, crew: crewed ? `crew${Math.floor(i / CREW_SIZE)}` : "",
             format: format === "mix" ? (i % 2 ? "json" : "bin") : format };
  });
  const threads = Math.max(1, Math.min(6, Math.ceil(os.availableParallelism() / 2)));
  const warmMs = 6000, measureMs = seconds * 1000;
  const parts = Array.from({ length: threads }, () => []);
  // A crew's convoy is driven by one thread, its leader first.
  list.forEach((b, i) => parts[(b.crew ? Number(b.crew.slice(4)) : i) % threads].push(b));
  const before = { at: Date.now() + warmMs };
  const workers = parts.map((part, i) => new Worker(fileURLToPath(import.meta.url), {
    workerData: { port: relay.port, room: "roam", list: part, warmMs, measureMs, crowd: scenario === "crowd", seed: 1234 + i * 7919 },
  }));
  const done = workers.map((w) => new Promise((resolve, reject) => { w.once("message", resolve); w.once("error", reject); }));
  await delay(warmMs);
  Object.assign(before, await relay.resources());
  const samples = [before.rssMiB];
  const sampler = setInterval(async () => { try { samples.push((await relay.resources()).rssMiB); } catch {} }, 2000);
  const bots = (await Promise.all(done)).flat();
  clearInterval(sampler);
  await Promise.all(workers.map((w) => w.terminate()));
  const after = await relay.resources();
  relay.child.kill();
  const elapsed = (Date.now() - before.at) / 1000;
  const open = bots.filter((b) => b.open).length;
  const downs = bots.map((b) => b.down / b.seconds).sort((a, b) => a - b);
  const ups = bots.map((b) => b.up / b.seconds).sort((a, b) => a - b);
  const poses = bots.map((b) => (b.frames.pose?.n || 0) / b.seconds).sort((a, b) => a - b);
  const cars = bots.map((b) => b.senders).sort((a, b) => a - b);
  const delays = bots.flatMap((b) => b.delays).sort((a, b) => a - b);
  const kinds = {};
  for (const b of bots) for (const [t, f] of Object.entries(b.frames)) {
    const k = (kinds[t] ??= { framesPerPlayerPerSecond: 0, bytesPerPlayerPerSecond: 0 });
    k.framesPerPlayerPerSecond += f.n / b.seconds / bots.length;
    k.bytesPerPlayerPerSecond += f.bytes / b.seconds / bots.length;
  }
  for (const k of Object.values(kinds)) { k.framesPerPlayerPerSecond = r1(k.framesPerPlayerPerSecond); k.bytesPerPlayerPerSecond = Math.round(k.bytesPerPlayerPerSecond); }
  const mean = (a) => a.reduce((x, y) => x + y, 0) / a.length;
  const byFormat = {};
  for (const f of ["bin", "json"]) {
    const these = bots.filter((b) => b.format === f);
    if (these.length) byFormat[f] = { players: these.length, downBytesPerSecond: Math.round(mean(these.map((b) => b.down / b.seconds))),
                                      upBytesPerSecond: Math.round(mean(these.map((b) => b.up / b.seconds))) };
  }
  return {
    scenario, relay: relay.program, format, players, connectedAtEnd: open, seconds: r1(elapsed), byFormat,
    relayCpuPercentOfOneCore: r1(((after.cpuSeconds - before.cpuSeconds) / elapsed) * 100),
    relayRssMiB: { start: r1(before.rssMiB), peak: r1(Math.max(...samples, after.rssMiB)), end: r1(after.rssMiB) },
    perPlayerDownBytesPerSecond: { mean: Math.round(mean(downs)), p50: Math.round(pct(downs, 0.5)), p95: Math.round(pct(downs, 0.95)), max: Math.round(downs.at(-1)) },
    perPlayerUpBytesPerSecond: { mean: Math.round(mean(ups)) },
    relayOutMBPerSecond: r1(downs.reduce((a, b) => a + b, 0) / 1e6),
    posesReceivedPerPlayerPerSecond: { mean: r1(mean(poses)), p50: r1(pct(poses, 0.5)), max: r1(poses.at(-1)) },
    carsHeardPerPlayer: { mean: r1(mean(cars)), min: cars[0], max: cars.at(-1) },
    whereFrameBytes: Math.max(...bots.map((b) => b.whereBytes)),
    poseDelayMs: delays.length ? { p50: pct(delays, 0.5), p95: pct(delays, 0.95), p99: pct(delays, 0.99), max: delays.at(-1) } : null,
    byMessage: kinds,
  };
}

if (isMainThread) await main();
else bots(workerData);
