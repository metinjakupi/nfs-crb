// Startup cleanup of the streamed-file cache (source/kit/web/launcher/worker.js tidyStreamCache).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import * as core from '../../source/kit/web/launcher/core.js';

const source = await readFile(process.env.CARBON_WORKER_SOURCE || new URL('../../source/kit/web/launcher/worker.js', import.meta.url), 'utf8');
const MiB = 1048576;
const BASE = 'https://assets.example/local-game/files';
function load({ quota, other, entries, index = null }) {
  const saved = new Map(entries.map(url => [url, MiB]));
  let deleted = false;
  const cache = { keys: async () => [...saved.keys()].map(url => ({ url })), delete: async r => saved.delete(r.url) };
  const c = { ...core, URL, Response, Error, Map, Math, Number, String, Object, encodeURIComponent,
    self: { location: new URL('https://site.example/play/worker.js'), caches: true },
    caches: { has: async () => !deleted && saved.size > 0, open: async () => cache, delete: async () => { deleted = true; return true; } },
    navigator: { storage: { estimate: async () => ({ quota: quota * MiB, usage: other * MiB + [...saved.values()].reduce((a, b) => a + b, 0) }) } },
    postMessage() {}, setTimeout: fn => queueMicrotask(fn),
    fetch: async url => index && url === 'https://assets.example/local-game/stream/index.json' ? Response.json(index) : new Response('', { status: 404 }) };
  vm.createContext(c);
  vm.runInContext(source.replace(/^import .* from "\.\/core.js";\r?\n/m, ''), c);
  return { c, saved, deleted: () => deleted };
}
const key = (base, path, block, version = 'v1') => `${base}/${path}?recomp-block=${block}&recomp-version=${version}`;

test('entries of another asset location are removed; current ones are kept while space allows', async () => {
  const old = Array.from({ length: 40 }, (_, i) => key('https://old.example/files', 'TRACKS/A.BUN', i));
  const now = Array.from({ length: 10 }, (_, i) => key(BASE, 'TRACKS/A.BUN', i));
  const s = load({ quota: 2048, other: 1100, entries: [...old, ...now] });
  const r = await s.c.tidyStreamCache({ base: BASE, wantFree: 600 * MiB });
  assert.equal(r.removedOtherLocation, 40);
  assert.equal(r.evictedForSpace, 0);
  assert.deepEqual([...s.saved.keys()], now);
});

test('entries of another file version are removed when the stream index is known', async () => {
  const index = { schema: 2, base: '../files/', files: { 'TRACKS/A.BUN': { etag: 'v2' }, 'CARS/X Y/G.BIN': { etag: 'v1' } } };
  const entries = [key(BASE, 'TRACKS/A.BUN', 0, 'v1'), key(BASE, 'TRACKS/A.BUN', 1, 'v2'), key(BASE, 'CARS/X%20Y/G.BIN', 0, 'v1')];
  const s = load({ quota: 4096, other: 0, entries, index });
  const r = await s.c.tidyStreamCache({ base: BASE, streamManifest: 'https://assets.example/local-game/stream/index.json' });
  assert.equal(r.removedOtherVersion, 1);
  assert.deepEqual([...s.saved.keys()], entries.slice(1));
});

test('with too little free space the oldest current entries are evicted until the game has room', async () => {
  // Brave-like 2 GiB quota: 1.1 GB of game files, 0.9 GB of cache, 48 MB free.
  const now = Array.from({ length: 900 }, (_, i) => key(BASE, 'TRACKS/A.BUN', i));
  const s = load({ quota: 2048, other: 1100, entries: now });
  const r = await s.c.tidyStreamCache({ base: BASE, wantFree: (300 + 512 + 8) * MiB });
  assert.ok(r.freeAfter >= (300 + 512 + 8) * MiB, `free after ${r.freeAfter / MiB} MiB`);
  assert.equal(r.evictedForSpace, 900 - s.saved.size);
  assert.equal([...s.saved.keys()][0], now[900 - s.saved.size], 'the oldest entries went first');
});

test('an empty or missing cache is a no-op', async () => {
  const s = load({ quota: 100, other: 99, entries: [] });
  const r = await s.c.tidyStreamCache({ base: BASE });
  assert.equal(r.entries, 0);
  assert.equal(s.deleted(), false);
});

test('a cache emptied completely is deleted', async () => {
  const s = load({ quota: 1024, other: 0, entries: [key('https://old.example/files', 'A', 0)] });
  await s.c.tidyStreamCache({ base: BASE });
  assert.equal(s.deleted(), true);
});
