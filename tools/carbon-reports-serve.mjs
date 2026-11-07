#!/usr/bin/env node
// carbon-reports-serve.mjs - the reports viewer as a small local server.
//
//   npm run reports:serve            then open http://127.0.0.1:8791/
//
// Every page load first copies the reports that are new from the relay host (tools/carbon-reports-sync.sh,
// read-only there) and rebuilds the page from logs/reports-all. A load within 10 s of the last sync reuses it.
// The "Refresh now" button does the same and reloads. If the host cannot be reached the page still opens with
// what is here, and says so. Only this computer can reach it: it listens on 127.0.0.1 and answers only requests
// whose Host is localhost, because the reports hold player names.
import { createServer } from "node:http";
import { execFile } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { loadAll, buildPage } from "./carbon-reports-viewer.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const SYNC_MAX_AGE_MS = 10_000;

export function realSync() {
  return new Promise(resolve => {
    execFile("bash", [join(root, "tools/carbon-reports-sync.sh")], { timeout: 120_000, cwd: root }, (err, stdout, stderr) => {
      if (err) return resolve({ error: String(stderr || err.message).trim().split("\n").pop().slice(0, 300), added: 0 });
      const m = String(stdout).match(/(\d+) new report/);
      resolve({ added: m ? +m[1] : 0 });
    });
  });
}

export function createReportsServer({ dir = join(root, "logs/reports-all"), sync = realSync, now = Date.now } = {}) {
  let last = { at: 0, error: "", added: 0 }, running = null;
  const doSync = force => {
    if (running) return running;
    if (!force && last.at && now() - last.at < SYNC_MAX_AGE_MS) return Promise.resolve(last);
    running = sync().then(r => { last = { at: now(), error: r.error || "", added: r.added || 0 }; return last; })
      .catch(e => { last = { at: now(), error: String(e?.message || e), added: 0 }; return last; })
      .finally(() => { running = null; });
    return running;
  };
  const local = h => /^(127\.0\.0\.1|localhost)(:\d+)?$/.test(h || "");
  return createServer(async (req, res) => {
    const send = (code, type, body) => { res.writeHead(code, { "content-type": type, "cache-control": "no-store", "x-content-type-options": "nosniff" }); res.end(body); };
    if (!local(req.headers.host)) return send(403, "text/plain", "forbidden");
    const url = new URL(req.url, "http://localhost");
    if (req.method === "POST" && url.pathname === "/refresh") {
      const origin = req.headers.origin;
      if (origin && !local(origin.replace(/^https?:\/\//, ""))) return send(403, "text/plain", "forbidden");
      const r = await doSync(true);
      return send(200, "application/json", JSON.stringify({ added: r.added, error: r.error }));
    }
    if (req.method !== "GET") return send(405, "text/plain", "method not allowed");
    if (url.pathname === "/" || url.pathname === "/index.html") {
      const r = await doSync(false);
      const data = loadAll(dir);
      const status = r.error ? `Could not reach the host (${r.error}); showing what is here.`
        : `Synced ${new Date(last.at).toISOString().slice(11, 16)} UTC` + (r.added ? `, ${r.added} new` : ", nothing new");
      return send(200, "text/html; charset=utf-8", buildPage(data, undefined, { status }));
    }
    const m = url.pathname.match(/^\/reports-all\/(\d{8}-\d{6}-[0-9a-z]+)\.txt$/);
    if (m && existsSync(join(dir, m[1] + ".txt"))) return send(200, "text/plain; charset=utf-8", readFileSync(join(dir, m[1] + ".txt")));
    send(404, "text/plain", "not found");
  });
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  // 8791 unless PORT says otherwise; a port that is taken (another local server) moves on to the next one.
  const first = +(process.env.PORT || 8791);
  const tryPort = port => {
    const server = createReportsServer();
    server.once("error", e => {
      if (e.code === "EADDRINUSE" && port < first + 20) return tryPort(port + 1);
      console.error(e.message); process.exit(1);
    });
    server.listen(port, "127.0.0.1", () => console.log(`reports viewer: http://127.0.0.1:${port}/  (Ctrl-C to stop)`));
  };
  tryPort(first);
}
