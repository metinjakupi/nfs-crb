'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs/promises'),path=require('node:path'),os=require('node:os'),http=require('node:http'),{createHash}=require('node:crypto');
const {inspectGame,createLocalServer,adaptWorker}=require('../local-server.cjs');
test('local game transport validates the executable, serves exact ranges, rejects writes and does not expose unrelated files',async t=>{
 const temp=await fs.mkdtemp(path.join(os.tmpdir(),'carbon-launcher-'));t.after(()=>fs.rm(temp,{recursive:true,force:true}));
 const game=path.join(temp,'game'),runtime=path.join(temp,'runtime'),ui=path.join(temp,'ui');
 await fs.mkdir(game);await fs.mkdir(runtime);await fs.mkdir(ui);
 for(const folder of ['CARS','FRONTEND','GLOBAL','SOUND','TRACKS']){await fs.mkdir(path.join(game,folder));await fs.writeFile(path.join(game,folder,'asset.bin'),'0123456789');}
 for(const name of ['NFSC.exe','msvcr71.dll','msvcp71.dll','private.txt'])await fs.writeFile(path.join(game,name),'fixture');
 const hash=createHash('sha256').update('fixture').digest('hex');
 await assert.rejects(inspectGame(game),/supported PC v1.4/);
 const selected=await inspectGame(game,hash);
 await fs.writeFile(path.join(runtime,'games.json'),JSON.stringify([{id:'nfsc',title:'Carbon'}]));
 await fs.writeFile(path.join(runtime,'worker.js'),'if(remote&&url.protocol!=="https:")throw new Error("Game downloads require HTTPS");');
 await fs.writeFile(path.join(ui,'index.html'),'<h1>Launcher</h1>');
 const server=await createLocalServer({runtime,ui,port:0});server.setGame(selected);t.after(()=>server.close());
 const get=(url,options)=>fetch(server.origin+url,options);
 const manifest=await(await get('/local/manifest.json')).json();assert.equal(manifest.base,server.origin+'/game/');assert(!manifest.files.some(f=>f.path==='private.txt'));assert(manifest.files.some(f=>f.path==='NFSC.exe'));
 const range=await get('/game/CARS/asset.bin',{headers:{Range:'bytes=2-5'}});assert.equal(range.status,206);assert.equal(range.headers.get('content-range'),'bytes 2-5/10');assert.equal(await range.text(),'2345');assert.equal(range.headers.get('cross-origin-embedder-policy'),'require-corp');
 assert.equal((await get('/game/CARS/asset.bin',{headers:{Range:'bytes=99-'}})).status,416);
 assert.equal((await get('/game/private.txt')).status,404);
 assert.equal((await get('/game/NFSC.exe',{method:'POST',body:'changed'})).status,405);
 assert.equal((await get('/local/manifest.json',{headers:{Origin:'https://example.com'}})).status,403);
 const metadata=await(await get('/play/games.json')).json();assert.equal(metadata[0].downloadManifest,server.origin+'/local/manifest.json');
 const worker=await(await get('/play/worker.js')).text();assert(worker.includes('url.origin!==self.location.origin'));
 assert.equal(await fs.readFile(path.join(game,'NFSC.exe'),'utf8'),'fixture');
 // A file swapped for a link after selection must not escape the selected folder.
 await fs.writeFile(path.join(temp,'secret.bin'),'private');await fs.unlink(path.join(game,'CARS/asset.bin'));
 await fs.symlink(path.join(temp,'secret.bin'),path.join(game,'CARS/asset.bin'));
 assert.equal((await get('/game/CARS/asset.bin')).status,403);
 // Raw encoded traversal must also stay inside the selected tree.
 const status=await new Promise((resolve,reject)=>{http.get(server.origin+'/game/%2e%2e%2fsecret.bin',r=>{r.resume();resolve(r.statusCode);}).on('error',reject);});assert.equal(status,400);
});
test('worker adapter permits same-origin local reads while preserving remote HTTPS restriction',()=>{
 for(const text of ['if(remote && url.protocol !== "https:") throw Error("x")','if(remote&&url.protocol!=="https:")throw Error("x")','if(r&&"https:"!==u.protocol)throw Error("x")']){
  const adapted=adaptWorker(text);
  const check=new Function('remote','url','self','r','u',adapted);
  const self={location:{origin:'http://127.0.0.1:47738'}};
  const local=new URL(self.location.origin+'/local/manifest.json');assert.doesNotThrow(()=>check(true,local,self,true,local));
  const remote=new URL('http://example.com/manifest.json');assert.throws(()=>check(true,remote,self,true,remote),/x/);
 }
 assert.throws(()=>adaptWorker('unsupported'),/Unsupported runtime/);
});
