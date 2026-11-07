// worker.js - imports, status and saves in the origin private file system
// (OPFS), off the page's thread. Layout: /<game id>/game/... (the game, its
// .manifest.json and .stamp) and /<game id>/profile/... (saves and settings).
import { planImport, importGame, gameStatus, ZipReader, zipWrite, Sha256 } from "./core.js";

let controller = null;

async function dirFor(path, create = true) {
  let dir = await navigator.storage.getDirectory();
  for (const part of path.split("/").filter(Boolean))
    dir = await dir.getDirectoryHandle(part, { create });
  return dir;
}

async function fileFor(root, rel, create) {
  const parts = rel.split("/");
  const name = parts.pop();
  let dir = root;
  for (const p of parts) dir = await dir.getDirectoryHandle(p, { create });
  return dir.getFileHandle(name, { create });
}

function opfsStore(root) {
  return {
    async readText(rel) {
      try {
        return await (await (await fileFor(root, rel, false)).getFile()).text();
      } catch {
        return null;
      }
    },
    async writeText(rel, text) {
      const handle = await fileFor(root, rel, true);
      const access = await handle.createSyncAccessHandle();
      const bytes = new TextEncoder().encode(text);
      try {
        access.truncate(0);
        // A short write (storage full) is not an exception; a cut-off file must not stay behind.
        if (access.write(bytes, { at: 0 }) !== bytes.length) throw new Error("The browser could not store the whole file (storage full)");
        access.flush();
      } catch (e) {
        try { access.truncate(0); } catch {}
        throw e;
      } finally { access.close(); }
    },
    async remove(rel) {
      const parts = rel.split("/");
      const name = parts.pop();
      try {
        let dir = root;
        for (const p of parts) dir = await dir.getDirectoryHandle(p);
        await dir.removeEntry(name);
      } catch {}
    },
    async hash(rel) {
      const file = await (await fileFor(root, rel, false)).getFile();
      const sha = new Sha256();
      const reader = file.stream().getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        sha.update(value);
      }
      return sha.hex();
    },
    // The manifest, written only after close, is what marks a file complete,
    // so a file cut short is never trusted and is simply written again.
    async createWriter(rel) {
      const handle = await fileFor(root, rel, true);
      const access = await handle.createSyncAccessHandle();
      access.truncate(0);
      let at = 0;
      return {
        async write(bytes) {
          const n = access.write(bytes, { at });
          if (n !== bytes.length) throw new DOMException("short write", "QuotaExceededError");
          at += n;
        },
        async close() {
          access.flush();
          access.close();
        },
        async abort() {
          try { access.close(); } catch {}
        },
      };
    },
  };
}

// Sources ---------------------------------------------------------------
function filesSource(files) {
  // [{path, file}] from a folder input or a drop.
  const entries = files.map(({ path, file }) => ({
    path: path.replaceAll("\\", "/").replace(/^\/+/, ""),
    size: file.size,
    mtime: Math.floor((file.lastModified || 0) / 1000),
    isDir: false,
    file,
  }));
  return { list: async () => entries, stream: async (e) => e.file.stream() };
}

async function handleSource(dirHandle) {
  const entries = [];
  const walk = async (dir, prefix) => {
    for await (const [name, h] of dir.entries()) {
      const path = prefix ? prefix + "/" + name : name;
      if (h.kind === "directory") {
        entries.push({ path, size: 0, mtime: 0, isDir: true });
        await walk(h, path);
      } else {
        const file = await h.getFile();
        entries.push({ path, size: file.size, mtime: Math.floor(file.lastModified / 1000), isDir: false, file });
      }
    }
  };
  await walk(dirHandle, dirHandle.name);
  return { list: async () => entries, stream: async (e) => e.file.stream() };
}

async function zipSource(file) {
  const zip = await ZipReader.open(file);
  return { list: async () => zip.entries(), stream: (e) => zip.stream(e) };
}

