// Customization install transaction over an in-memory OPFS (2026-11-06): retries, re-downloads,
// read-back checks, cars left out and restored, and when the game may start without customization.
// The installer's catalog constants (79 files, the vector resource, the executable, the streamed
// base caches) are swapped for a six-file synthetic catalog; every other line is the shipped file.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile, writeFile, mkdtemp, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const sourcePath = process.env.CARBON_CUSTOM_INSTALL_SOURCE || new URL('../../source/web/custom-install.js', import.meta.url).pathname;
const hex = b => createHash('sha256').update(b).digest('hex');
const bytes = (n, seed) => { const a = new Uint8Array(n); let x = seed >>> 0; for (let i = 0; i < n; i++) { x = (Math.imul(x, 1103515245) + 12345) >>> 0; a[i] = x >>> 24; } return a; };
const EXE = bytes(70000, 99), VECTOR = bytes(300000, 7);
const CACHE = { path: 'GLOBAL/GlobalMemoryFile.bin', data: memoryFile() };
// A memory file whose table caches nothing the catalog changes (the pruner leaves it alone).
function memoryFile() { const b = new Uint8Array(64); const v = new DataView(b.buffer); v.setUint32(8, 0x53219999, true); v.setUint32(12, 0, true); return b; }
const FILES = {
  'CARS/240SX/GEOMETRY.BIN': bytes(5 << 20 | 123, 1),
  'CARS/240SX/TEXTURES.BIN': bytes(70001, 2),
  'CARS/300C/GEOMETRY.BIN': bytes(90002, 3),
  'CARS/300C/TEXTURES.BIN': bytes(40003, 4),
  'CARS/350Z/GEOMETRY.BIN': bytes(30005, 6),
  'CARS/VINYLS/VINYLS.BIN': VECTOR,
  'GLOBAL/GLOBALB.BUN': bytes(120004, 5),
};
const VERSION = 'a'.repeat(64);
const manifest = { version: VERSION, executable: hex(EXE), partCount: 23115, nativeFingerprint: 'e1e15255', originalIndexesPreserved: true, originalAttributesPreserved: true,
  files: Object.entries(FILES).map(([p, b]) => ({ path: p, bytes: b.length, sha256: hex(b) })) };

let source = await readFile(sourcePath, 'utf8');
const swap = (from, to) => { assert.ok(from.test ? from.test(source) : source.includes(from), `installer source no longer contains ${from}`); source = source.replace(from, to); };
swap(/const CUSTOM_VERSION = '[a-f0-9]{64}';/, `const CUSTOM_VERSION = '${VERSION}';`);
swap(/const EXECUTABLE = '[a-f0-9]{64}';/, `const EXECUTABLE = '${hex(EXE)}';`);
swap(/const VECTOR_SHA = '[a-f0-9]{64}';/, `const VECTOR_SHA = '${hex(VECTOR)}';`);
swap('manifest.files?.length !== 79', `manifest.files?.length !== ${manifest.files.length}`);
swap('vector?.bytes !== 5502976', `vector?.bytes !== ${VECTOR.length}`);
swap(/const STREAM_CACHES = \[.*\];/, `const STREAM_CACHES = ${JSON.stringify([{ path: CACHE.path, bytes: CACHE.data.length, sha256: hex(CACHE.data) }])};`);
const dir = await mkdtemp(path.join(tmpdir(), 'carbon-custom-install-'));
const modulePath = path.join(dir, 'custom-install.js');
await writeFile(modulePath, source);
const { prepareCustomization, sha256, unitOf } = await import(pathToFileURL(modulePath).href);
test.after(() => rm(dir, { recursive: true, force: true }));

