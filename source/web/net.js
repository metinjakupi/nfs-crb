// net.js - Need for Speed: Carbon's online client. The player page loads it
// when the start page (start.js) turned free roam with others on: it hands
// over the relay address and the player's name as window.recompOnline. For
// development the address may carry them instead: ?mp=<room> (the room on
// RELAY) or a whole relay address ?mp=ws://<host>:<port>/<room>; &key= is the
// relay's key, &name= the name others see.
//
// The game (mods/core/nfsc/mp.h) shares a block of its memory with the page:
// this player's car pose and model at the front, then 8 slots for other
// players' cars. Each new pose the game writes (up to 30 a second) goes to the
// relay (tools/carbon-net-relay.mjs) stamped with the game's physics time at the
// moment it describes, so a busy page cannot make the motion stutter. Each pose
// that arrives goes straight into that player's slot (a ring of the last 4,
// with the sender's time); the game itself works out where the car is at the
// moment it draws each frame, and builds and moves a car for each slot. Each
// side guards what it writes with a sequence number: odd while writing, even
// when done. The game also writes where on screen each car's name goes
// (labels[], after the slots), and this file draws the names there.
//
// A room may hold a few hundred players; the relay sends this one the cars of
// the (up to 24) players near it, and once a second where everyone is (the
// "where" feed, for the world map: window.recompOnlineWhere and
// "recomp-online-where" events). The 8 slots go to the players racing with this
// one (race.js), then crew-mates within CREW_NEAR, then the nearest; a waiting
// player takes the slot of the one that counts least only when clearly nearer
// (SWAP) and not within SLOT_HOLD of that slot's last change, so cars do not
// pop in and out. The racers also go to the relay ({t:"follow"}), which then
// sends their cars whatever the distance.
//
// Poses travel compact once the relay's welcome says it speaks them (bin: 1): a binary
// frame of 46 bytes instead of about 180 of JSON (format: "Compact poses" in the relay's
// header). Older relays answer without it, and this file then sends and reads JSON as before.
// With each compact pose go the car's controls (steering, gas, brake, handbrake, gear, nitrous and
// revs: 8 bytes, "Driving" in the relay's header) once the welcome says drive: 1; the others' come
// into the game's drive block (mods/core/nfsc/mp_drive.h), which drives their cars as they do:
// tyre smoke and spinning wheels, brake and reverse lights, revs and gear changes, nitrous.
//
// The link also times the relay's answers to its pings (every 2 s, "Ping" in the relay's header);
// the page keeps the median round trip as window.recompOnlinePing (whole ms, null while not known,
// e.g. with a relay from before, which does not answer), for the PING line of the kit's
// performance box (F9 menu: Display & graphics, Screen, performance overlay).
//
// The link itself (the relay connection, reading and writing the block) runs
// in a worker: Chrome slows a background tab's own timers to once a second and
// later once a minute, while the game keeps running there, so a player whose
// window is behind another would jump about for everyone else. The page keeps
// only what is seen: the status badge and the names.
"use strict";
(() => {
  const params = new URLSearchParams(location.search);
  const RELAY = "wss://drive.metinjakupi.dev";
  const given = globalThis.recompOnline;
  let relay = given ? given.relay : params.get("mp");
  if (!relay) return;
  if (!given) {
    if (!/^wss?:\/\//.test(relay)) relay = `${RELAY}/${encodeURIComponent(relay)}`;
    if (params.get("key")) relay += `${relay.includes("?") ? "&" : "?"}key=${encodeURIComponent(params.get("key"))}`;
  }
  const name = ((given ? given.name : params.get("name")) || "Player").slice(0, 20);
  const claim = /^[0-9a-f]{64}$/.test((given && given.claim) || "") ? given.claim : "";
  const legacy = /^[A-Za-z0-9_-]{16,64}$/.test((given && given.legacy) || "") ? given.legacy : "";
  const SLOTS = 8, SLOT0 = 0x60, SLOT = 0x100;
  const LABELS = SLOT0 + SLOTS * SLOT; // {x, y, depth, spare} floats per slot

  // Everything below runs in the worker; it is started from this function's text.
  function link() {
    const MAGIC = 0x31504d43, SLOTS = 8, SLOT0 = 0x60, SLOT = 0x100, RING = 4, SNAP = 0x38;
    const POLL_MS = 8, SEND_MS = 33, STALE = 5000, POSE_RECOVER_MS = 100;
    const LABELS = SLOT0 + SLOTS * SLOT, CONTACTS = 0x8e0, IMPACTS = 32, IMPACT = 0x40;
    const TX = CONTACTS + 0x10, RX = TX + IMPACTS * IMPACT;
    // Who gets a slot (see the top of the file). FRESH: a player heard from this recently may
    // take one; CREW_NEAR: a little past where the game builds a car (300 m).
    const PICK_MS = 250, SLOT_HOLD = 3000, SWAP = 0.7, FRESH = 1500, CREW_NEAR = 400, FAR = 1e9, TOP_SPEED = 100;
    let relay = "", name = "", crew = "", claim = "", legacy = "", memory = null, ptr = 0, views = null, ws = null, me = 0;
    let latest = null, sentMs = -1, sentMode = -1, sentAt = 0, told = "", ghost = false, hidden = false;
    let physics = false, serverPhysics = false;
    // Compact poses (tools/carbon-net-relay.mjs, "Compact poses"): the format this page reads and
    // writes; bin is what the relay's welcome agreed to (0: JSON).
    const BIN = 1, BIN_POSE = 1, BIN_WHERE = 2, POSE_BYTES = 46;
    const SCALE = [32767, 32767, 32767, 32767, 1000, 1000, 1000, 100, 100, 100, 500, 500, 500];
    let bin = 0;
    const players = new Map(); // id -> {id, name, crew, key, mode, heard, slot, at, pos, vel, last}
    const owners = new Array(SLOTS).fill(0);
    const slotAt = new Array(SLOTS).fill(-Infinity); // when each slot last changed hands
    let self = null;          // this car: {at, vel} from its newest pose
    let racers = new Set();   // relay ids racing with this player (race.js, through the page)
    let followSent = "[]";    // the racers the relay was last told
    // Car looks (mods/core/nfsc/look.h LookShared, a block of its own): this car's look goes to
    // the room when it changes, at most one per LOOK_MS (the newest waits), and again on each new
    // connection; each other player's goes into a peer entry of its own, by relay id.
    const LOOK_MAGIC = 0x4b4c4d43, LOOK_REC = 0x20, LOOK_SPACE = 0x440, LOOK_PEER0 = 0x460, LOOK_PEER = 0x460;
    const LOOK_PEERS = 8, LOOK_MS = 5000;
    let look = null, lookStamp = 0, lookNewest = null, lookDue = false, lookSentAt = -Infinity, lookStamps = 0;
    const looks = new Map(); // id -> {k, bytes}: what each other player last sent
    // Police chases (mods/core/nfsc/cops.h CopShared, a block of its own; the relay header's
    // "Police chases"). This player's chasing cops go to the relay as binary frames about 15 times a
    // second while there is a chase, and one empty frame when it ends; other players' cops go into
    // the game's COP_SLOTS slots (MpSlot layout: id = owner, mode 1, pad[0] cop number, pad[1]
    // flags). Hits on those cops go back to their owner ({t:"cophit"}), and hits from others on
    // this player's cops go into the game's queue. Only with a relay whose welcome says cops: 1.
    const COP_MAGIC = 0x31504f43, COP_OUT = 0x20, COP_OUT_SIZE = 0x40, COP_MAX = 8, COP_SLOT0 = 0x220;
    const COP_SLOTS = 16, COP_Q = 0x1220, COP_TX = 0x1230, COP_RX = 0x1630, COP_HITS = 16, COP_HIT = 0x40;
    const BIN_COPS = 3, COPS_HEAD = 10, COP_BYTES = 38, COP_SEND_MS = 66, COP_STALE = 1500;
    let cops = null, copsOk = false, copViews = null, copsSentMs = -1, copsSentCount = 0, copsSentAt = 0;
    const copSlots = new Array(COP_SLOTS).fill(null); // {owner, c} each slot holds
    const copHeard = new Map(); // owner id -> when its last cops frame came

    // Rival encounters: owner simulation only, eight visible copies, four per sender.
    const RV_MAGIC = 0x31564952, RV_OUT = 0x478, RV_SLOT = 0x53c, RV_RX = 0x1200;
    let rivals = null, rivalOk = false, rivalSent = false;
    const rivalFrames = new Map(), rivalLooksSent = new Map();
    const rivalSlots = new Array(8).fill(null);
    function rivalMem() {
      const buf = rivals.memory.buffer;
      return {u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf), u8: new Uint8Array(buf)};
    }
    function rivalWrite(i, item) {
      if (!rivals) return;
      const {u32, i32, f32, u8} = rivalMem(), base = rivals.ptr + RV_RX + i * RV_SLOT, b = base >> 2;
      Atomics.add(i32, b + 1, 1);
      if (!item) { u32[b] = 0; u32[b + 4] = 0; }
      else {
        const {owner, x, ts} = item;
        const previous = rivalSlots[i];
        if (!previous || previous.owner !== owner || previous.c !== x.c || previous.k !== x.k || previous.stamp !== x.stamp) u32[b + 4] = 0;
        const count = u32[b + 4], at = b + 8 + (count % 4) * 14;
        u32[b] = owner; u32[b + 2] = x.k; u32[b + 3] = 1; u32[b + 5] = x.c;
        u32[at] = ts; f32.set(x.s, at + 1); u32[b + 4] = count + 1;
        u32[b + 64] = x.bytes.length; u32[b + 65] = x.stamp;
        u8.set(x.bytes, base + 0x108);
      }
      Atomics.add(i32, b + 1, 1);
      rivalSlots[i] = item ? {owner: item.owner, c: item.x.c, k: item.x.k, stamp: item.x.stamp, ts: item.ts} : null;
    }
    function rivalArrange() {
      if (!rivals) return;
      const now = performance.now(), items = [];
      for (const [owner, frame] of rivalFrames) {
        if (!players.has(owner) || now - frame.at > 3000) { rivalFrames.delete(owner); continue; }
        for (const x of frame.cars) if (x.bytes && self) {
          const d = x.s.slice(4, 7).reduce((sum, p, k) => sum + (p - self.at[k]) ** 2, 0);
          if (d <= 500 ** 2) items.push({owner, x, ts: frame.ts, d});
        }
      }
      items.sort((a, b) => a.d - b.d || a.owner - b.owner || a.x.c - b.x.c);
      const chosen = items.slice(0, 8);
      for (let i = 0; i < 8; i++) {
        const prev = rivalSlots[i];
        if (prev && !chosen.some(e => e.owner === prev.owner && e.x.c === prev.c)) rivalWrite(i, null);
      }
      for (const e of chosen) {
        let i = rivalSlots.findIndex(p => p && p.owner === e.owner && p.c === e.x.c);
        if (i < 0) i = rivalSlots.indexOf(null);
        if (i >= 0 && (!rivalSlots[i] || rivalSlots[i].ts !== e.ts || rivalSlots[i].k !== e.x.k || rivalSlots[i].stamp !== e.x.stamp)) rivalWrite(i, e);
      }
    }
    function rivalDrop(owner) {
      if (owner === undefined) { rivalFrames.clear(); rivalLooksSent.clear(); rivalSent = false; }
      else rivalFrames.delete(owner);
      rivalArrange();
    }
    function rivalsFrom(m) {
      if (!rivalOk || m.id === me || !players.has(m.id) || !Array.isArray(m.cars) || m.cars.length > 4) return;
      const old = rivalFrames.get(m.id), cars = [];
      for (const x of m.cars) {
        if (!Number.isInteger(x.c) || !Number.isInteger(x.k) || !Number.isInteger(x.stamp) ||
            !Array.isArray(x.s) || x.s.length !== 13 || !x.s.every(Number.isFinite)) return;
        let bytes = old?.cars.find(p => p.c === x.c && p.k === x.k && p.stamp === x.stamp)?.bytes;
        if (typeof x.look === "string") {
          try { bytes = Uint8Array.from(atob(x.look), c => c.charCodeAt(0)); } catch { return; }
          if (bytes.length !== 0 && bytes.length !== 0x434) return;
        }
        cars.push({...x, bytes});
      }
      rivalFrames.set(m.id, {ts: m.ts, at: performance.now(), cars});
      rivalArrange();
    }
    setInterval(() => {
      if (!rivals) return;
      const {u32, i32, f32, u8} = rivalMem(), b = rivals.ptr >> 2;
      const live = !!me && rivalOk && ws?.readyState === WebSocket.OPEN;
      Atomics.store(i32, b + 2, live ? 1 : 0);
      rivalArrange();
      if (!live) return;
      const before = Atomics.load(i32, b + 3);
      if (before & 1) return;
      const cars = [], now = performance.now();
      let appearance = null;
      for (let i = 0; i < Math.min(u32[b + 5], 4); i++) {
        const base = rivals.ptr + 0x20 + i * RV_OUT, at = base >> 2;
        const x = {c: u32[at], k: u32[at + 1], stamp: u32[at + 3], s: Array.from(f32.subarray(at + 4, at + 17))};
        const key = `${x.c}:${x.k}:${x.stamp}`, sent = rivalLooksSent.get(key) ?? -Infinity;
        // One appearance per frame keeps the existing 4096-byte relay limit.
        if (!appearance && now - sent >= 2000) {
          const size = u32[at + 2];
          if (size !== 0 && size !== 0x434) return;
          x.look = btoa(String.fromCharCode(...u8.subarray(base + 0x44, base + 0x44 + size)));
          appearance = key;
        }
        cars.push(x);
      }
      const ts = u32[b + 4];
      if (Atomics.load(i32, b + 3) !== before) return;
      if (!cars.length && !rivalSent) return;
      ws.send(JSON.stringify({t: "rivals", ts, cars}));
      if (appearance) rivalLooksSent.set(appearance, now);
      const keys = new Set(cars.map(x => `${x.c}:${x.k}:${x.stamp}`));
      for (const key of rivalLooksSent.keys()) if (!keys.has(key)) rivalLooksSent.delete(key);
      rivalSent = cars.length > 0;
    }, 100);

    // Shared police chases (mods/core/nfsc/pursuit.h PuShared, a block of its own): magic, version,
    // me (+0x08: our relay id, 0 unlinked), spare, tx head/tail and rx head/tail (+0x10..+0x1c),
    // then 16 records to send from +0x20 and 32 received from +0x220, 0x20 bytes each: id (to /
    // from), k, a, b, 4 floats. The game's go to the relay as {t:"pursuit", to, k, a, b, f}; others'
    // come back as {t:"pursuit", id, k, a, b, f} into the received ring.
    const PU_MAGIC = 0x31535043, PU_TX = 16, PU_RX = 32, PU_REC = 0x20, PU_TX0 = 0x20, PU_RX0 = 0x220;
    let pursuit = null;

    // Car lights (mods/core/nfsc/lights.h LtShared, a block of its own): magic, version, `mine`
    // (+0x08: this player's lights, bits 1 left signal, 2 right signal, 4 headlight flash, 8
    // headlights off), seq, then remote[8] (+0x10: the lights of the player whose car is in each
    // slot). This player's go to the relay as {t:"lights", s} when they change, at most one per
    // LIGHTS_MS (the newest waits), and again on each new connection; others' come back as
    // {t:"lights", id, s} and are written by slot (0 for an empty slot or a player who left).
    const LT_MAGIC = 0x3154474c, LT_REMOTE = 0x10, LIGHTS_MS = 250;
    let lights = null, lightsSent = 0, lightsSentAt = -Infinity, lightsMe = 0;
    const lightsOf = new Map(); // id -> the lights that player last sent (none kept for 0)

    // Ping (the relay header's "Ping"): every PING_MS this link sends {t:"ping", n} and times the
    // relay's {t:"pong", n}. The median of the last PING_KEEP round trips goes to the page as
    // {t:"ping", ms} when it changes (ms null: not known - no link yet, or no answer for PING_LOST,
    // e.g. a relay from before that never answers), for the kit's performance box.
    const PING_MS = 2000, PING_KEEP = 5, PING_LOST = 6000;
    let pingN = 0, pingAt = -Infinity, pingMe = 0, pingShown;
    const pingSent = new Map(); // n -> when it went, oldest first
    const pings = [];           // the newest round trips, ms

    // Driving (mods/core/nfsc/mp_drive.h DrvShared, a block of its own; the relay header's
    // "Driving"): magic, version, seq (+0x08, odd while the game writes), ms (+0x0c), own (+0x10:
    // this car's controls, DRIVE_BYTES), page (+0x18: 1 while they travel), then 8 slots from
    // +0x20 of DRV_SLOT bytes - id, seq, count, spare, a ring of DRV_RING {ms, 8 bytes} - one per
    // car slot of the same number. This car's controls go in each of its compact poses (flags bit
    // 3, after the acks); each other player's go into its car's slot with its pose's time. Only
    // with a relay whose welcome says drive: 1 (and bin).
    const DRV_MAGIC = 0x31565244, DRV_OWN = 0x10, DRV_PAGE = 0x18, DRV_SLOT0 = 0x20, DRV_SLOT = 0x70;
    const DRV_RING = 8, DRV_SNAP = 12, DRIVE_BIT = 8, DRIVE_BYTES = 8;
    let drive = null, driveOk = false, driveLast = null;

    function driveMem() {
      const buf = drive.memory.buffer;
      return { u8: new Uint8Array(buf), i32: new Int32Array(buf), u32: new Uint32Array(buf) };
    }
    // The game's page flag (it only logs it).
    function driveLinked() {
      if (drive) Atomics.store(driveMem().i32, (drive.ptr + DRV_PAGE) >> 2, driveOk ? 1 : 0);
    }
    // This car's controls now (the game's part, read whole), or the last ones while it writes.
    function driveOwn() {
      if (!drive) return null;
      const { u8, i32 } = driveMem(), seq = (drive.ptr >> 2) + 2, before = Atomics.load(i32, seq);
      if (before & 1) return driveLast;
      const c = u8.slice(drive.ptr + DRV_OWN, drive.ptr + DRV_OWN + DRIVE_BYTES);
      return Atomics.load(i32, seq) === before ? (driveLast = c) : driveLast;
    }
    // Car slot i's controls, beside the pose writeSlot just wrote: emptied with the slot, started
    // over for another player, or one more in the ring with the pose's time. A pose without
    // controls leaves the ring (the game stops using controls a second away from the car's time).
    function driveSlot(i, id, ms, s, ctl) {
      if (!drive) return;
      const { u8, i32, u32 } = driveMem(), base = drive.ptr + DRV_SLOT0 + i * DRV_SLOT, b = base >> 2;
      if (s && !ctl && u32[b] === id) return;
      Atomics.add(i32, b + 1, 1);
      if (!s || u32[b] !== id) { u32[b] = s ? id : 0; u32[b + 2] = 0; }
      if (s && ctl) {
        const count = u32[b + 2], at = base + 0x10 + (count % DRV_RING) * DRV_SNAP;
        u32[at >> 2] = ms >>> 0;
        u8.set(ctl, at + 4);
        u32[b + 2] = count + 1;
      }
      Atomics.add(i32, b + 1, 1);
    }

    function mem() {
      const buf = memory.buffer;
      if (!views || views.buf !== buf)
        views = { buf, u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
      return views;
    }

    // The page's view of the link: whether it is up, who is here, whose car
    // each slot holds, this player's crew. Sent only when it changes.
    function tell() {
      const state = {
        open: !!ws && ws.readyState === WebSocket.OPEN,
        names: [...players.values()].filter((p) => p.name).map((p) => p.name),
        people: [...players].filter(([, p]) => p.name).map(([id, p]) => ({ id, name: p.name, crew: p.crew })),
        slots: owners.map((id) => (id ? players.get(id)?.name || "Player" : "")),
        slotIds: owners.slice(),
        slotModes: owners.map((id) => (id ? players.get(id)?.mode ?? 0 : -1)),
        crew,
        me, // our relay id (race.js tells its own messages from others' by it)
      };
      const text = JSON.stringify(state);
      if (text !== told) postMessage({ t: "state", ...state });
      told = text;
    }

    // page: the link is up; flags (mp.h MpShared, +0x54): bit 0 see-through cars.
    function linked() {
      if (!memory) return;
      Atomics.store(mem().i32, (ptr >> 2) + 2, me ? 1 : 0);
      Atomics.store(mem().i32, (ptr + 0x54) >> 2, (ghost ? 1 : 0) | (physics && serverPhysics ? 2 : 0));
      if (physics) Atomics.store(mem().i32, (ptr + 0x5c) >> 2, me);
    }

    // The game's own part, or null while it is being written.
    function own() {
      const { i32, u32, f32 } = mem(), b = ptr >> 2;
      const before = Atomics.load(i32, b + 3);
      if (before & 1) return null;
      const out = { state: u32[b + 4], key: u32[b + 5], frame: u32[b + 6], ms: u32[b + 7],
                    s: Array.from(f32.subarray(b + 8, b + 21)) };
      if (physics) out.acks = owners.flatMap((id, i) => {
        const serial = u32[(ptr + LABELS + i * 16 + 12) >> 2];
        return id && serial ? [[id, serial]] : [];
      });
      return Atomics.load(i32, b + 3) === before ? out : null;
    }

    // Slot layout (mp.h MpSlot): id, seq, key, mode, count, 3 spare, then RING
    // poses of {sender ms, 13 floats}.
    function writeSlot(i, id, key, mode, ms, s, contacts = 0, ack = 0, ctl = null) {
      const { i32, u32, f32 } = mem(), base = ptr + SLOT0 + i * SLOT, b = base >> 2;
      Atomics.add(i32, b + 1, 1);
      u32[b] = id; u32[b + 2] = key; u32[b + 3] = mode;
      u32[b + 5] = contacts; u32[b + 6] = ack;
      if (s) {
        const count = u32[b + 4], at = (base + 0x20 + (count % RING) * SNAP) >> 2;
        u32[at] = ms >>> 0;
        f32.set(s, at + 1);
        u32[b + 4] = count + 1;
      } else {
        u32[b + 4] = 0;
      }
      Atomics.add(i32, b + 1, 1);
      driveSlot(i, id, ms, s, ctl);
    }

    function player(id) {
      let p = players.get(id);
      if (!p) players.set(id, (p = { id, name: "", crew: "", key: 0, mode: 0, heard: 0, slot: -1, at: null, physics: false,
                                     pos: null, vel: null, last: null, applied: -Infinity, pending: 0 }));
      return p;
    }

    // ---- Who gets a slot ----
    const crewKey = (c) => String(c || "").toLowerCase().replace(/\s+/g, "");
    const capped = (x, y, z) => {
      const n = Math.sqrt(x * x + y * y + z * z);
      return n > TOP_SPEED ? [x * (TOP_SPEED / n), y * (TOP_SPEED / n), z * (TOP_SPEED / n)] : [x, y, z];
    };
    // How p counts for a slot: {group, d}. group 0 racing with us, 1 a crew-mate within
    // CREW_NEAR, 2 anyone; d the nearer of the two cars 1 s and 4 s on (as the relay counts).
    function rank(p, now) {
      let d = FAR;
      if (p.mode === 1 && p.pos && self && now - p.heard <= FRESH) {
        for (const t of [1, 4]) {
          const x = self.at[0] + self.vel[0] * t - p.pos[0] - p.vel[0] * t;
          const y = self.at[1] + self.vel[1] * t - p.pos[1] - p.vel[1] * t;
          const z = self.at[2] + self.vel[2] * t - p.pos[2] - p.vel[2] * t;
          const e = Math.sqrt(x * x + y * y + z * z);
          if (e < d) d = e;
        }
      }
      const mine = crewKey(crew);
      return { p, group: racers.has(p.id) || (streetOpp && p.id === streetOpp) ? 0 : mine && crewKey(p.crew) === mine && d <= CREW_NEAR ? 1 : 2, d };
    }
    const order = (a, b) => a.group - b.group || (a.d < b.d ? -1 : a.d > b.d ? 1 : 0) || a.p.id - b.p.id;
    // Whether waiting player w may take holder h's slot: a better group, or clearly nearer.
    const better = (w, h) => w.group < h.group || (w.group === h.group && w.d < FAR && w.d < SWAP * h.d);

    // p's newest pose into its slot.
    function put(p, now = performance.now()) {
      const l = p.last;
      writeSlot(p.slot, p.id, p.key, p.mode, l.ms, l.s, l.contacts, l.ack, l.drive);
      p.applied = now;
    }

    // TCP can already hold old poses when a connection stalls. After the stall it can deliver
    // those poses much faster than the sender made them. Keep only the newest pose until the
    // normal 30 Hz apply time, so the game resumes once instead of replaying the backlog.
    function queuePut(p, now, recovered) {
      if (p.pending) return;
      const wait = recovered ? SEND_MS : SEND_MS - (now - p.applied);
      if (wait <= 0) { put(p, now); return; }
      p.pending = setTimeout(() => {
        p.pending = 0;
        if (memory && me && p.slot >= 0 && p.last) put(p);
      }, wait);
    }

    // p takes slot i (empty or just emptied); its look goes to the game first, so the car is built in it.
    function take(p, i, now) {
      if (p.pending) { clearTimeout(p.pending); p.pending = 0; }
      p.slot = i;
      owners[i] = p.id;
      slotAt[i] = now;
      const l = looks.get(p.id);
      if (l) lookPut(p.id, l.k, l.bytes);
      writeSlot(i, 0, 0, 0, 0, null);
      if (p.last) put(p, now);
    }

    // Free slots to the best players waiting; then, slot by slot, a clearly better one takes
    // the place of the holder that counts least.
    function arrange(now) {
      if (!memory || !me) return;
      const waiting = [];
      for (const p of players.values()) if (p.slot < 0 && p.last && p.heard && now - p.heard <= FRESH) waiting.push(rank(p, now));
      if (!waiting.length) return;
      waiting.sort(order);
      let changed = false;
      for (const w of waiting) {
        let i = owners.indexOf(0);
        if (i < 0) {
          let worst = null;
          for (let j = 0; j < SLOTS; j++) {
            const h = players.get(owners[j]);
            if (!h || now - slotAt[j] < SLOT_HOLD) continue;
            const r = rank(h, now);
            if (!worst || order(r, worst) > 0) worst = r;
          }
          if (!worst || !better(w, worst)) break;
          i = worst.p.slot;
          park(worst.p);
        }
        take(w.p, i, now);
        changed = true;
      }
      if (changed) tell();
    }

    // The racers to the relay, when they change (and on each new connection).
    function follow() {
      const ids = [...racers];
      if (streetOpp && !racers.has(streetOpp) && ids.length < 8) ids.push(streetOpp); // streetrace.h's other player
      const text = JSON.stringify(ids);
      if (!me || ws?.readyState !== WebSocket.OPEN || text === followSent) return;
      followSent = text;
      ws.send(JSON.stringify({ t: "follow", ids }));
    }

    // A player's car goes; the player stays known (with their name) until they leave.
    function park(p) {
      if (p.slot < 0) return;
      if (p.pending) { clearTimeout(p.pending); p.pending = 0; }
      owners[p.slot] = 0;
      if (memory) writeSlot(p.slot, 0, 0, 0, 0, null);
      p.slot = -1;
      p.heard = 0;
    }

    function drop(id) {
      if (looks.delete(id)) lookPut(id, 0, null);
      const p = players.get(id);
      if (!p) return;
      park(p);
      players.delete(id);
    }

    // A player's look into its entry (bytes null: the entry is emptied). Guarded like a slot.
    function lookPut(id, k, bytes) {
      if (!look) return;
      const buf = look.memory.buffer, u32 = new Uint32Array(buf), i32 = new Int32Array(buf);
      let at = -1;
      for (let i = 0; i < LOOK_PEERS && at < 0; i++) if (u32[(look.ptr + LOOK_PEER0 + i * LOOK_PEER) >> 2] === id) at = i;
      for (let i = 0; i < LOOK_PEERS && at < 0 && bytes; i++) if (!u32[(look.ptr + LOOK_PEER0 + i * LOOK_PEER) >> 2]) at = i;
      // The game has entries for 8 cars, as many as slots, and more players than that send looks:
      // a car in a slot takes the entry of a player who has none.
      for (let i = 0; i < LOOK_PEERS && at < 0 && bytes && owners.includes(id); i++)
        if (!owners.includes(u32[(look.ptr + LOOK_PEER0 + i * LOOK_PEER) >> 2])) at = i;
      if (at < 0) return;
      const base = look.ptr + LOOK_PEER0 + at * LOOK_PEER, b = base >> 2;
      Atomics.add(i32, b + 1, 1);
      u32[b] = bytes ? id : 0; u32[b + 2] = k >>> 0; u32[b + 3] = bytes ? bytes.length : 0;
      u32[b + 4] = ++lookStamps;
      if (bytes) new Uint8Array(buf, base + LOOK_REC, bytes.length).set(bytes);
      Atomics.add(i32, b + 1, 1);
    }

    // This car's look as the game last wrote it ({stamp, k, bytes}), or null while it is written.
    function lookOwn() {
      const buf = look.memory.buffer, u32 = new Uint32Array(buf), i32 = new Int32Array(buf), b = look.ptr >> 2;
      const before = Atomics.load(i32, b + 2);
      if (before & 1) return null;
      const out = { stamp: u32[b + 5], k: u32[b + 3],
                    bytes: new Uint8Array(buf, look.ptr + LOOK_REC, Math.min(u32[b + 4], LOOK_SPACE)).slice() };
      return Atomics.load(i32, b + 2) === before ? out : null;
    }

    // ---- Police chases ----
    function copMem() {
      const buf = cops.memory.buffer;
      if (!copViews || copViews.buf !== buf)
        copViews = { buf, u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
      return copViews;
    }

    // The cops chasing this player as the game last wrote them ({ms, list: [{c, k, f, s}]}), or
    // null while they are being written.
    function copsOwn() {
      const { u32, i32, f32 } = copMem(), b = cops.ptr >> 2;
      const before = Atomics.load(i32, b + 3);
      if (before & 1) return null;
      const list = [];
      for (let i = 0; i < Math.min(u32[b + 5], COP_MAX); i++) {
        const o = (cops.ptr + COP_OUT + i * COP_OUT_SIZE) >> 2;
        list.push({ c: u32[o], k: u32[o + 1], f: u32[o + 2] & 255, s: Array.from(f32.subarray(o + 3, o + 16)) });
      }
      const out = { ms: u32[b + 4], list };
      return Atomics.load(i32, b + 3) === before ? out : null;
    }

    // A cops frame (the relay header's "Police chases"): its 13 numbers per cop as a pose's.
    function packCops(o) {
      const list = o.list.filter((x) => x.c >= 1 && x.c <= 255 && x.s.every(Number.isFinite));
      const b = new ArrayBuffer(COPS_HEAD + COP_BYTES * list.length), v = new DataView(b);
      v.setUint8(0, BIN_COPS);
      v.setUint8(1, list.length);
      v.setUint32(6, o.ms >>> 0, true);
      list.forEach((x, j) => {
        const at = COPS_HEAD + COP_BYTES * j;
        v.setUint8(at, x.c);
        v.setUint8(at + 1, x.f);
        v.setUint32(at + 2, x.k >>> 0, true);
        for (let i = 0; i < 13; i++) {
          const limit = i >= 4 && i < 7 ? 0x7fffffff : 0x7fff, n = Math.round(x.s[i] * SCALE[i]);
          const q = n > limit ? limit : n < -limit ? -limit : n;
          if (i < 4) v.setInt16(at + 6 + 2 * i, q, true);
          else if (i < 7) v.setInt32(at + 14 + 4 * (i - 4), q, true);
          else v.setInt16(at + 26 + 2 * (i - 7), q, true);
        }
      });
      return b;
    }

    // Slot i of the game's friends' cops: owner's cop c at its newest pose (s null: emptied).
    function copWrite(i, owner, k, c, f, ms, s) {
      const { u32, i32, f32 } = copMem(), base = cops.ptr + COP_SLOT0 + i * SLOT, b = base >> 2;
      Atomics.add(i32, b + 1, 1);
      u32[b] = owner; u32[b + 2] = k >>> 0; u32[b + 3] = owner ? 1 : 0; u32[b + 5] = c; u32[b + 6] = f;
      if (s) {
        const count = u32[b + 4], at = (base + 0x20 + (count % RING) * SNAP) >> 2;
        u32[at] = ms >>> 0;
        f32.set(s, at + 1);
        u32[b + 4] = count + 1;
      } else {
        u32[b + 4] = 0;
      }
      Atomics.add(i32, b + 1, 1);
    }

    function copFree(i) {
      copSlots[i] = null;
      if (cops) copWrite(i, 0, 0, 0, 0, 0, null);
    }

    // An owner's cops go (undefined: everyone's).
    function copDrop(owner) {
      for (let i = 0; i < COP_SLOTS; i++) if (copSlots[i] && (owner === undefined || copSlots[i].owner === owner)) copFree(i);
      if (owner === undefined) copHeard.clear();
      else copHeard.delete(owner);
    }

    // Another player's cops frame: each cop keeps its slot, a new one takes a free one (the game
    // shows at most COP_SLOTS), a cop no longer in the chase gives its slot back.
    function copsFrom(owner, ts, list) {
      copHeard.set(owner, performance.now());
      const keep = new Set(list.map((x) => x.c));
      for (let i = 0; i < COP_SLOTS; i++) if (copSlots[i]?.owner === owner && !keep.has(copSlots[i].c)) copFree(i);
      if (!list.length) copHeard.delete(owner);
      if (!cops) return;
      for (const x of list) {
        let i = copSlots.findIndex((h) => h && h.owner === owner && h.c === x.c);
        if (i < 0) {
          i = copSlots.indexOf(null);
          if (i < 0) continue;
          copSlots[i] = { owner, c: x.c };
          copWrite(i, 0, 0, 0, 0, 0, null); // a new cop starts from an empty ring
        }
        copWrite(i, owner, x.k, x.c, x.f, ts, x.s);
      }
    }

    // Hits our game's car made on other players' cops, to their owners.
    function copHitsOut() {
      const { i32, u32, f32 } = copMem(), q = (cops.ptr + COP_Q) >> 2;
      let tail = Atomics.load(i32, q + 1) >>> 0;
      const head = Atomics.load(i32, q) >>> 0;
      const nowMs = memory ? mem().u32[(ptr >> 2) + 7] : 0;
      while (tail !== head && ws.bufferedAmount < 65536) {
        const at = (cops.ptr + COP_TX + (tail % COP_HITS) * COP_HIT) >> 2;
        if (copsOk && me && ((nowMs - u32[at + 4]) | 0) <= 500)
          ws.send(JSON.stringify({ t: "cophit", to: u32[at], c: u32[at + 1], seq: u32[at + 2], k: u32[at + 3],
            ts: u32[at + 4], s: Array.from(f32.subarray(at + 5, at + 14)) }));
        tail = (tail + 1) >>> 0;
      }
      Atomics.store(i32, q + 1, tail | 0);
    }

    // This car's pose as a binary frame (s as whole numbers of 1/SCALE), or null when a number is
    // not finite (as JSON it would carry a null, which the relay drops).
    function packPose(o, contacts, ctl = null) {
      const s = o.s;
      if (!s.every(Number.isFinite)) return null;
      const acks = contacts ? (o.acks || []).slice(0, 8) : [];
      const b = new ArrayBuffer(POSE_BYTES + 8 * acks.length + (ctl ? DRIVE_BYTES : 0)), v = new DataView(b);
      v.setUint8(0, BIN_POSE);
      v.setUint8(1, (o.state === 1 ? 1 : 0) | (contacts && ghost ? 2 : 0) | (ctl ? DRIVE_BIT : 0) | (acks.length << 4));
      v.setUint32(6, o.key >>> 0, true);
      v.setUint32(10, o.ms >>> 0, true);
      for (let i = 0; i < 13; i++) {
        const limit = i >= 4 && i < 7 ? 0x7fffffff : 0x7fff, n = Math.round(s[i] * SCALE[i]);
        const x = n > limit ? limit : n < -limit ? -limit : n;
        if (i < 4) v.setInt16(14 + 2 * i, x, true);
        else if (i < 7) v.setInt32(22 + 4 * (i - 4), x, true);
        else v.setInt16(34 + 2 * (i - 7), x, true);
      }
      acks.forEach(([id, serial], j) => { v.setUint32(46 + 8 * j, id, true); v.setUint32(50 + 8 * j, serial, true); });
      if (ctl) new Uint8Array(b).set(ctl, POSE_BYTES + 8 * acks.length);
      return b;
    }

    // A binary frame from the relay as the message it stands for, or null.
    function unpack(buf) {
      const v = new DataView(buf), n = buf.byteLength, type = n ? v.getUint8(0) : 0;
      if (type === BIN_POSE && n >= POSE_BYTES &&
          n === POSE_BYTES + 8 * (v.getUint8(1) >> 4) + (v.getUint8(1) & DRIVE_BIT ? DRIVE_BYTES : 0)) {
        const f = v.getUint8(1), s = new Array(13), acks = [];
        for (let i = 0; i < 4; i++) s[i] = v.getInt16(14 + 2 * i, true) / SCALE[i];
        for (let i = 4; i < 7; i++) s[i] = v.getInt32(22 + 4 * (i - 4), true) / SCALE[i];
        for (let i = 7; i < 13; i++) s[i] = v.getInt16(34 + 2 * (i - 7), true) / SCALE[i];
        for (let j = 0; j < f >> 4; j++) acks.push([v.getUint32(46 + 8 * j, true), v.getUint32(50 + 8 * j, true)]);
        return { t: "pose", id: v.getUint32(2, true), k: v.getUint32(6, true), s, m: f & 1, ts: v.getUint32(10, true),
                 ...(f & 4 ? { physics: 1, ghost: (f & 2) !== 0, acks } : {}),
                 ...(f & DRIVE_BIT ? { drive: new Uint8Array(buf.slice(n - DRIVE_BYTES)) } : {}) };
      }
      if (type === BIN_WHERE && (n - 1) % 14 === 0) {
        const p = [];
        for (let at = 1; at < n; at += 14)
          p.push([v.getUint32(at, true), v.getInt32(at + 4, true), v.getInt32(at + 8, true), v.getUint16(at + 12, true)]);
        return { t: "where", p };
      }
      if (type === BIN_COPS && n >= COPS_HEAD && n === COPS_HEAD + COP_BYTES * v.getUint8(1)) {
        const list = [];
        for (let j = 0, at = COPS_HEAD; j < v.getUint8(1); j++, at += COP_BYTES) {
          const s = new Array(13);
          for (let i = 0; i < 4; i++) s[i] = v.getInt16(at + 6 + 2 * i, true) / SCALE[i];
          for (let i = 4; i < 7; i++) s[i] = v.getInt32(at + 14 + 4 * (i - 4), true) / SCALE[i];
          for (let i = 7; i < 13; i++) s[i] = v.getInt16(at + 26 + 2 * (i - 7), true) / SCALE[i];
          list.push({ c: v.getUint8(at), f: v.getUint8(at + 1), k: v.getUint32(at + 2, true), s });
        }
        return { t: "cops", id: v.getUint32(2, true), ts: v.getUint32(6, true), list };
      }
      return null;
    }

    function receive(m) {
      if (m.t === "welcome") {
        me = m.id;
        serverPhysics = m.physics === 1;
        bin = m.bin === 1 ? 1 : 0; // a relay from before compact poses says nothing: JSON
        rivalOk = m.rivals === 1;
        copsOk = bin === 1 && m.cops === 1; // nor does one from before police chases
        driveOk = bin === 1 && m.drive === 1; // nor one from before driving
        driveLinked();
        for (const peer of m.peers || []) Object.assign(player(peer.id), { name: peer.name, crew: peer.crew || "", physics: peer.physics === 1 });
        linked();
        lookDue = !!lookNewest; // a new connection: the relay has not seen this car's look
        lookSentAt = -Infinity;
        followSent = "[]"; // nor whom this player races with
        follow();
      } else if (m.t === "join") {
        Object.assign(player(m.id), { name: m.name, crew: m.crew || "", physics: m.physics === 1 });
      } else if (m.t === "physics") {
        if (m.id !== me) player(m.id).physics = m.on === true;
      } else if (m.t === "impact") {
        if (!physics || !serverPhysics || ghost || !me || m.id >= me || !players.get(m.id)?.physics ||
            !Array.isArray(m.s) || m.s.length !== 12 || !m.s.every(Number.isFinite)) return;
        const { i32, u32, f32 } = mem(), c = (ptr + CONTACTS) >> 2;
        const head = Atomics.load(i32, c + 2) >>> 0, tail = Atomics.load(i32, c + 3) >>> 0;
        if (((head - tail) >>> 0) >= IMPACTS) return;
        const at = (ptr + RX + (head % IMPACTS) * IMPACT) >> 2;
        u32[at] = m.id; u32[at + 1] = m.seq; u32[at + 2] = m.k;
        u32[at + 3] = u32[(ptr >> 2) + 7]; // arrival in our game's physics clock
        f32.set(m.s, at + 4);
        Atomics.store(i32, c + 2, (head + 1) | 0);
        return;
      } else if (m.t === "rivals") {
        rivalsFrom(m);
      } else if (m.t === "cops") {
        if (m.id !== me) copsFrom(m.id, m.ts, m.list);
        return;
      } else if (m.t === "cophit") {
        // Another player's car hit one of the cops chasing this one (cops.h applies it to the real cop).
        if (!cops || !copsOk || !Number.isInteger(m.id) || !Number.isInteger(m.c) || !Number.isInteger(m.seq) ||
            !Number.isInteger(m.k) || !Array.isArray(m.s) || m.s.length !== 9 || !m.s.every(Number.isFinite)) return;
        const { i32, u32, f32 } = copMem(), q = (cops.ptr + COP_Q) >> 2;
        const head = Atomics.load(i32, q + 2) >>> 0, tail = Atomics.load(i32, q + 3) >>> 0;
        if (((head - tail) >>> 0) >= COP_HITS) return;
        const at = (cops.ptr + COP_RX + (head % COP_HITS) * COP_HIT) >> 2;
        u32[at] = m.id; u32[at + 1] = m.c; u32[at + 2] = m.seq >>> 0; u32[at + 3] = m.k >>> 0;
        u32[at + 4] = memory ? mem().u32[(ptr >> 2) + 7] : 0; // arrival in our game's physics clock
        f32.set(m.s, at + 5);
        Atomics.store(i32, q + 2, (head + 1) | 0);
        return;
      } else if (m.t === "race") {
        // Racing together (race.js): the relay sends these to everyone, the sender too.
        const p = m.id === me ? null : players.get(m.id);
        postMessage({ t: "msg", m: { ...m, name: m.id === me ? name : p?.name || "Player", crew: m.id === me ? crew : p?.crew || "" } });
        return;
      } else if ((m.t === "chat" || m.t === "horn") && m.id !== me) {
        // For the page (chat.js): who, their crew, and where their car last was.
        const p = player(m.id);
        postMessage({ t: "msg", m: { ...m, name: p.name || "Player", crew: p.crew, at: p.at } });
        return;
      } else if (m.t === "crew" && typeof m.crew === "string") {
        // A player changed crew while driving (the relay's cleaned name); this
        // player's own change comes back the same way. Others' changes also go
        // to the page as {t:"crew", id, name, crew, was, at}.
        if (m.id === me) crew = m.crew;
        else {
          const p = player(m.id), was = p.crew;
          p.crew = m.crew;
          tell(); // first, so the message finds the room's new colours
          if (p.name && was !== m.crew) postMessage({ t: "msg", m: { t: "crew", id: m.id, name: p.name, crew: m.crew, was, at: p.at } });
          return;
        }
      } else if (m.t === "look" && m.id !== me) {
        // Another player's car look (look.h checks it before building with it); data "" = stock.
        let bytes = null;
        try { bytes = typeof m.data === "string" ? Uint8Array.from(atob(m.data), (c) => c.charCodeAt(0)) : null; } catch {}
        if (!bytes || bytes.length > LOOK_SPACE || !Number.isInteger(m.k)) return;
        looks.set(m.id, { k: m.k >>> 0, bytes });
        lookPut(m.id, m.k, bytes);
        return;
      } else if (m.t === "lights" && m.id !== me) {
        // Another player's car lights (lights.h bits): written into its slot by the lights timer.
        if (!Number.isInteger(m.id) || m.id <= 0 || !Number.isInteger(m.s) || m.s < 0 || m.s > 15) return;
        player(m.id);
        if (m.s) lightsOf.set(m.id, m.s);
        else lightsOf.delete(m.id);
        return;
      } else if (m.t === "pursuit" && m.id !== me) {
        pursuitPut(m);
        return;
      } else if (m.t === "street" && m.id !== me) {
        streetPut(m); // a street race's record (streetrace.h)
        return;
      } else if (m.t === "pong") {
        // The relay's answer to one of this link's pings; it answers in order, so older pings
        // still waiting were not answered (over its limit) and never will be.
        const at = pingSent.get(m.n);
        if (at === undefined) return;
        for (const n of pingSent.keys()) { pingSent.delete(n); if (n === m.n) break; }
        pings.push(performance.now() - at);
        if (pings.length > PING_KEEP) pings.shift();
        pingTell();
        return;
      } else if (m.t === "left") {
        drop(m.id);
        copDrop(m.id);
        rivalDrop(m.id);
      } else if (m.t === "pose" && m.id !== me) {
        const p = player(m.id), now = performance.now();
        const recovered = p.heard > 0 && now - p.heard > POSE_RECOVER_MS;
        p.key = m.k >>> 0;
        p.mode = m.m | 0;
        p.physics = m.physics === 1;
        p.heard = now;
        p.at = [m.s[4], m.s[5], m.s[6]];
        if (p.mode === 1) {
          p.pos = p.at;
          p.vel = capped(m.s[7], m.s[8], m.s[9]);
        }
        const ack = Array.isArray(m.acks) ? m.acks.find((a) => Array.isArray(a) && a[0] === me)?.[1] : 0;
        p.last = { ms: Number.isFinite(m.ts) ? m.ts : 0, s: m.s, contacts: p.physics && !m.ghost ? 1 : 0,
                   ack: Number.isInteger(ack) ? ack : 0,
                   drive: driveOk && m.drive instanceof Uint8Array && m.drive.length === DRIVE_BYTES ? m.drive : null };
        if (!memory || !me) return;
        if (p.slot >= 0) { queuePut(p, now, recovered); return; } // nothing the page shows has changed
        // A free slot goes at once to a player among the best of those waiting for one; the
        // rest wait for arrange(). (The relay sends a joiner its cars nearest first.)
        const free = owners.filter((o) => !o).length;
        if (!free) return;
        const mine = rank(p, now);
        let ahead = 0;
        for (const q of players.values())
          if (q !== p && q.slot < 0 && q.last && q.heard && now - q.heard <= FRESH && order(rank(q, now), mine) < 0) ahead++;
        if (ahead >= free) return;
        take(p, owners.indexOf(0), now);
      } else if (m.t === "where" && Array.isArray(m.p)) {
        // Where everyone driving is (whole metres, heading in degrees), for the world map; a
        // far player's place also stands in for chat until its own poses come.
        const list = [], now = performance.now();
        for (const e of m.p) {
          if (!Array.isArray(e) || e.length !== 4 || !e.every(Number.isFinite) || e[0] === me) continue;
          list.push(e);
          const p = players.get(e[0]);
          if (p && !(p.heard && now - p.heard <= FRESH)) p.at = [e[1], p.at ? p.at[1] : self ? self.at[1] : 0, e[2]];
        }
        postMessage({ t: "where", p: list });
        return;
      }
      tell();
    }

    function connect() {
      ws = new WebSocket(relay);
      ws.binaryType = "arraybuffer";
      ws.onopen = () => { ws.send(JSON.stringify({ t: "hi", name, crew, claim, pk: legacy, physics: physics ? 1 : 0, bin: BIN, cops: 1, drive: 1, rivals: 1 })); tell(); };
      ws.onmessage = (e) => {
        try {
          const m = typeof e.data === "string" ? JSON.parse(e.data) : unpack(e.data);
          if (m) receive(m);
        } catch (error) { console.warn("[online]", error); }
      };
      ws.onclose = () => {
        me = 0;
        serverPhysics = false;
        bin = 0;
        copsOk = false;
        driveOk = false;
        driveLinked();
        copDrop();
        rivalOk = false; rivalDrop();
        linked();
        if (physics) {
          const { i32 } = mem(), c = (ptr + CONTACTS) >> 2;
          Atomics.store(i32, c + 1, Atomics.load(i32, c));
          Atomics.store(i32, c + 3, Atomics.load(i32, c + 2));
        }
        for (const id of new Set([...players.keys(), ...looks.keys()])) drop(id);
        tell();
        setTimeout(connect, 2000);
      };
    }

    onmessage = (e) => {
      const m = e.data;
      if (m.t === "start") {
        relay = m.relay;
        name = m.name;
        crew = m.crew || "";
        claim = m.claim || "";
        legacy = m.legacy || "";
        ghost = !!m.ghost;
        hidden = !!m.hidden;
        connect();
        tell();
      } else if (m.t === "visibility") {
        hidden = !!m.hidden;
      } else if (m.t === "send") {
        // From the page: chat and horn only; the relay checks them again.
        if ((m.m?.t === "chat" || m.m?.t === "horn" || m.m?.t === "race") && me && ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify(m.m));
      } else if (m.t === "crew" && typeof m.crew === "string") {
        // From the page (menu.js): this player's new crew. Linked, the relay
        // cleans it and tells everyone, this player too (receive); otherwise the
        // next hi carries it.
        if (ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ t: "crew", crew: m.crew }));
        else {
          crew = m.crew;
          tell();
        }
      } else if (m.t === "racers" && Array.isArray(m.ids)) {
        // From the page: whom this player races with now (race.js's race, read from race.h's block).
        racers = new Set(m.ids.filter((id) => Number.isInteger(id) && id > 0).slice(0, 8));
        follow();
      } else if (m.t === "ghost") {
        ghost = !!m.on;
        linked();
      } else if (m.t === "block" && !memory) {
        memory = m.memory;
        ptr = m.ptr;
        if (mem().u32[ptr >> 2] !== MAGIC) { console.warn("[online] the game's shared block is not there"); memory = null; return; }
        console.log("[online] linked to the game");
        physics = mem().u32[(ptr >> 2) + 1] >= 2;
        linked();
        if (me && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ t: "physics", on: physics }));
      } else if (m.t === "rival-block" && !rivals) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== RV_MAGIC) return;
        rivals = {memory: m.memory, ptr: m.ptr};
      } else if (m.t === "cop-block" && !cops) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== COP_MAGIC) { console.warn("[online] the game's police block is not there"); return; }
        cops = { memory: m.memory, ptr: m.ptr };
      } else if (m.t === "drive-block" && !drive) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== DRV_MAGIC) { console.warn("[online] the game's drive block is not there"); return; }
        drive = { memory: m.memory, ptr: m.ptr };
        driveLinked();
      } else if (m.t === "lights-block" && !lights) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== LT_MAGIC) { console.warn("[online] the game's lights block is not there"); return; }
        lights = { memory: m.memory, ptr: m.ptr };
      } else if (m.t === "pursuit-block" && !pursuit) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== PU_MAGIC) { console.warn("[online] the game's pursuit block is not there"); return; }
        pursuit = { memory: m.memory, ptr: m.ptr };
      } else if (m.t === "street-block" && !street) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== SR_MAGIC) { console.warn("[online] the game's street block is not there"); return; }
        street = { memory: m.memory, ptr: m.ptr };
      } else if (m.t === "look-block" && !look) {
        if (new Uint32Array(m.memory.buffer)[m.ptr >> 2] !== LOOK_MAGIC) { console.warn("[online] the game's look block is not there"); return; }
        look = { memory: m.memory, ptr: m.ptr };
        for (const [id, l] of looks) lookPut(id, l.k, l.bytes);
      }
    };

    // Shared police chases: a record that arrived, into the game's received ring (dropped when full).
    function pursuitPut(m) {
      if (!pursuit || !Number.isInteger(m.id) || !Number.isInteger(m.k) || m.k < 1 || m.k > 15 ||
          !Number.isInteger(m.a) || m.a < 0 || m.a > 65535 || !Number.isInteger(m.b) || m.b < 0 || m.b > 65535 ||
          !Array.isArray(m.f) || m.f.length !== 4 || !m.f.every(Number.isFinite)) return;
      const u32 = new Uint32Array(pursuit.memory.buffer), i32 = new Int32Array(pursuit.memory.buffer);
      const f32 = new Float32Array(pursuit.memory.buffer), b = pursuit.ptr >> 2;
      const head = Atomics.load(i32, b + 6) >>> 0, tail = Atomics.load(i32, b + 7) >>> 0;
      if (((head - tail) >>> 0) >= PU_RX) return;
      const at = (pursuit.ptr + PU_RX0 + (head % PU_RX) * PU_REC) >> 2;
      u32[at] = m.id; u32[at + 1] = m.k; u32[at + 2] = m.a; u32[at + 3] = m.b;
      f32.set(m.f, at + 4);
      Atomics.store(i32, b + 6, (head + 1) | 0);
      if (m.k === 3) pursuitRole(m.id, m.a);
    }
    // Who is the police (pursuit.h k 3, every second while linked: a bit 0 police, bit 1 siren on),
    // for the page's name tags: a player stays police for 5 s after its last word on it.
    const policeUntil = new Map();
    let policeTold = "";
    function pursuitRole(id, a) {
      if (a & 1) policeUntil.set(id, Date.now() + 5000);
      else policeUntil.delete(id);
      policeTell();
    }
    function policeTell() {
      const now = Date.now();
      for (const [id, until] of policeUntil) if (until < now || !players.has(id)) policeUntil.delete(id);
      const ids = [...policeUntil.keys()].sort((x, y) => x - y), text = ids.join(",");
      if (text !== policeTold) { policeTold = text; postMessage({ t: "police", ids }); }
    }

    // Shared police chases: the game learns our relay id (0 = not linked: it then does nothing),
    // and its records go to the relay as they come (the game sends a few a second).
    setInterval(() => {
      if (!pursuit) return;
      if (policeUntil.size) policeTell();
      const u32 = new Uint32Array(pursuit.memory.buffer), i32 = new Int32Array(pursuit.memory.buffer);
      const f32 = new Float32Array(pursuit.memory.buffer), b = pursuit.ptr >> 2;
      const open = !!me && ws?.readyState === WebSocket.OPEN;
      Atomics.store(i32, b + 2, open ? me : 0);
      let tail = Atomics.load(i32, b + 5) >>> 0;
      const head = Atomics.load(i32, b + 4) >>> 0;
      for (; tail !== head; tail = (tail + 1) >>> 0) {
        const at = (pursuit.ptr + PU_TX0 + (tail % PU_TX) * PU_REC) >> 2;
        const f = Array.from(f32.subarray(at + 4, at + 8), (x) => Math.round(x * 1000) / 1000);
        if (open && f.every(Number.isFinite))
          ws.send(JSON.stringify({ t: "pursuit", to: u32[at], k: u32[at + 1], a: u32[at + 2] & 0xffff, b: u32[at + 3] & 0xffff, f }));
      }
      Atomics.store(i32, b + 5, tail | 0);
    }, 50);

    // Car lights: each slot's player's into the game (0 once the player has gone or the slot is
    // empty), and this player's out when they change.
    setInterval(() => {
      if (!lights) return;
      const i32 = new Int32Array(lights.memory.buffer), b = lights.ptr >> 2, r = b + (LT_REMOTE >> 2);
      for (const id of lightsOf.keys()) if (!players.has(id)) lightsOf.delete(id); // left the room
      for (let i = 0; i < SLOTS; i++) {
        const s = owners[i] ? lightsOf.get(owners[i]) || 0 : 0;
        if (Atomics.load(i32, r + i) !== s) Atomics.store(i32, r + i, s);
      }
      if (!me || ws?.readyState !== WebSocket.OPEN) return;
      if (me !== lightsMe) { lightsMe = me; lightsSent = 0; lightsSentAt = -Infinity; } // a new connection knows none
      const mine = Atomics.load(i32, b + 2) & 15, now = performance.now();
      if (mine === lightsSent || now - lightsSentAt < LIGHTS_MS) return;
      ws.send(JSON.stringify({ t: "lights", s: mine }));
      lightsSent = mine;
      lightsSentAt = now;
    }, 50);

    // Flash to race (mods/core/nfsc/streetrace.h SrShared, a block of its own; the relay header's
    // "Street races"): magic, version, me (+0x08: our relay id, 0 unlinked), opp (+0x0c: the other
    // player of the game's challenge or race now, 0 none), ping (+0x10: the median round trip, ms),
    // phase, races, wins, then tx head/tail and rx head/tail (+0x20..+0x2c), 8 records to send from
    // +0x30 and 16 received from +0x1b0, 0x30 bytes each (id: to / from, k, r, a, 4 floats, the
    // sender's name in 16 bytes), then 16 names from +0x4b0 (id, 16 bytes; the id written last).
    // The game's records go to the relay as {t:"street", to, k, r, a, f}; others' come back as
    // {t:"street", id, k, r, a, f} into the received ring, with the sender's name. The other player
    // of a race is followed (the relay sends its car whatever the distance), and if that player
    // leaves the room the game gets a QUIT (k 5, a 2 "left", r 0) from it.
    const SR_MAGIC = 0x31525453, SR_TX = 8, SR_RX = 16, SR_REC = 0x30, SR_TX0 = 0x30, SR_RX0 = 0x1b0;
    const SR_NAMES = 0x4b0, SR_NAME = 0x14, SR_NAMES_MAX = 16, SR_QUIT = 5, SR_WHY_LEFT = 2;
    let street = null, streetOpp = 0, streetOppHere = false, streetNamesTold = "";
    function streetMem() {
      const buf = street.memory.buffer;
      return { u8: new Uint8Array(buf), u32: new Uint32Array(buf), i32: new Int32Array(buf), f32: new Float32Array(buf) };
    }
    // A name as 16 bytes for the game (Latin-1, the rest as "?"; it keeps the letters its font has).
    function streetText(u8, at, text) {
      for (let i = 0; i < 16; i++) {
        const c = i < 15 && i < text.length ? text.charCodeAt(i) : 0;
        u8[at + i] = c > 255 ? 63 : c;
      }
    }
    // A record into the game's received ring (dropped when full).
    function streetWrite(m, who) {
      const { u8, u32, i32, f32 } = streetMem(), b = street.ptr >> 2;
      const head = Atomics.load(i32, b + 10) >>> 0, tail = Atomics.load(i32, b + 11) >>> 0;
      if (((head - tail) >>> 0) >= SR_RX) return;
      const base = street.ptr + SR_RX0 + (head % SR_RX) * SR_REC, at = base >> 2;
      u32[at] = m.id; u32[at + 1] = m.k; u32[at + 2] = m.r >>> 0; u32[at + 3] = m.a;
      f32.set(m.f, at + 4);
      streetText(u8, base + 0x20, who);
      Atomics.store(i32, b + 10, (head + 1) | 0);
    }
    function streetPut(m) {
      if (!street || !Number.isInteger(m.id) || m.id <= 0 || !Number.isInteger(m.k) || m.k < 1 || m.k > 15 ||
          !Number.isInteger(m.r) || m.r < 0 || m.r > 0xffffffff || !Number.isInteger(m.a) || m.a < 0 || m.a > 65535 ||
          !Array.isArray(m.f) || m.f.length !== 4 || !m.f.every(Number.isFinite)) return;
      streetWrite(m, players.get(m.id)?.name || "");
    }
    // The game learns our relay id and the round trip; its records go out; the names it may need
    // (the players with a car slot first) go in; the other player of a race is followed, and its
    // leaving the room is passed on.
    setInterval(() => {
      if (!street) return;
      const { u8, u32, i32, f32 } = streetMem(), b = street.ptr >> 2;
      const open = !!me && ws?.readyState === WebSocket.OPEN;
      Atomics.store(i32, b + 2, open ? me : 0);
      Atomics.store(i32, b + 4, open && pingShown ? pingShown : 0);
      const opp = Atomics.load(i32, b + 3) >>> 0;
      if (opp !== streetOpp) {
        streetOpp = opp;
        streetOppHere = players.has(opp);
        follow();
      } else if (open && opp) {
        if (players.has(opp)) streetOppHere = true;
        else if (streetOppHere) {
          streetOppHere = false;
          streetWrite({ id: opp, k: SR_QUIT, r: 0, a: SR_WHY_LEFT, f: [0, 0, 0, 0] }, "");
        }
      }
      const known = [...owners.filter((id) => id), ...(opp ? [opp] : []), ...players.keys()];
      const names = [...new Set(known)].filter((id) => players.get(id)?.name).slice(0, SR_NAMES_MAX);
      const text = names.map((id) => `${id}:${players.get(id).name}`).join("\n");
      if (text !== streetNamesTold) {
        streetNamesTold = text;
        for (let i = 0; i < SR_NAMES_MAX; i++) {
          const at = street.ptr + SR_NAMES + i * SR_NAME;
          Atomics.store(i32, at >> 2, 0);
          if (i < names.length) {
            streetText(u8, at + 4, players.get(names[i]).name);
            Atomics.store(i32, at >> 2, names[i]);
          }
        }
      }
      let tail = Atomics.load(i32, b + 9) >>> 0;
      const head = Atomics.load(i32, b + 8) >>> 0;
      for (; tail !== head; tail = (tail + 1) >>> 0) {
        const at = (street.ptr + SR_TX0 + (tail % SR_TX) * SR_REC) >> 2;
        const f = Array.from(f32.subarray(at + 4, at + 8), (x) => Math.round(x * 1000) / 1000);
        if (open && u32[at] > 0 && f.every(Number.isFinite))
          ws.send(JSON.stringify({ t: "street", to: u32[at], k: u32[at + 1], r: u32[at + 2], a: u32[at + 3] & 0xffff, f }));
      }
      Atomics.store(i32, b + 9, tail | 0);
    }, 50);

    // Ping: the round trip to the page when it changes (whole ms; null while not known).
    function pingTell() {
      const sorted = pings.slice().sort((a, b) => a - b), h = sorted.length >> 1;
      const ms = !sorted.length ? null : Math.round(sorted.length & 1 ? sorted[h] : (sorted[h - 1] + sorted[h]) / 2);
      if (ms === pingShown) return;
      pingShown = ms;
      postMessage({ t: "ping", ms });
    }

    // Ping: one out every PING_MS while linked (the first at once on each new connection); a
    // ping unanswered for PING_LOST makes the round trip not known until the next answer.
    setInterval(() => {
      const now = performance.now(), open = !!me && ws?.readyState === WebSocket.OPEN;
      if (!open || me !== pingMe) {
        pingMe = open ? me : 0;
        pingSent.clear();
        pings.length = 0;
        pingAt = -Infinity;
      }
      if (open) {
        for (const [n, at] of pingSent) if (now - at >= PING_LOST) { pingSent.delete(n); pings.length = 0; }
        if (now - pingAt >= PING_MS) {
          pingAt = now;
          pingN = pingN >= 0xffffffff ? 1 : pingN + 1;
          pingSent.set(pingN, now);
          ws.send(JSON.stringify({ t: "ping", n: pingN }));
        }
      }
      pingTell();
    }, 500);

    // This car's look: read twice a second, sent when it changed (see LOOK_MS above).
    setInterval(() => {
      if (!look) return;
      const o = lookOwn();
      if (o && o.stamp && o.stamp !== lookStamp) {
        lookStamp = o.stamp;
        let text = "";
        for (const byte of o.bytes) text += String.fromCharCode(byte);
        lookNewest = { t: "look", k: o.k, data: btoa(text) };
        lookDue = true;
      }
      const now = performance.now();
      if (lookDue && me && ws?.readyState === WebSocket.OPEN && now - lookSentAt >= LOOK_MS) {
        ws.send(JSON.stringify(lookNewest));
        lookDue = false;
        lookSentAt = now;
      }
    }, 500);

    // Polled often, so each game frame's pose is caught; sent at most every SEND_MS.
    setInterval(() => {
      if (!memory || !me || ws.readyState !== WebSocket.OPEN) return;
      const o = own();
      if (o && (!latest || o.frame !== latest.frame)) {
        latest = o;
        // Where this car is, for choosing the nearest players (still while not driving).
        const s = o.s;
        self = { at: [s[4], s[5], s[6]], vel: o.state === 1 ? capped(s[7], s[8], s[9]) : [0, 0, 0] };
      }
      // Impacts are ordered reliable events. Poses may be coalesced by the
      // relay, but cannot erase a hit before the owner acknowledges it.
      if (physics) {
        const { i32, u32, f32 } = mem(), c = (ptr + CONTACTS) >> 2;
        let tail = Atomics.load(i32, c + 1) >>> 0;
        const head = Atomics.load(i32, c) >>> 0;
        while (tail !== head && ws.bufferedAmount < 65536) {
          const at = (ptr + TX + (tail % IMPACTS) * IMPACT) >> 2;
          // The game queues a hit just before publishing its next pose. If we
          // caught that gap, wait for the pose instead of discarding a new hit.
          const age = latest ? (latest.ms - u32[at + 3]) | 0 : -1;
          if (age < 0) break;
          if (serverPhysics && !ghost && u32[at] > me && players.get(u32[at])?.physics &&
              age <= 500) {
            ws.send(JSON.stringify({ t: "impact", to: u32[at], seq: u32[at + 1], k: u32[at + 2],
              ts: u32[at + 3], s: Array.from(f32.subarray(at + 4, at + 16)) }));
          }
          tail = (tail + 1) >>> 0;
        }
        Atomics.store(i32, c + 1, tail | 0);
      }
      if (cops) copHitsOut();
      const now = performance.now();
      if (!latest) return;
      const mode = hidden ? 0 : latest.state;
      if ((latest.ms === sentMs && mode === sentMode) || (mode === sentMode && now - sentAt < SEND_MS)) return;
      // A connected player in a menu keeps the last car pose, but it must be stationary.
      // Mode 0 tells friends to show PAUSED; a real disconnect still sends `left`.
      const stopped = mode === 0;
      const still = stopped ? [...latest.s.slice(0, 7), 0, 0, 0, 0, 0, 0] : latest.s;
      const outbound = mode === latest.state && !stopped ? latest : { ...latest, state: mode, s: still };
      if (bin) {
        const b = packPose(outbound, mode === 1 && physics && serverPhysics, driveOk && mode === 1 ? driveOwn() : null);
        if (b) ws.send(b);
      } else {
        const s = outbound.s.map((x, i) => (i < 4 ? Math.round(x * 1e5) / 1e5 : Math.round(x * 1e3) / 1e3));
        ws.send(JSON.stringify({ t: "pose", k: latest.key, s, m: mode, ts: latest.ms,
          ...(mode === 1 && physics && serverPhysics ? { ghost, acks: latest.acks || [] } : {}) }));
      }
      sentMs = latest.ms;
      sentMode = mode;
      sentAt = now;
    }, POLL_MS);

    // The chase: this player's cops out while there is one (and one empty frame when it ends);
    // other players' cops go when their owner has not sent any for COP_STALE.
    setInterval(() => {
      if (!cops) return;
      const now = performance.now();
      for (const [owner, at] of copHeard) if (now - at > COP_STALE) copDrop(owner);
      const live = !!me && copsOk && ws?.readyState === WebSocket.OPEN;
      Atomics.store(copMem().i32, (cops.ptr >> 2) + 2, live ? 1 : 0); // the game lists its chase only then
      if (!live) { copsSentCount = 0; return; }
      const o = copsOwn();
      // Unchanged (the game paused): again once a second, so the friends keep the cops.
      if (!o || (o.list.length && o.ms === copsSentMs && now - copsSentAt < 1000) || (!o.list.length && !copsSentCount)) return;
      ws.send(packCops(o));
      copsSentMs = o.ms;
      copsSentAt = now;
      copsSentCount = o.list.length;
    }, COP_SEND_MS);

    // Players the relay has not heard from for a while lose their car.
    setInterval(() => {
      const now = performance.now();
      for (const p of players.values()) if (p.heard && now - p.heard > STALE) park(p);
      tell();
    }, 1000);

    // The slots: nearer players in, farther ones out (see arrange).
    setInterval(() => arrange(performance.now()), PICK_MS);
  }

  // Crews. Players who type the same crew name are a crew; case and spaces do
  // not count ("Night Owls" = "nightowls"), as on nfsu2.xyz. Six colours that
  // read well on the dark menus and stay clear of the interface's own cyan
  // (#5fe4f2). Which crew wears which is worked out from the crews in the room
  // alone, so every computer in the room shows the same colours without the
  // relay handing them out: the crews here (this player's and everyone else's),
  // sorted by name, each take their own colour (a hash of the name) unless a
  // crew before them has it, else the next free one along. Up to six crews
  // never share a colour; a seventh and eighth wear their own and may. Colours
  // move only when the set of crews here changes.
  // Defined here because net.js is on the page whenever the game is online;
  // menu.js and chat.js find them on window when they draw, so the order the
  // scripts load in does not matter.
  const CREW_COLORS = ["#ff66cc", "#ffa040", "#a3e85a", "#b48cff", "#ffe14d", "#ff5c5c"];
  // The name as the relay keeps it (tools/carbon-net-relay.mjs label()).
  const crewName = (c) => (typeof c === "string" ? c : "").replace(/[^\p{L}\p{N} _.-]/gu, "").trim().slice(0, 16);
  const crewKey = (c) => crewName(c).toLowerCase().replace(/\s+/g, "");
  function crewHash(key) {
    let h = 0x811c9dc5; // FNV-1a
    for (let i = 0; i < key.length; i++) h = Math.imul(h ^ key.charCodeAt(i), 0x01000193);
    return (h >>> 0) % CREW_COLORS.length;
  }
  let crew = crewName((given && given.crew) || params.get("crew")), myCrew = crewKey(crew);
  let state = { open: false, names: [], slots: [], people: [] };
  let colors = new Map(), colorsFor = null; // crew key -> colour; the crews here they were worked out for
  let paint = 0; // counts changes of colours or of this player's crew: badge and tags dress again
  let police = new Set(); // ids of the players who are the police now (police.h, via the worker)
  const wore = new Map(); // crew key -> the colour it last wore here, for a crew that has just gone
  function recolor() {
    // Plain sort, not localeCompare: the same order on every computer.
    const keys = [...new Set([crew, ...(state.people || []).map((p) => p.crew)].map(crewKey).filter(Boolean))].sort();
    const of = keys.join("/");
    if (of === colorsFor) return false;
    colorsFor = of;
    colors = new Map();
    const taken = new Set();
    for (const key of keys) {
      let i = crewHash(key);
      if (taken.size < CREW_COLORS.length) while (taken.has(i)) i = (i + 1) % CREW_COLORS.length;
      taken.add(i);
      colors.set(key, CREW_COLORS[i]);
      wore.set(key, CREW_COLORS[i]);
    }
    return true;
  }
  function crewColor(c) {
    const key = crewKey(c);
    if (!key) return "";
    return colors.get(key) || wore.get(key) || CREW_COLORS[crewHash(key)];
  }
  recolor();
  window.recompCrewKey = crewKey;
  window.recompCrewColor = crewColor;

  const worker = new Worker(URL.createObjectURL(new Blob([`"use strict";(${link})();`], { type: "text/javascript" })));
  window.recompOnlinePing = null; // online, round trip not known yet: the performance box shows "PING --"
  worker.onmessage = (e) => {
    if (e.data.t === "msg") { dispatchEvent(new CustomEvent("recomp-online-message", { detail: e.data.m })); return; }
    if (e.data.t === "where") {
      // For the world map (worldmap.js): {at: this page's clock, p: [[id, x, z, heading], ...]},
      // everyone else driving, about once a second (relays from 2026-10-25 on).
      window.recompOnlineWhere = { at: performance.now(), p: e.data.p };
      dispatchEvent(new CustomEvent("recomp-online-where", { detail: window.recompOnlineWhere }));
      return;
    }
    if (e.data.t === "ping") {
      // The round trip to the relay in whole ms (median of the last few), null while not known.
      // The kit's performance box (kit/host/performance_overlay.cpp) reads it as its PING line.
      window.recompOnlinePing = e.data.ms;
      return;
    }
    if (e.data.t === "police") { police = new Set(e.data.ids); return; } // their name tags say so
    if (e.data.t !== "state") return;
    state = e.data;
    if (typeof state.crew === "string" && state.crew !== crew) {
      // This player changed crew (recompOnlineCrew): the start page shows it next time.
      crew = state.crew;
      myCrew = crewKey(crew);
      if (given) given.crew = crew;
      try { localStorage.setItem(`${window.recompGame?.id || "nfsc"}.crew`, JSON.stringify(crew)); } catch {}
      paint++;
    }
    if (recolor()) paint++;
    window.recompOnlineState = { ...state, name, crew };
    dispatchEvent(new CustomEvent("recomp-online", { detail: window.recompOnlineState }));
    show();
  };
  worker.onerror = (e) => console.warn("[online] the link stopped:", e.message);
  // Other players' cars are always solid: the user dropped the see-through switch (2026-10-24),
  // so a "nfsc.ghost" left in this browser from before is ignored. recompOnlineGhost stays for
  // tests and a possible later option.
  worker.postMessage({ t: "start", relay, name, crew, claim, legacy, ghost: false, hidden: document.hidden });
  document.addEventListener("visibilitychange", () =>
    worker.postMessage({ t: "visibility", hidden: document.hidden }));
  window.recompOnlineGhost = (on) => worker.postMessage({ t: "ghost", on: !!on });
  // For chat.js: send {t:"chat", text} or {t:"horn", on}; what others send arrives
  // as "recomp-online-message" events ({t, id, name, crew, text | on, at: [x, y, z] or null}).
  // race.js sends {t:"race", k, ...} the same way; those come back from everyone, us too.
  window.recompOnlineSend = (m) => worker.postMessage({ t: "send", m });
  // For the in-game menu: change this player's crew while driving ("" = none).
  // The relay cleans the name and tells everyone, this player too; then
  // recompOnlineState.crew, the badge and the tags follow. It allows a burst of
  // 3 changes, then one per 5 s (tools/carbon-net-relay.mjs LIMITS), counted
  // here too so the menu can say how long to wait. Returns {ok: true, crew}
  // (the cleaned name asked for) or {ok: false, wait: seconds}.
  const crewLimit = { tokens: 3, at: performance.now() };
  window.recompOnlineCrew = (c) => {
    const next = crewName(c);
    if (state.open) {
      const now = performance.now();
      crewLimit.tokens = Math.min(3, crewLimit.tokens + (now - crewLimit.at) / 5000);
      crewLimit.at = now;
      if (crewLimit.tokens < 1) return { ok: false, wait: Math.ceil((1 - crewLimit.tokens) * 5) };
      crewLimit.tokens -= 1;
    }
    worker.postMessage({ t: "crew", crew: next });
    return { ok: true, crew: next };
  };
  // Where this player's car is now (physics space, metres), or null.
  window.recompOnlineSelf = () => {
    if (!block) return null;
    const f = new Float32Array(block.memory.buffer), i = (block.ptr + 0x30) >> 2;
    return [f[i], f[i + 1], f[i + 2]];
  };

  let block = null;
  function attach(b) {
    if (!b || block) return;
    block = b;
    worker.postMessage({ t: "block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-mp", () => attach(globalThis.carbonMpBlock));
  attach(globalThis.carbonMpBlock);
  // The car looks' block (look.h), handed over the same way.
  let lookBlock = null;
  function attachLook(b) {
    if (!b || lookBlock) return;
    lookBlock = b;
    worker.postMessage({ t: "look-block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-look", () => attachLook(globalThis.carbonLookBlock));
  attachLook(globalThis.carbonLookBlock);
  let rivalBlock = null;
  function attachRivals(b) {
    if (!b || rivalBlock) return;
    rivalBlock = b;
    worker.postMessage({t: "rival-block", memory: b.memory, ptr: b.ptr});
  }
  addEventListener("carbon-rivals", () => attachRivals(globalThis.carbonRivalBlock));
  attachRivals(globalThis.carbonRivalBlock);

  // And the police chases' block (cops.h).
  let copBlock = null;
  function attachCops(b) {
    if (!b || copBlock) return;
    copBlock = b;
    worker.postMessage({ t: "cop-block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-cops", () => attachCops(globalThis.carbonCopBlock));
  attachCops(globalThis.carbonCopBlock);

  // The shared police chases' block (pursuit.h), handed over the same way.
  let pursuitBlock = null;
  function attachPursuit(b) {
    if (!b || pursuitBlock) return;
    pursuitBlock = b;
    worker.postMessage({ t: "pursuit-block", memory: b.memory, ptr: b.ptr });
  }
  // Police entry is disabled: police cars can break races. The menu only clears the wish (+0x0c, u16);
  // the game says at +0x0e whether this player is the police (1) or will be once it can (2: in
  // free roam with cops, not chased). Other players' roles come from the worker ({t:"police"}).
  // +0x0e: bits 0-1 the role, bit 2 this player's siren on, bits 8-15 how loud the nearest
  // friend's siren is here (0-255). The wish's bits 8-15 count siren presses (the game toggles).
  const policeWord = () => {
    const b = pursuitBlock;
    return b ? Atomics.load(new Uint16Array(b.memory.buffer), (b.ptr + 0x0e) >> 1) : 0;
  };
  const policeRole = () => policeWord() & 3;
  function repairPress() {
    const b = pursuitBlock, u16 = new Uint16Array(b.memory.buffer), at = (b.ptr + 0x0c) >> 1, w = Atomics.load(u16, at);
    Atomics.store(u16, at, (w & 0xff01) | ((((w >> 1) + 1) & 0x7f) << 1));
  }
  function sirenPress() {
    const b = pursuitBlock, u16 = new Uint16Array(b.memory.buffer), at = (b.ptr + 0x0c) >> 1, w = Atomics.load(u16, at);
    Atomics.store(u16, at, (w & 0xff) | ((((w >> 8) + 1) & 0xff) << 8));
  }
  // chat.js hands its horn key here first: for a police player it is the siren (R3 on a pad).
  window.recompPoliceKey = (e) => {
    if (!pursuitBlock || policeRole() !== 1) return false;
    if (!e.repeat) sirenPress();
    return true;
  };
  // The siren's sound: the game has none for a player-driven police car (its siren follows the
  // cop AI), so a two-tone wail is made here, for this player's own siren and, by distance, the
  // nearest friend's (the game says how loud).
  let sirenAudio = null;
  function sirenSound() {
    updatePoliceHud();
    const word = policeWord(), mine = word & 4 ? 0.16 : 0, near = ((word >> 8) / 255) * 0.2, level = Math.max(mine, near);
    if (!sirenAudio && level <= 0) return;
    try {
      if (!sirenAudio) {
        const ctx = new AudioContext(), osc = ctx.createOscillator(), lfo = ctx.createOscillator(), depth = ctx.createGain();
        const filter = ctx.createBiquadFilter(), gain = ctx.createGain();
        osc.type = "sawtooth"; osc.frequency.value = 950;
        lfo.type = "triangle"; lfo.frequency.value = 0.28; depth.gain.value = 420; // a slow wail, 530-1370 Hz
        filter.type = "lowpass"; filter.frequency.value = 2200;
        gain.gain.value = 0;
        lfo.connect(depth).connect(osc.frequency);
        osc.connect(filter).connect(gain).connect(ctx.destination);
        osc.start(); lfo.start();
        sirenAudio = { ctx, gain };
      }
      if (sirenAudio.ctx.state === "suspended" && level > 0) sirenAudio.ctx.resume().catch(() => {});
      sirenAudio.gain.gain.setTargetAtTime(document.hidden ? 0 : level, sirenAudio.ctx.currentTime, 0.08);
    } catch {}
  }
  setInterval(sirenSound, 100);
  (window.recompMenuActions ||= []).push({
    id: "police", group: "Cops and robbers", keepOpen: true, shown: () => !!pursuitBlock && policeRole() !== 0,
    label: "Return to your own car",
    hint: "Police mode is unavailable. Return to free roam to restore your own car.",
    run: () => {
      const b = pursuitBlock, u16 = new Uint16Array(b.memory.buffer), at = (b.ptr + 0x0c) >> 1, w = Atomics.load(u16, at);
      Atomics.store(u16, at, w & 0xfffe);
      setTimeout(() => dispatchEvent(new Event("recomp-menu-toggles")), 600);
    },
  }, {
    id: "police-siren", group: "Cops and robbers", keepOpen: true, shown: () => !!pursuitBlock && policeRole() === 1,
    label: () => (policeWord() & 4 ? "Siren off" : "Siren on"),
    hint: () => "the horn key (H) or R3 while driving: the nearest player ahead is wanted",
    run: () => { sirenPress(); setTimeout(() => dispatchEvent(new Event("recomp-menu-toggles")), 300); },
  }, {
    id: "police-repair", group: "Cops and robbers", keepOpen: true, shown: () => !!pursuitBlock && policeRole() === 1,
    label: "Repair police car",
    hint: () => policeRepairHint(),
    run: () => { repairPress(); setTimeout(() => dispatchEvent(new Event("recomp-menu-toggles")), 400); },
  });
  const policeCars = ["Patrol car", "Interceptor", "Undercover", "GTO", "SUV", "Rhino", "Cross's Corvette"];
  function policeV3() {
    const b = pursuitBlock;
    if (!b) return null;
    const words = new Uint32Array(b.memory.buffer), at = b.ptr >> 2;
    return words[at + 1] >= 3 ? { words, at } : null;
  }
  function policeRepairHint() {
    const v = policeV3(), wait = v ? v.words[v.at + 0x638 / 4] : 0;
    return wait > 30 ? "Repair locked during a nearby chase" : wait ? `Repair available in ${wait}s after the chase` : "Stop to repair. No repairs during a chase or for 30 seconds afterward.";
  }
  window.recompMenuActions.push({
    id: "police-car", group: "Cops and robbers", keepOpen: true, shown: () => !!policeV3() && policeRole() === 1,
    label: () => { const v = policeV3(); return `Police car: ${policeCars[v ? v.words[v.at + 0x620 / 4] : 0] || policeCars[0]}`; },
    hint: () => "Car changes follow the repair lock. " + policeRepairHint(),
    run: () => { const v = policeV3(); if (!v) return; const at = v.at + 0x620 / 4; Atomics.store(v.words, at, (v.words[at] + 1) % policeCars.length); dispatchEvent(new Event("recomp-menu-toggles")); },
  });
  let policeHud, policeHudText = "";
  function updatePoliceHud() {
    const v = policeV3(), shown = !!v && !!v.words[v.at + 2] && !!block && !!Atomics.load(new Uint32Array(block.memory.buffer), (block.ptr >> 2) + 4) && policeRole() === 1 && (document.querySelector(".cm-shade")?.hidden ?? true);
    if (!shown) { if (policeHud) policeHud.hidden = true; return; }
    const at = v.at + 0x624 / 4, id = v.words[at], status = v.words[at + 1], heat = v.words[at + 2], distance = v.words[at + 3], wait = v.words[at + 4];
    const text = id ? `${["", "Pursuing", "Searching", "Busted", "Escaped"][status] || "Suspect"} · ${distance} m · Heat ${heat}` : "Patrolling · Use the siren to challenge a suspect";
    const repairWait = v.words[at + 5], wrecked = v.words[at + 6];
    const repair = repairWait > 30 ? "Repairs locked · Chase active" : repairWait ? `Repairs locked · ${repairWait}s remaining` : "F9: repair when stopped";
    const full = (wrecked ? "Police car wrecked · Pursuit disabled" : text) + `\n${repair}` + (wrecked ? "" : `\nB: roadblock${wait ? ` (${wait}s)` : ""} · N: spike strip\nD-pad down: hold for roadblock, tap for spikes`);
    if (!policeHud) {
      policeHud = document.createElement("div");
      policeHud.style.cssText = "position:fixed;top:max(5vh,env(safe-area-inset-top));left:50%;transform:translateX(-50%);max-width:90vw;padding:.65em 1em;border-left:3px solid #82c8ff;background:#10171edb;color:#f4f8ff;font:500 clamp(12px,1.25vw,18px)/1.5 system-ui;white-space:pre-line;text-align:center;pointer-events:none;z-index:20";
      document.body.append(policeHud);
    }
    policeHud.hidden = false;
    if (full !== policeHudText) { policeHudText = full; policeHud.textContent = full; }
  }
  addEventListener("carbon-pursuit", () => attachPursuit(globalThis.carbonPursuitBlock));
  attachPursuit(globalThis.carbonPursuitBlock);

  // The car lights' block (lights.h), handed over the same way.
  let lightsBlock = null;
  function attachLights(b) {
    if (!b || lightsBlock) return;
    lightsBlock = b;
    worker.postMessage({ t: "lights-block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-lights", () => attachLights(globalThis.carbonLightsBlock));
  attachLights(globalThis.carbonLightsBlock);

  // The street races' block (streetrace.h), handed over the same way.
  let streetBlock = null;
  function attachStreet(b) {
    if (!b || streetBlock) return;
    streetBlock = b;
    worker.postMessage({ t: "street-block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-street", () => attachStreet(globalThis.carbonStreetBlock));
  attachStreet(globalThis.carbonStreetBlock);

  // The driving block (mp_drive.h), handed over the same way.
  let driveBlock = null;
  function attachDrive(b) {
    if (!b || driveBlock) return;
    driveBlock = b;
    worker.postMessage({ t: "drive-block", memory: b.memory, ptr: b.ptr });
  }
  addEventListener("carbon-drive", () => attachDrive(globalThis.carbonDriveBlock));
  attachDrive(globalThis.carbonDriveBlock);

  // Racing together: the players in the race this player is in (race.js writes them into
  // race.h's block, RaceShared: page seq +0x08, then RacePage: session +0x0c, count +0x1c,
  // peers from +0x60, 0x40 bytes each, relay id first). Their cars keep their slots, and the
  // relay sends them whatever the distance.
  let racersTold = "[]";
  setInterval(() => {
    const b = globalThis.carbonRaceBlock, ids = [];
    if (b) {
      const u32 = new Uint32Array(b.memory.buffer), at = b.ptr >> 2, seq = u32[at + 2];
      if (u32[at] !== 0x31435243 || seq & 1) return; // not there, or being written: next time
      if (u32[at + 3]) for (let i = 0; i < Math.min(u32[at + 7], 8); i++) {
        const id = u32[(b.ptr + 0x60 + i * 0x40) >> 2];
        if (id) ids.push(id);
      }
      if (u32[at + 2] !== seq) return;
    }
    const text = JSON.stringify(ids);
    if (text === racersTold) return;
    racersTold = text;
    worker.postMessage({ t: "racers", ids });
  }, 500);

  // The status badge, in the game's menu style: cyan dot when linked, then
  // your name and your crew as a chip in its colour.
  const badge = document.createElement("div");
  // Left of it, the in-game menu's button (menu.js).
  badge.style.cssText = "position:fixed;left:64px;top:12px;z-index:5;pointer-events:none;display:flex;align-items:center;gap:8px;" +
    "font:italic 700 12px/1 'Avenir Next Condensed','DIN Condensed','Bahnschrift',system-ui,sans-serif;letter-spacing:.12em;" +
    "text-transform:uppercase;color:#e9f6ff;background:rgba(6,12,22,.62);border:1px solid rgba(95,228,242,.25);" +
    "padding:6px 12px;transform:skewX(-12deg)";
  const dot = document.createElement("span");
  dot.style.cssText = "width:7px;height:7px;border-radius:50%;flex:none;transform:skewX(12deg)";
  const label = document.createElement("span");
  label.style.cssText = "transform:skewX(12deg)";
  const chip = document.createElement("span"), chipText = document.createElement("span");
  chipText.style.cssText = "display:inline-block;transform:skewX(12deg)";
  chip.append(chipText);
  chip.style.cssText = "display:none;padding:2px 7px;margin:-3px 0;border:1px solid;font-size:11px;letter-spacing:.1em";
  const rest = document.createElement("span");
  rest.style.cssText = "transform:skewX(12deg)";
  badge.append(dot, label, chip, rest);
  document.body.append(badge);
  let badgePaint = -1;
  function show() {
    const color = state.open ? "#5fe4f2" : "#f2b45f";
    dot.style.background = color;
    dot.style.boxShadow = `0 0 8px ${color}`;
    label.textContent = state.open ? name : "Online · connecting…";
    if (badgePaint !== paint) {
      badgePaint = paint;
      const mine = crewColor(crew);
      chipText.textContent = crew;
      chip.style.color = chip.style.borderColor = mine;
      chip.style.background = mine ? `${mine}22` : "";
    }
    chip.style.display = state.open && myCrew ? "inline-block" : "none";
    rest.textContent = !state.open ? ""
      : state.names.length ? `· ${state.names.length + 1} online` : "· nobody else here yet";
  }
  show();

  // Name tags over the other players' cars: "Name · 12 m", after a chip with
  // the player's crew in its colour. Your own crew's tags get a frame and a glow
  // in that colour, and stay bright however far away the car is.
  const canvas = document.getElementById("canvas");
  const tags = [];
  let views = null, crewsOf = null, crews = new Map(); // player id -> crew, from state.people
  function dress(el, c) {
    const color = crewColor(c), mate = !!myCrew && crewKey(c) === myCrew;
    el.crew = c;
    el.paint = paint;
    el.mate = mate;
    el.chip.textContent = c;
    el.chip.style.display = color ? "inline-block" : "none";
    el.chip.style.color = el.chip.style.borderColor = color;
    el.chip.style.background = color ? `${color}2a` : "";
    el.style.borderColor = mate ? color : "transparent";
    el.style.background = mate ? "rgba(6,12,22,.62)" : "rgba(0,0,0,.35)";
    el.style.boxShadow = mate ? `0 0 10px ${color}88` : "none";
  }
  function drawTags() {
    requestAnimationFrame(drawTags);
    if (!block) return;
    if (crewsOf !== state) {
      crewsOf = state;
      crews = new Map((state.people || []).map((p) => [p.id, p.crew || ""]));
    }
    const buf = block.memory.buffer;
    if (!views || views.buf !== buf) views = { buf, f32: new Float32Array(buf) };
    const f32 = views.f32, box = canvas.getBoundingClientRect();
    for (let i = 0; i < SLOTS; i++) {
      const who = state.slots[i];
      const b = (block.ptr + LABELS + i * 16) >> 2, depth = f32[b + 2];
      let el = tags[i];
      if (!who || !(depth > 0)) {
        if (el) el.style.display = "none";
        continue;
      }
      if (!el) {
        el = tags[i] = document.createElement("div");
        el.style.cssText = "position:fixed;left:0;top:0;pointer-events:none;z-index:4;white-space:nowrap;" +
          "font:600 14px system-ui,sans-serif;color:#fff;text-shadow:0 1px 3px #000,0 0 2px #000;" +
          "padding:1px 6px;border-radius:4px;background:rgba(0,0,0,.35);border:1px solid transparent";
        el.chip = document.createElement("span");
        el.chip.style.cssText = "display:none;margin:0 6px 0 -2px;padding:0 5px;border:1px solid;border-radius:3px;" +
          "font:italic 700 .82em/1.3 'Avenir Next Condensed','DIN Condensed','Bahnschrift',system-ui,sans-serif;" +
          "letter-spacing:.08em;text-transform:uppercase;vertical-align:1px";
        el.text = document.createTextNode("");
        el.badge = document.createElement("span");
        el.badge.textContent = "POLICE ";
        el.badge.style.cssText = "display:none;margin-right:6px;padding:1px 4px;border-radius:2px;background:#b8dfff;color:#101b2b;text-shadow:none;font:800 .8em/1.4 system-ui;letter-spacing:.04em";
        el.append(el.badge, el.chip, el.text);
        el.crew = null;
        document.body.append(el);
      }
      const c = crews.get(state.slotIds?.[i]) || "";
      if (el.crew !== c || el.paint !== paint) dress(el, c);
      const isPolice = police.has(state.slotIds?.[i]);
      el.badge.style.display = isPolice ? "inline-block" : "none";
      const paused = state.slotModes?.[i] === 0;
      const text = paused ? `${who} · PAUSED` : `${who} · ${Math.round(depth)} m`;
      if (el.text.data !== text) el.text.data = text;
      const size = Math.max(11, Math.min(16, 17 - depth / 25));
      el.style.display = "block";
      el.style.fontSize = `${size}px`;
      el.style.opacity = depth > 250 && !el.mate && !isPolice ? "0.6" : "1";
      el.style.transform = `translate(${box.left + f32[b] * box.width}px, ${box.top + f32[b + 1] * box.height}px) translate(-50%, -100%)`;
    }
  }
  requestAnimationFrame(drawTags);
})();
