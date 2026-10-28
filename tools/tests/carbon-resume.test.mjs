import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { webcrypto } from 'node:crypto';
import vm from 'node:vm';

const source = await readFile(new URL('../../source/web/resume.js', import.meta.url), 'utf8');
const game = { id: 'nfsc', sha256: 'carbon-v14' };
const config = { online: true, name: 'Driver', room: 'Friends', crew: 'Crew', server: 'internet' };
const point = { id: 99, name: [65, 66], model: 123, track: 5000, p: [-300, 28, 3400], q: [0, 0, 0, 1] };
const storage = () => {
  const values = new Map();
  return { getItem: k => values.get(k) ?? null, setItem: (k, v) => values.set(k, String(v)), removeItem: k => values.delete(k) };
};
function browser({ saved, busy = [], navigation = 'reload', session = storage(), save = { bytes: 'original career' } } = {}) {
  if (saved) session.setItem('nfsc.recovery.v1', JSON.stringify(saved));
  const held = new Set(busy), events = new Map();
  const file = { kind: 'file', async getFile() { return { arrayBuffer: async () => new TextEncoder().encode(save.bytes).buffer }; } };
  const dir = { async getDirectoryHandle() { return dir; }, async *entries() { yield ['Career', file]; } };
  const element = () => ({ style: {}, setAttribute() {}, addEventListener() {}, append() {}, remove() {} });
  const scope = {
    sessionStorage: session, performance: { getEntriesByType: () => [{ type: navigation }] },
    navigator: { storage: { getDirectory: async () => dir }, locks: { async request(name, options, fn) {
      if (held.has(name)) return fn(null);
      held.add(name); return fn({ name });
    } } },
    document: { createElement: element, body: { append() {} }, addEventListener() {} },
    addEventListener: (name, fn) => events.set(name, fn),
    setInterval: () => 1, clearInterval() {}, setTimeout: fn => fn(),
    Date, TextEncoder, TextDecoder, Uint8Array, Uint32Array, Float32Array, Atomics, crypto: webcrypto,
  };
  vm.runInNewContext(source, scope);
  return { recovery: new scope.CarbonRecovery(game), scope, session, held, events, save };
}
const record = overrides => ({ version: 1, game: game.sha256, slot: 2, config,
  at: Date.now(), armed: true, attempted: false, point, stamp: 'unchecked', ...overrides });
function gameMailbox(scope, { valid = true, frame = 1, odd = false } = {}) {
  const memory = { buffer: new SharedArrayBuffer(4096) }, ptr = 512;
  const u = new Uint32Array(memory.buffer, ptr), f = new Float32Array(memory.buffer, ptr);
  u[0] = 0x31525343; u[1] = 1; u[2] = odd ? 1 : 2; u[3] = Number(valid); u[9] = frame;
  u[0x40 / 4] = point.id;
  new Uint8Array(memory.buffer, ptr + 0x44, 32).set(point.name);
  u[0x64 / 4] = point.model; u[0x68 / 4] = point.track;
  f.set(point.p, 0x6c / 4); f.set(point.q, 0x78 / 4);
  scope.carbonResumeBlock = { memory, ptr };
  return u;
}

test('refresh reclaims this tab’s career slot instead of the first available slot', async () => {
  const b = browser({ saved: record() });
  assert.equal(await b.recovery.acquireSlot(), 2);
  assert.equal(b.recovery.pending.config.room, 'Friends');
  assert.deepEqual([...b.held], ['nfsc-slot-2']);
});

test('a copied/busy tab cannot restore another open window’s career', async () => {
  const b = browser({ saved: record(), busy: ['nfsc-slot-2'] });
  assert.equal(await b.recovery.acquireSlot(), 1);
  assert.equal(b.recovery.pending, null);
  const full = browser({ busy: [1, 2, 3, 4].map(n => `nfsc-slot-${n}`) });
  assert.equal(await full.recovery.acquireSlot(), 0);
});

