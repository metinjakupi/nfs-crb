import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import * as core from '../../source/kit/web/launcher/core.js';
const source = await readFile(process.env.CARBON_WORKER_SOURCE || new URL('../../source/kit/web/launcher/worker.js', import.meta.url), 'utf8');
// Load the complete worker with its real importer; only browser I/O is supplied.
async function run(responses) {
 const messages=[];let calls=0;
 const dir={getDirectoryHandle:async()=>dir};
 const c={...core,URL,AbortController,TransformStream,TextDecoder,TextEncoder,Uint8Array,Response,Error,
  self:{location:new URL('https://example.test/worker.js')},
  navigator:{storage:{getDirectory:async()=>dir,estimate:async()=>({quota:1e9,usage:0})}},
  postMessage:m=>messages.push(m),setTimeout:fn=>{queueMicrotask(fn);},
  fetch:async()=>{calls++;const value=responses.shift();if(value instanceof Error)throw value;return value;}};
 vm.createContext(c);vm.runInContext(source.replace(/^import .* from "\.\/core.js";\r?\n/m,''),c);
 // Manifest is valid, but intentionally lacks a required data directory.
 await c.onmessage({data:{id:7,cmd:'import',kind:'download',game:{id:'test',executable:'GAME.EXE',requiredDirs:['DATA'],downloadManifest:'https://example.test/manifest.json'}}});
 return {messages,calls,result:messages.find(m=>m.id===7)};
}
const manifest=()=>new Response(JSON.stringify({base:'https://example.test/game/',files:[{path:'GAME.EXE',size:5,mtime:0}]}));
test('worker retries a transient manifest fetch and reaches real import validation',async()=>{
 const r=await run([new TypeError('connection reset'),manifest()]);
 assert.equal(r.calls,2);assert.equal(r.result.result.result,'incomplete');
 assert.equal(r.messages[0].diagnostic.attempt,1);
});
test('worker preserves exhausted network error details and does not retry HTTP 404',async()=>{
 const e=Object.assign(new TypeError('network unavailable'),{stack:'worker download stack'});
 const r=await run([e,e,e]);assert.equal(r.calls,3);assert.equal(r.result.name,'TypeError');
 assert.equal(r.result.stack,'worker download stack');assert.equal(r.result.context.url,'https://example.test/manifest.json');
 assert.equal(r.result.context.attempt,3);
 const missing=await run([new Response('missing',{status:404})]);
 assert.equal(missing.calls,1);assert.equal(missing.result.context.status,404);
});
