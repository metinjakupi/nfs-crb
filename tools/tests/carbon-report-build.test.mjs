import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
const report=await readFile(new URL('../../source/web/report.js',import.meta.url),'utf8');
test('reports retain loaded page/runtime/reporter identities without fetching the current release',async()=>{
 const c={addEventListener(){},TextEncoder,Date,Error,console:{log(){},info(){},warn(){},error(){}},document:{getElementById:()=>null},location:new URL('https://example.test/'),navigator:{userAgent:'test'},localStorage:{getItem:()=>null},screen:{width:1,height:1},innerWidth:1,innerHeight:1,devicePixelRatio:1,crossOriginIsolated:true,
 recompPageBuild:{id:'page-old'},recompRuntimeBuild:{id:'runtime-old',wasm:'wasm-old'},recompReportBuild:{id:'report-old'},carbonGraphicsInfo:{vendor:'test'},fetch:()=>{throw Error('must not fetch latest build');}};
 c.window=c;vm.runInNewContext(report,c);
 const text=await c.recompReport.build(50000,'');
 assert.match(text,/Page build: page-old/);assert.match(text,/Runtime build: runtime-old/);assert.match(text,/Reporter build: report-old/);assert.match(text,/Runtime wasm SHA-256: wasm-old/);
 // A newly fetched reporter must not relabel the already-installed report builder.
 c.recompReportBuild={id:'report-new'};
 assert.match(await c.recompReport.build(50000,''),/Reporter build: report-old/);
 delete c.recompRuntimeBuild;
 assert.match(await c.recompReport.build(50000,''),/Runtime build: unknown/);
});
