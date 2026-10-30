import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const html = await readFile(new URL('../../source/kit/web/player/index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const settle = async () => { for (let i=0;i<8;i++) await new Promise(r=>setImmediate(r)); };
async function page({ adapters = ['ok'], storageError, workerResult, workerResults, isolated = true, userAgent = '', runtimeFetch, wasm = WebAssembly, pageUrl = 'http://localhost/nfsc/' } = {}) {
  const node = () => ({ textContent:'', hidden:false, append(){}, remove(){}, addEventListener(){}, focus(){}, style:{} });
  const status=node(), canvas=node(), appended=[], calls=[];
  const location=new URL(pageUrl);let replaced='';location.replace=url=>{replaced=String(url)};
  const queuedWorkers=workerResults ? [...workerResults] : null;
  const dir={ async getDirectoryHandle(){return dir;}, async getFileHandle(name){
    if(name!=='.stamp'||workerResult||queuedWorkers) throw Error('missing');
    return {getFile:async()=>({text:async()=> 'verified'})};
  }};
  let workerStopped=0,workerCount=0;
  const c={URL,URLSearchParams,TextEncoder,Error,Number,Date,Promise,console:{warn(){},error(){},log(){}},
    location,screen:{width:1920,height:1080},innerWidth:1280,innerHeight:720,devicePixelRatio:1,crossOriginIsolated:isolated,WebAssembly:wasm,setTimeout:fn=>{queueMicrotask(fn);return 1;},
    matchMedia:()=>({matches:false}),setInterval:()=>1,clearInterval(){},
    document:{baseURI:'http://localhost/nfsc/',documentElement:{dataset:{}},getElementById:id=>id==='status'?status:canvas,createElement:node,body:{append:n=>appended.push(n)}},
    addEventListener(){}, navigator:{userAgent,storage:{getDirectory:async()=>{if(storageError)throw storageError;return dir;},estimate:async()=>({quota:10,usage:10})},gpu:{requestAdapter:async options=>{
      calls.push(options);const next=adapters.shift();if(next instanceof Error)throw next;if(next==='ok')return {info:{},features:new Set(),limits:{},requestDevice:async()=>({lost:new Promise(()=>{}),addEventListener(){}})};return null;
    }}},fetch:async(url,...args)=>String(url).endsWith('.wasm') ? runtimeFetch(url,...args) : ({json:async()=>[{id:'nfsc',title:'Carbon',sha256:'verified',executable:'NFSC.exe',streamAssets:!!workerResult||!!queuedWorkers,downloadManifest:'manifest'}]}),
    BroadcastChannel:class {postMessage(){}},Worker:class {constructor(){workerCount++}postMessage(){const data=queuedWorkers?.shift()||{result:workerResult};queueMicrotask(()=>this.onmessage({data}));}terminate(){workerStopped++}}
  };
  c.window=c;vm.createContext(c);vm.runInContext(script,c);await settle();c.recompBoot();await settle();
  return {c,status,calls,appended,workerStopped,workerCount,replaced};
}
for(const first of [null,new Error('adapter request rejected')]) test(`startup falls back after ${first?'rejection':'no adapter'}`,async()=>{
  const p=await page({adapters:[first,'ok']});assert.equal(p.calls.length,2);assert.ok(p.c.Module);assert.equal(p.c.recompFailure,undefined);
});
test('both graphics requests failing show recovery instructions instead of leaking an exception',async()=>{
 const p=await page({adapters:[new Error('preferred failed'),new Error('default failed')]});assert.equal(p.calls.length,2);assert.match(p.status.textContent,/hardware acceleration/);assert.equal(p.c.Module,undefined);
});
test('Android without a WebGPU adapter gets mobile instructions instead of desktop settings',async()=>{
 const p=await page({adapters:[null,null],userAgent:'Mozilla/5.0 (Linux; Android 10) SamsungBrowser/30.0 Chrome/143 Mobile'});
 assert.equal(p.calls.length,2);assert.match(p.status.textContent,/Android/);assert.match(p.status.textContent,/current Chrome/);assert.match(p.status.textContent,/cannot run this build/);assert.doesNotMatch(p.status.textContent,/Turn on hardware acceleration/);
});
test('storage exceptions and worker noSpace stop startup with save-preserving recovery',async()=>{
 for(const options of [{storageError:Object.assign(new Error('quota exceeded'),{name:'QuotaExceededError'})},{workerResult:{result:'noSpace',needed:2048,free:1024}}]){
  const p=await page(options);assert.equal(p.c.recompFailure.kind,'startup-storage');assert.match(p.status.textContent,/1\. Select Open game page below/);assert.match(p.status.textContent,/2\. Select Delete downloaded files/);assert.match(p.status.textContent,/keeps your saves and settings/);assert.equal(p.calls.length,0);assert.equal(p.c.Module,undefined);if(options.storageError)assert.match(p.status.textContent,/normal window, not a Private or Tor window/);if(options.workerResult)assert.equal(p.workerStopped,1);
 }
});
test('a stale startup storage handle is reopened once before play continues',async()=>{
 const stale={error:'An operation that depends on state cached in an interface object was made but the state had changed since it was read from disk.'};
 const p=await page({workerResults:[stale,{result:{result:'done',base:'https://assets.example',index:'/opfs/nfsc/game/.stream-index'}}]});
 assert.equal(p.workerCount,2);assert.equal(p.workerStopped,2);assert.ok(p.c.Module);assert.equal(p.c.recompFailure,undefined);
});
test('a stale storage handle retries only once and then keeps the save safe',async()=>{
 const stale={error:'An operation that depends on state cached in an interface object was made but the state had changed since it was read from disk.'};
 const p=await page({workerResults:[stale,stale]});
 assert.equal(p.workerCount,2);assert.equal(p.workerStopped,2);assert.equal(p.c.recompFailure.kind,'startup-storage');assert.match(p.status.textContent,/saves are kept/i);assert.equal(p.c.Module,undefined);
});
test('a trailing dot redirects to the canonical site before startup',async()=>{
 const p=await page({pageUrl:'https://nfscrb.xyz./nfsc/?room=test'});
 assert.equal(p.replaced,'https://nfscrb.xyz/nfsc/?room=test');assert.equal(p.calls.length,0);assert.equal(p.workerCount,0);assert.equal(p.c.Module,undefined);
});

for (const userAgent of ['Android; wv Instagram 452', 'iPhone Instagram 452']) test(`in-app browser explains how to leave it: ${userAgent}`, async()=>{
 const p=await page({isolated:false,userAgent});
 assert.equal(p.c.Module,undefined);assert.equal(p.calls.length,0);
 assert.match(p.status.textContent,/Instagram/);assert.match(p.status.textContent,/Open in browser/);
 assert.doesNotMatch(p.status.textContent,/Reload this page and try again/);
});
test('missing isolation in a regular browser is not labelled Instagram',async()=>{
 const p=await page({isolated:false,userAgent:'Chrome/156'});
 assert.match(p.status.textContent,/secure browser environment/);assert.doesNotMatch(p.status.textContent,/Instagram/);
});
const tinyWasm = new Uint8Array([0,97,115,109,1,0,0,0]);
async function instantiate(p) {
 let received;
 assert.equal(typeof p.c.Module.instantiateWasm,'function');
 p.c.Module.instantiateWasm({},(instance,module)=>{received={instance,module}});
 for(let i=0;i<100&&!received&&!p.c.recompFailure;i++)await settle();
 return received;
}
test('runtime download recovers from a broken response body and instantiates real wasm once',async()=>{
 let calls=0;
 const p=await page({runtimeFetch:async()=>++calls===1
   ? new Response(new ReadableStream({start(c){c.error(new TypeError('connection lost'))}}),{headers:{'Content-Type':'application/wasm'}})
   : new Response(tinyWasm,{headers:{'Content-Type':'application/wasm'}})});
 const received=await instantiate(p);
 assert.equal(calls,2);assert.ok(received.instance instanceof WebAssembly.Instance);assert.ok(received.module instanceof WebAssembly.Module);assert.equal(p.c.recompFailure,undefined);
});
test('runtime retry is bounded and gives recovery after repeated network failures',async()=>{
 let calls=0;const p=await page({runtimeFetch:async()=>{calls++;throw new TypeError('Load failed')}});
 assert.equal(await instantiate(p),undefined);assert.equal(calls,3);assert.match(p.status.textContent,/connection/);assert.match(p.status.textContent,/saves are kept/);
});
test('invalid wasm is not retried or labelled a network failure',async()=>{
 let calls=0;const p=await page({runtimeFetch:async()=>{calls++;return new Response('invalid',{headers:{'Content-Type':'application/wasm'}})}});
 assert.equal(await instantiate(p),undefined);assert.equal(calls,1);assert.match(p.c.recompFailure.message,/compile/i);
});
test('runtime loading supports a server without the wasm MIME type',async()=>{
 let calls=0;const p=await page({runtimeFetch:async()=>{calls++;return new Response(tinyWasm,{headers:{'Content-Type':'application/octet-stream'}})}});
 assert.ok((await instantiate(p)).instance instanceof WebAssembly.Instance);assert.equal(calls,1);
});
