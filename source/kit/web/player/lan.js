// lan.js - the page's side of the kit's virtual LAN (runtime/winsock.cpp). It
// carries the frames of the game's own network code between the LAN mailbox
// in the game's memory (runtime/lan_mailbox.h: the same numbers as here) and
// one WebSocket to the server's switch at <server>/lan, which gives this
// browser a virtual address 10.77.x.y and routes frames by address.
//
// The player page loads it when the start options carry lan: {url, name}, or
// the address has ?lan=ws://host:port (for tests); it also sets RECOMP_LAN=1,
// which turns the kit's Winsock on. The kit publishes the mailbox as
// globalThis.recompLanMailbox ({memory, ptr}) with a "recomp-lan" event at the
// game's first WSAStartup, and says in `want` whether the game has a network
// session open; the link is made while it does and let go 10 s after.
//
// The link runs in a worker: Chrome slows a background tab's own timers to
// once a second and later once a minute while the game keeps running there,
// and a link on the page's thread would stall the game's connections whenever
// its window is behind another.
//
// Frames on the WebSocket (binary, big-endian):
//   [0] kind (3 UDP, 4 TCP)  [1..3] 0  [4..7] source address  [8..11] destination
//   [12..13] source port  [14..15] destination port  [16..] payload (LAN header + data)
// Text messages are JSON: {"t":"lan","vip","netmask"} and {"t":"gone","vip"}
// from the switch, {"t":"hello","name","pk"} from here. pk is this browser's
// player key (random, kept in localStorage): the server ties the name to it,
// so another browser cannot sign in under it (EA Messenger's friends follow
// the name). A test page may give one with ?pk=, only together with ?testing;
// that one is never stored.
"use strict";
(() => {
  const params = new URLSearchParams(location.search);
  const given = globalThis.recompLan;
  const raw = (given && given.url) || params.get("lan");
  if (!raw) return;
  // ws(s)://host:port or http(s)://... ; "/lan" is added when the path lacks it.
  let url;
  try {
    const u = new URL(raw, location.href);
    if (u.protocol === "http:") u.protocol = "ws:";
    else if (u.protocol === "https:") u.protocol = "wss:";
    if (!/\/lan$/.test(u.pathname)) u.pathname = u.pathname.replace(/\/?$/, "/lan");
    // The server's key (CARBON_RELAY_KEY), unless the address carries one: recompLan.key or ?key=.
    const key = (given && given.key) || params.get("key");
    if (key && !u.searchParams.has("key")) u.searchParams.set("key", key);
    url = u.href;
  } catch {
    console.warn(`[lan] "${raw}" is not a server address: no virtual LAN`);
    return;
  }
  const name = ((given && given.name) || params.get("name") || "Player").slice(0, 32);
  let pk = "";
  if (params.has("testing") && /^[A-Za-z0-9_-]{16,64}$/.test(params.get("pk") || "")) {
    pk = params.get("pk");
  } else {
    try {
      pk = localStorage.getItem("carbon-player-key") || "";
      if (!/^[A-Za-z0-9_-]{16,64}$/.test(pk)) {
        const b = crypto.getRandomValues(new Uint8Array(24));
        pk = btoa(String.fromCharCode(...b)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
        localStorage.setItem("carbon-player-key", pk);
      }
    } catch {
      pk = ""; // no storage (a private window): the name is used, not claimed
    }
  }

  // Everything below runs in the worker; it is started from this function's text.
  function link() {
    // Header words (u32 index = byte offset / 4) and sizes: runtime/lan_mailbox.h.
    const MAGIC = 0x314e4c52, VERSION = 1, HDR_BYTES = 256, SLOT = 1536, SLOT_HDR = 32;
    const TX_SLOTS = 128, RX_SLOTS = 256, MAX_PAYLOAD = 1472, LAN_HDR = 8;
    const W_MAGIC = 0, W_VERSION = 1, W_HDR = 2, W_SLOT = 3, W_TX_SLOTS = 4, W_RX_SLOTS = 5, W_TX_OFF = 6, W_RX_OFF = 7;
    const W_WANT = 8, W_TX_HEAD = 9, W_TX_BELL = 10, W_RX_TAIL = 11, W_KIT_ALIVE = 12;
    const W_PAGE_STATE = 16, W_VIP = 17, W_NETMASK = 18, W_GENERATION = 19;
    const NAME_OFF = 128; // player_name, char[64]
    const W_TX_TAIL = 20, W_RX_HEAD = 21, W_RX_BELL = 22, W_RX_FULL_DROP = 23, W_TX_OFFLINE_DROP = 24;
    const KIND_UDP = 3, KIND_TCP = 4, TCP_GONE = 6;
    const PAGE_CONNECTING = 1, PAGE_ONLINE = 2, PAGE_OFFLINE = 3;
    const IDLE_MS = 10000;

    let url = "", name = "", pk = "", memory = null, ptr = 0, views = null;
    let ws = null, online = false, vip = 0, generation = 0, retry = 0, retryTimer = 0, idleSince = 0, flushTimer = 0;
    const backlog = []; // received TCP frames waiting for RX ring room, in order: TCP is never dropped
    const stats = { out: 0, in: 0, rxFull: 0, offline: 0, waitedMax: 0 };

    const log = (text) => postMessage({ t: "log", text });
    const ipStr = (n) => `${n >>> 24}.${(n >>> 16) & 255}.${(n >>> 8) & 255}.${n & 255}`;
    const ipNum = (s) => {
      const m = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(String(s || ""));
      if (!m || m.slice(1).some((x) => +x > 255)) return 0;
      return ((+m[1] << 24) | (+m[2] << 16) | (+m[3] << 8) | +m[4]) >>> 0;
    };
    function mem() {
      const buf = memory.buffer; // a new buffer after the memory grows
      if (!views || views.buf !== buf)
        views = { buf, i32: new Int32Array(buf), u8: new Uint8Array(buf), dv: new DataView(buf) };
      return views;
    }
    const at = (w) => (ptr >>> 2) + w;
    const load = (w) => Atomics.load(mem().i32, at(w)) >>> 0;
    const store = (w, v) => Atomics.store(mem().i32, at(w), v | 0);
    function ring() {
      const { i32 } = mem();
      Atomics.add(i32, at(W_RX_BELL), 1);
      Atomics.notify(i32, at(W_RX_BELL));
    }
    function tell() {
      postMessage({ t: "state", state: memory ? load(W_PAGE_STATE) : 0, vip: vip ? ipStr(vip) : "", generation,
                    want: memory ? load(W_WANT) : 0, kitAlive: memory ? load(W_KIT_ALIVE) : 0,
                    stats: { ...stats, backlog: backlog.length } });
    }
    function setState(s) {
      if (!memory) return;
      store(W_PAGE_STATE, s);
      ring(); // the kit's network thread notices at once
      tell();
    }

    // A frame from the switch into the RX ring; false while the ring is full.
    function put(f) {
      const { u8, dv } = mem();
      const head = load(W_RX_HEAD), tail = load(W_RX_TAIL);
      if (((head - tail) >>> 0) >= RX_SLOTS) return false;
      const s = ptr + load(W_RX_OFF) + (head % RX_SLOTS) * SLOT;
      const be = new DataView(f.buffer, f.byteOffset, f.byteLength);
      dv.setUint32(s, f.length - 16, true);
      dv.setUint32(s + 4, f[0], true);
      dv.setUint32(s + 8, be.getUint32(4), true);
      dv.setUint32(s + 12, be.getUint32(8), true);
      dv.setUint16(s + 16, be.getUint16(12), true);
      dv.setUint16(s + 18, be.getUint16(14), true);
      dv.setUint32(s + 20, 0, true);
      dv.setFloat64(s + 24, Date.now(), true);
      u8.set(f.subarray(16), s + SLOT_HDR);
      store(W_RX_HEAD, head + 1);
      return true;
    }
    function flushBacklog() {
      flushTimer = 0;
      let n = 0;
      while (backlog.length && put(backlog[0])) { backlog.shift(); n++; }
      if (n) ring();
      if (backlog.length) {
        stats.waitedMax = Math.max(stats.waitedMax, backlog.length);
        flushTimer = setTimeout(flushBacklog, 2); // the kit frees slots without a bell
      }
    }
    function received(f) {
      if (!memory || f.length < 16 + LAN_HDR || f.length - 16 > MAX_PAYLOAD) return;
      const kind = f[0];
      if (kind !== KIND_UDP && kind !== KIND_TCP) return;
      stats.in++;
      if (kind === KIND_TCP && backlog.length) { backlog.push(f); return; } // behind the ones waiting
      if (put(f)) { ring(); return; }
      if (kind === KIND_TCP) {
        backlog.push(f);
        if (!flushTimer) flushTimer = setTimeout(flushBacklog, 2);
        return;
      }
      store(W_RX_FULL_DROP, load(W_RX_FULL_DROP) + 1); // a datagram may be lost, as on any network
      stats.rxFull++;
    }
    // A host left the switch: the kit resets its connections to it.
    function gone(addr) {
      const f = new Uint8Array(16 + LAN_HDR);
      const be = new DataView(f.buffer);
      f[0] = KIND_TCP;
      be.setUint32(4, addr);
      be.setUint32(8, vip);
      f[16] = TCP_GONE;
      received(f);
    }

    // The kit's TX ring onto the WebSocket; frames are dropped (and counted)
    // while the link is down, as a cable pulled out would.
    function drain() {
      if (!memory) return;
      const { u8, dv } = mem();
      let tail = load(W_TX_TAIL);
      const head = load(W_TX_HEAD), off = load(W_TX_OFF);
      while (tail !== head) {
        const s = ptr + off + (tail % TX_SLOTS) * SLOT;
        const len = dv.getUint32(s, true);
        if (ws && online && ws.readyState === WebSocket.OPEN && len >= LAN_HDR && len <= MAX_PAYLOAD) {
          const f = new Uint8Array(16 + len);
          const be = new DataView(f.buffer);
          f[0] = dv.getUint32(s + 4, true);
          be.setUint32(4, dv.getUint32(s + 8, true));
          be.setUint32(8, dv.getUint32(s + 12, true));
          be.setUint16(12, dv.getUint16(s + 16, true));
          be.setUint16(14, dv.getUint16(s + 18, true));
          f.set(u8.subarray(s + SLOT_HDR, s + SLOT_HDR + len), 16);
          ws.send(f);
          stats.out++;
        } else {
          store(W_TX_OFFLINE_DROP, load(W_TX_OFFLINE_DROP) + 1);
          stats.offline++;
        }
        tail = (tail + 1) >>> 0;
        store(W_TX_TAIL, tail);
      }
    }

    function control(m) {
      if (m.t === "lan") {
        const a = ipNum(m.vip), mask = ipNum(m.netmask || "255.255.248.0");
        if (!a) return;
        vip = a;
        generation = (generation + 1) >>> 0;
        // Address first, then the state that says it is valid.
        store(W_VIP, vip);
        store(W_NETMASK, mask);
        store(W_GENERATION, generation);
        online = true;
        retry = 0;
        setState(PAGE_ONLINE);
        log(`online: this browser is ${m.vip} on ${url}`);
      } else if (m.t === "gone") {
        const a = ipNum(m.vip);
        if (a) gone(a);
      }
    }

    function connect() {
      const sock = new WebSocket(url);
      ws = sock;
      online = false;
      sock.binaryType = "arraybuffer";
      if (load(W_PAGE_STATE) !== PAGE_OFFLINE) setState(PAGE_CONNECTING);
      sock.onopen = () => sock.send(JSON.stringify({ t: "hello", name, pk }));
      sock.onmessage = (e) => {
        if (ws !== sock) return;
        if (typeof e.data === "string") {
          try { control(JSON.parse(e.data)); } catch (error) { log(`a control message was not understood: ${error.message}`); }
        } else {
          received(new Uint8Array(e.data));
        }
      };
      sock.onerror = () => {};
      sock.onclose = (e) => {
        if (ws !== sock) return;
        const was = online;
        ws = null;
        online = false;
        backlog.length = 0; // frames for that link: the kit resets its connections
        if (load(W_WANT) === 1) {
          const wait = Math.min(10000, 1000 * 2 ** retry++);
          setState(PAGE_OFFLINE);
          log(`${was ? "lost the switch" : "could not reach the switch"} (${e.code}); trying again in ${wait / 1000} s`);
          retryTimer = setTimeout(() => {
            retryTimer = 0;
            if (load(W_WANT) === 1 && !ws) connect();
          }, wait);
        } else {
          setState(PAGE_CONNECTING); // let go while unwanted; connects again when wanted
          log("let the switch go: the game has no network session");
        }
      };
    }

    // want: the game has a network session (WSAStartup .. its last WSACleanup).
    function wantCheck() {
      if (!memory) return;
      if (load(W_WANT) === 1) {
        idleSince = 0;
        if (!ws && !retryTimer) connect();
      } else if (ws) {
        if (!idleSince) idleSince = Date.now();
        else if (Date.now() - idleSince >= IDLE_MS) {
          idleSince = 0;
          ws.close(1000, "idle");
        }
      }
    }

    // Sleeps on the kit's TX bell (rung for frames and for a change of want).
    async function txLoop() {
      for (;;) {
        const bell = Atomics.load(mem().i32, at(W_TX_BELL));
        drain();
        wantCheck();
        const r = Atomics.waitAsync(mem().i32, at(W_TX_BELL), bell, 1000);
        if (r.async) await r.value;
      }
    }

    onmessage = (e) => {
      const m = e.data;
      if (m.t === "start") {
        url = m.url;
        name = m.name;
        pk = m.pk || "";
      } else if (m.t === "mailbox" && !memory) {
        memory = m.memory;
        ptr = m.ptr >>> 0; // above 2 GB the kit's address arrives as a negative int32
        const bad = load(W_MAGIC) !== MAGIC || load(W_VERSION) !== VERSION || load(W_HDR) !== HDR_BYTES ||
          load(W_SLOT) !== SLOT || load(W_TX_SLOTS) !== TX_SLOTS || load(W_RX_SLOTS) !== RX_SLOTS ||
          load(W_TX_OFF) !== HDR_BYTES || load(W_RX_OFF) !== HDR_BYTES + TX_SLOTS * SLOT;
        if (bad) {
          log("the game's LAN mailbox is not the layout this page knows: no virtual LAN");
          memory = null;
          return;
        }
        // The player's name for the kit (lan_mailbox.h player_name, bytes 128..191), before
        // the first state change: the game's own Online mode signs in under it.
        const nameBytes = new TextEncoder().encode(name).slice(0, 63);
        const { u8 } = mem();
        u8.fill(0, ptr + NAME_OFF, ptr + NAME_OFF + 64);
        u8.set(nameBytes, ptr + NAME_OFF);
        log(`linked to the game's LAN mailbox; switch ${url}`);
        setState(PAGE_CONNECTING);
        txLoop();
        setInterval(() => { wantCheck(); tell(); }, 1000);
      }
    };
  }

  const worker = new Worker(URL.createObjectURL(new Blob([`"use strict";(${link})();`], { type: "text/javascript" })));
  worker.onmessage = (e) => {
    const m = e.data;
    if (m.t === "log") {
      console.log(`[lan] ${m.text.replace(/([?&]key=)[^&\s]*/g, "$1(hidden)")}`); // the key stays out of logs
    } else if (m.t === "state") {
      window.recompLanState = m;
      dispatchEvent(new CustomEvent("recomp-lan-state", { detail: m }));
    }
  };
  worker.onerror = (e) => console.warn("[lan] the link stopped:", e.message);
  worker.postMessage({ t: "start", url, name, pk });
  // Leaving for another page: Chrome may keep this one in its back/forward cache, link and all,
  // so friends would still see the player online. The link ends at once instead, and a page
  // brought back from that cache starts again (its game has lost the link).
  addEventListener("pagehide", (e) => { if (e.persisted) worker.terminate(); });
  addEventListener("pageshow", (e) => { if (e.persisted) location.reload(); });

  let attached = false;
  function attach(box) {
    if (!box || attached) return;
    attached = true;
    worker.postMessage({ t: "mailbox", memory: box.memory, ptr: box.ptr });
  }
  addEventListener("recomp-lan", () => attach(globalThis.recompLanMailbox));
  attach(globalThis.recompLanMailbox);
})();
