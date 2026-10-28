// Pre-game controls use the same numeric mod-settings.json keys as the F9 menu.
// Each write merges the latest file so unrelated preferences and careers stay intact.
"use strict";
(() => {
  const begin = () => build(window.recompStartPage).catch(error => {
    console.warn("[home settings]", error);
    const body = document.querySelector(".cp-body");
    if (body) body.textContent = "Your settings could not be loaded. Reload to try again.";
  });
  if (window.recompStartPage) begin();
  else addEventListener("recomp-start-page", begin, { once: true });

  async function build(page) {
    const host = page?.panel("preferences");
    if (!host || host.childElementCount) return;
    const section = document.createElement("section");
    section.className = "cs-panel cp";
    section.innerHTML = `<style>
      .cp h3 { color:var(--cyan); font-size:15px; margin:26px 0 10px; }
      .cp .cp-row { display:grid; grid-template-columns:minmax(0,1fr) minmax(110px,45%); gap:14px; align-items:center; padding:12px 0; border-bottom:1px solid var(--line); font-size:14px; }
      .cp .cp-row select,.cp .cp-row input[type=number] { width:100%; min-width:0; padding:8px; font:inherit; color:var(--ink); background:#070d16; border:1px solid var(--line); }
      .cp .cp-row input:focus-visible,.cp .cp-row select:focus-visible { outline:2px solid var(--cyan); outline-offset:2px; }
      .cp .cs-switch { margin:0; padding:12px 0; border-bottom:1px solid var(--line); font-size:14px; }
      .cp .cp-status { min-height:20px; margin-top:12px; }
      .cp .cp-status.bad { color:#ff8da1; }
      .cp button { margin-top:14px; }
      @media(max-width:480px) { .cp .cp-row { grid-template-columns:minmax(0,1fr) minmax(100px,46%); gap:10px; } }
    </style><h2>Settings</h2><p class="cs-note">Changes apply the next time you press Play.</p>
    <div class="cp-body">Loading settings…</div><div class="cs-note cp-status" role="status" aria-live="polite"></div>
    <button class="cs-btn cp-reset" type="button" disabled><span>Reset settings</span></button>`;
    host.append(section);
    const status = section.querySelector(".cp-status");
    const body = section.querySelector(".cp-body");
    let pending = Promise.resolve(), failed = null, restoring = false;
    const inputs = new Map();
    const schema = await fetch("settings.json", { cache: "no-cache" }).then(r => {
      if (!r.ok) throw Error("Settings are unavailable");
      return r.json();
    });
    const folder = await page.prepareProfile();
    const game = window.recompGame.id;
    const profile = await (await (await navigator.storage.getDirectory()).getDirectoryHandle(game)).getDirectoryHandle(folder, { create: true });
    async function read() {
      let file;
      try { file = await profile.getFileHandle("mod-settings.json"); }
      catch (e) { if (e.name === "NotFoundError") return {}; throw e; }
      const value = JSON.parse(await (await file.getFile()).text());
      if (!value || Array.isArray(value) || typeof value !== "object") throw Error("The saved settings could not be read");
      return value;
    }
    const rows = schema.settings;
    let values = await read();
    function shown(row) {
      const value = values[row.id];
      return Number.isSafeInteger(value) ? Math.max(row.min, Math.min(row.max, value)) : row.default;
    }
    function renderValues() {
      for (const row of rows) {
        const input = inputs.get(row.id);
        if (row.kind === "bool") input.checked = !!shown(row);
        else input.value = String(shown(row));
        input.disabled = restoring;
      }
      section.querySelector(".cp-reset").disabled = restoring;
    }
    function save(changes) {
      failed = null;
      status.textContent = "Saving…";
      status.classList.remove("bad");
      pending = pending.then(async () => {
        const write = async () => {
          const latest = await read();
          Object.assign(latest, changes);
          const writable = await (await profile.getFileHandle("mod-settings.json", { create: true })).createWritable();
          try { await writable.write(JSON.stringify(latest, null, 2) + "\n"); await writable.close(); }
          catch (e) { await writable.abort().catch(() => {}); throw e; }
          values = latest;
        };
        if (navigator.locks) await navigator.locks.request(`${game}:settings:${folder}`, write);
        else await write();
        status.textContent = "Saved";
      }).catch(error => {
        failed = error;
        status.textContent = "Your change could not be saved. Reload to keep your previous settings.";
        status.classList.add("bad");
      });
    }
    page.flushSettings = async () => { await pending; if (failed) throw failed; };
    page.beforeProfileRestore = async () => {
      restoring = true; renderValues();
      await pending;
    };
    page.afterProfileRestore = async () => {
      try { await pending; values = await read(); failed = null; status.textContent = ""; }
      catch (e) { failed = e; status.textContent = "Your saved settings could not be read. Reload to try again."; }
      finally { restoring = false; renderValues(); }
    };
    body.replaceChildren();
    for (const [group, title] of [["game", "Game"], ["display", "Display & graphics"], ["controls", "Controller"]]) {
      const heading = document.createElement("h3"); heading.textContent = title; body.append(heading);
      for (const row of rows.filter(r => r.group === group)) {
        const label = document.createElement("label"), text = document.createElement("span");
        label.className = row.kind === "bool" ? "cs-switch" : "cp-row";
        text.textContent = row.label;
        let input;
        if (row.choices.length) {
          input = document.createElement("select");
          row.choices.forEach((choice, i) => input.append(new Option(choice, String(row.min + i))));
        } else {
          input = document.createElement("input"); input.type = row.kind === "bool" ? "checkbox" : "number";
          if (row.kind === "bool") input.setAttribute("role", "switch");
          else { input.min = row.min; input.max = row.max; input.step = row.step; input.required = true; }
        }
        input.addEventListener("change", () => {
          if (!input.checkValidity()) { input.reportValidity(); return; }
          const value = row.kind === "bool" ? Number(input.checked) : Number(input.value);
          if (!Number.isSafeInteger(value) || value < row.min || value > row.max) { input.reportValidity(); return; }
          save({ [row.id]: value });
        });
        label.append(text, input); inputs.set(row.id, input); body.append(label);
      }
    }
    section.querySelector(".cp-reset").addEventListener("click", () => {
      const defaults = Object.fromEntries(rows.map(row => [row.id, row.default]));
      Object.assign(values, defaults); renderValues(); save(defaults);
    });
    renderValues();
    installGameFiles(page);
  }

  function installGameFiles(page) {
    const box = document.querySelector(".cs-files")?.closest("section");
    if (!box) return;
    const old = box.querySelector(".cs-manage");
    old?.remove();
    const actions = box.querySelector(".cs-actions");
    const download = document.createElement("button"), remove = document.createElement("button");
    download.className = remove.className = "cs-btn";
    download.type = remove.type = "button";
    download.textContent = "Download all files (5.1 GB)";
    remove.textContent = "Delete downloaded files";
    actions.append(download, remove);
    const note = box.querySelector(".cs-files");
    note.textContent = "Optional: download everything for fewer loading pauses.";
    let active = null, busy = false, confirmed = false;
    const go = document.querySelector(".cs-go");
    function lock(value) { busy = value; page.homeBusy = value; go.disabled = value; remove.disabled = value; }
    async function command(cmd, extra = {}, progress) {
      const worker = new Worker(new URL("../worker.js", document.baseURI), { type: "module" });
      if (cmd === "import") active = worker;
      try {
        return await new Promise((resolve, reject) => {
          worker.onerror = () => reject(Error("The request could not finish"));
          worker.onmessage = ({ data }) => {
            if (data.progress) { progress?.(data.progress); return; }
            if (data.id !== 1) return;
            data.error ? reject(Error(data.error)) : resolve(data.result);
          };
          worker.postMessage({ id: 1, cmd, game: null, ...extra });
        });
      } finally { worker.terminate(); if (active === worker) active = null; }
    }
    async function config() {
      const games = await fetch("../games.json").then(r => r.json());
      return games.find(g => g.id === window.recompGame.id);
    }
    download.addEventListener("click", async () => {
      if (active) { active.postMessage({ id: 2, cmd: "cancel" }); note.textContent = "Stopping download…"; return; }
      if (busy) return;
      lock(true); note.textContent = "Starting download…"; download.textContent = "Cancel download";
      try {
        const result = await command("import", { game: await config(), kind: "download" }, p => {
          note.textContent = `Downloading… ${p.bytesTotal ? Math.floor(p.bytesDone / p.bytesTotal * 100) : 0}%`;
        });
        note.textContent = result.result === "done" ? "All game files are on this device." : result.result === "cancelled" ? "Download paused. You can continue later." : result.result === "noSpace" ? "Not enough space. Free some space and try again." : "The download could not finish. Try again to continue.";
      } catch { note.textContent = "The download could not finish. Check your connection and try again."; }
      finally { lock(false); download.textContent = "Download all files (5.1 GB)"; }
    });
    remove.addEventListener("click", async () => {
      if (busy) return;
      if (!confirmed) {
        confirmed = true; remove.textContent = "Delete files — click again";
        setTimeout(() => { confirmed = false; remove.textContent = "Delete downloaded files"; }, 5000);
        return;
      }
      confirmed = false; lock(true); download.disabled = true;
      try { await command("delete", { game: await config() }); note.textContent = "Downloaded files deleted. Your saves and settings are kept."; }
      catch { note.textContent = "The files could not be deleted. Close other game windows and try again."; }
      finally { lock(false); download.disabled = false; remove.textContent = "Delete downloaded files"; }
    });
  }
})();
