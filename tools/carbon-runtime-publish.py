from pathlib import Path
import importlib.util, tempfile, time
root=Path(__file__).resolve().parents[1]/'source'
spec=importlib.util.spec_from_file_location('web_launcher',root/'kit/tools/web_launcher.py')
w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
import sys
source=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'build/web/recomp'
wasm=source/'CarbonRecomp.wasm'
assert wasm.is_file(),'Full game wasm is missing'
with wasm.open('rb') as f: assert f.read(8)==b'\0asm\x01\0\0\0','Invalid wasm header'
assert '@APP@' not in (source/'index.html').read_text(),'Player template is unresolved'
state=root.parent/'logs/web-game-exit-code.txt'
assert state.read_text().strip()=='0','Full game build has not passed'
site=root/'build/web-stub-site'
staging=Path(tempfile.mkdtemp(prefix='runtime-publish-',dir=site))
w.copy_web_build('nfsc',source,staging)
for f in (staging/'nfsc').iterdir():
    assert f.stat().st_size==(source/f.name).stat().st_size, f.name
# Stamp the exact publish copy; preserve the original build output for reproducibility.
import subprocess
subprocess.run([sys.executable,str(root.parent/'tools/carbon-build-info.py'),
                str(staging/'nfsc/index.html'),str(staging/'nfsc')],check=True)
current=site/'nfsc'
backup=site/('nfsc.previous-'+str(time.time_ns()))
if current.exists(): current.rename(backup)
try: (staging/'nfsc').rename(current)
except BaseException:
    if backup.exists(): backup.rename(current)
    raise
staging.rmdir()
# Keep only the newest three backups: each is ~125 MB, and a day of publishing
# 39 of them filled the disk (2026-10-24).
for old in sorted(site.glob('nfsc.previous-*'))[:-3]:
    import shutil; shutil.rmtree(old, ignore_errors=True)
print('Full game build is available at the existing private launcher')
print('Wasm bytes:',wasm.stat().st_size)
