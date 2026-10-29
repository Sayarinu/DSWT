import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
import patcher
from support import assemble, literal_patch, source_read, target_copy, target_read

PINS = [bytes([0x11] * 32), bytes([0x22] * 32)]
PROFILE = {"hostname": "wt.example.test", "ipv4": "192.0.2.10", "port": 443,
           "current_spki_sha256": PINS[0].hex(), "next_spki_sha256": PINS[1].hex()}
RECORD = {key: key for key in ("tool", "version", "revision", "archive", "license", "notices", "build", "binary")}


def rom(revision, payload=b""):
    data = bytearray(4096)
    data[0x0C:0x10] = b"CPUE"
    data[0x1E] = revision
    data[0x200:0x200 + len(payload)] = payload
    return bytes(data)


BASES = {"us-rev0": rom(0), "us-rev1": rom(1)}
TARGETS = {key: rom(value[0x1E], b"wt.example.test:443" + PINS[0] + PINS[1] + key.encode()) for key, value in BASES.items()}


class FakeFlips:
    """Stands in for the container: emits literal-only patches and the expected output."""

    record = RECORD

    def __init__(self, deterministic=True, output=None, during_apply=None, fail=None):
        self.deterministic, self.output, self.during_apply, self.fail = deterministic, output, during_apply, fail
        self.calls = []

    def create(self, stage, source, target, patch_name):
        self.calls.append("create")
        if self.fail:
            raise self.fail
        data = literal_patch(stage.read(source, bps.MAX_FILE), stage.read(target, bps.MAX_FILE))
        if not self.deterministic and patch_name.endswith(".b.bps"):
            data = assemble(stage.read(source, bps.MAX_FILE), stage.read(target, bps.MAX_FILE),
                            [source_read(1), target_read(stage.read(target, bps.MAX_FILE)[1:])])
        stage.write(patch_name, data)

    def apply(self, stage, patch_name, source, target):
        self.calls.append("apply")
        if self.fail:
            raise self.fail
        literal = bps.walk(stage.read(patch_name, bps.MAX_PATCH))["literals"]["items"][0]
        data = stage.read(patch_name, bps.MAX_PATCH)
        stage.write(target, self.output if self.output is not None else
                    data[literal["patch_offset"]:literal["patch_offset"] + literal["length"]])
        if self.during_apply:
            self.during_apply(stage)


class Fixture(unittest.TestCase):
    temp_parent = None

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=self.temp_parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / "game").mkdir()
        self.inputs = self.root / "inputs"
        self.inputs.mkdir()
        self.catalog = {"variants": [
            {"id": key, "name": key, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
             "pristine_verified": True, "planned_patch": f"platinum-wonder-trade-{key}.bps"}
            for key, data in BASES.items()] + [
            {"id": "europe", "name": "Europe", "size": 4096, "sha256": hashlib.sha256(rom(9)).hexdigest(),
             "pristine_verified": False, "planned_patch": "platinum-wonder-trade-europe.bps"}]}
        self.source = {"commit": "a" * 40, "dirty": False, "upstream_repository": "upstream", "upstream_revision": "b" * 40}
        self.profile = self.write("profile.json", json.dumps(PROFILE).encode())
        self.pairs = [(self.write(f"{key}.nds", BASES[key]), self.write(f"{key}-wt.nds", TARGETS[key])) for key in BASES]

    def write(self, name, data):
        path = self.inputs / name
        path.write_bytes(data)
        return path

    def create(self, flips=None, output=None, pairs=None, **changes):
        arguments = {"pairs": pairs or self.pairs, "profile_path": self.profile,
                     "output": output or self.root / "game/bundle", "version": "0.1.0-rc1",
                     "flips": flips or FakeFlips(), "source": self.source, "catalog": self.catalog, "root": self.root}
        arguments.update(changes)
        return patcher.create(**arguments)

    def leftovers(self, directory):
        return [path.name for path in Path(directory).iterdir() if ".partial-" in path.name]


