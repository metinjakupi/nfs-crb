"""Read and rewrite Need for Speed: Carbon's chunk files (.BUN/.BIN/.LZC) and the texture packs (TPK) in them.

Used at build time to apply texture mods to the app's own copy of the game (tools/setup_kit/hd_ui.py); the
player's game folder is never written to. The TPK layout follows Darius (github.com/nlgxzef/Darius, MIT,
Nikki/Support.Carbon/Class/TPKBlock.cs and Texture.cs); see CLAUDE.md "HD UI mod" for the field map.

    python tools/setup_kit/bun.py FILE          # round-trip check: parse, rewrite, compare
"""
import ctypes
import hashlib
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TPK, TPK_INFO, TPK_DATA = 0xB3300000, 0xB3310000, 0xB3320000
INFO_HEADER, INFO_KEYS, INFO_TEXTURES, INFO_FORMATS = 0x33310001, 0x33310002, 0x33310004, 0x33310005
INFO_SLOTS, PART_MAGIC = 0x33310003, 0x55441122
DATA_HEADER, DATA_PIXELS = 0x33320001, 0x33320002
ALIGN = 0x80
_JDLZ = []


def jdlz_compressor():
    """jdlz.c's jdlz_compress through ctypes, built once into the Setup cache; None if there is no compiler."""
    if not _JDLZ:
        source = Path(__file__).with_name("jdlz.c")
        tag = hashlib.sha256(source.read_bytes()).hexdigest()[:12]
        lib = Path.home() / f"Library/Caches/NFS Carbon Setup/jdlz-{tag}.dylib"
        fn = None
        try:
            if not lib.is_file():
                lib.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(["cc", "-O2", "-shared", "-o", str(lib), str(source)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            fn = ctypes.CDLL(str(lib)).jdlz_compress
            fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
            fn.restype = ctypes.c_size_t
        except (OSError, subprocess.CalledProcessError):
            fn = None
        _JDLZ.append(fn)
    return _JDLZ[0]


def jdlz_compress(data, cap):
    """A JDLZ block for data, or None if it would not be smaller than cap bytes (or no compressor)."""
    fn = jdlz_compressor()
    if fn is None or not data:
        return None
    out = ctypes.create_string_buffer(cap)
    n = fn(data, len(data), out, cap)
    return out.raw[:n] if n else None


def jdlz_decompress(src):
    """EA's JDLZ (header 'JDLZ' 02 10, uncompressed size at +8)."""
    size = struct.unpack_from("<I", src, 8)[0]
    out = bytearray()
    i, f1, f2 = 16, 1, 1
    while len(out) < size:
        if f1 == 1:
            f1 = src[i] | 0x100
            i += 1
        if f2 == 1:
            f2 = src[i] | 0x100
            i += 1
        if f1 & 1:
            if f2 & 1:
                length, back = (src[i + 1] | (src[i] & 0xF0) << 4) + 3, (src[i] & 0x0F) + 1
            else:
                back, length = (src[i + 1] | (src[i] & 0xE0) << 3) + 17, (src[i] & 0x1F) + 3
            i += 2
            for _ in range(length):
                out.append(out[-back])
            f2 >>= 1
        else:
            out.append(src[i])
            i += 1
        f1 >>= 1
    return bytes(out)


# HUFF (EA's canonical Huffman codec) decoder: from the Need for Speed: Most Wanted Mac port
# (github.com/elforeign/nfs-most-wanted-mac, tools/setup_kit/tpk.py, MIT; see NOTICE).
class Bits:
    """Big-endian bit reader for HUFF."""
    def __init__(self, data):
        self.data, self.at, self.value, self.count = data, 0, 0, 0

    def peek(self, size):
        while self.count < size and self.at < len(self.data):
            self.value = (self.value << 8) | self.data[self.at]
            self.count += 8
            self.at += 1
        if self.count < size:
            return self.value << (size - self.count)
        return self.value >> (self.count - size)

    def get(self, size):
        result = self.peek(size)
        if self.count < size:
            raise ValueError("truncated bitstream")
        self.count -= size
        self.value &= (1 << self.count) - 1
        return result

    def number(self):
        width = 2
        while not self.get(1):
            width += 1
            if width > 30:
                raise ValueError("unbounded variable number")
        return self.get(width) + (1 << width) - 4


def huff_decompress(data, max_output_bytes=64 << 20):
    """HUFF (EA's canonical Huffman packing, 'HUFF' 01 10; its stored size excludes the 16-byte header, unlike JDLZ):
    the code table, the symbol order, the coded bytes with a clue symbol for runs and literals, and the optional
    delta passes. Every size and table is bounded; anything malformed raises ValueError."""
    if len(data) < 16 or data[:6] != b"HUFF\x01\x10":
        raise ValueError("unsupported HUFF header")
    expected, compressed = struct.unpack_from("<II", data, 8)
    if not 0 < expected <= max_output_bytes or not 0 < compressed <= len(data) - 16:
        raise ValueError("invalid bounded sizes")
    bits = Bits(data[16:16 + compressed])
    kind = bits.get(16)
    if kind & 0x100:
        bits.get(32 if kind & 0x8000 else 24)
    kind &= ~0x100
    if kind not in (0x30FB, 0x32FB, 0x34FB, 0xB0FB, 0xB2FB, 0xB4FB):
        raise ValueError(f"unsupported coding {kind:04x}")
    count = bits.get(32 if kind & 0x8000 else 24)
    if count != expected:
        raise ValueError("inner/outer output-size mismatch")
    clue = bits.get(8)
    entries, base, total = [], 0, 0
    for width in range(1, 17):
        base <<= 1
        n = bits.number()
        if total + n > 256 or base + n > 1 << width:
            raise ValueError("invalid canonical code table")
        entries.append((width, base, n))
        base += n
        total += n
        if base == 1 << width:
            break
    else:
        raise ValueError("incomplete canonical code table")
    used, symbols, current = set(), [], 255
    for _ in range(total):
        skip = bits.number() + 1
        if skip > 256 - len(used):
            raise ValueError("invalid symbol permutation")
        while skip:
            current = (current + 1) & 255
            if current not in used:
                skip -= 1
        used.add(current)
        symbols.append(current)
    quick = [None] * 256
    full, cursor = {}, 0
    for width, start, n in entries:
        for code in range(start, start + n):
            symbol = symbols[cursor]
            cursor += 1
            full[width, code] = symbol
            if width <= 8:
                for i in range(code << (8 - width), (code + 1) << (8 - width)):
                    quick[i] = width, symbol
    output = bytearray()
    while True:
        entry = quick[bits.peek(8)]
        if entry is None:
            for width in range(9, 17):
                symbol = full.get((width, bits.peek(width)))
                if symbol is not None:
                    bits.get(width)
                    break
            else:
                raise ValueError("unassigned code")
        else:
            width, symbol = entry
            bits.get(width)
        if symbol != clue:
            output.append(symbol)
        else:
            run = bits.number()
            if run:
                if not output or len(output) + run > expected:
                    raise ValueError("invalid repeat")
                output.extend(bytes([output[-1]]) * run)
            elif bits.get(1):
                break
            else:
                output.append(bits.get(8))
        if len(output) > expected:
            raise ValueError("output overrun")
    if len(output) != expected:
        raise ValueError("early terminator")
    if kind in (0x32FB, 0xB2FB, 0x34FB, 0xB4FB):
        value = second = 0
        for i, delta in enumerate(output):
            value = (value + delta) & 255
            second = (second + value) & 255
            output[i] = second if kind in (0x34FB, 0xB4FB) else value
    return bytes(output)



def unpack_block(blk):
    """One packed block (JDLZ or HUFF, 16-byte header)."""
    if blk[:4] == b"JDLZ":
        return jdlz_decompress(blk)
    if blk[:4] == b"HUFF":
        return huff_decompress(blk)
    if blk[:4] == b"RAWW":
        return blk[16:16 + struct.unpack_from("<I", blk, 8)[0]]
    raise ValueError(f"unknown block codec {blk[:4]!r}")


def load(path):
    """A chunk file's bytes, decompressed if it is JDLZ."""
    data = Path(path).read_bytes()
    return jdlz_decompress(data) if data[:4] == b"JDLZ" else data


def chunks(d, start=0, end=None):
    """(id, payload offset, payload size) for each chunk in d[start:end]."""
    end = len(d) if end is None else end
    o = start
    while o + 8 <= end:
        cid, size = struct.unpack_from("<II", d, o)
        yield cid, o + 8, size
        o += 8 + size


def padding(position, align=ALIGN):
    """A filler chunk (id 0) that brings `position` to the next `align` boundary (nothing if already there)."""
    gap = -position % align
    if gap == 0:
        return b""
    if gap < 8:
        gap += align
    return struct.pack("<II", 0, gap - 8) + bytes(gap - 8)


class Texture:
    """One texture: its header (0x58 bytes + name, padded to 4), format entry (0x18) and top-to-bottom data."""

    def __init__(self, header, fmt, data):
        self.header, self.fmt, self.data = bytearray(header), bytearray(fmt), data

    @property
    def name(self):
        n = self.header[0x58]
        return self.header[0x59:0x59 + n].split(b"\0")[0].decode("latin-1")

    @property
    def key(self):
        return struct.unpack_from("<I", self.header, 12)[0]

    @property
    def size(self):
        return struct.unpack_from("<hh", self.header, 40)


class Tpk:
    def __init__(self, d, o, size):
        self.info_header = self.anim = None
        self.extra_info = []  # other chunks in the info block, kept as they are
        keys = headers = formats = None
        data_header = data = None
        for cid, p, sz in chunks(d, o, o + size):
            if cid == TPK_INFO:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == INFO_HEADER:
                        self.info_header = d[p2:p2 + s2]
                    elif c2 == INFO_KEYS:
                        keys = d[p2:p2 + s2]
                    elif c2 == INFO_TEXTURES:
                        headers = (p2, s2)
                    elif c2 == INFO_FORMATS:
                        formats = (p2, s2)
                    else:
                        self.extra_info.append((c2, d[p2:p2 + s2]))
            elif cid == TPK_DATA:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == DATA_HEADER:
                        data_header = d[p2:p2 + s2]
                    elif c2 == DATA_PIXELS:
                        data = (p2 + ALIGN - 1) & ~(ALIGN - 1)
        if self.info_header is None or headers is None or formats is None or data is None:
            raise ValueError("not a complete TPK")
        self.data_header = data_header
        self.textures = []
        p, end, i = headers[0], headers[0] + headers[1], 0
        while p + 0x59 <= end:
            n = d[p + 0x58]
            h = d[p:p + 0x59 + n]
            off, sz = struct.unpack_from("<I", h, 20)[0], struct.unpack_from("<I", h, 28)[0]
            self.textures.append(Texture(h, d[formats[0] + 0x18 * i:formats[0] + 0x18 * (i + 1)],
                                         d[data + off:data + off + sz]))
            p += 0x59 + n
            i += 1

    @property
    def collection(self):
        return self.info_header[4:4 + 0x1C].split(b"\0")[0].decode("latin-1")

    def find(self, name):
        return next((t for t in self.textures if t.name.lower() == name.lower()), None)

    def assemble(self, position):
        """The TPK chunk's bytes, written at file offset `position` (alignment is absolute)."""
        out = bytearray()

        def chunk(cid, payload):
            out.extend(struct.pack("<II", cid, len(payload)))
            out.extend(payload)

        # Data offsets: each texture's data starts on a 0x80 boundary relative to the pixel base.
        offset = 0
        for t in self.textures:
            struct.pack_into("<I", t.header, 20, offset)
            struct.pack_into("<I", t.header, 28, len(t.data))
            offset += len(t.data) + (-len(t.data) % ALIGN)
        info = bytearray()
        for cid, payload in [(INFO_HEADER, self.info_header),
                             (INFO_KEYS, b"".join(struct.pack("<II", t.key, 0) for t in self.textures)),
                             (INFO_TEXTURES, b"".join(t.header for t in self.textures)),
                             (INFO_FORMATS, b"".join(t.fmt for t in self.textures))] + self.extra_info:
            info += struct.pack("<II", cid, len(payload)) + payload
        body = bytearray(struct.pack("<II", 0, 0x30) + bytes(0x30))
        body += struct.pack("<II", TPK_INFO, len(info)) + info
        body += padding(position + 8 + len(body))
        data = bytearray(struct.pack("<II", DATA_HEADER, len(self.data_header)) + self.data_header)
        data += struct.pack("<II", 0, 0x50) + bytes(0x50)
        pixels = bytearray(struct.pack("<I", 0x11111111) * 30)
        for t in self.textures:
            pixels += t.data + bytes(-len(t.data) % ALIGN)
        data += struct.pack("<II", DATA_PIXELS, len(pixels)) + pixels
        body += struct.pack("<II", TPK_DATA, len(data)) + data
        chunk(TPK, body)
        return bytes(out)


class CompressedTpk:
    """A TPK whose textures are each packed in parts (the 33310003 rows; DYNTEX.BIN, WMDYNTEX.BIN, the minimaps,
    TRACKMAPS.BIN). A row is {name hash, offset from the TPK chunk, stored size, unpacked size, user flags, flags
    (2 = by parts), ref count, 0}. A texture's parts are {0x55441122, unpacked size, stored size (to the next part),
    unpacked position, stored position, 0} + a JDLZ/HUFF/RAWW block; unpacked, they are its pixels followed by its
    0x7C-byte header and 0x18-byte format entry. Unchanged textures are written back as they were; changed or new
    ones JDLZ-packed in 32 KB parts like the originals (jdlz.c), or plain (row flags 0: the stored bytes are the
    unpacked texture) when that does not work. The game reads a by-parts texture into a buffer of its unpacked
    size and unpacks it there in place (0055b8d0, 006aaba0), so stored parts must never be bigger than what they
    unpack to (uncompressed RAWW parts are, and overflowed that buffer) - see fits_in_place."""

    def __init__(self, d, o, size):
        self.base = o - 8
        self.info_header = self.data_header = None
        self.extra_info, rows = [], None
        for cid, p, sz in chunks(d, o, o + size):
            if cid == TPK_INFO:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == INFO_HEADER:
                        self.info_header = d[p2:p2 + s2]
                    elif c2 == INFO_SLOTS:
                        rows = (p2, s2)
                    elif c2 != INFO_KEYS:
                        self.extra_info.append((c2, d[p2:p2 + s2]))
            elif cid == TPK_DATA:
                for c2, p2, s2 in chunks(d, p, p + sz):
                    if c2 == DATA_HEADER:
                        self.data_header = d[p2:p2 + s2]
        if self.info_header is None or rows is None or self.data_header is None:
            raise ValueError("not a compressed TPK")
        self.textures = []
        for i in range(rows[1] // 24):
            key, off, stored, unpacked = struct.unpack_from("<4I", d, rows[0] + 24 * i)
            flags = d[rows[0] + 24 * i + 16:rows[0] + 24 * i + 24]
            raw = d[self.base + off:self.base + off + stored]
            parts, stored, pos = [], [], 0
            if flags[1] == 0:   # plain
                parts, pos = [(0, raw[:unpacked])], len(raw)
            while pos < len(raw):
                if struct.unpack_from("<I", raw, pos)[0] != PART_MAGIC:
                    pos += 4
                    continue
                _, out_size, step, out_pos, _, _ = struct.unpack_from("<6I", raw, pos)
                blk = raw[pos + 24:]
                n = struct.unpack_from("<I", blk, 12)[0] + (0 if blk[4] == 2 else 16)
                parts.append((out_pos, unpack_block(blk[:n])))
                stored.append((out_pos, out_size, raw[pos:pos + step]))
                pos += step
            blob = b"".join(b for _, b in sorted(parts, key=lambda x: x[0]))
            if len(blob) != unpacked or len(blob) < 0x94:
                raise ValueError(f"texture {key:08x}: unpacked {len(blob)} bytes, expected {unpacked}")
            t = Texture(blob[-0x94:-0x18], blob[-0x18:], blob[:-0x94])
            t.raw, t.flags, t.offset, t.stored = raw, flags, off, stored
            t.original = (bytes(t.header), bytes(t.fmt), t.data)
            self.textures.append(t)

    @property
    def collection(self):
        return self.info_header[4:4 + 0x1C].split(b"\0")[0].decode("latin-1")

    PART = 0x8000

    def _packed(self, t):
        """A changed texture packed the way the originals are: JDLZ parts of 32 KB unpacked (RAWW where a piece
        does not shrink), the header in its own part, part 0 last. None if it would not unpack in place."""
        blob = t.data + bytes(t.header[:0x7C].ljust(0x7C, b"\0")) + bytes(t.fmt)
        pieces = [(i, t.data[i:i + self.PART]) for i in range(self.PART, len(t.data), self.PART)]
        pieces += [(len(t.data), blob[len(t.data):]), (0, t.data[:self.PART])]
        parts = []
        for pos, piece in pieces:
            block = jdlz_compress(piece, len(piece))
            if block is None:
                parts.append((pos, self._raww_part(piece, pos)))
            else:
                block += bytes(-len(block) % 4)
                parts.append((pos, struct.pack("<6I", PART_MAGIC, len(piece), 24 + len(block), pos, 0, 0) + block))
        packed = self._restamp(parts)
        return packed if self.fits_in_place(packed, len(blob)) else None

    @staticmethod
    def _raww_part(piece, out_pos):
        block = b"RAWW\x01\x10\0\0" + struct.pack("<II", len(piece), len(piece)) + piece
        block += bytes(-len(block) % 4)
        return struct.pack("<6I", PART_MAGIC, len(piece), 24 + len(block), out_pos, 0, 0) + block

    def _with_header(self, t):
        """An unchanged texture whose header moved: its stored parts as they were, except the one holding the
        header (the last unpacked bytes), rewritten as RAWW with the new header. None if the result would not
        unpack in place (see fits_in_place)."""
        if not t.stored:
            return None
        last = max(t.stored, key=lambda x: x[0])
        head = (t.data + bytes(t.header[:0x7C].ljust(0x7C, b"\0")) + bytes(t.fmt))[last[0]:]
        parts = [(pos, raw if (pos, size, raw) != last else self._raww_part(head, pos)) for pos, size, raw in t.stored]
        blob = self._restamp(parts)
        return blob if self.fits_in_place(blob, len(t.data) + 0x94) else None

    @staticmethod
    def fits_in_place(blob, unpacked):
        """006aaba0's in-place unpack works: the stored parts fit the unpacked-size buffer, and unpacking them
        backwards (the last stored part, part 0, from a copy) never writes over a part not yet unpacked."""
        if len(blob) > unpacked:
            return False
        pos, parts = 0, []
        while pos < len(blob):
            _, size, step, out_pos = struct.unpack_from("<4I", blob, pos)
            parts.append((pos, step, out_pos, size))
            pos += step
        return all(out_pos >= at + step and out_pos + size <= unpacked for at, step, out_pos, size in parts[:-1])

    @staticmethod
    def _restamp(parts):
        """Parts in their stored order with their stored positions (+16) recomputed."""
        out = bytearray()
        for _, raw in parts:
            raw = bytearray(raw)
            struct.pack_into("<I", raw, 16, len(out))
            out += raw
        return bytes(out)

    def assemble(self, position):
        changed = [t for t in self.textures if getattr(t, "raw", None) is None or
                   (bytes(t.header), bytes(t.fmt), t.data) != t.original]
        # The game unpacks a whole pack into one buffer: each texture at its header's offset, the offsets running
        # on in stored order, 0x80-aligned (every retail file follows this). New textures go last.
        order = sorted([t for t in self.textures if getattr(t, "raw", None) is not None], key=lambda t: t.offset)
        order += [t for t in self.textures if getattr(t, "raw", None) is None]
        offset, moved = 0, []
        for t in order:
            if struct.unpack_from("<I", t.header, 20)[0] != offset and t not in changed:
                moved.append(t)
            struct.pack_into("<iiii", t.header, 20, offset, -1, len(t.data), 0)
            offset += len(t.data) + (-len(t.data) % 0x80)
        blobs, plain = {}, set()
        for t in self.textures:
            if t in moved:
                blobs[id(t)] = self._with_header(t)
            elif t not in changed:
                blobs[id(t)] = t.raw
        todo = [t for t in self.textures if blobs.get(id(t)) is None]
        with ThreadPoolExecutor(os.cpu_count() or 4) as pool:   # ctypes lets the compressor run in parallel
            for t, packed in zip(todo, pool.map(self._packed, todo)):
                blobs[id(t)] = packed
        for t in self.textures:
            if blobs.get(id(t)) is None:   # plain: the unpacked bytes themselves
                plain.add(id(t))
                blobs[id(t)] = t.data + bytes(t.header[:0x7C].ljust(0x7C, b"\0")) + bytes(t.fmt)
        self.textures.sort(key=lambda t: t.key)
        info = bytearray()
        for cid, payload in [(INFO_HEADER, self.info_header),
                             (INFO_KEYS, b"".join(struct.pack("<II", t.key, 0) for t in self.textures)),
                             (INFO_SLOTS, None)] + self.extra_info:
            if cid == INFO_SLOTS:
                slots_at = len(info) + 8
                payload = bytes(24 * len(self.textures))
            info += struct.pack("<II", cid, len(payload)) + payload
        body = bytearray(struct.pack("<II", 0, 0x30) + bytes(0x30))
        info_at = len(body) + 8
        body += struct.pack("<II", TPK_INFO, len(info)) + info
        body += padding(position + 8 + len(body))
        data = bytearray(struct.pack("<II", DATA_HEADER, len(self.data_header)) + self.data_header)
        data += struct.pack("<II", 0, 0x50) + bytes(0x50)
        pixels_at = 8 + len(body) + 8 + len(data) + 8   # the pixel payload, from the TPK chunk start
        pixels = bytearray(struct.pack("<I", 0x11111111) * 30)
        where = {}
        for t in order:
            pixels += bytes(-(pixels_at + len(pixels)) % 0x40)
            where[id(t)] = pixels_at + len(pixels)
            pixels += blobs[id(t)]
        pixels += bytes(-(pixels_at + len(pixels)) % 0x40)
        data += struct.pack("<II", DATA_PIXELS, len(pixels)) + pixels
        body += struct.pack("<II", TPK_DATA, len(data)) + data
        for i, t in enumerate(self.textures):
            row = struct.pack("<4I", t.key, where[id(t)], len(blobs[id(t)]), len(t.data) + 0x94)
            flags = bytearray(t.flags if getattr(t, "raw", None) is not None else bytes(8))
            flags[1] = 0 if id(t) in plain else 2
            row += bytes(flags)
            body[info_at + slots_at + 24 * i:info_at + slots_at + 24 * (i + 1)] = row
        return struct.pack("<II", TPK, len(body)) + bytes(body)


class ChunkFile:
    """A chunk file: top-level chunks kept as bytes, TPKs parsed. Filler chunks (id 0) are rebuilt on write
    so every other chunk keeps the start alignment (mod 0x80) it had."""

    def __init__(self, data):
        self.items = []  # (original offset % ALIGN, bytes | Tpk)
        for cid, p, size in chunks(data):
            if cid == 0:
                continue
            raw = data[p - 8:p + size]
            item = raw
            if cid == TPK:
                for kind in (Tpk, CompressedTpk):
                    try:
                        item = kind(data, p, size)
                        break
                    except ValueError:
                        item = raw
            self.items.append(((p - 8) % ALIGN, item))

    def tpks(self):
        return [item for _, item in self.items if isinstance(item, (Tpk, CompressedTpk))]

    def tpk(self, collection):
        # The stored name has room for 27 characters (MINI_MAP_CASINOTOWN_UNLOCKED is kept as ..._UNLOCKE).
        want = collection.lower()[:0x1B]
        found = next((t for t in self.tpks() if t.collection.lower()[:0x1B] == want), None)
        if found is None and want.startswith("tpk") and want[3:].isdigit() and int(want[3:]) < len(self.tpks()):
            found = self.tpks()[int(want[3:])]   # Binarius's name for the n-th pack (stream sections repeat names)
        return found

    def assemble(self):
        out = bytearray()
        for phase, item in self.items:
            gap = (phase - len(out)) % ALIGN
            if gap:
                if gap < 8:
                    gap += ALIGN
                out += struct.pack("<II", 0, gap - 8) + bytes(gap - 8)
            out += item.assemble(len(out)) if isinstance(item, (Tpk, CompressedTpk)) else item
        return bytes(out)


if __name__ == "__main__":
    original = load(sys.argv[1])
    f = ChunkFile(original)
    rebuilt = f.assemble()
    print(f"{len(f.tpks())} TPKs: {', '.join(t.collection for t in f.tpks())}")
    print("identical" if rebuilt == original else f"differs: {len(original)} -> {len(rebuilt)} bytes")
