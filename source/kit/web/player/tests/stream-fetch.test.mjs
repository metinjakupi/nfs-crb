import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { createHash, webcrypto } from 'node:crypto';

const source = (await readFile(new URL('../stream-fetch.js', import.meta.url), 'utf8'))
  .replaceAll('{{{ cDefs.EIO }}}', '5').replaceAll('{{{ cDefs.EROFS }}}', '30');
const size = 2 * 1048576 + 19;
const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 13 + 7) % 251);
async function setup(saved = new Map(), fault = '', rangeWait = () => {}, cacheWait = () => {}, indexed = false, cacheOpenWait = () => {}, assetPath = 'TRACKS/large.bun') {
  const requests = [], messages = [];
  const channels = [];
  const data = indexed ? Uint8Array.from({ length: 8 * 1048576 + 19 }, (_, i) => (i * 17 + 3) % 251) : bytes;
  const hash = value => createHash('sha256').update(value).digest('hex');
  const hashes = Array.from({ length: Math.ceil(data.length / 1048576) }, (_, i) => hash(data.slice(i * 1048576, (i + 1) * 1048576)));
  const pieces = [data.slice(0, 8 * 1048576), data.slice(8 * 1048576)];
  const pieceNames = pieces.map(hash);
  const index = { schema: 2, blockSize: 1048576, pieceSize: 8 * 1048576,
    base: '../files/', chunksBase: '../chunks/', files: {
      [assetPath]: { size: data.length, etag: 'v1', blocks: hashes, pieces: pieceNames },
    } };
  let library;
  const context = {
    Map, Number, String, Error, URL, Response, Uint8Array, AbortSignal, performance, crypto: webcrypto,
    setTimeout: fn => queueMicrotask(fn),
    withStackSave: fn => fn(), stringToUTF8OnStack: name => name,
    _getenv: name => indexed ? ({ RECOMP_WEB_ASSET_MANIFEST: 'https://assets.example/stream/index.json',
      RECOMP_WEB_ASSET_BASE: 'https://assets.example/files' })[name] || 0 : 0,
    console: { error() {}, warn() {} },
    BroadcastChannel: class { constructor() { channels.push(this); } postMessage(message) { messages.push(message); } },
    caches: { open: async () => { await cacheOpenWait(); return ({
      match: async key => saved.get(key)?.clone(),
      put: async (key, response) => { await cacheWait(); saved.set(key, response.clone()); },
    }); } },
    wasmFS$backends: {}, HEAPU8: new Uint8Array(2 * 1048576),
    UTF8ToString: p => p, __wasmfs_fetch_get_file_url: () => 'https://assets.example/files//' + assetPath,
    addToLibrary: value => { library = value; },
    fetch: async (url, options) => {
      requests.push({ url, ...options });
      if (url === 'https://assets.example/stream/index.json') return Response.json(index);
      if (indexed) {
        assert.notEqual(options.method, 'HEAD', 'the verified index supplies file metadata');
        const piece = pieceNames.findIndex(h => url === `https://assets.example/chunks/${h}.bin`);
        assert.ok(piece >= 0, 'a large asset must be fetched from independently cacheable pieces');
        const rangeRequest = requests.filter(r => r.headers).length;
        if (fault === 'network-once' && rangeRequest === 1) throw new TypeError('temporary network failure');
        const [, a, b] = /bytes=(\d+)-(\d+)/.exec(options.headers.Range);
        const start = Number(a), end = Number(b);
        assert.ok(end < pieces[piece].length, 'range must be relative to its piece');
        await rangeWait(piece * 8 * 1048576 + start);
        const payload = pieces[piece].slice(start, end + 1);
        if (fault === 'corrupt' || (fault === 'corrupt-once' && rangeRequest === 1)) payload[0] ^= 1;
        const body = (fault === 'body-always' || (fault === 'body-once' && rangeRequest === 1))
          ? new ReadableStream({ start(controller) {
              controller.enqueue(payload.slice(0, 10));
              controller.error(new TypeError('connection dropped after response headers'));
            } }) : payload;
        return new Response(body, { status: 206, headers: {
          'Content-Range': `bytes ${start}-${end}/${pieces[piece].length}`, ETag: 'piece-etag',
        } });
      }
      assert.equal(url, "https://assets.example/files/TRACKS/large.bun");
      if (options.method === 'HEAD' && fault === 'metadata') return new Response(null, { headers: { ETag: 'v1', 'Accept-Ranges': 'bytes' } });
      if (options.method === 'HEAD') return new Response(null, { headers: { 'Content-Length': String(size), 'ETag': 'v1', 'Accept-Ranges': 'bytes' } });
      const [, a, b] = /bytes=(\d+)-(\d+)/.exec(options.headers.Range);
      const start = Number(a), end = Number(b);
      await rangeWait(start);
      const short = fault === 'short' || (fault === 'prefetch-short' && start === 1048576);
      return new Response(bytes.slice(start, end + (short ? 0 : 1)), {
        status: fault === 'whole' ? 200 : 206,
        headers: { 'Content-Range': `bytes ${start}-${end}/${size}`, ETag: fault === 'version' ? 'v2' : 'v1' },
      });
    },
  };
  vm.runInNewContext(source, context);
  library._wasmfs_create_fetch_backend_js(1);
  return { api: context.wasmFS$backends[1], heap: context.HEAPU8, requests, saved, messages, channels, data };
}

