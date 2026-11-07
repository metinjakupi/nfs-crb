// The streamed-file cache shares the site's storage quota with the game's files and saves:
// it must stay under its cap, never use the last reserve of the quota, and give space back.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { webcrypto } from 'node:crypto';

const MiB = 1048576;
const source = (await readFile(new URL('../stream-fetch.js', import.meta.url), 'utf8'))
  .replaceAll('{{{ cDefs.EIO }}}', '5').replaceAll('{{{ cDefs.EROFS }}}', '30');
const blocks = 40, size = blocks * MiB;
const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 13 + 7) % 251);
const URL_ = 'https://assets.example/files/TRACKS/large.bun';
const key = n => `${URL_}?recomp-block=${n}&recomp-version=v1`;

// quota: total quota in MiB; other: MiB used by everything except this cache (OPFS game files, saves).
async function setup({ quota = 1 << 20, other = 0, env = {}, saved = new Map(), failPut = () => false } = {}) {
  const usage = { other: other * MiB };
  let tick = null, library;
  const cacheBytes = () => [...saved.values()].reduce((sum, v) => sum + v.size, 0);
  const cache = {
    match: async k => { const v = saved.get(k); return v && new Response(v.data); },
    put: async (k, response) => {
      const data = new Uint8Array(await response.arrayBuffer());
      if (failPut() || usage.other + cacheBytes() + data.length > quota * MiB)
        throw new DOMException('quota', 'QuotaExceededError');
      saved.delete(k); saved.set(k, { data, size: data.length }); // insertion order, like Cache Storage
    },
    keys: async () => [...saved.keys()].map(url => ({ url })),
    delete: async request => saved.delete(request.url ?? request),
  };
  const context = {
    Map, Set, Number, String, Error, URL, Response, Uint8Array, AbortSignal, Promise, DOMException, Math, Array, Object,
    performance, crypto: webcrypto, setTimeout: fn => queueMicrotask(fn), setInterval: fn => { tick = fn; },
    navigator: { storage: { estimate: async () => ({ quota: quota * MiB, usage: usage.other + cacheBytes() }) } },
    withStackSave: fn => fn(), stringToUTF8OnStack: name => name,
    _getenv: name => env[name] ?? 0,
    console: { error() {}, warn() {} },
    BroadcastChannel: class { postMessage() {} },
    caches: { open: async () => cache },
    wasmFS$backends: {}, HEAPU8: new Uint8Array(4 * MiB),
    UTF8ToString: p => p, __wasmfs_fetch_get_file_url: () => URL_,
    addToLibrary: value => { library = value; },
    fetch: async (url, options) => {
      if (options.method === 'HEAD') return new Response(null, { headers: { 'Content-Length': String(size), ETag: 'v1', 'Accept-Ranges': 'bytes' } });
      const [, a, b] = /bytes=(\d+)-(\d+)/.exec(options.headers.Range);
      return new Response(bytes.slice(+a, +b + 1), { status: 206, headers: { 'Content-Range': `bytes ${a}-${b}/${size}`, ETag: 'v1' } });
    },
  };
  context.globalThis = context;
  vm.runInNewContext(source, context);
  library._wasmfs_create_fetch_backend_js(1);
  const api = context.wasmFS$backends[1];
  const settle = async () => { for (let i = 0; i < 50; i++) await new Promise(r => setImmediate(r)); };
  // Read every block once (a required read each), letting the cache writes finish in between.
  const readAll = async (from = 0, to = blocks) => {
    for (let n = from; n < to; n++) { assert.equal(await api.read(1, 0, 16, n * MiB), 16); await settle(); }
  };
  const free = () => quota * MiB - usage.other - cacheBytes();
  return { api, saved, usage, free, readAll, settle, tick: () => tick?.() };
}

