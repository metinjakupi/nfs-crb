// Overlay generated artwork only after original streamed blocks pass verification.
const manifestSha='adb6dab48cf6693b7868f96b81da9d6fe8c565b026a415c3984b681dbb64ff7b';
const hash=async b=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b)),x=>x.toString(16).padStart(2,'0')).join('');
async function checked(url,expected){const r=await fetch(url);if(!r.ok)throw Error('City artwork unavailable');const b=await r.arrayBuffer();if(await hash(b)!==expected)throw Error('City artwork differs');return b;}
const base=new URL('./billboard-assets/',import.meta.url);
let ready;const assets=new Map();
export async function overlay(file,offset,bytes){
 const path=file.path||new URL(file.url).pathname.split('/').slice(-2).join('/');
 if(!['TRACKS/STREAML5RA.BUN','TRACKS/STREAML5RB.BUN'].includes(path))return bytes;
 ready??=checked(new URL('manifest.json',base),manifestSha).then(b=>JSON.parse(new TextDecoder().decode(b)));
 const m=await ready,fi=m.files.findIndex(f=>f.path===path&&f.bytes===file.size);if(fi<0)return bytes;
 const rows=m.patches.filter(p=>p.file===fi&&p.offset<offset+bytes.length&&p.offset+p.bytes>offset);if(!rows.length)return bytes;
 const out=bytes.slice();
 for(const p of rows){if(!assets.has(p.id))assets.set(p.id,checked(new URL(p.asset,base),p.after).catch(e=>{assets.delete(p.id);throw e;}));const data=new Uint8Array(await assets.get(p.id));if(data.length!==p.bytes)throw Error('City artwork size differs');const start=Math.max(offset,p.offset),end=Math.min(offset+bytes.length,p.offset+p.bytes);out.set(data.subarray(start-p.offset,end-p.offset),start-offset);}
 return out;
}
