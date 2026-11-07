// WasmFS fetch backend with bounded memory and persistent, versioned range caching.
// Downloads run on Emscripten's filesystem worker. The calling game thread still
// waits for a required read; speculative requests must never take its slots.
addToLibrary({
  _wasmfs_create_fetch_backend_js__deps: ['$wasmFS$backends', '_wasmfs_fetch_get_file_url',
    'getenv', '$withStackSave', '$stringToUTF8OnStack'],
  _wasmfs_create_fetch_backend_js: function(backend) {
    const blockSize = 1024 * 1024;
    const memory = new Map();
    const files = new Map();
    const pending = new Map();
    let memoryBytes = 0;
    let prefetches = 0;
    const channel = new BroadcastChannel('recomp-stream');
    // WasmFS calls allocFile as soon as this function returns. Install its
    // methods synchronously; only actual reads wait for optional setup.
    let cache = null;
    const cacheReady = caches.open('recomp-stream-v1').then(value => { cache = value; }).catch(() => {});
    const env = name => withStackSave(() => {
      const value = _getenv(stringToUTF8OnStack(name));
      return value ? UTF8ToString(value) : '';
    });
    const indexed = new Map(), paths = new Map(), models = new Map();
    const network = [], writes = [], warming = [], warmKeys = new Set();
    let networkActive = 0, backgroundActive = 0, writesActive = 0, storageFailed = false;
    let chunksBase = '', pieceSize = 8 * blockSize;
    const stats = { networkBytes: 0, networkRequests: 0, memoryHits: 0, diskHits: 0,
      reads: 0, slowReads: 0, maxReadMs: 0, cacheWritesSkipped: 0, cacheFailures: 0,
      cacheEntries: -1, cacheEvicted: 0, cacheSkippedForSpace: 0, storageFree: null };
    const diagnostics = env('RECOMP_STREAM_STATS') === '1';
    // The cache shares the site's storage quota with the game's own files and saves.
    // It may never use the last cacheReserve bytes of the quota, nor more than cacheCap
    // in total; the oldest entries (Cache Storage keeps insertion order) go first.
    const megabytes = (name, fallback) => {
      const value = Number(env(name));
      return (Number.isFinite(value) && value >= 0 && env(name) !== '' ? value : fallback) * 1048576;
    };
    const cacheReserve = megabytes('RECOMP_STREAM_CACHE_RESERVE_MB', 512);
    const cacheCapEntries = Math.floor(megabytes('RECOMP_STREAM_CACHE_CAP_MB', 2048) / blockSize);
    const evictBatch = 32;
    let cacheCount = -1, storageFree = null, estimatedAt = -Infinity, putsSinceEstimate = 0, spaceRetryAt = 0;
    let maintenance = Promise.resolve();
    const serial = fn => { const run = maintenance.then(fn); maintenance = run.catch(() => {}); return run; };
    async function estimateFree() {
      try {
        const e = await globalThis.navigator?.storage?.estimate?.();
        storageFree = Number.isFinite(e?.quota) && Number.isFinite(e?.usage) ? e.quota - e.usage : null;
      } catch { storageFree = null; }
      estimatedAt = performance.now(); putsSinceEstimate = 0; stats.storageFree = storageFree;
    }
    async function countCache() {
      if (cacheCount >= 0) return;
      try { cacheCount = cache.keys ? (await cache.keys()).length : 0; } catch { cacheCount = 0; }
      stats.cacheEntries = cacheCount;
    }
    // Delete the oldest entries until at most `keep` remain.
    async function evictTo(keep) {
      if (!cache.keys || !cache.delete || cacheCount <= keep) return;
      let keys;
      try { keys = await cache.keys(); } catch { return; }
      let left = keys.length;
      for (const key of keys.slice(0, Math.max(0, keys.length - Math.max(0, keep)))) {
        if (await cache.delete(key).catch(() => false)) { --left; ++stats.cacheEvicted; }
      }
      cacheCount = left; stats.cacheEntries = cacheCount;
    }
    // Serialized: may this block be stored without crossing the reserve or the cap?
    async function makeRoom(bytes) {
      await countCache();
      if (performance.now() < spaceRetryAt) return false;
      if (performance.now() - estimatedAt > 5000 || putsSinceEstimate >= evictBatch) await estimateFree();
      const short = () => Math.max(cacheCount + 1 - cacheCapEntries,
        storageFree === null ? 0 : Math.ceil((cacheReserve + bytes - storageFree) / blockSize));
      if (short() > 0) {
        await evictTo(cacheCount - short() - evictBatch);
        await estimateFree();
        if (short() > 0) {
          // Not enough room even with the cache emptied as far as allowed: try again later.
          ++stats.cacheSkippedForSpace;
          spaceRetryAt = performance.now() + 5000;
          return false;
        }
      }
      ++cacheCount; ++putsSinceEstimate; stats.cacheEntries = cacheCount;
      if (storageFree !== null) storageFree -= bytes;
      return true;
    }
    // Give space back if something else (game saves, another tab) used the quota. `full`: a
    // write was refused for space. Chrome's estimate() can report 10 GiB free while its real
    // limit is reached, so then the reserve is freed whatever the estimate says.
    const releaseSpace = (full = false) => serial(async () => {
      await countCache();
      if (cacheCount <= 0) return;
      await estimateFree();
      let excess = storageFree !== null && storageFree < cacheReserve
        ? Math.ceil((cacheReserve - storageFree) / blockSize) + evictBatch : 0;
      if (full) excess = Math.max(excess, Math.ceil(cacheReserve / blockSize));
      if (excess > 0) {
        await evictTo(cacheCount - excess);
        await estimateFree();
      }
    });
    cacheReady.then(() => {
      if (cache && typeof setInterval === 'function') setInterval(() => { releaseSpace().catch(() => {}); }, 10000);
    });
    // At most two background downloads; four slots remain available to game reads.
    function pumpNetwork() {
      while (networkActive < 6) {
        let at = network.findIndex(j => !j.background);
        if (at < 0 && backgroundActive < 2) at = network.findIndex(j => j.background);
        if (at < 0) break;
        const job = network.splice(at, 1)[0], background = job.background;
        ++networkActive;
        if (background) ++backgroundActive;
        job.run().then(job.resolve, job.reject).finally(() => {
          --networkActive;
          if (background) --backgroundActive;
          pumpNetwork();
        });
      }
    }
    function download(job, run) {
      return new Promise((resolve, reject) => {
        Object.assign(job, { run, resolve, reject });
        network.push(job);
        pumpNetwork();
      });
    }
    // Storage is a best-effort copy, not part of delivering bytes to the game.
    // Bound both outstanding writes and their buffers if storage is slow/full.
    function persist(id, bytes) {
      if (!cache || storageFailed || writes.length + writesActive >= 8) {
        ++stats.cacheWritesSkipped;
        return;
      }
      writes.push({ id, bytes });
      pumpWrites();
    }
    function pumpWrites() {
      while (!storageFailed && writesActive < 2 && writes.length) {
        const item = writes.shift();
        ++writesActive;
        serial(() => makeRoom(item.bytes.length)).then(room => {
          if (!room) { ++stats.cacheWritesSkipped; return; }
          return cache.put(item.id, new Response(item.bytes)).catch(error => {
            storageFailed = true;
            ++stats.cacheFailures;
            writes.length = 0;
            cacheCount = -1; // recount; the failed entry was not stored
            return releaseSpace(error?.name === 'QuotaExceededError' || /quota|space/i.test(String(error?.message || error)));
          });
        }).catch(() => {}).finally(() => { --writesActive; pumpWrites(); pumpWarm(); });
      }
    }
    async function request(url, options, consume = response => response) {
      let error;
      // 8 tries (player reports 2026-11-06: 24 of 26 "A game file could not be loaded" were Failed to fetch or a
      // timeout, and the 5 tries with 0.25-2 s between them gave up inside 4 s of a dropped connection). The waits
      // grow to 6 s, and while the browser says it is offline the next try waits for it (30 s at most).
      for (let attempt = 0; attempt < 8; ++attempt) {
        try {
          const response = await fetch(url, { ...options, redirect: 'error', signal: AbortSignal.timeout(20000) });
          if (!response.ok) {
            const failure = new Error(`HTTP ${response.status}`);
            failure.status = response.status;
            throw failure;
          }
          // Body reads can fail after fetch has delivered the response headers.
          return await consume(response);
        } catch (e) {
          error = e;
          const retryable = !e?.status || e.status === 408 || e.status === 425 || e.status === 429 || e.status >= 500;
          if (!retryable || attempt === 7) break;
          await new Promise(resolve => setTimeout(resolve, Math.min(6000, 250 * 2 ** attempt)));
          for (let waited = 0; typeof navigator !== 'undefined' && navigator.onLine === false && waited < 30000; waited += 500)
            await new Promise(resolve => setTimeout(resolve, 500));
        }
      }
      throw error;
    }
    const indexURL = env('RECOMP_WEB_ASSET_MANIFEST');
    const indexReady = (async () => {
      if (!indexURL) return;
      try {
        const index = await request(indexURL, {}, response => response.json());
        const base = new URL(index.base, indexURL), chunks = new URL(index.chunksBase, indexURL);
        if (index.schema !== 2 || index.blockSize !== blockSize || index.pieceSize !== 8 * blockSize ||
            base.href.replace(/\/$/, '') !== env('RECOMP_WEB_ASSET_BASE').replace(/\/$/, '') ||
            chunks.origin !== base.origin || !chunks.pathname.endsWith('/') || !index.files)
          throw new Error('Invalid streaming index');
        const prepared = [];
        for (const [path, entry] of Object.entries(index.files)) {
          if (!path || path.split('/').some(p => !p || p === '.' || p === '..') || /[\\?#%\r\n\t]/.test(path) ||
              !Number.isSafeInteger(entry.size) || entry.size < 0 || typeof entry.etag !== 'string' ||
              !Array.isArray(entry.blocks) || entry.blocks.length !== Math.ceil(entry.size / blockSize) ||
              !entry.blocks.every(h => /^[a-f0-9]{64}$/.test(h)) ||
              (entry.pieces && (!Array.isArray(entry.pieces) || entry.pieces.length !== Math.ceil(entry.size / pieceSize) ||
                !entry.pieces.every(h => /^[a-f0-9]{64}$/.test(h)))))
            throw new Error('Invalid asset in streaming index');
          const url = new URL(path.split('/').map(encodeURIComponent).join('/'), base).href;
          prepared.push({ ...entry, path, url, version: entry.etag });
        }
        chunksBase = chunks.href;
        for (const file of prepared) { indexed.set(file.url, file); paths.set(file.path, file); }
        for (const [key, prefix] of Object.entries(index.models || {})) {
          if (/^\d+$/.test(key) && /^CARS\/[^/]+\/$/.test(prefix) && prepared.some(f => f.path.startsWith(prefix)))
            models.set(key, prefix);
        }
      } catch (error) {
        // Old deployments and temporary index failures can still read the original files.
        console.warn('[stream] using original asset ranges:', String(error));
      }
    })();
    async function valid(bytes, expected, hash) {
      if (bytes.length !== expected) return false;
      if (!hash) return true; // legacy range mode validates the server's ETag instead
      const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
      return Array.from(digest, b => b.toString(16).padStart(2, '0')).join('') === hash;
    }
    async function info(file) {
      await indexReady;
      if (!files.has(file)) {
        const raw = UTF8ToString(__wasmfs_fetch_get_file_url(file));
        const address = new URL(raw);
        address.pathname = address.pathname.replace(/\/{2,}/g, '/');
        const url = address.href;
        if (indexed.has(url)) {
          const f = indexed.get(url);
          files.set(file, Promise.resolve(f));
          return f;
        }
        files.set(file, (async () => {
          const response = await request(url, { method: 'HEAD', cache: 'no-cache' });
          const sizeText = response.headers.get('Content-Length');
          const size = Number(sizeText);
          const version = response.headers.get('ETag');
          if (!/^\d+$/.test(sizeText || '') || !Number.isSafeInteger(size) || !version || response.headers.get('Accept-Ranges') !== 'bytes')
            throw new Error('Asset server must provide size, ETag and byte ranges');
          return { url, size, version };
        })().catch(e => { files.delete(file); throw e; }));
      }
      return files.get(file);
    }
    function cacheKey(f, first) {
      const key = new URL(f.url);
      key.searchParams.set('recomp-block', String(first));
      key.searchParams.set('recomp-version', f.version);
      return key.href;
    }
    async function block(f, first, background = false) {
      const start = first * blockSize, end = Math.min(f.size, start + blockSize) - 1;
      const id = cacheKey(f, first);
      if (memory.has(id)) {
        ++stats.memoryHits;
        const value = memory.get(id); memory.delete(id); memory.set(id, value); return value;
      }
      if (pending.has(id)) {
        const job = pending.get(id);
        if (!background) { job.background = false; pumpNetwork(); }
        return job.promise;
      }
      const job = { background };
      const work = (async () => {
        await cacheReady;
        let bytes;
        const saved = cache && await cache.match(id).catch(() => null);
        if (saved) {
          const value = new Uint8Array(await saved.arrayBuffer());
          if (await valid(value, end - start + 1, f.blocks?.[first])) {
            bytes = value;
            ++stats.diskHits;
          }
        }
        if (!bytes) {
          bytes = await download(job, async () => {
            channel.postMessage({ type: 'loading', busy: true });
            try {
              let url = f.url, from = start, to = end, total = f.size;
              if (f.pieces) {
                const piece = Math.floor(start / pieceSize);
                url = new URL(f.pieces[piece] + '.bin', chunksBase).href;
                from = start % pieceSize;
                to = from + end - start;
                total = Math.min(pieceSize, f.size - piece * pieceSize);
              }
              let invalid;
              for (let verifyAttempt = 0; verifyAttempt < 3; ++verifyAttempt) {
                const { response, value } = await request(url, { headers: { Range: `bytes=${from}-${to}` },
                  priority: job.background ? 'low' : 'high', cache: verifyAttempt ? 'reload' : 'default' }, async response => {
                    const expected = response.status === 206 && (f.pieces || response.headers.get('ETag') === f.version) &&
                      response.headers.get('Content-Range') === `bytes ${from}-${to}/${total}`;
                    // Do not read a whole-file response or an unrelated byte range into memory.
                    return { response, value: expected ? new Uint8Array(await response.arrayBuffer()) : null };
                  });
                if (response.status !== 206 || (!f.pieces && response.headers.get('ETag') !== f.version) ||
                    response.headers.get('Content-Range') !== `bytes ${from}-${to}/${total}`) {
                  invalid = new Error('Asset server returned the wrong byte range');
                } else {
                  if (await valid(value, end - start + 1, f.blocks?.[first])) {
                    stats.networkBytes += value.length;
                    ++stats.networkRequests;
                    persist(id, value);
                    return value;
                  }
                  invalid = new Error('Asset download failed its byte check');
                }
                if (verifyAttempt < 2) await new Promise(resolve => setTimeout(resolve, 250 * 2 ** verifyAttempt));
              }
              throw invalid;
            } finally { channel.postMessage({ type: 'loading', busy: false }); }
          });
        }
        memory.set(id, bytes); memoryBytes += bytes.length;
        while (memoryBytes > 64 * blockSize) {
          const oldest = memory.keys().next().value;
          memoryBytes -= memory.get(oldest).length; memory.delete(oldest);
        }
        return bytes;
      })();
      job.promise = work;
      pending.set(id, job);
      try { return await work; } finally { pending.delete(id); }
    }
    function readAhead(f, first) {
      // Imminent sequential data goes before optional car preparation. A busy
      // warm queue must not disable the music buffer that is already working.
      warm(f, first, true);
      pumpWarm();
    }
    function warm(f, first, front = false) {
      const id = cacheKey(f, first);
      if (first * blockSize >= f.size || memory.has(id) || pending.has(id)) return true;
      if (warmKeys.has(id)) {
        if (front) {
          const at = warming.findIndex(item => item.id === id);
          if (at >= 0) {
            warming[at].sequential = true;
            if (at > 0) warming.unshift(warming.splice(at, 1)[0]);
          }
        }
        return true;
      }
      if (warming.length >= 256) return false;
      warmKeys.add(id);
      warming[front ? 'unshift' : 'push']({ f, first, id, sequential: front });
      return true;
    }
    const warmedGroups = new Set();
    function warmGroup(prefix) {
      if (warmedGroups.has(prefix)) return;
      let complete = true;
      for (const f of paths.values()) {
        if (!f.path.startsWith(prefix)) continue;
        for (let i = 0; i * blockSize < f.size; ++i) if (!warm(f, i)) complete = false;
      }
      if (complete) warmedGroups.add(prefix);
      pumpWarm();
    }
    function pumpWarm() {
      // Optional warming stops if storage cannot retain its results. Urgent reads
      // and the two-block music buffer continue independently.
      while (prefetches < 2 && warming.length) {
        const at = storageFailed || writes.length + writesActive >= 6
          ? warming.findIndex(item => item.sequential) : 0;
        if (at < 0) break;
        const { f, first, id } = warming.splice(at, 1)[0];
        warmKeys.delete(id);
        ++prefetches;
        block(f, first, true).catch(() => {}).finally(() => { --prefetches; pumpWarm(); });
      }
    }
    const modelJobs = new Map();
    async function prepareModelReady(key) {
      const prefix = models.get(String(key));
      if (!prefix) return false;
      const group = [...paths.values()].filter(f => f.path.startsWith(prefix));
      if (!group.length) return false;
      if (!modelJobs.has(key)) modelJobs.set(key, (async () => {
        // One block at a time per model; never retain a whole model's buffers.
        // Background requests retain the same two-download concurrency limit.
        for (const f of group) for (let i = 0; i * blockSize < f.size; ++i)
          await block(f, i, true);
        return true;
      })().finally(() => modelJobs.delete(key)));
      return modelJobs.get(key);
    }
    channel.onmessage = async ({ data }) => {
      await indexReady;
      if (data?.type === 'prefetch-models' && Array.isArray(data.keys)) {
        if (typeof data.request === 'string' && data.request.length <= 80 && data.keys.length === 1) {
          const key = data.keys[0];
          let ready = false;
          try { ready = await prepareModelReady(key); } catch {}
          channel.postMessage({ type: 'model-ready', request: data.request, key, ready });
          return;
        }
        for (const key of data.keys.slice(0, 9)) {
          const prefix = models.get(String(key));
          if (prefix) warmGroup(prefix);
        }
        return;
      }
      if (data?.type !== 'prefetch' || !Array.isArray(data.paths)) return;
      for (const path of data.paths.slice(0, 32)) {
        const f = paths.get(path);
        if (f) for (let i = 0; i * blockSize < f.size && i < 64; ++i) warm(f, i);
      }
      pumpWarm();
    };
    if (diagnostics) setInterval(() => channel.postMessage({ type: 'stats', ...stats,
      queued: warming.length, networkActive, backgroundActive, memoryBytes }), 2000);
    function failed(error, file) {
      let path = '';
      try { path = new URL(UTF8ToString(__wasmfs_fetch_get_file_url(file))).pathname.replace(/\/{2,}/g, '/'); } catch {}
      console.error('[stream]', path, error);
      channel.postMessage({ type: 'unavailable', path, message: String(error) });
      return -{{{ cDefs.EIO }}};
    }
    wasmFS$backends[backend] = {
      allocFile: async () => {},
      freeFile: async file => { files.delete(file); },
      write: async () => -{{{ cDefs.EROFS }}},
      getSize: async file => { try { return (await info(file)).size; } catch (e) { failed(e, file); return 0; } },
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        const began = performance.now();
        try {
          const f = await info(file);
          // Stat/getSize also visits cars the player has not chosen. Prepare a
          // car only when its bytes are used or an active player names it.
          const car = /^CARS\/[^/]+\//.exec(f.path || '');
          if (car) warmGroup(car[0]);
          length = Math.max(0, Math.min(length, f.size - offset));
          let done = 0;
          while (done < length) {
            const first = Math.floor((offset + done) / blockSize);
            const last = Math.min(first + 3, Math.floor((offset + length - 1) / blockSize));
            // Bound concurrent downloads and temporary buffers, even for a whole-file read.
            const blocks = await Promise.all(Array.from({ length: last - first + 1 }, (_, i) => block(f, first + i)));
            for (const bytes of blocks) {
              const from = (offset + done) % blockSize;
              const count = Math.min(bytes.length - from, length - done);
              const artworkURL = env('RECOMP_BILLBOARD_OVERLAY');
              const delivered = artworkURL
                ? await (await import(artworkURL)).overlay(f, offset + done, bytes.subarray(from, from + count))
                : bytes.subarray(from, from + count);
              HEAPU8.set(delivered, buffer + done);
              done += count;
            }
          }
          // Buffer one block ahead once a sequential reader has consumed half
          // of its current block. In particular, music must not stop the game
          // every time its decoder reaches another uncached megabyte.
          const end = offset + done;
          if (done && (end % blockSize === 0 || end % blockSize >= blockSize / 2))
            readAhead(f, Math.floor((end - 1) / blockSize) + 1);
          const elapsed = performance.now() - began;
          ++stats.reads;
          stats.maxReadMs = Math.max(stats.maxReadMs, elapsed);
          if (elapsed >= 50) {
            ++stats.slowReads;
            if (diagnostics) channel.postMessage({ type: 'read-wait', path: f.path || new URL(f.url).pathname,
              offset, length: done, elapsed });
          }
          return done;
        } catch (e) { return failed(e, file); }
      },
    };
  },
});
