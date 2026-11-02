// Local customization upgrade. Only game assets are changed; profiles are never opened.
const CUSTOM_VERSION = '5911a37d9f9bc652b3fa457127273970b0d2a5e0e87c7081fb789a2c05439dbc';
const EXECUTABLE = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3';
const VECTOR_SHA = 'aa21d7944dea5e72c28aba39d3a936d7179866a7d6aec504f0c78666fba51a81';
const ASSET_PATH = /^(GLOBAL\/(GLOBALB\.BUN|GlobalB\.lzc|attributes\.bin|FE_ATTRIB\.bin|gameplay\.bin|gameplay\.lzc)|CARS\/(240SX|300C|350Z|911TURBO|997GT3RS|997S|997TT|BMWM3GTRE46|BRAKES|BRERA|CAMARO|CAMARON|CARRERAGT|CAYMANS|CCX|CHALLENGER71|CHALLENGERN|CHARGER06|CHARGER69|CLIO|CLK500|COROLLA|CORVETTEZ06|DB9|DRIVERS|ECLIPSE|ECLIPSEGT|ELISE|EUROPA|EXHAUST|FORDGT|G35|GALLARDO|GTO|IMPREZAWRXSTI|IS300|JAGUARXK|LANCEREVO9|MAZDA3SPORT|MAZDASPEED3|MONARO|MR2|MURCIELAGO|MURCIELAGO640|MUSTANGGT|MUSTANGSHLBYN|MUSTANGSHLBYO|PLATES|R32|R8|RX7|RX8|SKYLINE|SL65|SLR|SPOILER|SPOILER_CARRERA|SPOILER_HATCH|SPOILER_PORSCHES|SUPRA|TIRES|VIPER|WHEELS5|WHEELS6|ZONDA)\/(GEOMETRY|TEXTURES)\.BIN|FRONTEND\/FRONTB2\.BUN|CARS\/VINYLS\/VINYLS\.BIN|LANGUAGES\/English_(Frontend|Global)\.bin)$/;
const sha = async data => [...new Uint8Array(await crypto.subtle.digest('SHA-256', data))]
  .map(v => v.toString(16).padStart(2, '0')).join('');