test('only a recent, unconsumed reload record can automatically restart', () => {
  for (const saved of [record({ attempted: true }), record({ armed: false }), record({ at: Date.now() - 13 * 3600000 }),
    record({ game: 'another executable' }), record({ slot: 0 }), record({ config: null })]) {
    assert.equal(browser({ saved }).recovery.pending, null);
  }
  assert.equal(browser({ saved: record(), navigation: 'navigate' }).recovery.pending, null);
});

test('writes a validated checkpoint to the native restore mailbox only for the same career file', async () => {
  for (const changed of [false, true]) {
    const b = browser({ saved: record({ slot: 1 }) });
    await b.recovery.acquireSlot();
    b.recovery.pending.stamp = await b.recovery.careerStamp();
    if (changed) b.save.bytes = 'a restored or more recent career';
    const u = gameMailbox(b.scope);
    await b.recovery.begin(config, true);
    assert.equal(u[0x104 / 4], changed ? 0 : 1);
    if (!changed) {
      assert.equal(u[0x108 / 4], 99);
      assert.equal(u[(0x108 + 36) / 4], 123);
      assert.equal(u[(0x108 + 40) / 4], 5000);
      assert.equal(u[0x100 / 4] % 2, 0);
    }
    assert.equal(new b.scope.CarbonRecovery(game).pending, null, 'a second refresh cannot repeat an unfinished attempt');
  }
});

test('zero quaternion and unterminated profile names never reach the native restore command', async () => {
  for (const bad of [{ ...point, q: [0, 0, 0, 0] }, { ...point, name: Array(32).fill(65) },
    { ...point, p: [Infinity, 0, 0] }]) {
    const b = browser({ saved: record({ point: bad, slot: 1 }) });
    await b.recovery.acquireSlot();
    b.recovery.pending.stamp = await b.recovery.careerStamp();
    const u = gameMailbox(b.scope);
    await b.recovery.begin(config, true);
    assert.equal(u[0x104 / 4], 0);
  }
});

test('a race/pursuit invalidates the last free-roam checkpoint, and Quit clears auto-restart', async () => {
  const b = browser(); await b.recovery.acquireSlot();
  const u = gameMailbox(b.scope);
  await b.recovery.begin(config);
  await b.recovery.capture(true);
  let saved = JSON.parse(b.session.getItem('nfsc.recovery.v1'));
  assert.deepEqual(saved.point, point);
  assert.equal(typeof saved.stamp, 'string');
  u[3] = 0; u[9]++;
  await b.recovery.capture(true);
  saved = JSON.parse(b.session.getItem('nfsc.recovery.v1'));
  assert.equal(saved.point, null, 'refresh in a race must not return to an earlier road');
  b.events.get('recomp-quit')();
  assert.equal(b.session.getItem('nfsc.recovery.v1'), null);
  u[3] = 1; u[9]++;
  await b.recovery.capture(true);
  assert.equal(b.session.getItem('nfsc.recovery.v1'), null, 'late capture cannot undo Quit');
});

test('an incomplete native update cannot be saved as a valid checkpoint', async () => {
  const b = browser(); await b.recovery.acquireSlot();
  gameMailbox(b.scope, { odd: true });
  await b.recovery.begin(config);
  await b.recovery.capture(true);
  assert.equal(JSON.parse(b.session.getItem('nfsc.recovery.v1')).point, null);
});

test('a caught startup failure exposes the player error and clears automatic recovery', async () => {
  const b = browser({ saved: record({ slot: 1 }) });
  await b.recovery.acquireSlot();
  b.recovery.pending.stamp = await b.recovery.careerStamp();
  await b.recovery.begin(config, true);
  let removed = false;
  b.recovery.overlay.remove = () => { removed = true; };
  b.scope.document.querySelector = () => ({});
  b.recovery.tick();
  assert.equal(removed, true);
  assert.equal(b.session.getItem('nfsc.recovery.v1'), null);
});
