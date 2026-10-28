#!/usr/bin/env python3
"""Package the built game with your game files: "Need for Speed Carbon.app".

    python tools/setup_kit/package.py --game "/path/to/Need for Speed - Carbon"
    python tools/setup_kit/package.py --game DIR --install     # also put it in /Applications

Takes build/CarbonRecomp.app (run tools/build.py first) and adds:
  Contents/MacOS/Launch           tools/setup_kit/launch.c, the app's executable
  Contents/Resources/Game/        your game folder (APFS clones, so almost no extra disk
                                  space), minus Windows-only files, plus the rebased
                                  msvcr71/msvcp71 DLLs the translation was made from
  Contents/Resources/Defaults/    first-run registry.json and mod-settings.json
  Contents/Resources/AppIcon.icns the game's own logo from your GLOBAL files (game_art.py)
  Contents/Resources/Game/GLOBAL/XtendedInputButtons.tpk
                                  only with --button-icons: controller button icons from
                                  XtendedInput's release (xan1242; art by Aero_), downloaded
                                  from GitHub and checked; not part of this repository
Info.plist gets the app's name, the racing-games category and LSSupportsGameMode, so macOS
turns on Game Mode in full screen. Every Mach-O is signed ad hoc, inside out.

Nothing here is game data: the app it writes holds YOUR copy and is not for sharing.
"""
import argparse
import fnmatch
import hashlib
import json
import plistlib
import shutil
import io
import subprocess
import sys
import urllib.request
import zipfile
try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = Path(__file__).resolve().parent
APP_NAME = "Need for Speed Carbon"
BINARY = "CarbonRecomp"
VERSION = "0.1.0"
MAGIC = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
# Beyond game.toml's [bundle] exclude: the installer and online-only leftovers.
EXTRA_EXCLUDE = ["setup.exe", "server.cfg", "autorun.inf", "*.ico"]
# Controller button icons: XtendedInput 1.22's Carbon pack (only its icon file is used).
ICONS_URL = "https://github.com/xan1242/NFS-XtendedInput/releases/download/1.22/Release-Carbon-Pack.zip"
ICONS_ZIP_SHA256 = "86c019cd79a1625c4061d5c7b1bd42cca5350c2d2271f8257ec612e752804001"
ICONS_MEMBER = "GLOBAL/XtendedInputButtons.tpk"
ICONS_SHA256 = "8a2c6e4e131ceb66767677d8ff0425a5d10e3e656c4efaf06231c58482b05990"
ICONS_CACHE = Path.home() / "Library/Caches/NFS Carbon Setup/XtendedInputButtons.tpk"


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def button_icons():
    """The icon file: from the cache, else downloaded and checked. None when offline."""
    if ICONS_CACHE.exists() and sha256(ICONS_CACHE) == ICONS_SHA256:
        return ICONS_CACHE
    try:
        with urllib.request.urlopen(ICONS_URL, timeout=60) as r:
            data = r.read()
    except OSError as e:
        print(f"button icons: download failed ({e}); menus keep keyboard prompts")
        return None
    if hashlib.sha256(data).hexdigest() != ICONS_ZIP_SHA256:
        print("button icons: the download is not the expected file; skipped")
        return None
    tpk = zipfile.ZipFile(io.BytesIO(data)).read(ICONS_MEMBER)
    if hashlib.sha256(tpk).hexdigest() != ICONS_SHA256:
        print("button icons: unexpected icon file in the download; skipped")
        return None
    ICONS_CACHE.parent.mkdir(parents=True, exist_ok=True)
    ICONS_CACHE.write_bytes(tpk)
    return ICONS_CACHE


def is_macho(p):
    with open(p, "rb") as f:
        return f.read(4) in MAGIC


def find_exe(game):
    for p in game.iterdir():
        if p.name.lower() == "nfsc.exe":
            return p
    return None


