#!/usr/bin/env node
// carbon-reports-viewer.mjs - builds a local, browsable page from the players' problem reports.
//
//   node tools/carbon-reports-viewer.mjs            writes logs/reports-viewer/index.html (open it in a browser)
//   node tools/carbon-reports-viewer.mjs --summary  also prints the counts in the terminal
//   npm run reports                                 sync from the relay host, then build the page
//   npm run reports:serve                           local server: every page load (and the Refresh button) syncs first
//
// Reads logs/reports-all/*.txt (copied by tools/carbon-reports-sync.sh). Nothing leaves this machine: logs/ is not
// in git and the page loads nothing from the network. Each row links to the raw report file.
import { readdirSync, readFileSync, mkdirSync, writeFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const inDir = process.env.REPORTS_DIR || join(root, "logs/reports-all");
const outDir = join(root, "logs/reports-viewer");

// One label per report: the first rule that matches decides. Order matters.
export const CATEGORIES = [
  ["Game crash: memory access out of bounds", r => /memory access out of bounds/i.test(r.stopped)],
  ["Game crash: other (abort / error in play)", r => /^error:|^rejection:|^abort|aborted|RuntimeError/i.test(r.stopped) && !/could not allocate/i.test(r.stopped)],
  ["Could not allocate memory (device too small)", r => /could not allocate memory/i.test(r.stopped)],
  ["Tab closed unexpectedly (killed by the system?)", r => /closed unexpectedly/i.test(r.stopped)],
  ["Startup error: unreadable game manifest (JSON)", r => /JSON/i.test(r.stopped) && /^startup/i.test(r.stopped)],
  ["Browser storage full / too small", r => /storage-quota|not have enough storage|QuotaExceeded|only .* free/i.test(r.stopped)],
  ["iPhone/iPad storage error at startup (private window?)", r => /unknown transient reason/i.test(r.stopped)],
  ["No WebGPU in this browser or device", r => /WebGPU/i.test(r.stopped)],
  ["Blocked: in-app browser / not a secure page", r => /secure browser|cross-origin isolat|isolation|Instagram/i.test(r.stopped)],
  ["A game file / download failed", r => /customization|streamed|could not be loaded|downloaded|Failed to fetch|network error|stream:/i.test(r.stopped)],
  ["Startup bug: \"object can not be cloned\" (older builds)", r => /can not be cloned/i.test(r.stopped)],
  ["Browser storage (OPFS) error at startup", r => /requested file or directory could not be found|GetDirectory|getFileHandle|createWritable|browsing context|changed its game storage|data pipe/i.test(r.stopped)],
  ["Game runtime missing or damaged (build, hash, compile)", r => /web build is missing|runtime-load|hash mismatch|Could not prepare game files/i.test(r.stopped)],
  ["Sent by the player while the game was running", r => /sent while the game was running/i.test(r.stopped)],
  ["Sent by the player with no error recorded", r => r.stopped.trim() === ""],
];

function device(ua) {
  if (/iPhone/i.test(ua)) return "iPhone";
  if (/iPad/i.test(ua)) return "iPad";
  if (/Android/i.test(ua)) return "Android";
  if (/Windows/i.test(ua)) return "Windows";
  if (/Macintosh|Mac OS X/i.test(ua)) return "Mac";
  if (/CrOS/i.test(ua)) return "ChromeOS";
  if (/Linux/i.test(ua)) return "Linux";
  return "other";
}

const mb = n => (n / 1048576) | 0;

export function parseReport(file, text) {
  const head = text.split(/\n--- The game's log/)[0];
  const line = key => (head.match(new RegExp("^" + key + ": (.*)$", "m")) || [])[1] || "";
  const id = file.replace(/\.txt$/, "");
  const m = id.match(/^(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)/);
  const received = m ? `${m[1]}-${m[2]}-${m[3]}T${m[4]}:${m[5]}:${m[6]}Z` : "";
  const ua = line("Browser");
  const quota = (line("Storage estimate (browser quota, not disk capacity)").match(/"quota":(\d+),"usage":(\d+)/) || []);
  const env = line("Environment");
  const crash = (head.match(/Guest: guest thread \d+: EIP=([0-9a-f]{8})/) || [])[1] || "";
  const top = (head.match(/#0 wasm \d+ 0x[0-9a-f]+ (.+)/) || [])[1] || "";
  const noteAt = text.indexOf("Player's note:");
  const note = noteAt < 0 ? "" : text.slice(noteAt + 14).split(/\n--- /)[0].trim().slice(0, 400);
  const log = text.slice(text.indexOf("--- The game's log"));
  const guardLines = [...log.matchAll(/core\.nfsc: guards: [^\n]*(?:dead collection|callee-saved)[^\n]*/g)].map(x => x[0].slice(0, 300));
  const r = {
    id, received,
    player: line("Player name") || "",
    browser: ua.replace(/\s*\(Mozilla.*$/, "").slice(0, 80) || "?",
    device: device(ua),
    ios: (ua.match(/OS (\d+[_\d]*) like Mac/) || [])[1]?.replace(/_/g, ".") || "",
    release: (head.match(/releases\/([0-9a-z-]+)/) || [])[1] || (line("Page build") ? "page " + line("Page build").slice(0, 8) : ""),
    stopped: line("Stopped").slice(0, 200),
    phase: line("Startup phase"),
    deviceMemoryGiB: (env.match(/"deviceMemoryGiB":([\d.]+)/) || [])[1] || "",
    wasmMB: (env.match(/"wasmMemoryBytes":(\d+)/) || (head.match(/WebAssembly memory then: (\d+)/) || []))[1] ? mb(+((env.match(/"wasmMemoryBytes":(\d+)/) || head.match(/WebAssembly memory then: (\d+)/))[1])) : "",
    quotaMB: quota[1] ? mb(+quota[1]) : "", usageMB: quota[2] ? mb(+quota[2]) : "",
    crashEip: crash, crashTop: top.slice(0, 80),
    note, guardLines, bytes: text.length,
  };
  r.category = (CATEGORIES.find(([, f]) => f(r)) || ["Other"])[0];
  return r;
}

export function loadAll(dir = inDir) {
  if (!existsSync(dir)) return [];
  return readdirSync(dir).filter(f => /^\d{8}-\d{6}-.*\.txt$/.test(f)).sort().reverse()
    .map(f => { try { return parseReport(f, readFileSync(join(dir, f), "utf8")); } catch { return null; } }).filter(Boolean);
}

const PAGE = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Carbon player reports</title>
<style>
:root{--bg:#fff;--fg:#1b1f24;--muted:#5b6570;--line:#dde2e8;--card:#f5f7fa;--bar:#2f6fed;--bad:#c62828;--ok:#1b7f3a}
@media (prefers-color-scheme:dark){:root{--bg:#14171b;--fg:#e6e9ed;--muted:#97a1ac;--line:#2a3038;--card:#1c2025;--bar:#5b8cff;--bad:#ff6b6b;--ok:#5fd38d}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}
main{max-width:1180px;margin:0 auto;padding:20px 16px 60px}h1{font-size:20px;margin:0 0 2px}h2{font-size:15px;margin:26px 0 8px}
.sub{color:var(--muted)}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:12px 0}
button{background:var(--bar);color:#fff;border:0;border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}button:disabled{opacity:.6;cursor:wait}
select,input{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px 8px;font:inherit}
input[type=search]{flex:1;min-width:180px}
.cats{display:grid;gap:4px}.cat{display:grid;grid-template-columns:minmax(220px,380px) 1fr 52px;gap:8px;align-items:center;cursor:pointer;padding:2px 4px;border-radius:6px}
.cat:hover,.cat.on{background:var(--card)}.cat .bar{height:10px;background:var(--bar);border-radius:3px;min-width:2px}.cat .n{text-align:right;font-variant-numeric:tabular-nums}
table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--muted);font-weight:600;font-size:12px}
tr.r{cursor:pointer}tr.r:hover{background:var(--card)}tr.d td{background:var(--card);white-space:pre-wrap;word-break:break-word}
.pill{display:inline-block;padding:0 7px;border-radius:9px;background:var(--card);border:1px solid var(--line);font-size:12px}.guard{color:var(--ok);border-color:var(--ok)}
.warn{color:var(--bad)}a{color:var(--bar)}.mono{font-family:ui-monospace,Menlo,monospace;font-size:12px}
@media (max-width:700px){.cat{grid-template-columns:1fr 60px 40px}th:nth-child(n+5),td:nth-child(n+5){display:none}}
</style></head><body><main>
<h1>Carbon player reports</h1><div class="sub" id="meta"></div>
<div class="row"><select id="range"><option value="1">last hour</option><option value="6">last 6 hours</option><option value="24">last 24 hours</option><option value="72">last 3 days</option><option value="0" selected>everything here</option></select>
<select id="device"></select><select id="release"></select><input id="q" type="search" placeholder="search player, browser, note, error text"><label><input type="checkbox" id="guard"> guard fired</label><button id="refresh" hidden>Refresh now</button><span class="sub" id="live"></span></div>
<h2>What stopped the game <span class="sub">(click a line to filter)</span></h2><div class="cats" id="cats"></div>
<div id="crashes"></div>
<h2>Reports <span class="sub" id="count"></span></h2>
<table><thead><tr><th>Time (UTC)</th><th>Player</th><th>Device</th><th>Release</th><th>What stopped it</th><th>Note</th></tr></thead><tbody id="rows"></tbody></table>
</main><script>
const DATA=__DATA__;const LIVE=__LIVE__;const $=s=>document.querySelector(s);let cat="";
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function opts(el,all,vals){el.innerHTML='<option value="">'+all+'</option>'+[...new Set(vals)].filter(Boolean).sort().map(v=>'<option>'+esc(v)+'</option>').join("")}
opts($("#device"),"all devices",DATA.map(r=>r.device));opts($("#release"),"all releases",DATA.map(r=>r.release));
$("#meta").textContent=DATA.length+" reports, "+(DATA.length?DATA[DATA.length-1].received.slice(0,16).replace("T"," ")+" to "+DATA[0].received.slice(0,16).replace("T"," ")+" UTC":"none")+" - built "+__BUILT__;
function filtered(ignoreCat){const h=+$("#range").value,d=$("#device").value,rel=$("#release").value,q=$("#q").value.toLowerCase(),g=$("#guard").checked;
const from=h&&DATA.length?new Date(DATA[0].received).getTime()-h*3600e3:0;
return DATA.filter(r=>(!from||new Date(r.received).getTime()>=from)&&(!d||r.device===d)&&(!rel||r.release===rel)&&(!g||r.guardLines.length)&&(ignoreCat||!cat||r.category===cat)&&(!q||(r.player+" "+r.browser+" "+r.note+" "+r.stopped+" "+r.id).toLowerCase().includes(q)))}
function draw(){const base=filtered(true),by={};for(const r of base)by[r.category]=(by[r.category]||0)+1;const max=Math.max(1,...Object.values(by));
$("#cats").innerHTML=Object.entries(by).sort((a,b)=>b[1]-a[1]).map(([k,n])=>'<div class="cat'+(k===cat?" on":"")+'" data-c="'+esc(k)+'"><span>'+esc(k)+'</span><span><div class="bar" style="width:'+(n/max*100)+'%"></div></span><span class="n">'+n+'</span></div>').join("")||'<div class="sub">nothing in this range</div>';
const list=filtered(false),sites={};for(const r of list)if(r.crashEip){const k=r.crashEip;(sites[k]=sites[k]||{n:0,rel:new Set(),top:r.crashTop}).n++;sites[k].rel.add(r.release)}
const sl=Object.entries(sites).sort((a,b)=>b[1].n-a[1].n);
$("#crashes").innerHTML=sl.length?'<h2>Where the game code crashed <span class="sub">(guest address; same address = same bug)</span></h2><table><thead><tr><th>Address</th><th>Reports</th><th>Releases</th><th>Wasm function</th></tr></thead><tbody>'+sl.map(([k,v])=>'<tr><td class="mono">'+k+'</td><td>'+v.n+'</td><td>'+esc([...v.rel].join(", "))+'</td><td class="mono">'+esc(v.top)+'</td></tr>').join("")+'</tbody></table>':"";
$("#count").textContent=list.length+" shown"+(list.length>300?" (first 300)":"");
$("#rows").innerHTML=list.slice(0,300).map((r,i)=>'<tr class="r" data-i="'+DATA.indexOf(r)+'"><td class="mono">'+r.received.slice(5,16).replace("T"," ")+'</td><td>'+esc(r.player||"-")+'</td><td>'+esc(r.device)+(r.ios?" "+esc(r.ios):"")+'</td><td class="mono">'+esc(r.release.replace(/^2026/,""))+'</td><td>'+esc(r.category)+(r.guardLines.length?' <span class="pill guard">guard fired</span>':"")+(r.crashEip?' <span class="pill mono">'+r.crashEip+'</span>':"")+'</td><td>'+esc(r.note.slice(0,90))+'</td></tr>').join("")}
document.addEventListener("click",e=>{const c=e.target.closest(".cat");if(c){cat=cat===c.dataset.c?"":c.dataset.c;draw();return}
const t=e.target.closest("tr.r");if(!t)return;const nx=t.nextElementSibling;if(nx&&nx.classList.contains("d")){nx.remove();return}const r=DATA[+t.dataset.i];
const d=document.createElement("tr");d.className="d";d.innerHTML='<td colspan="6">'+esc([r.stopped,"Browser: "+r.browser+(r.ios?" (iOS "+r.ios+")":""),"Phase: "+(r.phase||"-"),"Memory: device "+(r.deviceMemoryGiB||"?")+" GiB, game heap "+(r.wasmMB||"?")+" MB; browser storage quota "+(r.quotaMB||"?")+" MB, used "+(r.usageMB||"?")+" MB",r.crashEip?"Crash: guest "+r.crashEip+" in "+r.crashTop:"",r.note?"Player note: "+r.note:"",...r.guardLines.map(x=>"GUARD: "+x)].filter(Boolean).join("\\n"))+'\\n<a href="../reports-all/'+encodeURIComponent(r.id)+'.txt" target="_blank">open the full report</a></td>';t.after(d)});
if(LIVE){const b=$("#refresh");b.hidden=false;$("#live").textContent=LIVE.status||"";b.onclick=async()=>{b.disabled=true;b.textContent="Syncing...";try{const j=await(await fetch("/refresh",{method:"POST"})).json();if(j.error)$("#live").textContent="Sync failed: "+j.error;else location.reload()}catch(e){$("#live").textContent="Sync failed: "+e.message}b.disabled=false;b.textContent="Refresh now"}}
for(const id of ["range","device","release","guard"])$("#"+id).addEventListener("change",draw);$("#q").addEventListener("input",draw);draw();
</script></body></html>`;

export function buildPage(data, built = new Date().toISOString().slice(0, 16).replace("T", " ") + " UTC", live = null) {
  // The data goes in last: a player's text must never be read as one of the markers.
  const safe = v => JSON.stringify(v).replace(/</g, "\\u003c");
  return PAGE.replace("__LIVE__", () => safe(live)).replace("__BUILT__", () => safe(built)).replace("__DATA__", () => safe(data));
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const data = loadAll();
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "index.html"), buildPage(data));
  console.log(`${data.length} reports -> ${join(outDir, "index.html")}`);
  if (process.argv.includes("--summary")) {
    const by = {};
    for (const r of data) by[r.category] = (by[r.category] || 0) + 1;
    for (const [k, n] of Object.entries(by).sort((a, b) => b[1] - a[1])) console.log(String(n).padStart(5), k);
    const g = data.filter(r => r.guardLines.length);
    console.log(`guard fired in ${g.length} report(s)`);
  }
}
