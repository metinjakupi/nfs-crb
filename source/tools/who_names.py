#!/usr/bin/env python3
"""Who names an address? Diagnostic for a block the translator insists is code.

    python3 tools/who_names.py 10002a70 rebased/msvcr71.dll a_msvcr71

Prints every place the DLL stores that address as a dword (with the bytes
around it and the section it sits in), the generated code that dispatches to
it, and the Ghidra listings whose instructions mention it.
"""
import glob
import os
import struct
import sys
from pathlib import Path

addr = int(sys.argv[1], 16)
dll = Path(sys.argv[2])
key = sys.argv[3]
data = dll.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt_size + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, va, vsize, raw, rsize))


def where(off):
    for name, va, vsize, raw, rsize in secs:
        if raw <= off < raw + rsize:
            return name, base + va + off - raw
    return "?", None


print("== dword %08x stored in %s" % (addr, dll))
needle = struct.pack("<I", addr)
start = 0
while True:
    off = data.find(needle, start)
    if off < 0:
        break
    name, va = where(off)
    before = data[max(0, off - 8):off].hex(" ")
    after = data[off + 4:off + 12].hex(" ")
    print("  file %06x  va %s  %-6s  [%s] %s [%s]" % (off, "%08x" % va if va else "?", name,
                                                      before, needle.hex(" "), after))
    start = off + 1

print("\n== generated code mentioning it")
gens = sorted(glob.glob("build/recomp/gen.new.*"), key=os.path.getmtime)
gens = [g for g in gens if os.path.isdir(os.path.join(g, "aux-" + key))] or gens
if not gens:
    print("  no build/recomp/gen.new.* directory")
else:
    root = os.path.join(gens[-1], "aux-" + key)
    print("  in", root)
    hits = 0
    for path in sorted(glob.glob(os.path.join(root, "**", "*"), recursive=True)):
        if not os.path.isfile(path) or os.path.getsize(path) > 50_000_000:
            continue
        with open(path, errors="replace") as f:
            fn = None
            for n, line in enumerate(f, 1):
                if line.startswith(("void fn_", "static void fn_", "RECOMP_FN")) or "void fn_" in line[:40]:
                    fn = line.strip()[:60]
                if ("%x" % addr) in line.lower():
                    print("  %s:%d  [%s]  %s" % (os.path.basename(path), n, fn, line.strip()[:160]))
                    hits += 1
                    if hits >= 40:
                        break
        if hits >= 40:
            print("  (stopped at 40)")
            break

print("\n== Ghidra listings mentioning it")
listing = Path("analysis/decompiled") / dll.name / "functions"
hits = 0
for path in sorted(listing.glob("*.asm")):
    for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if ("%x" % addr) in line.lower():
            print("  %s:%d  %s" % (path.name, n, line.strip()[:160]))
            hits += 1
tsv = Path("analysis/decompiled") / dll.name / "functions.tsv"
if tsv.is_file():
    rows = tsv.read_text().splitlines()
    near = [r for r in rows[1:] if abs(int(r.split("\t")[0], 16) - addr) < 0x100]
    print("\n== listed functions within 0x100 of it")
    for r in near:
        print("  " + r[:160])
