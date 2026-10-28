#!/usr/bin/env python3
"""Embed content-based release identities in the served page, runtime and reporter.

Run on publish copies, after minification and before compression. Existing tabs
retain their loaded identities; reporting never fetches a moving 'latest' value.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

MARK = '// carbon-build-info '
START = '<!-- carbon-build-info -->'
END = '<!-- /carbon-build-info -->'

def clean_js(data):
    if data.startswith(MARK.encode()):
        return data.split(b'\n', 2)[2]
    return data

def clean_page(data):
    return re.sub(rb'<!-- carbon-build-info -->.*?<!-- /carbon-build-info -->\n?', b'', data, flags=re.S)

def stamp(page, runtime):
    page_data = clean_page(page.read_bytes())
    inputs = {'page.html': page_data}
    for p in sorted(runtime.iterdir()):
        if p.is_file() and (p.suffix == '.js' or p.name in ('CarbonRecomp.wasm', 'CarbonRecomp.data')):
            inputs[p.name] = clean_js(p.read_bytes()) if p.suffix == '.js' else p.read_bytes()
    assert 'CarbonRecomp.js' in inputs and 'CarbonRecomp.wasm' in inputs and 'report.js' in inputs
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in inputs.items()}
    build = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    info = {'id': build, 'wasm': hashes['CarbonRecomp.wasm']}
    encoded = json.dumps(info, separators=(',', ':'))
    page.write_bytes(page_data.replace(b'<head>', ('<head>\n' + START + '\n<script>window.recompPageBuild = Object.freeze(' + encoded + ');</script>\n' + END).encode(), 1))
    for name, variable in [('CarbonRecomp.js', 'recompRuntimeBuild'), ('report.js', 'recompReportBuild')]:
        # Keep an existing strict-mode directive effective after inserting the stamp.
        lead = re.sub(rb'\A(?:\s+|//[^\n]*(?:\n|$)|/\*.*?\*/)*', b'', inputs[name], flags=re.S)
        strict = '"use strict";' if lead.startswith((b'"use strict"', b"'use strict'")) else ''
        prefix = f'{MARK}{build}\n{strict}globalThis.{variable} = Object.freeze({encoded});\n'
        (runtime/name).write_bytes(prefix.encode() + inputs[name])
    print(f'Embedded build {build}; wasm SHA-256 {info["wasm"]}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--strip', action='store_true', help='Remove publish stamps before minification')
    parser.add_argument('page', type=Path)
    parser.add_argument('runtime', type=Path)
    args = parser.parse_args()
    if args.strip:
        args.page.write_bytes(clean_page(args.page.read_bytes()))
        for name in ('CarbonRecomp.js', 'report.js'):
            path = args.runtime / name
            path.write_bytes(clean_js(path.read_bytes()))
    else:
        stamp(args.page, args.runtime)
