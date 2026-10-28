from pathlib import Path
import importlib.util, json, os, shutil, fnmatch
root=Path(__file__).resolve().parents[1]/'source'
site=root/'build/web-stub-site'
spec=importlib.util.spec_from_file_location('web_launcher',root/'kit/tools/web_launcher.py')
w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
games=w.build([root],site)
dest=site/'local-game/files';dest.mkdir(parents=True,exist_ok=True)
original=root/'original'
entries=[]
for src in original.rglob('*'):
    if not src.is_file(): continue
    rel=src.relative_to(original)
    # Match the launcher's configured exclusions; do not omit game-data folders.
    patterns=games[0].get('exclude',[])
    if any(fnmatch.fnmatchcase(rel.parts[0].lower(),pat.lower()) or
           fnmatch.fnmatchcase(rel.name.lower(),pat.lower()) for pat in patterns):continue
    if len(rel.parts)==1 and src.suffix.lower()=='.exe' and rel.name.lower()!='nfsc.exe':continue
    target=dest/rel;target.parent.mkdir(parents=True,exist_ok=True)
    if not target.exists():os.link(src,target)
    st=src.stat();entries.append(dict(path=rel.as_posix(),size=st.st_size,mtime=int(st.st_mtime)))
for name in ('msvcr71.dll','msvcp71.dll'):
    src=root/'rebased'/name;target=dest/name
    shutil.copy2(src,target)
    st=src.stat();entries.append(dict(path=name,size=st.st_size,mtime=int(st.st_mtime)))
(site/'local-game/manifest.json').write_text(json.dumps(dict(base='files/',files=entries)))
games[0]['localManifest']='local-game/manifest.json'
(site/'games.json').write_text(json.dumps(games,indent=2))
print('Prepared',len(entries),'files,',sum(e['size'] for e in entries),'bytes; assets use hard links')
