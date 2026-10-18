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


class IdentifyRomTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="platinumwt-rom-test-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "arbitrary-name.nds"
        self.payload = b"synthetic input, no game data\n"
        self.path.write_bytes(self.payload)
        self.variant = {
            "id": "fixture", "name": "Fixture", "size": len(self.payload),
            "sha256": hashlib.sha256(self.payload).hexdigest(),
            "pristine_verified": True,
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
        with self.assertRaisesRegex(ValueError, "Recognized Fixture"):
            identify_rom.identify(self.path, self.catalog)

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
        self.assertEqual(catalog["schema_version"], 1)
        variants = catalog["variants"]
        self.assertEqual(len(variants), 3)
        for key in ("id", "sha256", "planned_patch"):
            self.assertEqual(len({row[key] for row in variants}), len(variants))
        for row in variants:
            self.assertEqual(row["size"], 134217728)
            for key, length in (("sha256", 64), ("sha1", 40), ("crc32", 8)):
                self.assertRegex(row[key], f"^[0-9a-f]{{{length}}}$")
            self.assertRegex(row["planned_patch"], r"^platinum-wonder-trade-[a-z0-9-]+\.bps$")
        pin = json.loads((SCRIPT.parent.parent / "client/upstream.json").read_text())
        built = [row for row in variants if row["pristine_verified"]]
        self.assertEqual({row["build_revision"] for row in built}, {0, 1})
        for row in built:
            self.assertEqual(row["sha1"], pin["rom_sha1"][str(row["build_revision"])])


if __name__ == "__main__":
    unittest.main()
