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
    ['enabled', 'Driver view', 0, 1, 1, 1, 'Your change camera button (C) reaches driver view next to bumper view. F6 switches straight to it and back. Use right-drag or the right stick to look around. F7 adjusts the seat.'],
    ['height', 'Seat height (cm)', 75, 140, 1, 105, 'Adjusts the seat height for this car.'],
    ['forward', 'Seat forward / back (cm)', -65, 65, 1, -18, 'Moves the seat forward or back for this car.'],
    ['left', 'Seat left / right (cm)', -55, 55, 1, 34, 'Moves the seat left or right for this car. Right-hand-drive cars keep their own seat position.'],
    ['pitch', 'View angle (degrees)', -20, 20, 1, -7, 'Tilts the driver view up or down for this car.'],
    ['light', 'Cabin brightness %', 30, 600, 10, 300, 'Adjusts interior brightness without changing the road lights.'],
  ];
  let model = 0, attached = null, defaults = {}, panel = null;
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
    // The game's camera button changes this too: keep the choice for the next start.
    const live = value('enabled');
    if (!seatOpen() && live !== (saved.enabled === 0 ? 0 : 1)) { saved.enabled = live; persist(); notify(); }
    const next = s.v.getUint32(28, true);
    if (next && next !== model) {
      model = next;
      apply('forward', model === 2485894945 ? -18 : model === 1728462292 ? -40 : -30);
      apply('height', model === 2485894945 ? 105 : model === 1728462292 ? 83 : 93);
      apply('pitch', -7);
      defaults = Object.fromEntries(Object.keys(fields).map(name => [name, value(name)]));
      if (saved.seats[0]) {
        saved.seats[model] = {...saved.seats[model], ...saved.seats[0]};
        delete saved.seats[0];
        persist();
      }
      const seat = saved.seats[model];
      for (const [name, , min, max] of definitions) if (name in fields && Number.isFinite(seat?.[name])) apply(name, Math.max(min, Math.min(max, seat[name])));
      notify();
    }
  }
  const settings = globalThis.carbonCockpitSettings = {
    list() {
      sync(); if (!shared()) return [];
      return definitions.map(([name, label, min, max, step, def, help]) => ({
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
  // ---- Driver seat panel (F7, or Driver seat in the F9 menu) ----
  // Sits beside the live view: sliders apply at once, Save keeps them for this
  // car, Cancel puts back what was there. The game keeps running behind it, so
  // keys stay in the panel (as in the menu) and never reach the car.
  const SEAT_ROWS = ['height', 'forward', 'left', 'pitch', 'light'];
  const SEAT_UNITS = {height: ' cm', forward: ' cm', left: ' cm', pitch: '°', light: '%'};
  const seatCss = `
  .ds { --ink: #e9f6ff; --dim: #8fa6bd; --cyan: #5fe4f2; --line: rgba(95, 228, 242, .22);
        position: fixed; z-index: 29; top: 50%; right: max(16px, env(safe-area-inset-right)); transform: translateY(-50%);
        box-sizing: border-box; width: min(300px, calc(100vw - 32px)); max-height: calc(100dvh - 24px); overflow: auto;
        padding: 16px 18px; background: rgba(6, 12, 22, .9); border: 1px solid var(--line); color: var(--ink);
        font: 14px/1.4 "Avenir Next", "Segoe UI", system-ui, sans-serif; scrollbar-width: thin; }
  .ds[hidden] { display: none; }
  .ds * { box-sizing: border-box; }
  .ds h2 { margin: 0; font-size: 17px; font-weight: 700; }
  .ds-status { margin: 2px 0 12px; font-size: 12px; color: var(--dim); }
  .ds-row { display: grid; grid-template-columns: 1fr auto; gap: 0 10px; margin-bottom: 8px; }
  .ds-row output { color: var(--cyan); font-variant-numeric: tabular-nums; }
  .ds-row input { grid-column: 1 / -1; width: 100%; height: 24px; margin: 0; accent-color: var(--cyan); cursor: pointer; }
  .ds-row input:disabled { opacity: .35; cursor: default; }
  .ds footer { display: flex; align-items: center; gap: 8px; margin-top: 12px; }
  .ds footer span { flex: 1; }
  .ds button { min-height: 34px; padding: 6px 12px; font: 700 13px/1 inherit; font-family: inherit; color: var(--ink); cursor: pointer;
               background: transparent; border: 1px solid var(--line); }
  .ds button:hover:not(:disabled) { border-color: var(--cyan); }
  .ds button:focus-visible { outline: 2px solid var(--cyan); outline-offset: 2px; }
  .ds input:focus-visible { outline: 1px solid var(--cyan); outline-offset: 0; }
  .ds button:disabled { opacity: .4; cursor: default; }
  .ds .ds-save { color: #04131a; background: var(--cyan); border-color: var(--cyan); }
  `;
  let seatBefore = null, seatModel = 0, seatTimer = 0, seatUpdates = 0, seatSeen = -1e9, seatReady = false;
  const seatOpen = () => !!panel && !panel.hidden;
  // True while the seat camera is really running: paused, the game's own camera is back.
  function seatDriving() {
    const s = shared(), b = globalThis.carbonCockpitVisual;
    if (!s || !b || !model || new DataView(b.memory.buffer, b.ptr, 236).getUint32(232, true) !== 1) return false;
    const updates = s.v.getUint32(24, true);
    if (updates !== seatUpdates) { seatUpdates = updates; seatSeen = performance.now(); }
    return performance.now() - seatSeen < 500;
  }
  function seatShow() {
    for (const name of SEAT_ROWS) {
      const input = panel.querySelector(`input[name=${name}]`), v = value(name);
      input.value = v;
      panel.querySelector(`output[for=seat-${name}]`).textContent = v + SEAT_UNITS[name];
    }
  }
  function seatStatus() {
    if (seatBefore && model !== seatModel) { seatCancel(); return; } // a different car: its own seat
    const ready = seatDriving();
    panel.querySelector('.ds-status').textContent = ready ? 'Save keeps this for this car' : 'Start driving to adjust your seat';
    for (const el of panel.querySelectorAll('input, .ds-reset, .ds-save')) el.disabled = !ready;
    if (ready && (!seatReady || !panel.contains(document.activeElement))) panel.querySelector('input').focus();
    seatReady = ready;
  }
  function seatMove(from, by) {
    const list = [...panel.querySelectorAll('input, button')].filter(el => !el.disabled);
    const next = list[list.indexOf(from) + by];
    if (next) next.focus();
  }
  function makeSeatPanel() {
    const style = document.createElement('style');
    style.textContent = seatCss;
    document.head.append(style);
    panel = document.createElement('section');
    panel.className = 'ds';
    panel.hidden = true;
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-label', 'Driver seat');
    panel.innerHTML = `<h2>Driver seat</h2><p class="ds-status" role="status"></p><div class="ds-rows"></div>
      <footer><button type="button" class="ds-reset">Reset</button><span></span>
      <button type="button" class="ds-cancel">Cancel</button><button type="button" class="ds-save">Save</button></footer>`;
    for (const [name, label, min, max, step] of definitions) {
      if (!SEAT_ROWS.includes(name)) continue;
      const row = document.createElement('div'), text = document.createElement('label');
      const output = document.createElement('output'), input = document.createElement('input');
      row.className = 'ds-row';
      text.textContent = label.replace(/ \(.*\)| %$/, '');
      text.htmlFor = input.id = 'seat-' + name;
      output.htmlFor = input.id;
      input.type = 'range';
      input.name = name;
      Object.assign(input, {min, max, step});
      input.addEventListener('input', () => { apply(name, Number(input.value)); output.textContent = input.value + SEAT_UNITS[name]; });
      row.append(text, output, input);
      panel.querySelector('.ds-rows').append(row);
    }
    panel.querySelector('.ds-reset').addEventListener('click', () => {
      for (const [name, , , , , def] of definitions) if (SEAT_ROWS.includes(name)) apply(name, defaults[name] ?? def);
      seatShow();
    });
    panel.querySelector('.ds-cancel').addEventListener('click', seatCancel);
    panel.querySelector('.ds-save').addEventListener('click', seatSave);
    panel.addEventListener('keydown', e => {
      e.stopPropagation(); // before the game's listeners on the window
      if (e.key === 'Escape') { e.preventDefault(); seatCancel(); }
      else if (e.key === 'Enter' && !e.target.matches('.ds-reset, .ds-cancel')) { e.preventDefault(); seatSave(); }
      else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); seatMove(e.target, e.key === 'ArrowDown' ? 1 : -1); }
    });
    panel.addEventListener('keypress', e => e.stopPropagation());
    document.body.append(panel);
  }
  function seatClose() {
    clearInterval(seatTimer);
    panel.hidden = true;
    seatBefore = null;
    globalThis.recompMenuOpen = false;
    document.getElementById('canvas')?.focus();
  }
  function seatCancel() {
    if (!seatOpen() || !seatBefore) return;
    for (const name of SEAT_ROWS) apply(name, seatBefore[name]);
    apply('enabled', seatBefore.enabled);
    seatClose();
  }
  function seatSave() {
    if (!seatOpen() || !seatDriving()) return;
    saved.seats[model] ||= {};
    for (const name of SEAT_ROWS) {
      if (name in fields) saved.seats[model][name] = value(name);
      else saved[name] = value(name);
    }
    persist(); notify();
    seatClose();
  }
  function openSeatPanel() {
    sync();
    if (!shared() || seatOpen()) return;
    if (!panel) makeSeatPanel();
    if (document.pointerLockElement) document.exitPointerLock();
    seatBefore = {enabled: value('enabled'), ...Object.fromEntries(SEAT_ROWS.map(name => [name, value(name)]))};
    seatModel = model; seatReady = false;
    apply('enabled', 1); // the seat can only be judged from the seat
    seatShow();
    panel.hidden = false;
    globalThis.recompMenuOpen = true; // the kit gives the game a pad with nothing held
    seatStatus();
    (panel.querySelector('input:not(:disabled)') || panel.querySelector('.ds-cancel')).focus();
    seatTimer = setInterval(seatStatus, 200);
  }
  addEventListener('keydown', e => {
    if (seatOpen()) {
      if (e.code === 'F7' || e.code === 'F9' || e.code === 'F10') {
        e.preventDefault(); e.stopImmediatePropagation();
        if (!e.repeat) seatCancel();
      } else if (!panel.contains(e.target)) { // focus left for the canvas: keys still stay out of the game
        e.preventDefault(); e.stopImmediatePropagation();
        (panel.querySelector('input:not(:disabled)') || panel.querySelector('.ds-cancel')).focus();
      }
      return;
    }
    if (e.code !== 'F7' || e.repeat || e.ctrlKey || e.altKey || e.metaKey || globalThis.recompMenuOpen || e.target?.closest?.('input,textarea,[contenteditable=true]') || !shared()) return;
    e.preventDefault(); openSeatPanel();
  }, true);
  addEventListener('keypress', e => { if (seatOpen() && !panel.contains(e.target)) { e.preventDefault(); e.stopImmediatePropagation(); } }, true);
  addEventListener('recomp-menu', seatCancel); // the controller's menu chord
  (globalThis.recompMenuActions ||= []).push({
    id: 'driver-seat', group: 'Controls', label: 'Driver seat', hint: 'F7', shown: () => !!shared(),
    run: () => setTimeout(openSeatPanel), // after the menu has closed
  });
  dispatchEvent(new Event('recomp-menu-toggles'));
  setInterval(sync, 200);
  // Kept for the isolated native camera checks.
  globalThis.cockpitTest = {
    snapshot() { const s = shared(); return s ? {enabled: value('enabled'), ...Object.fromEntries(Object.entries(fields).map(([name, offset]) => [name, s.v.getFloat32(offset, true)])), updates: s.v.getUint32(24, true), model: s.v.getUint32(28, true), eye: [32,36,40].map(o => s.v.getFloat32(o,true)), aim: [44,48,52].map(o => s.v.getFloat32(o,true))} : null; },
    seat(values) { for (const [name, v] of Object.entries(values)) if (name in fields) settings.set('camera/' + name, v * (name === 'pitch' ? 1 : 100)); return this.snapshot(); }, toggle,
  };
  // Temporary localhost diagnosis: record camera gates in the local server log.
  let diagnosticCount = 0, diagnosticLast = 0, diagnosticGuestBase;
  const diagnosticStart = Date.now();
  function driverDiagnostic(reason, event) {
    if (Date.now() - diagnosticStart > 300000 || diagnosticCount >= 70 || Date.now() - diagnosticLast < 700) return;
    diagnosticLast = Date.now(); diagnosticCount++;
    const record = {kind: 'driver-view-diagnostic', time: new Date().toISOString(), reason,
      path: location.pathname, build: globalThis.recompPageBuild?.wasm,
      runtime: globalThis.recompRuntimeBuild?.wasm,
      cockpitEnv: globalThis.Module?.ENV?.RECOMP_COCKPIT,
      customizationEnv: globalThis.Module?.ENV?.RECOMP_CUSTOMIZATION,
      menuOpen: !!globalThis.recompMenuOpen, focused: document.hasFocus(),
      key: event ? {code: event.code, key: event.key, repeat: event.repeat} : undefined};
    try {
      record.camera = globalThis.cockpitTest?.snapshot();
      const block = globalThis.carbonCockpitVisual;
      if (block) {
        const view = new DataView(block.memory.buffer, block.ptr, 236);
        record.visual = {frame: view.getUint32(8,true), active: view.getUint32(12,true), valid: view.getUint32(232,true)};
      }
      const memory = globalThis.carbonMpBlock?.memory;
      if (memory) {
        const bytes = new Uint8Array(memory.buffer);
        if (diagnosticGuestBase === undefined) {
          const anchor = new TextEncoder().encode('Fe_WorldMap_RandomEncounter.fng');
          let at = 0;
          while ((at = bytes.indexOf(anchor[0],at)) >= 0) {
            if (anchor.every((x,i)=>bytes[at+i]===x)) break;
            at++;
          }
          if (at >= 0) diagnosticGuestBase = at - 0x9d0150;
        }
        if (Number.isInteger(diagnosticGuestBase)) {
          const view = new DataView(memory.buffer), read = address => view.getUint32(diagnosticGuestBase+address,true);
          record.flow = read(0xa99bbc);
          for (let node=read(0xa9e728),i=0;node&&node!==0xa9e728&&i<64;node=read(node),i++) {
            const car=node-0xd0;
            if (read(car+0x7c)&&read(car+0x148)===0) {
              let collection=read(car+0xd8);
              if (collection&&(read(car+0xe4)&1)) collection=read(collection+0xc);
              record.car = {model:collection?read(collection+0x10):0,body:!!read(car+0x70)};
              break;
            }
          }
        }
      }
    } catch (error) {record.diagnosticError=String(error.message).slice(0,200);}
    fetch('/_runtime-diagnostics',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(record)}).catch(()=>{});
  }
  addEventListener('keydown',event=>{if(event.code==='F6'||event.key==='F6')setTimeout(()=>driverDiagnostic('F6',event),250);},true);
  addEventListener('carbon-cockpit-settings',()=>setTimeout(()=>driverDiagnostic('settings'),100));
  setInterval(()=>driverDiagnostic('sample'),5000);
})();
