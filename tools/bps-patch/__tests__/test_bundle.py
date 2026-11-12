import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bundle
from support import BASES, TARGETS, USED, assemble, literal_patch, rom, target_copy
from test_patcher import Fixture


class BundleTests(Fixture):
    def setUp(self):
        super().setUp()
        self.create()
        self.bundle = self.root / "game/bundle"
        self.manifest_path = self.bundle / "manifest.json"
        self.manifest = bundle.load_manifest(self.manifest_path.read_bytes())
        self.out = self.root / "out"
        self.out.mkdir()

    def patch(self, data, name="input.nds", output=None):
        source = self.write(name, data)
        return bundle.patch_file(self.bundle, source, output or self.out / "wt.nds")

    def assert_refused(self, data, message, **arguments):
        with self.assertRaisesRegex(bundle.BundleError, message):
            self.patch(data, **arguments)
        self.assertEqual([path.name for path in self.out.iterdir()], [])

    def rewrite(self, change):
        manifest = json.loads(self.manifest_path.read_text())
        change(manifest)
        self.manifest_path.unlink()
        self.manifest_path.write_text(json.dumps(manifest))

    def test_patches_full_and_trimmed_dumps(self):
        for name, data in (("full", BASES["us-rev1"]), ("trimmed", BASES["us-rev1"][:USED]),
                           ("partly trimmed", BASES["us-rev1"][:USED + 7])):
            with self.subTest(name):
                entry, output = self.patch(data)
                self.assertEqual(entry["variant"], "us-rev1")
                self.assertEqual(output.read_bytes(), TARGETS["us-rev1"])
                self.assertEqual([path.name for path in self.out.iterdir()], ["wt.nds"])
                output.unlink()

    def test_default_output_is_beside_the_input(self):
        source = self.write("Platinum.nds", BASES["us-rev0"])
        entry, output = bundle.patch_file(self.bundle, source)
        self.assertEqual(output, self.inputs / "Platinum (Wonder Trade).nds")
        self.assertEqual(output.read_bytes(), TARGETS["us-rev0"])
        self.assertEqual(source.read_bytes(), BASES["us-rev0"])

    def test_explains_unsupported_inputs(self):
        cases = {
            "already patched": (TARGETS["us-rev1"], "already has DSWT 0.1.0"),
            "trimmed patched": (TARGETS["us-rev1"][:USED], "already has DSWT"),
            "other game": (rom(1, "x", code="ADAE"), "not a Pokémon Platinum ROM"),
            "short file": (b"NDS", "not a Pokémon Platinum ROM"),
            "french": (rom(0, "fr", code="CPUF"), r"France, French, Rev 0\) is not supported yet. Supported: us-rev0, us-rev1"),
            "japanese rev 1": (rom(1, "jp", code="CPUJ"), "Japan, Japanese, Rev 1"),
            "modified": (BASES["us-rev1"][:-1] + b"\0", "modified, encrypted or damaged"),
            "trimmed too far": (BASES["us-rev1"][:USED - 1], "modified, encrypted or damaged"),
            "oversized": (BASES["us-rev1"] + b"\xff", "unexpected size"),
        }
        for name, (data, message) in cases.items():
            with self.subTest(name):
                self.assert_refused(data, message)

    def test_rejects_changed_patches(self):
        path = self.bundle / "dswt-us-rev1.bps"
        original = path.read_bytes()

        def replace(data, rehash):
            path.unlink()
            path.write_bytes(data)
            if rehash:
                digest = bundle.sha256(data)
                self.rewrite(lambda m: [e["patch"].update(sha256=digest, size=len(data))
                                        for e in m["entries"] if e["variant"] == "us-rev1"])
        corrupt = original[:40] + bytes([original[40] ^ 0xFF]) + original[41:]
        for name, data, rehash, message in (
                ("digest", corrupt, False, "does not match the manifest"),
                ("crc", corrupt, True, "Patch failed"),
                ("invalid action", assemble(BASES["us-rev1"], TARGETS["us-rev1"], [target_copy(4096, 0)]), True, "Patch failed"),
                ("wrong target", literal_patch(BASES["us-rev1"], TARGETS["us-rev0"]), True, "failed verification")):
            with self.subTest(name):
                replace(data, rehash)
                self.assert_refused(BASES["us-rev1"], message)
        replace(original, True)
        self.patch(BASES["us-rev1"])

    def test_rejects_malformed_manifests(self):
        cases = {
            "traversal": lambda m: m["entries"][1]["patch"].update(file="../dswt-us-rev1.bps"),
            "duplicate input": lambda m: m["entries"][0].update(input=m["entries"][1]["input"]),
            "duplicate file": lambda m: m["entries"][0]["patch"].update(file=m["entries"][1]["patch"]["file"]),
            "output is input": lambda m: m["entries"][0]["output"].update(sha256=m["entries"][1]["input"]["sha256"]),
            "old private schema": lambda m: m.update(schema_version=1, kind="dswt-private-bps-bundle"),
            "size type": lambda m: m["entries"][1]["output"].update(size="4096"),
            "game code": lambda m: m["entries"][1].update(game_code="XXXX"),
            "revision type": lambda m: m["entries"][1].update(revision="1"),
            "variant": lambda m: m["entries"][1].update(variant="../x"),
            "no entries": lambda m: m.update(entries=[]),
        }
        original = self.manifest_path.read_text()
        for name, change in cases.items():
            with self.subTest(name):
                self.rewrite(change)
                with self.assertRaisesRegex(bundle.BundleError, "Malformed manifest"):
                    self.patch(BASES["us-rev1"])
                self.manifest_path.unlink()
                self.manifest_path.write_text(original)
        for data in (b"not json", b" " * (bundle.MAX_MANIFEST + 1)):
            self.manifest_path.unlink()
            self.manifest_path.write_bytes(data)
            with self.assertRaises(bundle.BundleError):
                self.patch(BASES["us-rev1"])
        self.manifest_path.unlink()
        self.manifest_path.symlink_to(self.profile)
        self.assert_refused(BASES["us-rev1"], "regular file")

    def test_never_replaces_or_leaves_partial_files(self):
        target = self.out / "wt.nds"
        for kind in ("file", "symlink", "dangling"):
            with self.subTest(kind):
                if kind == "file":
                    target.write_bytes(b"keep")
                else:
                    target.symlink_to(self.pairs[1][0] if kind == "symlink" else self.out / "missing")
                with self.assertRaisesRegex(bundle.BundleError, "already exists"):
                    self.patch(BASES["us-rev1"])
                self.assertEqual([path.name for path in self.out.iterdir()], ["wt.nds"])
                self.assertEqual(self.pairs[1][0].read_bytes(), BASES["us-rev1"])
                target.unlink()
        with self.assertRaisesRegex(bundle.BundleError, "already exists"):
            self.patch(BASES["us-rev1"], output=self.pairs[1][0])
        self.assertEqual(self.pairs[1][0].read_bytes(), BASES["us-rev1"])

        def appear(source, destination):
            Path(destination).write_bytes(b"other writer")
            raise FileExistsError(destination)
        with patch.object(os, "link", appear), self.assertRaisesRegex(bundle.BundleError, "appeared"):
            self.patch(BASES["us-rev1"])
        self.assertEqual([path.name for path in self.out.iterdir()], ["wt.nds"])
        self.assertEqual(target.read_bytes(), b"other writer")
        target.unlink()
        with patch.object(os, "link", side_effect=PermissionError), self.assertRaisesRegex(bundle.BundleError, "local disk"):
            self.patch(BASES["us-rev1"])
        self.assertEqual(list(self.out.iterdir()), [])
        with patch.object(bundle.bps, "apply", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            self.patch(BASES["us-rev1"])
        self.assertEqual(list(self.out.iterdir()), [])

    def test_identifies_every_retail_cartridge(self):
        self.assertEqual(sorted(bundle.CARTRIDGES), ["CPUD", "CPUE", "CPUF", "CPUI", "CPUJ", "CPUK", "CPUP", "CPUS"])
        self.assertEqual(bundle.cartridge(rom(1, "k", code="CPUK")),
                         {"game_code": "CPUK", "region": "Korea", "language": "Korean", "revision": 1})
        self.assertIsNone(bundle.cartridge(b"\xff" * 0x200))


if __name__ == "__main__":
    unittest.main()