test('WasmFS can allocate a file immediately while cache initialization is pending', async () => {
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const s = await setup(new Map(), '', () => {}, () => {}, true, () => gate);
  try {
    assert.ok(s.api, 'WasmFS calls allocFile synchronously after creating the backend');
    await s.api.allocFile(1);
    const read = s.api.read(1, 0, 20, 0);
    release();
    assert.equal(await read, 20);
    assert.deepEqual(s.heap.slice(0, 20), s.data.slice(0, 20));
  } finally { release(); }
});

test('listing a car file does not download its contents or start car preparation', async () => {
  const s = await setup(new Map(), '', () => {}, () => {}, true, () => {}, 'CARS/RX8/GEOMETRY.BIN');
  assert.equal(await s.api.getSize(1), s.data.length);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(s.requests.filter(r => r.headers).length, 0, 'metadata probes must not download every listed car');
});

test('reads across immutable piece boundaries without downloading metadata or the whole archive', async () => {
  const s = await setup(new Map(), '', () => {}, () => {}, true);
  assert.equal(await s.api.getSize(1), s.data.length);
  assert.equal(s.requests.length, 1, 'only the streaming index is needed for file size');
  const at = 8 * 1048576 - 9;
  assert.equal(await s.api.read(1, 0, 37, at), 28, 'read must stop at the actual EOF');
  assert.deepEqual(s.heap.slice(0, 28), s.data.slice(at));
  assert.equal(s.requests.filter(r => r.headers).length, 2);
});

test('rejects corrupted piece bytes and replaces a corrupted persistent block', async () => {
  const bad = await setup(new Map(), 'corrupt', () => {}, () => {}, true);
  assert.equal(await bad.api.read(1, 0, 20, 0), -5);
  assert.equal(bad.saved.size, 0);
  const saved = new Map([['https://assets.example/files/TRACKS/large.bun?recomp-block=0&recomp-version=v1',
    new Response(new Uint8Array(1048576))]]);
  const s = await setup(saved, '', () => {}, () => {}, true);
  assert.equal(await s.api.read(1, 0, 20, 0), 20);
  assert.deepEqual(s.heap.slice(0, 20), s.data.slice(0, 20));
  assert.equal(s.requests.filter(r => r.headers).length, 1, 'bad cached bytes must be fetched again');
});

test('retries a temporary network failure and a failed byte check before failing the game read', async () => {
  for (const fault of ['network-once', 'body-once', 'corrupt-once']) {
    const s = await setup(new Map(), fault, () => {}, () => {}, true);
    assert.equal(await s.api.read(1, 0, 20, 0), 20, fault);
    assert.deepEqual(s.heap.slice(0, 20), s.data.slice(0, 20), fault);
    assert.equal(s.requests.filter(r => r.headers).length, 2, `${fault} must retry once`);
    assert.equal(s.messages.some(m => m.type === 'unavailable'), false, fault);
  }
});

test('a permanently interrupted body stops after bounded retries without caching partial bytes', async () => {
  for (const path of ['TRACKS/large.bun', 'CARS/RX7/GEOMETRY.BIN']) {
    const s = await setup(new Map(), 'body-always', () => {}, () => {}, true, () => {}, path);
    assert.equal(await s.api.read(1, 0, 20, 0), -5);
    const ranges = s.requests.filter(r => r.headers);
    assert.equal(ranges.filter(r => r.headers.Range === 'bytes=0-1048575').length, 5);
    for (const range of new Set(ranges.map(r => r.headers.Range)))
      assert.ok(ranges.filter(r => r.headers.Range === range).length <= 5, 'each background or required range has bounded retries');
    assert.equal(s.saved.size, 0);
    assert.equal(s.messages.find(m => m.type === 'unavailable')?.path, '/files/' + path);
  }
});