// Retry transient connection/server failures before opening an import writer.
// A failed response body still aborts the import and is resumed on the next Play.
async function downloadResponse(url, options) {
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      const response = await fetch(url, options);
      if (response.ok) return response;
      const error = new Error(`Download returned HTTP ${response.status}.`);
      error.status = response.status;
      error.retryable = response.status === 408 || response.status === 429 || response.status >= 500;
      await response.body?.cancel();
      throw error;
    } catch (error) {
      const context = { url: url.origin + url.pathname, attempt, status: error.status ?? null };
      error.context = context;
      postMessage({ diagnostic: { phase: "download", ...context, name: error.name, message: error.message } });
      if (options.signal?.aborted || error.name === "AbortError" || error.retryable === false || attempt === 3) throw error;
      await new Promise(resolve => setTimeout(resolve, 500 * attempt));
    }
  }
}

// A private development server can expose an installed copy beside the launcher.
async function installedSource(manifest, signal, remote = false) {
  const url = new URL(manifest, self.location.href);
  if (!remote && url.origin !== self.location.origin) throw new Error("Installed game must use this server");
  if (remote && url.protocol !== "https:") throw new Error("Game downloads require HTTPS");
  const response = await downloadResponse(url, { signal, redirect: "error", cache: "no-cache" });
  if (!response.ok) throw new Error(`Game manifest: HTTP ${response.status}`);
  const { files, base, stream } = await response.json();
  const prefix = new URL(base, url);
  if (prefix.origin !== url.origin) throw new Error("Invalid installed game location");
  const streamManifest = stream ? new URL(stream, url) : null;
  if (streamManifest && streamManifest.origin !== url.origin) throw new Error("Invalid streaming index location");
  const entries = files.map(e => {
    if (!Number.isSafeInteger(e.size) || e.size < 0 || !e.path || e.path.split("/").some(p => !p || p === "." || p === "..") || e.path.includes("\\"))
      throw new Error("Invalid installed game path");
    return { ...e, isDir: false };
  });
  return { base: prefix.href.replace(/\/$/, ""), streamManifest: streamManifest?.href || "", list: async () => entries, stream: async e => {
    const file = new URL(e.path.split("/").map(encodeURIComponent).join("/"), prefix);
    const response = await downloadResponse(file, { signal, redirect: "error" });
    if (!response.ok) throw new Error(`${e.path}: HTTP ${response.status}`);
    let received = 0;
    return response.body.pipeThrough(new TransformStream({
      transform(chunk, controller) {
        received += chunk.byteLength;
        if (received > e.size) throw new Error(`${e.path}: file is larger than the manifest`);
        controller.enqueue(chunk);
      },
      flush() {
        if (received !== e.size) throw new Error(`${e.path}: incomplete download`);
      },
    }));
  }};
}

async function freeBytes() {
  try {
    const { quota, usage } = await navigator.storage.estimate();
    return quota - usage;
  } catch {
    return undefined;
  }
}

