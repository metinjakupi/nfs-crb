"""Generate a separate diagnostic player using the current public runtime URLs."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
template = (root / 'source/kit/web/player/index.html').read_text()
diagnostics = (root / 'tools/site/mobile-beta.js').read_text()
menu = (root / 'tools/site/mobile-menu.js').read_text()
page = template.replace('@APP@', 'CarbonRecomp').replace('@TITLE@', 'Carbon mobile beta')
page = page.replace('<html lang="en">', '<html lang="en" data-game-id="nfsc">')
page = page.replace('<head>', '<head>\n<base href="/play/nfsc/">\n<meta name="robots" content="noindex">')
page = page.replace('initial-scale=1"', 'initial-scale=1, viewport-fit=cover"')
page = page.replace('<script>\n"use strict";', '<script>\n' + diagnostics + '\n</script>\n<script>\n"use strict";', 1)
page = page.replace('<script>\n"use strict";', '<script>\n' + menu + '\n</script>\n<script>\n"use strict";', 1)
page = page.replace('      gameRunning = true;', '      gameRunning = true;\n      window.carbonMobileMenuReady?.();', 1)
page = page.replace('      for (const [k, v] of extra) if (k.startsWith("RECOMP_")) m.ENV[k] = v;', '      for (const [k, v] of extra) if (k.startsWith("RECOMP_")) m.ENV[k] = v;\n      // Mobile beta: avoid automatic high-density render-target upscaling.\n      m.ENV.RECOMP_D3D9_SCALE = "1";', 1)
page = page.replace('  startupPhase = phase;', '  window.carbonMobileStage?.(phase);\n  startupPhase = phase;', 1)
page = page.replace('new URL("CarbonRecomp.wasm", document.baseURI)', 'new URL("/mobile-beta/runtime/CarbonRecomp.wasm", location.origin)')
page = page.replace('script.src = "CarbonRecomp.js";', 'script.src = "/mobile-beta/runtime/CarbonRecomp-mobile-density.js";')
page = page.replace('    canvas,\n', '    canvas,\n    locateFile: name => "/mobile-beta/runtime/" + name,\n', 1)
page = page.replace('      loadRuntime(imports)', '      window.carbonMobileRuntime?.(imports);\n      loadRuntime(imports)', 1)
page = page.replace('log.push(t);', 'window.carbonMobileLog?.(t); log.push(t);')
page = page.replace('(async () => {\n  if (canonicalRedirect) return;', '(async () => {\n  if (canonicalRedirect || new URLSearchParams(location.search).has("results")) return;')
Path(sys.argv[1]).write_text(page)