test('an urgent game read proceeds while optional preparation downloads are stalled', async () => {
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const s = await setup(new Map(), '', start => start < 2 * 1048576 ? gate : undefined, () => {}, true);
  try {
    s.channels[0].onmessage({ data: { type: 'prefetch', paths: ['TRACKS/large.bun'] } });
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(s.requests.filter(r => r.headers).length, 2, 'two background requests are deliberately stalled');
    const read = s.api.read(1, 0, 20, 7 * 1048576);
    assert.equal(await Promise.race([read, new Promise(resolve => setTimeout(() => resolve('blocked'), 250))]), 20);
    assert.deepEqual(s.heap.slice(0, 20), s.data.slice(7 * 1048576, 7 * 1048576 + 20));
  } finally { release(); }
});

test('sequential read-ahead continues in memory after browser storage becomes full', async () => {
  const s = await setup(new Map(), '', () => {}, () => { throw new Error('QuotaExceededError'); });
  assert.equal(await s.api.read(1, 0, 20, 600000), 20);
  await new Promise(resolve => setImmediate(resolve));
  assert.ok(s.requests.some(r => r.headers?.Range === 'bytes=1048576-2097151'), 'full disk must not disable the next music block');
  assert.equal(await s.api.read(1, 0, 20, 1048576), 20);
  assert.deepEqual(s.heap.slice(0, 20), bytes.slice(1048576, 1048596));
});

test('returns verified game bytes while persistent storage is still busy', async () => {
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const s = await setup(new Map(), '', () => {}, () => gate);
  try {
    const read = s.api.read(1, 0, 20, 0);
    assert.equal(await Promise.race([read, new Promise(resolve => setTimeout(() => resolve('blocked'), 100))]), 20);
    assert.deepEqual(s.heap.slice(0, 20), bytes.slice(0, 20));
    assert.equal(s.saved.size, 0, 'the storage write is deliberately unfinished');
    assert.equal(await s.api.read(1, 0, 20, 0), 20, 'the verified memory copy is already reusable');
  } finally { release(); }
});

test('range reads cross block boundaries, return EOF, and reuse persistent blocks after restart', async () => {
  const first = await setup();
  assert.equal(await first.api.getSize(1), size);
  assert.equal(first.requests.length, 1, 'file size must not download file data');
  const at = 1048576 - 9;
  assert.equal(await first.api.read(1, 0, 37, at), 37);
  assert.deepEqual(first.heap.slice(0, 37), bytes.slice(at, at + 37));
  assert.equal(first.requests.filter(r => r.headers).length, 2);
  const second = await setup(first.saved);
  assert.equal(await second.api.read(1, 0, 37, at), 37);
  assert.deepEqual(second.heap.slice(0, 37), bytes.slice(at, at + 37));
  assert.equal(second.requests.filter(r => r.headers).length, 0, 'restart must use persistent data');
  assert.equal(await second.api.read(1, 0, 100, size - 5), 5);
  assert.deepEqual(second.heap.slice(0, 5), bytes.slice(size - 5));
  assert.equal(await second.api.read(1, 0, 10, size + 1), 0);
});

test('buffers the next sequential block without waiting for its response', async () => {
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const s = await setup(new Map(), '', start => start === 1048576 ? gate : undefined);
  try {
    assert.equal(await s.api.read(1, 0, 524288, 0), 524288);
    assert.deepEqual(s.heap.slice(0, 524288), bytes.slice(0, 524288));
    await new Promise(resolve => setImmediate(resolve));
    assert.ok(s.requests.some(r => r.headers?.Range === 'bytes=1048576-2097151'),
      'the next block should already be downloading while playback uses the first');
    const next = s.api.read(1, 0, 32, 1048576);
    release();
    assert.equal(await next, 32);
    assert.deepEqual(s.heap.slice(0, 32), bytes.slice(1048576, 1048608));
    assert.equal(s.requests.filter(r => r.headers?.Range === 'bytes=1048576-2097151').length, 1,
      'a foreground read must share the pending read-ahead request');
  } finally { release(); }
});

test('a failed read-ahead stays uncached and only a required read reports failure', async () => {
  const s = await setup(new Map(), 'prefetch-short');
  assert.equal(await s.api.read(1, 0, 524288, 0), 524288);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(s.messages.some(m => m.type === 'unavailable'), false);
  assert.equal(s.saved.size, 1, 'only the valid first block may be cached');
  assert.equal(await s.api.read(1, 0, 20, 1048576), -5);
  const failure = s.messages.find(m => m.type === 'unavailable');
  assert.equal(failure?.path, '/files/TRACKS/large.bun');
  assert.match(failure?.message || '', /byte range|byte check/);
  assert.equal(s.saved.size, 1);
});

for (const fault of ['short', 'whole', 'version', 'metadata']) {
  test(`rejects ${fault} responses without caching untrusted data`, async () => {
    const s = await setup(new Map(), fault);
    assert.equal(await s.api.read(1, 0, 20, 0), -5);
    assert.equal(s.saved.size, 0);
  });
}
