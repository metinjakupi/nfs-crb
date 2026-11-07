// language.js - the start page's game language choice.
//
// Carbon reads the registry value Language (REG_SZ) under KEY once at startup and
// loads LANGUAGES\<PREFIX>_FRONTEND.BIN; no value means English US. The table is
// the one in source/tools/setup_kit/package.py.
"use strict";
(() => {
  const KEY = "HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Need for Speed Carbon";
  const LANGUAGES = [
    { prefix: "ENGLISH", registry: "English US", label: "English" },
    { prefix: "FRENCH", registry: "French", label: "Français" },
  ];
  const DEFAULT = LANGUAGES[0];

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
  function saved(registry) {
    const values = registry?.[carbonKey(registry)];
    const value = values?.[valueName(values)];
    return value && typeof value.data === "string" ? value.data : undefined;
  }
  function known(name) {
    if (name === undefined) return DEFAULT;
    return LANGUAGES.find(l => l.registry.toLowerCase() === name.toLowerCase());
  }

  function choose(registry, languages) {
    const name = saved(registry), current = known(name);
    if (!current) return null;
    if (!languages.length || languages.includes(current)) return current;
    return languages.includes(DEFAULT) ? DEFAULT : languages[0];
  }

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

  const page = window.recompStartPage, game = window.recompGame;
  const host = page?.panel("language");
  if (!host || !game) return;

  async function gameFiles() {
    if (game.streamAssets && game.downloadManifest) {
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
  function profileDir() {
    profile ??= (async () => {
      const folder = await page.prepareProfile();
      const root = await (await navigator.storage.getDirectory()).getDirectoryHandle(game.id);
      return root.getDirectoryHandle(folder, { create: true });
    })().catch(error => { profile = null; throw error; });
    return profile;
  }
  async function folderIn(dir, name) {
    try { return await dir.getDirectoryHandle(name); }
    catch (e) { if (e.name === "NotFoundError") return null; throw e; }
  }
  async function readRegistry(dir) {
    let file;
    try { file = await dir.getFileHandle("registry.json"); }
    catch (e) { if (e.name === "NotFoundError") return null; throw e; }
    const value = JSON.parse(await (await file.getFile()).text());
    if (!value || Array.isArray(value) || typeof value !== "object") throw Error("registry.json is not an object");
    return value;
  }
  // A window's own folder (slot > 1) is copied from the main save at its first Play
  // (player page, profileFolder); until then, show the main save without copying it.
  async function shownRegistry() {
    if (!profile && page.slot > 1) {
      const root = await folderIn(await navigator.storage.getDirectory(), game.id);
      const own = root && await folderIn(root, page.profileFolder);
      let copied = false;
      if (own) {
        try { await own.getFileHandle(".copied"); copied = true; }
        catch (e) { if (e.name !== "NotFoundError") throw e; }
      }
      if (!copied) {
        const main = root && await folderIn(root, "profile");
        return main ? readRegistry(main) : null;
      }
    }
    return readRegistry(await profileDir());
  }
  async function update(change) {
    const dir = await profileDir();
    const registry = await readRegistry(dir);
    const language = change(registry);
    const next = language && withLanguage(registry, language);
    if (!next) return;
    const writable = await (await dir.getFileHandle("registry.json", { create: true })).createWritable();
    try { await writable.write(JSON.stringify(next, null, 2) + "\n"); await writable.close(); }
    catch (e) { await writable.abort().catch(() => {}); throw e; }
  }

  let pending = Promise.resolve(), failed = null;
  page.prepareLanguage = async () => {
    await pending;
    if (failed) throw failed;
    const languages = await detected;
    pending = pending.then(() => update(registry => choose(registry, languages))).catch(error => {
      console.warn("[language] the saved language could not be checked:", error);
    });
    await pending;
  };

  const NOTE = "The game's own translation, from your game files. Applies the next time you press Play.";
  (async () => {
    const languages = await detected;
    if (languages.length < 2) return;
    const current = choose(await shownRegistry(), languages);
    const section = document.createElement("div");
    section.innerHTML = `<label class="cs-row" style="margin:14px 0 6px"><span class="cs-label">Game language</span>
      <select class="cs-language"></select></label>
      <div class="cs-note cs-language-note" role="status" aria-live="polite">${NOTE}</div>`;
    const select = section.querySelector("select"), note = section.querySelector(".cs-language-note");
    for (const l of languages) select.append(new Option(l.label, l.prefix));
    let restoring = false;
    const lock = () => { select.disabled = restoring || !!page.playStarting?.(); };
    addEventListener("recomp-play-state", lock);
    lock();
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

    const before = page.beforeProfileRestore, after = page.afterProfileRestore;
    page.beforeProfileRestore = async () => {
      await before?.();
      restoring = true; lock();
      await pending;
    };
    page.afterProfileRestore = async () => {
      try { await after?.(); }
      finally {
        failed = null; // the restore replaced the choice that could not be saved
        note.textContent = NOTE;
        try { select.value = (choose(await shownRegistry(), languages) || DEFAULT).prefix; }
        catch (error) { console.warn("[language] the restored language could not be read:", error); }
        restoring = false; lock();
      }
    };
  })().catch(error => console.warn("[language]", error));
})();