// ---- In-memory OPFS ------------------------------------------------------------------------
const notFound = name => Object.assign(new Error(`${name} not found`), { name: 'NotFoundError' });
class FileHandle {
  constructor(fs, name, at) { this.kind = 'file'; this.name = name; this.fs = fs; this.at = at; this.data = new Uint8Array(0); }
  async getFile() { return new File([this.data], this.name); }
  async createWritable() {
    const fs = this.fs, where = this.at, handle = this; const parts = []; let open = true;
    return {
      async write(data) {
        if (!open) throw new Error('closed');
        if (ArrayBuffer.isView(data) && data.byteLength !== data.buffer.byteLength) fs.viewWrites.push(where);
        let chunk = data instanceof Blob ? new Uint8Array(await data.arrayBuffer()) : typeof data === 'string' ? new TextEncoder().encode(data)
          : data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer); // like WebKit: a view writes its whole buffer
        chunk = fs.onWrite?.(where, chunk) ?? chunk;
        parts.push(chunk.slice());
      },
      async close() { open = false; fs.onClose?.(where); const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0)); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } handle.data = out; fs.writes.push(where); },
      async abort() { open = false; },
    };
  }
}
class Dir {
  constructor(fs, at) { this.kind = 'directory'; this.fs = fs; this.at = at; this.items = new Map(); }
  async getDirectoryHandle(name, { create } = {}) {
    let d = this.items.get(name);
    if (!d) { if (!create) throw notFound(name); d = new Dir(this.fs, this.at + name + '/'); this.items.set(name, d); }
    if (d.kind !== 'directory') throw Object.assign(new Error('type'), { name: 'TypeMismatchError' });
    return d;
  }
  async getFileHandle(name, { create } = {}) {
    let f = this.items.get(name);
    if (!f) { if (!create) throw notFound(name); f = new FileHandle(this.fs, name, this.at + name); this.items.set(name, f); }
    return f;
  }
  async removeEntry(name) { if (!this.items.delete(name)) throw notFound(name); }
  async *entries() { for (const e of [...this.items]) yield e; }
}
function opfs() { const fs = { writes: [], viewWrites: [] }; fs.root = new Dir(fs, '/'); return fs; }
async function put(root, p, data) { const parts = p.split('/'); let d = root; for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x, { create: true }); (await d.getFileHandle(parts.at(-1), { create: true })).data = new Uint8Array(data); }
async function get(root, p) { try { const parts = p.split('/'); let d = root; for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x); return (await d.getFileHandle(parts.at(-1))).data; } catch (e) { if (e.name === 'NotFoundError') return null; throw e; } }
async function text(root, p) { const b = await get(root, p); return b && new TextDecoder().decode(b); }

// A streamed game as the kit's worker leaves it: the executable and an all-"R" stream index.
async function streamedGame(fs) {
  await put(fs.root, 'nfsc/game/NFSC.exe', EXE);
  await put(fs.root, 'nfsc/game/.stream-index', new TextEncoder().encode(['L\tNFSC.exe', ...Object.keys(FILES).map(p => 'R\t' + p), 'R\t' + CACHE.path, 'R\tTRACKS/L5RA.BUN'].join('\n') + '\n'));
}
// A full import: the stamp and the original files the catalog replaces.
const ORIGINAL = Object.fromEntries(Object.keys(FILES).map((p, i) => [p, bytes(30000 + i, 500 + i)]));
async function importedGame(fs) {
  await put(fs.root, 'nfsc/game/.stamp', new TextEncoder().encode(hex(EXE) + '\n'));
  for (const [p, b] of Object.entries(ORIGINAL)) await put(fs.root, 'nfsc/game/' + p, b);
}

const ASSETS = 'https://assets.test/custom/', BASE = 'https://assets.test/game';
// fetch over the synthetic catalog; plan[url] is a list of 'fail' | 'corrupt' | 'ok' consumed per request.
function server(plan = {}) {
  const calls = [];
  const fetch = async (url, options = {}) => {
    calls.push({ url: String(url), cache: options.cache });
    const step = (plan[url] ||= []).length ? plan[url].shift() : plan['*']?.(url) || 'ok';
    if (step === 'fail') throw new TypeError('Failed to fetch');
    let body;
    if (url === ASSETS + 'manifest.json') body = new TextEncoder().encode(JSON.stringify(manifest));
    else if (url.startsWith(ASSETS)) body = FILES[url.slice(ASSETS.length)];
    else if (url === BASE + '/' + CACHE.path) body = CACHE.data;
    if (!body) return new Response(null, { status: 404 });
    if (step === 'corrupt') { body = body.slice(); body[body.length >> 1] ^= 0xff; }
    // Several small chunks per response, as a network body arrives.
    const chunks = []; for (let o = 0; o < body.length; o += 65536) chunks.push(body.slice(o, o + 65536));
    return new Response(new ReadableStream({ pull(c) { chunks.length ? c.enqueue(chunks.shift()) : c.close(); } }), { status: 200 });
  };
  return { fetch, calls };
}
function install(t, fs, srv, options = {}) {
  const saved = ['fetch', 'navigator', 'carbonDiagnostics'].map(k => [k, Object.getOwnPropertyDescriptor(globalThis, k)]);
  t.after(() => { for (const [k, d] of saved) if (d) Object.defineProperty(globalThis, k, d); else delete globalThis[k]; });
  const env = { fetch: srv.fetch, carbonDiagnostics: { downloads: [] },
    navigator: { locks: { request: async (_n, _o, fn) => fn() }, storage: { getDirectory: async () => { if (options.storageError) throw options.storageError; return fs.root; } } } };
  for (const [k, v] of Object.entries(env)) Object.defineProperty(globalThis, k, { value: v, configurable: true, writable: true });
  return prepareCustomization(() => {}, { assetBase: ASSETS, stream: options.stream, streamBase: options.stream ? BASE : undefined, retryBaseMs: 1, nativeHashLimit: 1 << 20, ...options.extra });
}
const indexLines = async fs => (await text(fs.root, 'nfsc/game/.stream-index')).trim().split('\n');

