import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
import bundle
from support import blz, number, source_read, target_read, with_overlay

TOOL = Path(__file__).resolve().parents[1]
CHUNK = 1024 * 1024
# Patch in a child and report its peak resident memory in bytes: Linux counts KiB, macOS bytes.
CHILD = """
import resource, sys
sys.path.insert(0, sys.argv[1])
import bundle
bundle.patch_file(sys.argv[2], sys.argv[3], sys.argv[4])
peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
print(peak if sys.platform == "darwin" else peak * 1024)
"""


class Digest:
    def __init__(self):
        self.size, self.sha, self.crc = 0, hashlib.sha256(), 0

    def update(self, data):
        self.size += len(data)
        self.sha.update(data)
        self.crc = zlib.crc32(data, self.crc)

    def record(self):
        return {"size": self.size, "sha256": self.sha.hexdigest(), "crc32": f"{self.crc:08x}"}


class MemoryTests(unittest.TestCase):
    def test_largest_cartridge_patches_within_two_rom_copies(self):
        # A trimmed dump of a 512 MiB Black 2 cartridge, using what the retail one uses, with
        # a compressed overlay: the patch restores the padding and appends expanded code first.
        size, used = bps.MAX_FILE, 293804544
        code = bytes(range(256)) * 64 + bytes(4096)
        first = bytearray(with_overlay(bytes(CHUNK), blz(code, 4096)))
        first[0x0C:0x10], first[0x14], first[0x1E] = b"IREO", 12, 0
        first[0x80:0x84] = used.to_bytes(4, "little")
        body, padding = os.urandom(CHUNK), b"\xff" * CHUNK
        change = os.urandom(0x100)
        rom, target = Digest(), Digest()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with open(root / "rom.nds", "wb") as handle:
                for start in range(0, size, CHUNK):
                    kept = min(max(used - start, 0), CHUNK)
                    chunk = (bytes(first) if start == 0 else body)[:kept] + padding[kept:]
                    handle.write(chunk[:kept])
                    rom.update(chunk)
                    target.update(chunk[:0x200] + change + chunk[0x300:] if start == 0 else chunk)
            source_crc = zlib.crc32(code, rom.crc)
            patch = (b"BPS1" + number(size + len(code)) + number(size) + number(0)
                     + source_read(0x200) + target_read(change) + source_read(size - 0x300)
                     + source_crc.to_bytes(4, "little") + target.crc.to_bytes(4, "little"))
            patch += zlib.crc32(patch).to_bytes(4, "little")
            directory = root / "bundle"
            directory.mkdir()
            (directory / "large.bps").write_bytes(patch)
            (directory / "manifest.json").write_text(json.dumps({
                "schema_version": bundle.SCHEMA_VERSION, "kind": bundle.KIND, "version": "memory", "entries": [{
                    "variant": "large", "name": "Large", "game_code": "IREO", "revision": 0, "language": "English",
                    "input": rom.record(), "patch": {"file": "large.bps", "size": len(patch),
                              "sha256": bundle.sha256(patch), "crc32": bundle.crc32_hex(patch)},
                    "output": target.record()}]}))
            result = subprocess.run([sys.executable, "-c", CHILD, str(TOOL), str(directory), str(root / "rom.nds"),
                                     str(root / "out.nds")], capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stderr[-2000:])
            written = Digest()
            with open(root / "out.nds", "rb") as handle:
                while chunk := handle.read(CHUNK * 16):
                    written.update(chunk)
            self.assertEqual(written.record(), target.record())
        peak = int(result.stdout)
        self.assertLess(peak, 2 * size + 64 * CHUNK, f"patching peaked at {peak / CHUNK:.0f} MiB")


if __name__ == "__main__":
    unittest.main()
