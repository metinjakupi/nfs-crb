import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const html = await readFile(new URL('../../source/kit/web/player/index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const settle = async () => { for (let i=0;i<8;i++) await new Promise(r=>setImmediate(r)); };
async function page({ adapters = ['ok'], storageError, workerResult } = {}) {
  const node = () => ({ textContent:'', hidden:false, append(){}, remove(){}, addEventListener(){}, focus(){}, style:{} });
  const status=node(), canvas=node(), appended=[], calls=[];
  const dir={ async getDirectoryHandle(){return dir;}, async getFileHandle(name){
    if(name!=='.stamp'||workerResult) throw Error('missing');
    return {getFile:async()=>({text:async()=> 'verified'})};
  }};
  let workerStopped=false;
  const c={URL,URLSearchParams,TextEncoder,Error,Number,Date,Promise,console:{warn(){},error(){},log(){}},
    location:new URL('http://localhost/nfsc/'),screen:{width:1920,height:1080},innerWidth:1280,innerHeight:720,devicePixelRatio:1,crossOriginIsolated:true,
    matchMedia:()=>({matches:false}),setInterval:()=>1,clearInterval(){},
    document:{documentElement:{dataset:{}},getElementById:id=>id==='status'?status:canvas,createElement:node,body:{append:n=>appended.push(n)}},
    addEventListener(){}, navigator:{storage:{getDirectory:async()=>{if(storageError)throw storageError;return dir;},estimate:async()=>({quota:10,usage:10})},gpu:{requestAdapter:async options=>{
      calls.push(options);const next=adapters.shift();if(next instanceof Error)throw next;if(next==='ok')return {info:{},features:new Set(),limits:{},requestDevice:async()=>({lost:new Promise(()=>{}),addEventListener(){}})};return null;
    }}},fetch:async()=>({json:async()=>[{id:'nfsc',title:'Carbon',sha256:'verified',executable:'NFSC.exe',streamAssets:!!workerResult,downloadManifest:'manifest'}]}),
    BroadcastChannel:class {postMessage(){}},Worker:class {postMessage(){queueMicrotask(()=>this.onmessage({data:{result:workerResult}}));}terminate(){workerStopped=true;}}
  };
  c.window=c;vm.createContext(c);vm.runInContext(script,c);await settle();c.recompBoot();await settle();
  return {c,status,calls,appended,workerStopped};
}
for(const first of [null,new Error('adapter request rejected')]) test(`startup falls back after ${first?'rejection':'no adapter'}`,async()=>{
  const p=await page({adapters:[first,'ok']});assert.equal(p.calls.length,2);assert.ok(p.c.Module);assert.equal(p.c.recompFailure,undefined);
});
test('both graphics requests failing show recovery instructions instead of leaking an exception',async()=>{
 const p=await page({adapters:[new Error('preferred failed'),new Error('default failed')]});assert.equal(p.calls.length,2);assert.match(p.status.textContent,/hardware acceleration/);assert.equal(p.c.Module,undefined);
});
test('storage exceptions and worker noSpace stop startup with save-preserving recovery',async()=>{
 for(const options of [{storageError:Object.assign(new Error('quota exceeded'),{name:'QuotaExceededError'})},{workerResult:{result:'noSpace',needed:2048,free:1024}}]){
  const p=await page(options);assert.equal(p.c.recompFailure.kind,'startup-storage');assert.match(p.status.textContent,/Your saves are kept/);assert.equal(p.calls.length,0);assert.equal(p.c.Module,undefined);if(options.workerResult)assert.equal(p.workerStopped,true);
 }
});
