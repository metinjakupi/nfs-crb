import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
const source=process.env.CARBON_CUSTOM_INSTALL_SOURCE || new URL('../../source/web/custom-install.js',import.meta.url).pathname;
const {prepareCustomization}=await import(pathToFileURL(source).href);
const invalidCatalog=()=>new Response(JSON.stringify({version:'unsupported'}),{status:200});
function fixture(t, fetch) {
  const keys=['fetch','navigator','carbonDiagnostics'];
  const before=keys.map(k=>[k,Object.getOwnPropertyDescriptor(globalThis,k)]);
  t.after(()=>{for(const[k,d]of before)if(d)Object.defineProperty(globalThis,k,d);else delete globalThis[k];});
  for(const[k,v]of Object.entries({fetch,navigator:{locks:{request:async(_name,_options,fn)=>fn()},storage:{getDirectory:async()=>{throw Error('Invalid catalogs must never open storage');}}},carbonDiagnostics:{downloads:[]}}))Object.defineProperty(globalThis,k,{value:v,configurable:true,writable:true});
  return (assetBase='https://assets.example/custom/')=>prepareCustomization(()=>{},{assetBase});
}
for(const [name,first]of [
  ['connection failure',()=>{throw new TypeError('Failed to fetch');}],
  ['interrupted HTTP 200 body',()=>new Response(new ReadableStream({start(c){c.error(new TypeError('Connection interrupted'));}}),{status:200})],
  ['temporary server failure',()=>new Response(null,{status:503})],
])test(`${name} retries and reaches catalog validation without touching storage`,async t=>{
  let calls=0;const run=fixture(t,async()=>++calls===1?first():invalidCatalog());
  await assert.rejects(run(),/Customization runtime and catalog differ/);
  assert.equal(calls,2);
  assert.equal(carbonDiagnostics.downloads[0].attempt,1);
  assert.ok(carbonDiagnostics.downloads[0].error);
  assert.equal(carbonDiagnostics.downloads.at(-1).status,200);
});
test('missing files fail immediately and retain the exact resource and status',async t=>{
  let calls=0;const run=fixture(t,async()=>{calls++;return new Response(null,{status:404});});
  await assert.rejects(run(),/HTTP 404/);assert.equal(calls,1);
  assert.deepEqual(carbonDiagnostics.downloads.map(x=>[x.url,x.status,x.attempt]),[['https://assets.example/custom/manifest.json',404,1]]);
});
test('persistent network failure stops after three attempts and removes URL secrets from diagnostics',async t=>{
  let calls=0;const run=fixture(t,async()=>{calls++;throw new TypeError('Failed to fetch');});
  await assert.rejects(run('https://assets.example/custom/?token=private&next='),/Completed downloads are kept/);
  assert.equal(calls,3);assert.deepEqual(carbonDiagnostics.downloads.map(x=>x.attempt),[1,2,3]);
  assert.equal(JSON.stringify(carbonDiagnostics.downloads).includes('private'),false);
});
test('invalid JSON and memory-allocation failures are not retried',async t=>{
  let calls=0;const run=fixture(t,async()=>{calls++;return new Response('not JSON');});
  await assert.rejects(run(),SyntaxError);assert.equal(calls,1);
  globalThis.fetch=async()=>{calls++;throw new RangeError('Allocation failed');};
  await assert.rejects(run(),/Allocation failed/);assert.equal(calls,2);
});
