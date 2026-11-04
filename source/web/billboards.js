// Local full-import community artwork. Only pinned texture pixel ranges are writable.
const VERSION = 'carbon-city-art-v2';
const MANIFEST_SHA = 'adb6dab48cf6693b7868f96b81da9d6fe8c565b026a415c3984b681dbb64ff7b';
const LEGACY_SHA = '578c5a10b7f45391e99a5de64aaed2051a8926fd16d127959cf25b892798a117';
const hash = async b => [...new Uint8Array(await crypto.subtle.digest('SHA-256',b))].map(x=>x.toString(16).padStart(2,'0')).join('');
async function put(dir,name,data) {
  const out=await(await dir.getFileHandle(name,{create:true})).createWritable();
  try {await out.write(data);await out.close();}catch(e){await out.abort().catch(()=>{});throw e;}
}
async function fileIn(dir,path) {
  const parts=path.split('/');
  for(let i=0;i<parts.length;i++) {
    let name=parts[i];for await(const[n]of dir.entries())if(n.toLowerCase()===name.toLowerCase())name=n;
    if(i===parts.length-1)return dir.getFileHandle(name);dir=await dir.getDirectoryHandle(name);
  }
}
async function pending(dir) {
  try {const f=await(await dir.getFileHandle('pending.json')).getFile();if(f.size>4096)throw Error('Invalid artwork recovery record');return JSON.parse(await f.text());}
  catch(e){if(e.name==='NotFoundError')return null;throw e;}
}
function workerBody() {
  const digest=async b=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',b))].map(x=>x.toString(16).padStart(2,'0')).join('');
  onmessage=async({data})=>{
    const access=[],original=[],changed=[];
    try {
      // Acquire every file before the first write. A running game keeps all artwork intact.
      for(const f of data.files){const a=await f.handle.createSyncAccessHandle();access.push(a);if(a.getSize()!==f.bytes)throw Error('Track size differs');}
      for(const p of data.rows){
        const a=access[p.file],header=new Uint8Array(p.headerBytes),bytes=new Uint8Array(p.bytes);
        if(a.read(header,{at:p.headerOffset})!==header.length || await digest(header)!==p.headerSha256)throw Error('Texture header differs');
        if(a.read(bytes,{at:p.offset})!==bytes.length)throw Error('Texture read failed');
        const h=await digest(bytes);
        if(!data.recover && ![p.before,p.after,...(p.prior||[])].includes(h))throw Error('Artwork changed outside this install');
        original.push(bytes);
      }
      for(let i=0;i<data.rows.length;i++){
        const p=data.rows[i];if(await digest(original[i])===p.target)continue;
        changed.push(i);if(access[p.file].write(p.data,{at:p.offset})!==p.bytes)throw Error('Artwork write failed');
      }
      for(const a of access)a.flush();
      for(const p of data.rows){const b=new Uint8Array(p.bytes);if(access[p.file].read(b,{at:p.offset})!==b.length || await digest(b)!==p.target)throw Error('Artwork verification failed');}
      for(const a of access)a.close();postMessage({ok:true,changed:changed.length});
    }catch(e){
      let restored=true;
      try {for(const i of changed){const p=data.rows[i];if(access[p.file].write(original[i],{at:p.offset})!==p.bytes)throw Error('Rollback failed');}for(const a of access)a.flush();}catch{restored=false;}
      for(const a of access)try{a.close();}catch{}
      postMessage({ok:false,name:e.name,message:String(e.message||e),restored});
    }
  };
}
async function apply(files,rows,recover=false) {
  const url=URL.createObjectURL(new Blob(['('+workerBody.toString()+')()'],{type:'text/javascript'})),worker=new Worker(url);
  try{return await new Promise((resolve,reject)=>{
    worker.onmessage=({data})=>{if(data.ok)resolve(data);else{const e=Error(data.message);e.name=data.name||'Error';e.restored=data.restored;reject(e);}};
    worker.onerror=e=>reject(Error(e.message||'Artwork worker failed'));worker.postMessage({files,rows,recover});
  });}finally{worker.terminate();URL.revokeObjectURL(url);}
}
async function checkedJson(url,digest) {
  const r=await fetch(url);if(!r.ok)throw Error('Artwork manifest unavailable');const b=await r.arrayBuffer();
  if(b.byteLength>262144 || await hash(b)!==digest)throw Error('Artwork manifest differs');return JSON.parse(new TextDecoder().decode(b));
}
export async function prepareBillboards(report=()=>{},options={}) {
  if(options.stream)return {state:'skipped'};
  const assets=new URL('./billboard-assets/',import.meta.url),manifest=await checkedJson(new URL('manifest.json',assets),MANIFEST_SHA);
  if(manifest.schema!==2 || manifest.version!==VERSION || manifest.files.length!==2 || manifest.patches.length!==67)throw Error('Invalid city artwork manifest');
  const root=await(await navigator.storage.getDirectory()).getDirectoryHandle('nfsc'),game=await root.getDirectoryHandle('game'),files=[];
  for(const f of manifest.files){
    if(!['TRACKS/STREAML5RA.BUN','TRACKS/STREAML5RB.BUN'].includes(f.path))throw Error('Invalid artwork file');
    let handle;try{handle=await fileIn(game,f.path);}catch(e){if(e.name==='NotFoundError')return {state:'unavailable'};throw e;}
    if((await handle.getFile()).size!==f.bytes){console.warn('[billboards] Different track version; artwork retained');return {state:'different-track'};}
    files.push({...f,handle});
  }
  // Complete an interrupted older install before upgrading its three existing designs.
  let legacyDir;try{legacyDir=await root.getDirectoryHandle('.community-billboards-v1');}catch(e){if(e.name!=='NotFoundError')throw e;}
  if(legacyDir && await pending(legacyDir)){
    const old=await checkedJson(new URL('legacy-v1.json',assets),LEGACY_SHA),rows=[];
    for(const p of old.patches){
      let bytes;try{bytes=await(await(await legacyDir.getFileHandle(p.key+'.bin')).getFile()).arrayBuffer();}
      catch(e){if(e.name!=='NotFoundError')throw e;bytes=await(await files[0].handle.getFile()).slice(p.offset,p.offset+p.bytes).arrayBuffer();}
      if(await hash(bytes)!==p.before)throw Error('Older artwork backup differs');rows.push({...p,file:0,data:bytes,target:p.before});
    }
    report('Recovering community artwork…');await apply(files,rows,true);await legacyDir.removeEntry('pending.json');
  }
  const backup=await root.getDirectoryHandle('.community-billboards-v2',{create:true});
  const readOriginal=async p=>{
    const data=await(await(await backup.getFileHandle(p.id+'.bin')).getFile()).arrayBuffer();
    if(data.byteLength!==p.bytes || await hash(data)!==p.before)throw Error('Artwork backup differs');return data;
  };
  if(await pending(backup)){
    report('Recovering city artwork…');const rows=[];for(const p of manifest.patches)rows.push({...p,data:await readOriginal(p),target:p.before});
    await apply(files,rows,true);await backup.removeEntry('pending.json');
  }
  const snapshots=await Promise.all(files.map(f=>f.handle.getFile())),rows=[],enabled=options.enabled!==false;
  let previousFile=-1,end=0;
  for(const p of manifest.patches){
    if(p.file!==previousFile){previousFile=p.file;end=0;}
    if(!Number.isInteger(p.file) || p.file<0 || p.file>=files.length || !/^[a-z0-9-]+$/.test(p.id) || !/^[a-z0-9-]+\.dxt[15]$/.test(p.asset) ||
       !Number.isSafeInteger(p.offset) || p.offset<end || p.bytes<=0 || p.bytes>1048576 || p.offset+p.bytes>files[p.file].bytes)throw Error('Invalid artwork range');
    end=p.offset+p.bytes;const file=snapshots[p.file];
    if(await hash(await file.slice(p.headerOffset,p.headerOffset+p.headerBytes).arrayBuffer())!==p.headerSha256)return {state:'different-track'};
    const current=await file.slice(p.offset,p.offset+p.bytes).arrayBuffer(),h=await hash(current);
    if(![p.before,p.after,...(p.prior||[])].includes(h)){console.warn('[billboards] Modified artwork retained');return {state:'different-artwork'};}
    let original;
    try{original=await readOriginal(p);}catch(e){
      if(e.name!=='NotFoundError')throw e;
      if(h===p.before)original=current;
      else if((p.prior||[]).includes(h) && legacyDir)original=await(await(await legacyDir.getFileHandle(p.key+'.bin')).getFile()).arrayBuffer();
      else throw Error('Original artwork backup is missing');
      if(await hash(original)!==p.before)throw Error('Original artwork differs');await put(backup,p.id+'.bin',original);
    }
    let data=original;
    if(enabled){
      if(h===p.after)data=current;
      else{const r=await fetch(new URL(p.asset,assets));if(!r.ok)throw Error('City artwork unavailable');data=await r.arrayBuffer();}
      if(data.byteLength!==p.bytes || await hash(data)!==p.after)throw Error('City artwork hash differs');
    }
    rows.push({...p,data,target:enabled?p.after:p.before,current:h});
  }
  const state=enabled?'installed':'original';
  if(!rows.every(p=>p.current===p.target)){
    report(enabled?'Installing city artwork…':'Restoring original artwork…');await put(backup,'pending.json',JSON.stringify({version:VERSION}));
    try{await apply(files,rows);}catch(e){
      if(e.restored)await backup.removeEntry('pending.json');
      if(e.restored && ['NoModificationAllowedError','InvalidStateError'].includes(e.name)){console.warn('[billboards] Close other game tabs and reload to install city artwork');return {state:'busy'};}
      throw e;
    }
    await backup.removeEntry('pending.json');
  }
  const result={state,version:VERSION,textures:rows.length,designs:manifest.designs};globalThis.carbonBillboards=result;console.info('[billboards]',state,rows.length);return result;
}
