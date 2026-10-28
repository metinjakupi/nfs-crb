#!/usr/bin/env python3
"""Find byte patterns (ThirteenAG/hooking.Patterns style, "?" = any byte) in NFSC.exe's code.

    python tools/find_pattern.py "D8 0D ? ? ? ? D8 25"            # every match, with its instructions
    python tools/find_pattern.py --at 2 "D8 05 ? ? ? ? 89 44 24 18" # also the dword at match+2
"""
import argparse, re, struct, sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parents[1]

def load(exe):
    d = exe.read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    secs = [struct.unpack_from("<8sIIII", d, pe + 24 + opt + 40 * i) for i in range(n)]
    return d, base, secs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("--at", type=int, help="print the u32 at match + AT (a pointer the pattern names)")
    ap.add_argument("--exe", type=Path, default=ROOT / "original/NFSC.exe")
    a = ap.parse_args()
    d, base, secs = load(a.exe)
    rx = re.compile(b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in a.pattern.split()), re.S)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for name, vs, va, rs, ro in secs:
        if not name.startswith(b".text"):
            continue
        for m in rx.finditer(d, ro, ro + rs):
            addr = base + va + m.start() - ro
            line = f"{addr:08x}"
            if a.at is not None:
                ptr = struct.unpack_from("<I", d, m.start() + a.at)[0]
                line += f"  [+{a.at}] = {ptr:08x}"
            print(line)
            for ins in md.disasm(d[m.start():m.end() + 8], addr):
                if ins.address >= addr + (m.end() - m.start()):
                    break
                print(f"    {ins.address:08x} {ins.mnemonic} {ins.op_str}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
