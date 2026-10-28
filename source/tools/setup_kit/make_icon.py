#!/usr/bin/env python3
"""App icons.

    python tools/setup_kit/make_icon.py    # draw the fallback defaults/AppIcon.icns

The fallback is original artwork made here (carbon-fibre weave, speed streaks,
the word CARBON), so the repository carries no EA logo or game art. package.py
prefers the game's own icon, read from the player's NFSC.exe at build time
(icns_from_exe), or any picture the player names (icns_from_image).
"""
import io
import math
import struct
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tools/setup_kit/defaults/AppIcon.icns"
FONT = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
S = 1024  # master size; macOS icon grid: 824px rounded square centred in 1024


def rounded_mask(size, inset, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([inset, inset, size - inset, size - inset], radius, fill=255)
    return m


def carbon_weave(size, cell=28):
    """Twill weave: alternating light/dark diagonal tows."""
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            cx, cy = x // cell, y // cell
            u, v = (x % cell) / cell, (y % cell) / cell
            horizontal = (cx + cy) % 2 == 0
            t = v if horizontal else u            # across the tow
            shade = 0.55 + 0.45 * math.sin(math.pi * t)  # rounded tow highlight
            base = 34 if (cx + cy // 2) % 2 else 24
            g = int(base * (0.6 + 0.8 * shade))
            px[x, y] = (g, g, int(g * 1.08))
    return img


def write_icns(icon, out):
    """A 1024x1024 RGBA picture -> .icns with every size macOS asks for."""
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                icon.resize((px, px), Image.LANCZOS).save(iconset / name)
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(iconset), "-o", str(out)], check=True)


def on_tile(art, smooth_from=None):
    """Fill the macOS icon grid's rounded square (824 px in 1024) with `art`."""
    if smooth_from:  # a tiny source: bicubic up, then sharpen, before the final resize
        art = art.resize((smooth_from, smooth_from), Image.BICUBIC).filter(ImageFilter.UnsharpMask(4, 120, 2))
    inset, radius = 100, 185
    full = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    full.paste(art.convert("RGBA").resize((S - 2 * inset, S - 2 * inset), Image.LANCZOS), (inset, inset))
    icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    icon.paste(full, (0, 0), rounded_mask(S, inset, radius))
    return icon


def exe_icon(exe):
    """The largest icon in a PE file's resources, as a PIL image, or None."""
    d = Path(exe).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    magic = struct.unpack_from("<H", d, pe + 24)[0]
    rsrc = struct.unpack_from("<I", d, pe + 24 + (96 if magic == 0x10B else 112) + 16)[0]
    secs = [struct.unpack_from("<8sIIII", d, pe + 24 + opt + 40 * i) for i in range(n)]

    def off(rva):
        for _, vs, va, rs, ro in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + ro
        return None

    base = off(rsrc) if rsrc else None
    if base is None:
        return None

    def entries(o):
        named, ids = struct.unpack_from("<HH", d, o + 12)
        for i in range(named + ids):
            yield struct.unpack_from("<II", d, o + 16 + 8 * i)

    def leaves(type_id):
        for tid, ptr in entries(base):
            if tid == type_id:
                for nid, p2 in entries(base + (ptr & 0x7FFFFFFF)):
                    for _, p3 in entries(base + (p2 & 0x7FFFFFFF)):
                        rva, size = struct.unpack_from("<II", d, base + p3)
                        yield nid, d[off(rva):off(rva) + size]

    icons = dict(leaves(3))  # RT_ICON by id
    best = None
    for _, group in leaves(14):  # RT_GROUP_ICON: pick its largest, deepest entry
        for k in range(struct.unpack_from("<H", group, 4)[0]):
            w, h, colors, _, planes, bits, size, iid = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * k)
            key = ((w or 256) * (h or 256), bits)
            if iid in icons and (best is None or key > best[0]):
                best = (key, w, h, colors, planes, bits, icons[iid])
    if not best:
        return None
    _, w, h, colors, planes, bits, data = best
    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w, h, colors, 0, planes, bits, len(data), 22) + data
    return Image.open(io.BytesIO(ico)).convert("RGBA")