// The runtime's streamed-file cache (kit/web/player/stream-fetch.js). It is only a copy
// of downloadable bytes: at startup, entries for another asset location or another file
// version are removed, and the newest entries are kept only while at least `wantFree`
// bytes of the quota stay free for the game's own files and saves.
const STREAM_CACHE = "recomp-stream-v1";
const STREAM_CACHE_RESERVE = 512 * 1024 * 1024;
async function streamIndexVersions(streamManifest, base) {
  if (!streamManifest) return null;
  try {
    const response = await fetch(streamManifest, { redirect: "error", cache: "no-cache" });
    if (!response.ok) return null;
    const index = await response.json();
    const files = new URL(index.base, streamManifest);
    if (files.href.replace(/\/$/, "") !== base.replace(/\/$/, "") || !index.files) return null;
    const versions = new Map();
    for (const [path, entry] of Object.entries(index.files))
      versions.set(new URL(path.split("/").map(encodeURIComponent).join("/"), files).href, String(entry.etag));
    return versions;
  } catch { return null; }
}
async function tidyStreamCache({ base, streamManifest = "", wantFree = STREAM_CACHE_RESERVE } = {}) {
  const report = { entries: 0, removedOtherLocation: 0, removedOtherVersion: 0, evictedForSpace: 0, freeBefore: null, freeAfter: null };
  const free = async () => {
    try { const { quota, usage } = await navigator.storage.estimate(); return Number.isFinite(quota - usage) ? quota - usage : null; }
    catch { return null; }
  };
  report.freeBefore = await free();
  if (!self.caches || !await caches.has(STREAM_CACHE).catch(() => false)) { report.freeAfter = report.freeBefore; return report; }
  const cache = await caches.open(STREAM_CACHE);
  const keys = await cache.keys();
  report.entries = keys.length;
  const prefix = base ? base.replace(/\/$/, "") + "/" : null;
  const versions = prefix ? await streamIndexVersions(streamManifest, base) : null;
  const kept = [];
  for (const request of keys) {
    let file, version;
    try { const u = new URL(request.url); version = u.searchParams.get("recomp-version"); u.search = ""; file = u.href; } catch {}
    if (!prefix || !file || !file.startsWith(prefix)) {
      if (await cache.delete(request)) report.removedOtherLocation++;
    } else if (versions && versions.has(file) && versions.get(file) !== version) {
      if (await cache.delete(request)) report.removedOtherVersion++;
    } else kept.push(request);
  }
  // Keys come back oldest first; evict in batches, re-measuring after each.
  let left = await free();
  while (left !== null && left < wantFree && kept.length) {
    const batch = kept.splice(0, Math.min(kept.length, Math.max(16, Math.ceil((wantFree - left) / 1048576))));
    for (const request of batch) if (await cache.delete(request)) report.evictedForSpace++;
    left = await free();
  }
  if (!kept.length && report.entries) await caches.delete(STREAM_CACHE).catch(() => {});
  report.freeAfter = left;
  return report;
}

async function listFiles(dir, prefix, out) {
  for await (const [name, h] of dir.entries()) {
    const path = prefix ? prefix + "/" + name : name;
    if (h.kind === "directory") await listFiles(h, path, out);
    else out.push({ path, handle: h });
  }
  return out;
}

