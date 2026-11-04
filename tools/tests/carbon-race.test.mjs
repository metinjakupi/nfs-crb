import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';

const source = await readFile(process.env.CARBON_RACE_SOURCE || new URL('../../source/web/race.js', import.meta.url), 'utf8');

class Element {
  constructor(tag = 'div') {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.hidden = false;
    this.textContent = '';
    this.className = '';
    this.events = new Map();
  }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = [...children]; }
  addEventListener(type,fn) { this.events.set(type,fn); }
  click() { this.events.get("click")?.({preventDefault(){}}); }
  setAttribute() {}
  get childElementCount() { return this.children.filter(value => value instanceof Element).length; }
}

function harness() {
  const listeners = new Map();
  const intervals = [];
  const sent = [];
  const navigation = [];
  let now = 1000;
  const buffer = new SharedArrayBuffer(4096);
  const u32 = new Uint32Array(buffer);
  u32[0] = 0x31435243;
  u32[0x2cc / 4] = 3;
  const context = {
    console,
    location: {pathname:"/play/nfsc/",replace:path=>navigation.push(path)},
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
    navigation,
    tick,
    setNow(value) { now = value; },
    game(values) {
      const at = offset => offset >>> 2;
      if ('phase' in values) u32[at(0x264)] = values.phase;
      if ('caught' in values) u32[at(0x26c)] = values.caught;
      if ('caughtKey' in values) u32[at(0x270)] = values.caughtKey;
      if ('caughtType' in values) u32[at(0x274)] = values.caughtType;
      if ('version' in values) u32[1] = values.version;
      if ('flowFlags' in values) u32[at(0x2cc)] = values.flowFlags;
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

const nodeText = node => typeof node === 'string' ? node : node.textContent + node.children.map(nodeText).join(' ');
function findButton(node,label) {
 if (!(node instanceof Element)) return null;
 if (node.tagName==='BUTTON' && nodeText(node)===label) return node;
 for (const child of node.children) { const match=findButton(child,label);if(match)return match; }
 return null;
}
for(const busy of [false,true]) test(`cancelled loading provides a working exit while native state is unavailable: ${busy}`,()=>{
 const h=harness();start(h);h.game({phase:0,flow:6,session:77,busy});
 h.setNow(62000);h.tick();
 const exit=findButton(h.context.document.body,'Exit to start page');
 assert.ok(exit,'cancelled race must always offer exit');
 assert.equal(findButton(h.context.document.body,'Dismiss'),null,'cannot dismiss into loading');
 let quit=false;h.context.addEventListener('recomp-quit',()=>{quit=true;});
 exit.click();assert.equal(quit,true);assert.deepEqual(h.navigation,['/play/nfsc/']);
 assert.equal(h.sent.some(m=>m.k==='finish'),false);
});
test('a cancelled race can be dismissed only after the native game is back in its menus',()=>{
 const h=harness();start(h);h.game({phase:3,flow:6,session:77});h.setNow(62000);h.tick();
 assert.equal(findButton(h.context.document.body,'Dismiss'),null);
 h.game({phase:0,flow:3});h.context.dispatchEvent({type:'recomp-online',detail:{me:1}});
 const dismiss=findButton(h.context.document.body,'Dismiss');assert.ok(dismiss);dismiss.click();
 assert.equal(h.context.recompRace.state().race,null);
});

test('Online panel joins and leaves the selected invitation rather than the newest one',()=>{
 const h=harness();h.game({flow:3,car:123});
 for(const [id,race] of [[2,78],[3,79]]) h.message({t:'race',k:'lobby',id,race,name:`Host ${id}`,key:0x8ef528ca,type:2,opts:'00'.repeat(40),roster:[{id,name:`Host ${id}`,car:1}]});
 h.context.recompRace.join(78);
 assert.equal(h.sent.at(-1).k,'join');assert.equal(h.sent.at(-1).race,78);
 h.context.recompRace.leave(78);
 assert.equal(h.sent.at(-1).k,'leave');assert.equal(h.sent.at(-1).race,78);
});


test('Pursuit Tag cannot be announced or launched through the ordinary Quick Race bridge', () => {
 const h=harness();h.game({car:123,flow:3,caught:0});h.tick();h.context.recompRace.host(true);
 h.game({caught:1,caughtKey:1234,caughtType:14});h.tick();
 assert.equal(h.context.recompRace.state().lobby,null);assert.equal(h.sent.some(m=>m.k==='lobby'),false);
 assert.match(nodeText(h.context.document.body),/Online → Custom Match/);
 h.message({t:'race',k:'lobby',id:2,race:88,key:1234,type:14,opts:'00'.repeat(40),roster:[{id:2,name:'Friend',car:1}]});
 assert.equal(h.context.recompRace.state().invites.length,0);
 h.message({t:'race',k:'start',id:2,race:88,key:1234,type:14,opts:'00'.repeat(40),roster:[{id:1,name:'Test',car:1},{id:2,name:'Friend',car:1}]});
 assert.equal(h.context.recompRace.state().race,null);
 assert.equal(new Uint32Array(h.context.carbonRaceBlock.memory.buffer)[0x24/4],0,'unsupported start must not queue a native launch');
});

test('cancelled race keeps its exit button across online-state refreshes and releases mouse capture',()=>{
 const h=harness();let released=0;h.context.document.pointerLockElement={};h.context.document.exitPointerLock=()=>{released++;};
 start(h);h.setNow(62000);h.tick();assert.equal(released,1);
 const button=findButton(h.context.document.body,'Exit to start page');
 h.context.dispatchEvent({type:'recomp-online',detail:{me:1}});
 assert.equal(findButton(h.context.document.body,'Exit to start page'),button);
});

function online(h,open,ids,me=1) { h.context.dispatchEvent({type:'recomp-online',detail:{open,me,people:ids.map(id=>({id,name:`Player${id}`}))}}); }
function invite(h,id=2) {h.game({car:123});h.message({t:'race',k:'lobby',id,race:78,key:1,type:2,opts:'00'.repeat(40),roster:[{id,name:'Host',car:1}]});}
test('a disconnected host cancels guest loading and a late start cannot revive the session',()=>{
 const h=harness();invite(h);h.context.recompRace.join();
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1,grid:0},{id:1,name:'Test',car:123,grid:1}]});
 online(h,true,[]);assert.equal(h.context.recompRace.state().race.cancelled,true);
 h.game({flow:3,phase:0});h.context.dispatchEvent({type:'recomp-online',detail:{me:1}});findButton(h.context.document.body,'Dismiss').click();
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,opts:'00'.repeat(40),roster:[{id:1,name:'Test',car:123,grid:0}]});
 assert.equal(h.context.recompRace.state().race,null);
});
test('a guest lost after Start cancels the committed race without releasing a partial roster',()=>{
 const h=harness();start(h);h.message({t:'race',k:'ready',id:1,race:77});online(h,true,[]);
 assert.equal(h.sent.filter(m=>m.k==='go').length,0);assert.equal(h.sent.filter(m=>m.k==='cancel').length,1);
 assert.equal(h.context.recompRace.state().race.cancelled,true);
});
test('network loss retires invitations so a delayed start does not launch on reconnect',()=>{
 const h=harness();invite(h);h.context.recompRace.join();online(h,false,[]);online(h,true,[2]);
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,opts:'00'.repeat(40),roster:[{id:1,name:'Test',car:123,grid:0}]});
 assert.equal(h.context.recompRace.state().race,null);assert.equal(h.context.recompRace.state().invites.length,0);
});
test('a relay identity change cancels the previous race loading session',()=>{
 const h=harness();start(h);online(h,true,[2],9);assert.equal(h.context.recompRace.state().race.cancelled,true);
 assert.equal(h.context.recompRace.state().me,9);
});

