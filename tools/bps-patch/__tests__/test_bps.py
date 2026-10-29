import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
from support import assemble, number, recrc, source_copy, source_read, target_copy, target_read


SOURCE = bytes(range(64))
TARGET = SOURCE[:16] + b"NEW!" + SOURCE[20:40] + SOURCE[40:44] * 6


def valid():
    return assemble(SOURCE, TARGET, [source_read(16), target_read(b"NEW!"), source_copy(20, 20),
                                     source_read(4), target_copy(20, 40)])


class WalkTests(unittest.TestCase):
    def test_inventories_actions_and_literals(self):
        report = bps.walk(valid())
        self.assertEqual(report["actions"], {"source_read": 2, "target_read": 1, "source_copy": 1, "target_copy": 1})
        self.assertEqual(report["source"], {"size": 64, "crc32": f"{bps.crc32(SOURCE):08x}"})
        self.assertEqual(report["target"]["size"], len(TARGET))
        [literal] = report["literals"]["items"]
        self.assertEqual((literal["target_offset"], literal["length"]), (16, 4))
        self.assertEqual(literal["sha256"], hashlib.sha256(b"NEW!").hexdigest())
        self.assertEqual(valid()[literal["patch_offset"]:literal["patch_offset"] + 4], b"NEW!")

    def test_numbers_are_bijective(self):
        for value in (0, 1, 127, 128, 16511, 16512, 2**32, 2**56):
            self.assertEqual(bps._number(number(value), 0, 20), (value, len(number(value))))

    def test_metadata_is_reported(self):
        report = bps.walk(assemble(b"", b"x", [target_read(b"x")], metadata=b"meta"))
        self.assertEqual(report["metadata"], {"offset": 7, "size": 4, "sha256": hashlib.sha256(b"meta").hexdigest()})

    def test_rejects_framing_and_checksums(self):
        patch = valid()
        cases = {
            "short": patch[:18],
            "magic": recrc(b"UPS1" + patch[4:]),
            "patch crc": patch[:-1] + bytes([patch[-1] ^ 1]),
            "oversized": b"BPS1" + bytes(bps.MAX_PATCH),
        }
        for name, data in cases.items():
            with self.subTest(name), self.assertRaises(bps.BPSError):
                bps.walk(data)

    def test_rejects_crc_valid_invalid_actions(self):
        cases = {
            "source read past source": [source_read(64), target_read(b"x" * 4)],
            "write past target": [source_read(16), target_read(b"x" * 100)],
            "source copy before start": [source_copy(4, -1)],
            "source copy past end": [source_copy(4, 61)],
            "target copy unwritten": [source_read(4), target_copy(4, 4)],
            "target copy before start": [source_read(4), target_copy(4, -1)],
            "short output": [source_read(16)],
        }
        for name, actions in cases.items():
            with self.subTest(name), self.assertRaises(bps.BPSError):
                bps.walk(assemble(SOURCE, TARGET, actions, target_size=20))

    def test_rejects_overruns_and_bounds(self):
        overrun = assemble(SOURCE, b"abcd", [number(3 << 2 | 1) + b"ab"])
        cases = {
            "literal into footer": overrun,
            "truncated number": recrc(b"BPS1" + bytes([0x00] * 3) + bytes(12)),
            "long number": recrc(b"BPS1" + bytes(11) + bytes(12)),
            "huge target": assemble(b"", b"", [], target_size=bps.MAX_FILE + 1),
            "empty target": assemble(b"", b"", []),
            "huge metadata": recrc(b"BPS1" + number(0) + number(1) + number(bps.MAX_METADATA + 1) + bytes(20)),
        }
        for name, data in cases.items():
            with self.subTest(name), self.assertRaises(bps.BPSError):
                bps.walk(data)


if __name__ == "__main__":
    unittest.main()
