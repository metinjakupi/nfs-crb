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
      reads: 0, slowReads: 0, maxReadMs: 0, cacheWritesSkipped: 0, cacheFailures: 0 };
    const diagnostics = env('RECOMP_STREAM_STATS') === '1';
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
        cache.put(item.id, new Response(item.bytes)).catch(() => {
          storageFailed = true;
          ++stats.cacheFailures;
          writes.length = 0;
        }).finally(() => { --writesActive; pumpWrites(); pumpWarm(); });
      }
    }
    async function request(url, options, consume = response => response) {
      let error;
      for (let attempt = 0; attempt < 5; ++attempt) {
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
          if (!retryable || attempt === 4) break;
          await new Promise(resolve => setTimeout(resolve, 250 * 2 ** attempt));
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
    channel.onmessage = async ({ data }) => {
      await indexReady;
      if (data?.type === 'prefetch-models' && Array.isArray(data.keys)) {
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
              HEAPU8.set(bytes.subarray(from, from + count), buffer + done);
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
