"""Install "NFS Carbon Remastered UI" (BadassBaboon, nfsmods.xyz/mod/1095) into the app's copy of the game.

    python tools/setup_kit/hd_ui.py GAME_COPY [--archive FILE] [--language English]

On Windows the mod is installed with Binarius, which runs its Endscript (assets/main.end) against the game folder.
This runs the same script with a small Endscript interpreter (commands as in Endscript, MIT,
github.com/nlgxzef/Endscript) and the TPK writer in bun.py, against GAME_COPY - the app's own copy, never the
player's folder. The archive is downloaded once (pinned SHA-256) into ~/Library/Caches/NFS Carbon Setup.

Streamed track sections (unpack_stream ... pack_stream) are edited in place of Binarius's temp folder: a changed
section is appended to the app's stream file and its index record (L5RA.BUN chunk 0x34110) pointed at it. Not done
yet (logged as "skipped"): packages imported into a file that lacks them. Optional extras (checkbox blocks) take
their "disabled" branch, except those in CHECKBOXES_ON.
"""
import argparse
import hashlib
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bun  # noqa: E402

URL = "https://nfsmods.xyz/uploads/mods/1095/f096ab-Remastered-Textures-4-4.7z"
SHA256 = "e55c7d8ebbdf6e4332e54ef3c77292b5a89875e19e550b66f39ca7fdee7c66ab"
CACHE = Path.home() / "Library/Caches/NFS Carbon Setup"
BASE_GAME = "Vanilla Carbon (Unmodded)"
FNG_PACKAGE, FNG_PACKAGE_PACKED = 0x30203, 0x30210

# DDS pixel format -> (header compression byte, format-table value)
FORMATS = {b"DXT1": (0x22, b"DXT1"), b"DXT3": (0x24, b"DXT3"), b"DXT5": (0x26, b"DXT5"),
           "ARGB": (0x20, struct.pack("<I", 0x15))}
# update_texture fields: (header offset, struct format, named values)
FIELDS = {
    "ClassKey": (16, "<I", None), "TileableUV": (51, "<B", {"NONE": 0, "HORIZONTAL": 1, "VERTICAL": 2, "ALL_SIDES": 3}),
    "BiasLevel": (52, "<B", None), "RenderingOrder": (53, "<B", None),
    "ScrollType": (54, "<B", {"TEXSCROLL_NONE": 0, "TEXSCROLL_SMOOTH": 1, "TEXSCROLL_SNAP": 2, "TEXSCROLL_OFFSETSCALE": 3}),
    "ApplyAlphaSort": (56, "<B", None),
    "AlphaUsageType": (57, "<B", {"TEXUSAGE_NONE": 0, "TEXUSAGE_PUNCHTHRU": 1, "TEXUSAGE_MODULATED": 2}),
    "AlphaBlendType": (58, "<b", {"TEXBLEND_DEFAULT": -1, "TEXBLEND_SRCCOPY": 0, "TEXBLEND_BLEND": 1,
                                  "TEXBLEND_ADDATIVE": 2, "TEXBLEND_SUBTRACTIVE": 3, "TEXBLEND_OVERBRIGHT": 4,
                                  "TEXBLEND_DEST_BLEND": 5, "TEXBLEND_DEST_ADDATIVE": 6,
                                  "TEXBLEND_DEST_SUBTRACTIVE": 7, "TEXBLEND_DEST_OVERBRIGHT": 8}),
    "Flags": (59, "<B", None),
    "MipmapBiasType": (60, "<B", {"TEXBIAS_DEFAULT": 0, "TEXBIAS_ADS": 1, "TEXBIAS_ARC": 2, "TEXBIAS_OBJ": 3,
                                  "TEXBIAS_ORG": 4, "TEXBIAS_SGN": 5, "TEXBIAS_TRN": 6, "TEXBIAS_PARTICLES": 7}),
    "ScrollTimestep": (62, "<h", None), "ScrollSpeedS": (64, "<h", None), "ScrollSpeedT": (66, "<h", None),
    "OffsetS": (68, "<h", None), "OffsetT": (70, "<h", None), "ScaleS": (72, "<h", None), "ScaleT": (74, "<h", None),
}


