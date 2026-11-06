// Local customization upgrade. Only game assets are changed; profiles are never opened.
//
// Robustness rules (2026-11-06, player reports logs/reports-review-20261106):
// - Files are streamed: downloads go straight to the staging folder in 1 MiB writes, copies go in
//   4 MiB Blob slices and large files are hashed in pieces, so no step holds a whole 38 MB car file
//   in memory on a phone (59 iPhone reports stopped with WebKit's "unknown transient reason (e.g.
//   out of memory)" while preparing these files).
// - WebKit writes the WHOLE underlying ArrayBuffer when FileSystemWritableFileStream.write() gets a
//   typed-array view (logs/startup-fixes-20261106/webkit/probe-view.txt), so only exact buffers or
//   Blobs are ever written.
// - Downloads retry with backoff, a stalled body is aborted, and a file whose hash differs is
//   downloaded again. An installed file is read back and checked; a copy that differs is copied
//   again, then downloaded again.
// - A car or shared part folder (CARS/<name>/) that still fails is restored to what was there
//   before and left out; the game starts without it and the page shows a notice. Shared catalog
//   files (GLOBAL, FRONTEND, LANGUAGES, VINYLS) are all-or-nothing: if one fails, every change of
//   this run is restored and the error says (customizationOptional) whether the game can start
//   without any customization.
const CUSTOM_VERSION = '5911a37d9f9bc652b3fa457127273970b0d2a5e0e87c7081fb789a2c05439dbc';
const EXECUTABLE = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3';
const VECTOR_SHA = 'aa21d7944dea5e72c28aba39d3a936d7179866a7d6aec504f0c78666fba51a81';
const ASSET_PATH = /^(GLOBAL\/(GLOBALB\.BUN|GlobalB\.lzc|attributes\.bin|FE_ATTRIB\.bin|gameplay\.bin|gameplay\.lzc)|CARS\/(240SX|300C|350Z|911TURBO|997GT3RS|997S|997TT|BMWM3GTRE46|BRAKES|BRERA|CAMARO|CAMARON|CARRERAGT|CAYMANS|CCX|CHALLENGER71|CHALLENGERN|CHARGER06|CHARGER69|CLIO|CLK500|COROLLA|CORVETTEZ06|DB9|DRIVERS|ECLIPSE|ECLIPSEGT|ELISE|EUROPA|EXHAUST|FORDGT|G35|GALLARDO|GTO|IMPREZAWRXSTI|IS300|JAGUARXK|LANCEREVO9|MAZDA3SPORT|MAZDASPEED3|MONARO|MR2|MURCIELAGO|MURCIELAGO640|MUSTANGGT|MUSTANGSHLBYN|MUSTANGSHLBYO|PLATES|R32|R8|RX7|RX8|SKYLINE|SL65|SLR|SPOILER|SPOILER_CARRERA|SPOILER_HATCH|SPOILER_PORSCHES|SUPRA|TIRES|VIPER|WHEELS5|WHEELS6|ZONDA)\/(GEOMETRY|TEXTURES)\.BIN|FRONTEND\/FRONTB2\.BUN|CARS\/VINYLS\/VINYLS\.BIN|LANGUAGES\/English_(Frontend|Global)\.bin)$/;
const MiB = 1 << 20;
const WRITE_CHUNK = MiB, COPY_CHUNK = 4 * MiB, HASH_CHUNK = 4 * MiB;
const DOWNLOAD_ATTEMPTS = 5, INSTALL_ATTEMPTS = 3, STALL_MS = 30000, BREAKER = 3;
let tuning = { retryBaseMs: 500, stallMs: STALL_MS, nativeHashLimit: null };

// ---- SHA-256 -------------------------------------------------------------------------------
const sha = async data => [...new Uint8Array(await crypto.subtle.digest('SHA-256', data))]
  .map(v => v.toString(16).padStart(2, '0')).join('');
const K = new Int32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
  0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
  0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,
  0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,
  0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
