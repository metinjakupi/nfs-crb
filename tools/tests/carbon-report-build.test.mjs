import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const report=await readFile(new URL('../../source/web/report.js',import.meta.url),'utf8');
test('reports retain loaded page/runtime/reporter identities without fetching the current release',async()=>{
 let tick;const saved=new Map();
 const c={setInterval(fn){tick=fn;},sessionStorage:{getItem:k=>saved.get(k),setItem:(k,v)=>saved.set(k,v)},addEventListener(){},TextEncoder,Date,Error,console:{log(){},info(){},warn(){},error(){}},document:{getElementById:()=>null},location:new URL('https://example.test/'),navigator:{userAgent:'test'},localStorage:{getItem:()=>null},screen:{width:1,height:1},innerWidth:1,innerHeight:1,devicePixelRatio:1,crossOriginIsolated:true,
 recompPageBuild:{id:'page-old'},recompRuntimeBuild:{id:'runtime-old',wasm:'wasm-old'},recompReportBuild:{id:'report-old'},carbonGraphicsInfo:{vendor:'test'},fetch:()=>{throw Error('must not fetch latest build');}};
 c.window=c;vm.runInNewContext(report,c);
 const text=await c.recompReport.build(50000,'');
 assert.match(text,/Page build: page-old/);assert.match(text,/Runtime build: runtime-old/);assert.match(text,/Reporter build: report-old/);assert.match(text,/Runtime wasm SHA-256: wasm-old/);
 // A newly fetched reporter must not relabel the already-installed report builder.
 c.recompReportBuild={id:'report-new'};
 assert.match(await c.recompReport.build(50000,''),/Reporter build: report-old/);
 delete c.recompRuntimeBuild;
 assert.match(await c.recompReport.build(50000,''),/Runtime build: unknown/);
 // The report must distinguish an empty opponent roster from an asset read failure.
 const buffer=new ArrayBuffer(0x400),v=new DataView(buffer),put=(o,x)=>v.setUint32(o,x,true);
 put(0x260,2);put(0x264,3);put(0x2b4,1);put(0x2b8,1);put(0x2d0+24,1);
 c.carbonRaceBlock={memory:{buffer},ptr:0};
 c.carbonTestAssets={unavailable:{path:'/CARS/RX7/GEOMETRY.BIN',message:'byte check failed'}};
 const diagnostic=await c.recompReport.build(50000,'');
 const roster=JSON.parse(diagnostic.match(/^Race roster: (.+)$/m)[1]);
 assert.equal(roster.count,1);assert.equal(roster.rows[0].local,1);
 assert.match(diagnostic,/CARS\/RX7\/GEOMETRY.BIN/);
 tick();tick();
 assert.equal(JSON.parse(saved.get('carbon.race-diagnostics.v1')).length,1,'unchanged roster does not grow history');
 put(0x2b4,2);put(0x260,4);tick();
 const history=JSON.parse((await c.recompReport.build(50000,'')).match(/^Race history: (.+)$/m)[1]);
 assert.deepEqual(history.map(x=>x.count),[1,2]);
 put(0x260,3);
 assert.match(await c.recompReport.build(50000,''),/Race roster: \{"busy":true\}/);
});
