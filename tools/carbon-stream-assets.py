#!/usr/bin/env python3
"""Prepare and publish immutable R2 streaming pieces without changing game files.

Preparation is local. --upload writes only content-addressed chunks and the new
stream index. It never changes the active download manifest or deletes objects.
Credentials come from the existing Wrangler login, never from command arguments.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import tomllib
from urllib.parse import quote

BLOCK = 1024 * 1024
PIECE = 8 * BLOCK


def encoded(value):
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode()


def vehicle_key(name):
    """Carbon's AttribSys key: Jenkins lookup2 with seed 0xabcdef00.

    Folder names use uppercase; vehicle collection names use lowercase. The
    observed RX-8 and R8 network keys are d6334437 and 8774322c respectively.
    """
    data = name.lower().encode("ascii")
    mask = 0xffffffff
    def mix(a, b, c):
        for right, shift in ((True, 13), (False, 8), (True, 13), (True, 12),
                             (False, 16), (True, 5), (True, 3), (False, 10), (True, 15)):
            a = ((a - b - c) ^ ((c >> shift) if right else (c << shift))) & mask
            a, b, c = b, c, a
        return a, b, c
    a = b = 0x9e3779b9
    c = 0xabcdef00
    at = 0
    while len(data) - at >= 12:
        a = (a + int.from_bytes(data[at:at + 4], "little")) & mask
        b = (b + int.from_bytes(data[at + 4:at + 8], "little")) & mask
        c = (c + int.from_bytes(data[at + 8:at + 12], "little")) & mask
        a, b, c = mix(a, b, c)
        at += 12
    tail = data[at:]
    a = (a + int.from_bytes(tail[:4], "little")) & mask
    b = (b + int.from_bytes(tail[4:8], "little")) & mask
    c = (c + len(data) + (int.from_bytes(tail[8:], "little") << 8)) & mask
    return mix(a, b, c)[2]


class Cloudflare:
    def __init__(self, account, bucket, work):
        self.root = f"https://api.cloudflare.com/client/v4/accounts/{account}/r2/buckets/{quote(bucket, safe='')}/objects"
        self.profile = Path.home() / "Library/Preferences/.wrangler/config/default.toml"
        self.work = work
        self.lock = threading.Lock()
        self.next_request = 0.0

    def request(self, key="", data=None, content_type="application/octet-stream",
                cache_control="public, max-age=31536000, immutable"):
        url = self.root + ("/" + quote(key, safe="/") if key else "")
        for attempt in range(5):
            # Stay below the management API's request budget, including retries.
            with self.lock:
                delay = max(0, self.next_request - time.monotonic())
                self.next_request = max(time.monotonic(), self.next_request) + 0.3
            if delay:
                time.sleep(delay)
            token = tomllib.loads(self.profile.read_text())["oauth_token"]
            config = 'header = "Authorization: Bearer ' + token + '"\n'
            command = ["curl", "-sS", "--connect-timeout", "15", "--max-time", "120",
                       "--config", "-", "-w", "\n%{http_code}", url]
            with tempfile.NamedTemporaryFile(dir=self.work, prefix="upload-") as body:
                if data is not None:
                    body.write(data)
                    body.flush()
                    command += ["-X", "PUT", "--data-binary", "@" + body.name,
                                "-H", "Content-Type: " + content_type,
                                "-H", "Cache-Control: " + cache_control]
                result = subprocess.run(command, input=config, capture_output=True, text=True)
            payload, _, code = result.stdout.rpartition("\n")
            if result.returncode == 0 and code.startswith("2"):
                return json.loads(payload) if payload.strip() else {}
            if code in ("401", "403"):
                raise RuntimeError(f"Cloudflare refused access ({code}); refresh the existing Wrangler login")
            if attempt == 4:
                raise RuntimeError(f"R2 request failed ({code or result.returncode}) for {key}")
            time.sleep(2 ** attempt)

    def catalog(self):
        # R2's object-list API uses a cursor, independent of its S3 API.
        result = {}
        cursor = ""
        while True:
            original = self.root
            self.root = original + "?prefix=local-game%2F&per_page=1000" + (
                "&cursor=" + quote(cursor, safe="") if cursor else "")
            try:
                page = self.request()
            finally:
                self.root = original
            if not page.get("success"):
                raise RuntimeError("Could not list the authorized asset bucket")
            result.update((o["key"], o) for o in page["result"])
            cursor = page.get("result_info", {}).get("cursor", "")
            if not cursor:
                return result


def prepare(manifest, source, catalog):
    files, uploads = {}, {}
    for entry in manifest["files"]:
        relative = entry["path"]
        path = (source / relative).resolve()
        if not path.is_relative_to(source.resolve()) or path.stat().st_size != entry["size"]:
            raise ValueError(f"Manifest/source mismatch: {relative}")
        remote = catalog.get("local-game/files/" + relative)
        if not remote or remote["size"] != entry["size"]:
            raise ValueError(f"Published asset missing or has another size: {relative}")
        # BIN/EXE already use the CDN; the DLLs are verified local startup files.
        split = path.suffix.lower() not in (".bin", ".exe", ".dll") or entry["size"] > 512 * BLOCK
        whole = hashlib.sha256()
        blocks, pieces = [], []
        with path.open("rb") as stream:
            offset = 0
            while data := stream.read(PIECE):
                whole.update(data)
                blocks.extend(hashlib.sha256(data[i:i + BLOCK]).hexdigest()
                              for i in range(0, len(data), BLOCK))
                if split:
                    digest = hashlib.sha256(data).hexdigest()
                    pieces.append(digest)
                    uploads.setdefault(digest, {"path": relative, "offset": offset, "size": len(data),
                                                "md5": hashlib.md5(data).hexdigest()})
                offset += len(data)
        if whole.hexdigest() != remote.get("custom_metadata", {}).get("sha256"):
            raise ValueError(f"Local bytes differ from the verified published asset: {relative}")
        record = {"size": entry["size"], "etag": '"' + remote["etag"].strip('"') + '"', "blocks": blocks}
        if split:
            record["pieces"] = pieces
        files[relative] = record
    folders = sorted({path.split("/")[1] for path in files if path.startswith("CARS/")})
    models = {str(vehicle_key(folder)): "CARS/" + folder + "/" for folder in folders}
    if len(models) != len(folders):
        raise ValueError("Ambiguous vehicle key; do not preload the wrong car")
    index = {"schema": 2, "blockSize": BLOCK, "pieceSize": PIECE,
             "base": "../files/", "chunksBase": "../chunks/", "files": files, "models": models}
    return index, uploads


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--account", required=True)
    p.add_argument("--bucket", required=True)
    p.add_argument("--upload", action="store_true")
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    api = Cloudflare(args.account, args.bucket, args.output)
    catalog = api.catalog()
    (args.output / "catalog.json").write_bytes(encoded(catalog))
    index, uploads = prepare(json.loads(args.manifest.read_text()), args.source, catalog)
    body = encoded(index)
    revision = hashlib.sha256(body).hexdigest()
    (args.output / "stream-index.json").write_bytes(body)
    (args.output / "uploads.json").write_bytes(encoded(uploads))
    total = sum(o["size"] for o in uploads.values())
    print(f"Verified {len(index['files'])} source files; {len(uploads)} distinct pieces, {total} bytes", flush=True)
    print(f"Index: local-game/stream/{revision}.json", flush=True)
    if not args.upload:
        return
    pending = [(digest, o) for digest, o in uploads.items()
               if catalog.get(f"local-game/chunks/{digest}.bin", {}).get("etag") != o["md5"]]
    def put(item):
        digest, o = item
        with (args.source / o["path"]).open("rb") as stream:
            stream.seek(o["offset"])
            data = stream.read(o["size"])
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError("Source changed during upload")
        api.request(f"local-game/chunks/{digest}.bin", data)
        return o["size"]
    uploaded = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        for count, size in enumerate(pool.map(put, pending), 1):
            uploaded += size
            if count % 20 == 0 or count == len(pending):
                print(f"Uploaded {count}/{len(pending)} pieces ({uploaded // BLOCK} MiB)", flush=True)
    final = api.catalog()
    for digest, o in uploads.items():
        remote = final.get(f"local-game/chunks/{digest}.bin", {})
        if remote.get("size") != o["size"] or remote.get("etag") != o["md5"]:
            raise RuntimeError(f"Uploaded piece did not verify: {digest}")
    api.request(f"local-game/stream/{revision}.json", body, "application/json")
    print("All pieces verified; immutable index uploaded. Active download manifest unchanged.", flush=True)


if __name__ == "__main__":
    main()