const missing = e => e?.name === 'NotFoundError';
async function write(dir, name, bytes) {
  const out = await (await dir.getFileHandle(name, {create:true})).createWritable();
  try { await out.write(bytes); await out.close(); }
  catch (e) { await out.abort().catch(() => {}); throw e; }
}
async function json(dir, name) {
  try {
    const file = await (await dir.getFileHandle(name)).getFile();
    if (file.size > 262144) throw Error('Customization recovery record is too large');
    return JSON.parse(await file.text());
  } catch (e) { if (missing(e)) return null; throw e; }
}
async function locationFor(root, path, create = false) {
  const parts = path.split('/'); let dir = root;
  for (const part of parts.slice(0, -1)) {
    let actual = part;
    for await (const [name] of dir.entries()) if (name.toLowerCase() === part.toLowerCase()) actual = name;
    dir = await dir.getDirectoryHandle(actual, {create});
  }
  let name = parts.at(-1);
  for await (const [actual] of dir.entries()) if (actual.toLowerCase() === name.toLowerCase()) name = actual;
  return {dir, name};
}
async function fileAt(root, path) {
  try { const {dir,name} = await locationFor(root,path); return await (await dir.getFileHandle(name)).getFile(); }
  catch(e) { if(missing(e)) return null; throw e; }
}
async function writeAt(root, path, data) { const {dir,name} = await locationFor(root,path,true); await write(dir,name,data); }
async function removeAt(root, path) {
  try { const {dir,name} = await locationFor(root,path); await dir.removeEntry(name); }
  catch(e) { if(!missing(e)) throw e; }
}
function recoveryPath(path) { return ASSET_PATH.test(path) || /^GLOBAL\/[A-Za-z0-9_-]*MemoryFile\.bin$/i.test(path); }
async function verify(file, item) { return file && file.size === item.bytes && await sha(await file.arrayBuffer()) === item.sha256; }
function pruneMemory(bytes, changed) {
  const v = new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  if(bytes.length < 16 || v.getUint32(8,true) !== 0x53219999) return null;
  const count = v.getUint32(12,true);
  if(16 + count*20 > bytes.length) throw Error('Invalid memory-file table');
  const kept=[];
  for(let i=0;i<count;i++) {
    const row=Array.from({length:5},(_,j)=>v.getUint32(16+i*20+j*4,true));
    if(row[1]+row[3] > bytes.length) throw Error('Invalid cached file range');
    if(!changed.has(row[0])) kept.push(row);
  }
  if(kept.length === count) return null;
  const begin = (16+kept.length*20+15)&~15;
  const result = new Uint8Array(begin + kept.reduce((sum,row)=>sum+((row[3]+15)&~15),0));
  const out = new DataView(result.buffer); result.set(bytes.subarray(0,12)); out.setUint32(12,kept.length,true);
  let offset=begin;
  kept.forEach((row,i)=>{ const original=row[1]; row[1]=offset; row.forEach((n,j)=>out.setUint32(16+i*20+j*4,n,true));
    result.set(bytes.subarray(original,original+row[3]),offset); offset+=(row[3]+15)&~15; });
  return result;
}
async function restorePending(target, game, pending, report) {
  if(pending.schema !== 1 || !/^[a-f0-9]{64}$/.test(pending.version) || !Array.isArray(pending.entries) || pending.entries.length>128)
    throw Error('Invalid customization recovery record');
  const backup = await target.getDirectoryHandle('.customization-backup-'+pending.version);
  // Verify every retained original before restoring any game asset.
  for(const row of pending.entries) {
    if(!recoveryPath(row.path) || typeof row.existed !== 'boolean') throw Error('Invalid customization recovery path');
    if(row.existed && !await verify(await fileAt(backup,row.path),row)) throw Error('Customization backup verification failed: '+row.path);
  }
  report('Restoring the previous game assets…');
  for(const row of pending.entries) {
    if(row.existed) await writeAt(game,row.path,await (await fileAt(backup,row.path)).arrayBuffer());
    else await removeAt(game,row.path);
  }
  await target.removeEntry('.customization-pending.json');
  await target.removeEntry('.custom-install.json').catch(e=>{if(!missing(e))throw e;});
}
function downloadDiagnostic(record) {
  const records=globalThis.carbonDiagnostics?.downloads;
  if(!Array.isArray(records)) return;
  records.push({at:new Date().toISOString(),phase:'customization',...record});
  while(records.length>20) records.shift();
}
async function downloadCustomization(url, asJson=false) {
  const parsed=new URL(url,globalThis.document?.baseURI || globalThis.location?.href);
  const safeUrl=parsed.origin+parsed.pathname;
  for(let attempt=1;attempt<=3;attempt++) {
    let response;
    try {
      response=await fetch(url,{cache:'no-store'});
      if(!response.ok) {
        const error=new Error(`Download returned HTTP ${response.status}.`);
        error.retryable=response.status===408 || response.status===429 || response.status>=500;
        await response.body?.cancel();
        throw error;
      }
      // Reading the body belongs inside the retry: a connection can fail after HTTP 200.
      const bytes=await response.arrayBuffer();
      const result=asJson?JSON.parse(new TextDecoder().decode(bytes)):bytes;
      downloadDiagnostic({url:safeUrl,attempt,status:response.status});
      return result;
    } catch(error) {
      downloadDiagnostic({url:safeUrl,attempt,status:response?.status ?? null,
        error:{name:String(error.name || 'Error'),message:String(error.message || error)}});
      if(error.name==='AbortError' || error.name==='SyntaxError' ||
         !(error.retryable===true || (error.name==='TypeError' && error.retryable!==false))) throw error;
      if(attempt===3) throw new Error('A customization file could not be downloaded. Check your connection and press Play again. Completed downloads are kept.',{cause:error});
      await new Promise(resolve=>setTimeout(resolve,500*attempt));
    }
  }
}
async function install(report, options) {
  const assets = options.assetBase || "./custom-assets/";
  const manifest = await downloadCustomization(assets+'manifest.json',true);
  if(manifest.version !== CUSTOM_VERSION || manifest.executable !== EXECUTABLE || manifest.files?.length !== 79 ||
     manifest.partCount !== 23115 || manifest.nativeFingerprint !== 'e1e15255' || manifest.originalIndexesPreserved !== true || manifest.originalAttributesPreserved !== true)
    throw Error('Customization runtime and catalog differ');
  const paths = new Set();
  for(const row of manifest.files) {
    if(!ASSET_PATH.test(row.path) || paths.has(row.path.toLowerCase()) || !Number.isSafeInteger(row.bytes) || row.bytes<=0 || !/^[a-f0-9]{64}$/.test(row.sha256))
      throw Error('Invalid customization asset manifest');
    paths.add(row.path.toLowerCase());
  }
  const vector = manifest.files.find(row=>row.path==='CARS/VINYLS/VINYLS.BIN');
  if(vector?.bytes !== 5502976 || vector.sha256 !== VECTOR_SHA) throw Error('Vector resource and catalog differ');
  const target = await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
  const game = await target.getDirectoryHandle('game');
  const stamp = await fileAt(game,'.stamp');
  if(options.stream) {
    const exe = await fileAt(game,'NFSC.exe');
    if(!exe || await sha(await exe.arrayBuffer()) !== EXECUTABLE) throw Error('Streaming executable differs from the customization baseline');
  } else if(!stamp || (await stamp.text()).trim() !== EXECUTABLE) throw Error('Imported game executable differs from the customization baseline');
  const pending = await json(target,'.customization-pending.json');
  if(pending) await restorePending(target,game,pending,report);
  const ready = await json(target,'.custom-install.json');
  if(ready?.version === CUSTOM_VERSION && ready?.savePolicy === 'existing-profiles-unchanged') {
    let complete=true;
    for(const row of manifest.files) { const file=await fileAt(game,row.path); if(!file || file.size!==row.bytes) { complete=false; break; } }
    if(complete && await verify(await fileAt(game,vector.path),vector)) {
      globalThis.__carbonCustomVectorResourceSha256=VECTOR_SHA;
      globalThis.__carbonCustomizationAssetsVersion=CUSTOM_VERSION;
      return manifest;
    }
  }
  const stageName='.customization-stage-'+CUSTOM_VERSION;
  const stage=await target.getDirectoryHandle(stageName,{create:true});
  const changes=[];
  // Download and hash every incoming asset before any installed asset is changed.
  for(let i=0;i<manifest.files.length;i++) {
    const row=manifest.files[i]; report(`Preparing customization assets… ${i+1}/${manifest.files.length}`);
    const current=await fileAt(game,row.path);
    if(await verify(current,row)) continue;
    if(!await verify(await fileAt(stage,row.path),row)) {
      const bytes=await downloadCustomization(assets+row.path);
      if(bytes.byteLength!==row.bytes || await sha(bytes)!==row.sha256) throw Error('Customization asset hash mismatch: '+row.path);
      await writeAt(stage,row.path,bytes);
    }
    changes.push(row);
  }
  const changed=new Set();
  for(const row of manifest.files) for(const name of [row.path.replaceAll('/','\\'),row.path.replaceAll('/','\\').toUpperCase(),row.path.replaceAll('/','\\').toLowerCase()]) {
    let h=0xffffffff; for(const c of name) h=(Math.imul(h,33)+c.charCodeAt(0))>>>0; changed.add(h);
  }
  const global=await game.getDirectoryHandle('GLOBAL',{create:true});
  for await(const[name,handle]of global.entries()) {
    if(handle.kind!=='file' || !/MemoryFile\.bin$/i.test(name)) continue;
    const result=pruneMemory(new Uint8Array(await (await handle.getFile()).arrayBuffer()),changed);
    if(!result) continue;
    const row={path:'GLOBAL/'+name,bytes:result.byteLength,sha256:await sha(result)};
    await writeAt(stage,row.path,result); changes.push(row);
  }
  const backup=await target.getDirectoryHandle('.customization-backup-'+CUSTOM_VERSION,{create:true});
  const existingBackup=await json(backup,'.originals.json');
  let originals=existingBackup?.entries || [];
  if(existingBackup && (existingBackup.schema!==1 || existingBackup.version!==CUSTOM_VERSION || !Array.isArray(originals))) throw Error('Invalid customization backup record');
  const entries=[];
  for(const row of changes) {
    const saved=originals.find(item=>item.path===row.path);
    if(saved) {
      if(saved.existed && !await verify(await fileAt(backup,row.path),saved)) throw Error('Customization original backup differs: '+row.path);
      entries.push(saved); continue;
    }
    const original=await fileAt(game,row.path);
    const originalBytes=original?await original.arrayBuffer():null;
    const record={path:row.path,existed:!!original,bytes:original?.size||0,sha256:originalBytes?await sha(originalBytes):null};
    if(originalBytes) {
      await writeAt(backup,row.path,originalBytes);
      if(!await verify(await fileAt(backup,row.path),record)) throw Error('Customization backup write failed: '+row.path);
    }
    originals.push(record); entries.push(record);
  }
  await write(backup,'.originals.json',JSON.stringify({schema:1,version:CUSTOM_VERSION,entries:originals}));
  const transaction={schema:1,version:CUSTOM_VERSION,entries};
  await write(target,'.customization-pending.json',JSON.stringify(transaction));
  try {
    for(let i=0;i<changes.length;i++) {
      const row=changes[i]; report(`Installing customization assets… ${i+1}/${changes.length}`);
      await writeAt(game,row.path,await (await fileAt(stage,row.path)).arrayBuffer());
      if(!await verify(await fileAt(game,row.path),row)) throw Error('Installed customization asset differs: '+row.path);
    }
    await write(target,'.custom-install.json',JSON.stringify({version:CUSTOM_VERSION,savePolicy:'existing-profiles-unchanged',installed:new Date().toISOString()}));
    await target.removeEntry('.customization-pending.json');
  } catch(e) {
    await restorePending(target,game,transaction,report);
    throw e;
  }
  await target.removeEntry(stageName,{recursive:true}).catch(()=>{});
  globalThis.__carbonCustomVectorResourceSha256=VECTOR_SHA;
  globalThis.__carbonCustomizationAssetsVersion=CUSTOM_VERSION;
  report('Starting Carbon…');
  return manifest;
}
// Streamed packed-data caches can contain older copies of the replaced assets.
const STREAM_CACHES = [{"path": "GLOBAL/GlobalMemoryFile.bin", "bytes": 9381872, "sha256": "6da121b60de65b2a7f78c24fc7a891dd253799f0124313b66c63e097e74d1892"}, {"path": "GLOBAL/CarHeadersMemoryFile.bin", "bytes": 362736, "sha256": "bdb080b1d50c6fc167970f6cfec2519b39558dfab568fe03d6e6c70b4ec9ecf4"}];
async function prepareStreamCaches(options,report) {
  if(!options.stream) return;
  const root=await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
  const game=await root.getDirectoryHandle('game');
  const ready=await json(root,'.custom-stream-caches.json');
  if(ready?.version===CUSTOM_VERSION && (await Promise.all(STREAM_CACHES.map(row=>fileAt(game,row.path)))).every(Boolean)) return;
  if(!options.streamBase) throw Error('Streaming asset base is missing');
  for(const row of STREAM_CACHES) {
    report('Preparing customization data caches…');
    const bytes=await downloadCustomization(options.streamBase.replace(/\/$/,'')+'/'+row.path);
    if(bytes.byteLength!==row.bytes || await sha(bytes)!==row.sha256)throw Error('Base data cache hash mismatch: '+row.path);
    await writeAt(game,row.path,bytes);
  }
  // Re-run the existing transactional cache pruner even after an earlier asset install.
  await root.removeEntry('.custom-install.json').catch(e=>{if(!missing(e))throw e;});
}
export async function prepareCustomization(report=()=>{}, options={}) {
  if(!navigator.locks?.request) throw Error('Customization installation requires browser storage locks');
  return navigator.locks.request('carbon-customization-assets',{mode:'exclusive'},async()=>{
    await prepareStreamCaches(options,report);
    const manifest=await install(report,options);
    if(options.stream) {
      const root=await (await navigator.storage.getDirectory()).getDirectoryHandle('nfsc');
      const game=await root.getDirectoryHandle('game');
      const file=await fileAt(game,'.stream-index');
      if(!file) throw Error('Streaming file index is missing');
      const lines=(await file.text()).trim().split('\n');
      const local=new Map([...manifest.files,...STREAM_CACHES].map(row=>[row.path.toLowerCase(),row.path]));
      const seen=new Set();
      const output=lines.map(line=>{const path=line.slice(2);seen.add(path.toLowerCase());return local.has(path.toLowerCase())?'L\t'+local.get(path.toLowerCase()):line;});
      for(const row of manifest.files) if(!seen.has(row.path.toLowerCase())) output.push('L\t'+row.path);
      await write(game,'.stream-index',output.join('\n')+'\n');
      await write(root,'.custom-stream-caches.json',JSON.stringify({version:CUSTOM_VERSION}));
    }
  });
}
