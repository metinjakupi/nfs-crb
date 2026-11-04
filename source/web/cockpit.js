(() => {
  'use strict';
  if (!['localhost', '127.0.0.1', '[::1]'].includes(location.hostname) || window.recompSite) return;
  const key = 'nfsc.cockpit-settings-v1';
  let saved;
  try { saved = JSON.parse(localStorage.getItem(key)); } catch {}
  if (!saved || typeof saved !== 'object' || Array.isArray(saved)) saved = {};
  if (!saved.seats || typeof saved.seats !== 'object' || Array.isArray(saved.seats)) saved.seats = {};
  try {
    if (localStorage.getItem('nfsc.driver-view-recovery-v1') !== '1') {
      saved.enabled = 1;
      localStorage.setItem(key, JSON.stringify(saved));
      localStorage.setItem('nfsc.driver-view-recovery-v1', '1');
    }
  } catch {}
  const fields = {forward: 8, left: 12, height: 16, pitch: 20};
  const definitions = [
    ['enabled', 'Driver view', 0, 1, 1, 1, 'F6 also switches between driver view and outside view. Use right-drag or the right stick to look around.'],
    ['height', 'Seat height (cm)', 75, 140, 1, 105, 'Adjusts the seat height for this car.'],
    ['forward', 'Seat forward / back (cm)', -65, 65, 1, -18, 'Moves the seat forward or back for this car.'],
    ['left', 'Seat left / right (cm)', -55, 55, 1, 34, 'Moves the seat left or right for this car. Right-hand-drive cars keep their own seat position.'],
    ['pitch', 'View angle (degrees)', -20, 20, 1, -7, 'Tilts the driver view up or down for this car.'],
    ['light', 'Cabin brightness %', 30, 600, 10, 300, 'Adjusts interior brightness without changing the road lights.'],
  ];
  let model = 0, attached = null, defaults = {};
  const shared = () => {
    const b = globalThis.carbonCockpitBlock;
    return b ? {b, v: new DataView(b.memory.buffer, b.ptr, 56)} : null;
  };
  function write(offset, value) {
    const s = shared(); if (!s) return;
    const bits = new Int32Array(new Float32Array([value]).buffer)[0];
    Atomics.store(new Int32Array(s.b.memory.buffer, s.b.ptr + offset, 1), 0, bits);
  }
  function persist() { try { localStorage.setItem(key, JSON.stringify(saved)); } catch {} }
  function notify() { dispatchEvent(new Event('carbon-cockpit-settings')); }
  function apply(name, value) {
    const s = shared(); if (!s) return;
    if (name === 'enabled') Atomics.store(new Int32Array(s.b.memory.buffer, s.b.ptr + 4, 1), 0, value);
    else if (name === 'light') {
      const b = globalThis.carbonNativeCockpit;
      if (b) { const bits = new Int32Array(new Float32Array([value / 100]).buffer)[0]; Atomics.store(new Int32Array(b.memory.buffer, b.ptr + 16, 1), 0, bits); }
    } else write(fields[name], name === 'pitch' ? value : value / 100);
  }
  function value(name) {
    const s = shared(); if (!s) return null;
    if (name === 'enabled') return s.v.getInt32(4, true) ? 1 : 0;
    if (name === 'light') { const b = globalThis.carbonNativeCockpit; return b ? Math.round(new DataView(b.memory.buffer, b.ptr + 16, 4).getFloat32(0, true) * 100) : 200; }
    return Math.round(s.v.getFloat32(fields[name], true) * (name === 'pitch' ? 1 : 100));
  }
  function sync() {
    const s = shared(); if (!s) return;
    if (attached !== s.b) {
      attached = s.b; model = 0;
      apply('enabled', saved.enabled === 0 ? 0 : 1);
      apply('light', Number.isFinite(saved.light) ? Math.max(30, Math.min(600, saved.light)) : 300);
      notify();
    }
    const next = s.v.getUint32(28, true);
    if (next && next !== model) {
      model = next;
      apply('forward', model === 2485894945 ? -18 : model === 1728462292 ? -40 : -30);
      apply('height', model === 2485894945 ? 105 : model === 1728462292 ? 83 : 93);
      apply('pitch', -7);
      defaults = Object.fromEntries(Object.keys(fields).map(name => [name, value(name)]));
      const seat = saved.seats[model];
      for (const [name, , min, max] of definitions) if (name in fields && Number.isFinite(seat?.[name])) apply(name, Math.max(min, Math.min(max, seat[name])));
      notify();
    }
  }
  const settings = globalThis.carbonCockpitSettings = {
    list() {
      sync(); if (!shared()) return [];
      return definitions.filter(([name]) => model || !(name in fields)).map(([name, label, min, max, step, def, help]) => ({
        id: 'camera/' + name, label, group: 'controls', kind: name === 'enabled' ? 'bool' : 'int',
        min, max, step, def: defaults[name] ?? def, choices: [], apply: 'live', help, value: value(name),
      }));
    },
    get(id) { return this.list().find(s => s.id === id); },
    set(id, next) {
      const s = this.get(id); if (!s || !Number.isFinite(next)) return;
      const name = id.slice(7), n = Math.max(s.min, Math.min(s.max, Math.round(next)));
      apply(name, n);
      if (name in fields) { saved.seats[model] ||= {}; saved.seats[model][name] = n; }
      else saved[name] = n;
      persist(); notify();
    },
  };
  function toggle() { sync(); const s = settings.get('camera/enabled'); if (s) settings.set(s.id, s.value ? 0 : 1); }
  addEventListener('keydown', e => {
    if (e.code !== 'F6' || e.repeat || e.ctrlKey || e.altKey || e.metaKey || globalThis.recompMenuOpen || e.target?.closest?.('input,textarea,[contenteditable=true]') || !shared()) return;
    e.preventDefault(); toggle();
  }, true);
  setInterval(sync, 200);
  // Kept for the isolated native camera checks.
  globalThis.cockpitTest = {
    snapshot() { const s = shared(); return s ? {enabled: value('enabled'), ...Object.fromEntries(Object.entries(fields).map(([name, offset]) => [name, s.v.getFloat32(offset, true)])), updates: s.v.getUint32(24, true), model: s.v.getUint32(28, true), eye: [32,36,40].map(o => s.v.getFloat32(o,true)), aim: [44,48,52].map(o => s.v.getFloat32(o,true))} : null; },
    seat(values) { for (const [name, v] of Object.entries(values)) if (name in fields) settings.set('camera/' + name, v * (name === 'pitch' ? 1 : 100)); return this.snapshot(); }, toggle,
  };
})();