function worldInvite(h) { h.message({t:'race',k:'lobby',id:2,race:78,key:1,type:2,world:true,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1}]}); }
test('world-map invitation requires native support and free roam',()=>{
 const h=harness();h.game({car:123,flow:6});worldInvite(h);assert.equal(h.context.recompRace.state().invites.length,0);
 h.game({version:2,flow:3});worldInvite(h);h.context.recompRace.join();assert.equal(h.sent.some(m=>m.k==='join'),false);
 h.game({flowFlags:6|0x800});h.context.recompRace.join();assert.equal(h.sent.at(-1).k,'join');assert.equal(h.sent.at(-1).world,true);
});
test('hosting from free roam opens the map and captures a world-map lobby',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800,caught:0});h.tick();h.context.recompRace.host(true);
 const memory=new Uint32Array(h.context.carbonRaceBlock.memory.buffer);
 assert.equal(memory[0x10/4]&5,5);assert.equal(memory[0x54/4],1);
 h.game({caught:1,caughtKey:1234,caughtType:2});h.tick();
 assert.equal(h.context.recompRace.state().lobby.world,true);assert.equal(h.sent.find(m=>m.k==='lobby').world,true);
 h.message({t:'race',k:'join',id:2,race:h.context.recompRace.state().lobby.race,car:222});
 assert.equal(h.context.recompRace.state().lobby.roster.length,1,'old clients cannot silently join through Quick Race');
});
test('host loss during an active world race requests native abandonment without a finish',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800});worldInvite(h);h.context.recompRace.join();
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,world:true,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1,grid:0},{id:1,name:'Test',car:123,grid:1}]});
 h.message({t:'race',k:'go',id:2,race:78});h.game({phase:4,session:78});online(h,true,[]);
 assert.equal(h.context.recompRace.state().race.cancelled,true);
 const memory=new Uint32Array(h.context.carbonRaceBlock.memory.buffer);assert.equal(memory[0x10/4]&8,8);assert.equal(memory[0x24/4],0);
 h.game({done:1,time:10});h.tick();assert.equal(h.sent.some(m=>m.k==='finish'),false);
});

