/* Network lifecycle for Carbon's original encounter activity. Native code
 * supplies the destination map, race, standings and result presentation. */
"use strict";
(() => {
  if(globalThis.carbonEncounterClient)return;
  globalThis.carbonEncounterClient=true;
  const VERSION=4, MAGIC=0x314e4343, RP=152, PH={offered:1,selecting:2,chosen:3,running:4,over:5,declined:6,failed:7,prompt:8,accepted:9};
  let me=0, session=null, command=0, action=0, lastOffer=0, capAt=0, caps=new Map();
  let peopleSource=null,peopleMap=new Map();
  const uint=x=>Number.isInteger(x)&&x>0&&x<=0xffffffff;
  const now=()=>performance.now();
  const crewKey=value=>window.recompCrewKey?window.recompCrewKey(value):String(value||"").trim().toLowerCase();
  const rival=id=>{const other=members().get(id);const mine=crewKey(window.recompOnlineState?.crew);const theirs=crewKey(other?.crew);return !!other&&!!mine&&!!theirs&&mine!==theirs;};
  const members=()=>{const source=window.recompOnlineState?.people;if(source!==peopleSource){peopleSource=source;peopleMap=new Map((source||[]).map(p=>[p.id,p]));}return peopleMap;};
  const send=(op,extra={})=>window.recompOnlineSend?.({t:"race",k:"encounter",v:VERSION,op,...extra});
  const wire=(op,extra={})=>session&&send(op,{s:session.id,to:session.peer,...extra});
  function native() {
    const b=globalThis.carbonEncounterBlock;if(!b)return null;
    const v=new DataView(b.memory.buffer),u=o=>v.getUint32(b.ptr+o,true),f=o=>v.getFloat32(b.ptr+o,true);
    if(u(0)!==MAGIC||u(4)!==3)return null;
    const lock=new Int32Array(b.memory.buffer,b.ptr+0x300,1),seq=Atomics.load(lock,0);if(seq&1)return null;
    const g={phase:u(0x304),session:u(0x308),offer:u(0x30c),to:u(0x310),eligible:!!u(0x314),destination:u(0x318),error:u(0x31c),car:u(0x320),
      own:Array.from({length:6},(_,i)=>f(0x324+i*4)),peer:Array.from({length:6},(_,i)=>f(0x33c+i*4)),ownFinish:f(0x354),finishSession:u(0x358),resultWinner:u(0x35c)};
    return seq===Atomics.load(lock,0)?g:null;
  }
  function raceGame() {
    const b=globalThis.carbonRaceBlock;if(!b)return null;
    const v=new DataView(b.memory.buffer),u=o=>v.getUint32(b.ptr+o,true),f=o=>v.getFloat32(b.ptr+o,true),lock=new Int32Array(b.memory.buffer,b.ptr+0x260,1),seq=Atomics.load(lock,0);
    if(seq&1)return null;
    const g={phase:u(0x264),session:u(0x2bc),loaded:u(0x2b8),place:u(0x2a0),laps:u(0x2a4),done:!!u(0x2a8),pct:f(0x2ac),time:f(0x2b0),lapPct:f(0x2c0),cp:u(0x2c4),ms:u(0x2c8)};
    return seq===Atomics.load(lock,0)?g:null;
  }
  function halfPing() {
    const b=globalThis.carbonStreetBlock;if(!b)return 0;
    return Math.min(1000,new DataView(b.memory.buffer).getUint32(b.ptr+16,true)/2);
  }
  function write() {
    const b=globalThis.carbonEncounterBlock;if(!b)return;
    const v=new DataView(b.memory.buffer),seq=new Int32Array(b.memory.buffer,b.ptr+8,1),p=b.ptr+12;
    const u=(o,x)=>v.setUint32(p+o,x>>>0,true),f=(o,x)=>v.setFloat32(p+o,Number.isFinite(x)?x:0,true);
    Atomics.add(seq,0,1);
    try {
      u(0,command);u(4,action);u(8,session?.peer||0);u(12,session?.id||0);u(16,me);u(20,session?.destination||0);
      const available=new Set([...caps].filter(([id,c])=>now()-c.at<12000&&members().has(id)&&(id===session?.peer||(c.ready&&rival(id)))).map(([id])=>id));
      const ids=[...new Set([session?.peer,...(window.recompOnlineState?.slotIds||[]),...available])].filter(id=>available.has(id)).slice(0,16);
      u(24,ids.length);for(let i=0;i<16;i++)u(28+i*4,ids[i]||0);
      for(let i=0;i<12;i++)f(92+i*4,session?.starts?.[i]||0);
      for(let i=0;i<0x254;i+=4)u(140+i,0);
      if(session) {
        const s=session,o=140,ps=s.progress||{},grid=me<s.peer?0:1;
        u(o,s.id);u(o+8,me);u(o+12,grid);u(o+16,1);u(o+20,s.go?s.id:0);u(o+24,s.launched?s.id:0);
        const r=o+0x54;u(r,s.peer);u(r+4,s.peerCar);u(r+8,1-grid);u(r+12,s.peerFinish?1:0);f(r+16,s.peerFinish);
        f(r+20,ps.pct);f(r+24,ps.lp);u(r+28,ps.laps);u(r+32,ps.cp);u(r+36,ps.ms);f(r+40,ps.rate);f(r+44,ps.lapRate);
        const text=new TextEncoder().encode(members().get(s.peer)?.name||"FRIEND");
        for(let i=0;i<16;i++)v.setUint8(p+r+48+i,i<15&&i<text.length?text[i]:0);
      }
      u(736,session?.result?.winner||0);f(740,session?.result?.time||0);
    } finally {Atomics.add(seq,0,1);}
    // net.js uses this normal racer block to request the peer's poses at any distance.
    const rb=globalThis.carbonRaceBlock;
    if(session&&rb){const rs=new Int32Array(rb.memory.buffer,rb.ptr+8,1);Atomics.add(rs,0,1);try{
      new Uint8Array(rb.memory.buffer,rb.ptr+12,0x254).set(new Uint8Array(b.memory.buffer,b.ptr+RP,0x254));
    }finally{Atomics.add(rs,0,1);}}
  }
  function cmd(a){action=a;command=(command+1)>>>0;write();}
  function own(s) {session=s;globalThis.carbonEncounterSession=s.id;dispatchEvent(new CustomEvent("carbon-encounter-session",{detail:true}));write();}
  function make(id,peer,host,car){return {id,peer,host,peerCar:car||0,at:now(),heard:now(),progress:{},lastProgress:0,lastAlive:0,lastFinish:0,lastReady:0};}
  function cancel(reason="cancelled",notify=true) {
    if(!session||session.cancelling||session.clearing)return;
    if(notify)wire("cancel",{reason});session.cancelling=true;session.cancelAt=now();cmd(3);
  }
  function clear() {
    session=null;globalThis.carbonEncounterSession=0;action=0;write();
    dispatchEvent(new CustomEvent("carbon-encounter-session",{detail:false}));
  }
  function formation(a) {
    if(!Array.isArray(a)||a.length!==12||!a.every(x=>Number.isFinite(x)&&Math.abs(x)<100000))return false;
    for(let i=0;i<2;i++){const n=Math.hypot(...a.slice(i*6+3,i*6+6));if(n<0.7||n>1.3)return false;}
    return true;
  }
  function launch(destination,starts,peerCar) {
    const s=session;if(!s||s.launched||!uint(destination)||!formation(starts))return;
    s.destination=destination;s.starts=starts;s.peerCar=peerCar||s.peerCar;s.launched=true;s.launchAt=now();cmd(2);
  }
  function maybeGo() {
    const s=session;if(!s||s.host!==me||s.sentGo||!s.ready||!s.peerReady||s.cancelling)return;
    s.sentGo=true;const delay=1500;wire("go",{delay,half:halfPing()});scheduleGo(delay,s.id);
  }
  function scheduleGo(delay,id) {
    setTimeout(()=>{if(!session||session.id!==id||session.cancelling)return;session.go=true;write();},Math.max(0,delay));
  }
  const finishTime=x=>Number.isFinite(x)&&x>0&&x<1200;
  const sameTime=(a,b)=>finishTime(a)&&finishTime(b)&&Math.fround(a)===Math.fround(b);
  function registeredFinish(winner,time) {
    const s=session;if(!s||s.host!==me||!s.go||s.cancelling||s.clearing||s.decided)return;
    // Decide from relayed finishes, including our own echo. Both players use
    // this one host decision instead of independent local finish order.
    s.decided={winner,time};s.lastResult=now();wire("result",s.decided);
  }
  function receiveResult(m) {
    const s=session;if(!s||m.id!==s.host||m.s!==s.id||!s.go||s.cancelling||s.clearing||
      (m.winner!==me&&m.winner!==s.peer)||!finishTime(m.time))return;
    const known=m.winner===me?s.myFinish:s.peerFinish;
    if(!known){s.pendingResult=m;return;}
    if(!sameTime(known,m.time)||s.result)return;
    s.result={winner:m.winner,time:known};s.pendingResult=null;s.resultCommandAt=now();cmd(6);
  }
  function heard(m) {
    if(!m||m.t!=="race"||m.k!=="encounter"||m.v!==VERSION||!uint(m.id))return;
    const active=session;
    if(m.id===me) {
      if(active&&active.host===me&&m.s===active.id&&m.to===active.peer&&active.go) {
        if(m.op==="finish"&&sameTime(active.myFinish,m.time))registeredFinish(me,active.myFinish);
        else if(m.op==="result"&&active.decided&&m.winner===active.decided.winner&&sameTime(m.time,active.decided.time))receiveResult(m);
      }
      return;
    }
    if(!members().has(m.id))return;
    if(active&&m.id===active.peer&&m.to===me&&m.s===active.id&&active.go&&!active.cancelling&&!active.clearing) {
      if(m.op==="finish"&&finishTime(m.time)) {
        if(active.peerFinish&&!sameTime(active.peerFinish,m.time))return;
        active.heard=now();active.peerFinish=m.time;registeredFinish(m.id,m.time);
        if(active.pendingResult)receiveResult(active.pendingResult);write();return;
      }
      if(m.op==="result"){active.heard=now();receiveResult(m);return;}
    }
    const g=native();if(!g)return;
    if(m.op==="cap"&&uint(m.car)){caps.set(m.id,{at:now(),car:m.car,ready:m.ready===true});return;}
    if(m.to!==me||!uint(m.s))return;
    if(m.op==="offer") {
      if(session?.id===m.s&&session.peer===m.id)return;
      if((g.session&&g.session!==m.s&&!session)||session?.cancelling||session?.clearing){send("decline",{s:m.s,to:m.id});return;}
      if(!caps.has(m.id)||!rival(m.id)||m.id>me||!g.eligible||(session&&!(session.host===me&&session.peer===m.id&&m.id<me))) {
        send("decline",{s:m.s,to:m.id});return;
      }
      if(session)wire("cancel",{reason:"crossed offers"});
      own(make(m.s,m.id,m.id,uint(m.car)?m.car:caps.get(m.id)?.car));cmd(1);return;
    }
    const s=session;if(!s||m.s!==s.id||m.id!==s.peer)return;s.heard=now();
    if(m.op==="cancel"||m.op==="decline"){cancel(m.op,false);return;}
    if(s.cancelling||s.clearing)return;
    if(m.op==="accept"){s.peerAccepted=true;return;}
    if(m.op==="pick"&&s.host===me&&s.accepted&&s.peerAccepted&&!s.go&&uint(m.destination)&&formation(m.starts)&&uint(m.car)) {
      if(!g.eligible&&!s.launched){cancel("not free roam");return;}
      if(!s.launched)launch(m.destination,m.starts,m.car);
      wire("commit",{destination:s.destination,starts:s.starts,car:g.car});return;
    }
    if(m.op==="commit"&&s.host===s.peer&&g.phase===PH.chosen&&m.destination===s.destination&&formation(m.starts)&&
       // The relay may round JSON doubles. The native start records use floats.
       m.starts.every((x,i)=>Math.fround(x)===Math.fround(s.starts?.[i]))&&uint(m.car)) {s.pendingCommit=m;return;}
    if(m.op==="ready"&&s.launched){s.peerReady=true;maybeGo();return;}
    if(m.op==="go"&&m.id===s.host&&s.ready&&s.peerReady&&!s.goPending&&m.delay>=500&&m.delay<=3000&&m.half>=0&&m.half<=1000) {
      s.goPending=true;scheduleGo(m.delay-m.half-halfPing(),s.id);return;
    }
    if(m.op==="progress"&&s.go&&Number.isFinite(m.pct)&&m.pct>=0&&m.pct<=100&&Number.isFinite(m.lp)&&m.lp>=0&&m.lp<=100&&
       Number.isInteger(m.ms)&&m.ms>0&&m.ms<=0xffffffff&&Number.isInteger(m.laps)&&m.laps>=0&&m.laps<100&&Number.isInteger(m.cp)&&m.cp>=0&&m.cp<10000) {
      const p=s.progress,dt=p.ms?(m.ms-p.ms)/1000:0;if(p.ms&&m.ms<p.ms)return;
      const rate=dt>0&&dt<3?Math.max(0,Math.min(50,(m.pct-(p.pct||0))/dt)):0;
      const lapRate=dt>0&&dt<3?Math.max(0,Math.min(50,(m.lp-(p.lp||0))/dt)):0;
      s.progress={pct:m.pct,lp:m.lp,laps:m.laps,cp:m.cp,ms:m.ms,rate,lapRate};write();return;
    }
  }
  function tick() {
    const state=window.recompOnlineState;me=uint(state?.me)?state.me:0;
    const g=native(),r=raceGame();if(!g)return;
    const offline=!me||!state?.open;
    if(offline){
      if(!session&&g.session&&g.to)own(make(g.session,g.to,me,0));
      if(session)cancel("connection lost",false);
      if(!session){write();return;}
    }
    if(!offline&&g.car&&now()-capAt>3000){capAt=now();send("cap",{car:g.car,ready:g.phase===0&&g.eligible});}
    if(!offline&&g.offer!==lastOffer){lastOffer=g.offer;if(g.to&&g.session&&!session){own(make(g.session,g.to,me,caps.get(g.to)?.car));wire("offer",{car:g.car});}}
    const s=session;
    if(!s){write();return;}
    if(s.clearing){if(!g.session){clear();return;}if(now()-(s.resetAt||0)>500){s.resetAt=now();cmd(4);}write();return;}
    if(!members().has(s.peer))cancel("player left");
    if(!s.go&&!rival(s.peer))cancel("crews changed");
    if(s.host!==me&&!s.launched&&g.session!==s.id&&now()-s.at>5000)cancel("encounter unavailable");
    if(s.host!==me&&!s.launched&&!s.cancelling&&g.session!==s.id&&now()-(s.openAt||0)>500){s.openAt=now();cmd(1);}
    if(g.session===s.id&&(g.phase===PH.declined||g.phase===PH.failed))cancel("native encounter cancelled");
    if(s.cancelling) {
      if(g.phase===PH.declined||g.phase===PH.failed||!g.session||(g.phase===PH.over&&(!r||r.phase===0))) {s.clearing=true;cmd(4);}
      write();return;
    }
    if(g.session===s.id&&g.phase===PH.accepted)s.accepted=true;
    if(s.accepted&&!s.go&&now()-(s.acceptAt||0)>1000){s.acceptAt=now();wire("accept");}
    if(s.host!==me&&s.accepted&&s.peerAccepted&&g.session===s.id&&g.phase===PH.accepted&&g.eligible&&now()-(s.mapAt||0)>500){s.mapAt=now();cmd(5);}
    if(g.session===s.id&&g.phase===PH.chosen&&!s.destination&&uint(g.destination)) {
      s.destination=g.destination;s.starts=me<s.peer?[...g.own,...g.peer]:[...g.peer,...g.own];
    }
    if(s.pendingCommit&&g.eligible&&!s.launched)launch(s.pendingCommit.destination,s.pendingCommit.starts,s.pendingCommit.car);
    if(s.host!==me&&s.destination&&!s.launched&&now()-(s.pickAt||0)>1000){s.pickAt=now();wire("pick",{destination:s.destination,starts:s.starts,car:g.car});}
    if(r?.session===s.id&&r.phase>=3&&r.phase<=5){s.ready=true;if(!s.go&&now()-s.lastReady>1000){s.lastReady=now();wire("ready");}maybeGo();}
    if(s.go&&r?.session===s.id&&r.phase>=4) {
      if(now()-s.lastProgress>=250){s.lastProgress=now();wire("progress",{pct:r.pct,lp:r.lapPct,laps:r.laps,cp:r.cp,ms:r.ms});}
    }
    if(s.go&&g.finishSession===s.id&&g.ownFinish>0&&!s.myFinish)s.myFinish=g.ownFinish;
    if(s.pendingResult&&!s.result)receiveResult(s.pendingResult);
    if(s.decided&&now()-s.lastResult>1000){s.lastResult=now();wire("result",s.decided);}
    if(s.result&&g.session===s.id&&g.phase===PH.running&&g.resultWinner!==s.result.winner&&now()-(s.resultCommandAt||0)>500){s.resultCommandAt=now();cmd(6);}
    if(s.go&&s.myFinish&&now()-s.lastFinish>1000){s.lastFinish=now();wire("finish",{time:s.myFinish});}
    if(g.session===s.id&&g.phase===PH.over&&r?.phase===0) {
      if(!s.result||g.resultWinner!==s.result.winner){cancel("incomplete encounter");return;}
      s.clearing=true;cmd(4);return;
    }
    if(now()-s.lastAlive>2000){s.lastAlive=now();wire("alive");}
    if((!s.launched&&now()-s.at>120000)||(s.launched&&!s.go&&now()-s.launchAt>60000)||(now()-s.heard>20000)||now()-s.at>720000)cancel("timeout");
    write();
  }
  addEventListener("recomp-online-message",e=>heard(e.detail));
  addEventListener("carbon-encounter",()=>{lastOffer=0;write();});
  setInterval(tick,100);
})();
