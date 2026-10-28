#!/usr/bin/env python3
"""The setup kit: turn your own installed copy of Need for Speed: Carbon (PC, v1.4) into the native Mac app.
Run through ./setup.sh, which prepares the Python environment first.

    ./setup.sh "/path/to/Need for Speed - Carbon" [--out DIR] [--button-icons] [--hd-ui] [--check-only]

Steps: check the game folder, NFSC.exe and its runtime DLLs -> link the game (nothing in it is changed) ->
relocate the runtime DLLs -> analyse NFSC.exe once (Ghidra and a Java runtime, pinned hashes) -> translate and
build -> controller button icons (optional download) -> package "Need for Speed Carbon.app". Everything else
stays on this Mac. The Setup app (tools/installer/) runs this and reads its "n/7" step lines."""
import argparse
import hashlib
import platform
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

ROOT = Path(__file__).resolve().parents[2]
LOG = ROOT / "build/setup.log"
APP_NAME = "Need for Speed Carbon"
# The runtime DLLs as the game installs them: game.toml's addresses in them (data_ranges, entry_points) are for
# these exact files, relocated by tools/rebase_pe.py.
DLLS = {
    "msvcr71.dll": "8094af5ee310714caebccaeee7769ffb08048503ba478b879edfef5f1a24fefe",
    "msvcp71.dll": "df96156f6a548fd6fe5672918de5ae4509d3c810a57bffd2a91de45a3ed5b23b",
}
PY = sys.executable
# The environment's own tools (cmake, ninja from kit/requirements-dev.txt) come first on PATH for every step.
ENV = None


def say(msg):
    print(msg, flush=True)
    with LOG.open("a") as f:
        f.write(msg + "\n")


def fail(msg):
    say("\nSETUP STOPPED: " + msg)
    sys.exit(1)


def run(args, **kw):
    say("  $ " + " ".join(str(a) for a in args))
    with LOG.open("a") as log:
        kw.setdefault("cwd", ROOT)
        kw.setdefault("env", ENV)
        r = subprocess.run([str(a) for a in args], stdout=log, stderr=subprocess.STDOUT, **kw)
    if r.returncode:
        fail(f"step failed (exit {r.returncode}); the last lines of {LOG} say why")


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find(game, name):
    """game/name without regard to case (Windows game folders mix cases)."""
    return next((p for p in game.iterdir() if p.name.lower() == name.lower()), None)


def check_game(game, cfg):
    if not game.is_dir():
        fail(f"{game} is not a folder")
    exe = find(game, "NFSC.exe")
    if exe is None:
        fail(f"no NFSC.exe in {game}; choose the installed game folder (the one with NFSC.exe, GLOBAL, TRACKS ...)")
    names = {p.name.upper() for p in game.iterdir() if p.is_dir()}
    missing = [d for d in cfg["setup"]["required_dirs"] if d not in names]
    if missing:
        fail(f"the game folder is incomplete: {', '.join(missing)} missing")
    if sha(exe) != cfg["game"]["sha256"]:
        fail("This NFSC.exe can't be used. The port is built for the PC game, version 1.4, with an unwrapped "
             "(no-DVD) NFSC.exe. Nothing was changed.")
    say("  NFSC.exe: OK")
    for name, digest in DLLS.items():
        p = find(game, name)
        if p is None:
            fail(f"{name} is missing from the game folder (the game installs it beside NFSC.exe)")
        if sha(p) != digest:
            fail(f"This {name} is a different version from the one the game installs; put the game's own copy "
                 "back beside NFSC.exe. Nothing was changed.")
    say("  msvcr71.dll, msvcp71.dll: OK")


def link_game(game):
    """original -> the game folder. The game folder itself is never written to."""
    dest = ROOT / "original"
    if dest.is_symlink():
        dest.unlink()
    elif dest.exists():
        fail(f"{dest} is a real folder, not a link; move it aside")
    dest.symlink_to(game, target_is_directory=True)
    say(f"  linked {game}")


def analysis_done():
    out = ROOT / "analysis/decompiled"
    return all((out / name / "functions.tsv").is_file() for name in ("NFSC.exe", *DLLS))


def main():
    global ENV
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path, help="your installed game folder (holds NFSC.exe, GLOBAL, TRACKS, ...)")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="where to put the app (default dist/)")
    ap.add_argument("--hd-ui", action="store_true",
                    help="download and install the NFS Carbon Remastered UI HD textures (about 52 MB)")
    ap.add_argument("--button-icons", action="store_true",
                    help="download XtendedInput's controller button icons for the menus (about 1.5 MB)")
    ap.add_argument("--check-only", action="store_true", help="only check the game folder")
    ap.add_argument("--ghidra-home", type=Path, help="use this Ghidra 12.1.3 instead of downloading one")
    ap.add_argument("--java-home", type=Path, help="use this Java runtime (21 or later) instead of downloading one")
    args = ap.parse_args()
    if platform.machine() != "arm64":
        fail("this Python runs under Rosetta (Intel). Run ./setup.sh, which picks a native Apple Silicon Python.")
    import os
    ENV = dict(os.environ, PATH=str(Path(PY).parent) + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"))
    (ROOT / "build").mkdir(exist_ok=True)
    LOG.write_text("")
    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    game = args.game.expanduser().resolve()

    say("1/7 Checking your game folder")
    check_game(game, cfg)
    if args.check_only:
        say("Check passed. Nothing was changed.")
        return

    say("2/7 Linking the game")
    link_game(game)

    say("3/7 Preparing the game's runtime DLLs")
    run([PY, "tools/rebase_pe.py", game])

    say("4/7 Analysing NFSC.exe (once; can take a while)")
    if analysis_done():
        say("  already analysed; reused")
    else:
        sys.path.insert(0, str(ROOT / "tools"))
        import fetch_tools
        java = args.java_home or fetch_tools.fetch("jdk")
        ghidra = args.ghidra_home or fetch_tools.fetch("ghidra")
        fetch_tools.build_decompiler(ghidra, java)
        run([PY, "tools/analyze.py", "--ghidra-home", ghidra, "--java-home", java])

    say("5/7 Translating and building")
    run([PY, "tools/build.py", "--regenerate"])
    if not (ROOT / "build/CarbonRecomp.app/Contents/MacOS/CarbonRecomp").is_file():
        fail("the build finished without build/CarbonRecomp.app")

    say("6/7 Optional downloads")
    say("  controller button icons: " + ("downloading XtendedInput's icons while packaging" if args.button_icons
        else "not added (menus use keyboard-style prompts); choose them in the Setup app or pass --button-icons"))
    say("  HD UI: " + ("downloading and installing Remastered UI while packaging" if args.hd_ui
        else "not added; choose it in the Setup app or pass --hd-ui"))

    say("7/7 Packaging")
    out = args.out.expanduser().resolve()
    package = [PY, "tools/setup_kit/package.py", "--game", game, "--out", out]
    if args.button_icons:
        package.append("--button-icons")
    if args.hd_ui:
        package.append("--hd-ui")
    run(package)
    say(f"\nDone: {out / (APP_NAME + '.app')}\nYour saves and settings will be in "
        "~/Library/Application Support/NFS Carbon/.")


if __name__ == "__main__":
    main()