// Messages --------------------------------------------------------------
const handlers = {
  async status({ game }) {
    const root = await dirFor(game.id + "/game");
    const status = await gameStatus(game, opfsStore(root));
    const estimate = await navigator.storage.estimate().catch(() => ({}));
    const persisted = await navigator.storage.persisted?.().catch(() => false);
    return { status, estimate, persisted };
  },
  async import({ game, kind, files, handle, file }) {
    controller = new AbortController();
    let source;
    if (kind === "files") source = filesSource(files);
    else if (kind === "handle") source = await handleSource(handle);
    else if (kind === "installed") source = await installedSource(game.localManifest, controller.signal);
    else if (kind === "download") source = await installedSource(game.downloadManifest, controller.signal, true);
    else source = await zipSource(file);
    const root = await dirFor(game.id + "/game");
    const outcome = await importGame(game, source, opfsStore(root), {
      signal: controller.signal,
      freeBytes: await freeBytes(),
      onProgress: (p) => postMessage({ progress: p }),
    });
    controller = null;
    return outcome;
  },
  async streamSetup({ game }) {
    controller = new AbortController();
    try {
      const source = await installedSource(game.downloadManifest, controller.signal, true);
      const plan = planImport(game, await source.list());
      if (plan.error || plan.missing.length) throw new Error("The game manifest is incomplete");
      if (plan.files.some(f => /[\r\n\t?#%]/.test(f.relative))) throw new Error("Unsupported asset path");
      const pinned = f => f.relative === plan.exe || (game.auxiliaryFiles || []).some(m => m.name.toLowerCase() === f.relative.toLowerCase());
      const root = await dirFor(game.id + "/game");
      const store = opfsStore(root);
      // Before measuring free space: the streamed-file cache yields to the game's files and saves
      // (the same 512 MB reserve the runtime keeps free; at least what the import below checks for).
      const pinnedBytes = plan.files.filter(pinned).reduce((sum, f) => sum + f.size, 0);
      const cacheTidy = await tidyStreamCache({ base: source.base, streamManifest: source.streamManifest,
        wantFree: Math.max(STREAM_CACHE_RESERVE, pinnedBytes + (game.minFreeMb || 0) * 1024 * 1024) }).catch(e => ({ error: String(e?.message || e) }));
      postMessage({ diagnostic: { phase: "stream-cache", ...cacheTidy } });
      const boot = { list: async () => plan.files.filter(pinned).map(f => f.entry), stream: e => source.stream(e) };
      // Startup verification must never publish a full-install stamp, even if the tab closes.
      const startupStore = { ...store, writeText: (path, text) => path === ".stamp" ? Promise.resolve() : store.writeText(path, text) };
      const outcome = await importGame({ ...game, requiredDirs: [] }, boot, startupStore, {
        signal: controller.signal, freeBytes: await freeBytes(),
        onProgress: p => postMessage({ progress: p, startup: true }),
      });
      await store.remove(".stamp");
      if (outcome.result !== "done") return outcome;
      await store.writeText(".stream-source", source.base);
      await store.writeText(".stream-index", plan.files.map(f => `${pinned(f) ? "L" : "R"}\t${f.relative}`).join("\n") + "\n");
      return { result: "done", base: source.base, manifest: source.streamManifest, index: `/opfs/${game.id}/game/.stream-index`, cacheTidy };
    } finally { controller = null; }
  },
  async cancel() {
    if (controller) controller.abort();
    return {};
  },
  async delete({ game }) {
    const dir = await dirFor(game.id);
    const gameDir = await dirFor(game.id + "/game");
    const base = await opfsStore(gameDir).readText(".stream-source");
    if (base) {
      const cache = await caches.open("recomp-stream-v1");
      for (const request of await cache.keys())
        if (request.url.startsWith(base.replace(/\/$/, "") + "/")) await cache.delete(request);
    }
    await dir.removeEntry("game", { recursive: true }).catch(() => {});
    return {};
  },
  async exportSaves({ game }) {
    const dir = await dirFor(game.id + "/profile");
    const files = [];
    for (const { path, handle } of await listFiles(dir, "", [])) {
      const f = await handle.getFile();
      files.push({ name: path, data: new Uint8Array(await f.arrayBuffer()), mtime: Math.floor(f.lastModified / 1000) });
    }
    return { blob: zipWrite(files), count: files.length };
  },
  async importSaves({ game, file }) {
    const zip = await ZipReader.open(file);
    const root = await dirFor(game.id + "/profile");
    const store = opfsStore(root);
    let count = 0;
    for (const e of zip.entries()) {
      if (e.isDir || e.path.split("/").some((p) => p === "..")) continue;
      const w = await store.createWriter(e.path);
      const reader = (await zip.stream(e)).getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        await w.write(value);
      }
      await w.close();
      count++;
    }
    return { count };
  },
};

onmessage = async ({ data }) => {
  const { id, cmd } = data;
  try {
    const result = await handlers[cmd](data);
    postMessage({ id, result });
  } catch (e) {
    const cancelled = cmd === "import" && controller?.signal.aborted;
    if (cmd === "import") controller = null;
    postMessage(cancelled
      ? { id, result: { result: "cancelled", copied: 0, skipped: 0 } }
      : { id, error: String((e && e.message) || e), name: String(e?.name || "Error"),
          stack: String(e?.stack || "").slice(0, 5000), context: { command: cmd, ...e?.context } });
  }
};
