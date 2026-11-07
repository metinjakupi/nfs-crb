import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../../source/web/language.js', import.meta.url), 'utf8');
const KEY = 'HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Need for Speed Carbon';
const settle = async () => { for (let i = 0; i < 20; i++) await new Promise(r => setImmediate(r)); };
const notFound = () => Object.assign(new Error('missing'), { name: 'NotFoundError' });

function folder(tree = {}) {
  const items = new Map();
  for (const [name, value] of Object.entries(tree))
    items.set(name, typeof value === 'string' ? { kind: 'file', text: value } : folder(value));
  const dir = {
    kind: 'directory', items,
    async getDirectoryHandle(name, { create } = {}) {
      if (!items.has(name)) { if (!create) throw notFound(); items.set(name, folder()); }
      const d = items.get(name); if (d.kind !== 'directory') throw new TypeError('not a folder'); return d;
    },
    async getFileHandle(name, { create } = {}) {
      if (!items.has(name)) { if (!create) throw notFound(); items.set(name, { kind: 'file', text: '' }); }
      const f = items.get(name);
      return { kind: 'file', getFile: async () => ({ text: async () => f.text }), createWritable: async () => {
        if (dir.failWrites) throw new Error('disk full');
        let out = ''; return { write: async t => { out += t; }, close: async () => { f.text = out; }, abort: async () => {} };
      } };
    },
    async *entries() { for (const [name, h] of items) yield [name, h.kind === 'file' ? { kind: 'file' } : h]; },
  };
  return dir;
}

// slot > 1: `main` is the main save that prepareProfile copies; `registry` means the copy exists.
async function startPage({ files = {}, registry, main, slot = 1, manifest, manifestReady, failWrites = false, stall = false, hooks = {} } = {}) {
  const json = value => typeof value === 'string' ? value : JSON.stringify(value);
  const folderName = slot > 1 ? `profile-${slot}` : 'profile';
  const profiles = { profile: main !== undefined ? { 'registry.json': json(main) } : {} };
  if (registry !== undefined) profiles[folderName] = { 'registry.json': json(registry), ...(slot > 1 ? { '.copied': '' } : {}) };
  const root = folder({ nfsc: { game: files, ...profiles } });
  const nfsc = root.items.get('nfsc');
  const profileDir = () => nfsc.items.get(folderName);
  if (profileDir()) profileDir().failWrites = failWrites;
  const host = { children: [], append(n) { this.children.push(n); } };
  const select = { value: '', options: [], listeners: [], append(o) { this.options.push(o); }, addEventListener(_, fn) { this.listeners.push(fn); } };
  const note = { textContent: '' };
  let prepared = 0, playing = false;
  const playListeners = [];
  const page = {
    ...hooks,
    slot, profileFolder: folderName,
    panel: id => id === 'language' ? host : null,
    gameFolder: async () => (await root.getDirectoryHandle('nfsc')).getDirectoryHandle('game'),
    async entry(dir, ...parts) {
      for (const part of parts) {
        let found = null;
        for await (const [name, h] of dir.entries()) if (name.toLowerCase() === part.toLowerCase()) { found = dir.items.get(name); break; }
        if (!found) return null; dir = found;
      }
      return dir;
    },
    async prepareProfile() {
      prepared++;
      await new Promise(r => setImmediate(r));
      const own = await nfsc.getDirectoryHandle(folderName, { create: true });
      if (slot > 1 && !own.items.has('.copied')) {
        for (const [name, item] of nfsc.items.get('profile').items) if (item.kind === 'file') own.items.set(name, { ...item });
        own.items.set('.copied', { kind: 'file', text: '' });
      }
      return folderName;
    },
    playStarting: () => playing,
  };
  const controller = new AbortController();
  const scope = {
    window: { recompStartPage: page, recompGame: { id: 'nfsc', streamAssets: !!manifest, downloadManifest: manifest ? 'https://assets.example/manifest.json' : '' } },
    navigator: { storage: { getDirectory: async () => root } },
    addEventListener: (type, fn) => { if (type === 'recomp-play-state') playListeners.push(fn); },
    document: { createElement: () => ({ set innerHTML(_) {}, querySelector: s => s === 'select' ? select : note }) },
    fetch: async (_, { signal } = {}) => {
      if (stall) return new Promise((_, reject) => signal?.addEventListener('abort', () => reject(signal.reason)));
      await manifestReady;
      return { ok: true, json: async () => ({ files: (manifest || []).map(path => ({ path, size: 1 })) }) };
    },
    AbortSignal: { timeout: () => controller.signal },
    Option: class { constructor(label, value) { this.label = label; this.value = value; } },
    console: { warn() {} }, Error, JSON, Object, Array, Set, String, Promise,
  };
  vm.runInNewContext(source, scope);
  await settle();
  const saved = () => { const f = profileDir()?.items.get('registry.json'); return f ? JSON.parse(f.text) : undefined; };
  const choose = async prefix => { select.value = prefix; for (const fn of select.listeners) fn(); await settle(); };
  const replace = value => profileDir().items.set('registry.json', { kind: 'file', text: JSON.stringify(value) });
  const play = value => { playing = value; for (const fn of playListeners) fn(); };
  return { page, host, select, note, saved, choose, replace, play, prepared: () => prepared, abort: () => controller.abort() };
}

