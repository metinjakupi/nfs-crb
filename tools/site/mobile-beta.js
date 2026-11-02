"use strict";
(() => {
  const key = 'carbon.mobile-startup.v1';
  let current = null;
  const save = () => { try { localStorage.setItem(key, JSON.stringify(current)); } catch {} };
  window.carbonMobileRuntime = imports => {
    for (const group of Object.values(imports)) {
      for (const value of Object.values(group || {})) {
        if (value instanceof WebAssembly.Memory) window.carbonMobileMemory = value;
      }
    }
  };
  window.carbonMobileLog = message => {
    if (!current) return;
    const text = String(message);
    // Keep fixed engine categories only, never arbitrary messages containing identity data.
    const category = /d3d9 webgpu:/i.test(text) ? 'graphics' : /out of memory|allocation failed|cannot allocate|cannot map|\boom\b|stack overflow/i.test(text) ? 'allocation failure' : /\brace:|\[race\]/i.test(text) ? 'race event' : /\bstream:|\[stream\]/i.test(text) ? 'asset streaming' : null;
    if (!category) return;
    current.events ||= [];
    const event = {category, at:Date.now(), memoryBytes:window.carbonMobileMemory?.buffer.byteLength, numbers:(text.match(/\b\d+(?:\.\d+)?\b/g) || []).slice(0,16)};
    if (category === 'allocation failure') event.error = text.replace(/https?:\/\/\S+|Bearer\s+\S+|[a-f0-9]{64}/gi,'[redacted]').slice(0,500);
    current.events.push(event);
    current.events = current.events.slice(-30); save();
  };
  document.addEventListener('pointerdown', () => {
    if (!current) return;
    current.lastInteraction = {at:Date.now(),memoryBytes:window.carbonMobileMemory?.buffer.byteLength}; save();
  }, {capture:true,passive:true});
  window.carbonMobileStage = phase => {
    if (!current) return;
    if (current.stages.at(-1)?.phase === phase) return;
    current.stages.push({phase, at: Date.now()}); save();
  };
  // A separate report survives a browser-process termination. Never capture save data,
  // player names, cloud proofs, URLs, or arbitrary game logs.
  let previous = null;
  try { previous = JSON.parse(localStorage.getItem(key)); } catch {}
  const panel = document.createElement('section');
  panel.style.cssText = 'position:fixed;inset:0;z-index:1000;background:#070c16;color:#e9f6ff;overflow:auto;padding:24px;font:16px/1.5 system-ui';
  const title = document.createElement('h1'); title.textContent = 'Carbon mobile startup test';
  const text = document.createElement('p'); text.textContent = 'Rotate to landscape, then start the test. If Safari closes or reloads, open this same link again and download the previous results. This test uses the current game runtime.';
  const start = document.createElement('button'); start.textContent = 'Start test';
  if (new URLSearchParams(location.search).has('results')) start.hidden = true;
  const download = document.createElement('button'); download.textContent = 'Download previous results'; download.hidden = !previous;
  for (const button of [start, download]) button.style.cssText = 'min-height:48px;padding:12px 18px;margin:8px;background:#5fe4f2;border:0;color:#070c16;font:inherit';
  const summary = document.createElement('p');
  if (previous) summary.textContent = 'Previous last stage: ' + (previous.stages?.at(-1)?.phase || 'unknown');
  download.onclick = () => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(previous,null,2)],{type:'application/json'}));
    const a = document.createElement('a'); a.href = url; a.download = 'carbon-mobile-startup.json'; a.click(); setTimeout(() => URL.revokeObjectURL(url),1000);
  };
  panel.append(title,text,summary,start,download); document.body.append(panel);
  // Hold the normal start-page script until the explicit test button is pressed.
  const originalAppend = document.body.append.bind(document.body);
  let queued = null;
  document.body.append = (...nodes) => {
    if (nodes.length === 1 && nodes[0] instanceof HTMLScriptElement && nodes[0].getAttribute('src') === 'start.js' && !current) { queued = nodes[0]; return; }
    return originalAppend(...nodes);
  };
  start.onclick = () => {
    if (!queued) { summary.textContent = 'The game page is still loading. Try again in a moment.'; return; }
    current = {version:6, profile:'768MiB-arena-4-workers-16MiB-stacks-16MiB-growth-base-render-1x-canvas', renderScale:1, canvasPixelRatio:1, started: new Date().toISOString(), userAgent:navigator.userAgent, cores:navigator.hardwareConcurrency, stages:[]};
    window.carbonMobileStage('test started'); panel.remove(); originalAppend(queued);
    setInterval(() => {
      const memory = window.carbonMobileMemory?.buffer || window.Module?.HEAPU8?.buffer;
      if (memory) {
        current.memoryBytes = memory.byteLength;
        current.peakMemoryBytes = Math.max(current.peakMemoryBytes || 0,memory.byteLength);
      }
      current.phase = window.recompStartupPhase;
      const canvas = document.getElementById('canvas');
      if (canvas) current.display = {canvasWidth:canvas.width,canvasHeight:canvas.height,cssWidth:canvas.clientWidth,cssHeight:canvas.clientHeight,dpr:devicePixelRatio};
      if (window.recompFailure) current.failure = {kind:window.recompFailure.kind, message:String(window.recompFailure.message).slice(0,500)};
      save();
    }, 1000);
  };
})();
