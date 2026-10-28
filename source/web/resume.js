// Small per-tab refresh checkpoints. Career files remain the source of progress.
// Never save credentials, game memory, or an online race's previous state here.
"use strict";
(() => {
  const MAGIC = 0x31525343, MAX_AGE = 12 * 60 * 60 * 1000;
  const pointOK = p => p && Number.isInteger(p.id) && p.id > 0 && p.id <= 0xffffffff &&
    Number.isInteger(p.model) && p.model > 0 && p.model <= 0xffffffff &&
    Number.isInteger(p.track) && p.track > 0 && p.track <= 0xffffffff &&
    Array.isArray(p.name) && p.name.length > 0 && p.name.length <= 31 &&
    p.name.every(x => Number.isInteger(x) && x > 0 && x < 256) &&
    Array.isArray(p.p) && p.p.length === 3 && p.p.every(x => Number.isFinite(x) && Math.abs(x) < 100000) &&
    Array.isArray(p.q) && p.q.length === 4 && p.q.every(Number.isFinite) &&
    p.q.reduce((n, x) => n + x * x, 0) > .81 && p.q.reduce((n, x) => n + x * x, 0) < 1.21;
  const configOK = c => c && typeof c.online === "boolean" &&
    ["name", "room", "crew", "server"].every(k => typeof c[k] === "string" && c[k].length <= 128);

  class CarbonRecovery {
    constructor(game) {
      this.game = game;
      this.key = `${game.id}.recovery.v1`;
      this.slotKey = `${game.id}.window-slot`;
      this.record = null;
      this.pending = null;
      this.running = false;
      this.busy = false;
      this.stopped = false;
      this.lastCapture = 0;
      this.lastFrame = 0;
      try {
        const saved = JSON.parse(sessionStorage.getItem(this.key));
        const age = Date.now() - saved?.at;
        if (saved?.version === 1 && saved.game === game.sha256 && saved.slot >= 1 && saved.slot <= 4 &&
            Number.isInteger(saved.slot) && configOK(saved.config) && age >= 0 && age < MAX_AGE &&
            saved.armed === true && saved.attempted !== true &&
            performance.getEntriesByType("navigation")[0]?.type === "reload") {
          this.pending = saved;
        }
      } catch { /* malformed or unavailable storage: normal Play */ }
    }

    // A reload reclaims its previous slot first. A duplicated tab cannot take
    // that slot while the original still owns its Web Lock.
    async acquireSlot() {
      if (!navigator.locks) { this.pending = null; return 1; }
      let preferred = this.pending?.slot || 1;
      try { preferred = Number(sessionStorage.getItem(this.slotKey)) || preferred; } catch {}
      if (!Number.isInteger(preferred) || preferred < 1 || preferred > 4) preferred = 1;
      const take = n => new Promise(resolve => {
        navigator.locks.request(`${this.game.id}-slot-${n}`, { ifAvailable: true }, lock => {
          resolve(!!lock);
          return lock ? new Promise(() => {}) : undefined;
        }).catch(() => resolve(false));
      });
      // Give the unloading document time to release its lease.
      if (this.pending && !(await take(preferred))) {
        await new Promise(resolve => setTimeout(resolve, 200));
        if (await take(preferred)) return this.acceptSlot(preferred);
      } else if (this.pending) return this.acceptSlot(preferred);
      for (const n of [preferred, ...[1, 2, 3, 4].filter(n => n !== preferred)]) {
        if (await take(n)) return this.acceptSlot(n);
      }
      this.pending = null;
      return 0;
    }
    acceptSlot(slot) {
      this.slot = slot;
      if (this.pending && this.pending.slot !== slot) this.pending = null;
      try { sessionStorage.setItem(this.slotKey, String(slot)); } catch {}
      return slot;
    }
    persist() {
      try { sessionStorage.setItem(this.key, JSON.stringify(this.record)); } catch {}
    }
    clear() {
      this.stopped = true;
      this.pending = null;
      this.record = null;
      clearInterval(this.timer);
      try { sessionStorage.removeItem(this.key); } catch {}
    }

    // Hash only career files, never graphics settings or logs. A restore/import
    // or a different save must not inherit an old world's position.
    async careerStamp() {
      let dir = await navigator.storage.getDirectory();
      for (const part of [this.game.id, this.slot > 1 ? `profile-${this.slot}` : "profile", "Documents", "NFS Carbon"])
        dir = await dir.getDirectoryHandle(part);
      const entries = [];
      async function walk(folder, prefix = "") {
        for await (const [name, handle] of folder.entries()) {
          const path = `${prefix}${name}`;
          if (handle.kind === "directory") await walk(handle, `${path}/`);
          else {
            const file = await handle.getFile();
            const hash = await crypto.subtle.digest("SHA-256", await file.arrayBuffer());
            entries.push([path, Array.from(new Uint8Array(hash))]);
          }
        }
      }
      await walk(dir);
      if (!entries.length) throw new Error("No career save");
      entries.sort((a, b) => a[0].localeCompare(b[0]));
      const hash = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(JSON.stringify(entries)));
      return Array.from(new Uint8Array(hash), n => n.toString(16).padStart(2, "0")).join("");
    }
    async begin(config, restoring = false) {
      const previous = restoring ? this.pending : null;
      this.pending = null;
      this.record = { version: 1, game: this.game.sha256, slot: this.slot || 1,
        config, at: Date.now(), armed: !restoring, attempted: restoring, point: null, stamp: null };
      this.persist(); // consume the attempt before any asynchronous boot work
      this.wanted = null;
      if (previous && pointOK(previous.point) && typeof previous.stamp === "string") {
        try {
          if (await this.careerStamp() === previous.stamp) this.wanted = previous.point;
        } catch { /* normal career loading remains available */ }
      }
      this.running = true;
      this.started = Date.now();
      if (this.wanted) this.showRestoring();
      addEventListener("carbon-resume", () => this.attach());
      addEventListener("recomp-quit", () => this.clear());
      document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "hidden") this.capture(true);
      });
      addEventListener("pagehide", () => this.capture(true));
      this.timer = setInterval(() => this.tick(), 500);
      this.attach();
    }
    attach() {
      if (this.stopped) return;
      const block = globalThis.carbonResumeBlock;
      if (!block?.memory || !block.ptr || this.attached) return;
      const u = new Uint32Array(block.memory.buffer, block.ptr);
      if (u[0] !== MAGIC || u[1] !== 1) return;
      this.block = block;
      this.attached = true;
      if (this.wanted) this.command(1, this.wanted);
    }
    command(command, point) {
      if (!this.block) return;
      const { memory, ptr } = this.block;
      const u = new Uint32Array(memory.buffer, ptr), f = new Float32Array(memory.buffer, ptr);
      Atomics.add(u, 0x100 / 4, 1);
      u[0x104 / 4] = command;
      if (point) {
        const offset = 0x108;
        u[offset / 4] = point.id;
        const name = new Uint8Array(memory.buffer, ptr + offset + 4, 32);
        name.fill(0); name.set(point.name);
        u[(offset + 36) / 4] = point.model;
        u[(offset + 40) / 4] = point.track;
        f.set(point.p, (offset + 44) / 4);
        f.set(point.q, (offset + 56) / 4);
      }
      Atomics.add(u, 0x100 / 4, 1);
    }
    snapshot() {
      if (!this.block) return null;
      const { memory, ptr } = this.block;
      const u = new Uint32Array(memory.buffer, ptr), f = new Float32Array(memory.buffer, ptr);
      const seq = Atomics.load(u, 2);
      if (seq & 1) return null;
      const name = new Uint8Array(memory.buffer, ptr + 0x44, 32).slice();
      const end = name.indexOf(0);
      const point = { id: u[0x40 / 4], name: Array.from(name.subarray(0, end < 0 ? 32 : end)),
        model: u[0x64 / 4], track: u[0x68 / 4],
        p: Array.from(f.slice(0x6c / 4, 0x78 / 4)), q: Array.from(f.slice(0x78 / 4, 0x88 / 4)) };
      const result = { valid: u[3] === 1 && pointOK(point), status: u[4], reason: u[5], frame: u[9], point };
      return Atomics.load(u, 2) === seq ? result : null;
    }
    tick() {
      if (this.stopped) return;
      // The player also reports caught startup failures as a Back to game link.
      // Do not leave the recovery panel covering that existing error screen.
      if (globalThis.recompFailure || document.querySelector?.("#status:not([hidden]) a")) {
        this.overlay?.remove();
        this.clear();
        return;
      }
      this.attach();
      const snapshot = this.snapshot();
      if (this.wanted && snapshot?.status === 4) {
        this.wanted = null;
        this.overlay?.remove();
        this.record.attempted = false;
        this.record.armed = true;
      } else if (this.wanted && (snapshot?.status === 5 || Date.now() - this.started > 190000)) {
        if (snapshot?.status !== 5) this.command(2);
        this.wanted = null;
        this.overlay?.remove();
        // Leave the game on its normal loading/menu path. A second refresh
        // must not repeat a failed recovery attempt.
        this.record.armed = false;
        this.persist();
      }
      this.capture(false, snapshot);
    }
    async capture(force, snapshot = this.snapshot()) {
      if (this.stopped || !this.running || this.wanted || !this.record || !snapshot) return;
      if (!snapshot.valid) {
        // Invalidate immediately on entering a race/pursuit/menu, rather than
        // restoring an older, unrelated free-roam checkpoint after refresh.
        this.record.point = null;
        this.record.stamp = null;
        this.record.at = Date.now();
        this.persist();
        return;
      }
      // pagehide cannot wait for an asynchronous disk read. Reuse only the
      // already verified save fingerprint for the same career/car/track. If
      // the game saved again meanwhile, the next boot rejects this old stamp.
      const old = this.record.point;
      if (force && old && this.record.stamp && old.id === snapshot.point.id &&
          old.model === snapshot.point.model && old.track === snapshot.point.track &&
          JSON.stringify(old.name) === JSON.stringify(snapshot.point.name)) {
        this.record.point = snapshot.point;
        this.record.at = Date.now();
        this.persist();
      }
      if (this.busy || (!force && Date.now() - this.lastCapture < 3000) || snapshot.frame === this.lastFrame) return;
      this.busy = true;
      this.lastCapture = Date.now();
      this.lastFrame = snapshot.frame;
      const owner = this.record;
      try {
        const stamp = await this.careerStamp();
        const current = this.snapshot();
        if (this.stopped || this.record !== owner || !current?.valid ||
            current.point.id !== snapshot.point.id || current.point.model !== snapshot.point.model ||
            JSON.stringify(current.point.name) !== JSON.stringify(snapshot.point.name)) return;
        owner.point = snapshot.point;
        owner.stamp = stamp;
        owner.at = Date.now();
        owner.armed = true;
        owner.attempted = false;
        this.persist();
      } catch { /* storage temporarily unavailable: never bind a position to an unknown save */ }
      finally { this.busy = false; }
    }
    showRestoring() {
      const panel = document.createElement("div");
      panel.setAttribute("role", "status");
      panel.style.cssText = "position:fixed;z-index:9999;inset:0;background:#050c12ed;display:grid;place-content:center;gap:20px;text-align:center;color:#eafcff;font:18px system-ui";
      const label = document.createElement("div"); label.textContent = "Restoring your drive…";
      const cancel = document.createElement("button"); cancel.textContent = "Back to home";
      cancel.style.cssText = "padding:12px 24px;border:1px solid #5cbac5;background:#12262e;color:inherit;font:inherit;cursor:pointer";
      cancel.addEventListener("click", () => { this.clear(); location.replace(location.pathname); });
      panel.append(label, cancel);
      document.body.append(panel);
      this.overlay = panel;
    }
  }
  globalThis.CarbonRecovery = CarbonRecovery;
})();