def clone_tree(src, dst, exclude):
    """Copy the game folder with APFS clones (cp -c) where possible."""
    dst.mkdir(parents=True)
    for p in sorted(src.iterdir()):
        name = p.name.lower()
        if name.startswith(".") or name == "nfsc.exe" or any(fnmatch.fnmatch(name, x) for x in exclude):
            continue
        if subprocess.run(["/bin/cp", "-cR", str(p), str(dst / p.name)], capture_output=True).returncode:
            subprocess.run(["/bin/cp", "-R", str(p), str(dst / p.name)], check=True)


def sign(app):
    nested = sorted((q for q in app.rglob("*") if q.is_file() and not q.is_symlink() and is_macho(q)
                     and "Resources/Game" not in str(q)),
                    key=lambda q: (-len(q.relative_to(app).parts), str(q)))
    for q in nested:
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(q)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(app)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)


# The game's language files (LANGUAGES\\<PREFIX>_FRONTEND.BIN, table at 0xa5eac4 in NFSC.exe) and the
# registry Language names that select them (table 0xa65588, read by 007346b0). This exe has only
# these; Asian releases (Thai, Japanese, Korean, Chinese) shipped a different exe.
LANGUAGES = {"ENGLISH": "English US", "FRENCH": "French", "GERMAN": "German", "ITALIAN": "Italian",
             "SPANISH": "Spanish", "MEXICAN": "Spanish (Mexican)", "DUTCH": "Dutch", "SWEDISH": "Swedish",
             "DANISH": "Danish", "FINNISH": "Finnish", "POLISH": "Polish", "HUNGARIAN": "Hungarian",
             "CZECH": "Czech"}


def game_language(game):
    """The Language registry name for a game folder holding exactly one language's text, else None
    (English, or several languages: the game's own default, English US, applies)."""
    folder = next((p for p in game.iterdir() if p.name.lower() == "languages"), None)
    if not folder:
        return None
    found = {p.name.upper()[:-len("_FRONTEND.BIN")] for p in folder.iterdir()
             if p.name.upper().endswith("_FRONTEND.BIN")} & set(LANGUAGES)
    if len(found) != 1 or found == {"ENGLISH"}:
        return None
    return LANGUAGES[found.pop()]