test('the incremental SHA-256 matches the browser digest for every split', async () => {
  for (const n of [0, 1, 55, 56, 63, 64, 65, 1000, 70001]) {
    const b = bytes(n, n + 1), h = sha256();
    for (let o = 0; o < n;) { const k = Math.min(n - o, 1 + (o * 7) % 9000); h.update(b.subarray(o, o + k)); o += k; }
    assert.equal(h.hex(), hex(b), `length ${n}`);
  }
});
test('car folders are units; vinyls and the shared catalog are not', () => {
  assert.equal(unitOf('CARS/300C/GEOMETRY.BIN'), 'CARS/300C');
  assert.equal(unitOf('CARS/SPOILER_HATCH/GEOMETRY.BIN'), 'CARS/SPOILER_HATCH');
  for (const p of ['CARS/VINYLS/VINYLS.BIN', 'GLOBAL/GLOBALB.BUN', 'FRONTEND/FRONTB2.BUN', 'LANGUAGES/English_Global.bin', 'GLOBAL/GlobalMemoryFile.bin']) assert.equal(unitOf(p), 'core');
});

test('a streamed game installs every file, marks them local and never writes a typed-array view', async t => {
  const fs = opfs(); await streamedGame(fs); const srv = server();
  const result = await install(t, fs, srv, { stream: true });
  assert.deepEqual(result, { state: 'installed', skipped: [] });
  for (const [p, b] of Object.entries(FILES)) assert.deepEqual(await get(fs.root, 'nfsc/game/' + p), b, p);
  const lines = await indexLines(fs);
  for (const p of Object.keys(FILES)) assert.ok(lines.includes('L\t' + p), p);
  assert.ok(lines.includes('L\t' + CACHE.path)); assert.ok(lines.includes('R\tTRACKS/L5RA.BUN'));
  assert.equal(await get(fs.root, 'nfsc/.customization-pending.json'), null);
  assert.deepEqual(JSON.parse(await text(fs.root, 'nfsc/.custom-install.json')).skipped, []);
  assert.deepEqual(fs.viewWrites, []);
  assert.ok(srv.calls.every(c => c.cache === 'no-store'));
  // The staging folder is gone after a complete install.
  await assert.rejects((await fs.root.getDirectoryHandle('nfsc')).getDirectoryHandle('.customization-stage-' + VERSION), { name: 'NotFoundError' });
});

test('a download whose hash differs is downloaded again instead of stopping', async t => {
  const fs = opfs(); await streamedGame(fs);
  const url = ASSETS + 'CARS/300C/GEOMETRY.BIN', srv = server({ [url]: ['corrupt', 'fail', 'corrupt'] });
  assert.deepEqual(await install(t, fs, srv, { stream: true }), { state: 'installed', skipped: [] });
  assert.equal(srv.calls.filter(c => c.url === url).length, 4);
  assert.deepEqual(await get(fs.root, 'nfsc/game/CARS/300C/GEOMETRY.BIN'), FILES['CARS/300C/GEOMETRY.BIN']);
  assert.deepEqual(carbonDiagnostics.downloads.filter(d => d.url === url).map(d => d.error?.name || d.status), ['HashMismatch', 'TypeError', 'HashMismatch', 200]);
});

