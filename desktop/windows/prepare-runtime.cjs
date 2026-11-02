'use strict';
// Package an already reviewed website release. Never copy a game installation.
const fs=require('node:fs/promises'),path=require('node:path'),{createHash}=require('node:crypto');
const {adaptWorker}=require('./local-server.cjs');
(async()=>{
  const args=process.argv.slice(2),option=k=>{const n=args.indexOf(k);if(n<0||!args[n+1])throw Error('Required: --site <published play folder> --page <published root index.html>');return path.resolve(args[n+1]);};
  const site=option('--site'),page=await fs.readFile(option('--page'),'utf8');
  const info=/recompPageBuild\s*=\s*Object\.freeze\((\{[^\n]+?\})\)/.exec(page);
  if(!info)throw Error('The player page has no release identity.');
  const release=JSON.parse(info[1]),out=path.join(__dirname,'runtime');
  const wasm=await fs.readFile(path.join(site,'nfsc/CarbonRecomp.wasm'));
  if(createHash('sha256').update(wasm).digest('hex')!==release.wasm)throw Error('The page and WebAssembly runtime do not match.');
  const runtimeJS=await fs.readFile(path.join(site,'nfsc/CarbonRecomp.js'),'utf8');
  if(!runtimeJS.includes(release.id))throw Error('The JavaScript runtime and page do not match.');
  adaptWorker(await fs.readFile(path.join(site,'worker.js'),'utf8'));
  await fs.mkdir(path.join(out,'nfsc'),{recursive:true});
  for(const name of ['games.json','core.js','worker.js'])if(path.resolve(site,name)!==path.resolve(out,name))await fs.copyFile(path.join(site,name),path.join(out,name));
  const names=await fs.readdir(path.join(site,'nfsc'));
  for(const name of names)if(/\.(js|wasm|data)$/.test(name)&&path.resolve(site,'nfsc',name)!==path.resolve(out,'nfsc',name))await fs.copyFile(path.join(site,'nfsc',name),path.join(out,'nfsc',name));
  // The runtime's original page is a redirect on the public site. Package its root player page.
  await fs.writeFile(path.join(out,'nfsc/index.html'),page);
  const hashes={};for(const dir of ['', 'nfsc'])for(const name of await fs.readdir(path.join(out,dir))){const p=path.join(out,dir,name);if((await fs.stat(p)).isFile()&&name!=='release.json')hashes[path.posix.join(dir,name)]=createHash('sha256').update(await fs.readFile(p)).digest('hex');}
  await fs.writeFile(path.join(out,'release.json'),JSON.stringify({build:release,files:hashes},null,2)+'\n');
  console.log('Prepared reviewed runtime '+release.id+'; game files excluded.');
})().catch(e=>{console.error(e.message);process.exitCode=1;});