# unlock_memory: the game preloads some files from "memory files" (GLOBAL/*MemoryFile.bin: header 0, 0, 0x53219999,
# count; then count x {name hash, offset, size, preloaded size, id}; then the data), so a rewritten file would still
# be read from the preloaded copy. Binarius empties them (Darius MemoryUnlock), which also replaces the game's own
# BuildVersion.moo entry with one from another build - a career saved by the player's game then came back as
# "Start Career". Here only the entries of files this install rewrites are taken out; everything else, the build
# stamp included, stays as shipped.
# Checkbox extras turned on: the game runs at 60 fps (or more) here.
CHECKBOXES_ON = {"Use 60FPS Barrier Animation?"}
# Track streaming: index chunk of 0x5C-byte section records {name[8], number, ..., +0x14 file offset, +0x18 size,
# +0x1C stored size, ...}; sections start at 0x800 boundaries of STREAM<track>.BUN.
SECTIONS_CHUNK, SECTION_SIZE, SECTION_ALIGN = 0x34110, 0x5C, 0x800

MEMORY_FILES = ["GLOBAL/GlobalMemoryFile.bin", "GLOBAL/InGameMemoryFile.bin", "GLOBAL/CarHeadersMemoryFile.bin",
                "GLOBAL/PermanentMemoryFile.bin", "GLOBAL/FrontEndMemoryFile.bin"]


def without_entries(data, hashes):
    """A memory file minus the entries whose name hash is in `hashes` (None when nothing changes)."""
    if len(data) < 16 or struct.unpack_from("<I", data, 8)[0] != 0x53219999:
        return None
    count = struct.unpack_from("<I", data, 12)[0]
    entries = [struct.unpack_from("<5I", data, 16 + 20 * i) for i in range(count)]
    keep = [e for e in entries if e[0] not in hashes]
    if len(keep) == len(entries):
        return None
    start = (16 + 20 * len(keep) + 15) & ~15
    index, blob, offset = bytearray(), bytearray(), start
    for h, off, size, preloaded, ident in keep:
        chunk = data[off:off + preloaded]
        index += struct.pack("<5I", h, offset if preloaded else start, size, preloaded, ident)
        blob += chunk + bytes(-len(chunk) % 16)
        offset += len(chunk) + (-len(chunk) % 16)
    return data[:12] + struct.pack("<I", len(keep)) + index + bytes(start - 16 - len(index)) + blob


# update_collection FILE Tracks NUMBER FIELD VALUE: track records (chunk 0x34201, 0x110 bytes each, the track number
# a uint16 at +0x8a; Darius Support.Carbon/Class/Track.cs). Only the map calibration the mod sets.
TRACKS_CHUNK, TRACK_SIZE = 0x34201, 0x110
TRACK_FIELDS = {"TrackMapCalibrationOffsetX": 0xAC, "TrackMapCalibrationOffsetY": 0xB0, "TrackMapCalibrationWidth": 0xB4}


def binhash(name):
    """The engine's bStringHash, which keys every texture."""
    h = 0xFFFFFFFF
    for c in name.encode("latin-1"):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def read_dds(path):
    d = Path(path).read_bytes()
    if d[:4] != b"DDS " or len(d) < 128:
        raise ValueError(f"{path}: not a DDS file")
    h, w, _, _, mips = struct.unpack_from("<5I", d, 12)
    pf_flags, fourcc, bits = struct.unpack_from("<I4sI", d, 80)
    if pf_flags & 4:
        if fourcc not in FORMATS:
            raise ValueError(f"{path}: unsupported format {fourcc!r}")
        kind = fourcc
        top = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fourcc == b"DXT1" else 16)
    elif bits == 32:
        kind, top = "ARGB", w * h * 4
    else:
        raise ValueError(f"{path}: unsupported {bits}-bit format")
    return w, h, max(1, mips), kind, top, d[128:]


