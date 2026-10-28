#!/usr/bin/env python3
"""Download the Ghidra and Java that tools/analyze.py needs.

    python3 tools/fetch_tools.py

Same pinned releases and SHA-256 checks as the MW port's setup kit. Everything
lands in ~/Library/NFSC-Native-Setup (not ~/Library/Caches: Ghidra 12.1.3
cannot find its extension points there). Ghidra ships no Apple Silicon
decompiler, so it is built once from Ghidra's own sources. Prints the two
paths to pass to analyze.py at the end; rerunning reuses what is there.
"""
import hashlib
import shutil
import subprocess
import urllib.request
from pathlib import Path

CACHE = Path.home() / "Library/NFSC-Native-Setup"
TOOLS = {
    "ghidra": ("https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.3_build/"
               "ghidra_12.1.3_PUBLIC_20260817.zip",
               "93a5d11a9ad510622acaaf908c556a7b9b764d338e78a7567f3689bf5081fd54"),
    "jdk": ("https://download.java.net/java/GA/jdk26.0.1/458fda22e4c54d5ba572ab8d2b22eb83/8/GPL/"
            "openjdk-26.0.1_macos-aarch64_bin.tar.gz",
            "b2d57405194a312ed4ec6ec08e83b314d3fd2e425e895d704ec5ef8ea6059e17"),
}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    url, digest = TOOLS[name]
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = CACHE / url.rsplit("/", 1)[1]
    if not archive.exists() or sha(archive) != digest:
        print(f"downloading {archive.name}", flush=True)
        part = archive.with_suffix(".part")
        with urllib.request.urlopen(url) as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        if sha(part) != digest:
            part.unlink()
            raise SystemExit(f"{archive.name} did not match its pinned SHA-256; not used")
        part.rename(archive)
    out = CACHE / name
    if not out.exists():
        print(f"unpacking {archive.name}", flush=True)
        tmp = CACHE / (name + ".unpack")
        shutil.rmtree(tmp, ignore_errors=True)
        if archive.suffix == ".zip":
            subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(tmp)], check=True)
        else:
            tmp.mkdir()
            # macOS's own tar: works on every Python (tarfile's filter= needs 3.12+).
            subprocess.run(["/usr/bin/tar", "-xzf", str(archive), "-C", str(tmp)], check=True)
        tmp.rename(out)
    if name == "ghidra":
        return next(out.glob("ghidra_*_PUBLIC"))
    return next(out.glob("jdk-*.jdk")) / "Contents/Home"


def build_decompiler(ghidra, java):
    patterns = ("Ghidra/Features/Decompiler/*/os/mac_arm_64/decompile",
                "Ghidra/Features/Decompiler/os/mac_arm_64/decompile")
    if any(any(ghidra.glob(p)) for p in patterns):
        return
    print("building Ghidra's decompiler for Apple Silicon (once, a few minutes)", flush=True)
    # Started as arm64 explicitly: an Intel parent makes Gradle's xcrun an Intel
    # process, which the Command Line Tools on macOS 27 can no longer run.
    subprocess.run(["/usr/bin/arch", "-arm64", "/bin/sh", str(ghidra / "support/gradle/gradlew"),
                    "--no-daemon", "-q", "buildNatives"],
                   cwd=ghidra / "support/gradle", env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                                                       "HOME": str(Path.home()), "JAVA_HOME": str(java)},
                   check=True)
    if not any(any(ghidra.glob(p)) for p in patterns):
        raise SystemExit("Ghidra's decompiler did not build; see the output above")


def main():
    java = fetch("jdk")
    ghidra = fetch("ghidra")
    build_decompiler(ghidra, java)
    print("\nReady. Run:")
    print(f'  .venv/bin/python tools/analyze.py --ghidra-home "{ghidra}" --java-home "{java}"')


if __name__ == "__main__":
    main()