def set_default_language(registry, language):
    """Write Language (REG_SZ) into the first-run registry.json beside the game's other options."""
    data = json.loads(registry.read_text())
    key = next(iter(data))
    data[key]["Language"] = {"type": 1, "data": language}
    registry.write_text(json.dumps(data, indent=2) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", type=Path, required=True, help="your game folder (the one with NFSC.exe)")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="where to write the app (default dist/)")
    ap.add_argument("--install", action="store_true", help="also copy the app to /Applications")
    ap.add_argument("--hd-ui", action="store_true",
                    help="download and install the NFS Carbon Remastered UI HD textures (about 52 MB) into the app's copy")
    ap.add_argument("--button-icons", action="store_true",
                    help="download XtendedInput's controller button icons (about 1.5 MB) for the menus")
    ap.add_argument("--icon", type=Path, help="a picture to use as the app icon (default: the game's own icon)")
    a = ap.parse_args()

    cfg = tomllib.loads((ROOT / "game.toml").read_text())
    built = ROOT / f"build/{BINARY}.app"
    if not (built / f"Contents/MacOS/{BINARY}").exists():
        sys.exit(f"{built} is missing: run tools/build.py first")
    game = a.game.expanduser().resolve()
    exe = find_exe(game) if game.is_dir() else None
    if not exe:
        sys.exit(f"no NFSC.exe in {game}")
    if sha256(exe) != cfg["game"]["sha256"]:
        sys.exit(f"{exe} is not the supported NFSC.exe (PC v1.4, unwrapped; SHA-256 {cfg['game']['sha256']})")
    aux = []
    for key, mod in sorted(cfg.get("modules", {}).get("aux", {}).items()):
        p = ROOT / mod["path"]
        if not p.exists() or sha256(p) != mod["sha256"]:
            sys.exit(f"{p} is missing or not the rebased {mod['name']}: run tools/rebase_pe.py")
        aux.append((mod["name"], p))
    if subprocess.run(["/usr/bin/pgrep", "-f", f"{APP_NAME}.app/Contents/MacOS/{BINARY}"],
                      capture_output=True).returncode == 0:
        sys.exit("the game is running; quit it first")

    a.out.mkdir(parents=True, exist_ok=True)
    app = a.out / f"{APP_NAME}.app"
    tmp = a.out / f".{APP_NAME}.app.new"
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.copytree(built, tmp, symlinks=True)
    shutil.rmtree(tmp / "Contents/_CodeSignature", ignore_errors=True)
    res = tmp / "Contents/Resources"

    launch = tmp / "Contents/MacOS/Launch"
    subprocess.run(["/usr/bin/clang", "-O2", "-Wall", "-mmacosx-version-min=13.0", "-o", str(launch),
                    str(KIT / "launch.c")], check=True)
    (res / "Defaults").mkdir(exist_ok=True)
    for n in ("registry.json", "mod-settings.json"):
        shutil.copyfile(KIT / "defaults" / n, res / "Defaults" / n)
    language = game_language(game)
    if language:
        set_default_language(res / "Defaults/registry.json", language)
        print(f"language: {language} (the only language in your game folder)")
    for n in ("LICENSE", "NOTICE"):
        if (ROOT / n).exists():
            shutil.copyfile(ROOT / n, res / n)

    exclude = [x.lower() for x in cfg.get("bundle", {}).get("exclude", [])] + EXTRA_EXCLUDE
    print(f"copying the game from {game} ...")
    clone_tree(game, res / "Game", exclude)
    shutil.copyfile(exe, res / "Game/NFSC.exe")
    for name, p in aux:
        shutil.copyfile(p, res / "Game" / name)
    if a.button_icons:
        icons = button_icons()
        if icons:
            (res / "Game/GLOBAL").mkdir(exist_ok=True)
            shutil.copyfile(icons, res / "Game" / ICONS_MEMBER)
            print("button icons: added")
    if a.hd_ui:
        sys.path.insert(0, str(KIT))
        import hd_ui
        mod = hd_ui.archive()
        if mod:
            print("HD UI: installing Remastered UI into the app's copy of the game ...")
            hd_ui.install(res / "Game", mod)

    info = tmp / "Contents/Info.plist"
    plist = plistlib.loads(info.read_bytes())
    plist.update(CFBundleName=APP_NAME, CFBundleDisplayName=APP_NAME, CFBundleExecutable="Launch",
                 CFBundleIdentifier=cfg["game"].get("bundle_id", "io.github.nfsc-native-mac"),
                 CFBundleShortVersionString=VERSION, CFBundleVersion=VERSION,
                 LSApplicationCategoryType="public.app-category.racing-games", LSSupportsGameMode=True,
                 NSHighResolutionCapable=True)
    # The icon: the player's picture, else the game's own logo (from their GLOBAL files), else the icon
    # in their NFSC.exe (32x32 only), else ours.
    sys.path.insert(0, str(KIT))
    import make_icon
    import game_art
    icns = res / "AppIcon.icns"
    try:
        if a.icon:
            make_icon.icns_from_image(a.icon, icns)
            made = True
        else:
            logo = game_art.game_logo(game)
            if logo is not None:
                make_icon.icns_from_logo(logo, icns)
                made = True
            else:
                made = make_icon.icns_from_exe(exe, icns)
    except Exception as e:  # noqa: BLE001 - any failure falls back to the generic icon
        print(f"icon: {e}; using the generic one")
        made = False
    if not made:
        shutil.copyfile(KIT / "defaults/AppIcon.icns", icns)
    plist["CFBundleIconFile"] = "AppIcon"
    info.write_bytes(plistlib.dumps(plist))

    print("signing ...")
    sign(tmp)
    if app.exists():
        shutil.rmtree(app)
    tmp.rename(app)
    print(f"packaged {app}")

    if a.install:
        dest = Path("/Applications") / app.name
        if dest.exists():
            shutil.rmtree(dest)
        subprocess.run(["/usr/bin/ditto", str(app), str(dest)], check=True)
        print(f"installed {dest}")


if __name__ == "__main__":
    main()
