"""Read textures out of the player's own Carbon files (TPK packs inside .BUN/.BIN), for the app icon.

    python tools/setup_kit/game_art.py FILE [NAME ...]     # list a file's textures, or save NAME.png

A TPK (chunk 0xB3300000) holds an info block (0xB3310000: 0x33310004 one header per texture, 0x33310005
one 24-byte format entry each) and a data block (0xB3320000: 0x33320002, the pixels, from the next
0x80 boundary). A texture header is 0x58 bytes - hash at +12, data offset +20, size +28, width/height
+40/+42 - then a length byte and the name. Formats: DXT1/DXT3/DXT5 or 0x15 (A8R8G8B8). Only the top
mip is read. Nothing is written into the game folder.
"""
import struct
import sys
from pathlib import Path

from PIL import Image

A8R8G8B8 = b"\x15\x00\x00\x00"


def _chunks(d, o, end):
    while o + 8 <= end:
        cid, size = struct.unpack_from("<II", d, o)
        yield cid, o + 8, size
        o += 8 + size


def textures(d):
    """Every texture in every TPK of `d`: dicts with name, w, h, fmt, raw (top mip bytes)."""
    for cid, o, s in _chunks(d, 0, len(d)):
        if cid != 0xB3300000:
            continue
        info = fmt = data = None
        for c, p, sz in _chunks(d, o, o + s):
            if c == 0xB3310000:
                for c2, p2, s2 in _chunks(d, p, p + sz):
                    if c2 == 0x33310004:
                        info = (p2, s2)
                    elif c2 == 0x33310005:
                        fmt = (p2, s2)
            elif c == 0xB3320000:
                for c2, p2, s2 in _chunks(d, p, p + sz):
                    if c2 == 0x33320002:
                        data = (p2 + 0x7F) & ~0x7F
        if not info or not fmt or data is None:
            continue
        p, end, i = info[0], info[0] + info[1], 0
        while p + 0x59 <= end:
            off, size = struct.unpack_from("<I", d, p + 20)[0], struct.unpack_from("<I", d, p + 28)[0]
            w, h = struct.unpack_from("<HH", d, p + 40)
            n = d[p + 0x58]
            name = d[p + 0x59:p + 0x59 + n].split(b"\0")[0].decode("latin-1")
            yield {"name": name, "w": w, "h": h, "fmt": d[fmt[0] + 24 * i + 12:fmt[0] + 24 * i + 16],
                   "raw": d[data + off:data + off + size]}
            p += 0x59 + n
            i += 1


def _rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode(t):
    """A texture's top mip as an RGBA PIL image."""
    w, h, f, raw = t["w"], t["h"], t["fmt"], t["raw"]
    if f == A8R8G8B8:
        return Image.frombytes("RGBA", (w, h), raw[:w * h * 4], "raw", "BGRA")
    if f not in (b"DXT1", b"DXT3", b"DXT5"):
        raise ValueError(f"{t['name']}: unsupported format {f!r}")
    bs = 8 if f == b"DXT1" else 16
    px = bytearray(w * h * 4)
    o = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = raw[o:o + bs]
            o += bs
            alpha = None
            if f == b"DXT5":
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                pal = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else
                                  [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [pal[bits >> (3 * i) & 7] for i in range(16)]
            elif f == b"DXT3":
                bits = int.from_bytes(blk[0:8], "little")
                alpha = [(bits >> (4 * i) & 15) * 17 for i in range(16)]
            c0, c1, idx = struct.unpack_from("<HHI", blk, bs - 8)
            a, b = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or bs == 16:
                cols = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
            else:
                cols = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < w and y < h:
                    k = idx >> (2 * i) & 3
                    al = alpha[i] if alpha else (0 if c0 <= c1 and k == 3 else 255)
                    q = 4 * (y * w + x)
                    px[q:q + 4] = bytes((*cols[k], al))
    return Image.frombytes("RGBA", (w, h), bytes(px))


def find_ci(base, *parts):
    """base/parts... without regard to case (Windows game folders mix cases)."""
    p = Path(base)
    for part in parts:
        if not p.is_dir():
            return None
        p = next((q for q in p.iterdir() if q.name.lower() == part.lower()), None)
        if p is None:
            return None
    return p


def texture(path, name):
    """The texture `name` from the pack at `path`, decoded, or None."""
    if not path or not path.is_file():
        return None
    for t in textures(path.read_bytes()):
        if t["name"] == name:
            return decode(t)
    return None


def game_logo(game):
    """The game's own "Need for Speed Carbon" logo (512x128, from the boot textures), or None."""
    return texture(find_ci(game, "GLOBAL", "UniqueBootTextures.bin"), "NFS07_LOGO")


if __name__ == "__main__":
    data = Path(sys.argv[1]).read_bytes()
    wanted = set(sys.argv[2:])
    for t in textures(data):
        if not wanted:
            print(f"{t['name']:40} {t['w']}x{t['h']} {t['fmt']!r}")
        elif t["name"] in wanted:
            decode(t).save(t["name"] + ".png")
            print("saved", t["name"] + ".png")
