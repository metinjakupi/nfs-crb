import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';

const source = await readFile(new URL('../../source/web/race.js', import.meta.url), 'utf8');

class Element {
  constructor(tag = 'div') {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.hidden = false;
    this.textContent = '';
    this.className = '';
  }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = [...children]; }
  addEventListener() {}
  setAttribute() {}
  get childElementCount() { return this.children.filter(value => value instanceof Element).length; }
}

function harness() {
  const listeners = new Map();
  const intervals = [];
  const sent = [];
  let now = 1000;
  const buffer = new SharedArrayBuffer(4096);
  const u32 = new Uint32Array(buffer);
  u32[0] = 0x31435243;
  u32[0x2cc / 4] = 3;
  const context = {
    console,
    TextEncoder,
    Uint8Array,
    Uint32Array,
    Int32Array,
    Float32Array,
    SharedArrayBuffer,
    Atomics,
    Map,
    Set,
    Math,
    JSON,
    performance: { now: () => now },
    Event: class { constructor(type) { this.type = type; } },
    document: { createElement: tag => new Element(tag), body: new Element('body') },
    addEventListener(type, fn) {
      const list = listeners.get(type) || [];
      list.push(fn);
      listeners.set(type, list);
    },
    dispatchEvent(event) {
      for (const fn of listeners.get(event.type) || []) fn(event);
    },
    setInterval(fn, delay) { intervals.push({ fn, delay }); return intervals.length; },
    recompOnline: { name: 'Test' },
    recompOnlineState: { me: 1, name: 'Test', people: [] },
    recompOnlineSend(message) { sent.push(message); },
    carbonRaceBlock: { memory: { buffer }, ptr: 0 },
  };
  context.window = context;
  context.globalThis = context;
  vm.runInNewContext(source, context, { filename: 'race.js' });
  const tick = intervals.find(item => item.delay === 100)?.fn;
  assert.ok(tick, 'race.js must install its game-state timer');
  return {
    context,
    sent,
    tick,
    setNow(value) { now = value; },
    game(values) {
      const at = offset => offset >>> 2;
      if ('phase' in values) u32[at(0x264)] = values.phase;
      if ('flow' in values) u32[at(0x2cc)] = values.flow;
      if ('car' in values) u32[at(0x268)] = values.car;
      if ('busy' in values) u32[at(0x260)] = values.busy ? 1 : 0;
      if ('session' in values) u32[at(0x2bc)] = values.session;
      if ('done' in values) u32[at(0x2a8)] = values.done;
      if ('time' in values) new Float32Array(buffer)[at(0x2b0)] = values.time;
    },
    message(detail) { context.dispatchEvent({ type: 'recomp-online-message', detail }); },
  };
}

test('clearing an expired completed race does not inspect the cleared race', () => {
  const h = harness();
  h.message({
    t: 'race', k: 'start', id: 1, race: 77, key: 1, type: 1, laps: 1,
    opts: '00'.repeat(40), roster: [{ id: 1, name: 'Test', car: 1, grid: 0 }],
  });

  h.game({ phase: 4, session: 77 });
  h.tick();
  h.setNow(2000);
  h.game({ phase: 0, session: 0 });
  h.tick();
  h.setNow(23001);

  assert.doesNotThrow(() => h.tick());
  assert.equal(h.context.recompRace.state().race, null);
});

function start(h) {
  h.message({ t: 'race', k: 'start', id: 1, race: 77, key: 1, type: 1, laps: 1,
    opts: '00'.repeat(40), roster: [{ id: 1, name: 'Test', car: 1, grid: 0 },
      { id: 2, name: 'Friend', car: 1, grid: 1 }] });
}

test('a previous race cannot report progress or finish for a newly accepted race', () => {
  const h = harness();
  start(h);
  h.game({ phase: 5, session: 76, done: 1, time: 42 });
  h.tick();
  assert.equal(h.sent.filter(m => m.k === 'finish' || m.k === 'progress').length, 0);
  h.game({ phase: 4, session: 77, done: 0, time: 1 });
  h.message({ t: 'race', k: 'go', id: 1, race: 77 });
  h.tick();
  h.game({ phase: 5, session: 77, done: 1, time: 65 });
  h.tick();
  assert.deepEqual(h.sent.filter(m => m.k === 'finish').map(m => m.time), [65]);
});

test('a repeated start must not reset an active race or erase its results', () => {
  const h = harness();
  start(h);
  h.message({ t: 'race', k: 'go', id: 1, race: 77 });
  h.message({ t: 'race', k: 'finish', id: 2, race: 77, time: 60 });
  start(h);
  assert.equal(h.context.recompRace.state().race.went, true);
  assert.equal(h.context.recompRace.state().standings.find(([id]) => id === 2)[1].time, 60);
});

test('the host cannot release a race while a listed participant is not ready', () => {
  const h = harness();
  start(h);
  h.message({ t: 'race', k: 'ready', id: 1, race: 77 });
  assert.equal(h.sent.some(m => m.k === 'go'), false, 'absence from nearby people is not readiness');
  h.message({ t: 'race', k: 'ready', id: 2, race: 77 });
  assert.equal(h.sent.filter(m => m.k === 'go').length, 1);
});

for (const busy of [false, true]) test(`a loading timeout cancels without a winner (game block busy: ${busy})`, () => {
  const h = harness();
  start(h);
  h.game({ phase: 3, session: 77, busy });
  h.setNow(62000);
  h.tick();
  assert.equal(h.sent.some(m => m.k === 'go'), false);
  assert.equal(h.sent.some(m => m.k === 'cancel'), true);
  assert.equal(h.context.recompRace.state().race.cancelled, true);
  const memory = new Uint32Array(h.context.carbonRaceBlock.memory.buffer);
  assert.equal(memory[0x24 / 4], 0, 'do not launch later from another screen');
  assert.equal(memory[0x20 / 4], 77, 'release the loading hold so the player can exit');
  h.game({ phase: 5, session: 77, done: 1, time: 1, busy: false });
  h.tick();
  assert.equal(h.sent.some(m => m.k === 'finish'), false);
});

test('the Sprint invitation uses the frontend race type rather than the engine enum', () => {
  const h = harness();
  // Lincoln Boulevard: the Quick Race UI says Sprint; DAL 0xbc3 returns 2.
  h.message({ t: 'race', k: 'lobby', id: 2, race: 78, key: 0x8ef528ca, type: 2,
    opts: '00'.repeat(40), roster: [{ id: 2, name: 'Friend', car: 1 }] });
  const text = node => typeof node === 'string' ? node : node.textContent + node.children.map(text).join(' ');
  assert.match(text(h.context.document.body), /Sprint/);
  assert.doesNotMatch(text(h.context.document.body), /Drag/);
});

test('an invitation can join only with a loaded car in the frontend', () => {
  const h = harness();
  h.message({ t: 'race', k: 'lobby', id: 2, race: 78, key: 1, type: 2,
    opts: '00'.repeat(40), roster: [{ id: 2, name: 'Friend', car: 1 }] });
  h.context.recompRace.join();
  assert.equal(h.sent.some(m => m.k === 'join'), false);
  h.game({ car: 123, flow: 6 });
  h.context.recompRace.join();
  assert.equal(h.sent.some(m => m.k === 'join'), false, 'free roam cannot launch a Quick Race');
  h.game({ flow: 3 });
  h.context.recompRace.join();
  assert.equal(h.sent.filter(m => m.k === 'join').length, 1);
  assert.equal(h.sent.find(m => m.k === 'join').car, 123);
});
