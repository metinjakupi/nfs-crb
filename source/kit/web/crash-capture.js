// crash-capture.js - a --pre-js of the web build: where a thread was when it crashed.
//
// The game runs in Emscripten pthread workers. When one of them traps ("memory
// access out of bounds"), the page only gets the worker's ErrorEvent: a message,
// a file and a line, never the stack (Emscripten's worker.onerror rethrows that
// event, so window.recompFailure ends up with stack ""). Here, inside the worker,
// the 'error' event still carries the RuntimeError itself, whose stack names the
// wasm functions (wasm-function[N]; CarbonRecomp.js.symbols maps N to fn_XXXXXXXX,
// the translated game function, or to the kit function). The worker also asks the
// runtime for the guest's register file (recomp_crash_probe, host/crash_probe.cpp,
// when the wasm exports it) and prints one line, "[crash] {json}", through err():
// in a pthread that is Module.printErr proxied to the page, so the line lands in
// this tab's game log (window.recompLog), in order, and report.js turns it into
// the report's "Crash location" section. A trap on the page's own thread is
// printed the same way. Nothing here runs until a thread throws.
(() => {
  const worker = typeof WorkerGlobalScope !== "undefined" && self instanceof WorkerGlobalScope;
  if (!worker && typeof window === "undefined") return;
  // Ten frames (the default) rarely reach the game function under a trap in a helper.
  try { Error.stackTraceLimit = Math.max(Error.stackTraceLimit || 0, 64); } catch {}
  let posted = 0;
  function probe() {
    try {
      const exports = typeof wasmExports !== "undefined" ? wasmExports : null;
      const memory = typeof wasmMemory !== "undefined" ? wasmMemory : null;
      if (!exports?.recomp_crash_probe || !memory) return "";
      const at = exports.recomp_crash_probe() >>> 0;
      const heap = new Uint8Array(memory.buffer);
      if (!at || at >= heap.length) return "";
      let end = at;
      while (end < heap.length && heap[end] && end - at < 4096) end++;
      return new TextDecoder().decode(heap.slice(at, end)); // a copy: TextDecoder refuses shared memory
    } catch (e) {
      return `probe failed: ${e?.message || e}`;
    }
  }
  function report(e) {
    if (posted >= 4) return;
    const error = e.error;
    const wasm = typeof WebAssembly !== "undefined" && error instanceof WebAssembly.RuntimeError;
    // The page's own exceptions already reach the page with their stack; only traps are new there.
    if (!worker && !wasm) return;
    posted++;
    let thread = 0;
    try { thread = typeof _pthread_self === "function" ? _pthread_self() >>> 0 : 0; } catch {}
    const record = {
      at: new Date().toISOString(), where: worker ? `worker ${self.name || ""}`.trim() : "page",
      thread, name: String(error?.name || ""), message: String(error?.message ?? e.message ?? "").slice(0, 2000),
      stack: String(error?.stack || "").slice(0, 24000), source: String(e.filename || ""), line: e.lineno || 0,
      column: e.colno || 0, wasm, guest: wasm ? probe() : "",
      memoryBytes: typeof wasmMemory !== "undefined" ? wasmMemory?.buffer?.byteLength ?? null : null,
    };
    let line;
    try { line = "[crash] " + JSON.stringify(record); } catch { return; }
    try { (typeof err === "function" ? err : console.error)(line); } catch {}
  }
  self.addEventListener("error", report);
})();
