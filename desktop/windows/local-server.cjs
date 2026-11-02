'use strict';
const http = require('node:http');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const {createHash} = require('node:crypto');
const {pipeline} = require('node:stream/promises');
const EXPECTED_EXE = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3';
const FOLDERS = new Set(['CARS','FRONTEND','GLOBAL','SOUND','TRACKS','MOVIES','NIS','ONLINE','LANGUAGES']);
const ROOT_FILES = new Set(['nfsc.exe','msvcr71.dll','msvcp71.dll']);
const MIME = {'.html':'text/html; charset=utf-8','.js':'text/javascript','.json':'application/json','.wasm':'application/wasm','.css':'text/css'};
async function digest(file) {
  const hash = createHash('sha256');
  for await (const bytes of fs.createReadStream(file)) hash.update(bytes);
  return hash.digest('hex');
}
async function inspectGame(folder, expectedExe = EXPECTED_EXE) {
  const root = await fsp.realpath(folder), files = new Map();
  async function scan(dir, prefix = '') {
    for (const e of await fsp.readdir(dir,{withFileTypes:true})) {
      if (e.isSymbolicLink()) continue;
      if (!prefix && !(e.isDirectory() ? FOLDERS.has(e.name.toUpperCase()) : ROOT_FILES.has(e.name.toLowerCase()))) continue;
      if (/[\r\n\t?#%\\]/.test(e.name) || e.name === '.' || e.name === '..') continue;
      const rel = prefix + e.name, absolute = path.join(dir,e.name);
      if (e.isDirectory()) await scan(absolute,rel+'/');
      else if (e.isFile()) {
        // Every request rechecks realpath too: junctions must not expose another folder.
        const real = await fsp.realpath(absolute);
        if (!real.startsWith(root+path.sep)) throw Error('The game folder contains a link outside the installation.');
        const s = await fsp.stat(real);
        if (files.has(rel.toLowerCase())) throw Error('Ambiguous game filename: '+rel);
        files.set(rel.toLowerCase(),{path:rel,size:s.size,mtime:Math.floor(s.mtimeMs/1000),absolute:real});
      }
    }
  }
  await scan(root);
  for (const required of ['nfsc.exe','msvcr71.dll','msvcp71.dll']) if (!files.has(required)) throw Error('Missing '+required+'. Select the folder containing NFSC.exe.');
  for (const required of ['CARS','FRONTEND','GLOBAL','SOUND','TRACKS']) if (![...files.keys()].some(k=>k.startsWith(required.toLowerCase()+'/'))) throw Error('Missing game folder: '+required);
  if (await digest(files.get('nfsc.exe').absolute) !== expectedExe) throw Error('This NFSC.exe does not match the supported PC v1.4 executable. The launcher will not modify it.');
  return {root,files};
}
function adaptWorker(text) {
  // Keep HTTPS mandatory for remote servers; allow only this same-origin local manifest.
  const compact = 'remote&&url.protocol!=="https:"';
  if(text.includes(compact)) return text.replace(compact,compact+'&&url.origin!==self.location.origin');
  const plain = 'remote && url.protocol !== "https:"';
  if (text.includes(plain)) return text.replace(plain,'remote && url.protocol !== "https:" && url.origin !== self.location.origin');
  const pattern = /([A-Za-z_$][\w$]*)&&"https:"!==([A-Za-z_$][\w$]*)\.protocol/;
  const matches = [...text.matchAll(new RegExp(pattern.source,'g'))];
  if(matches.length!==1) throw Error('Unsupported runtime worker. Prepare a compatible runtime before packaging.');
  return text.replace(pattern,'$1&&"https:"!==$2.protocol&&$2.origin!==self.location.origin');
}
async function sendFile(req,res,file,allowRange=true) {
  const size=(await fsp.stat(file)).size;
  let start=0,end=size-1;
  if(req.headers.range && allowRange) {
    const match=/^bytes=(\d+)-(\d*)$/.exec(req.headers.range);
    if(!match || +match[1]>=size || (match[2] && +match[2]<+match[1])) {res.writeHead(416,{'Content-Range':`bytes */${size}`});res.end();return;}
    start=+match[1];end=match[2]?Math.min(+match[2],size-1):size-1;
    res.statusCode=206;res.setHeader('Content-Range',`bytes ${start}-${end}/${size}`);
  }
  res.setHeader('Content-Type',MIME[path.extname(file)]||'application/octet-stream');
  res.setHeader('Content-Length',size?end-start+1:0);res.setHeader('Accept-Ranges','bytes');
  if(req.method==='HEAD'||!size) {res.end();return;}
  await pipeline(fs.createReadStream(file,{start,end}),res);
}
async function createLocalServer({runtime,ui,port=47738}) {
  let game=null;
  const server=http.createServer(async(req,res)=>{
    try {
      const origin=`http://127.0.0.1:${server.address().port}`;
      if(req.headers.host!==new URL(origin).host || (req.headers.origin && req.headers.origin!==origin) || req.headers['sec-fetch-site']==='cross-site') {res.writeHead(403);res.end();return;}
      res.setHeader('Cross-Origin-Opener-Policy','same-origin');res.setHeader('Cross-Origin-Embedder-Policy','require-corp');res.setHeader('Cross-Origin-Resource-Policy','same-origin');res.setHeader('Cache-Control','no-store');
      if(!['GET','HEAD'].includes(req.method)){res.writeHead(405);res.end();return;}
      const pathname=decodeURIComponent(new URL(req.url,origin).pathname);
      if(pathname.includes('\\')||pathname.includes('\0')||pathname.split('/').includes('..')) {res.writeHead(400);res.end();return;}
      const json=value=>{res.setHeader('Content-Type','application/json');res.end(JSON.stringify(value));};
      if(pathname==='/local/manifest.json') {
        if(!game)throw Error('Select a game folder first.');
        return json({base:origin+'/game/',files:[...game.files.values()].map(({path,size,mtime})=>({path,size,mtime}))});
      }
      if(pathname.startsWith('/game/')) {
        const e=game?.files.get(pathname.slice(6).toLowerCase());
        if(!e){res.writeHead(404);res.end();return;}
        const actual=await fsp.realpath(e.absolute);
        if(!actual.startsWith(game.root+path.sep)){res.writeHead(403);res.end();return;}
        await sendFile(req,res,actual);return;
      }
      if(pathname==='/play/games.json') {
        const games=JSON.parse(await fsp.readFile(path.join(runtime,'games.json'),'utf8'));
        const g=games.find(g=>g.id==='nfsc');if(!g)throw Error('Carbon runtime metadata is missing.');
        return json([{...g,streamAssets:true,downloadManifest:origin+'/local/manifest.json',localManifest:undefined}]);
      }
      if(pathname==='/play/worker.js') {
        res.setHeader('Content-Type','text/javascript');res.end(adaptWorker(await fsp.readFile(path.join(runtime,'worker.js'),'utf8')));return;
      }
      if(pathname.startsWith('/play/nfsc/custom-assets/')) {
        // Customization is a separate overlay. Base game reads never use the internet.
        const rel=pathname.slice('/play/nfsc/custom-assets/'.length);
        res.writeHead(307,{Location:'https://assets.nfscrb.xyz/customization/5911a37d9f9bc652b3fa457127273970b0d2a5e0e87c7081fb789a2c05439dbc/'+rel.split('/').map(encodeURIComponent).join('/')});res.end();return;
      }
      const base=pathname.startsWith('/play/')?runtime:ui;
      const rel=pathname.startsWith('/play/')?pathname.slice(6):(pathname==='/'?'index.html':pathname.slice(1));
      const file=path.resolve(base,rel);
      if(!file.startsWith(path.resolve(base)+path.sep)){res.writeHead(403);res.end();return;}
      await sendFile(req,res,file,false);
    } catch(e) {if(!res.headersSent)res.writeHead(e.code==='ENOENT'?404:500);res.end('Unable to read launcher data.');}
  });
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(port,'127.0.0.1',resolve);});
  return {origin:`http://127.0.0.1:${server.address().port}`,setGame(value){game=value;},close:()=>new Promise(resolve=>{server.close(resolve);server.closeAllConnections();})};
}
module.exports={inspectGame,createLocalServer,adaptWorker};
