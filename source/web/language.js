// language.js - the start page's game language choice.
//
// Carbon picks its language once at startup from the registry value Language
// (REG_SZ) under HKLM\Software\Electronic Arts\Need for Speed Carbon, then loads
// LANGUAGES\<PREFIX>_FRONTEND.BIN. No value means English US. The table is the
// one in source/tools/setup_kit/package.py, which seeds the Mac app the same way.
// Only the languages below are offered; another Language value a save already
// holds is left alone.
//
// The registry lives in the slot's profile folder (OPFS <game id>/<profile>/
// registry.json, see saves.js), and the runtime rewrites that file as a whole
// while the game runs. This page changes it only from the start page, before
// Play, under the Web Lock "<game id>:registry:<profile>", and never while
// saves.js restores a backup (its before/after restore hooks, below).
//
// Only languages whose _FRONTEND.BIN is in the game's files are offered: the
// imported copy in OPFS, or the hosted manifest's file list. Before every Play
// a saved language whose files are gone (re-import, a restored backup) falls
// back to English, or to the only language there when English is missing.
"use strict";
(() => {
  const KEY = "HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Need for Speed Carbon";
  const LANGUAGES = [
    { prefix: "ENGLISH", registry: "English US", label: "English" },
    { prefix: "FRENCH", registry: "French", label: "Français" },
  ];
  const DEFAULT = LANGUAGES[0];

  // The languages among file paths (any case, / or \ separators) that end in
  // LANGUAGES/<PREFIX>_FRONTEND.BIN, in table order.
  function available(paths) {
    const found = new Set();
    for (const path of paths) {
      const m = /(?:^|[\\/])languages[\\/]([^\\/]+)_frontend\.bin$/i.exec(path);
      if (m) found.add(m[1].toUpperCase());
    }
    return LANGUAGES.filter(l => found.has(l.prefix));
  }

  function carbonKey(registry) {
    return registry && Object.keys(registry).find(k => k.toLowerCase() === KEY.toLowerCase());
  }
  function valueName(values) {
    return values && Object.keys(values).find(n => n.toLowerCase() === "language");
  }
  // The saved Language string, or undefined when there is none (English US).
  function saved(registry) {
    const values = registry?.[carbonKey(registry)];
    const value = values?.[valueName(values)];
    return value && typeof value.data === "string" ? value.data : undefined;
  }
  // The table row for a registry string; undefined for a language not offered here.
  function known(name) {
    if (name === undefined) return DEFAULT;
    return LANGUAGES.find(l => l.registry.toLowerCase() === name.toLowerCase());
  }

  // The language the game should start in: the saved one when its files are
  // there, else English, else the only language there. null: leave it alone
  // (a language not offered here, or no language files found at all).
  function choose(registry, languages) {
    const name = saved(registry), current = known(name);
    if (!current) return null;
    if (!languages.length || languages.includes(current)) return current;
    return languages.includes(DEFAULT) ? DEFAULT : languages[0];
  }

  // A copy of the registry with the language set (English: the value removed,
  // as a fresh install has it). null when nothing would change.
  function withLanguage(registry, language) {
    if (known(saved(registry)) === language) return null;
    const data = { ...(registry || {}) };
    const key = carbonKey(data) || KEY;
    const values = { ...(data[key] || {}) };
    for (const name of Object.keys(values)) if (name.toLowerCase() === "language") delete values[name];
    if (language !== DEFAULT) values.Language = { type: 1, data: language.registry };
    if (Object.keys(values).length) data[key] = values;
    else delete data[key];
    return data;
  }

  globalThis.CarbonLanguage = { LANGUAGES, available, saved, choose, withLanguage };

  const page = window.recompStartPage, game = window.recompGame;
  const host = page?.panel("language");
  if (!host || !game) return;

  async function gameFiles() {
    if (game.streamAssets && game.downloadManifest) {
      // A request that never answers must not hold up Play: no languages then, the saved one stays.
      const response = await fetch(game.downloadManifest, { cache: "no-cache", signal: AbortSignal.timeout(10000) });
      if (!response.ok) throw Error(`Game manifest: HTTP ${response.status}`);
      const { files } = await response.json();
      return Array.isArray(files) ? files.map(f => String(f?.path || "")) : [];
    }
    const folder = await page.entry(await page.gameFolder(), "LANGUAGES");
    if (!folder || folder.kind !== "directory") return [];
    const names = [];
    for await (const [name, handle] of folder.entries()) if (handle.kind === "file") names.push(`LANGUAGES/${name}`);
    return names;
  }
  const detected = gameFiles().then(available).catch(error => {
    console.warn("[language] the game's languages could not be listed:", error);
    return [];
  });

  let profile = null;
  async function profileDir() {
    if (!profile) {
      const folder = await page.prepareProfile();
      const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(game.id);
      profile = { folder, dir: await root.getDirectoryHandle(folder, { create: true }) };
    }
    return profile;
  }
  async function readRegistry(dir) {
    let file;
    try { file = await dir.getFileHandle("registry.json"); }
    catch (e) { if (e.name === "NotFoundError") return null; throw e; }
    const value = JSON.parse(await (await file.getFile()).text());
    if (!value || Array.isArray(value) || typeof value !== "object") throw Error("registry.json is not an object");
    return value;
  }
  // Read, change and write under the slot's lock. change(registry) returns the
  // language to set, or null for none.
  async function update(change) {
    const { folder, dir } = await profileDir();
    const run = async () => {
      const registry = await readRegistry(dir);
      const language = change(registry);
      const next = language && withLanguage(registry, language);
      if (!next) return;
      const writable = await (await dir.getFileHandle("registry.json", { create: true })).createWritable();
      try { await writable.write(JSON.stringify(next, null, 2) + "\n"); await writable.close(); }
      catch (e) { await writable.abort().catch(() => {}); throw e; }
    };
    if (navigator.locks) await navigator.locks.request(`${game.id}:registry:${folder}`, run);
    else await run();
  }

  let pending = Promise.resolve(), failed = null;
  // Before the game starts: the player's last choice must be written (start.js
  // stops Play if it was not), and a saved language whose files are gone is replaced.
  page.prepareLanguage = async () => {
    await pending;
    if (failed) throw failed;
    const languages = await detected;
    // Queued like a choice, so a restore that starts meanwhile waits for this write.
    pending = pending.then(() => update(registry => choose(registry, languages))).catch(error => {
      console.warn("[language] the saved language could not be checked:", error);
    });
    await pending;
  };

  const NOTE = "The game's own translation, from your game files. Applies the next time you press Play.";
  (async () => {
    const languages = await detected;
    if (languages.length < 2) return;
    const { dir } = await profileDir();
    const current = choose(await readRegistry(dir), languages);
    const section = document.createElement("div");
    section.innerHTML = `<label class="cs-row" style="margin:14px 0 6px"><span class="cs-label">Game language</span>
      <select class="cs-language"></select></label>
      <div class="cs-note cs-language-note" role="status" aria-live="polite">${NOTE}</div>`;
    const select = section.querySelector("select"), note = section.querySelector(".cs-language-note");
    for (const l of languages) select.append(new Option(l.label, l.prefix));
    // A language not offered here stays saved; the list shows English until one is picked.
    select.value = (current || DEFAULT).prefix;
    select.addEventListener("change", () => {
      const language = languages.find(l => l.prefix === select.value);
      failed = null;
      pending = pending.then(() => update(() => language)).then(() => {
        note.textContent = `${language.label} is saved. It applies the next time you press Play.`;
      }).catch(error => {
        failed = error;
        console.warn("[language]", error);
        note.textContent = "The language could not be saved. Reload the page to try again.";
      });
    });
    host.append(section);

    // saves.js replaces registry.json when it restores a backup. No choice may be
    // written meanwhile (it would put the replaced settings back), and the list
    // then shows the restored language. home-settings.js has these hooks too, so
    // each add-on calls the one set before it.
    const before = page.beforeProfileRestore, after = page.afterProfileRestore;
    page.beforeProfileRestore = async () => {
      await before?.();
      select.disabled = true;
      await pending;
    };
    page.afterProfileRestore = async () => {
      try { await after?.(); }
      finally {
        failed = null; // the restore replaced the choice that could not be saved
        note.textContent = NOTE;
        try { select.value = (choose(await readRegistry(dir), languages) || DEFAULT).prefix; }
        catch (error) { console.warn("[language] the restored language could not be read:", error); }
        select.disabled = false;
      }
    };
  })().catch(error => console.warn("[language]", error));
})();