const both = { LANGUAGES: { 'ENGLISH_FRONTEND.BIN': 'en', 'FRENCH_FRONTEND.BIN': 'fr', 'LABELS.BIN': 'x' } };

test('French in the game files is offered and saved as Carbon\'s Language value, keeping the other settings', async () => {
  const p = await startPage({ files: both, registry: { [KEY]: { g_Brightness: { type: 4, data: 52 } } } });
  assert.deepEqual(p.select.options.map(o => [o.label, o.value]), [['English', 'ENGLISH'], ['Français', 'FRENCH']]);
  assert.equal(p.select.value, 'ENGLISH');
  await p.choose('FRENCH');
  assert.deepEqual(p.saved(), { [KEY]: { g_Brightness: { type: 4, data: 52 }, Language: { type: 1, data: 'French' } } });
  assert.match(p.note.textContent, /Français is saved/);
  await p.page.prepareLanguage();
  assert.equal(p.saved()[KEY].Language.data, 'French');
});

test('the saved language is shown, and English goes back to the install default (no value, no empty key)', async () => {
  const p = await startPage({ files: both, registry: { [KEY.toUpperCase()]: { LANGUAGE: { type: 1, data: 'french' } } } });
  assert.equal(p.select.value, 'FRENCH');
  await p.choose('ENGLISH');
  assert.deepEqual(p.saved(), {});
});

test('a first French choice creates registry.json; English alone never creates it', async () => {
  const fr = await startPage({ files: both });
  await fr.choose('FRENCH');
  assert.deepEqual(fr.saved(), { [KEY]: { Language: { type: 1, data: 'French' } } });
  const en = await startPage({ files: { LANGUAGES: { 'ENGLISH_FRONTEND.BIN': 'en' } } });
  assert.equal(en.host.children.length, 0);
  await en.page.prepareLanguage();
  assert.equal(en.saved(), undefined);
});

test('Play falls back to English when the saved language\'s files are gone, and to French when it is the only one', async () => {
  const gone = await startPage({ files: { languages: { 'english_frontend.bin': 'en' } }, registry: { [KEY]: { Language: { type: 1, data: 'French' }, SIZE: { type: 4, data: 100 } } } });
  await gone.page.prepareLanguage();
  assert.deepEqual(gone.saved(), { [KEY]: { SIZE: { type: 4, data: 100 } } });
  const only = await startPage({ files: { LANGUAGES: { 'FRENCH_FRONTEND.BIN': 'fr' } } });
  assert.equal(only.host.children.length, 0);
  await only.page.prepareLanguage();
  assert.deepEqual(only.saved(), { [KEY]: { Language: { type: 1, data: 'French' } } });
});

test('a language this page does not offer stays saved, and an unreadable registry is left as it is', async () => {
  const german = { [KEY]: { Language: { type: 1, data: 'German' } } };
  const p = await startPage({ files: { LANGUAGES: { 'ENGLISH_FRONTEND.BIN': 'en' } }, registry: german });
  await p.page.prepareLanguage();
  assert.deepEqual(p.saved(), german);
  const broken = await startPage({ files: both, registry: '{not json' });
  await broken.page.prepareLanguage();
  assert.equal(broken.host.children.length, 0);
});

