// saves.js - the start page's Saves panel: a backup file of this window's save,
// and cloud saves sealed in the browser with a keyphrase (no account).
//
// start.js loads this file once the window's save slot is known and hands it
// window.recompStartPage (slot, store, panel, profileFolder, server, ...).
// The save is the slot's folder in the browser's private storage (OPFS):
// <game id>/profile, or <game id>/profile-N for the Nth window on a computer.
// It holds registry.json (the game's settings), mod-settings.json (F10) and
// Documents/NFS Carbon/<career>/ (the careers). Logs, caches and markers that
// the game or the player page make by themselves are left out (see SKIP).
//
// Backup file: a plain ZIP (stored, not compressed) of the folder, paths
// relative to it, like the launcher's and the Mac app's save export. The ZIP
// comment is {"backup":"nfsc-save","version":1,"made":<ISO time>,"folder":...}.
// Restore reads stored and deflated ZIPs (so the Mac app's export works too),
// checks every CRC, then replaces the folder. The save it replaces is kept in
// <game id>/saves-undo-<folder>.zip, so a restore can be undone once.
//
// Cloud: the same ZIP, sealed here and stored on the online server under an id;
// the server sees neither the keyphrase nor the save. Keyphrase "carbon-" plus
// 8 words from WORDS (256 words, 64 random bits from crypto.getRandomValues).
//   id   = hex PBKDF2-HMAC-SHA-256(keyphrase, salt "nfsc cloud save id v1", 600000, 256 bits)
//   blob = "NFSCSAVE" | version 1 | kdf 1 (PBKDF2-HMAC-SHA-256) | iterations (uint32, big-endian)
//          | salt (16 random bytes) | iv (12 random bytes) | AES-256-GCM(key, iv, aad = these 42 bytes)(zip)
//   key  = PBKDF2-HMAC-SHA-256(keyphrase, that salt, those iterations) as an AES-GCM 256 key
// Both derivations are slow on purpose: knowing the ids does not make the
// keyphrases quicker to guess. Endpoints: tools/carbon-net-relay.mjs header.
// Remove from cloud deletes the blob there (DELETE); this browser forgets the
// keyphrase only if the player chooses that, else it stops sending it up by itself.
// The server is the start page's (Online, Server), as http(s) with ?key=.
// For tests, localStorage "nfsc.savesServer" overrides it, for example
//   localStorage.setItem("nfsc.savesServer", '"http://127.0.0.1:61234?key=abc"')
// (JSON like the page's other settings; a bare URL works too; remove it to go back).
"use strict";
(() => {
  if (window.recompSavesLoaded) return;
  window.recompSavesLoaded = true;
  const GAME = window.recompGame?.id || "nfsc";
  const MAGIC = "NFSCSAVE";
  const ITERATIONS = 600000;
  const ID_SALT = "nfsc cloud save id v1";
  const CLOUD_MAX = 8 * 1024 * 1024; // the server's limit
  const PREFIX = "carbon-";

  // Keyphrase words. Written for this file and given to the public domain
  // (CC0): short everyday words, no two starting with the same four letters.
  const WORDS = `amber anchor apple arrow atlas autumn badge bamboo banjo barrel basil beacon bear berry bison blade
    blaze bloom bolt bonus boost brass brick bridge bronze brook bubble buffalo cabin cactus camel candle
    canyon cargo castle cedar cherry chess chrome cider circle citrus clover cobalt cocoa comet copper coral
    cotton cougar coyote crane crater crystal cyclone daisy dawn delta desert diesel dingo dolphin dragon drift
    drum dune eagle echo ember engine falcon fender fern ferry fiddle flame flint forest fossil fox
    frost galaxy garage garnet gecko geyser ginger glacier globe granite grape gravel hammock hawk hazel helmet
    heron hickory honey horizon hornet husky iceberg igloo indigo iris island ivory jaguar jasmine jelly jigsaw
    jungle juniper kayak kettle kiwi koala lagoon lantern laser lava lemon lilac lime linen lizard llama
    lobster locket lotus lunar magnet mango maple marble meadow melon meteor mint mirror mocha monsoon moose
    mosaic motor muffin nectar neon nickel nitro noodle nova oasis ocean olive onyx opal orbit orchid
    osprey otter oyster paddle panda panther papaya parrot pebble pepper pigeon pilot piston pixel planet plaza
    plum polar pony poppy prism puffin pulsar puma quartz quill rabbit radar radio rally raven reef
    rhino ribbon river robin rocket rodeo rover ruby saddle safari saffron salmon sandal sapphire satin scooter
    shadow sierra silver sketch sloth sparrow spiral sprint spruce squid stereo storm summit sunset swan tango
    tiger timber tornado toucan tractor trophy tulip tundra turbo turtle tuxedo umbrella valley velvet violet viper
    volcano voyage wafer walnut walrus wasabi whale willow winter wizard wombat yacht yarn zebra zenith zigzag`.split(/\s+/);
  const WORD_SET = new Set(WORDS);

  // What a backup leaves out, by path inside the folder (any case): things
  // the game, the kit or the browser make again by themselves.
  function skipped(path) {
    const parts = path.toLowerCase().split("/"), top = parts[0], name = parts[parts.length - 1];
    return name === ".ds_store" || name === "thumbs.db" || name.startsWith("._") || top === "__macosx" ||
      name.endsWith(".crswap") ||                         // the browser's half-written copy of a file
      name.endsWith(".log") || top === "logs" ||            // logs
      top === "shaderwarm" ||                               // the Mac app's shader cache
      (parts.length === 1 && (top === ".copied" || top === "game-path.txt")) || // window-copy marker, this PC's game path
      top === "game" || top.startsWith("launcher-") ||      // what the kit's own export leaves out
      path.toLowerCase() === "mods/run.json";               // written again at every start
  }

  class SaveError extends Error {
    constructor(code, detail) { super(detail || code); this.code = code; this.detail = detail; }
  }
  const MESSAGES = {
    "unreachable": "The server can't be reached right now. Your save is safe in this browser; try again later.",
    "no-server": "There is no server to keep the cloud save on (Online, Server).",
    "off": "This server doesn't keep cloud saves.",
    "forbidden": "The server refused the request: its key is wrong.",
    "too-many": "Too many tries in a row. Wait a minute, then try again.",
    "too-big": "This save is too big for the cloud (over 8 MB).",
    "full": "The server has no room for more cloud saves.",
    "busy-server": "Another computer is saving to the cloud right now. Try again in a moment.",
    "changed": "The cloud save changed just now (another computer saved to it), so nothing was removed. Try again to remove the new one.",
    "no-remove": "Nothing was removed: the server didn't take the request. It may run an older version that can't remove cloud saves yet, or the connection dropped. Try again later.",
    "not-found": "No cloud save was found for this keyphrase on this server. Check the words, and the server under Online.",
    "wrong-phrase": "Wrong keyphrase: the cloud save can't be opened with it.",
    "not-sealed": "The cloud save is damaged and can't be opened.",
    "not-zip": "This file isn't a save backup (it isn't a .zip file).",
    "not-backup": "This .zip doesn't hold a Carbon save. Use a file made with Download backup.",
    "damaged": "This backup is damaged: some of its files don't read back correctly.",
    "packed": "This is a .rar or .7z archive. Unpack it first, then choose the folder or the career file inside. A .zip works as it is.",
    "no-career": "No Need for Speed Carbon career was found there. Choose the NFS Carbon folder from Documents, or a career file inside it.",
    "not-14": "This save doesn't match the PC game version 1.4, so it can't be used here. Update the game to 1.4, load the career, save it, and import it again.",
    "corrupt": "This save is damaged: it fails the checks the game makes when loading a career. Your current save is unchanged.",
    "empty": "There is no save in this browser yet. Play first, then back it up.",
    "in-use": "Part of the save is in use. Is the game open in another window?",
    "quota": "This browser is out of storage space.",
  };
  function messageFor(error) {
    if (error instanceof SaveError) {
      if (error.code === "status") return `The server answered with an error (${error.detail}).`;
      if (error.code === "bad-phrase") return error.detail;
      return MESSAGES[error.code] || error.message;
    }
    if (error?.name === "QuotaExceededError") return MESSAGES.quota;
    if (error?.name === "NoModificationAllowedError" || error?.name === "InvalidStateError") return MESSAGES["in-use"];
    return `Something went wrong: ${error?.message || error}`;
  }

  // ---- Small helpers ----
  const enc = new TextEncoder();
  function concat(parts) {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) { out.set(p, at); at += p.length; }
    return out;
  }
  const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
  function bytes(n) {
    if (n < 1024) return `${n} bytes`;
    if (n < 1024 * 1024) return `${Math.max(1, Math.round(n / 1024))} KB`;
    return `${(n / 1048576).toFixed(1)} MB`;
  }
  function when(time) {
    const d = new Date(time);
    if (Number.isNaN(d.getTime())) return "an unknown time";
    const clock = d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
    if (d.toDateString() === new Date().toDateString()) return `today at ${clock}`;
    return `${d.toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" })}, ${clock}`;
  }
  const byPath = (a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  const esc = (text) => String(text).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

  // ---- ZIP (stored entries written; stored and deflated read) ----
  const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(data) {
    let c = 0xffffffff;
    for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
    return ~c >>> 0;
  }
  function dosTime(ms) {
    const d = new Date(ms || Date.now());
    const year = Math.min(2107, Math.max(1980, d.getFullYear()));
    return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
             date: ((year - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
  }
  function fromDos(time, date) {
    return new Date(1980 + (date >> 9), ((date >> 5) & 15) - 1, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2).getTime();
  }

  // files: [{path, data, mtime, dir}] -> the ZIP's bytes. Names are UTF-8 (flag bit 11).
  function zipWrite(files, comment) {
    const parts = [], central = [];
    let offset = 0;
    for (const f of files) {
      const name = enc.encode(f.dir ? `${f.path}/` : f.path), data = f.dir ? new Uint8Array(0) : f.data;
      const crc = crc32(data), { time, date } = dosTime(f.mtime);
      const local = new DataView(new ArrayBuffer(30));
      local.setUint32(0, 0x04034b50, true);
      local.setUint16(4, 20, true);
      local.setUint16(6, 0x0800, true);
      local.setUint16(10, time, true);
      local.setUint16(12, date, true);
      local.setUint32(14, crc, true);
      local.setUint32(18, data.length, true);
      local.setUint32(22, data.length, true);
      local.setUint16(26, name.length, true);
      parts.push(new Uint8Array(local.buffer), name, data);
      const c = new DataView(new ArrayBuffer(46));
      c.setUint32(0, 0x02014b50, true);
      c.setUint16(4, 20, true);
      c.setUint16(6, 20, true);
      c.setUint16(8, 0x0800, true);
      c.setUint16(12, time, true);
      c.setUint16(14, date, true);
      c.setUint32(16, crc, true);
      c.setUint32(20, data.length, true);
      c.setUint32(24, data.length, true);
      c.setUint16(28, name.length, true);
      c.setUint32(38, f.dir ? 0x10 : 0, true);
      c.setUint32(42, offset, true);
      central.push(new Uint8Array(c.buffer), name);
      offset += 30 + name.length + data.length;
    }
    const note = enc.encode(comment || "");
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true);
    end.setUint16(8, files.length, true);
    end.setUint16(10, files.length, true);
    end.setUint32(12, central.reduce((n, a) => n + a.length, 0), true);
    end.setUint32(16, offset, true);
    end.setUint16(20, note.length, true);
    return concat([...parts, ...central, new Uint8Array(end.buffer), note]);
  }

  // ---- PC game saves ----
  // A career is <name>/<name>, one file of a fixed size. The layout and the checks below are what
  // version 1.4 reads and writes: the header is built at 00894320 and checked at 008955c4 and
  // 008957e9, and the checksum is 0089747b. A save carries no version number, so "1.4" means
  // "exactly this layout": anything else is refused before it can reach the game.
  // SAVE-CHECK-START
  const CAREER = { size: 0x4602c, head: 0x1c, info: 0x10, data: 0x46000, magic: 0x4d433032, layout: 6, game: 0x656d6147 };
  const CAREER_TABLE = Array.from({ length: 256 }, (_, i) => {
    let c = i << 24;
    for (let k = 0; k < 8; k++) c = (c & 0x80000000) ? (c << 1) ^ 0x04c11db7 : c << 1;
    return c >>> 0;
  });
  function careerCrc(u8, start, end) {
    let a = ~((u8[start] << 24) | (u8[start + 1] << 16) | (u8[start + 2] << 8) | u8[start + 3]);
    for (let i = start + 4; i < end; i++) a = ((a << 8) | u8[i]) ^ CAREER_TABLE[a >>> 24];
    return ~a >>> 0;
  }
  // The game signs each save (0057ff60, 006b7120): the data after the first 0x10 bytes goes through
  // MD5, that digest through MD5 three more times, and the four digests are one 512-bit
  // little-endian number raised to e mod n. The low 16 bytes open the data section.
  const CAREER_KEY = {
    n: 0xef620ebb3403d099478fe08aaa365e57fc98e72983fe82cf691328bc47c81de807f34ea0a0a20e969b8ddf94c6dec0b987284be4d4406b7e4a50abe5860b960dn,
    e: 0x2829285589e575f2b149d71f0c6d70a88c4d4ec2ed4d1ebf135ece7fbfff37dce999a2ae905ce2a0aa219daa065152b845208a6e676f4f8bd129d25360bdd0e1n,
  };
  const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
  const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  function md5(bytes) {
    const blocks = Math.floor((bytes.length + 8) / 64) + 1, m = new Uint8Array(blocks * 64);
    m.set(bytes);
    m[bytes.length] = 0x80;
    const v = new DataView(m.buffer), h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    v.setUint32(m.length - 8, bytes.length * 8, true);
    for (let at = 0; at < m.length; at += 64) {
      let [a, b, c, d] = h;
      for (let i = 0; i < 64; i++) {
        const round = i >> 4;
        const f = round === 0 ? (b & c) | (~b & d) : round === 1 ? (d & b) | (~d & c) : round === 2 ? b ^ c ^ d : c ^ (b | ~d);
        const g = round === 0 ? i : round === 1 ? 5 * i + 1 : round === 2 ? 3 * i + 5 : 7 * i;
        const sum = (a + f + MD5_K[i] + v.getUint32(at + (g & 15) * 4, true)) | 0, s = MD5_S[round * 4 + (i & 3)];
        [a, d, c, b] = [d, c, b, (b + ((sum << s) | (sum >>> (32 - s)))) | 0];
      }
      h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
    }
    const out = new Uint8Array(16), o = new DataView(out.buffer);
    h.forEach((x, i) => o.setUint32(i * 4, x, true));
    return out;
  }
  function careerSignature(u8) {
    const body = CAREER.head + CAREER.info;
    let digest = md5(u8.subarray(body + 0x10, body + CAREER.data)), value = 0n;
    const chain = new Uint8Array(64);
    for (let i = 0; i < 4; i++) { chain.set(digest, i * 16); digest = md5(digest); }
    for (let i = 63; i >= 0; i--) value = (value << 8n) | BigInt(chain[i]);
    let power = 1n, base = value % CAREER_KEY.n;
    for (let e = CAREER_KEY.e; e; e >>= 1n) {
      if (e & 1n) power = (power * base) % CAREER_KEY.n;
      base = (base * base) % CAREER_KEY.n;
    }
    return Uint8Array.from({ length: 16 }, (_, i) => Number((power >> BigInt(i * 8)) & 0xffn));
  }
  // null when the file is a sound 1.4 career, else "not-14" or "corrupt".
  function checkCareer(u8) {
    if (u8.length !== CAREER.size) return "not-14";
    const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), at = (o) => v.getUint32(o, true);
    if (at(0) !== CAREER.magic || at(4) !== CAREER.size || at(8) !== CAREER.info || at(12) !== CAREER.data) return "not-14";
    const body = CAREER.head + CAREER.info;
    if (careerCrc(u8, 0, 0x18) !== at(0x18)) return "corrupt";
    if (careerCrc(u8, CAREER.head, body) !== at(0x10) || careerCrc(u8, body, CAREER.size) !== at(0x14)) return "corrupt";
    if (at(CAREER.head + 8) !== CAREER.layout || at(body + 0x50) !== CAREER.game) return "not-14";
    if (at(CAREER.head) + at(CAREER.head + 4) > CAREER.data) return "not-14";
    if (careerSignature(u8).some((b, i) => b !== u8[body + i])) return "corrupt";
    return null;
  }
  // A save also holds the serial number of the copy that wrote it, and the game refuses one that
  // differs from its own (007167b0). Here it has none, so an imported career's is cleared, then
  // the signature and the checksums that cover it are made again.
  function careerForThisGame(u8) {
    const body = CAREER.head + CAREER.info, serial = [body + 0x10, body + 0x30];
    if (u8.subarray(...serial).every((b) => !b)) return u8;
    const out = u8.slice(), v = new DataView(out.buffer);
    out.fill(0, ...serial);
    out.set(careerSignature(out), body);
    v.setUint32(0x14, careerCrc(out, body, CAREER.size), true);
    v.setUint32(0x18, careerCrc(out, 0, 0x18), true);
    return out;
  }
  // Careers among {path, data}: the file <name>/<name>, wherever that folder sits. A file on its
  // own is named by itself.
  function findCareers(files) {
    const found = new Map();
    for (const f of files) {
      if (f.dir) continue;
      const parts = f.path.replaceAll("\\", "/").split("/").filter(Boolean), name = parts.at(-1);
      if (parts.length > 1 && parts.at(-2).toLowerCase() !== name.toLowerCase()) continue;
      if (!name || name.length > 32 || !/^[\p{L}\p{N} _.-]+$/u.test(name) || /^[. ]|[. ]$/.test(name)) continue;
      found.set(name.toLowerCase(), { name, data: f.data, problem: checkCareer(f.data) });
    }
    return [...found.values()];
  }
  // SAVE-CHECK-END

  async function inflate(data) {
    const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  // A ZIP's bytes -> {files, comment, info}. Unsafe paths make the whole file
  // fail; junk and caches (SKIP) are left out.
  async function zipRead(u8, { anyFiles = false } = {}) {
    const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), dec = new TextDecoder();
    let eocd = -1;
    for (let i = u8.length - 22; i >= Math.max(0, u8.length - 22 - 65535); i--)
      if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
    if (eocd < 0) throw new SaveError("not-zip");
    const count = v.getUint16(eocd + 10, true), size = v.getUint32(eocd + 12, true);
    const start = v.getUint32(eocd + 16, true), noteLength = v.getUint16(eocd + 20, true);
    if (start + size > eocd) throw new SaveError("damaged");
    const comment = dec.decode(u8.subarray(eocd + 22, eocd + 22 + noteLength));
    const files = [];
    let p = start, total = 0;
    for (let i = 0; i < count; i++) {
      if (p + 46 > eocd || v.getUint32(p, true) !== 0x02014b50) throw new SaveError("damaged");
      const flags = v.getUint16(p + 8, true), method = v.getUint16(p + 10, true);
      const time = v.getUint16(p + 12, true), date = v.getUint16(p + 14, true), crc = v.getUint32(p + 16, true);
      const csize = v.getUint32(p + 20, true), usize = v.getUint32(p + 24, true);
      const nl = v.getUint16(p + 28, true), xl = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true);
      const local = v.getUint32(p + 42, true);
      const name = dec.decode(u8.subarray(p + 46, p + 46 + nl)).replaceAll("\\", "/");
      p += 46 + nl + xl + cl;
      const dir = name.endsWith("/"), path = name.replace(/\/+$/, "");
      const parts = path.split("/");
      if (!path || parts.some((s) => !s || s === "." || s === ".." || /[\0-\x1f:]/.test(s))) throw new SaveError("not-backup");
      if (skipped(path)) continue;
      if (flags & 1) throw new SaveError("not-backup"); // a password-protected ZIP
      if (local + 30 > u8.length || v.getUint32(local, true) !== 0x04034b50) throw new SaveError("damaged");
      const at = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
      if (at + csize > u8.length) throw new SaveError("damaged");
      if (dir) { files.push({ path, dir: true, data: new Uint8Array(0), mtime: fromDos(time, date) }); continue; }
      const raw = u8.subarray(at, at + csize);
      let data;
      if (method === 0) data = raw.slice();
      else if (method === 8) { try { data = await inflate(raw); } catch { throw new SaveError("damaged"); } }
      else throw new SaveError("not-backup");
      if (data.length !== usize || crc32(data) !== crc) throw new SaveError("damaged");
      if ((total += data.length) > 64 * 1024 * 1024) throw new SaveError("not-backup");
      files.push({ path, data, mtime: fromDos(time, date) });
    }
    let info = {};
    try { info = JSON.parse(comment) || {}; } catch {}
    const result = { files: tidy(files), info };
    if (anyFiles) return result;
    if (info.backup !== "nfsc-save" && !result.files.some((f) =>
        /^(registry\.json|mod-settings\.json)$/i.test(f.path) || /^documents\//i.test(f.path)))
      throw new SaveError("not-backup");
    return result;
  }

  // Sorted; a folder entry only for a folder with nothing else in it.
  function tidy(files) {
    const seen = new Map();
    for (const f of files) seen.set(f.path, f);
    const list = [...seen.values()];
    return list.filter((f) => !f.dir || !list.some((g) => g.path.startsWith(`${f.path}/`))).sort(byPath);
  }

  // A fingerprint of the save's contents (not its times), to tell whether it changed.
  async function fingerprint(files) {
    const parts = [];
    for (const f of files) parts.push(enc.encode(`${f.dir ? "d" : "f"} ${f.path} ${f.data.length}\n`), f.data);
    return hex(await crypto.subtle.digest("SHA-256", concat(parts)));
  }

  // What a list of files holds, in words: careers and size.
  function describe(files) {
    const careers = new Set();
    let size = 0, count = 0;
    for (const f of files) {
      const m = /^documents\/nfs carbon\/([^/]+)\//i.exec(f.path);
      if (m) careers.add(m[1]);
      if (!f.dir) { size += f.data.length; count++; }
    }
    const names = [...careers].map((n) => `“${n}”`).join(", ");
    const what = careers.size ? `${careers.size === 1 ? "career" : "careers"} ${names}` : "settings only, no career yet";
    return { what, size, count, text: `${what} (${count} ${count === 1 ? "file" : "files"}, ${bytes(size)})` };
  }

  // ---- Crypto ----
  async function pbkdf2(phrase) {
    return crypto.subtle.importKey("raw", enc.encode(phrase), "PBKDF2", false, ["deriveBits", "deriveKey"]);
  }
  async function cloudId(phrase) {
    const bits = await crypto.subtle.deriveBits({ name: "PBKDF2", hash: "SHA-256", salt: enc.encode(ID_SALT), iterations: ITERATIONS },
                                                await pbkdf2(phrase), 256);
    return hex(bits);
  }
  async function aesKey(phrase, salt, iterations) {
    return crypto.subtle.deriveKey({ name: "PBKDF2", hash: "SHA-256", salt, iterations }, await pbkdf2(phrase),
                                   { name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
  }
  async function seal(phrase, plain) {
    const head = new Uint8Array(42), salt = crypto.getRandomValues(new Uint8Array(16)), iv = crypto.getRandomValues(new Uint8Array(12));
    head.set(enc.encode(MAGIC), 0);
    head[8] = 1; // format version
    head[9] = 1; // PBKDF2-HMAC-SHA-256
    new DataView(head.buffer).setUint32(10, ITERATIONS);
    head.set(salt, 14);
    head.set(iv, 30);
    const key = await aesKey(phrase, salt, ITERATIONS);
    const body = await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: head }, key, plain);
    return concat([head, new Uint8Array(body)]);
  }
  async function unseal(phrase, blob) {
    if (blob.length < 42 + 16 || new TextDecoder().decode(blob.subarray(0, 8)) !== MAGIC || blob[8] !== 1 || blob[9] !== 1)
      throw new SaveError("not-sealed");
    const iterations = new DataView(blob.buffer, blob.byteOffset).getUint32(10);
    if (iterations < 100000 || iterations > 10000000) throw new SaveError("not-sealed");
    const head = blob.slice(0, 42), key = await aesKey(phrase, head.slice(14, 30), iterations);
    try {
      return new Uint8Array(await crypto.subtle.decrypt({ name: "AES-GCM", iv: head.slice(30, 42), additionalData: head }, key, blob.subarray(42)));
    } catch {
      throw new SaveError("wrong-phrase");
    }
  }

  // ---- Keyphrases ----
  function newPhrase() {
    return PREFIX + [...crypto.getRandomValues(new Uint8Array(8))].map((b) => WORDS[b]).join("-");
  }
  function parsePhrase(text) {
    const rest = String(text || "").toLowerCase().trim().replace(/^carbon(?![a-z])/, "");
    const words = rest.split(/[^a-z]+/).filter(Boolean);
    for (const w of words) if (!WORD_SET.has(w)) {
      const near = WORDS.find((x) => x.slice(0, 4) === w.slice(0, 4));
      throw new SaveError("bad-phrase", `“${w}” isn't one of the keyphrase words${near ? ` (“${near}”?)` : ""}. Check the spelling.`);
    }
    if (words.length !== 8)
      throw new SaveError("bad-phrase", `A keyphrase has 8 words after “carbon”; this has ${words.length}.`);
    return PREFIX + words.join("-");
  }

  // ---- Start ----
  function whenPage(run) {
    if (window.recompStartPage) run(window.recompStartPage);
    else addEventListener("recomp-start-page", () => run(window.recompStartPage), { once: true });
  }
  whenPage((page) => {
    const host = page?.panel?.("saves");
    if (!host) return;
    try { build(page, host); } catch (error) { console.warn("[saves] the Saves panel could not start:", error); }
  });

  function build(page, host) {
    const { slot, store, profileFolder: folder } = page;
    const cloudKey = `cloud.${slot}`, syncKey = `cloudSync.${slot}`;
    const undoName = `saves-undo-${folder}.zip`;
    const view = document.createElement("section");
    view.className = "cs-panel sv";
    view.innerHTML = `
      <style>
        .cs-ext[data-for="saves"] { display: contents; }
        .sv .sv-line { font-size: 14px; margin-bottom: 12px; }
        .sv .sv-line b { color: var(--cyan); font-weight: 600; }
        .sv .sv-sub { display: flex; align-items: center; gap: 10px; margin: 22px 0 12px; color: var(--dim);
              font: italic 800 12px/1 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
              letter-spacing: .2em; text-transform: uppercase; }
        .sv .sv-sub::after { content: ""; flex: 1; height: 1px; background: var(--line); }
        .sv .sv-gap { margin-top: 10px; }
        .sv .sv-link { background: none; border: 0; padding: 0; margin-top: 10px; font: inherit; font-size: 13px; color: var(--cyan);
              text-decoration: underline; text-underline-offset: 3px; cursor: pointer; text-align: left; }
        .sv .sv-link:disabled { opacity: .4; cursor: default; }
        .sv .sv-link:focus-visible { outline: 2px solid var(--cyan); outline-offset: 2px; }
        .sv .sv-msg { margin-top: 14px; font-size: 13px; color: var(--ink); }
        .sv .sv-msg:empty { display: none; }
        .sv .sv-msg.good { color: var(--cyan); }
        .sv .sv-msg.bad { color: #ff8da1; }
        .sv .sv-msg.busy::before { content: ""; display: inline-block; width: 8px; height: 8px; margin-right: 8px; border-radius: 50%;
              background: var(--cyan); box-shadow: 0 0 8px var(--cyan); animation: sv-pulse 1s ease-in-out infinite alternate; }
        @keyframes sv-pulse { from { opacity: .25; } to { opacity: 1; } }
        .sv .sv-notice { margin: 0 0 14px; padding: 12px 14px; border-left: 3px solid var(--cyan); background: rgba(95, 228, 242, .07); font-size: 14px; }
        .sv .sv-notice .cs-actions { margin-top: 10px; }
        .sv .sv-phrase { position: relative; margin: 4px 0 12px; padding: 14px 16px 16px; background: rgba(0, 0, 0, .5);
              border: 1px solid rgba(95, 228, 242, .6); box-shadow: inset 0 0 26px rgba(95, 228, 242, .08);
              clip-path: polygon(0 0, calc(100% - 12px) 0, 100% 12px, 100% 100%, 12px 100%, 0 calc(100% - 12px)); }
        .sv .sv-phrase::before, .sv .sv-phrase::after { content: ""; position: absolute; width: 18px; height: 1px;
              background: rgba(95, 228, 242, .6); transform-origin: left center; transform: rotate(45deg); }
        .sv .sv-phrase::before { left: calc(100% - 13px); top: -1px; }
        .sv .sv-phrase::after { left: -1px; top: calc(100% - 13px); }
        .sv .sv-pre { font-size: 14px; font-weight: 600; letter-spacing: .14em; text-transform: uppercase; color: var(--cyan); margin-bottom: 4px; }
        .sv .sv-why { margin: 0 0 14px; font-size: 14px; line-height: 1.4; color: #e8eef2; }
        .sv .sv-full { margin-top: 14px; font: 13px/1.4 ui-monospace, "SF Mono", Menlo, monospace; color: #cfd8de;
              overflow-wrap: anywhere; user-select: all; }
        .sv .sv-words { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px 14px; margin: 0; padding: 0;
              list-style: none; counter-reset: sv-word; }
        .sv .sv-words li { font: italic 700 22px/1.15 "Avenir Next Condensed", "DIN Condensed", "Bahnschrift", sans-serif;
              letter-spacing: .03em; white-space: nowrap; color: #fff; }
        .sv .sv-words li::before { counter-increment: sv-word; content: counter(sv-word); margin-right: 6px; vertical-align: 2px;
              font: 600 10px/1 system-ui, sans-serif; color: var(--cyan); }
        .sv .sv-phrase.sv-unsaved { border-color: #ffd27a; box-shadow: inset 0 0 26px rgba(255, 210, 122, .1); }
        .sv .sv-unsaved .sv-pre, .sv .sv-unsaved .sv-why { color: #ffd27a; }
        .sv .sv-phrase .cs-actions { margin-top: 14px; }
        .sv .sv-enter { display: grid; gap: 10px; margin-top: 12px; }
        .sv .sv-enter input { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 13px; }
        .sv .sv-btn-sm { font-size: 13px; padding: 9px 14px; }
        .sv-dialog .sv-list { margin: 0 0 16px; color: var(--dim); font-size: 13px; }
        .sv .sv-remove { display: block; color: var(--dim); }
        .sv .sv-remove:hover:not(:disabled) { color: #ff8da1; }
        .sv-dialog .sv-check { margin: 0; font-size: 14px; }
        @media (max-width: 520px) { .sv .sv-words { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
        @media (prefers-reduced-motion: reduce) { .sv .sv-msg.busy::before { animation: none; } }
      </style>
      <h2>Saves</h2>
      <div class="sv-line sv-local">Looking at the save in this browser…</div>
      <div class="cs-actions">
        <button class="cs-btn sv-down" type="button"><span>Download backup</span></button>
        <button class="cs-btn sv-up" type="button"><span>Restore from file</span></button>
      </div>
      <div class="cs-note sv-gap">Back up your progress and game settings.</div>
      <div class="sv-sub">From the PC game</div>
      <div class="cs-note">Keep playing your career from the PC version 1.4. On Windows, it's in Documents, in the NFS Carbon folder.</div>
      <div class="cs-actions">
        <button class="cs-btn sv-import-dir" type="button"><span>Choose the NFS Carbon folder</span></button>
        <button class="cs-btn sv-import-file" type="button"><span>Choose a career file or zip</span></button>
      </div>
      <input type="file" class="sv-pick-dir" webkitdirectory multiple hidden>
      <input type="file" class="sv-pick-file" hidden>
      <button class="sv-link sv-undo" type="button" hidden></button>
      <div class="sv-sub">Cloud</div>
      <div class="sv-cloud"></div>
      <div class="sv-msg" role="status" aria-live="polite"></div>
      <input type="file" class="sv-file" accept=".zip,application/zip" hidden>`;
    host.append(view);
    const $ = (sel) => view.querySelector(sel);
    const root = host.closest(".cs") || document.body;
    const msg = $(".sv-msg");

    const state = { files: [], fp: "", exists: false, undo: null, notice: null, entering: false, synced: false };
    let busy = false, blockPlay = false, dialogOpen = null;
    let cloud = store.get(cloudKey, null);
    try { cloud = cloud && /^[0-9a-f]{64}$/.test(cloud.id || "") ? { ...cloud, phrase: parsePhrase(cloud.phrase) } : null; }
    catch { cloud = null; }
    // start.js uses only the opaque cloud-save id as a bearer proof for an explicit name
    // claim. The keyphrase itself stays inside this module and is never sent to /names.
    window.recompCloudIdentity = Object.freeze({
      token: () => cloud?.id || null,
      show: () => host.scrollIntoView({ behavior: "smooth", block: "start" }),
    });
    const identityChanged = () => dispatchEvent(new CustomEvent("recomp-cloud-identity", { detail: { linked: !!cloud } }));
    identityChanged();

    function say(text, kind = "") {
      msg.textContent = text || "";
      msg.className = `sv-msg ${kind}`;
    }

    // ---- The folder ----
    async function gameDir(create) {
      const opfs = await navigator.storage.getDirectory();
      return opfs.getDirectoryHandle(GAME, { create });
    }
    async function folderDir(create) {
      try { return await (await gameDir(create)).getDirectoryHandle(folder, { create }); }
      catch (error) { if (!create && error?.name === "NotFoundError") return null; throw error; }
    }
    async function readFolder() {
      const dir = await folderDir(false);
      if (!dir) return { exists: false, files: [] };
      const files = [];
      async function walk(d, prefix) {
        let any = false;
        for await (const [name, h] of d.entries()) {
          const path = prefix + name;
          if (skipped(path)) continue;
          any = true;
          if (h.kind === "directory") {
            if (!(await walk(h, `${path}/`))) files.push({ path, dir: true, data: new Uint8Array(0), mtime: Date.now() });
          } else {
            const f = await h.getFile();
            files.push({ path, data: new Uint8Array(await f.arrayBuffer()), mtime: f.lastModified });
          }
        }
        return any;
      }
      await walk(dir, "");
      return { exists: true, files: tidy(files) };
    }
    // Everything a backup would hold goes; what it leaves out (caches, the copy marker) stays.
    async function clear(dir, prefix) {
      const entries = [];
      for await (const entry of dir.entries()) entries.push(entry);
      let empty = true;
      for (const [name, h] of entries) {
        const path = prefix + name;
        if (skipped(path)) { empty = false; continue; }
        if (h.kind === "directory") {
          if (await clear(h, `${path}/`)) await dir.removeEntry(name, { recursive: true });
          else empty = false;
        } else {
          await dir.removeEntry(name);
        }
      }
      return empty;
    }
    async function writeFiles(files) {
      const dir = await folderDir(true);
      await clear(dir, "");
      for (const f of files) {
        const parts = f.path.split("/");
        let d = dir;
        for (let i = 0; i < parts.length - (f.dir ? 0 : 1); i++) d = await d.getDirectoryHandle(parts[i], { create: true });
        if (f.dir) continue;
        const out = await (await d.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
        await out.write(f.data);
        await out.close();
      }
      // The player page copies the main save into a window's folder unless this marker is there.
      if (slot > 1) await dir.getFileHandle(".copied", { create: true });
    }
    function backupZip(files, kind = "backup") {
      return zipWrite(files, JSON.stringify({ backup: "nfsc-save", version: 1, made: new Date().toISOString(), folder, kind }));
    }
    // Replaces the folder with `files`. The save it replaces goes to the undo
    // file first, and back into the folder if writing the new one fails.
    async function restore(files, { undoing = false } = {}) {
      await page.beforeProfileRestore?.();
      try {
        const current = (await readFolder()).files;
        const nfsc = await gameDir(true);
        if (!undoing && current.length) {
          const out = await (await nfsc.getFileHandle(undoName, { create: true })).createWritable();
          await out.write(backupZip(current, "undo"));
          await out.close();
        }
        try {
          await writeFiles(files);
        } catch (error) {
          if (current.length) await writeFiles(current).catch(() => {});
          throw error;
        }
        if (undoing) await nfsc.removeEntry(undoName).catch(() => {});
      } finally {
        await page.afterProfileRestore?.();
      }
    }
    async function readUndo() {
      try {
        const file = await (await (await gameDir(false)).getFileHandle(undoName)).getFile();
        return { at: file.lastModified, file };
      } catch { return null; }
    }

    // ---- The server ----
    function server() {
      let value = null, key = "", label = "";
      try {
        const raw = localStorage.getItem(`${GAME}.savesServer`);
        if (raw) { try { value = JSON.parse(raw); } catch { value = raw; } label = "Test"; }
      } catch {}
      if (typeof value !== "string" || !value) {
        const s = page.server?.();
        if (!s?.url) return null;
        value = s.url; key = s.key || ""; label = s.label || "";
      }
      let url;
      try { url = new URL(value.replace(/^ws(s?):/i, "http$1:")); } catch { return null; }
      key = url.searchParams.get("key") || key;
      return { base: `${url.protocol}//${url.host}${url.pathname.replace(/\/+$/, "")}`, key, label };
    }
    async function request(method, id, { body, headers = {}, timeout = 120000 } = {}) {
      const s = server();
      if (!s) throw new SaveError("no-server");
      const url = `${s.base}/saves/${id}${s.key ? `?key=${encodeURIComponent(s.key)}` : ""}`;
      let r;
      try {
        r = await fetch(url, { method, body, headers, cache: "no-store", credentials: "omit", signal: AbortSignal.timeout(timeout) });
      } catch {
        throw new SaveError("unreachable");
      }
      return r;
    }
    function failure(r) {
      const code = { 403: "forbidden", 404: "not-found", 409: "busy-server", 412: "changed", 413: "too-big", 429: "too-many",
                     501: "off", 507: "full" }[r.status];
      return new SaveError(code || "status", code ? undefined : String(r.status));
    }
    const syncs = () => store.get(syncKey, {}) || {};
    // The same server under its earlier name (moved 2026-10-25, tools/carbon-relay-deploy.sh): its
    // record still counts, so a save in step there is not taken for a conflict here.
    const EARLIER = { "https://server.nfscrb.xyz": ["https://drive.metinjakupi.dev"] };
    function synced() {
      const s = server();
      if (!s) return null;
      const all = syncs();
      return all[s.base] || (EARLIER[s.base] || []).map((b) => all[b]).find(Boolean) || null;
    }
    function remember(record) {
      const s = server();
      if (!s) return;
      const all = syncs();
      if (record) all[s.base] = record; else delete all[s.base];
      store.set(syncKey, all);
    }

    // ---- What the panel shows ----
    async function look() {
      const { exists, files } = await readFolder();
      state.exists = exists;
      state.files = files;
      state.fp = await fingerprint(files);
      state.undo = await readUndo();
      showLocal();
      showCloud();
    }
    function showLocal() {
      const line = $(".sv-local"), own = slot > 1 ? "This window's save" : "Save in this browser";
      line.replaceChildren();
      if (!state.files.length || describe(state.files).what === "settings only, no career yet") {
        line.textContent = slot > 1 && !state.exists
          ? "This window has no save of its own yet: it gets a copy of the main one when you press Play."
          : "No career saved yet. Start one in the game.";
      } else {
        const d = describe(state.files), b = document.createElement("b");
        b.textContent = d.what;
        const newest = Math.max(...state.files.filter((f) => !f.dir).map((f) => f.mtime));
        line.append(b, ` · saved ${when(newest)}`);
      }
      const undo = $(".sv-undo");
      undo.hidden = !state.undo;
      if (state.undo) undo.textContent = `Undo the last restore: put back the save it replaced (kept ${when(state.undo.at)})`;
    }
    const button = (cls, text, extra = "") => `<button class="cs-btn ${extra} ${cls}" type="button"><span>${text}</span></button>`;
    function wordsHtml(phrase) {
      return `<ol class="sv-words">${phrase.slice(PREFIX.length).split("-").map((w) => `<li>${w}</li>`).join("")}</ol>
        <div class="sv-full">${phrase}</div>`;
    }
    function showCloud() {
      const box = $(".sv-cloud");
      const s = server();
      if (!cloud) {
        box.innerHTML = `
          <div class="cs-note" style="margin:0 0 12px">Keep a backup online to continue on another device. Your keyphrase unlocks it.</div>
          <div class="cs-actions">${button("sv-on", "Turn on cloud saves")}${button("sv-have", "I have a keyphrase")}</div>
          <form class="sv-enter" ${state.entering ? "" : "hidden"}>
            <label class="cs-row" style="margin:0"><span class="cs-label">Your keyphrase</span>
              <input type="text" class="sv-phrase-in" autocomplete="off" autocapitalize="none" spellcheck="false"
                     placeholder="carbon-… (8 words)"></label>
            <div class="cs-actions">${button("sv-load-typed", "Load from cloud")}</div>
          </form>`;
        $(".sv-on").onclick = () => run("Making your keyphrase and sealing your save…", turnOn);
        $(".sv-have").onclick = () => { state.entering = !state.entering; showCloud(); if (state.entering) $(".sv-phrase-in").focus(); };
        $(".sv-enter").onsubmit = (e) => { e.preventDefault(); loadTyped(); };
        $(".sv-load-typed").onclick = loadTyped;
        lock();
        return;
      }
      const rec = synced(), changed = !rec || rec.fp !== state.fp;
      let status;
      if (!s) status = MESSAGES["no-server"];
      else if (state.notice) status = rec?.at ? `Last in step with the cloud ${when(rec.at)}.` : "";
      else if (rec?.removed) status = `Removed from the cloud ${when(rec.removed)}. Nothing goes up by itself now: Save to cloud puts your save back.`;
      else if (!rec) status = state.files.length ? "Not on this server's cloud yet." : "Nothing saved to the cloud yet.";
      else if (changed && state.files.length) status = `Saved to the cloud ${when(rec.at)}. Changed here since: it goes up the next time this page opens, or now with Save to cloud.`;
      else status = `Saved to the cloud ${when(rec.at)} · up to date.`;
      let notice = "";
      if (state.notice?.kind === "newer")
        notice = `<div class="sv-notice">The cloud has a newer save, from ${when(state.notice.at)} (another computer).
          <div class="cs-actions">${button("sv-take", "Load it", "sv-btn-sm")}</div></div>`;
      else if (state.notice?.kind === "conflict")
        notice = `<div class="sv-notice">This browser's save and the cloud's (from ${when(state.notice.at)}) are different. Which one stays?
          <div class="cs-actions">${button("sv-mine", "Keep this browser's", "sv-btn-sm")}${button("sv-take", "Use the cloud's", "sv-btn-sm")}</div></div>`;
      const reveal = !cloud.ack;
      box.innerHTML = `${notice}
        <div class="sv-phrase${reveal ? " sv-unsaved" : ""}">
          <div class="sv-pre">Your keyphrase</div>
          <p class="sv-why">${reveal ? "Write it down now." : "Keep it safe and private."} It is the only way to open your cloud save on another device.
            There is no account and no reset.</p>
          ${wordsHtml(cloud.phrase)}
          <div class="cs-actions">${button("sv-copy", "Copy", "sv-btn-sm")}${button("sv-text", "Save as text file", "sv-btn-sm")}${reveal ? button("sv-ack", "I wrote it down", "sv-btn-sm") : ""}</div>
          <div class="cs-note sv-gap">On another device: open Saves, then Cloud save, then I have a keyphrase.</div>
        </div>
        <div class="sv-line sv-cloud-state"></div>
        <div class="cs-actions">${button("sv-push", "Save to cloud")}${button("sv-pull", "Load from cloud")}</div>
        <div class="cs-note sv-gap">${s ? `Kept on ${esc(s.label ? `“${s.label}”` : s.base)}, the server chosen under Online.` : ""}</div>
        ${s && !(rec?.removed && !state.notice) ? `<button class="sv-link sv-remove" type="button">Remove from cloud</button>` : ""}
        ${reveal ? "" : `<button class="sv-link sv-off" type="button">Stop cloud saves on this computer</button>`}`;
      $(".sv-cloud-state").textContent = status;
      $(".sv-push").onclick = () => run("Sealing your save and sending it to the cloud…", () => upload(rec?.etag ? "match" : "new"));
      $(".sv-pull").onclick = () => run("Fetching the cloud save…", () => download(cloud.phrase, cloud.id), { blocksPlay: true });
      for (const el of box.querySelectorAll(".sv-copy")) el.onclick = () => copy(el);
      for (const el of box.querySelectorAll(".sv-text")) el.onclick = savePhraseFile;
      const remove = $(".sv-remove");
      if (remove) remove.onclick = () => run("Checking the cloud…", removeCloud);
      if (reveal) $(".sv-ack").onclick = () => { cloud.ack = true; store.set(cloudKey, cloud); showCloud(); };
      else $(".sv-off").onclick = turnOff;
      const take = $(".sv-take"), mine = $(".sv-mine");
      if (take) take.onclick = () => run("Fetching the cloud save…", () => download(cloud.phrase, cloud.id), { blocksPlay: true });
      if (mine) mine.onclick = () => run("Sealing your save and sending it to the cloud…", () => upload("force"));
      lock();
    }
    function lock() {
      for (const el of view.querySelectorAll("button, input")) el.disabled = busy;
    }

    // One task at a time; the panel says what is going on and how it ended.
    async function run(label, task, { blocksPlay = false } = {}) {
      if (busy) return;
      busy = true;
      blockPlay = blocksPlay;
      lock();
      say(label, "busy");
      try {
        const done = await task();
        say(done || "", done ? "good" : "");
      } catch (error) {
        if (!(error instanceof SaveError)) console.warn("[saves]", error);
        say(messageFor(error), "bad");
      } finally {
        busy = false;
        blockPlay = false;
        try { await look(); } catch (error) { console.warn("[saves]", error); lock(); }
      }
    }

    // ---- Dialogs, in the page (not the browser's own) ----
    // check: an optional {label, checked} switch in the dialog; its .checked is
    // updated when the dialog closes. A button with focus: true is focused first.
    function ask(title, lines, buttons, check) {
      return new Promise((resolve) => {
        const layer = document.createElement("div");
        layer.className = "cs-dialog sv-dialog";
        const form = document.createElement("form");
        form.className = "cs-panel";
        const h = document.createElement("h2");
        h.textContent = title;
        form.append(h);
        for (const line of lines) {
          const p = document.createElement("p");
          p.textContent = line;
          form.append(p);
        }
        let tick = null;
        if (check) {
          const label = document.createElement("label"), span = document.createElement("span");
          label.className = "cs-switch sv-check";
          span.textContent = check.label;
          tick = document.createElement("input");
          tick.type = "checkbox";
          tick.checked = !!check.checked;
          label.append(span, tick);
          form.append(label);
        }
        const row = document.createElement("div");
        row.className = "cs-actions";
        for (const b of buttons) {
          const el = document.createElement("button");
          el.type = "button";
          el.className = `cs-btn${b.primary ? " big" : ""}`;
          if (b.primary) el.style.fontSize = "18px";
          el.innerHTML = "<span></span>";
          el.firstChild.textContent = b.label;
          el.onclick = () => close(b.value);
          if (b.focus) el.dataset.focus = "";
          row.append(el);
        }
        form.append(row);
        layer.append(form);
        layer.addEventListener("click", (e) => { if (e.target === layer) close(null); });
        root.append(layer);
        dialogOpen = { layer, close };
        (row.querySelector("[data-focus]") || row.querySelector(".big") || row.firstChild).focus();
        function close(value) {
          if (check) check.checked = tick.checked;
          layer.remove();
          dialogOpen = null;
          resolve(value);
        }
      });
    }
    // While a dialog is open or a save is being written: Enter does not press
    // Play (start.js listens on the window), Escape closes the dialog.
    addEventListener("keydown", (e) => {
      if (!view.isConnected) return;
      if (dialogOpen && e.key === "Escape") { e.preventDefault(); e.stopImmediatePropagation(); dialogOpen.close(null); return; }
      if ((dialogOpen || blockPlay) && e.key === "Enter" && !dialogOpen?.layer.contains(e.target)) {
        e.preventDefault();
        e.stopImmediatePropagation();
      }
    }, true);
    root.addEventListener("click", (e) => {
      if (blockPlay && e.target.closest?.(".cs-go")) {
        e.preventDefault();
        e.stopPropagation();
        say("Wait a moment: your save is being written.", "busy");
      }
    }, true);

    async function confirmRestore(title, files, from, yes) {
      const d = describe(files);
      const lines = [`${from} holds ${d.text}.`,
        `It replaces the save in this ${slot > 1 ? "window" : "browser"}. The save it replaces is kept, so you can put it back afterwards (Undo the last restore).`];
      return (await ask(title, lines, [{ label: yes, value: true, primary: true }, { label: "Cancel", value: false }])) === true;
    }

    // ---- Local backup ----
    async function downloadBackup() {
      const { files } = await readFolder();
      if (!files.length) throw new SaveError("empty");
      const zip = backupZip(files);
      const d = new Date(), pad = (n) => String(n).padStart(2, "0");
      const name = `NFS Carbon save ${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}.${pad(d.getMinutes())}${slot > 1 ? ` (window ${slot})` : ""}.zip`;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([zip], { type: "application/zip" }));
      a.download = name;
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 60000);
      return `Backup downloaded: “${name}”, ${describe(files).text}.`;
    }
    async function restoreFile(file) {
      if (file.size > 64 * 1024 * 1024) throw new SaveError("not-backup");
      const backup = await zipRead(new Uint8Array(await file.arrayBuffer()));
      const made = backup.info.made ? `The backup from ${when(backup.info.made)}` : `“${file.name}”`;
      say("");
      if (!(await confirmRestore("Restore this backup?", backup.files, made, "Replace my save"))) return "Nothing was changed.";
      say("Restoring…", "busy");
      await restore(backup.files);
      return `Restored: ${describe(backup.files).text}.`;
    }
    // Only careers change: settings and any other career stay, and the restore keeps the old save
    // for Undo. Nothing is written unless every career chosen passes.
    async function importCareers(files, from) {
      const careers = findCareers(files);
      if (!careers.length) throw new SaveError("no-career");
      const bad = careers.find((c) => c.problem);
      if (bad) throw new SaveError(bad.problem);
      const now = (await readFolder()).files;
      const taken = (name) => now.some((f) => f.path.toLowerCase() === `documents/nfs carbon/${name.toLowerCase()}/${name.toLowerCase()}`);
      const names = careers.map((c) => `“${c.name}”`).join(", "), same = careers.filter((c) => taken(c.name)).map((c) => `“${c.name}”`);
      say("");
      const lines = [`${from} holds ${careers.length === 1 ? "the career" : "the careers"} ${names}, and ${careers.length === 1 ? "it passes" : "they pass"} the game's checks.`];
      lines.push(same.length ? `This replaces your ${same.join(", ")} here. The old save is kept, so you can put it back (Undo the last restore).` : "Your settings and any other career here stay. You can undo this afterwards (Undo the last restore).");
      if ((await ask("Import from the PC game?", lines, [{ label: "Import", value: true, primary: true }, { label: "Cancel", value: false }])) !== true) return "Nothing was changed.";
      say("Importing…", "busy");
      const incoming = new Set(careers.map((c) => `documents/nfs carbon/${c.name.toLowerCase()}/`));
      const keep = now.filter((f) => ![...incoming].some((p) => `${f.path.toLowerCase()}/`.startsWith(p)));
      const mtime = Date.now();
      await restore(tidy([...keep, ...careers.flatMap((c) => [{ path: `Documents/NFS Carbon/${c.name}/${c.name}`, data: careerForThisGame(c.data), mtime }])]));
      return `Imported ${names}. Press Play, then choose Resume Career in the game. If you have more than one career, pick it with Load.`;
    }
    async function importFolder(list) {
      const picked = [...list].filter((f) => f.size === CAREER.size);
      if (!picked.length) throw new SaveError("no-career");
      const files = [];
      for (const f of picked.slice(0, 32)) files.push({ path: f.webkitRelativePath || f.name, data: new Uint8Array(await f.arrayBuffer()) });
      return importCareers(files, "This folder");
    }
    async function importFile(file) {
      if (file.size > 64 * 1024 * 1024) throw new SaveError("no-career");
      const data = new Uint8Array(await file.arrayBuffer());
      if ((data[0] === 0x52 && data[1] === 0x61 && data[2] === 0x72 && data[3] === 0x21) || (data[0] === 0x37 && data[1] === 0x7a && data[2] === 0xbc)) throw new SaveError("packed");
      if (data[0] === 0x50 && data[1] === 0x4b) {
        const zip = await zipRead(data, { anyFiles: true });
        if (zip.info.backup === "nfsc-save") return restoreFile(file); // a copy made here: career and settings
        return importCareers(zip.files, `“${file.name}”`);
      }
      return importCareers([{ path: file.name, data }], `“${file.name}”`);
    }
    async function undoRestore() {
      const undo = await readUndo();
      if (!undo) return "";
      const backup = await zipRead(new Uint8Array(await undo.file.arrayBuffer()));
      say("");
      if ((await ask("Undo the last restore?", [`This puts back the save that the last restore replaced: ${describe(backup.files).text}.`,
            "The save there now is replaced by it."],
          [{ label: "Put it back", value: true, primary: true }, { label: "Cancel", value: false }])) !== true) return "Nothing was changed.";
      await restore(backup.files, { undoing: true });
      return `Put back: ${describe(backup.files).text}.`;
    }

    // ---- Cloud ----
    async function turnOn() {
      const phrase = newPhrase();
      cloud = { phrase, id: await cloudId(phrase), ack: false, since: new Date().toISOString() };
      store.set(cloudKey, cloud);
      store.set(syncKey, {});
      identityChanged();
      state.notice = null;
      showCloud();
      if (!state.files.length) return "Cloud saves are on. Your save goes up once there is one.";
      return upload("new");
    }
    // mode: "match" replaces only the cloud save this browser last saw, "new"
    // only adds one where there is none, "force" replaces whatever is there.
    async function upload(mode) {
      const { files } = await readFolder();
      if (!files.length) throw new SaveError("empty");
      const sealed = await seal(cloud.phrase, backupZip(files));
      if (sealed.length > CLOUD_MAX) throw new SaveError("too-big");
      const headers = { "content-type": "application/octet-stream" };
      const rec = synced();
      if (mode === "match" && rec?.etag) headers["if-match"] = rec.etag;
      else if (mode !== "force") headers["if-none-match"] = "*";
      const r = await request("PUT", cloud.id, { body: sealed, headers });
      if (r.status === 412) {
        const head = await request("HEAD", cloud.id, { timeout: 10000 });
        state.notice = { kind: "conflict", at: head.headers.get("last-modified") };
        return "The cloud has a different save. Choose which one stays (above).";
      }
      if (!r.ok) throw failure(r);
      const answer = await r.json().catch(() => ({}));
      remember({ etag: r.headers.get("etag") || answer.etag, fp: await fingerprint(files), at: answer.saved || new Date().toISOString() });
      state.notice = null;
      return `Saved to the cloud: ${describe(files).text}.`;
    }
    async function download(phrase, id, adopt) {
      const r = await request("GET", id);
      if (!r.ok) throw failure(r);
      const etag = r.headers.get("etag"), at = r.headers.get("last-modified") || new Date().toISOString();
      const blob = new Uint8Array(await r.arrayBuffer());
      const backup = await zipRead(await unseal(phrase, blob));
      say("");
      if (!(await confirmRestore("Load the cloud save?", backup.files, `The cloud save from ${when(backup.info.made || at)}`, "Load it")))
        return "Nothing was changed.";
      say("Loading the cloud save…", "busy");
      await restore(backup.files);
      if (adopt) { cloud = adopt; store.set(cloudKey, cloud); store.set(syncKey, {}); identityChanged(); }
      remember({ etag, fp: await fingerprint(backup.files), at: backup.info.made || at });
      state.notice = null;
      return `Loaded from the cloud: ${describe(backup.files).text}.`;
    }
    function loadTyped() {
      const text = $(".sv-phrase-in")?.value || "";
      run("Opening the cloud save…", async () => {
        const phrase = parsePhrase(text);
        const id = await cloudId(phrase);
        return download(phrase, id, { phrase, id, ack: true, since: new Date().toISOString() });
      }, { blocksPlay: true });
    }
    // When the page opens: send this browser's changes up, or say that the
    // cloud has something newer. Nothing is replaced here without asking.
    async function sync() {
      if (!cloud || !server()) return;
      const rec = synced(), changed = !rec || rec.fp !== state.fp;
      const head = await request("HEAD", cloud.id, { timeout: 10000 });
      if (head.status === 404) {
        // Removed from the cloud here (keyphrase kept): it goes back up only with Save to cloud.
        if (!state.files.length || rec?.removed) return "";
        return upload("new");
      }
      if (!head.ok) throw failure(head);
      const etag = head.headers.get("etag"), at = head.headers.get("last-modified");
      if (rec && etag === rec.etag) return changed && state.files.length ? upload("match") : "";
      // Unchanged here (or nothing here): only the cloud moved on. Otherwise both did.
      state.notice = { kind: !state.files.length || (rec && !changed) ? "newer" : "conflict", at };
      return "";
    }
    async function copy(el) {
      try {
        await navigator.clipboard.writeText(cloud.phrase);
      } catch {
        const t = document.createElement("textarea");
        t.value = cloud.phrase;
        document.body.append(t);
        t.select();
        document.execCommand("copy");
        t.remove();
      }
      el.firstChild.textContent = "Copied";
      setTimeout(() => { if (el.isConnected) el.firstChild.textContent = "Copy"; }, 2000);
    }
    function savePhraseFile() {
      const text = `Need for Speed Carbon (browser edition): cloud save keyphrase\n\n${cloud.phrase}\n\n` +
        `On another computer: start page, Saves, Cloud, "I have a keyphrase".\n` +
        `Keep it private: anyone with it can load and replace your cloud save.\n`;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
      a.download = "NFS Carbon keyphrase.txt";
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    }
    async function turnOff() {
      const yes = await ask("Stop cloud saves here?", [
        "This computer forgets the keyphrase and stops sending your save to the cloud.",
        "The cloud save stays on the server: with the keyphrase you can load it again, here or elsewhere. (Remove from cloud deletes it.)"],
        [{ label: "Stop", value: true, primary: true }, { label: "Cancel", value: false }]);
      if (yes !== true) return;
      store.set(cloudKey, null);
      store.set(syncKey, {});
      cloud = null;
      identityChanged();
      state.notice = null;
      showCloud();
      say("Cloud saves are off on this computer.", "good");
    }

    // Deletes the cloud copy on this server, after asking. The keyphrase is
    // forgotten here only if the player chooses that; otherwise this browser
    // keeps it and sends nothing up by itself until Save to cloud (the sync
    // record for this server says `removed`). The save in the browser stays.
    async function removeCloud() {
      const s = server();
      if (!s) throw new SaveError("no-server");
      const head = await request("HEAD", cloud.id, { timeout: 10000 });
      if (head.status === 404) return "There is no cloud save for this keyphrase on this server, so there is nothing to remove.";
      if (!head.ok) throw failure(head);
      const etag = head.headers.get("etag"), at = head.headers.get("last-modified");
      const forget = { label: "Also forget the keyphrase on this computer (turns cloud saves off here)", checked: false };
      say("");
      const yes = await ask("Remove your save from the cloud?", [
        `This deletes the cloud copy of your save (saved ${when(at)}) from ${s.label ? `the “${s.label}” server` : s.base}. It can't be undone.`,
        "Other computers that use the same keyphrase lose it too: none of them can load it any more. " +
          "If one of them still has cloud saves on, it puts its own save back in the cloud the next time the game's start page opens there.",
        `The save in this ${slot > 1 ? "window" : "browser"} stays as it is.`],
        [{ label: "Remove from cloud", value: true, primary: true }, { label: "Cancel", value: false, focus: true }], forget);
      if (yes !== true) return "Nothing was changed.";
      say("Removing the cloud save…", "busy");
      // If-Match: only the copy the player was just told about; 404 means it is gone already.
      let r;
      try {
        r = await request("DELETE", cloud.id, { headers: etag ? { "if-match": etag } : {}, timeout: 30000 });
      } catch (error) {
        // The server answered the HEAD just now: a relay from before DELETE fails the browser's preflight.
        throw error?.code === "unreachable" ? new SaveError("no-remove") : error;
      }
      if (r.status === 405) throw new SaveError("no-remove");
      if (!r.ok && r.status !== 404) throw failure(r);
      state.notice = null;
      if (forget.checked) {
        store.set(cloudKey, null);
        store.set(syncKey, {});
        cloud = null;
        identityChanged();
        return "Removed from the cloud, and this computer forgot the keyphrase. The save in this browser is unchanged.";
      }
      remember({ removed: new Date().toISOString() });
      return "Removed from the cloud. The save in this browser is unchanged, and nothing goes up by itself until you press Save to cloud.";
    }

    $(".sv-down").onclick = () => run("Packing your save…", downloadBackup);
    $(".sv-up").onclick = () => $(".sv-file").click();
    $(".sv-file").onchange = () => {
      const file = $(".sv-file").files[0];
      $(".sv-file").value = "";
      if (file) run("Reading the backup…", () => restoreFile(file), { blocksPlay: true });
    };
    $(".sv-undo").onclick = () => run("Putting the save back…", undoRestore, { blocksPlay: true });
    if (matchMedia("(pointer: coarse)").matches) $(".sv-import-dir").hidden = true; // phones have no folder picker
    $(".sv-import-dir").onclick = () => $(".sv-pick-dir").click();
    $(".sv-import-file").onclick = () => $(".sv-pick-file").click();
    $(".sv-pick-dir").onchange = () => {
      const list = [...$(".sv-pick-dir").files];
      $(".sv-pick-dir").value = "";
      if (list.length) run("Checking your save…", () => importFolder(list), { blocksPlay: true });
    };
    $(".sv-pick-file").onchange = () => {
      const file = $(".sv-pick-file").files[0];
      $(".sv-pick-file").value = "";
      if (file) run("Checking your save…", () => importFile(file), { blocksPlay: true });
    };
    // The server is chosen in the Online panel: show and sync for the new one.
    root.querySelector(".cs-server")?.addEventListener("change", () => {
      state.notice = null;
      if (!busy) run("Checking the cloud…", sync);
    });

    run("Looking at your save…", async () => { await look(); return cloud ? sync() : ""; });
  }
})();
