"""Generate the local TITLE font header from the user's own Carbon files."""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "mods/core/nfsc/customization/candidate/missing-title-font.h"
DIGEST = "b124929eeeb7b3b1ef9db9ad220de848f963217bf5425e24c21ad21db77e185c"


def generate(game, output=TARGET):
    resource = Path(game) / "GLOBAL/GlobalMemoryFile.bin"
    with resource.open("rb") as stream:
        stream.seek(0x8B5810)
        data = stream.read(2936)
    if hashlib.sha256(data).hexdigest() != DIGEST:
        raise ValueError("TITLE font differs from the supported Carbon 1.4 resource")
    rows = ["    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ","
            for i in range(0, len(data), 16)]
    text = Path(__file__).with_name("title_font.h.in").read_text().replace(
        "@TITLE_FONT_BYTES@", "\n".join(rows))
    output = Path(output)
    if not output.exists() or output.read_text() != text:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path, help="Your Carbon 1.4 game folder")
    parser.add_argument("--output", type=Path, default=TARGET)
    args = parser.parse_args()
    generate(args.game, args.output)