def icns_from_exe(exe, out):
    art = exe_icon(exe)
    if art is None:
        return False
    write_icns(on_tile(art, smooth_from=512 if art.width < 256 else None), out)
    return True


def icns_from_image(path, out):
    art = Image.open(path).convert("RGBA")
    side = min(art.size)  # centre square crop
    art = art.crop(((art.width - side) // 2, (art.height - side) // 2,
                    (art.width + side) // 2, (art.height + side) // 2))
    write_icns(on_tile(art), out)


def icns_from_logo(logo, out):
    """The game's own logo (from the player's files, see game_art.py) across a dark carbon tile with a
    blue glow behind it."""
    logo = logo.convert("RGBA")
    logo = logo.crop(logo.getbbox())
    bg = carbon_weave(S, cell=16).convert("RGBA")
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([-S * 0.1, S * 0.22, S * 1.1, S * 0.78], fill=(30, 130, 255, 170))
    bg = Image.alpha_composite(bg, glow.filter(ImageFilter.GaussianBlur(120)))
    vignette = Image.new("L", (S, S), 0)
    ImageDraw.Draw(vignette).ellipse([-S * 0.3, -S * 0.3, S * 1.3, S * 1.3], fill=255)
    bg = Image.composite(bg, Image.new("RGBA", (S, S), (0, 0, 0, 255)), vignette.filter(ImageFilter.GaussianBlur(140)))
    w = int(S * 0.97)
    h = int(logo.height * w / logo.width)
    bg.alpha_composite(logo.resize((w, h), Image.LANCZOS), ((S - w) // 2, (S - h) // 2))
    write_icns(on_tile(bg), out)


def main():
    weave = carbon_weave(S)
    # Vignette and a cool top light.
    light = Image.new("L", (S, S), 0)
    ImageDraw.Draw(light).ellipse([-S * 0.2, -S * 0.6, S * 1.2, S * 0.7], fill=60)
    light = light.filter(ImageFilter.GaussianBlur(120))
    tint = Image.new("RGB", (S, S), (60, 100, 140))
    bg = Image.composite(tint, weave, light)

    # Speed streaks: cyan/purple light trails sweeping left to right.
    streaks = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(streaks)
    for i, (y, w, col) in enumerate([(585, 10, (0, 230, 255)), (640, 22, (0, 200, 255)),
                                     (700, 8, (170, 90, 255)), (745, 14, (0, 230, 255)),
                                     (800, 6, (170, 90, 255))]):
        x0 = 150 + 60 * i
        d.polygon([(x0, y), (S - 120, y - 40 - w), (S - 120, y - 40 + w), (x0, y + 2)], fill=col + (235,))
    glow = streaks.filter(ImageFilter.GaussianBlur(14))
    bg = bg.convert("RGBA")
    bg.alpha_composite(glow)
    bg.alpha_composite(streaks)

    # Wordmark.
    text = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    td = ImageDraw.Draw(text)
    font = ImageFont.truetype(FONT, 138)
    word = "CARBON"
    box = td.textbbox((0, 0), word, font=font)
    tw, th = box[2] - box[0], box[3] - box[1]
    pos = ((S - tw) // 2 - box[0], 330 - box[1])
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((pos[0] + 6, pos[1] + 10), word, font=font, fill=(0, 0, 0, 200))
    bg.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))
    td.text(pos, word, font=font, fill=(235, 245, 255, 255))
    # Cyan underline.
    td.rectangle([(S - tw) // 2, 330 + th + 34, (S + tw) // 2, 330 + th + 46], fill=(0, 220, 255, 255))
    bg.alpha_composite(text)

    # Rim highlight, then the macOS rounded-square shape.
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([100, 100, S - 100, S - 100], 185, outline=(255, 255, 255, 70), width=4)
    bg.alpha_composite(rim)
    icon = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    icon.paste(bg, (0, 0), rounded_mask(S, 100, 185))

    write_icns(icon, OUT)
    icon.resize((512, 512), Image.LANCZOS).save(OUT.with_suffix(".png"))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
