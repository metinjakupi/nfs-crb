import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createReportsServer } from "../carbon-reports-serve.mjs";

const report = `Report received 2026-11-06T21:42:06.608Z
Page: https://nfscrb.xyz/releases/20261106-1g-effec484/play/nfsc/
Player name: Tester
Browser: Chrome 157 on Windows (Mozilla/5.0 (Windows NT 10.0; Win64; x64))
Stopped: error: Uncaught RuntimeError: memory access out of bounds

--- The game's log: all 0 lines ---
`;

async function start(sync, clock) {
  const dir = mkdtempSync(join(tmpdir(), "reports-"));
  writeFileSync(join(dir, "20261106-214221-bd3383.txt"), report);
  const server = createReportsServer({ dir, sync, now: clock });
  await new Promise(r => server.listen(0, "127.0.0.1", r));
  const base = `http://127.0.0.1:${server.address().port}`;
  return { server, base, close: () => new Promise(r => server.close(r)) };
}

test("a page load syncs once, reuses a sync under 10 s old, and the button forces one", async () => {
  let calls = 0, t = 1_000_000;
  const { base, close } = await start(async () => ({ added: ++calls }), () => t);
  try {
    let html = await (await fetch(base + "/")).text();
    assert.equal(calls, 1);
    assert.match(html, /Game crash: memory access out of bounds/);
    assert.match(html, /Synced \d\d:\d\d UTC, 1 new/);
    await fetch(base + "/");
    assert.equal(calls, 1);
    t += 11_000;
    await fetch(base + "/");
    assert.equal(calls, 2);
    const j = await (await fetch(base + "/refresh", { method: "POST" })).json();
    assert.equal(calls, 3);
    assert.deepEqual(j, { added: 3, error: "" });
  } finally { await close(); }
});

test("a failed sync still opens the page and says so", async () => {
  const { base, close } = await start(async () => ({ error: "ssh: connect timed out", added: 0 }), () => 5);
  try {
    const html = await (await fetch(base + "/")).text();
    assert.match(html, /Could not reach the host \(ssh: connect timed out\)/);
    assert.match(html, /Tester/);
  } finally { await close(); }
});

test("raw reports only by id, and nothing for a foreign Host or Origin", async () => {
  const { base, close } = await start(async () => ({ added: 0 }), () => 5);
  try {
    assert.equal((await fetch(base + "/reports-all/20261106-214221-bd3383.txt")).status, 200);
    assert.equal((await fetch(base + "/reports-all/..%2F..%2Fetc%2Fpasswd")).status, 404);
    assert.equal((await fetch(base + "/reports-all/20261106-214221-ffffff.txt")).status, 404);
    // fetch cannot set Host; use http.request
    const { request } = await import("node:http");
    const status = await new Promise(r => { const q = request(base + "/", { headers: { host: "evil.example" } }, res => { res.resume(); r(res.statusCode); }); q.end(); });
    assert.equal(status, 403);
    const post = await fetch(base + "/refresh", { method: "POST", headers: { origin: "https://evil.example" } });
    assert.equal(post.status, 403);
  } finally { await close(); }
});
