// The start page's one-time settings defaults (source/web/start.js motionBlurOffOnce and
// reflectionsHalfOnce): the
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
function load(tree, name = "motionBlurOffOnce") {
  const run = new Function("navigator", "game", body + "\nreturn " + name + ";");
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

const reflections = (level) => JSON.stringify({ [KEY]: { g_CarEnvironmentMapEnable: { type: 4, data: level }, g_RainEnable: { type: 4, data: 1 } } });

test("car reflections at the top level are set to the middle one once", async () => {
  const tree = { nfsc: { profile: { "registry.json": reflections(3) } } };
  await load(tree, "reflectionsHalfOnce")("profile");
  const settings = JSON.parse(tree.nfsc.profile["registry.json"]);
  assert.equal(settings[KEY].g_CarEnvironmentMapEnable.data, 2);
  assert.deepEqual(settings[KEY].g_RainEnable, { type: 4, data: 1 });
  assert.ok("car-reflections-default-v1" in tree.nfsc.profile);
});

test("lower reflection levels are never raised, and a player's own choice is kept", async () => {
  for (const level of [0, 1, 2]) {
    const tree = { nfsc: { profile: { "registry.json": reflections(level) } } };
    await load(tree, "reflectionsHalfOnce")("profile");
    assert.equal(JSON.parse(tree.nfsc.profile["registry.json"])[KEY].g_CarEnvironmentMapEnable.data, level);
  }
  const tree = { nfsc: { profile: { "registry.json": reflections(3) } } };
  const once = load(tree, "reflectionsHalfOnce");
  await once("profile");
  tree.nfsc.profile["registry.json"] = reflections(3); // set back to the top level in the game's options
  await once("profile");
  assert.equal(JSON.parse(tree.nfsc.profile["registry.json"])[KEY].g_CarEnvironmentMapEnable.data, 3);
});

test("the motion blur and reflection defaults keep separate markers", async () => {
  const tree = { nfsc: { profile: { "registry.json": JSON.stringify({ [KEY]: { g_MotionBlurEnable: { type: 4, data: 1 }, g_CarEnvironmentMapEnable: { type: 4, data: 3 } } }) } } };
  await load(tree)("profile");
  await load(tree, "reflectionsHalfOnce")("profile");
  const settings = JSON.parse(tree.nfsc.profile["registry.json"]);
  assert.equal(settings[KEY].g_MotionBlurEnable.data, 0);
  assert.equal(settings[KEY].g_CarEnvironmentMapEnable.data, 2);
  assert.ok("motion-blur-default-v1" in tree.nfsc.profile && "car-reflections-default-v1" in tree.nfsc.profile);
});

// The performance overlay, off once in mod-settings.json (overlayOffOnce).
const modSettings = (overlay) =>
  `{\n "host.controls/hidden": 281474976710656,\n "host.display/frame_limit": 2,\n "host.display/performance_overlay": ${overlay},\n "core.nfsc/widescreen": 1\n}\n`;

test("the performance overlay is switched off once and every other value keeps its bytes", async () => {
  const tree = { nfsc: { profile: { "mod-settings.json": modSettings(2) } } };
  await load(tree, "overlayOffOnce")("profile");
  assert.equal(tree.nfsc.profile["mod-settings.json"], modSettings(0));
  assert.ok("performance-overlay-default-v1" in tree.nfsc.profile);
});

test("a player who turns the performance overlay back on keeps it", async () => {
  const tree = { nfsc: { profile: { "mod-settings.json": modSettings(2) } } };
  const once = load(tree, "overlayOffOnce");
  await once("profile");
  tree.nfsc.profile["mod-settings.json"] = modSettings(1); // F11 or F9 → Display & graphics
  await once("profile");
  assert.equal(tree.nfsc.profile["mod-settings.json"], modSettings(1));
});

test("a slot without mod settings yet gets the overlay off and nothing else", async () => {
  const tree = { nfsc: { profile: {} } };
  await load(tree, "overlayOffOnce")("profile");
  assert.deepEqual(JSON.parse(tree.nfsc.profile["mod-settings.json"]), { "host.display/performance_overlay": 0 });
});

test("settings without the overlay row get it added, the rest unchanged", async () => {
  const text = `{\n "host.display/frame_limit": 2\n}\n`;
  const tree = { nfsc: { profile: { "mod-settings.json": text } } };
  await load(tree, "overlayOffOnce")("profile");
  const out = tree.nfsc.profile["mod-settings.json"];
  assert.deepEqual(JSON.parse(out), { "host.display/performance_overlay": 0, "host.display/frame_limit": 2 });
});

test("a slot whose folder does not exist yet is left for the next Play", async () => {
  const tree = { nfsc: {} };
  await load(tree, "overlayOffOnce")("profile-3");
  assert.deepEqual(tree.nfsc, {});
});
