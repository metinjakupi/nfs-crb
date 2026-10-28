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
  let now = 1000;
  const buffer = new SharedArrayBuffer(4096);
  const u32 = new Uint32Array(buffer);
  u32[0] = 0x31435243;
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
    recompOnlineSend() {},
    carbonRaceBlock: { memory: { buffer }, ptr: 0 },
  };
  context.window = context;
  context.globalThis = context;
  vm.runInNewContext(source, context, { filename: 'race.js' });
  const tick = intervals.find(item => item.delay === 100)?.fn;
  assert.ok(tick, 'race.js must install its game-state timer');
  return {
    context,
    tick,
    setNow(value) { now = value; },
    game(values) {
      const at = offset => offset >>> 2;
      if ('phase' in values) u32[at(0x264)] = values.phase;
      if ('session' in values) u32[at(0x2bc)] = values.session;
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
