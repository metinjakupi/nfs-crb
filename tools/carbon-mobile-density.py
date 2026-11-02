"""Cap SDL's display-density query in an isolated mobile runtime, without a rebuild."""
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
runtime = source.read_text()
original = 'function _emscripten_get_device_pixel_ratio(){if(ENVIRONMENT_IS_PTHREAD)return proxyToMainThread(7,0,1);return devicePixelRatio}'
replacement = 'function _emscripten_get_device_pixel_ratio(){return 1}'
if runtime.count(original) != 1:
    raise SystemExit('Unexpected runtime density function; review the new runtime before patching.')
if source.resolve() == destination.resolve():
    raise SystemExit('Keep the original runtime intact; select a separate output file.')
destination.write_text(runtime.replace(original, replacement, 1))
