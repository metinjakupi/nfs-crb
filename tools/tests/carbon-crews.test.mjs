// Real Rust relay and temporary identity/crew storage. No real profiles or saves.
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const binary = process.env.CARBON_CREW_TEST_BIN || path.resolve("server/target/debug/carbon-server");
const token = n => createHash("sha256").update("synthetic crew test "+n).digest("hex");
const owner = n => createHash("sha256").update("nfsc name claim v1\0").update(token(n)).digest("hex");
async function until(fn, why) {
  const end=Date.now()+4000;
  while(Date.now()<end){const v=fn();if(v)return v;await delay(10);}
  throw new Error("Timed out: "+why);
}
async function fixture(t, count=16, {admin=false}={}) {
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),"carbon-crews-"));
  const names=Object.fromEntries(Array.from({length:count},(_,n)=>[`player${n}`,{name:`Player${n}`,owner:owner(n),claimed:1,seen:1}]));
  fs.writeFileSync(path.join(dir,"names.json"),JSON.stringify({migration:true,names,reserved:{}}));
  let process, port, sockets=[];
  const stop=async()=>{
    for(const s of sockets)s.close();sockets=[];
    if(process){const end=once(process,"close");process.kill();await end;process=null;}
  };
  const start=async()=>{
    process=spawn(binary,["0","127.0.0.1"],{env:{...globalThis.process.env,CARBON_ADMIN_OWNERS:admin?owner(0):"",CARBON_MODERATION_DIR:dir,CARBON_RELAY_KEY:"",CARBON_INTEREST_K:"1",CARBON_MESSENGER_DIR:dir,CARBON_CREWS_DIR:dir,CARBON_SAVES_DIR:"",CARBON_STATS_DIR:"",CARBON_TRACE_DIR:""},stdio:["ignore","pipe","pipe"]});
    let output="",errors="";process.stdout.on("data",b=>output+=b);process.stderr.on("data",b=>errors+=b);
    port=await until(()=>{if(process.exitCode!==null)throw new Error(errors);return Number(output.match(/relay on ws:\/\/127.0.0.1:(\d+)\//)?.[1]);},"server startup");
  };
  t.after(async()=>{await stop();fs.rmSync(dir,{recursive:true,force:true});});
  await start();
  return {dir,stop,start,url:()=>`ws://127.0.0.1:${port}/lobby`,async connect(n,{name=`Player${n}`,room="lobby",crew="",proof=token(n),crews=1}={}){
    const ws=new WebSocket(`ws://127.0.0.1:${port}/${room}`), messages=[];
    sockets.push(ws);ws.addEventListener("message",e=>{if(typeof e.data==="string")messages.push(JSON.parse(e.data));});
    await once(ws,"open");
    ws.send(JSON.stringify({t:"hi",name,claim:proof,crew,crews}));
    const welcome=await until(()=>messages.find(m=>m.t==="welcome"),"welcome");
    assert.equal(welcome.crews,1,"protected crew support must be negotiated");
    let q=0;
    return {ws,messages,welcome,send:m=>ws.send(JSON.stringify(m)),
      state:()=>messages.findLast(m=>m.t==="crewstate"),
      async moderate(op,fields={}) {
        const query=++q;ws.send(JSON.stringify({t:"moderation",q:query,op,...fields}));
        return until(()=>messages.find(m=>m.t==="moderation"&&m.q===query),op);
      },
      async request(op,fields={}) {
        const query=++q,c=messages.findLast(m=>m.t==="crewstate")?.crew;
        ws.send(JSON.stringify({t:"crewctl",op,q:query,crew:c?.id,revision:c?.revision,...fields}));
        return until(()=>messages.find(m=>m.t==="crewctl"&&m.q===query),op);
      }};
  }};
}
async function ok(p,op,fields) {const r=await p.request(op,fields);assert.equal(r.ok,true,JSON.stringify(r));return r;}
async function denied(p,op,error,fields) {const r=await p.request(op,fields);assert.equal(r.ok,false);assert.equal(r.error,error);return r;}

test("protected crews reject copied names and forged invitations; leader changes remove authority",async t=>{
  const f=await fixture(t),a=await f.connect(0),b=await f.connect(1),e=await f.connect(2);
  await ok(a,"create",{name:"Night Owls"});await until(()=>a.state()?.crew,"created crew");
  assert.equal(a.state().crew.count,1);
  b.send({t:"crew",crew:"nightowls"});
  const echo=await until(()=>b.messages.find(m=>m.t==="crew"&&m.id===b.welcome.id),"legacy copy rejection");
  assert.equal(echo.crew,"");assert.equal(echo.crewId,null);
  await denied(b,"create","taken",{name:"NIGHTOWLS"});
  await denied(e,"create","taken",{name:"Night  Owls"});
  await ok(a,"invite",{name:"Player1"});await until(()=>b.state()?.pending===1,"invitation notice");
  const inv=(await ok(b,"inbox")).page.rows[0];assert.equal(inv.crew,"Night Owls");
  await denied(e,"accept","forbidden",{invite:inv.id});
  await ok(b,"accept",{invite:inv.id});await until(()=>a.state()?.crew?.count===2,"joined roster");
  assert.equal(b.state().crew.id,a.state().crew.id);
  await denied(b,"invite","forbidden",{name:"Player2"});
  await denied(a,"leave","leader-leave");
  // A copied label confers neither a crew-only horn audience nor roster access.
  await denied(e,"roster","no-crew");
  const nearA=await f.connect(5),nearB=await f.connect(6),nearE=await f.connect(7);
  for(const [p,x] of [[a,0],[nearA,1],[b,10000],[nearB,10001],[e,20000],[nearE,20001]])
    p.send({t:"pose",k:123,m:1,ts:1500,s:[0,0,0,1,x,0,0,0,0,0,0,0,0]});
  await delay(650); // Let interest selection separate the three pairs.
  a.send({t:"horn",on:true});await until(()=>b.messages.find(m=>m.t==="horn"),"crew horn");
  await delay(100);assert.equal(e.messages.some(m=>m.t==="horn"),false);
  const old={crew:a.state().crew.id,revision:a.state().crew.revision};
  await ok(a,"transfer",{member:b.welcome.account});await until(()=>b.state()?.crew?.leader===b.welcome.account,"new leader");
  await denied(a,"kick","forbidden",{member:b.welcome.account});
  await denied(b,"rename","stale",{...old,name:"Stale"});
  await ok(b,"kick",{member:a.welcome.account});await until(()=>a.state()?.crew===null,"removed membership");
  const roster=(await ok(b,"roster")).page;assert.equal(roster.total,1);assert.equal(roster.rows[0].id,b.welcome.account);
  // A removed member cannot begin a crew challenge. Existing event progress/cleanup
  // must still travel; encounter.js independently requires the accepted peer/session.
  a.send({t:"race",k:"encounter",v:4,op:"offer",s:999,to:b.welcome.id});
  a.send({t:"race",k:"encounter",v:4,op:"progress",s:999,to:b.welcome.id,pct:20});
  await until(()=>b.messages.find(m=>m.t==="race"&&m.id===a.welcome.id&&m.op==="progress"),"accepted event transport");
  assert.equal(b.messages.some(m=>m.t==="race"&&m.id===a.welcome.id&&m.op==="offer"),false);
  // Arbitrary tokens and old clients cannot act as verified crew accounts.
  const unverified=await f.connect(99,{proof:token(99)});
  assert.equal(unverified.welcome.account,null);
  await denied(unverified,"create","identity",{name:"Forged"});
  const legacy=await f.connect(4,{crews:0});await denied(legacy,"create","upgrade",{name:"Legacy"});
});

test("crews have no ten-member cap; membership and offline invitations survive restart",async t=>{
  const f=await fixture(t,64),a=await f.connect(0);
  await ok(a,"create",{name:"Large Crew"});await until(()=>a.state()?.crew,"crew created");
  for(let n=1;n<=10;n++){
    const p=await f.connect(n);
    await ok(a,"invite",{name:`Player${n}`});await until(()=>p.state()?.pending===1,"pending");
    const inv=(await ok(p,"inbox")).page.rows[0];await ok(p,"accept",{invite:inv.id});
    await until(()=>a.state()?.crew?.count===n+1,"membership update");
  }
  assert.equal(a.state().crew.count,11);
  await ok(a,"invite",{name:"Player11"});
  const account=a.welcome.account,cid=a.state().crew.id;
  await f.stop();await f.start();
  const again=await f.connect(0);
  assert.equal(again.welcome.account,account);assert.equal(again.welcome.crewId,cid);
  const roster=(await ok(again,"roster")).page;assert.equal(roster.total,11);
  assert.equal(roster.rows.filter(p=>!p.online).length,10);
  const offline=await f.connect(11,{room:"other"});
  const pending=(await ok(offline,"inbox")).page.rows;assert.equal(pending.length,1);
  await ok(offline,"accept",{invite:pending[0].id});await until(()=>again.state()?.crew?.count===12,"cross-room membership");
  const updated=(await ok(again,"roster")).page;
  assert.equal(updated.rows.find(p=>p.id===offline.welcome.account).sameRoom,false);
  // IDs exposed to peers and private rosters do not contain the ownership proof.
  assert.equal(JSON.stringify(updated).includes(owner(0)),false);
  assert.equal(JSON.stringify(again.welcome).includes(token(0)),false);
  // A large committed roster must be readable in complete, disjoint pages.
  await f.stop();
  const file=path.join(f.dir,"crews.json"),saved=JSON.parse(fs.readFileSync(file,"utf8"));
  for(let n=12;n<64;n++){
    const id=createHash("sha256").update("public fixture account "+n).digest("hex").slice(0,32);
    saved.accounts[owner(n)]={id,name:`Player${n}`,crew:cid};saved.crews[cid].members.push(id);
  }
  fs.writeFileSync(file,JSON.stringify(saved));await f.start();
  const large=await f.connect(0),first=(await ok(large,"roster")).page,second=(await ok(large,"roster",{offset:50})).page;
  assert.equal(first.total,64);assert.equal(first.rows.length,50);assert.equal(second.rows.length,14);
  assert.equal(new Set([...first.rows,...second.rows].map(p=>p.id)).size,64);
});

test("revocation, competing membership and failed storage cannot silently change a crew",async t=>{
  const f=await fixture(t),a=await f.connect(0),b=await f.connect(1),c=await f.connect(2);
  await ok(a,"create",{name:"First"});await ok(c,"create",{name:"Second"});
  await until(()=>a.state()?.crew&&c.state()?.crew,"two crews");
  await ok(a,"invite",{name:"Player1"});await ok(c,"invite",{name:"Player1"});
  const invites=(await ok(b,"inbox")).page.rows;assert.equal(invites.length,2);
  await ok(a,"revoke",{invite:invites.find(i=>i.crew==="First").id});
  await denied(b,"accept","invitation",{invite:invites.find(i=>i.crew==="First").id});
  await ok(b,"accept",{invite:invites.find(i=>i.crew==="Second").id});
  await denied(b,"create","one-crew",{name:"Third"});
  const d=await f.connect(3),dOther=await f.connect(3,{room:"other"});
  await ok(a,"invite",{name:"Player3"});await ok(c,"invite",{name:"Player3"});
  const competing=(await ok(d,"inbox")).page.rows;
  const results=await Promise.all([d.request("accept",{invite:competing[0].id}),dOther.request("accept",{invite:competing[1].id})]);
  assert.equal(results.filter(r=>r.ok).length,1,"one account cannot accept two crews concurrently");
  await until(()=>a.state()?.crew?.count+c.state()?.crew?.count===4,"concurrent join notifications");
  // Force a real filesystem failure without changing permissions or touching real storage.
  const displaced=f.dir+"-moved";fs.renameSync(f.dir,displaced);fs.writeFileSync(f.dir,"blocked");
  try {await denied(a,"rename","storage",{name:"Lost"});}
  finally {fs.unlinkSync(f.dir);fs.renameSync(displaced,f.dir);}
  assert.equal(a.state().crew.name,"First");
  await f.stop();await f.start();
  const restored=await f.connect(0);assert.equal(restored.welcome.crew,"First");
  const member=await f.connect(1);await ok(member,"leave");await until(()=>member.state()?.crew===null,"leave");
  const leader=await f.connect(2);await ok(leader,"disband");await until(()=>leader.state()?.crew===null,"disband");
});


test("only pinned Admin can moderate; account mutes survive reconnect and restart",async t=>{
 const f=await fixture(t,16,{admin:true}),a=await f.connect(0,{name:"Admin"}),b=await f.connect(1),c=await f.connect(2,{room:"other"});
 assert.equal((await b.moderate("mute",{name:"Admin"})).error,"forbidden");
 const nonAdminAlias=await f.connect(0);
 assert.equal((await nonAdminAlias.moderate("announce",{text:"forged Admin"})).error,"forbidden");
 let r=await a.moderate("mute",{name:"Player1",minutes:10});assert.equal(r.ok,true,JSON.stringify(r));
 for(const to of [undefined,c.welcome.id]){
  b.send({t:"chat",text:"muted message",...(to?{to}:{})});
  await until(()=>b.messages.filter(m=>m.t==="chat"&&m.error==="muted").length===(to?2:1),"mute rejection");
 }
 await f.stop();await f.start();
 const aa=await f.connect(0,{name:"Admin"}),bb=await f.connect(1),observer=await f.connect(2);
 bb.send({t:"chat",text:"still muted"});await until(()=>bb.messages.some(m=>m.error==="muted"),"persistent mute");
 r=await aa.moderate("muted");assert.equal(r.data.total,1);assert.equal(r.data.rows[0].name,"Player1");
 assert.equal(JSON.stringify(r).includes(owner(1)),false,"private owner leaked");
 assert.equal((await aa.moderate("unmute",{name:"Player1"})).ok,true);
 bb.send({t:"chat",text:"allowed again"});await until(()=>observer.messages.some(m=>m.t==="chat"&&m.text==="allowed again"),"unmuted chat");
 assert.equal(observer.messages.some(m=>m.text==="still muted"),false);
 assert.equal((await aa.moderate("mute",{name:"Player1",minutes:0})).error,"duration");
 assert.equal((await aa.moderate("mute",{name:"Admin"})).error,"protected");
});

test("reports stay private; Admin announcements and kick are server-owned",async t=>{
 const f=await fixture(t,16,{admin:true}),a=await f.connect(0,{name:"Admin"}),b=await f.connect(1),c=await f.connect(2,{room:"other"});
 const report=await b.moderate("report",{name:"Player2",text:"Repeated abuse"});assert.equal(report.ok,true);
 assert.equal((await b.moderate("reports")).error,"forbidden");
 const inbox=await a.moderate("reports");assert.equal(inbox.data.rows[0].text,"Repeated abuse");
 assert.equal(JSON.stringify(inbox).includes(owner(1)),false);
 assert.equal(c.messages.some(m=>m.data?.rows),false);
 assert.equal((await b.moderate("kick",{name:"Player2"})).error,"forbidden");
 assert.equal((await b.moderate("announce",{text:"fake announcement"})).error,"forbidden");
 assert.equal((await a.moderate("announce",{text:"fuck you"})).error,"profanity");
 assert.equal((await a.moderate("announce",{text:"Race starts soon"})).ok,true);
 await until(()=>c.messages.some(m=>m.k==="announcement"&&m.text==="Race starts soon"),"cross-room announcement");
 const closed=once(c.ws,"close");assert.equal((await a.moderate("kick",{name:"Player2",text:"Abuse"})).ok,true);
 const [event]=await closed;assert.equal(event.code,1008);
 const stored=JSON.parse(fs.readFileSync(path.join(f.dir,"moderation.json")));
 assert.equal(stored.audit.at(-1).op,"kick");
});

test("Admin nickname and crew variants are reserved, including protected claims",async t=>{
 const f=await fixture(t,16,{admin:true}),a=await f.connect(0,{name:"Admin"}),b=await f.connect(1);
 for(const name of ["Admin","aDmIn","A_d-m.i.n","FakeAdmin","Ａｄｍｉｎ","4dm1n"]){
  const ws=new WebSocket(f.url());await once(ws,"open");const closed=once(ws,"close");
  ws.send(JSON.stringify({t:"hi",name,claim:token(1),crew:"Admin",admin:true}));
  const [event]=await closed;assert.equal(event.code,1008);
 }
 for(const name of ["Admin","A_d-m.i.n","FakeAdmin"]){await denied(b,"create","reserved",{name});}
 await ok(a,"create",{name:"Admin"});
 await denied(a,"invite","reserved",{name:"Player1"});
 const state=await fetch(f.url().replace('ws:','http:').replace('/lobby','/names?name=Admin'));
 assert.equal((await state.json()).state,"reserved");
});


test("expired mutes stop blocking; a failed moderation save has no live effect",async t=>{
 const f=await fixture(t,16,{admin:true}),a=await f.connect(0,{name:"Admin"}),b=await f.connect(1);
 assert.equal((await a.moderate("mute",{name:"Player1",minutes:1})).ok,true);
 await f.stop();const file=path.join(f.dir,"moderation.json"),data=JSON.parse(fs.readFileSync(file));
 Object.values(data.mutes).forEach(m=>m.until=1);fs.writeFileSync(file,JSON.stringify(data));await f.start();
 const aa=await f.connect(0,{name:"Admin"}),bb=await f.connect(1);
 bb.send({t:"chat",text:"expired mute is gone"});await until(()=>aa.messages.some(m=>m.text==="expired mute is gone"),"expiry");
 fs.mkdirSync(path.join(f.dir,"moderation.json.tmp"));
 assert.equal((await aa.moderate("mute",{name:"Player1"})).error,"storage");
 bb.send({t:"chat",text:"failed save did not mute me"});await until(()=>aa.messages.some(m=>m.text==="failed save did not mute me"),"failed save rollback");
});
