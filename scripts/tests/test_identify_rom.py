import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "identify-rom.py"
spec = importlib.util.spec_from_file_location("identify_rom", SCRIPT)
identify_rom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identify_rom)


def cartridge(code, revision, size=4096, used=4096 - 0x100):
    """A synthetic ROM-shaped input: a header, a made-up body and erased-flash padding."""
    data = bytearray(hashlib.sha256(code.encode()).digest() * (size // 32))
    data[0x0C:0x10] = code.encode()
    data[0x1E] = revision
    data[0x80:0x84] = used.to_bytes(4, "little")
    data[used:] = b"\xff" * (size - used)
    return bytes(data)


class IdentifyRomTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dswt-rom-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "arbitrary-name.nds"
        self.payload = b"synthetic input, no game data\n"
        self.path.write_bytes(self.payload)
        self.variant = {
            "id": "fixture", "game": "Pokémon Platinum", "name": "Fixture", "game_code": "CPUE", "revision": 0,
            "size": len(self.payload), "sha256": hashlib.sha256(self.payload).hexdigest(),
            "pristine_verified": True, "planned_patch": "dswt-fixture.bps",
        }
        self.catalog = {"variants": [self.variant]}

    def test_full_hash_selects_input_independently_of_filename(self):
        self.assertEqual(identify_rom.identify(self.path, self.catalog), self.variant)
        self.assertEqual(self.path.read_bytes(), self.payload)

    def test_modified_truncated_and_extended_inputs_are_rejected(self):
        for payload in (self.payload[:-1] + b"!", self.payload[:-1], self.payload + b"!"):
            with self.subTest(payload=payload):
                self.path.write_bytes(payload)
                with self.assertRaisesRegex(ValueError, "Unrecognized"):
                    identify_rom.identify(self.path, self.catalog)
                self.assertEqual(self.path.read_bytes(), payload)

    def test_reference_only_base_is_rejected(self):
        self.variant["pristine_verified"] = False
        with self.assertRaisesRegex(ValueError, "Recognized Pokémon Platinum Fixture, but it is not supported yet"):
            identify_rom.identify(self.path, self.catalog)

    def add(self, variant_id, game, code, revision, data, **fields):
        variant = {"id": variant_id, "game": game, "name": variant_id, "game_code": code, "revision": revision,
                   "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                   "pristine_verified": True, "planned_patch": None}
        variant.update(fields)
        self.catalog["variants"].append(variant)
        return variant

    def test_trimmed_dumps_are_restored_to_each_games_own_size(self):
        diamond = cartridge("ADAE", 5, size=2048, used=1024)
        platinum = cartridge("CPUE", 1, size=4096, used=1024)
        self.add("diamond", "Pokémon Diamond", "ADAE", 5, diamond)
        built = self.add("platinum", "Pokémon Platinum", "CPUE", 1, platinum, planned_patch="dswt-platinum.bps")
        for name, data in (("full", platinum), ("trimmed", platinum[:1024]), ("partly trimmed", platinum[:3000])):
            with self.subTest(name):
                self.path.write_bytes(data)
                self.assertEqual(identify_rom.identify(self.path, self.catalog), built)
        for data in (diamond, diamond[:1024], diamond[:1500]):
            self.path.write_bytes(data)
            with self.assertRaisesRegex(ValueError, "Recognized Pokémon Diamond diamond, but it is not supported yet"):
                identify_rom.identify(self.path, self.catalog)
        self.path.write_bytes(platinum[:1023])
        with self.assertRaisesRegex(ValueError, r"Unrecognized Pokémon Platinum \(North America, English, Rev 1\) dump"):
            identify_rom.identify(self.path, self.catalog)

    def test_refusals_name_the_game(self):
        self.add("pearl", "Pokémon Pearl", "APAE", 5, cartridge("APAE", 5, size=2048))
        cases = {
            "modified": (cartridge("APAE", 5, size=2048)[:-1] + b"\0", r"Unrecognized Pokémon Pearl \(North America"),
            "other revision": (cartridge("APAE", 3), r"Pokémon Pearl \(North America, English, Rev 3\) is not supported yet"),
            "other language": (cartridge("IPKJ", 0), r"Pokémon HeartGold \(Japan, Japanese, Rev 0\) is not supported yet"),
            "korean": (cartridge("IPGK", 0), r"Pokémon SoulSilver \(Korea, Korean, Rev 0\) is not supported yet"),
            "european": (cartridge("ADAP", 0), r"Pokémon Diamond \(Europe, English, Rev 0\) is not supported yet"),
            "other game": (cartridge("IRBO", 0), "Unrecognized input"),
        }
        for name, (data, message) in cases.items():
            with self.subTest(name):
                self.path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, message):
                    identify_rom.identify(self.path, self.catalog)
                self.assertEqual(self.path.read_bytes(), data)

    def test_ambiguous_catalog_is_rejected(self):
        self.catalog["variants"].append(self.variant.copy())
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            identify_rom.identify(self.path, self.catalog)

    def test_cli_failure_leaves_input_and_directory_unchanged(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Unrecognized", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(self.path.read_bytes(), self.payload)
        self.assertEqual(list(self.path.parent.iterdir()), [self.path])

    def test_catalog_has_unique_targets_and_matches_upstream_baselines(self):
        catalog = json.loads(identify_rom.CATALOG.read_text())
        self.assertEqual(catalog["schema_version"], identify_rom.SCHEMA_VERSION)
        variants = catalog["variants"]
        self.assertEqual([row["id"] for row in variants], [
            "us-rev0", "us-rev1", "europe", "diamond-us-rev5", "pearl-us-rev5", "heartgold-us-rev0", "soulsilver-us-rev0"])
        for key in ("id", "sha256", "sha1", "crc32"):
            self.assertEqual(len({row[key] for row in variants}), len(variants))
        patches = [row["planned_patch"] for row in variants if row["planned_patch"] is not None]
        self.assertEqual(len(set(patches)), len(patches))
        for row in variants:
            game, size = identify_rom.bundle.GAMES[row["game_code"][:3]]
            self.assertEqual(identify_rom.bundle.CARTRIDGES[row["game_code"]][0], row["game"])
            self.assertEqual(identify_rom.bundle.CARTRIDGES[row["game_code"]][2], row["language"])
            self.assertEqual((row["game"], row["size"]), (game, size))
            self.assertIs(type(row["revision"]), int)
            for key, length in (("sha256", 64), ("sha1", 40), ("crc32", 8)):
                self.assertRegex(row[key], f"^[0-9a-f]{{{length}}}$")
            if row["planned_patch"] is not None:
                self.assertRegex(row["planned_patch"], r"^dswt-[a-z0-9-]+\.bps$")
        self.assertEqual({row["size"] for row in variants if row["game_code"][:3] in ("ADA", "APA")}, {67108864})
        pin = json.loads((SCRIPT.parent.parent / "client/upstream.json").read_text())
        built = [row for row in variants if row["pristine_verified"] and row["planned_patch"] is not None]
        self.assertEqual([row["id"] for row in built], ["us-rev0", "us-rev1", "europe"])
        for row in built:
            self.assertEqual((row["game_code"], row["revision"]), ("CPUE", row["build_revision"]))
            self.assertEqual(row["sha1"], pin["rom_sha1"][str(row["build_revision"])])
        for row in variants:
            if row["build_revision"] is None:
                self.assertTrue(row["planned_patch"] is None or not row["pristine_verified"], row["id"])

if __name__ == "__main__":
    unittest.main()