test('the cache never takes the last 512 MB of the quota and evicts its oldest blocks first', async () => {
  // 600 MB quota, 70 MB used by game files: the cache may hold at most about 18 MB.
  const s = await setup({ quota: 600, other: 70 });
  await s.readAll();
  assert.ok(s.saved.size > 0, 'some blocks are cached');
  assert.ok(s.free() >= 512 * MiB - MiB, `free ${s.free() / MiB} MiB must stay at the reserve`);
  const kept = [...s.saved.keys()].map(k => +new URL(k).searchParams.get('recomp-block'));
  assert.deepEqual(kept, [...kept].sort((a, b) => a - b), 'remaining blocks are the newest, in order');
  assert.ok(kept.at(-1) >= blocks - 2, 'the newest blocks are kept');
  assert.ok(!kept.includes(0), 'the oldest block was evicted');
});

test('a hard cap bounds the cache when the quota is large', async () => {
  const s = await setup({ env: { RECOMP_STREAM_CACHE_CAP_MB: '8' } });
  await s.readAll();
  assert.ok(s.saved.size <= 8, `cache holds ${s.saved.size} blocks, cap 8`);
  assert.ok(s.saved.size >= 1);
});

test('with less than the reserve free, nothing is written and the game still gets its bytes', async () => {
  const s = await setup({ quota: 600, other: 300 });
  await s.readAll(0, 6);
  assert.equal(s.saved.size, 0);
});

test('old entries are evicted when something else fills the quota during play (game saves keep room)', async () => {
  const saved = new Map();
  for (let n = 0; n < 30; n++) saved.set(`https://assets.example/files/OLD/x.bin?recomp-block=${n}&recomp-version=v0`, { data: new Uint8Array(0), size: MiB });
  const s = await setup({ quota: 1024, other: 400, saved });
  await s.settle();
  assert.equal(s.free(), (1024 - 400 - 30) * MiB);
  s.usage.other += 100 * MiB; // the game writes 100 MB: free is now 494 MB, under the reserve
  await s.tick(); await s.settle();
  assert.ok(s.free() >= 512 * MiB, `free ${s.free() / MiB} MiB after the periodic check`);
  assert.ok(s.saved.size < 30 && s.saved.size >= 0);
  assert.ok(!s.saved.has('https://assets.example/files/OLD/x.bin?recomp-block=0&recomp-version=v0'), 'oldest first');
});

test('a failed write frees space down to the reserve and stops further caching', async () => {
  const saved = new Map();
  for (let n = 0; n < 20; n++) saved.set(`https://assets.example/files/OLD/y.bin?recomp-block=${n}&recomp-version=v0`, { data: new Uint8Array(0), size: MiB });
  // The estimate says there is room, but the write fails (another writer took it meanwhile).
  let fail = true;
  const s = await setup({ quota: 1024, other: 500, saved, failPut: () => fail });
  await s.readAll(0, 1);
  // free was 504 MB (< 512 + 1): the room check already evicts; then the put fails and space is released.
  assert.ok(s.free() >= 512 * MiB - MiB, `free ${s.free() / MiB} MiB`);
  fail = false;
  const before = s.saved.size;
  await s.readAll(1, 4);
  assert.equal(s.saved.size, before, 'no writes after a storage failure in this session');
});

test('a write refused for space frees the reserve even when estimate() reports plenty (Chrome)', async () => {
  const saved = new Map();
  for (let n = 0; n < 700; n++) saved.set(`https://assets.example/files/OLD/z.bin?recomp-block=${n}&recomp-version=v0`, { data: new Uint8Array(0), size: MiB });
  // Real limit: 1000 MB, reached. estimate() claims 10 GiB more (what Chrome 157 reports).
  const s = await setup({ quota: 10240 + 1000, other: 300, saved, failPut: () => true });
  await s.readAll(0, 1);
  assert.ok(s.saved.size <= 700 - 512, `${s.saved.size} entries left; 512 MB must be released`);
  assert.ok(!s.saved.has('https://assets.example/files/OLD/z.bin?recomp-block=0&recomp-version=v0'), 'oldest first');
});
