import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source=fs.readFileSync(process.env.CARBON_REPORT_SOURCE || new URL('../../source/web/report.js',import.meta.url),'utf8');

function fixture({query='',site=true,normal=false,status=201,storage=new Map(),session=new Map(),fail=false}={}) {
  let clock=0;
  const timers=[],events={},requests=[];
  const store=m=>({getItem:k=>m.get(k)??null,setItem:(k,v)=>m.set(k,v)});
  const date=class extends Date {static now(){return 1800000000000+clock;}};
  const c={window:null,Date:date,Math:Object.assign(Object.create(Math),{random:()=>normal?.05:.9}),
    URL,URLSearchParams,TextEncoder,AbortSignal,Uint32Array,setTimeout,clearTimeout,clearInterval(){},
    setInterval(fn,ms){timers.push({fn,ms});},addEventListener(t,fn){(events[t] ||= []).push(fn);},
    console:{log(){},warn(){},info(){},error(){}},performance:{now:()=>clock,timeOrigin:1800000000000},
    document:{hidden:false,visibilityState:'visible',getElementById:()=>null,addEventListener(t,fn){(events[t] ||= []).push(fn);}},
    location:new URL('https://example.test/'+query),recompSite:site,
    navigator:{userAgent:'Windows Chrome',hardwareConcurrency:8},
    sessionStorage:store(session),localStorage:store(storage),devicePixelRatio:1,
    screen:{width:1280,height:720},innerWidth:1280,innerHeight:720,
    recompPageBuild:{id:'page-a'},recompRuntimeBuild:{id:'runtime-a',wasm:'wasm-a'},recompReportBuild:{id:'reporter-a'},
    recompServers:{internet:{url:'wss://relay.test/?key=secret'}},
    recompOnline:{name:'PRIVATE PLAYER'},recompLog:['PRIVATE LOG'],carbonGraphicsInfo:{vendor:'nvidia',architecture:'turing'},
    fetch:async(url,init)=>{requests.push({url,init,at:clock});if(fail)throw Error('offline');return {status,headers:{get:()=> '1800'}};},
  };
  c.window=c;vm.runInNewContext(source,c);
  const tick=()=>{for(const t of timers) if(t.ms===1000)t.fn();};
  const settle=async()=>{for(let i=0;i<8;i++) await Promise.resolve();};
  async function run(seconds,{fps=16,cls=2,epoch=1,w=1280,h=720,repeatRate=0,gpu=false,gameUpdate=null}={}) {
    assert.ok(c.recompPerformance,'native performance receiver must be installed');
    for(let s=0;s<seconds;s++) {
      const from=clock;
      for(let i=0;i<60;i++) {
        clock=from+(i+1)*1000/60;
        gameUpdate?.(clock);
        const fresh=Math.floor((i+1)*fps/60)>Math.floor(i*fps/60);
        if(fresh||repeatRate) {
          c.recompPerformance.frame(1800000000000+clock,cls,epoch,w,h,fresh?0:1,1,2,3,4,5,6,7);
          if(fresh)c.recompPerformance.render(1800000000000+clock,6,2,1);
        }
      }
      clock=from+1000;
      if(gpu)c.recompPerformance.gpu(1800000000000+clock-500,2.5,12);
      tick();await settle();
    }
  }
  const bodies=()=>requests.map(r=>JSON.parse(r.init.body.split('\n').slice(1).join('\n')));
  return {c,run,requests,bodies,events,settle,advance(ms){clock+=ms;tick();},storage,session};
}

test('automatic low-FPS report counts new gameplay frames, carries timings, and omits private report data',async()=>{
  const f=fixture();
  await f.run(19,{repeatRate:60,gpu:true});assert.equal(f.requests.length,0,'warm-up plus sustained-low threshold not reached');
  f.c.console.log('[host] audio sink: 800 pulls, 2 late (>1.5 periods), 799 starved, max gap 80.0 ms, max render 1.2 ms, period 42.7 ms, ahead 2048 frames');
  await f.run(2,{repeatRate:60,gpu:true});
  assert.equal(f.requests.length,1);
  const body=f.bodies()[0];
  assert.equal(f.c.recompPerformance.enabled,false,'stop native collection after a successful low-FPS capture');
  assert.equal(body.reason,'sustained-low-fps');assert.equal(body.fps,16);
  assert.equal(body.rows.at(-1).repeats,44);assert.equal(body.rows.at(-1).encodeMs,96);
  assert.equal(body.gpu.status,'measured');assert.equal(body.gpu.meanMs,2.5);
  assert.equal(body.audio.lateCallbacks,2);assert.equal(body.audio.maxRenderMs,1.2);
  assert.equal(body.build.wasm,'wasm-a');assert.equal(body.build.reporter,'reporter-a');
  assert.ok(f.requests[0].init.body.length<24000);
  assert.doesNotMatch(f.requests[0].init.body,/PRIVATE|secret|starved|controller|playerName/);
  assert.equal(f.requests[0].url,'https://relay.test/reports?key=secret');
  await f.run(100,{repeatRate:60});assert.equal(f.requests.length,1,'successful low report is not repeated');
});

