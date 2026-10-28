from pathlib import Path
root=Path(r'C:\Users\Metin\projects\carbon-browser\source')
p=root/'kit/web/player/index.html';s=p.read_text()
a='    assets: window.carbonTestAssets || {},'
b=a+'''
    graphics: window.carbonGraphicsInfo || {},
    canvas: { width: canvas.width, height: canvas.height, dpr: devicePixelRatio },
    rendering: log.filter(t => /d3d9 webgpu:|d3d9 host:|audio sink:|sched: baton held/.test(String(t)))
      .slice(-10).map(t => String(t).slice(0, 500)),'''
assert s.count(a)==1 and 'window.carbonGraphicsInfo' not in s;s=s.replace(a,b)
a='  if (!adapter) throw new Error("WebGPU gave no adapter.");'
b=a+'''
  const info = adapter.info || {};
  window.carbonGraphicsInfo = {
    vendor: info.vendor || "", architecture: info.architecture || "",
    device: info.device || "", description: info.description || "",
    fallback: adapter.isFallbackAdapter ?? info.isFallbackAdapter ?? null,
  };'''
assert s.count(a)==1;s=s.replace(a,b)
p.write_text(s)
html=s.replace('@APP@','CarbonRecomp').replace('@TITLE@','Need for Speed: Carbon')
for out in ['build/web/recomp','build/web-site/nfsc','build/web-stub-site/nfsc']:
    (root/out/'index.html').write_text(html)
(root.parent/'tools/carbon-player-syntax.js').write_text(s.split('<script>',1)[1].split('</script>',1)[0])
print('GPU adapter, render-size and renderer-stat snapshots added')