class CreateTests(Fixture):
    def test_bundle_is_complete_private_and_deterministic(self):
        flips = FakeFlips()
        manifest = self.create(flips=flips)
        bundle = self.root / "game/bundle"
        self.assertEqual(flips.calls, ["create", "create", "apply"] * 2)
        self.assertEqual(manifest["supported_subset"], ["us-rev0", "us-rev1"])
        self.assertIs(manifest["publication_permitted"], False)
        self.assertIs(manifest["distribution_blocked"], True)
        self.assertEqual(manifest["connection_profile_sha256"], hashlib.sha256(self.profile.read_bytes()).hexdigest())
        self.assertEqual(json.loads((bundle / "manifest.json").read_text()), manifest)
        for entry in manifest["entries"]:
            data = (bundle / entry["patch"]["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["patch"]["sha256"])
            self.assertEqual(entry["output"]["sha256"], hashlib.sha256(TARGETS[entry["variant"]]).hexdigest())
            self.assertEqual((bundle / f"audit/{entry['variant']}.literals.bin").read_bytes(), TARGETS[entry["variant"]])
        self.assertEqual(self.leftovers(self.root / "game"), [])
        self.create(output=self.root / "game/again")
        for name in ("manifest.json", "platinum-wonder-trade-us-rev1.bps"):
            self.assertEqual((bundle / name).read_bytes(), (self.root / "game/again" / name).read_bytes())

    def test_rejections_publish_nothing(self):
        europe = self.write("europe.nds", rom(9))
        unprofiled = self.write("unprofiled.nds", rom(1, b"other"))
        other_revision = self.write("other.nds", TARGETS["us-rev0"].replace(b"us-rev0", b"us-rev1"))
        cases = {
            "nondeterministic": {"flips": FakeFlips(deterministic=False)},
            "dirty": {"source": dict(self.source, dirty=True)},
            "unsupported base": {"pairs": [(europe, self.pairs[0][1])]},
            "unknown base": {"pairs": [(self.pairs[0][1], self.pairs[0][1])]},
            "target without profile": {"pairs": [(self.pairs[1][0], unprofiled)]},
            "revision mismatch": {"pairs": [(self.pairs[1][0], other_revision)]},
            "duplicate variant": {"pairs": [self.pairs[0], self.pairs[0]]},
            "outside game": {"output": self.root / "inputs/bundle"},
            "bad version": {"version": "../x"},
            "tool failure": {"flips": FakeFlips(fail=patcher.PatchError("boom"))},
            "wrong roundtrip": {"flips": FakeFlips(output=b"wrong")},
        }
        for name, changes in cases.items():
            with self.subTest(name), self.assertRaises(patcher.PatchError):
                self.create(**changes)
            self.assertFalse((self.root / "game/bundle").exists(), name)
            self.assertEqual(self.leftovers(self.root / "game"), [], name)

    def test_interrupt_and_existing_output_are_preserved(self):
        with self.assertRaises(KeyboardInterrupt):
            self.create(flips=FakeFlips(fail=KeyboardInterrupt()))
        self.assertEqual(self.leftovers(self.root / "game"), [])
        existing = self.root / "game/bundle"
        existing.mkdir()
        with self.assertRaises(patcher.PatchError):
            self.create()
        self.assertEqual(list(existing.iterdir()), [])
        existing.rmdir()

        class Racer(FakeFlips):
            def apply(inner, stage, *arguments):
                super().apply(stage, *arguments)
                if stage.path.name.startswith(".bundle.partial") and not existing.exists():
                    existing.mkdir()
                    (existing / "other").write_bytes(b"other writer")
        with self.assertRaisesRegex(patcher.PatchError, "appeared"):
            self.create(flips=Racer())
        self.assertEqual([p.name for p in existing.iterdir()], ["other"])
        shutil.rmtree(existing)
        with patch.object(patcher, "_renameat_noreplace", return_value=errno.EXDEV), self.assertRaises(patcher.PatchError):
            self.create()
        self.assertFalse(existing.exists())
        self.assertEqual(self.leftovers(self.root / "game"), [])
        self.assertEqual([path.read_bytes() for pair in self.pairs for path in pair],
                         [data for key in BASES for data in (BASES[key], TARGETS[key])])


class ApplyTests(Fixture):
    def setUp(self):
        super().setUp()
        self.create()
        self.bundle = self.root / "game/bundle"
        self.manifest = self.bundle / "manifest.json"
        self.base = self.pairs[1][0]
        self.out = self.root / "out"
        self.out.mkdir()

    def apply(self, flips=None, manifest=None, source=None, output=None):
        return patcher.apply(manifest or self.manifest, source or self.base, output or self.out / "wt.nds", flips or FakeFlips())

    def rewrite(self, change):
        manifest = json.loads(self.manifest.read_text())
        change(manifest)
        self.manifest.unlink()
        self.manifest.write_text(json.dumps(manifest))

    def assert_rejected(self, **arguments):
        with self.assertRaises(patcher.PatchError):
            self.apply(**arguments)
        self.assertEqual(list(self.out.iterdir()), [])
        self.assertEqual(self.base.read_bytes(), BASES["us-rev1"])

    def test_applies_to_a_new_verified_file(self):
        entry = self.apply()
        self.assertEqual(entry["variant"], "us-rev1")
        self.assertEqual((self.out / "wt.nds").read_bytes(), TARGETS["us-rev1"])
        self.assertEqual((self.out / "wt.nds").stat().st_nlink, 1)
        self.assertEqual([path.name for path in self.out.iterdir()], ["wt.nds"])
        self.assertEqual(self.base.read_bytes(), BASES["us-rev1"])

    def test_existing_outputs_and_aliases_are_rejected(self):
        for kind in ("file", "symlink", "dangling"):
            with self.subTest(kind):
                target = self.out / "wt.nds"
                if kind == "file":
                    target.write_bytes(b"keep")
                else:
                    target.symlink_to(self.base if kind == "symlink" else self.out / "missing")
                with self.assertRaises(patcher.PatchError):
                    self.apply()
                self.assertEqual(os.readlink(target) if target.is_symlink() else target.read_bytes(),
                                 b"keep" if kind == "file" else str(self.base if kind == "symlink" else self.out / "missing"))
                target.unlink()
        link = self.inputs / "link.nds"
        link.symlink_to(self.base)
        self.assert_rejected(source=link)
        hard = self.inputs / "hard.nds"
        os.link(self.base, hard)
        self.assert_rejected(source=hard)
        hard.unlink()
        self.assert_rejected(output=self.base)

    def test_rejects_modified_inputs_and_patches(self):
        modified = self.write("modified.nds", BASES["us-rev1"][:-1] + b"\1")
        self.assert_rejected(source=modified)
        self.assert_rejected(source=self.pairs[1][1])
        patch_path = self.bundle / "platinum-wonder-trade-us-rev1.bps"
        original = patch_path.read_bytes()
        def replace(data, rehash):
            patch_path.unlink()
            patch_path.write_bytes(data)
            if rehash:
                self.rewrite(lambda m: [e["patch"].update(sha256=hashlib.sha256(data).hexdigest(), size=len(data))
                                        for e in m["entries"] if e["variant"] == "us-rev1"])
        corrupt = original[:40] + bytes([original[40] ^ 0xFF]) + original[41:]
        for name, data, rehash in (("digest", corrupt, False), ("crc", corrupt, True),
                                   ("crc-valid invalid action", assemble(BASES["us-rev1"], TARGETS["us-rev1"], [target_copy(4096, 0)]), True),
                                   ("wrong target", literal_patch(BASES["us-rev1"], TARGETS["us-rev0"]), True)):
            with self.subTest(name):
                replace(data, rehash)
                self.assert_rejected()
        replace(original, True)
        self.apply()

    def test_rejects_malformed_manifests(self):
        cases = {
            "traversal": lambda m: m["entries"][1]["patch"].update(file="../platinum-wonder-trade-us-rev1.bps"),
            "duplicate input": lambda m: m["entries"][0].update(input=m["entries"][1]["input"]),
            "duplicate file": lambda m: m["entries"][0]["patch"].update(file=m["entries"][1]["patch"]["file"]),
            "publication": lambda m: m.update(publication_permitted=True),
            "schema": lambda m: m.update(schema_version=2),
            "size type": lambda m: m["entries"][1]["output"].update(size="4096"),
            "output digest": lambda m: m["entries"][1]["output"].update(sha256="0" * 64),
        }
        original = self.manifest.read_text()
        for name, change in cases.items():
            with self.subTest(name):
                self.rewrite(change)
                self.assert_rejected()
                self.manifest.unlink()
                self.manifest.write_text(original)
        self.manifest.unlink()
        self.manifest.symlink_to(self.inputs / "profile.json")
        self.assert_rejected()

    def test_tool_failures_interruptions_and_races_publish_nothing(self):
        def appear(stage):
            (self.out / "wt.nds").write_bytes(b"other writer")
        def swap_input(stage):
            self.base.unlink()
            self.base.write_bytes(b"replaced")
        def replace_stage(stage):
            shutil.rmtree(stage.path)
            stage.path.mkdir()
        for name, flips in (("failure", FakeFlips(fail=patcher.PatchError("boom"))),
                            ("wrong output", FakeFlips(output=TARGETS["us-rev0"]))):
            with self.subTest(name):
                self.assert_rejected(flips=flips)
        with self.assertRaisesRegex(patcher.PatchError, "replaced"):
            self.apply(flips=FakeFlips(during_apply=replace_stage))
        [foreign] = self.out.iterdir()
        self.assertTrue(foreign.name.startswith(".wt.nds.partial-"))
        foreign.rmdir()
        with self.assertRaises(KeyboardInterrupt):
            self.apply(flips=FakeFlips(fail=KeyboardInterrupt()))
        self.assertEqual(list(self.out.iterdir()), [])
        with self.assertRaises(patcher.PatchError):
            self.apply(flips=FakeFlips(during_apply=appear))
        self.assertEqual([p.name for p in self.out.iterdir()], ["wt.nds"])
        self.assertEqual((self.out / "wt.nds").read_bytes(), b"other writer")
        (self.out / "wt.nds").unlink()
        with patch.object(patcher, "_renameat_noreplace", return_value=errno.EXDEV), self.assertRaises(patcher.PatchError):
            self.apply()
        self.assertEqual(list(self.out.iterdir()), [])
        self.apply(flips=FakeFlips(during_apply=swap_input))
        self.assertEqual((self.out / "wt.nds").read_bytes(), TARGETS["us-rev1"])

    def test_cli_interrupt_preserves_inputs(self):
        with patch.object(patcher, "installed_flips", side_effect=KeyboardInterrupt), \
                patch("sys.stderr"):
            code = patcher.main(["apply", "--manifest", str(self.manifest), "--input", str(self.base),
                                 "--output", str(self.out / "wt.nds")])
        self.assertEqual(code, 130)
        self.assertEqual(list(self.out.iterdir()), [])


class AuditTests(Fixture):
    def test_extracts_private_evidence_without_replacement(self):
        patch_path = self.write("p.bps", assemble(b"", b"xy", [target_read(b"xy")], metadata=b"meta"))
        summary = patcher.audit(patch_path, self.root / "game/evidence", root=self.root)
        self.assertEqual(summary["literals"], {"count": 1, "bytes": 2})
        evidence = self.root / "game/evidence"
        self.assertEqual((evidence / "literals.bin").read_bytes(), b"xy")
        self.assertEqual((evidence / "metadata.bin").read_bytes(), b"meta")
        with self.assertRaises(patcher.PatchError):
            patcher.audit(patch_path, evidence, root=self.root)
        with self.assertRaises(patcher.PatchError):
            patcher.audit(patch_path, self.root / "inputs/evidence", root=self.root)
        self.assertEqual(self.leftovers(self.root / "game"), [])


class InstalledToolTests(Fixture):
    def setUp(self):
        super().setUp()
        self.binary, archive, notice = b"pinned binary", b"archive", b"notice"
        pin = {"sha256": hashlib.sha256(archive).hexdigest(), "size": len(archive), "notices": ["COPYING"],
               "binaries": {"linux/amd64": {"size": len(self.binary), "sha256": hashlib.sha256(self.binary).hexdigest()}}}
        self.destination = self.root / "game/tools/flips"
        (self.destination / "notices").mkdir(parents=True)
        (self.destination / "notices/COPYING").write_bytes(notice)
        (self.destination / "source.tar.gz").write_bytes(archive)
        (self.destination / "flips").write_bytes(self.binary)
        (self.destination / "provenance.json").write_text(json.dumps({
            "archive": {"file": "source.tar.gz", "sha256": pin["sha256"]},
            "notices": {"COPYING": hashlib.sha256(notice).hexdigest()},
            "build": {"platform": "linux/amd64"}, "binary": pin["binaries"]["linux/amd64"], "image_id": "sha256:x"}))
        location = patch.object(patcher, "_tool_location",
                                return_value=(pin, b"", "key", self.destination.parent, self.destination))
        location.start()
        self.addCleanup(location.stop)

    def verify(self, image=None):
        with patch.object(patcher, "_image_binary", return_value=image or self.binary):
            return patcher.installed_flips(self.root)

    def test_reverifies_every_retained_artifact(self):
        self.assertEqual(self.verify()["image_id"], "sha256:x")
        with self.assertRaisesRegex(patcher.PatchError, "image"):
            self.verify(b"other")
        for name, data in (("flips", b"tampered"), ("notices/COPYING", b"changed"), ("source.tar.gz", b"swapped")):
            with self.subTest(name):
                path = self.destination / name
                original = path.read_bytes()
                path.unlink()
                path.write_bytes(data)
                with self.assertRaises(patcher.PatchError):
                    self.verify()
                path.unlink()
                path.write_bytes(original)
        provenance = self.destination / "provenance.json"
        original = provenance.read_text()
        for name, change in (("notice dropped", lambda r: r.update(notices={})),
                             ("unpinned platform", lambda r: r["build"].update(platform="linux/riscv64")),
                             ("missing field", lambda r: r.pop("archive")),
                             ("archive outside", lambda r: r["archive"].update(file="../source.tar.gz"))):
            with self.subTest(name):
                record = json.loads(original)
                change(record)
                provenance.unlink()
                provenance.write_text(json.dumps(record))
                with self.assertRaises(patcher.PatchError):
                    self.verify()
        provenance.unlink()
        provenance.write_text(original)
        self.verify()
        moved = self.destination.with_name("real")
        self.destination.rename(moved)
        self.destination.symlink_to(moved)
        with self.assertRaisesRegex(patcher.PatchError, "not prepared"):
            self.verify()


@unittest.skipUnless(os.environ.get("PLATINUMWT_FLIPS_PROVENANCE"), "set PLATINUMWT_FLIPS_PROVENANCE to a prepared tool")
class PinnedFlipsTests(Fixture):
    """Exercise the real pinned container on small synthetic ROM-shaped files."""

    # The Docker daemon must be able to bind-mount the staging directories.
    temp_parent = Path(__file__).resolve().parents[3] / "game"

    def test_generates_twice_roundtrips_and_applies(self):
        flips = patcher.Flips(json.loads(Path(os.environ["PLATINUMWT_FLIPS_PROVENANCE"]).read_text()))
        manifest = self.create(flips=flips)
        self.assertEqual(self.leftovers(self.root / "game"), [])
        out = self.root / "out.nds"
        self.assertEqual(patcher.apply(self.root / "game/bundle/manifest.json", self.pairs[0][0], out, flips)["variant"], "us-rev0")
        self.assertEqual(out.read_bytes(), TARGETS["us-rev0"])
        self.assertEqual(manifest["tool"]["binary"], flips.record["binary"])


if __name__ == "__main__":
    unittest.main()
