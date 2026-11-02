import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const html = await readFile(process.env.CARBON_PLAYER_SOURCE || new URL('../../source/kit/web/player/index.html', import.meta.url), 'utf8');
// These startup fixtures isolate graphics/download recovery; the real customization
// installer is verified in an OPFS browser against its complete asset catalog.
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).find(s => s.includes("window.recompBoot ="))
  .replace('import(new URL("./custom-install.js", document.baseURI).href)', 'Promise.resolve({prepareCustomization:async()=>{}})');
const settle = async () => { for (let i=0;i<8;i++) await new Promise(r=>setImmediate(r)); };
async function page({ adapters = ['ok'], storageError, workerResult, workerResults, isolated = true, userAgent = '', runtimeFetch, wasm = WebAssembly, pageUrl = 'http://localhost/nfsc/', perf = false, deviceFeatures = [], deviceRequest, adapterInfo = {}, fallbackAdapter = false, runtimeBytes, clock = Date, observerThrows = false } = {}) {
  const node = () => ({ textContent:'', hidden:false, append(){}, remove(){}, addEventListener(){}, focus(){}, setAttribute(k,v){this[k]=v},removeAttribute(k){delete this[k]}, style:{} });
  const status=node(), canvas=node(), appended=[], calls=[], events={}, nodes={status,canvas},intervals=[],observers=[];
  const location=new URL(pageUrl);let replaced='';location.replace=url=>{replaced=String(url)};
  const queuedWorkers=workerResults ? [...workerResults] : null;
  const dir={ async getDirectoryHandle(){return dir;}, async getFileHandle(name){
    if(name!=='.stamp'||workerResult||queuedWorkers) throw Error('missing');
    return {getFile:async()=>({text:async()=> 'verified'})};
  }};
  let workerStopped=0,workerCount=0;
  const c={recompPerformance:perf ? {enabled:true} : undefined,URL,URLSearchParams,TextEncoder,Error,Number,Date:clock,Promise,Response,ReadableStream,Uint8Array,recompPageBuild:runtimeBytes?{wasmBytes:runtimeBytes}:undefined,performance:{now:()=>clock.now()},PerformanceObserver:class {constructor(fn){observers.push(fn)}observe(){if(observerThrows)throw Error("Unsupported timing observer")}disconnect(){}},console:{warn(){},error(){},log(){}},
    location,screen:{width:1920,height:1080},innerWidth:1280,innerHeight:720,devicePixelRatio:1,crossOriginIsolated:isolated,WebAssembly:wasm,setTimeout:fn=>{queueMicrotask(fn);return 1;},
    matchMedia:()=>({matches:false}),setInterval:fn=>{intervals.push(fn);return intervals.length},clearInterval(){},
    document:{baseURI:'http://localhost/nfsc/',documentElement:{dataset:{}},getElementById:id=>nodes[id] ||= node(),createElement:node,body:{append:n=>appended.push(n)}},
    addEventListener(type,fn){events[type]=fn;}, navigator:{userAgent,storage:{getDirectory:async()=>{if(storageError)throw storageError;return dir;},estimate:async()=>({quota:10,usage:10})},gpu:{requestAdapter:async options=>{
      calls.push(options);const next=adapters.shift();if(next instanceof Error)throw next;if(next==='ok')return {info:adapterInfo,isFallbackAdapter:fallbackAdapter,features:new Set(deviceFeatures),limits:{},requestDevice:deviceRequest || (async()=>({lost:new Promise(()=>{}),addEventListener(){}}))};return null;
    }}},fetch:async(url,...args)=>String(url).endsWith('.wasm') ? runtimeFetch(url,...args) : ({json:async()=>[{id:'nfsc',title:'Carbon',sha256:'verified',executable:'NFSC.exe',streamAssets:!!workerResult||!!queuedWorkers,downloadManifest:'manifest'}]}),
    BroadcastChannel:class {postMessage(){}},Worker:class {constructor(){workerCount++}postMessage(){const data=queuedWorkers?.shift()||{result:workerResult};queueMicrotask(()=>this.onmessage({data}));}terminate(){workerStopped++}}
  };
  c.window=c;vm.createContext(c);vm.runInContext(script,c);await settle();c.recompBoot();await settle();
  return {c,status,calls,appended,workerStopped,workerCount,nodes,intervals,resource:entry=>observers.forEach(fn=>fn({getEntries:()=>[entry]})),get replaced(){return replaced},events};
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

test('early errors retain source and rejection detail without replacing the first failure',async()=>{
 const p=await page();
 p.events.error({message:'Script error.',filename:'https://cdn.example/runtime.js?key=secret',lineno:12,colno:34});
 assert.equal(p.c.recompFailure.source,'https://cdn.example/runtime.js');
 assert.equal(p.c.recompFailure.line,12);assert.equal(p.c.recompFailure.column,34);
 assert.equal(p.c.recompFailure.masked,true);
 p.events.unhandledrejection({reason:{name:'GPUFailure',message:'device request failed',code:17}});
 assert.equal(p.c.recompFailure.message,'Script error.');
 assert.equal(p.c.carbonDiagnostics.failures.at(-1).message,'device request failed');
 assert.match(p.status.textContent,/Script error/);
});
test('loading progress never compares decoded bytes against a smaller compressed size',async()=>{
 const p=await page();
 p.c.Module.setStatus('Downloading data... (11704261/634877)');
 assert.doesNotMatch(p.nodes['cl-data-text'].textContent,/11704261\/634877/);
 assert.match(p.nodes['cl-data-text'].textContent,/received/);
 assert.equal(p.c.carbonDiagnostics.runtimeDataProgress.comparable,false);
 p.c.Module.getPreloadedPackage('CarbonRecomp.data',200);
 p.c.Module.setStatus('Downloading data... (100/200)');
 assert.equal(p.nodes['cl-data'].value,100);assert.equal(p.nodes['cl-data'].max,200);
});
test('worker failure preserves error type, stack and failing file in startup reports',async()=>{
 const p=await page({workerResults:[{error:'could not open file',name:'NotReadableError',stack:'worker stack',context:{command:'streamSetup',url:'https://assets.example/file.bin'}}]});
 assert.equal(p.c.recompFailure.name,'NotReadableError');assert.equal(p.c.recompFailure.stack,'worker stack');
 assert.equal(p.c.carbonDiagnostics.workerFailure.url,'https://assets.example/file.bin');
});
test('graphics diagnostics retain failed preference and the adapter used for the device',async()=>{
 const p=await page({adapters:[new Error('driver denied preferred adapter'),'ok']});
 assert.equal(p.c.carbonDiagnostics.adapterRequests[0].error.message,'driver denied preferred adapter');
 assert.equal(p.c.carbonGraphicsInfo.source,'active device adapter');
 assert.equal(p.c.carbonGraphicsInfo.preference,'default');
});

test('optional resource failure is recorded without stopping a running game',async()=>{
 const p=await page();p.c.Module.callMain=()=>{};p.c.Module.onRuntimeInitialized();
 p.events.error({target:{src:'https://example.test/optional.png?token=secret'}});
 assert.equal(p.c.recompFailure,undefined);assert.equal(p.status.hidden,true);
 assert.equal(p.c.carbonDiagnostics.failures.at(-1).source,'https://example.test/optional.png');
 assert.equal(p.c.carbonDiagnostics.failures.at(-1).kind,'resource');
});

for (const supported of [true, false]) test(`optional GPU timing failure preserves game startup (supported=${supported})`,async()=>{
 const requests=[];
 const p=await page({perf:true,deviceFeatures:supported?['timestamp-query']:[],deviceRequest:async options=>{
  requests.push([...options.requiredFeatures]);
  if(options.requiredFeatures.includes('timestamp-query'))throw new Error('driver rejected optional timestamps');
  return {lost:new Promise(()=>{}),addEventListener(){}};
 }});
 assert.ok(p.c.Module);assert.equal(p.c.recompFailure,undefined);
 assert.deepEqual(requests,supported ? [['timestamp-query'],[]] : [[]]);
 assert.equal(p.c.carbonDiagnostics.requestedDevice.features.includes('timestamp-query'),false);
});

test('software graphics stops before a device or native runtime is created', async()=>{
 for (const options of [{fallbackAdapter:true},{adapterInfo:{vendor:'google',architecture:'swiftshader'}}]) {
  let requested=0;
  const p=await page({...options,deviceRequest:async()=>{requested++;throw Error('must not start')}});
  assert.equal(requested,0);assert.equal(p.c.Module,undefined);
  assert.match(p.status.textContent,/software renderer/i);assert.match(p.status.textContent,/hardware acceleration/i);
 }
});
test('device loss unloads the failed runtime and the recovery document never boots it again', async()=>{
 let lose;const lost=new Promise(resolve=>{lose=resolve});
 const p=await page({deviceRequest:async()=>({lost,addEventListener(){}})});
 assert.ok(p.c.Module);lose({message:'A valid external Instance reference no longer exists.'});await settle();
 assert.equal(new URL(p.replaced).searchParams.get('graphicsRecovery'),'1');
 const recovery=await page({pageUrl:p.replaced});
 assert.equal(recovery.calls.length,0);assert.equal(recovery.workerCount,0);assert.equal(recovery.c.Module,undefined);
 assert.match(recovery.status.textContent,/graphics device stopped/i);assert.match(recovery.status.textContent,/saves are kept/i);
});

test('loader preserves the original network Response for browser code caching',async()=>{
 const response=new Response(tinyWasm,{headers:{'Content-Type':'application/wasm'}});let received;
 const wasm={...WebAssembly,instantiateStreaming:async(r,imports)=>{received=r;return WebAssembly.instantiateStreaming(r,imports)}};
 const p=await page({wasm,runtimeFetch:async()=>response});await instantiate(p);assert.equal(received,response);
});
test('loader separates completed data from pending code and offers recovery during a long wait',async()=>{
 let now=0,body;const clock=class extends Date {static now(){return now}};
 const p=await page({clock,runtimeBytes:8,runtimeFetch:async()=>new Response(new ReadableStream({start(c){body=c}}),{headers:{'Content-Type':'application/wasm','Content-Encoding':'gzip','Content-Length':'4'}})});
 let received;p.c.Module.instantiateWasm({},(instance)=>{received=instance});await settle();
 body.enqueue(tinyWasm.slice(0,4));await settle();
 p.c.Module.getPreloadedPackage('CarbonRecomp.data',13377686);
 p.c.Module.setStatus('Downloading data... (13377686/642971)');
 assert.match(p.nodes['cl-code-text'].textContent,/Loading/);
 assert.match(p.nodes['cl-data-text'].textContent,/Ready/);
 assert.equal(p.nodes['cl-code'].value,undefined);
 now=50000;p.intervals.at(-1)();assert.match(p.nodes['cl-message'].textContent,/Game code is still loading/);assert.equal(p.nodes['cl-reload'].hidden,false);
 body.enqueue(tinyWasm.slice(4));body.close();await settle();
 assert.ok(received instanceof WebAssembly.Instance);assert.equal(p.nodes['cl-reload'].hidden,true);
 p.c.Module.callMain=()=>{};p.c.Module.onRuntimeInitialized();assert.equal(p.nodes['runtime-loader'].hidden,true);
});
test('loader identifies preparation separately after the code stream ends',async()=>{
 let finish;const gate=new Promise(r=>{finish=r});let consumed;
 const wasm={...WebAssembly,instantiateStreaming:async(response,imports)=>{const bytes=await response.arrayBuffer();consumed=true;await gate;return WebAssembly.instantiate(bytes,imports)}};
 const p=await page({runtimeBytes:8,wasm,runtimeFetch:async()=>new Response(tinyWasm,{headers:{'Content-Type':'application/wasm'}})});
 p.c.Module.getPreloadedPackage('CarbonRecomp.data',2);p.c.Module.setStatus('Downloading data... (2/2)');
 p.c.Module.instantiateWasm({},()=>{});await settle();assert.equal(consumed,true);
 p.resource({name:p.c.carbonDiagnostics.downloads.find(d=>d.phase==='runtime').url,startTime:Date.now(),decodedBodySize:8});
 assert.match(p.nodes['cl-title'].textContent,/Preparing/);assert.match(p.nodes['cl-prepare-text'].textContent,/Preparing/);
 finish();await settle();
});

test('missing resource timing support does not prevent startup or streaming compilation',async()=>{
 const p=await page({observerThrows:true,runtimeFetch:async()=>new Response(tinyWasm,{headers:{'Content-Type':'application/wasm'}})});
 assert.equal(p.c.recompFailure,undefined);assert.ok((await instantiate(p)).instance instanceof WebAssembly.Instance);
});