test('a car that cannot be downloaded is left out, streams its originals, and is tried again next start', async t => {
  const fs = opfs(); await streamedGame(fs);
  const url = ASSETS + 'CARS/300C/TEXTURES.BIN';
  const result = await install(t, fs, server({ [url]: Array(5).fill('fail') }), { stream: true });
  assert.deepEqual(result, { state: 'partial', skipped: ['CARS/300C'] });
  const lines = await indexLines(fs);
  assert.ok(lines.includes('R\tCARS/300C/GEOMETRY.BIN') && lines.includes('R\tCARS/300C/TEXTURES.BIN'), 'left-out car keeps its streamed originals');
  assert.ok(lines.includes('L\tCARS/240SX/GEOMETRY.BIN') && lines.includes('L\tGLOBAL/GLOBALB.BUN'));
  assert.equal(await get(fs.root, 'nfsc/game/CARS/300C/GEOMETRY.BIN'), null, 'no half-installed car');
  assert.ok(carbonDiagnostics.customization.some(n => n.step === 'unit-skipped' && n.unit === 'CARS/300C'));
  // Next start, network fine: only the left-out car is fetched again.
  await streamedGame(fs);
  const again = server();
  assert.deepEqual(await install(t, fs, again, { stream: true }), { state: 'installed', skipped: [] });
  assert.deepEqual(again.calls.map(c => c.url).filter(u => u.includes('/CARS/')).sort(), [ASSETS + 'CARS/300C/TEXTURES.BIN']);
  assert.ok((await indexLines(fs)).includes('L\tCARS/300C/TEXTURES.BIN'));
});

test('an installed file that reads back different is copied again', async t => {
  const fs = opfs(); await streamedGame(fs);
  let spoiled = 0;
  fs.onWrite = (where, chunk) => { if (where === '/nfsc/game/CARS/240SX/GEOMETRY.BIN' && !spoiled++) { chunk = chunk.slice(); chunk[0] ^= 1; return chunk; } };
  assert.deepEqual(await install(t, fs, server(), { stream: true }), { state: 'installed', skipped: [] });
  assert.deepEqual(await get(fs.root, 'nfsc/game/CARS/240SX/GEOMETRY.BIN'), FILES['CARS/240SX/GEOMETRY.BIN']);
  assert.ok(carbonDiagnostics.customization.some(n => n.step === 'install-differs' && n.path === 'CARS/240SX/GEOMETRY.BIN'));
});

test('a full import restores both files of a car whose second file keeps differing', async t => {
  const fs = opfs(); await importedGame(fs);
  // Only the catalog's copy is spoiled; the restore of the original writes cleanly.
  fs.onWrite = (where, chunk) => { if (where === '/nfsc/game/CARS/300C/TEXTURES.BIN' && chunk.length === FILES['CARS/300C/TEXTURES.BIN'].length) { chunk = chunk.slice(); chunk[5] ^= 1; return chunk; } };
  const srv = server();
  assert.deepEqual(await install(t, fs, srv), { state: 'partial', skipped: ['CARS/300C'] });
  // GEOMETRY.BIN had been installed before TEXTURES.BIN failed: both are the originals again.
  assert.deepEqual(await get(fs.root, 'nfsc/game/CARS/300C/GEOMETRY.BIN'), ORIGINAL['CARS/300C/GEOMETRY.BIN']);
  assert.deepEqual(await get(fs.root, 'nfsc/game/CARS/300C/TEXTURES.BIN'), ORIGINAL['CARS/300C/TEXTURES.BIN']);
  assert.deepEqual(await get(fs.root, 'nfsc/game/CARS/240SX/GEOMETRY.BIN'), FILES['CARS/240SX/GEOMETRY.BIN']);
  assert.equal(await get(fs.root, 'nfsc/.customization-pending.json'), null);
  // Third read-back miss re-downloaded the staged file.
  assert.equal(srv.calls.filter(c => c.url === ASSETS + 'CARS/300C/TEXTURES.BIN').length, 2);
});

