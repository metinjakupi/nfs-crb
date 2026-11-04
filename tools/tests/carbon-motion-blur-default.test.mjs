// The start page's one-time motion-blur default (source/web/start.js motionBlurOffOnce): the
// function's own source, run against an in-memory stand-in for the browser's private storage.
import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";

const source = fs.readFileSync(new URL("../../source/web/start.js", import.meta.url), "utf8");
const body = source.slice(source.indexOf("  const BLUR_DEFAULT_MARK"), source.indexOf("  let starting = false;"));

// Directories and files as plain objects, with the handle methods start.js uses.
function storage(tree) {
  const dir = (node) => ({
    async getDirectoryHandle(name, { create } = {}) {
      if (!(name in node)) { if (!create) throw new DOMException("missing", "NotFoundError"); node[name] = {}; }
      if (typeof node[name] === "string") throw new TypeError("not a directory");
      return dir(node[name]);
    },
    async getFileHandle(name, { create } = {}) {
      if (!(name in node)) { if (!create) throw new DOMException("missing", "NotFoundError"); node[name] = ""; }
      return {
        async getFile() { return { text: async () => node[name] }; },
        async createWritable() {
          let text = "";
          return { write: async (s) => { text += s; }, close: async () => { node[name] = text; } };
        },
      };
    },
  });
  return { getDirectory: async () => dir(tree) };
}
function load(tree) {
  const run = new Function("navigator", "game", body + "\nreturn motionBlurOffOnce;");
  return run({ storage: storage(tree) }, { id: "nfsc" });
}
const KEY = "HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Need for Speed Carbon";
const registry = (blur) => JSON.stringify({ [KEY]: { g_MotionBlurEnable: { type: 4, data: blur }, g_RainEnable: { type: 4, data: 1 } } });

test("motion blur is switched off once and the other settings are kept", async () => {
  const tree = { nfsc: { profile: { "registry.json": registry(1) } } };
  await load(tree)("profile");
  const settings = JSON.parse(tree.nfsc.profile["registry.json"]);
  assert.equal(settings[KEY].g_MotionBlurEnable.data, 0);
  assert.deepEqual(settings[KEY].g_RainEnable, { type: 4, data: 1 });
  assert.ok("motion-blur-default-v1" in tree.nfsc.profile);
});

test("a player who turns motion blur back on keeps it", async () => {
  const tree = { nfsc: { profile: { "registry.json": registry(1) } } };
  const once = load(tree);
  await once("profile");
  tree.nfsc.profile["registry.json"] = registry(1); // switched on again in the game's options
  await once("profile");
  assert.equal(JSON.parse(tree.nfsc.profile["registry.json"])[KEY].g_MotionBlurEnable.data, 1);
});

test("a slot without settings yet is left alone and tried again later", async () => {
  const tree = { nfsc: { "profile-2": {} } };
  await load(tree)("profile-2");
  assert.deepEqual(tree.nfsc["profile-2"], {});
  tree.nfsc["profile-2"]["registry.json"] = JSON.stringify({ [KEY]: { g_RainEnable: { type: 4, data: 1 } } });
  await load(tree)("profile-2"); // the game has written settings, but not this value
  assert.ok(!("motion-blur-default-v1" in tree.nfsc["profile-2"]));
  tree.nfsc["profile-2"]["registry.json"] = registry(1);
  await load(tree)("profile-2");
  assert.equal(JSON.parse(tree.nfsc["profile-2"]["registry.json"])[KEY].g_MotionBlurEnable.data, 0);
});

test("a damaged settings file does not stop the game from starting", async () => {
  const tree = { nfsc: { profile: { "registry.json": "{ not json" } } };
  await load(tree)("profile");
  assert.equal(tree.nfsc.profile["registry.json"], "{ not json");
});
