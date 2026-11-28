import base64
import importlib.util
import io
import ipaddress
import json
from pathlib import Path
import re
import struct
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load("package_companion", ROOT / "scripts/build/package-companion.py")
connection = load("configure_connection", ROOT / "scripts/build/configure-connection.py")


class PackageCompanionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        self.rom = bytearray(0x4020)
        self.rom[0x12], self.rom[0x1C] = 2, 1
        for base in (0x1C0, 0x1D0):
            struct.pack_into("<I", self.rom, base, 0x4000)
            struct.pack_into("<I", self.rom, base + 12, 0x20)
        (self.build / "build/text").mkdir(parents=True)
        (self.build / "build/text/font-manifest.json").write_text('{"name": "DSWT Companion Pixel"}\n')

    def pack(self, version="0.4.1"):
        (self.build / "dswt.nds").write_bytes(self.rom)
        return package.package(version, self.build, self.build / "release")

    def test_zip_contains_only_homebrew_notices_and_install_text(self):
        (self.build / "private.key").write_text("must stay out")
        (self.build / "cartridge.nds").write_text("must stay out")
        target = self.pack()
        first = target.read_bytes()
        self.assertEqual(self.pack().read_bytes(), first)
        self.assertEqual(target.name, "dswt-companion-0.4.1.zip")
        with zipfile.ZipFile(target) as archive:
            prefix = "dswt-companion-0.4.1/"
            expected = {"Install-DSWT.html", "dswt.nds", "NOTICE", "LICENSE", "INSTALL.txt", "font-manifest.json",
                        *package.LICENSES}
            self.assertEqual(set(archive.namelist()), {prefix + name for name in expected})
            self.assertEqual(archive.read(prefix + "dswt.nds"), self.rom)
            self.assertEqual((target.parent / "dswt.nds").read_bytes(), self.rom)
            self.assertEqual(archive.read(prefix + "Install-DSWT.html"), target.with_suffix(".html").read_bytes())
            for name in ("NOTICE", "LICENSE", *package.LICENSES):
                self.assertEqual(archive.read(prefix + name), (ROOT / name).read_bytes())
                self.assertIn((ROOT / name).read_bytes(), (target.parent / "THIRD-PARTY-NOTICES.txt").read_bytes())
            self.assertIn(b"https://github.com/Sayarinu/DSWT/wiki/DS-Companion",
                          archive.read(prefix + "INSTALL.txt"))
            self.assertIn(b"/sys/crts", archive.read(prefix + "NOTICE"))

    def test_public_installer_contains_an_unseeded_sd_layout(self):
        installer = self.pack().with_suffix(".html").read_text()
        encoded = re.search(r"const template = '([^']+)';", installer)[1]
        kit = base64.b64decode(encoded, validate=True)
        with zipfile.ZipFile(io.BytesIO(kit)) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.read("roms/nds/dswt.nds"), self.rom)
            record = archive.getinfo("_nds/dswt/seeds.bin")
            self.assertEqual(archive.read(record), bytes(32 * 64))
            self.assertEqual(record.compress_type, zipfile.ZIP_STORED)
            self.assertEqual(record.header_offset, 0)
            offset = int(re.search(r"const seedOffset = (\d+);", installer)[1])
            central_crc = int(re.search(r"const centralCrcOffset = (\d+);", installer)[1])
            self.assertEqual(offset, 30 + len(record.filename) + len(record.extra))
            self.assertEqual(central_crc, archive.start_dir + 16)
            self.assertEqual(struct.unpack_from("<I", kit, central_crc)[0], record.CRC)

    def test_rejects_ds_only_and_missing_or_truncated_dsi_payloads(self):
        original = bytes(self.rom)
        for position, value in ((0x12, 0), (0x1C, 0), (0x1C0, 0x3000), (0x1D0, 0x4020),
                                (0x1CC, 0), (0x1DC, 0x21)):
            with self.subTest(position=position, value=value):
                self.rom = bytearray(original)
                if position < 0x20:
                    self.rom[position] = value
                else:
                    struct.pack_into("<I", self.rom, position, value)
                with self.assertRaises(ValueError):
                    self.pack()
        self.rom = b"short"
        with self.assertRaises(ValueError):
            self.pack()

    def test_rejects_invalid_version_and_missing_notice(self):
        with self.assertRaises(ValueError):
            self.pack("../elsewhere")
        (self.build / "build/text/font-manifest.json").unlink()
        with self.assertRaises(FileNotFoundError):
            self.pack()
        self.assertFalse((self.build / "release").exists())

    def test_release_profile_has_only_public_connection_fields(self):
        profile = json.loads((ROOT / "client/companion/production-connection-profile.json").read_text())
        header = connection.header(profile)  # Rejects extra fields, bad pins and identical pins.
        self.assertTrue(ipaddress.IPv4Address(profile["ipv4"]).is_global)
        self.assertFalse(profile["hostname"].endswith((".invalid", ".test", ".localhost")))
        self.assertIn(profile["hostname"], header)
        self.assertEqual(profile["port"], 443)
