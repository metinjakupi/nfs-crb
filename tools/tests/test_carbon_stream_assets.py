import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("stream_assets", Path(__file__).parents[1] / "carbon-stream-assets.py")
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)


class StreamingAssetsTests(unittest.TestCase):
    def test_release_pieces_reconstruct_verified_bytes_and_keep_original_metadata(self):
        # Cross the on-disk piece boundary with a short tail, independently of
        # the uploader's helpers. The browser must see the exact original file.
        content = bytes(range(251)) * 34000
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            path = root / "TRACKS/world.bun"
            path.parent.mkdir()
            path.write_bytes(content)
            manifest = {"files": [{"path": "TRACKS/world.bun", "size": len(content)}]}
            catalog = {"local-game/files/TRACKS/world.bun": {"size": len(content), "etag": "original-v1",
                       "custom_metadata": {"sha256": hashlib.sha256(content).hexdigest()}}}
            index, uploads = assets.prepare(manifest, root, catalog)
            file = index["files"]["TRACKS/world.bun"]
            rebuilt = bytearray()
            for digest in file["pieces"]:
                piece = uploads[digest]
                data = path.read_bytes()[piece["offset"]:piece["offset"] + piece["size"]]
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest)
                rebuilt.extend(data)
            self.assertEqual(rebuilt, content)
            self.assertEqual(file["etag"], '"original-v1"')
            self.assertEqual(file["size"], len(content))
            path.write_bytes(b"x" + content[1:])
            with self.assertRaisesRegex(ValueError, "differ"):
                assets.prepare(manifest, root, catalog)

    def test_car_keys_match_observed_network_models(self):
        self.assertEqual(assets.vehicle_key("RX8"), 0xd6334437)
        self.assertEqual(assets.vehicle_key("R8"), 0x8774322c)


if __name__ == "__main__":
    unittest.main()
