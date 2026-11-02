#!/usr/bin/env python3
"""Build Need for Speed: Carbon with the kit in kit/. Every option is the kit's: see kit/tools/build.py --help."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
if not (KIT / "tools/build.py").is_file():
    sys.exit("The kit folder is missing: clone the whole repository again")
# The recovery resource is generated locally; original game bytes stay out of Git.
font_header = ROOT / "mods/core/nfsc/customization/candidate/missing-title-font.h"
if not font_header.exists() and not any(arg in ("-h", "--help") for arg in sys.argv[1:]):
    for game in (ROOT / "original", ROOT.parent / "game"):
        if (game / "GLOBAL/GlobalMemoryFile.bin").is_file():
            subprocess.run([sys.executable, str(ROOT / "tools/setup_kit/title_font.py"), str(game)], check=True)
            break
sys.exit(subprocess.call([sys.executable, str(KIT / "tools/build.py"), "--game-dir", str(ROOT)] + sys.argv[1:],
                         cwd=ROOT))
