import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

test('prepares stable own, approaching, and race car keys without modifying game mailboxes', async () => {
  const source = await readFile(new URL('../../source/web/streaming.js', import.meta.url), 'utf8');
  const memory = { buffer: new SharedArrayBuffer(65536) }, words = new Uint32Array(memory.buffer);
  const mp = 0x100, look = 0x2000, race = 0x5000;
  words[mp / 4] = 0x31504d43;
  words[mp / 4 + 5] = 0xd6334437;
  words[(mp + 0x60) / 4] = 2;
  words[(mp + 0x68) / 4] = 0x8774322c;
  words[look / 4] = 0x4b4c4d43;
  words[(look + 0x460) / 4] = 3;
  words[(look + 0x464) / 4] = 1; // another worker is replacing this record
  words[(look + 0x468) / 4] = 0xdeadbeef;
  words[race / 4] = 0x31435243;
  words[(race + 0x1c) / 4] = 1;
  words[(race + 0x60) / 4] = 4;
  words[(race + 0x64) / 4] = 0x12345678;
  const before = words.slice(), calls = [];
  let tick, hidden, cleared = false;
  const context = { Uint32Array, Atomics, Set, Math,
    window: { recompStream: { prepareModels: keys => calls.push(Array.from(keys)) } },
    carbonMpBlock: { memory, ptr: mp }, carbonLookBlock: { memory, ptr: look }, carbonRaceBlock: { memory, ptr: race },
    setInterval: fn => { tick = fn; return 1; }, clearInterval: () => { cleared = true; },
    addEventListener: (type, fn) => { if (type === 'pagehide') hidden = fn; },
  };
  vm.runInNewContext(source, context);
  assert.deepEqual(calls[0], [0xd6334437, 0x8774322c, 0x12345678]);
  assert.deepEqual(words, before, 'preparation must not mutate live game state');
  words[(look + 0x464) / 4] = 2;
  tick();
  assert.ok(calls[1].includes(0xdeadbeef), 'the completed peer update is now available');
  hidden();
  assert.equal(cleared, true);
});