test('menus, hidden tabs, resizing and visibility gaps cannot cause a false low-FPS report',async()=>{
  const f=fixture();await f.run(40,{cls:1});assert.equal(f.requests.length,0);
  f.c.document.hidden=true;await f.run(40);assert.equal(f.requests.length,0);
  f.c.document.hidden=false;f.events.visibilitychange.forEach(fn=>fn());
  await f.run(18);await f.run(18,{w:1920,h:1080});assert.equal(f.requests.length,0);
  f.advance(20000);await f.run(18,{w:1920,h:1080});assert.equal(f.requests.length,0,'long event-loop gaps reset the window');
  await f.run(3,{w:1920,h:1080});assert.equal(f.requests.length,1);
});

test('missing GPU samples remain unavailable and normal sessions are sampled separately',async()=>{
  const f=fixture({normal:true});await f.run(42,{fps:60});
  assert.equal(f.requests.length,1);const b=f.bodies()[0];
  assert.equal(b.reason,'normal-sample');assert.equal(b.fps,60);
  assert.equal(b.gpu.status,'unavailable');assert.equal(b.gpu.meanMs,null);assert.equal(b.audio,null);
  assert.equal(b.rows.length,30);await f.run(80,{fps:60});assert.equal(f.requests.length,1);
});

test('failed uploads are bounded across reloads, shared-origin cooldown and server backoff',async()=>{
  const f=fixture({status:429});await f.run(30);assert.equal(f.requests.length,1);
  await f.run(700);assert.equal(f.requests.length,1,'Retry-After extends the ten minute cooldown');
  await f.run(1150);assert.equal(f.requests.length,2);
  await f.run(2000);assert.equal(f.requests.length,2,'two attempts per tab session, including failures');
  const reload=fixture({storage:f.storage,session:f.session});assert.equal(reload.c.recompPerformance,undefined);assert.equal(reload.requests.length,0);
  const other=fixture({storage:f.storage});await other.run(60);assert.equal(other.requests.length,0,'shared origin cooldown applies across tabs');
  const offline=fixture({fail:true});await offline.run(600);assert.equal(offline.requests.length,1,'network failure has no immediate retry');
});

test('local pages and explicit opt-out do not install a collector or upload anything',async()=>{
  for(const opts of [{site:false},{query:'?RECOMP_PERF_REPORTS=0'}]) {
    const f=fixture(opts);assert.equal(f.c.recompPerformance,undefined);f.advance(900000);await f.settle();assert.equal(f.requests.length,0);
  }
});

test('a queued upload is cancelled when the player leaves the measured scene',async()=>{
 const f=fixture();let release;
 f.c.navigator.locks={request:async(_name,claim)=>{await new Promise(resolve=>{release=resolve});return claim();}};
 await f.run(22);assert.equal(typeof release,'function');
 f.c.document.hidden=true;f.events.visibilitychange.forEach(fn=>fn());
 release();await f.settle();assert.equal(f.requests.length,0,'a delayed claim cannot upload a stale scene');
});


test('unclassified Carbon frames use advancing native gameplay state and reject menus, pause and torn reads',async()=>{
  const f=fixture();
  const memory={buffer:new SharedArrayBuffer(0x1000)},u=new Uint32Array(memory.buffer),i=new Int32Array(memory.buffer);
  f.c.carbonRaceBlock={memory,ptr:0};u[0]=0x31435243;u[1]=1;u[0x268/4]=123;
  const update=clock=>{Atomics.add(i,0x260/4,1);u[0x2c8/4]=clock;Atomics.add(i,0x260/4,1);};
  u[0x2cc/4]=3;await f.run(25,{cls:-1,gameUpdate:update});assert.equal(f.requests.length,0,'front end');
  u[0x2cc/4]=6;await f.run(12,{cls:-1,gameUpdate:clock=>{update(clock);u[0x2b0/4]=clock;}});
  await f.run(25,{cls:-1,gameUpdate:update});assert.equal(f.requests.length,0,'native pause freezes race time while the general clock advances');
  Atomics.add(i,0x260/4,1);await f.run(25,{cls:-1});assert.equal(f.requests.length,0,'writer in progress');Atomics.add(i,0x260/4,1);
  await f.run(23,{cls:-1,gameUpdate:clock=>{update(clock);u[0x2b0/4]=clock;}});assert.equal(f.requests.length,1,'active Carbon race must report despite unknown renderer scene');
  assert.equal(f.bodies()[0].reason,'sustained-low-fps');assert.equal(f.bodies()[0].fps,16);
});


test('free roam reports from the existing driving state with no race timer',async()=>{
 const f=fixture(),memory={buffer:new SharedArrayBuffer(0x1000)},u=new Uint32Array(memory.buffer),i=new Int32Array(memory.buffer);
 f.c.carbonRaceBlock={memory,ptr:0};f.c.carbonMpBlock={memory,ptr:0x800};u[0]=0x31435243;u[1]=1;u[0x268/4]=123;u[0x2cc/4]=6;
 const mp=0x800/4;u[mp]=0x31504d43;u[mp+1]=2;
 const update=clock=>{Atomics.add(i,0x260/4,1);u[0x2c8/4]=clock;Atomics.add(i,0x260/4,1);};
 await f.run(25,{cls:-1,gameUpdate:update});assert.equal(f.requests.length,0,'no live player');
 u[mp+4]=1;await f.run(23,{cls:-1,gameUpdate:update});assert.equal(f.requests.length,1);
});
