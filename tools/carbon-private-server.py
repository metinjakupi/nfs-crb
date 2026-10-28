from pathlib import Path
import importlib.util
root=Path(__file__).resolve().parents[1]/'source'
spec=importlib.util.spec_from_file_location('web_launcher',root/'kit/tools/web_launcher.py')
w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
import sys
w.serve(root/'build/web-stub-site',int(sys.argv[1]) if len(sys.argv)>1 else 8096)