// Incremental SHA-256 (WebCrypto has no streaming digest). ~115 MB/s in V8 on an M-series Mac.
export function sha256() {
  const H = new Int32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const W = new Int32Array(64), pend = new Uint8Array(64); let used = 0, total = 0;
  function run(p, off, end) {
    let h0=H[0],h1=H[1],h2=H[2],h3=H[3],h4=H[4],h5=H[5],h6=H[6],h7=H[7];
    for (; off < end; off += 64) {
      for (let i = 0, j = off; i < 16; i++, j += 4) W[i] = (p[j] << 24) | (p[j+1] << 16) | (p[j+2] << 8) | p[j+3];
      for (let i = 16; i < 64; i++) { const x = W[i-15], y = W[i-2];
        W[i] = ((((x>>>7)|(x<<25))^((x>>>18)|(x<<14))^(x>>>3)) + W[i-7] + (((y>>>17)|(y<<15))^((y>>>19)|(y<<13))^(y>>>10)) + W[i-16]) | 0; }
      let a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (((e>>>6)|(e<<26))^((e>>>11)|(e<<21))^((e>>>25)|(e<<7))) + ((e&f)^(~e&g)) + K[i] + W[i]) | 0;
        const t2 = ((((a>>>2)|(a<<30))^((a>>>13)|(a<<19))^((a>>>22)|(a<<10))) + ((a&b)^(a&c)^(b&c))) | 0;
        h=g; g=f; f=e; e=(d+t1)|0; d=c; c=b; b=a; a=(t1+t2)|0;
      }
      h0=(h0+a)|0; h1=(h1+b)|0; h2=(h2+c)|0; h3=(h3+d)|0; h4=(h4+e)|0; h5=(h5+f)|0; h6=(h6+g)|0; h7=(h7+h)|0;
    }
    H[0]=h0; H[1]=h1; H[2]=h2; H[3]=h3; H[4]=h4; H[5]=h5; H[6]=h6; H[7]=h7;
  }
  return {
    update(p) {
      let i = 0; total += p.length;
      if (used) { const take = Math.min(64 - used, p.length); pend.set(p.subarray(0, take), used); used += take; i = take;
        if (used < 64) return; run(pend, 0, 64); used = 0; }
      const end = i + ((p.length - i) & ~63); run(p, i, end);
      if (end < p.length) { pend.set(p.subarray(end), 0); used = p.length - end; }
    },
    hex() {
      const bits = total * 8, tail = new Uint8Array((used < 56 ? 56 : 120) - used + 8); tail[0] = 0x80;
      const v = new DataView(tail.buffer); v.setUint32(tail.length - 8, Math.floor(bits / 2 ** 32)); v.setUint32(tail.length - 4, bits >>> 0);
      this.update(tail);
      return Array.from(H, x => (x >>> 0).toString(16).padStart(8, '0')).join('');
    },
  };
}
function phone() {
  try { return navigator.maxTouchPoints > 0 && !!globalThis.matchMedia?.('(pointer: coarse)').matches; } catch { return false; }
}
// Files up to this size use the browser's digest on one buffer; bigger ones are hashed in pieces.
const nativeHashLimit = () => tuning.nativeHashLimit ?? (phone() ? 4 * MiB : 32 * MiB);
export async function hashFile(file) {
  if (file.size <= nativeHashLimit()) return sha(await file.arrayBuffer());
  const h = sha256();
  for (let o = 0; o < file.size; o += HASH_CHUNK) h.update(new Uint8Array(await file.slice(o, o + HASH_CHUNK).arrayBuffer()));
  return h.hex();
}