def reload(t, path):
    """A texture's size, format, mips and pixels from a DDS file (Darius Texture.Initialize)."""
    w, h, mips, kind, top, data = read_dds(path)
    comp, fmt = FORMATS[kind]
    t.data = data
    struct.pack_into("<i", t.header, 24, -1)                 # no palette
    struct.pack_into("<iii", t.header, 28, len(data), 0, top)  # size, palette size, area
    struct.pack_into("<hhBBBB", t.header, 40, w, h, w.bit_length() - 1, h.bit_length() - 1, comp, 0)
    struct.pack_into("<h", t.header, 48, 0)                  # palettes
    t.header[50] = mips
    struct.pack_into("<7h", t.header, 62, 0, 0, 0, 0, 0x100, 0x100, 0)
    t.fmt[12:16] = fmt


def new_texture(name, path):
    """A texture the pack did not have (Darius Texture(CName, filename, tpk) defaults)."""
    chars = name.encode("latin-1")[:0x22]
    length = 4 + len(chars) - (1 + len(chars)) % 4
    header = bytearray(0x58) + bytes([length]) + chars + bytes(length - len(chars))
    struct.pack_into("<II", header, 12, binhash(name), 0x1A93CF)
    t = bun.Texture(header, bytes(0x18), b"")
    reload(t, path)
    return t


def add(tpk, t):
    tpk.textures.append(t)
    tpk.textures.sort(key=lambda x: x.key)


def parse_blocks(lines):
    """Script lines -> nested list of ("cmd", tokens) / ("if", cond, then, else) / ("checkbox", q, on, off) /
    ("combobox", options, question, {option: body})."""
    pos = 0

    def tokens(line):
        return shlex.split(line.replace("\\", "/"), posix=True)

    def block(stop):
        nonlocal pos
        out = []
        while pos < len(lines):
            raw = lines[pos].strip()
            pos += 1
            if not raw or raw.startswith("//") or raw.startswith("["):
                continue
            t = tokens(raw)
            if t[0] in stop:
                return out, t[0]
            if t[0] == "if":
                pos += 1  # "do"
                then, end = block({"else", "end"})
                other = block({"end"})[0] if end == "else" else []
                out.append(("if", t[1:], then, other))
            elif t[0] == "checkbox":
                on = off = []
                state = lines[pos].strip()
                pos += 1
                first, end = block({"enabled", "disabled", "end"})
                if state == "enabled":
                    on = first
                    if end == "disabled":
                        off = block({"end"})[0]
                else:
                    off = first
                out.append(("checkbox", t[1], on, off))
            elif t[0] == "combobox":
                pos += 1  # "{"
                bodies = {}
                while lines[pos].strip() != "}":
                    option = lines[pos].strip().strip('"')
                    pos += 2  # option, "{"
                    bodies[option] = block({"}"})[0]
                pos += 1  # "}"
                while lines[pos].strip() != "end":
                    pos += 1
                pos += 1
                out.append(("combobox", t[1:-1], t[-1], bodies))
            else:
                out.append(("cmd", t))
        return out, None

    return block(set())[0]


