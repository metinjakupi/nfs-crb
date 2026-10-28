// Prepare known cars while their owners approach or a race is being set up.
// These are read-only snapshots of the existing MP/look/race mailboxes. Keep
// their seqlock checks: half-written car keys must never schedule downloads.
(() => {
  'use strict';
  if (!window.recompStream) return;
  function view(block, magic) {
    if (!block?.memory || !block.ptr) return null;
    const words = new Uint32Array(block.memory.buffer);
    return words[block.ptr >>> 2] === magic ? words : null;
  }
  function snapshot(words, seq, read) {
    const before = Atomics.load(words, seq);
    if (before & 1) return [];
    const keys = read();
    return Atomics.load(words, seq) === before ? keys : [];
  }
  function prepare() {
    const keys = [];
    const mp = globalThis.carbonMpBlock, m = view(mp, 0x31504d43);
    if (m) {
      const b = mp.ptr >>> 2;
      keys.push(...snapshot(m, b + 3, () => [m[b + 5]]));
      for (let i = 0; i < 8; ++i) {
        const at = b + (0x60 + i * 0x100) / 4;
        keys.push(...snapshot(m, at + 1, () => m[at] ? [m[at + 2]] : []));
      }
    }
    const look = globalThis.carbonLookBlock, l = view(look, 0x4b4c4d43);
    if (l) {
      const b = look.ptr >>> 2;
      keys.push(...snapshot(l, b + 2, () => [l[b + 3]]));
      for (let i = 0; i < 8; ++i) {
        const at = b + (0x460 + i * 0x460) / 4;
        keys.push(...snapshot(l, at + 1, () => l[at] ? [l[at + 2]] : []));
      }
    }
    const race = globalThis.carbonRaceBlock, r = view(race, 0x31435243);
    if (r) {
      const b = race.ptr >>> 2;
      keys.push(...snapshot(r, b + 2, () => {
        const result = [];
        for (let i = 0; i < Math.min(r[b + 0x1c / 4], 8); ++i) {
          const at = b + (0x60 + i * 0x40) / 4;
          if (r[at]) result.push(r[at + 1]);
        }
        return result;
      }));
    }
    const unique = [...new Set(keys.filter(k => k > 0))].slice(0, 9);
    if (unique.length) window.recompStream.prepareModels(unique);
  }
  const timer = setInterval(prepare, 500);
  addEventListener('pagehide', () => clearInterval(timer), { once: true });
  prepare();
})();
