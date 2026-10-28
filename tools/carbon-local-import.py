from pathlib import Path
import json, os
root=Path(r'C:\Users\Metin\projects\carbon-browser\source')
worker=root/'kit/web/launcher/worker.js'
s=worker.read_text()
needle='async function freeBytes() {'
assert needle in s and 'async function installedSource' not in s
s=s.replace(needle,'''// A private development server can expose an installed copy beside the launcher.
async function installedSource(manifest, signal) {
  const url = new URL(manifest, self.location.href);
  if (url.origin !== self.location.origin) throw new Error("Installed game must use this server");
  const response = await fetch(url, { signal });
  if (!response.ok) throw new Error(`Game manifest: HTTP ${response.status}`);
  const { files, base } = await response.json();
  const prefix = new URL(base, url);
  if (prefix.origin !== url.origin) throw new Error("Invalid installed game location");
  const entries = files.map(e => {
    if (!e.path || e.path.split("/").some(p => !p || p === "." || p === "..") || e.path.includes("\\\\"))
      throw new Error("Invalid installed game path");
    return { ...e, isDir: false };
  });
  return { list: async () => entries, stream: async e => {
    const file = new URL(e.path.split("/").map(encodeURIComponent).join("/"), prefix);
    const response = await fetch(file, { signal });
    if (!response.ok) throw new Error(`${e.path}: HTTP ${response.status}`);
    return response.body;
  }};
}

'''+needle)
s=s.replace('else if (kind === "handle") source = await handleSource(handle);','else if (kind === "handle") source = await handleSource(handle);\n    else if (kind === "installed") source = await installedSource(game.localManifest, controller.signal);')
worker.write_text(s)
app=root/'kit/web/launcher/app.js';s=app.read_text()
s=s.replace('.import-folder, .import-zip, .import-saves, .delete, .play','.import-folder, .import-local, .import-zip, .import-saves, .delete, .play')
needle='  el.querySelector(".import-folder").onclick = async () => {'
assert needle in s
s=s.replace(needle,'''  const local = el.querySelector(".import-local");
  local.hidden = !game.localManifest;
  local.onclick = () => runImport(card, { kind: "installed" });
'''+needle)
app.write_text(s)
page=root/'kit/web/launcher/index.html';s=page.read_text();needle='<button class="import-folder">';assert needle in s
s=s.replace(needle,'<button class="import-local" hidden>Import Windows installation</button>\n      '+needle);page.write_text(s)
print('Installed-copy import added')