// ---- Storage helpers -----------------------------------------------------------------------
const missing = e => e?.name === 'NotFoundError';
const quota = e => e?.name === 'QuotaExceededError' || /quota|not enough (disk )?space/i.test(String(e?.message || ''));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const backoff = attempt => tuning.retryBaseMs * 2 ** (attempt - 1);
// Never hand a typed-array view to createWritable().write(): WebKit writes the whole buffer.
function exact(data) {
  if (data instanceof ArrayBuffer || (typeof Blob !== 'undefined' && data instanceof Blob) || typeof data === 'string') return data;
  const u8 = data instanceof Uint8Array ? data : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  return u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength ? u8.buffer : u8.slice().buffer;
}
async function openWriter(dir, name) {
  const handle = await dir.getFileHandle(name, {create:true});
  if (typeof handle.createWritable === 'function') {
    const out = await handle.createWritable();
    return { write: data => out.write(exact(data)), close: () => out.close(), abort: () => Promise.resolve(out.abort?.()).catch(() => {}) };
  }
  // Safari before 26 has no createWritable on any thread; a worker can still use a sync handle.
  if (typeof handle.createSyncAccessHandle === 'function' && typeof WorkerGlobalScope !== 'undefined') {
    const access = await handle.createSyncAccessHandle(); access.truncate(0); let at = 0;
    return {
      async write(data) {
        const bytes = new Uint8Array(data instanceof Blob ? await data.arrayBuffer() : typeof data === 'string' ? new TextEncoder().encode(data) : exact(data));
        if (access.write(bytes, {at}) !== bytes.length) throw new DOMException('short write', 'QuotaExceededError');
        at += bytes.length;
      },
      async close() { access.flush(); access.close(); },
      async abort() { try { access.close(); } catch {} },
    };
  }
  const error = new Error('This browser cannot write customization files (no FileSystemWritableFileStream).');
  error.name = 'NotSupportedError';
  throw error;
}
async function write(dir, name, data) {
  const out = await openWriter(dir, name);
  try { await out.write(data); await out.close(); }
  catch (e) { await out.abort(); throw e; }
}
async function json(dir, name) {
  try {
    const file = await (await dir.getFileHandle(name)).getFile();
    if (file.size > 262144) throw Error('Customization recovery record is too large');
    return JSON.parse(await file.text());
  } catch (e) { if (missing(e)) return null; throw e; }
}
async function locationFor(root, path, create = false) {
  const parts = path.split('/'); let dir = root;
  for (const part of parts.slice(0, -1)) {
    let actual = part;
    for await (const [name] of dir.entries()) if (name.toLowerCase() === part.toLowerCase()) actual = name;
    dir = await dir.getDirectoryHandle(actual, {create});
  }
  let name = parts.at(-1);
  for await (const [actual] of dir.entries()) if (actual.toLowerCase() === name.toLowerCase()) name = actual;
  return {dir, name};
}
async function fileAt(root, path) {
  try { const {dir,name} = await locationFor(root,path); return await (await dir.getFileHandle(name)).getFile(); }
  catch(e) { if(missing(e)) return null; throw e; }
}
async function writeAt(root, path, data) { const {dir,name} = await locationFor(root,path,true); await write(dir,name,data); }
// Copy a File into place in Blob slices: the browser reads them from disk, not from a JS buffer.
async function copyAt(root, path, file) {
  const {dir,name} = await locationFor(root,path,true);
  const out = await openWriter(dir, name);
  try { for (let o = 0; o < file.size; o += COPY_CHUNK) await out.write(file.slice(o, Math.min(file.size, o + COPY_CHUNK))); await out.close(); }
  catch (e) { await out.abort(); throw e; }
}
async function removeAt(root, path) {
  try { const {dir,name} = await locationFor(root,path); await dir.removeEntry(name); }
  catch(e) { if(!missing(e)) throw e; }
}
function recoveryPath(path) { return ASSET_PATH.test(path) || /^GLOBAL\/[A-Za-z0-9_-]*MemoryFile\.bin$/i.test(path); }
async function verify(file, item) { return !!file && file.size === item.bytes && await hashFile(file) === item.sha256; }
// A car or shared part folder can be left out on its own; everything else is the shared catalog.
export function unitOf(path) {
  const car = /^CARS\/([^/]+)\//i.exec(path);
  return car && car[1].toUpperCase() !== 'VINYLS' ? 'CARS/' + car[1].toUpperCase() : 'core';
}

// ---- Diagnostics ---------------------------------------------------------------------------
function downloadDiagnostic(record) {
  const records=globalThis.carbonDiagnostics?.downloads;
  if(!Array.isArray(records)) return;
  records.push({at:new Date().toISOString(),phase:'customization',...record});
  while(records.length>20) records.shift();
}
// Which step failed, for player reports (WebKit's DOMExceptions carry no stack).
function note(step, extra = {}) {
  const d = globalThis.carbonDiagnostics; if (!d || typeof d !== 'object') return;
  const list = d.customization ||= [];
  list.push({at:new Date().toISOString(), step, ...extra});
  while (list.length > 30) list.shift();
}
const errorInfo = e => ({name:String(e?.name || 'Error'), message:String(e?.message || e).slice(0, 300)});
function tag(error, step, path) {
  if (error && typeof error === 'object' && !error.customizationStep) { try { error.customizationStep = step; error.customizationPath = path; } catch {} }
  note('failed', {failedStep: step, path, error: errorInfo(error)});
  return error;
}
async function step(name, path, fn) { try { return await fn(); } catch (e) { throw tag(e, name, path); } }