test('hosted games list languages from the download manifest', async () => {
  const p = await startPage({ manifest: ['Carbon/NFSC.exe', 'Carbon/Languages/English_Frontend.bin', 'Carbon/LANGUAGES/FRENCH_FRONTEND.BIN', 'Carbon/LANGUAGES/GERMAN_FRONTEND.BIN'] });
  assert.deepEqual(p.select.options.map(o => o.value), ['ENGLISH', 'FRENCH']);
});

test('a choice that could not be saved stops Play', async () => {
  const p = await startPage({ files: both, failWrites: true });
  await p.choose('FRENCH');
  assert.match(p.note.textContent, /could not be saved/);
  await assert.rejects(p.page.prepareLanguage(), /disk full/);
});

test('a restored backup waits for the choice being written, then shows its own language; earlier restore hooks still run', async () => {
  const calls = [];
  const hooks = { beforeProfileRestore: async () => calls.push('before'), afterProfileRestore: async () => calls.push('after') };
  const p = await startPage({ files: both, hooks });
  p.select.value = 'FRENCH';
  for (const fn of p.select.listeners) fn();
  await p.page.beforeProfileRestore();
  assert.equal(p.saved()[KEY].Language.data, 'French');
  assert.equal(p.select.disabled, true);
  p.replace({ [KEY]: { SIZE: { type: 4, data: 100 } } });
  await p.page.afterProfileRestore();
  assert.deepEqual(calls, ['before', 'after']);
  assert.equal(p.select.value, 'ENGLISH');
  assert.equal(p.select.disabled, false);
  await p.page.prepareLanguage();
  assert.deepEqual(p.saved(), { [KEY]: { SIZE: { type: 4, data: 100 } } });
});

test('a choice that could not be saved no longer stops Play once a backup is restored', async () => {
  const p = await startPage({ files: both, failWrites: true });
  await p.choose('FRENCH');
  await p.page.beforeProfileRestore();
  p.replace({ [KEY]: { SIZE: { type: 4, data: 100 } } });
  await p.page.afterProfileRestore();
  await assert.doesNotReject(p.page.prepareLanguage());
  assert.doesNotMatch(p.note.textContent, /could not be saved/);
});

test('a hosted manifest that never answers gives up, and Play keeps the saved language', async () => {
  const french = { [KEY]: { Language: { type: 1, data: 'French' } } };
  const p = await startPage({ manifest: ['Carbon/LANGUAGES/ENGLISH_FRONTEND.BIN'], registry: french, stall: true });
  let done = false;
  const play = p.page.prepareLanguage().then(() => { done = true; });
  await settle();
  assert.equal(done, false);
  p.abort();
  await play;
  assert.deepEqual(p.saved(), french);
});

test('the list is locked while Play gets the game ready, and usable again when Play stops', async () => {
  const p = await startPage({ files: both });
  assert.equal(p.select.disabled, false);
  p.play(true);
  assert.equal(p.select.disabled, true);
  p.play(false);
  assert.equal(p.select.disabled, false);
});

test('Play pressed while the languages are still being found prepares the save folder once', async () => {
  let ready;
  const manifestReady = new Promise(r => { ready = r; });
  const p = await startPage({ manifest: ['LANGUAGES/ENGLISH_FRONTEND.BIN', 'LANGUAGES/FRENCH_FRONTEND.BIN'], manifestReady });
  const play = p.page.prepareLanguage();
  ready();
  await play;
  await settle();
  assert.equal(p.select.options.length, 2);
  assert.equal(p.prepared(), 1);
});

test('a window not played yet shows the main save\'s language and is copied from it only by Play', async () => {
  const french = { [KEY]: { Language: { type: 1, data: 'French' }, SIZE: { type: 4, data: 100 } } };
  const p = await startPage({ files: both, slot: 2, main: french });
  assert.equal(p.select.value, 'FRENCH');
  assert.equal(p.prepared(), 0);
  assert.equal(p.saved(), undefined);
  await p.page.prepareLanguage();
  assert.deepEqual(p.saved(), french);
});