class Installer:
    def __init__(self, game, assets, language, log=print):
        self.game, self.assets, self.language, self.log = Path(game), Path(assets), language, log
        self.files = {}      # game-relative path (upper) -> ChunkFile
        self.raw = {}        # game-relative path (upper) -> bytes, for whole-file copies
        self.skipped = {}
        self.names = {}      # game-relative path (upper) -> as the script spells it
        self.unlock = False
        self.streams = {}    # temp folder (upper) -> (index file, stream file), from unpack_stream

    def path(self, rel):
        """The real (case-preserving) path of a game-relative path in the copy."""
        p = self.game
        for part in rel.replace("\\", "/").split("/"):
            match = next((q for q in p.iterdir() if q.name.lower() == part.lower()), None) if p.is_dir() else None
            p = match if match else p / part
        return p

    def file(self, rel):
        key = rel.upper()
        self.names.setdefault(key, rel)
        if key not in self.files:
            section = self.section_of(rel)
            data = (self.raw[key] if key in self.raw else self.read_section(*section) if section
                    else bun.load(self.path(rel)))
            self.files[key] = bun.ChunkFile(data)
        return self.files[key]

    def section_of(self, rel):
        """(index file, stream file, section name) for TEMP/<section>/DATA.BIN under an unpack_stream folder."""
        parts = rel.replace("\\", "/").upper().split("/")
        if len(parts) >= 3 and parts[-1] == "DATA.BIN":
            stream = self.streams.get("/".join(parts[:-2]))
            if stream:
                return stream + (parts[-2],)
        return None

    def section_record(self, index, name):
        """(chunk file, item number, record offset) of a section's index record."""
        f = self.file(index)
        for i, (_, item) in enumerate(f.items):
            if isinstance(item, bytes) and struct.unpack_from("<I", item)[0] == SECTIONS_CHUNK:
                for at in range(8, len(item) - SECTION_SIZE + 1, SECTION_SIZE):
                    if item[at:at + 8].split(b"\0")[0].decode("latin-1").upper() == name:
                        return f, i, at
        raise RuntimeError(f"no stream section {name} in {index}")

    def read_section(self, index, stream, name):
        f, i, at = self.section_record(index, name)
        offset, size, stored = struct.unpack_from("<3I", f.items[i][1], at + 0x14)
        if size != stored:
            raise RuntimeError(f"stream section {name} is compressed")
        with open(self.path(stream), "rb") as fh:
            fh.seek(offset)
            return fh.read(size)

    def pack_sections(self):
        """Changed sections go at the end of the app's copy of the stream file (an independent copy, never the
        player's file); their index records point there."""
        for key in [k for k in self.files if getattr(self.files[k], "dirty", False) and self.section_of(k)]:
            index, stream, name = self.section_of(key)
            data = self.files.pop(key).assemble()
            dest = self.path(stream)
            with open(dest, "r+b") as fh:
                end = fh.seek(0, 2)
                offset = end + (-end % SECTION_ALIGN)
                fh.write(bytes(offset - end) + data)
            f, i, at = self.section_record(index, name)
            phase, item = f.items[i]
            item = bytearray(item)
            struct.pack_into("<3I", item, at + 0x14, offset, len(data), len(data))
            f.items[i] = (phase, bytes(item))
            self.mark(index)
            self.log(f"  stream section {name} rewritten at the end of {stream} ({len(data) // 1024} KB)")

    def tpk(self, rel, collection):
        p = self.path(rel)
        if not p.exists() and rel.upper() not in self.raw and not self.section_of(rel):
            return None, "missing file"
        tpk = self.file(rel).tpk(collection)
        if tpk is None:
            return None, "compressed or missing pack"
        return tpk, None

    def skip(self, why, cmd):
        self.skipped.setdefault(why, []).append(" ".join(cmd[:4]))

    def run(self, blocks):
        for b in blocks:
            if b[0] == "if":
                cond = b[1]
                exists = cond[0] == "collection_exists" and self.tpk(cond[1], cond[3])[0] is not None
                self.run(b[2] if exists else b[3])
            elif b[0] == "checkbox":
                self.run(b[2] if b[1] in CHECKBOXES_ON else b[3])
            elif b[0] == "combobox":
                choice = self.language if "language" in b[2].lower() else BASE_GAME
                self.run(b[3].get(choice, []))
            else:
                self.command(b[1])

    def command(self, c):
        op = c[0]
        if op == "end":
            return
        if op == "unpack_stream":
            # unpack_stream INDEX STREAM TEMP: sections are read from STREAM when TEMP/<name>/DATA.BIN is used
            self.streams[c[3].upper()] = (c[1], c[2])
            return
        if op in ("pack_stream", "erase_folder"):
            return  # sections are written back in write()
        if op == "delete" or (op == "new" and self.section_of(c[-1])):
            return  # Binarius closing a file it had open / opening a temp file
        if op == "unlock_memory":
            self.unlock = True  # applied in write(), once the rewritten files are known
            return
        if op == "move_file":
            # move_file override relative absolute SRC DST: SRC in the mod, DST in the game
            src, dst = self.assets / c[4], c[5]
            if dst.lower().startswith("scripts/") or dst.lower().endswith((".asi", ".ini")):
                self.log(f"  not needed on the Mac: {dst} (its job is done natively by core.nfsc)")
                return
            self.names[dst.upper()] = dst
            self.raw[dst.upper()] = src.read_bytes()
            self.files.pop(dst.upper(), None)
            self.log(f"  copied {c[4]} -> {dst}")
        elif op == "update_collection":
            if c[2] == "TPKBlocks" and c[4] in ("CompressionType", "CollectionName"):
                return  # packs keep their kind; names already match the vanilla game
            if c[2] == "Tracks" and c[4] in TRACK_FIELDS:
                self.track_field(c[1], int(c[3]), c[4], float(c[5]), c)
                return
            self.skip(f"unsupported collection field {c[2]} {c[4]}", c)
        elif op in ("bind_textures", "replace_texture", "update_texture"):
            mode, rel, collection = (c[1], c[2], c[4]) if op == "bind_textures" else (None, c[1], c[3])
            tpk, why = self.tpk(rel, collection)
            if tpk is None:
                self.skip(why, c)
                return
            if op == "bind_textures":
                folder = self.assets / c[5]
                for dds in sorted(folder.glob("*.dds")):
                    name, key = dds.stem, binhash(dds.stem)
                    t = next((x for x in tpk.textures if x.key == key), None)
                    if t is None:
                        add(tpk, new_texture(name, dds))
                    elif mode == "synchronize":
                        reload(t, dds)
                    else:  # override
                        tpk.textures.remove(t)
                        add(tpk, new_texture(name, dds))
                self.mark(rel)
            else:
                key = int(c[4], 0)
                t = next((x for x in tpk.textures if x.key == key), None)
                if t is None:
                    self.skip("texture not in pack", c)
                    return
                if op == "replace_texture":
                    reload(t, self.assets / c[5])
                else:
                    off, fmt, names = FIELDS[c[5]]
                    value = names[c[6]] if names else int(c[6], 0)
                    struct.pack_into(fmt, t.header, off, value)
                self.mark(rel)
        elif op == "import":
            # import synchronize|override FILE FNGroups PACKAGE.bin: the mod's package (a raw 0x30203 chunk) replaces
            # the package of the same name in FILE, stored raw (0x30203) or JDLZ-packed (0x30210, a hash + JDLZ).
            mode, rel, kind, src = c[1], c[2], c[3], self.assets / c[4]
            data = src.read_bytes()
            name = re.search(rb"[A-Za-z0-9_]+\.fng", data[:0x100])
            if not name:
                self.skip("package without a name", c)
                return
            want = name.group().lower()
            f = self.file(rel)
            for i, (phase, item) in enumerate(f.items):
                if not isinstance(item, bytes):
                    continue
                cid = struct.unpack_from("<I", item)[0]
                head = item[8:0x108] if cid == FNG_PACKAGE else None
                if cid == FNG_PACKAGE_PACKED:
                    try:
                        head = bun.jdlz_decompress(item[12:])[:0x100]
                    except (IndexError, ValueError):
                        head = None
                if head is not None and want in head.lower():
                    f.items[i] = (phase, data)
                    self.mark(rel)
                    self.log(f"  layout {name.group().decode()} replaced in {rel}")
                    return
            if mode == "override":
                f.items.append((0, data))
                self.mark(rel)
                self.log(f"  layout {name.group().decode()} added to {rel}")
                return
            self.skip("package not in that file", c)
        else:
            self.skip(f"unsupported command {op}", c)

    def track_field(self, rel, number, field, value, cmd):
        f = self.file(rel)
        for i, (phase, item) in enumerate(f.items):
            if isinstance(item, bytes) and struct.unpack_from("<I", item)[0] == TRACKS_CHUNK:
                rec = bytearray(item)
                for at in range(8, len(rec) - TRACK_SIZE + 1, TRACK_SIZE):
                    if struct.unpack_from("<H", rec, at + 0x8A)[0] == number:
                        struct.pack_into("<f", rec, at + TRACK_FIELDS[field], value)
                        f.items[i] = (phase, bytes(rec))
                        self.mark(rel)
                        return
        self.skip("track not found", cmd)

    def mark(self, rel):
        self.files[rel.upper()].dirty = True

    def write(self):
        written = []
        self.pack_sections()
        keys = sorted(set(self.raw) | {k for k, f in self.files.items() if getattr(f, "dirty", False)})
        # HD_UI_SKIP="GLOBAL/GLOBALB.LZC,..." leaves those files as shipped (for narrowing a problem down).
        skip = {x.strip().upper() for x in os.environ.get("HD_UI_SKIP", "").split(",") if x.strip()}
        import fnmatch
        for key in [k for k in keys if any(fnmatch.fnmatch(k, pat) for pat in skip)]:
            keys.remove(key)
            self.log(f"  left as shipped (HD_UI_SKIP): {key}")
        if self.unlock:
            hashes = set()
            for key in keys:
                rel = str(self.path(self.names.get(key, key)).relative_to(self.game)).replace("/", "\\")
                hashes |= {binhash(rel), binhash(rel.upper()), binhash(rel.lower())}
            for rel in MEMORY_FILES:
                p = self.path(rel)
                trimmed = without_entries(p.read_bytes(), hashes) if p.exists() else None
                if trimmed is not None:
                    self.names[rel.upper()], self.raw[rel.upper()] = rel, trimmed
                    keys.append(rel.upper())
                    self.log(f"  {rel}: preloaded copies of rewritten files taken out")
        for key in keys:
            data = self.files[key].assemble() if key in self.files else self.raw[key]
            dest = self.path(self.names.get(key, key))
            if dest.suffix.lower() == ".lzc" and data[:4] != b"JDLZ":
                # .lzc files ship as one JDLZ block, which the game unpacks on load (0069eda0); without a
                # compressor they stay unpacked, which the game also accepts.
                data = bun.jdlz_compress(data, len(data)) or data
            tmp = dest.with_name(dest.name + ".hdui")
            tmp.write_bytes(data)
            os.replace(tmp, dest)  # replaces the APFS clone, never the player's file
            written.append(f"{dest.relative_to(self.game)} ({len(data) // 1024} KB)")
        return written