// ---- Downloads -----------------------------------------------------------------------------
function retryableDownload(error) {
  if (error?.name === 'HashMismatch' || error?.name === 'TimeoutError' || error?.stalled) return true;
  if (error?.name === 'AbortError' || error?.name === 'SyntaxError') return false;
  return error?.retryable === true || (error?.name === 'TypeError' && error?.retryable !== false);
}
function finalDownloadError(cause) {
  const error = new Error('A customization file could not be downloaded. Check your connection and press Play again. Completed downloads are kept.', {cause});
  error.download = true;
  return error;
}
// fetch with a stall timer: a body that sends nothing for STALL_MS is aborted (and retried).
async function fetchWithStall(url, consume, attempt) {
  const controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
  let timer = 0;
  const arm = () => { if (!controller) return; clearTimeout(timer); timer = setTimeout(() => controller.abort(), tuning.stallMs); };
  arm();
  try {
    let response;
    // no-store on every attempt: a retry must not get a cached copy, and 1 GB of assets must not
    // fill the HTTP cache next to the game's own storage.
    try { response = await fetch(url, {cache: 'no-store', ...(controller ? {signal: controller.signal} : {})}); }
    catch (e) { if (controller?.signal.aborted) { const s = new Error('The download stalled.'); s.name = 'TimeoutError'; s.stalled = true; throw s; } throw e; }
    if (!response.ok) {
      const error = new Error(`Download returned HTTP ${response.status}.`);
      error.retryable = response.status === 408 || response.status === 429 || response.status >= 500;
      error.status = response.status;
      await response.body?.cancel?.().catch?.(() => {});
      throw error;
    }
    try { return {status: response.status, value: await consume(response, arm)}; }
    catch (e) { if (controller?.signal.aborted && e?.name === 'AbortError') { const s = new Error('The download stalled.'); s.name = 'TimeoutError'; s.stalled = true; s.status = response.status; throw s; }
      if (e && typeof e === 'object' && e.status === undefined) try { e.status = response.status; } catch {}
      throw e; }
  } finally { clearTimeout(timer); }
}
async function withRetries(url, consume) {
  const parsed = new URL(url, globalThis.document?.baseURI || globalThis.location?.href);
  const safeUrl = parsed.origin + parsed.pathname;
  for (let attempt = 1; attempt <= DOWNLOAD_ATTEMPTS; attempt++) {
    try {
      const {status, value} = await fetchWithStall(url, consume, attempt);
      downloadDiagnostic({url:safeUrl, attempt, status});
      return value;
    } catch (error) {
      downloadDiagnostic({url:safeUrl, attempt, status:error?.status ?? null, error:errorInfo(error)});
      if (quota(error) || !retryableDownload(error)) throw error;
      if (attempt === DOWNLOAD_ATTEMPTS) throw finalDownloadError(error);
      await sleep(backoff(attempt));
    }
  }
}
// Small files (the catalog): one buffer.
async function downloadCustomization(url, asJson = false) {
  return withRetries(url, async response => {
    // Reading the body belongs inside the retry: a connection can fail after HTTP 200.
    const bytes = await response.arrayBuffer();
    return asJson ? JSON.parse(new TextDecoder().decode(bytes)) : bytes;
  });
}
// Large files: streamed into root/path in 1 MiB writes while hashing; the file is closed (and so
// replaced) only when its size and hash match the catalog row. A mismatch is downloaded again.
async function downloadTo(url, root, path, row) {
  return withRetries(url, async (response, arm) => {
    const {dir, name} = await locationFor(root, path, true);
    const out = await openWriter(dir, name);
    const h = sha256(); let received = 0;
    try {
      const reader = response.body?.getReader?.();
      if (reader) {
        let buffer = new Uint8Array(WRITE_CHUNK), used = 0;
        for (;;) {
          const {done, value} = await reader.read();
          if (done) break;
          arm(); received += value.byteLength;
          if (received > row.bytes) { await reader.cancel().catch(() => {}); break; }
          h.update(value);
          for (let o = 0; o < value.byteLength;) {
            const n = Math.min(buffer.length - used, value.byteLength - o);
            buffer.set(value.subarray(o, o + n), used); used += n; o += n;
            if (used === buffer.length) { await out.write(buffer.buffer); buffer = new Uint8Array(WRITE_CHUNK); used = 0; }
          }
        }
        if (used && received <= row.bytes) await out.write(buffer.slice(0, used).buffer);
      } else {
        const bytes = new Uint8Array(await response.arrayBuffer()); received = bytes.byteLength;
        if (received === row.bytes) { h.update(bytes); await out.write(bytes.buffer); }
      }
      if (received !== row.bytes || h.hex() !== row.sha256) {
        const error = new Error(`Customization asset hash mismatch: ${row.path}`);
        error.name = 'HashMismatch'; throw error;
      }
      await out.close();
    } catch (e) { await out.abort(); throw e; }
  });
}

