// The Skip intro movies setting: compiles the real source/mods/core/nfsc/intro.h against a small
// stand-in for the mod API, so both values are checked by running the hook, and keeps the
// mod.toml default and the C fallback in step.
import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

const intro = new URL("../../source/mods/core/nfsc/intro.h", import.meta.url).pathname;
const toml = fs.readFileSync(new URL("../../source/mods/core/nfsc/mod.toml", import.meta.url), "utf8");

const harness = `
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int PopModStatus;
typedef struct pop_cpu_v1 pop_cpu_v1;
typedef struct PopHookInvocation PopHookInvocation;
typedef struct PopModApi PopModApi;
struct PopModApi {
    PopModStatus (*guest_ptr)(const PopModApi *, uint32_t, uint32_t, void **);
    void (*guest_write_u32)(const PopModApi *, uint32_t, uint32_t);
};
typedef void (*Hook)(const PopModApi *, pop_cpu_v1 *, PopHookInvocation *, void *);
enum { POP_OK = 0, POP_HOOK_BEFORE = 0 };
static char queued[64];
static uint32_t flag, hooked;
static Hook hook;
static long long stored = -1;
static uint32_t get_u32(uint32_t a) { (void)a; return flag; }
static long long setting(const char *key, long long fallback) { (void)key; return stored < 0 ? fallback : stored; }
static void log_line(const char *t) { (void)t; }
static PopModStatus install(uint32_t a, Hook h, int when, int bytes) { (void)when; (void)bytes; hooked = a; hook = h; return POP_OK; }
static PopModStatus ptr(const PopModApi *a, uint32_t at, uint32_t n, void **out) { (void)a; (void)at; (void)n; *out = queued; return POP_OK; }
static void wr(const PopModApi *a, uint32_t at, uint32_t v) { (void)a; (void)at; flag = v; }
#include "${intro}"
static const PopModApi api = {ptr, wr};
static void frame(const char *movie) { strcpy(queued, movie); if (hook) hook(&api, 0, 0, 0); printf("%s=%u ", movie, flag); }
int main(int argc, char **argv) {
    if (argc > 1) sscanf(argv[1], "%lld", &stored);
    intro_install();
    printf("hook=%d ", hooked != 0);
    for (int i = 2; i < argc; ++i) frame(argv[i]);
    return 0;
}`;

const cc = spawnSync("cc", ["--version"]).status === 0;
function run(value, ...movies) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "carbon-intro-"));
  try {
    fs.writeFileSync(path.join(dir, "t.c"), harness);
    execFileSync("cc", ["-w", "-o", path.join(dir, "t"), path.join(dir, "t.c")]);
    return execFileSync(path.join(dir, "t"), [String(value), ...movies], { encoding: "utf8" }).trim();
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
}

test("the default is on in mod.toml and in the C fallback", () => {
  assert.match(toml, /^skip_intro = \{ type = "bool", default = true,/m);
  assert.match(fs.readFileSync(intro, "utf8"), /setting\("skip_intro", 1\)/);
});

test("on: the logo and warning are skipped, and story movies still play", { skip: !cc }, () => {
  assert.equal(run(1, "EAHD_Bumper", "psa_nikki", "Seq01_opening", "EA_Bumper_WS"),
    "hook=1 EAHD_Bumper=1 psa_nikki=1 Seq01_opening=0 EA_Bumper_WS=1");
});

test("off: no hook is installed and every movie plays", { skip: !cc }, () => {
  assert.equal(run(0, "EAHD_Bumper", "PSA_Nikki", "Seq01_opening"),
    "hook=0 EAHD_Bumper=0 PSA_Nikki=0 Seq01_opening=0");
});

test("a profile with no saved value gets the default, on", { skip: !cc }, () => {
  assert.equal(run(-1, "EAHD_Bumper"), "hook=1 EAHD_Bumper=1");
});