def archive(path=None):
    """The mod's .7z: a given file, else the cache, else downloaded; None when unavailable."""
    if path:
        return Path(path)
    cached = CACHE / "Remastered-Textures-4-4.7z"
    if cached.exists() and hashlib.sha256(cached.read_bytes()).hexdigest() == SHA256:
        return cached
    try:
        CACHE.mkdir(parents=True, exist_ok=True)
        part = cached.with_suffix(".part")
        req = urllib.request.Request(URL, headers={"User-Agent": "nfs-carbon-mac setup"})
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        if hashlib.sha256(part.read_bytes()).hexdigest() != SHA256:
            part.unlink()
            print("HD UI: the download is not the expected file; skipped")
            return None
        part.rename(cached)
        return cached
    except OSError as e:
        print(f"HD UI: download failed ({e}); skipped")
        return None


def install(game, archive_path, language="English", log=print):
    with tempfile.TemporaryDirectory() as tmp:
        if subprocess.run(["/usr/bin/tar", "-xf", str(archive_path), "-C", tmp]).returncode:
            raise RuntimeError("could not unpack the HD UI archive")
        assets = Path(tmp) / "RemasteredUI/assets"
        lines = (assets / "main.end").read_text(encoding="utf-8-sig", errors="replace").splitlines()
        inst = Installer(game, assets, language, log)
        inst.run(parse_blocks(lines))
        for name in inst.write():
            log(f"  wrote {name}")
        for why, cmds in inst.skipped.items():
            log(f"  skipped ({why}): {len(cmds)} commands")
        return inst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path, help="the app's copy of the game (Contents/Resources/Game)")
    ap.add_argument("--archive", type=Path, help="Remastered-Textures-4-4.7z (default: download, cached)")
    ap.add_argument("--language", default="English", choices=["English", "Russian"])
    a = ap.parse_args()
    path = archive(a.archive)
    if path:
        install(a.game, path, a.language)


if __name__ == "__main__":
    main()