// ---- Recovery ------------------------------------------------------------------------------
// Streamed packed-data caches can contain older copies of the replaced assets.
const STREAM_CACHES = [{"path": "GLOBAL/GlobalMemoryFile.bin", "bytes": 9381872, "sha256": "6da121b60de65b2a7f78c24fc7a891dd253799f0124313b66c63e097e74d1892"}, {"path": "GLOBAL/CarHeadersMemoryFile.bin", "bytes": 362736, "sha256": "bdb080b1d50c6fc167970f6cfec2519b39558dfab568fe03d6e6c70b4ec9ecf4"}];
// Restores one recorded original (or removes a file that did not exist) and checks the result.
async function restoreRow(game, backup, row, options) {
  if (!row.existed) { await removeAt(game, row.path); return; }
  const saved = await fileAt(backup, row.path);
  if (await verify(saved, row)) {
    await copyAt(game, row.path, saved);
    if (!await verify(await fileAt(game, row.path), row)) throw Error('Customization restore differs: ' + row.path);
    return;
  }
  // Streamed games read every original from the server (the stream index marks it "R"), so a
  // broken local backup is replaced by removing the local copy; the base caches are downloaded.
  if (options.stream) {
    const cache = STREAM_CACHES.find(c => c.path.toLowerCase() === row.path.toLowerCase());
    note('backup-unusable', {path: row.path, recovery: cache && options.streamBase ? 'download base cache' : 'remove local copy'});
    if (cache && options.streamBase) {
      await downloadTo(options.streamBase.replace(/\/$/,'') + '/' + cache.path, game, cache.path, cache);
      return;
    }
    await removeAt(game, row.path);
    return;
  }
  throw Error('Customization backup verification failed: ' + row.path);
}
async function restorePending(target, game, pending, report, options) {
  if(pending.schema !== 1 || !/^[a-f0-9]{64}$/.test(pending.version) || !Array.isArray(pending.entries) || pending.entries.length>128)
    throw Error('Invalid customization recovery record');
  const backup = await target.getDirectoryHandle('.customization-backup-'+pending.version, {create: !!options.stream});
  // Verify every retained original before restoring any game asset (streamed games can rebuild them).
  for(const row of pending.entries) {
    if(!recoveryPath(row.path) || typeof row.existed !== 'boolean') throw Error('Invalid customization recovery path');
    if(!options.stream && row.existed && !await verify(await fileAt(backup,row.path),row)) throw Error('Customization backup verification failed: '+row.path);
  }
  report('Restoring the previous game assets…');
  for(const row of pending.entries) await step('restore', row.path, () => restoreRow(game, backup, row, options));
  await target.removeEntry('.customization-pending.json');
  await target.removeEntry('.custom-install.json').catch(e=>{if(!missing(e))throw e;});
}
// A full import may only start without customization when no other customization version was ever
// installed into it (its files would then be neither original nor current).
async function originalFilesOnly(target) {
  if (await json(target, '.custom-install.json')) return false;
  for await (const [name] of target.entries())
    if (name.startsWith('.customization-backup-') && name !== '.customization-backup-' + CUSTOM_VERSION) return false;
  return true;
}

