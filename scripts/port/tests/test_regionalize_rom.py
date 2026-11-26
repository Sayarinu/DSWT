import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "regionalize-rom.py"
spec = importlib.util.spec_from_file_location("regionalize_rom", SCRIPT)
assert spec and spec.loader
regionalize_rom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(regionalize_rom)


def narc(members):
    """A minimal NARC: header, BTAF table, empty BTNF and GMIF image."""
    table = b"".join(struct.pack("<II", sum(map(len, members[:i])), sum(map(len, members[:i + 1])))
                     for i in range(len(members)))
    btaf = b"BTAF" + struct.pack("<IHH", 12 + len(table), len(members), 0) + table
    btnf = b"BTNF" + struct.pack("<I", 8)
    gmif = b"GMIF" + struct.pack("<I", 8 + sum(map(len, members))) + b"".join(members)
    body = btaf + btnf + gmif
    return b"NARC" + struct.pack("<HHIHH", 0xFFFE, 0x0100, 16 + len(body), 16, 3) + body


def rom(revision=0, code=b"ARM9" * 64, data=b"D" * 64, members=(b"M0" * 8, b"M1" * 8)):
    """A tiny cartridge: header, ARM9, ARM7, one overlay and two named files."""
    image = bytearray(0x8000)
    image[0x0C:0x10] = b"IPKE"
    image[0x1E] = revision
    files = [b"OVERLAY!" * 8, data, narc(list(members))]
    layout, cursor = [], 0x5000
    for content in files:
        layout.append((cursor, cursor + len(content)))
        image[cursor:cursor + len(content)] = content
        cursor += (len(content) + 0x1FF) & ~0x1FF
    image[0x4000:0x4100] = code
    image[0x4100:0x4200] = b"ARM7" * 64
    names = b"\x05a.bin" + b"\x06m.narc" + b"\x00"
    image[0x4200:0x4208] = struct.pack("<IHH", 8, 1, 1)
    image[0x4208:0x4208 + len(names)] = names
    for index, (start, end) in enumerate(layout):
        struct.pack_into("<II", image, 0x4300 + 8 * index, start, end)
    struct.pack_into("<8I", image, 0x4400, 0, 0, 0, 0, 0, 0, 0, 0)
    struct.pack_into("<IIII", image, 0x20, 0x4000, 0, 0, 0x100)
    struct.pack_into("<IIII", image, 0x30, 0x4100, 0, 0, 0x100)
    struct.pack_into("<IIII", image, 0x40, 0x4200, 0x100, 0x4300, 8 * len(layout))
    struct.pack_into("<II", image, 0x50, 0x4400, 32)
    struct.pack_into("<H", image, 0x15E, regionalize_rom.crc16(image[:0x15E]))
    return bytes(image)


class RegionalizeTests(unittest.TestCase):
    def setUp(self):
        self.us = rom()
        self.region = rom(revision=10, data=b"E" * 64, members=(b"E0" * 8, b"M1" * 8))

    def test_us_dump_becomes_the_regional_dump(self):
        self.assertEqual(regionalize_rom.regionalize(self.us, self.us, self.region), self.region)

    def test_build_changes_are_kept_beside_regional_members(self):
        build = rom(members=(b"M0" * 8, b"WT" * 8))  # The build changed member 1 only.
        result = regionalize_rom.regionalize(build, self.us, self.region)
        self.assertEqual(result, rom(revision=10, data=b"E" * 64, members=(b"E0" * 8, b"WT" * 8)))

    def test_conflicts_and_other_code_are_refused(self):
        cases = {
            "same file": (rom(data=b"W" * 64), "both change /a.bin"),
            "same member": (rom(members=(b"W0" * 8, b"M1" * 8)), "both change member 0"),
            "header": (rom(revision=1), "header byte 0x1e"),
        }
        for name, (build, message) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(regionalize_rom.RegionError, message):
                regionalize_rom.regionalize(build, self.us, self.region)
        with self.assertRaisesRegex(regionalize_rom.RegionError, "different code"):
            regionalize_rom.regionalize(self.us, self.us, rom(revision=10, code=b"FRA9" * 64))

    def test_cli_requires_catalogued_dumps_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            for name in ("us.nds", "eu.nds", "build.nds"):
                (temp / name).write_bytes(self.us)
            args = ["--us", str(temp / "us.nds"), "--region", str(temp / "eu.nds"), str(temp / "build.nds")]
            self.assertEqual(regionalize_rom.main(["--variant", "heartgold-europe", *args, str(temp / "out.nds")]), 1)
            self.assertEqual(regionalize_rom.main(["--variant", "heartgold-us-rev0", *args, str(temp / "out.nds")]), 1)
            self.assertFalse((temp / "out.nds").exists())


if __name__ == "__main__":
    unittest.main()
