from pathlib import Path
root=Path(r'C:\Users\Metin\projects\carbon-browser\source')
p=root/'kit/web/player/index.html';s=p.read_text()
anchor='async function gameId() {'
assert s.count(anchor)==1
extra='''// Temporary loading diagnostics: bounded to the first ten minutes of a run.
let loadingReports = 0;
const loadingTimer = setInterval(() => {
  if (reportingFailure || ++loadingReports > 20) { clearInterval(loadingTimer); return; }
  const record = {
    kind: "loading-snapshot", game: location.pathname, time: new Date().toISOString(),
    assets: window.carbonTestAssets || {},
    log: log.slice(-35).map(t => String(t).slice(0, 200)),
    warnings: log.filter(t => /missing|not found|failed|unimplemented|unknown|error/i.test(String(t)))
      .slice(-8).map(t => String(t).slice(0, 200)),
  };
  let body = JSON.stringify(record);
  while (new TextEncoder().encode(body).length > 14000 && record.log.length) {
    record.log.shift(); body = JSON.stringify(record);
  }
  fetch("/_runtime-diagnostics", {
    method: "POST", headers: { "Content-Type": "application/json" }, body,
  }).catch(() => {});
}, 30000);

'''
assert 'const loadingTimer' not in s
s=s.replace(anchor,extra+anchor)
anchor='  const adapter = await navigator.gpu.requestAdapter'
a=s.index(anchor)
s=s[:a]+'''  window.carbonTestAssets = { NIS: false };
  try { await dir.getDirectoryHandle("NIS"); window.carbonTestAssets.NIS = true; }
  catch { console.warn("[browser] NIS scene data is missing; resume Import Windows installation."); }

'''+s[a:]
p.write_text(s)
html=s.replace('@APP@','CarbonRecomp').replace('@TITLE@','Need for Speed: Carbon')
for out in ['build/web/recomp','build/web-site/nfsc','build/web-stub-site/nfsc']:
    (root/out/'index.html').write_text(html)
(root.parent/'tools/carbon-player-syntax.js').write_text(s.split('<script>',1)[1].split('</script>',1)[0])
print('Loading snapshots added; compiled game code unchanged')