test('a shared catalog file that cannot be downloaded leaves a streamed game original and allows starting without customization', async t => {
  const fs = opfs(); await streamedGame(fs);
  const before = await text(fs.root, 'nfsc/game/.stream-index');
  const error = await install(t, fs, server({ [ASSETS + 'GLOBAL/GLOBALB.BUN']: Array(5).fill('fail') }), { stream: true }).catch(e => e);
  assert.match(error.message, /could not be downloaded/);
  assert.equal(error.customizationOptional, true);
  assert.equal(await text(fs.root, 'nfsc/game/.stream-index'), before, 'the stream index still streams every original');
  assert.equal(await get(fs.root, 'nfsc/.customization-pending.json'), null);
  assert.equal(await get(fs.root, 'nfsc/.custom-install.json'), null);
});

test('a full import may start without customization only when no other version was installed into it', async t => {
  for (const foreign of [false, true]) {
    const fs = opfs(); await importedGame(fs);
    if (foreign) await put(fs.root, 'nfsc/.customization-backup-' + 'b'.repeat(64) + '/.originals.json', new TextEncoder().encode('{}'));
    const error = await install(t, fs, server({ [ASSETS + 'GLOBAL/GLOBALB.BUN']: Array(5).fill('fail') })).catch(e => e);
    assert.match(error.message, /could not be downloaded/);
    assert.equal(error.customizationOptional, !foreign, foreign ? 'older version present' : 'original files only');
    for (const [p, b] of Object.entries(ORIGINAL)) assert.deepEqual(await get(fs.root, 'nfsc/game/' + p), b, p);
  }
});

test('three cars failing in a row stop the install (likely offline) and leave a streamed game original', async t => {
  const fs = opfs(); await streamedGame(fs);
  const before = await text(fs.root, 'nfsc/game/.stream-index');
  const srv = server({ '*': url => /\/CARS\/(240SX|300C|350Z)\//.test(url) ? 'fail' : 'ok' });
  const error = await install(t, fs, srv, { stream: true }).catch(e => e);
  assert.match(error.message, /repeated failures/);
  assert.equal(error.customizationOptional, true);
  assert.equal(await text(fs.root, 'nfsc/game/.stream-index'), before);
  // Each car is tried with all its retries once, then the rest of the catalog is not downloaded.
  assert.equal(srv.calls.filter(c => c.url.includes('/CARS/')).length, 15);
  assert.ok(!srv.calls.some(c => c.url.endsWith('GLOBALB.BUN')));
});

test('an installed customization starts without the network from the saved catalog', async t => {
  const fs = opfs(); await streamedGame(fs);
  await install(t, fs, server(), { stream: true });
  await streamedGame(fs);
  const offline = server({ '*': () => 'fail' });
  assert.deepEqual(await install(t, fs, offline, { stream: true }), { state: 'installed', skipped: [] });
  assert.ok((await indexLines(fs)).includes('L\tGLOBAL/GLOBALB.BUN'));
});

test('a broken backup in a streamed game is rebuilt instead of stopping every start', async t => {
  const fs = opfs(); await streamedGame(fs);
  await put(fs.root, 'nfsc/game/' + CACHE.path, bytes(64, 3));
  await put(fs.root, 'nfsc/.customization-backup-' + VERSION + '/' + CACHE.path, bytes(64, 4)); // not what the record says
  await put(fs.root, 'nfsc/.customization-pending.json', new TextEncoder().encode(JSON.stringify({ schema: 1, version: VERSION,
    entries: [{ path: CACHE.path, existed: true, bytes: CACHE.data.length, sha256: hex(CACHE.data) }, { path: 'CARS/300C/GEOMETRY.BIN', existed: false, bytes: 0, sha256: null }] })));
  await put(fs.root, 'nfsc/.custom-stream-caches.json', new TextEncoder().encode(JSON.stringify({ version: VERSION })));
  assert.deepEqual(await install(t, fs, server(), { stream: true }), { state: 'installed', skipped: [] });
  assert.ok(carbonDiagnostics.customization.some(n => n.step === 'backup-unusable' && n.path === CACHE.path));
});

test('storage quota errors are not hidden as an optional add-on failure', async t => {
  const fs = opfs(); await streamedGame(fs);
  fs.onClose = where => { if (where.includes('.customization-stage-')) throw Object.assign(new Error('quota'), { name: 'QuotaExceededError' }); };
  const error = await install(t, fs, server(), { stream: true }).catch(e => e);
  assert.equal(error.name, 'QuotaExceededError');
  assert.equal(error.customizationOptional, false);
});