test('a world race cannot pull a player in without an accepted invitation',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800});worldInvite(h);
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,world:true,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1,grid:0},{id:1,name:'Test',car:123,grid:1}]});
 assert.equal(h.context.recompRace.state().race,null);assert.equal(new Uint32Array(h.context.carbonRaceBlock.memory.buffer)[0x24/4],0);
});
test('a joined player who starts a career event is not queued for a later world race',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800});worldInvite(h);h.context.recompRace.join();h.game({flowFlags:6|0x200});
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,world:true,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1,grid:0},{id:1,name:'Test',car:123,grid:1}]});
 assert.equal(h.context.recompRace.state().race,null);assert.equal(h.sent.at(-1).k,'leave');
});

test('world host releases the native world-map mode after returning to free roam',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800,caught:0});h.tick();h.context.recompRace.host(true);
 h.game({caught:1,caughtKey:1234,caughtType:2});h.tick();const l=h.context.recompRace.state().lobby;
 h.message({t:'race',k:'start',id:1,race:l.race,key:l.key,type:l.type,world:true,ai:true,opts:'00'.repeat(40),roster:[{id:1,name:'Test',car:123,grid:0}]});
 assert.equal(h.context.recompRace.state().race.ai,false,'world races cannot enable unsynchronised AI');
 h.game({phase:4,session:l.race,flowFlags:6|0x200});h.tick();h.game({phase:0,session:0,flowFlags:6|0x800});h.tick();
 assert.equal(h.context.recompRace.state().race,null);assert.equal(new Uint32Array(h.context.carbonRaceBlock.memory.buffer)[0x10/4]&4,0,'ordinary career events must work again');
});

test('an unexpected native exit tells the host to release the other world racers',()=>{
 const h=harness();h.game({version:2,car:123,flowFlags:6|0x800});worldInvite(h);h.context.recompRace.join();
 h.message({t:'race',k:'start',id:2,race:78,key:1,type:2,world:true,opts:'00'.repeat(40),roster:[{id:2,name:'Host',car:1,grid:0},{id:1,name:'Test',car:123,grid:1}]});
 h.message({t:'race',k:'go',id:2,race:78});h.game({phase:4,session:78});h.tick();
 h.game({phase:0,session:0,flowFlags:6|0x800});h.tick();
 assert.equal(h.sent.at(-1).k,'leave');assert.equal(h.sent.at(-1).race,78);assert.equal(h.context.recompRace.state().race,null);
});
