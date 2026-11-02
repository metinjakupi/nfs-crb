import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const report=await readFile(process.env.CARBON_REPORT_SOURCE || new URL('../../source/web/report.js',import.meta.url),'utf8');
test('reports retain loaded page/runtime/reporter identities without fetching the current release',async()=>{
 let tick;const saved=new Map();
 const c={setInterval(fn){tick=fn;},sessionStorage:{getItem:k=>saved.get(k),setItem:(k,v)=>saved.set(k,v)},addEventListener(){},TextEncoder,URLSearchParams,Date,Error,console:{log(){},info(){},warn(){},error(){}},document:{getElementById:()=>null},location:new URL('https://example.test/'),navigator:{userAgent:'test'},localStorage:{getItem:()=>null},screen:{width:1,height:1},innerWidth:1,innerHeight:1,devicePixelRatio:1,crossOriginIsolated:true,
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

test('reports include bounded input, storage, timing and errors without URL credentials',async()=>{
 const events={},saved=new Map();let tick,raf,clock=1000;
 const c={setInterval(fn){tick=fn;},setTimeout,clearTimeout,requestAnimationFrame(fn){raf=fn;},
 sessionStorage:{getItem:k=>saved.get(k),setItem:(k,v)=>saved.set(k,v)},addEventListener(type,fn){events[type]=fn;},TextEncoder,URLSearchParams,Date,Error,URL,
 performance:{now:()=>clock,getEntriesByType:()=>[{name:'https://assets.test/file.bin?key=SECRET',duration:62,initiatorType:'fetch',transferSize:100,encodedBodySize:100,decodedBodySize:200,responseStatus:503}]},
 console:{log(){},info(){},warn(){},error(){}},document:{hidden:false,visibilityState:'visible',getElementById:()=>null},location:new URL('https://example.test/'),
 navigator:{userAgent:'test',hardwareConcurrency:8,storage:{estimate:async()=>({quota:1000,usage:400})},getGamepads:()=>[{id:'Test Pad',connected:true,index:0,mapping:'standard',axes:[0,-.7],buttons:[{pressed:true,value:1}]}]},
 localStorage:{getItem:()=>null},screen:{width:1,height:1},innerWidth:1,innerHeight:1,devicePixelRatio:1,crossOriginIsolated:true,
 carbonGraphicsInfo:{vendor:'nvidia',source:'active device adapter'},carbonDiagnostics:{downloads:[{url:'https://assets.test/file?token=SECRET'}]}};
 c.window=c;vm.runInNewContext(report,c);
 raf?.(clock);clock+=16;raf?.(clock);clock+=120;raf?.(clock);
 events.error?.({error:new Error('bad input'),filename:'https://example.test/menu.js?claim=SECRET',lineno:9,colno:2});
 for(let i=0;i<100;i++){clock++;tick();}
 const text=await c.recompReport.build(500*1024,'');
 assert.match(text,/"quota":1000,"usage":400/);assert.match(text,/"axes":\[0,-0.7\]/);
 assert.match(text,/"gapsOver50ms":1/);assert.match(text,/not rendered game FPS/);
 assert.match(text,/bad input/);assert.match(text,/menu.js/);assert.doesNotMatch(text,/SECRET/);
 const history=JSON.parse(text.match(/^Controller history .*?: (.+)$/m)[1]);assert.equal(history.length,1);
 const timing=JSON.parse(text.match(/^Browser timing .*?: (.+)$/m)[1]);assert.equal(timing.maxMs,120);
 assert.ok(new TextEncoder().encode(text).length<=500*1024);
 // Repeated native resets need their own timestamps; smooth browser callbacks
 // must not hide a stalled presentation path. Keep report overhead bounded.
 for(let i=0;i<90;i++)c.console.log('[recomp] d3d9: Reset to 2550x1285');
 c.console.warn('presenter: FAULT: target pool stalled for 250ms; guest frame dropped');
 const renderingText=await c.recompReport.build(500*1024,'');
 assert.match(renderingText,/^Renderer events .*?: /m,'report must retain timestamped native renderer events');
 const renderer=JSON.parse(renderingText.match(/^Renderer events .*?: (.+)$/m)[1]);
 assert.equal(renderer.resets,90);assert.equal(renderer.presentationStallReports,1);
 assert.equal(renderer.recent.length,80);assert.equal(renderer.recent[0].width,2550);
 assert.equal(renderer.recent.at(-1).kind,'presentation stall');
 assert.ok(renderer.recent.every(e=>!Number.isNaN(Date.parse(e.at))));
 assert.ok(new TextEncoder().encode(renderingText).length<=500*1024);
 c.innerWidth=2040;events.resize();
 const resized=JSON.parse((await c.recompReport.build(500*1024,'')).match(/^Renderer events .*?: (.+)$/m)[1]);
 assert.equal(resized.recent.at(-1).kind,'window resize');assert.equal(resized.recent.at(-1).windowWidth,2040);
 c.navigator.storage.estimate=async()=>{throw new Error('storage access denied')};
 assert.match(await c.recompReport.build(500*1024,''),/storage access denied/);
});