// ---- Install -------------------------------------------------------------------------------
function checkManifest(manifest) {
  if(manifest?.version !== CUSTOM_VERSION || manifest.executable !== EXECUTABLE || manifest.files?.length !== 79 ||
     manifest.partCount !== 23115 || manifest.nativeFingerprint !== 'e1e15255' || manifest.originalIndexesPreserved !== true || manifest.originalAttributesPreserved !== true)
    throw Error('Customization runtime and catalog differ');
  const paths = new Set();
  for(const row of manifest.files) {
    if(!ASSET_PATH.test(row.path) || paths.has(row.path.toLowerCase()) || !Number.isSafeInteger(row.bytes) || row.bytes<=0 || !/^[a-f0-9]{64}$/.test(row.sha256))
      throw Error('Invalid customization asset manifest');
    paths.add(row.path.toLowerCase());
  }
  const vector = manifest.files.find(row=>row.path==='CARS/VINYLS/VINYLS.BIN');
  if(vector?.bytes !== 5502976 || vector.sha256 !== VECTOR_SHA) throw Error('Vector resource and catalog differ');
  return vector;
}
async function storageRoots() {
  const target = await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
  return {target, game: await target.getDirectoryHandle('game')};
}
function coreFailure(cause, failedUnit) {
  const error = cause instanceof Error ? cause : new Error(String(cause));
  try { error.customizationUnit = failedUnit; } catch {}
  return error;
}
async function install(report, options) {
  const assets = options.assetBase || "./custom-assets/";
  let manifest, fromCache = false;
  try { manifest = await downloadCustomization(assets+'manifest.json',true); }
  catch (e) {
    // A catalog checked on an earlier start lets an installed customization start without the network.
    if (!e?.download) throw e;
    let cached = null;
    try { cached = await json((await storageRoots()).target, '.custom-manifest.json'); } catch {}
    if (cached?.version !== CUSTOM_VERSION) throw e;
    manifest = cached; fromCache = true; note('catalog-from-cache');
  }
  const vector = checkManifest(manifest);
  const {target, game} = await storageRoots();
  const stamp = await fileAt(game,'.stamp');
  if(options.stream) {
    const exe = await fileAt(game,'NFSC.exe');
    if(!exe || await hashFile(exe) !== EXECUTABLE) throw Error('Streaming executable differs from the customization baseline');
  } else if(!stamp || (await stamp.text()).trim() !== EXECUTABLE) throw Error('Imported game executable differs from the customization baseline');
  if (!fromCache) await write(target, '.custom-manifest.json', JSON.stringify(manifest)).catch(e => { if (quota(e)) throw e; });
  const pending = await json(target,'.customization-pending.json');
  if(pending) await restorePending(target,game,pending,report,options);
  const ready = await json(target,'.custom-install.json');
  const readyOk = ready?.version === CUSTOM_VERSION && ready?.savePolicy === 'existing-profiles-unchanged';
  const earlierSkips = readyOk && Array.isArray(ready.skipped) ? ready.skipped.filter(u => typeof u === 'string') : [];
  if(readyOk && !earlierSkips.length) {
    let complete=true;
    for(const row of manifest.files) { const file=await fileAt(game,row.path); if(!file || file.size!==row.bytes) { complete=false; break; } }
    if(complete && await verify(await fileAt(game,vector.path),vector)) return finish(manifest, []);
  }
  // Files of a finished earlier run are trusted by size; their hash was checked when installed.
  const trusted = row => readyOk && !earlierSkips.includes(unitOf(row.path));
  const vanillaSafe = options.stream ? true : await originalFilesOnly(target).catch(() => false);
  const fail = (e, unit) => { const error = coreFailure(e, unit); try { error.customizationOptional = vanillaSafe; } catch {} return error; };
  const stageName='.customization-stage-'+CUSTOM_VERSION;
  const stage=await target.getDirectoryHandle(stageName,{create:true});
  const failed = new Map(); let streak = 0;
  const failUnit = (unit, error) => {
    failed.set(unit, error); note('unit-skipped', {unit, error: errorInfo(error)});
    if (++streak >= BREAKER && unit !== 'core' && !failed.has('core'))
      failed.set('core', Object.assign(new Error('Customization stopped after repeated failures.', {cause: error}), {customizationStep: 'breaker'}));
  };
  const changes=[];
  // Download and hash every incoming asset before any installed asset is changed.
  for(let i=0;i<manifest.files.length && !failed.has('core');i++) {
    const row=manifest.files[i], unit=unitOf(row.path); report(`Preparing customization assets… ${i+1}/${manifest.files.length}`);
    if (failed.has(unit)) continue;
    try {
      const current=await step('check installed', row.path, () => fileAt(game,row.path));
      if (current && trusted(row) && current.size === row.bytes) { streak = 0; continue; }
      if(await step('hash installed', row.path, () => verify(current,row))) { streak = 0; continue; }
      if(!await step('hash staged', row.path, async () => verify(await fileAt(stage,row.path),row)))
        await step('download', row.path, () => downloadTo(assets+row.path, stage, row.path, row));
      changes.push(row); streak = 0;
    } catch (e) { if (quota(e)) throw e; failUnit(unit, e); }
  }
  if (failed.has('core')) throw fail(failed.get('core'), 'core');
  const changed=new Set();
  for(const row of manifest.files) for(const name of [row.path.replaceAll('/','\\'),row.path.replaceAll('/','\\').toUpperCase(),row.path.replaceAll('/','\\').toLowerCase()]) {
    let h=0xffffffff; for(const c of name) h=(Math.imul(h,33)+c.charCodeAt(0))>>>0; changed.add(h);
  }
  const global=await game.getDirectoryHandle('GLOBAL',{create:true});
  try {
    for await(const[name,handle]of global.entries()) {
      if(handle.kind!=='file' || !/MemoryFile\.bin$/i.test(name)) continue;
      const result=pruneMemory(new Uint8Array(await (await handle.getFile()).arrayBuffer()),changed);
      if(!result) continue;
      const row={path:'GLOBAL/'+name,bytes:result.byteLength,sha256:await sha(result)};
      await writeAt(stage,row.path,result); changes.push(row);
    }
  } catch (e) { if (quota(e)) throw e; throw fail(tag(e, 'prune memory file', 'GLOBAL'), 'core'); }
  const backup=await target.getDirectoryHandle('.customization-backup-'+CUSTOM_VERSION,{create:true});
  const existingBackup=await json(backup,'.originals.json');
  let originals=existingBackup?.entries || [];
  if(existingBackup && (existingBackup.schema!==1 || existingBackup.version!==CUSTOM_VERSION || !Array.isArray(originals))) throw Error('Invalid customization backup record');
  const entries=[];
  for(const row of changes) {
    const unit = unitOf(row.path);
    if (failed.has(unit)) continue;
    try {
      const saved=originals.find(item=>item.path===row.path);
      if(saved) {
        if(saved.existed && !await step('check backup', row.path, async () => verify(await fileAt(backup,row.path),saved))) throw Error('Customization original backup differs: '+row.path);
        entries.push(saved); continue;
      }
      const original=await fileAt(game,row.path);
      const record={path:row.path,existed:!!original,bytes:original?.size||0,sha256:original?await step('hash original', row.path, () => hashFile(original)):null};
      if(original) {
        await step('back up original', row.path, () => copyAt(backup,row.path,original));
        if(!await verify(await fileAt(backup,row.path),record)) throw Error('Customization backup write failed: '+row.path);
      }
      originals.push(record); entries.push(record);
    } catch (e) { if (quota(e)) throw e; failUnit(unit, e); }
  }
  if (failed.has('core')) throw fail(failed.get('core'), 'core');
  const active = changes.filter(row => !failed.has(unitOf(row.path)));
  await write(backup,'.originals.json',JSON.stringify({schema:1,version:CUSTOM_VERSION,entries:originals}));
  const transaction={schema:1,version:CUSTOM_VERSION,entries:entries.filter(e => active.some(r => r.path === e.path))};
  await write(target,'.customization-pending.json',JSON.stringify(transaction));
  try {
    for(let i=0;i<active.length;i++) {
      const row=active[i], unit=unitOf(row.path); report(`Installing customization assets… ${i+1}/${active.length}`);
      if (failed.has(unit)) continue;
      const error = await installRow(game, stage, assets, row);
      if (!error) { streak = 0; continue; }
      if (quota(error)) throw error;
      failUnit(unit, error);
      if (unit === 'core' || failed.has('core')) break;
      // Never leave a half-installed car: every file of it goes back to what was there before.
      for (const entry of transaction.entries) if (unitOf(entry.path) === unit) await step('restore', entry.path, () => restoreRow(game, backup, entry, options));
    }
    if (failed.has('core')) throw failed.get('core');
    await write(target,'.custom-install.json',JSON.stringify({version:CUSTOM_VERSION,savePolicy:'existing-profiles-unchanged',installed:new Date().toISOString(),skipped:[...failed.keys()]}));
    await target.removeEntry('.customization-pending.json');
  } catch(e) {
    await restorePending(target,game,transaction,report,options);
    if (quota(e)) throw e;
    throw fail(e, 'core');
  }
  // Staged copies are kept only for what was left out (the next start tries those again).
  if (!failed.size) await target.removeEntry(stageName,{recursive:true}).catch(()=>{});
  else for (const row of active) if (!failed.has(unitOf(row.path))) await removeAt(stage, row.path).catch(() => {});
  return finish(manifest, [...failed.keys()]);
}
// Copies one staged file into the game and reads it back; a copy that differs is copied again,
// then the staged file is downloaded again. Returns the last error, or null when installed.
async function installRow(game, stage, assets, row) {
  let lastError = null;
  for (let attempt = 1; attempt <= INSTALL_ATTEMPTS; attempt++) {
    try {
      let staged = await fileAt(stage, row.path);
      if (!await step('hash staged', row.path, () => verify(staged, row))) {
        await step('download', row.path, () => downloadTo(assets + row.path, stage, row.path, row));
        staged = await fileAt(stage, row.path);
      }
      await step('install', row.path, () => copyAt(game, row.path, staged));
      const installed = await fileAt(game, row.path);
      if (await step('hash installed', row.path, () => verify(installed, row))) return null;
      lastError = new Error('Installed customization asset differs: ' + row.path);
      note('install-differs', {path: row.path, attempt, expectedBytes: row.bytes, bytes: installed?.size ?? null});
      // The second miss suspects the staged copy itself: download it again.
      if (attempt >= 2) await removeAt(stage, row.path);
    } catch (e) {
      if (quota(e)) return e;
      lastError = e;
      if (e?.download) return e; // downloads already retried with backoff
    }
  }
  return lastError;
}
function finish(manifest, skipped) {
  globalThis.__carbonCustomVectorResourceSha256=VECTOR_SHA;
  globalThis.__carbonCustomizationAssetsVersion=CUSTOM_VERSION;
  return {manifest, skipped, state: skipped.length ? 'partial' : 'installed'};
}
function pruneMemory(bytes, changed) {
  const v = new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  if(bytes.length < 16 || v.getUint32(8,true) !== 0x53219999) return null;
  const count = v.getUint32(12,true);
  if(16 + count*20 > bytes.length) throw Error('Invalid memory-file table');
  const kept=[];
  for(let i=0;i<count;i++) {
    const row=Array.from({length:5},(_,j)=>v.getUint32(16+i*20+j*4,true));
    if(row[1]+row[3] > bytes.length) throw Error('Invalid cached file range');
    if(!changed.has(row[0])) kept.push(row);
  }
  if(kept.length === count) return null;
  const begin = (16+kept.length*20+15)&~15;
  const result = new Uint8Array(begin + kept.reduce((sum,row)=>sum+((row[3]+15)&~15),0));
  const out = new DataView(result.buffer); result.set(bytes.subarray(0,12)); out.setUint32(12,kept.length,true);
  let offset=begin;
  kept.forEach((row,i)=>{ const original=row[1]; row[1]=offset; row.forEach((n,j)=>out.setUint32(16+i*20+j*4,n,true));
    result.set(bytes.subarray(original,original+row[3]),offset); offset+=(row[3]+15)&~15; });
  return result;
}
async function prepareStreamCaches(options,report) {
  if(!options.stream) return;
  const root=await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
  const game=await root.getDirectoryHandle('game');
  const ready=await json(root,'.custom-stream-caches.json');
  if(ready?.version===CUSTOM_VERSION && (await Promise.all(STREAM_CACHES.map(row=>fileAt(game,row.path)))).every(Boolean)) return;
  if(!options.streamBase) throw Error('Streaming asset base is missing');
  for(const row of STREAM_CACHES) {
    report('Preparing customization data caches…');
    // Streamed into place and replaced only when complete and matching.
    await step('download base cache', row.path, () => downloadTo(options.streamBase.replace(/\/$/,'')+'/'+row.path, game, row.path, row));
  }
  // Re-run the existing transactional cache pruner even after an earlier asset install.
  await root.removeEntry('.custom-install.json').catch(e=>{if(!missing(e))throw e;});
}
// Resolves {state: 'installed' | 'partial', skipped: ['CARS/<name>', ...]}. Rejects when the shared
// catalog could not be installed; error.customizationOptional then says whether the game files are
// as they were before any customization (a streamed game always is: its index is not rewritten),
// so the game may start without it.
export async function prepareCustomization(report=()=>{}, options={}) {
  tuning = {retryBaseMs: options.retryBaseMs ?? 500, stallMs: options.stallMs ?? STALL_MS, nativeHashLimit: options.nativeHashLimit ?? null};
  if(!navigator.locks?.request) {
    const error = Error('Customization installation requires browser storage locks');
    error.customizationOptional = !!options.stream;
    throw error;
  }
  return navigator.locks.request('carbon-customization-assets',{mode:'exclusive'},async()=>{
    try {
      try { await prepareStreamCaches(options,report); }
      catch (e) { if (!quota(e)) try { e.customizationOptional = true; } catch {} throw e; }
      const result=await install(report,options);
      if(options.stream) {
        const root=await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
        const game=await root.getDirectoryHandle('game');
        const file=await fileAt(game,'.stream-index');
        if(!file) throw Error('Streaming file index is missing');
        const lines=(await file.text()).trim().split('\n');
        // A left-out car keeps the worker's "R" line: the game streams its original files.
        const local=new Map([...result.manifest.files.filter(row=>!result.skipped.includes(unitOf(row.path))),...STREAM_CACHES].map(row=>[row.path.toLowerCase(),row.path]));
        const seen=new Set();
        const output=lines.map(line=>{const path=line.slice(2);seen.add(path.toLowerCase());return local.has(path.toLowerCase())?'L\t'+local.get(path.toLowerCase()):line;});
        for(const row of result.manifest.files) if(!seen.has(row.path.toLowerCase()) && local.has(row.path.toLowerCase())) output.push('L\t'+row.path);
        await write(game,'.stream-index',output.join('\n')+'\n');
        await write(root,'.custom-stream-caches.json',JSON.stringify({version:CUSTOM_VERSION}));
      }
      note(result.state, {skipped: result.skipped});
      return {state: result.state, skipped: result.skipped};
    } catch (e) {
      if (e && typeof e === 'object' && e.customizationOptional === undefined) {
        let optional = !!options.stream && !quota(e);
        // A full import that never had any customization can start without it when the catalog
        // cannot be downloaded (no storage is opened for an invalid catalog).
        if (!options.stream && e.download) optional = await storageRoots().then(r => originalFilesOnly(r.target)).catch(() => false);
        try { e.customizationOptional = optional; } catch {}
      }
      throw e;
    }
  });
}
