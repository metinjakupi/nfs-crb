// Local cockpit texture upgrade. Profiles and original installation files are never opened.
const ORIGINAL='54ef5ccf89f587d1ee88c423ccc03021f8bc835a85b2b545d886ef6a0cb89519';
const ENHANCED='6c35b3f40ec84ddcaf6f7d1733ed41fc11098029ae7db59bb954026808b5abf8';
const hash=async b=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b)),x=>x.toString(16).padStart(2,'0')).join('');
async function put(dir,name,bytes){const out=await(await dir.getFileHandle(name,{create:true})).createWritable();try{await out.write(bytes);await out.close();}catch(e){await out.abort().catch(()=>{});throw e;}}
async function child(dir,want){for await(const [name,entry]of dir.entries())if(name.toLowerCase()===want.toLowerCase())return entry;throw new DOMException('Missing cockpit asset','NotFoundError');}
export async function prepareCockpitAssets(report=()=>{}){
 if(!['localhost','127.0.0.1','[::1]'].includes(location.hostname)||globalThis.recompSite)return {state:'not-local'};
 try{
  const root=await(await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
  const game=await root.getDirectoryHandle('game'),cars=await child(game,'CARS'),car=await child(cars,'R32'),handle=await child(car,'TEXTURES.BIN');
  const file=await handle.getFile();if(file.size>33554432)return {state:'different-textures'};
  const original=await file.arrayBuffer(),before=await hash(original);
  if(before===ENHANCED)return {state:'ready'};
  if(before!==ORIGINAL){console.info('[cockpit] Existing R32 texture mod retained');return {state:'different-textures'};}
  report('Preparing HD cockpit texture…');const response=await fetch(new URL('./cockpit-assets/R32-TEXTURES.BIN',import.meta.url),{cache:'no-cache'});
  if(!response.ok)throw Error('Cockpit texture HTTP '+response.status);
  const enhanced=await response.arrayBuffer();if(enhanced.byteLength!==16958528||await hash(enhanced)!==ENHANCED)throw Error('Cockpit texture verification failed');
  const backup=await root.getDirectoryHandle('.cockpit-assets-v1',{create:true});
  let prior;try{prior=await(await(await backup.getFileHandle('R32-TEXTURES.original.bin')).getFile()).arrayBuffer();}catch(e){if(e.name!=='NotFoundError')throw e;}
  if(prior&&await hash(prior)!==ORIGINAL)throw Error('Cockpit texture backup differs');
  if(!prior)await put(backup,'R32-TEXTURES.original.bin',original);
  // createWritable replaces the file atomically at close; a failed write leaves the old file intact.
  await put(car,file.name,enhanced);
  if(await hash(await(await handle.getFile()).arrayBuffer())!==ENHANCED){await put(car,file.name,original);throw Error('Cockpit texture restored after verification failure');}
  await put(backup,'installed.json',JSON.stringify({version:1,original:ORIGINAL,enhanced:ENHANCED}));
  return {state:'installed',backup:'.cockpit-assets-v1/R32-TEXTURES.original.bin'};
 }catch(e){console.warn('[cockpit] HD texture retained at its previous version:',e.message);return {state:'unavailable',reason:e.message};}
}
