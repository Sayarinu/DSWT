import base64
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
import patcher
from support import BASES, PINS, PROFILE, TARGETS, assemble, rom, source_read, target_read

RECORD = {key: key for key in ("tool", "version", "revision", "archive", "license", "notices", "build", "binary")}
SCALAR = bytes([0x11] * 32)
# A P-256 SEC1 private key shape around a known scalar.
_DER = (bytes.fromhex("30770201010420") + SCALAR + bytes.fromhex("a00a06082a8648ce3d030107a144034200")
        + b"\x04" + bytes(range(64)))
KEY = (b"-----BEGIN EC PRIVATE KEY-----\n" + base64.encodebytes(_DER) + b"-----END EC PRIVATE KEY-----\n")


class FakeFlips:
    """Stands in for the container: keeps the header and padding from the source, ships the body."""

    record = RECORD

    def __init__(self, deterministic=True, output=None, during_apply=None, fail=None):
        self.deterministic, self.output, self.during_apply, self.fail = deterministic, output, during_apply, fail
        self.calls = []

    def create(self, stage, source, target, patch_name):
        self.calls.append("create")
        if self.fail:
            raise self.fail
        base, data = stage.read(source, bps.MAX_FILE), stage.read(target, bps.MAX_FILE)
        split = 0x200 if self.deterministic or not patch_name.endswith(".b.bps") else 0x1FF
        body_end = len(data) - 0x100
        stage.write(patch_name, assemble(base, data, [source_read(split), target_read(data[split:body_end]),
                                                      source_read(0x100)]))

    def apply(self, stage, patch_name, source, target):
        self.calls.append("apply")
        if self.fail:
            raise self.fail
        result = bps.apply(stage.read(source, bps.MAX_FILE), stage.read(patch_name, bps.MAX_PATCH))
        stage.write(target, self.output if self.output is not None else result)
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
            {"id": key, "game": "Pokémon Platinum", "name": key, "game_code": "CPUE", "revision": data[0x1E],
             "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
             "pristine_verified": True, "planned_patch": f"dswt-{key}.bps"}
            for key, data in BASES.items()] + [
            {"id": "europe", "game": "Pokémon Platinum", "name": "Europe", "game_code": "CPUP", "revision": 0,
             "size": 4096, "sha256": hashlib.sha256(rom(0, "europe", "CPUP")).hexdigest(),
             "pristine_verified": False, "planned_patch": "dswt-europe.bps"},
            {"id": "diamond", "game": "Pokémon Diamond", "name": "Diamond", "game_code": "ADAE", "revision": 5,
             "size": 4096, "sha256": hashlib.sha256(rom(5, "diamond", "ADAE")).hexdigest(),
             "pristine_verified": True, "planned_patch": None}]}
        self.source = {"commit": "a" * 40, "dirty": False, "upstream_repository": "upstream", "upstream_revision": "b" * 40}
        self.profile = self.write("profile.json", json.dumps(PROFILE).encode())
        self.pairs = [(self.write(f"{key}.nds", BASES[key]), self.write(f"{key}-wt.nds", TARGETS[key])) for key in BASES]

    def write(self, name, data):
        path = self.inputs / name
        path.write_bytes(data)
        return path

    def create(self, flips=None, output=None, pairs=None, **changes):
        arguments = {"pairs": pairs or self.pairs, "profile_path": self.profile,
                     "output": output or self.root / "game/bundle", "version": "0.1.0",
                     "flips": flips or FakeFlips(), "source": self.source, "catalog": self.catalog, "root": self.root}
        arguments.update(changes)
        return patcher.create(**arguments)

    def leftovers(self, directory):
        return [path.name for path in Path(directory).iterdir() if ".partial-" in path.name]


class CreateTests(Fixture):
    def test_bundle_is_complete_checked_and_deterministic(self):
        flips = FakeFlips()
        manifest = self.create(flips=flips)
        bundle = self.root / "game/bundle"
        self.assertEqual(flips.calls, ["create", "create", "apply"] * 2)
        self.assertEqual((manifest["schema_version"], manifest["kind"]), (2, "dswt-bps-bundle"))
        self.assertNotIn("publication_permitted", manifest)
        self.assertEqual(manifest["release_checks"]["private_keys_checked"], 0)
        self.assertEqual(manifest["connection_profile_sha256"], hashlib.sha256(self.profile.read_bytes()).hexdigest())
        self.assertEqual(json.loads((bundle / "manifest.json").read_text()), manifest)
        for entry in manifest["entries"]:
            self.assertEqual((entry["game_code"], entry["language"]), ("CPUE", "English"))
            self.assertEqual(entry["revision"], BASES[entry["variant"]][0x1E])
            data = (bundle / entry["patch"]["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["patch"]["sha256"])
            self.assertEqual(bps.apply(BASES[entry["variant"]], data), TARGETS[entry["variant"]])
            self.assertEqual(entry["output"]["sha256"], hashlib.sha256(TARGETS[entry["variant"]]).hexdigest())
            self.assertEqual((bundle / f"audit/{entry['variant']}.literals.bin").read_bytes(),
                             TARGETS[entry["variant"]][0x200:-0x100])
        self.assertEqual(self.leftovers(self.root / "game"), [])
        self.create(output=self.root / "game/again")
        for name in ("manifest.json", "dswt-us-rev1.bps"):
            self.assertEqual((bundle / name).read_bytes(), (self.root / "game/again" / name).read_bytes())

    def test_rejections_publish_nothing(self):
        europe = self.write("europe.nds", rom(0, "europe", "CPUP"))
        unprofiled = self.write("unprofiled.nds", rom(1, "other"))
        local = self.write("local.json", json.dumps(dict(PROFILE, hostname="wt.example.test")).encode())
        private = self.write("private.json", json.dumps(dict(PROFILE, ipv4="10.0.0.2")).encode())
        cases = {
            "nondeterministic": {"flips": FakeFlips(deterministic=False)},
            "dirty": {"source": dict(self.source, dirty=True)},
            "unsupported base": {"pairs": [(europe, self.pairs[0][1])]},
            "unknown base": {"pairs": [(self.pairs[0][1], self.pairs[0][1])]},
            "target without profile": {"pairs": [(self.pairs[1][0], unprofiled)]},
            "revision mismatch": {"pairs": [(self.pairs[1][0], self.pairs[0][1])]},
            "duplicate variant": {"pairs": [self.pairs[0], self.pairs[0]]},
            "outside game": {"output": self.root / "inputs/bundle"},
            "bad version": {"version": "../x"},
            "test hostname": {"profile_path": local},
            "private address": {"profile_path": private},
            "tool failure": {"flips": FakeFlips(fail=patcher.PatchError("boom"))},
            "wrong roundtrip": {"flips": FakeFlips(output=b"wrong")},
        }
        for name, changes in cases.items():
            with self.subTest(name), self.assertRaises(patcher.PatchError):
                self.create(**changes)
            self.assertFalse((self.root / "game/bundle").exists(), name)
            self.assertEqual(self.leftovers(self.root / "game"), [], name)

    def test_only_catalogued_supported_bases_are_used(self):
        diamond = self.write("diamond.nds", rom(5, "diamond", "ADAE"))
        relabelled = rom(0, "relabelled", "CPUP")
        self.catalog["variants"].append(dict(self.catalog["variants"][0], id="relabelled", planned_patch="dswt-x.bps",
                                             sha256=hashlib.sha256(relabelled).hexdigest()))
        relabelled = self.write("relabelled.nds", relabelled)
        for base, message in ((diamond, "Recognized Pokémon Diamond Diamond, but it is not supported yet"),
                              (relabelled, "does not have its catalogued cartridge header")):
            with self.subTest(base.name), self.assertRaisesRegex(patcher.PatchError, message):
                self.create(pairs=[(base, self.pairs[0][1])])
            self.assertFalse((self.root / "game/bundle").exists())

    def leak(self, data, at=0x800):
        target = bytearray(TARGETS["us-rev1"])
        target[at:at + len(data)] = data
        return [(self.pairs[1][0], self.write("leak.nds", bytes(target)))]

    def test_release_checks_reject_data_that_does_not_belong(self):
        (self.inputs / "current.key").write_bytes(KEY)
        cases = {
            "private key scalar": (SCALAR, 0x800, "private key current.key"),
            "pem text": (b"-----BEGIN EC PRIVATE KEY-----", 0x800, "key text"),
            "build path": (str(self.root).encode(), 0x800, "build-host"),
            "home path": (b"/home/builder/dswt", 0x800, "build-host"),
            "retail copy": (BASES["us-rev1"][0x400:0x42F], 0x800, "retail ROM content"),
            "other base copy": (BASES["us-rev0"][0x600:0x640], 0x900, "retail ROM content"),
        }
        for name, (data, at, message) in cases.items():
            with self.subTest(name):
                with self.assertRaisesRegex(patcher.PatchError, message):
                    self.create(pairs=[self.pairs[0], *self.leak(data, at)])
                self.assertFalse((self.root / "game/bundle").exists())
                self.assertEqual(self.leftovers(self.root / "game"), [])
                (self.inputs / "leak.nds").unlink()
        manifest = self.create(pairs=self.leak(BASES["us-rev1"][0x400:0x41F]))  # 31 bytes: shorter than a window
        self.assertEqual(manifest["release_checks"]["private_keys_checked"], 1)
        self.assertEqual(manifest["release_checks"]["verbatim_retail_run_limit"], 47)

    def test_explicit_keys_replace_the_default_and_must_be_pem(self):
        other = self.inputs / "elsewhere.pem"
        other.write_bytes(KEY)
        with self.assertRaisesRegex(patcher.PatchError, "elsewhere.pem"):
            self.create(pairs=self.leak(SCALAR), secret_paths=[other])
        (self.inputs / "leak.nds").unlink()
        other.unlink()
        other.write_bytes(b"-----BEGIN KEY-----\nnot base64!\n-----END KEY-----\n")
        with self.assertRaisesRegex(patcher.PatchError, "not a PEM"):
            self.create(secret_paths=[other])

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


class ReleaseTests(Fixture):
    def setUp(self):
        super().setUp()
        self.create()
        self.bundle = self.root / "game/bundle"
        for name, relative in patcher.RELEASE_DOCUMENTS.items():
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.root / relative).write_text(name)

    def test_packages_a_reproducible_player_archive_without_audit_data(self):
        first = patcher.package(self.bundle, self.root / "game/a.zip", root=self.root)
        second = patcher.package(self.bundle, self.root / "game/b.zip", root=self.root)
        self.assertEqual(first["sha256"], second["sha256"])
        with zipfile.ZipFile(first["archive"]) as archive:
            self.assertEqual(archive.namelist(), [f"dswt-0.1.0/{name}" for name in
                                                  ("INSTALL.md", "LICENSE", "NOTICE", "dswt-us-rev0.bps",
                                                   "dswt-us-rev1.bps", "manifest.json")])
            self.assertEqual(archive.read("dswt-0.1.0/LICENSE"), b"LICENSE")
        with self.assertRaises(patcher.PatchError):
            patcher.package(self.bundle, self.root / "game/a.zip", root=self.root)
        with self.assertRaises(patcher.PatchError):
            patcher.package(self.bundle, self.root / "inputs/c.zip", root=self.root)

    def test_package_rejects_a_changed_patch(self):
        path = self.bundle / "dswt-us-rev0.bps"
        data = path.read_bytes()
        path.unlink()
        path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        with self.assertRaisesRegex(patcher.PatchError, "does not match"):
            patcher.package(self.bundle, self.root / "game/a.zip", root=self.root)
        self.assertEqual(self.leftovers(self.root / "game"), [])

    def test_cli_applies_without_docker(self):
        out = self.root / "out.nds"
        with patch.object(patcher, "installed_flips", side_effect=AssertionError("Docker is not needed")), \
                patch("sys.stdout"):
            code = patcher.main(["apply", "--manifest", str(self.bundle / "manifest.json"),
                                 "--input", str(self.pairs[1][0]), "--output", str(out)])
        self.assertEqual(code, 0)
        self.assertEqual(out.read_bytes(), TARGETS["us-rev1"])
        with patch("sys.stderr"):
            code = patcher.main(["apply", "--manifest", str(self.bundle / "manifest.json"),
                                 "--input", str(self.pairs[1][0]), "--output", str(out)])
        self.assertEqual(code, 1)
        self.assertEqual(out.read_bytes(), TARGETS["us-rev1"])


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
        pin = {"sha256": hashlib.sha256(archive).hexdigest(), "size": len(archive), "notices": {"COPYING": hashlib.sha256(notice).hexdigest()},
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
                             ("missing binary", lambda r: r.pop("binary")),
                             ("notice digest", lambda r: r["notices"].update(COPYING="0" * 64)),
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


@unittest.skipUnless(os.environ.get("DSWT_FLIPS_PROVENANCE"), "set DSWT_FLIPS_PROVENANCE to a prepared tool")
class PinnedFlipsTests(Fixture):
    """Exercise the real pinned container on small synthetic ROM-shaped files."""

    # The Docker daemon must be able to bind-mount the staging directories.
    temp_parent = Path(__file__).resolve().parents[3] / "game"

    def test_generates_twice_roundtrips_and_applies(self):
        flips = patcher.Flips(json.loads(Path(os.environ["DSWT_FLIPS_PROVENANCE"]).read_text()))
        manifest = self.create(flips=flips)
        self.assertEqual(self.leftovers(self.root / "game"), [])
        out = self.root / "out.nds"
        self.assertEqual(patcher.apply(self.root / "game/bundle/manifest.json", self.pairs[0][0], out)["variant"], "us-rev0")
        self.assertEqual(out.read_bytes(), TARGETS["us-rev0"])
        self.assertEqual(manifest["tool"]["binary"], flips.record["binary"])

if __name__ == "__main__":
    unittest.main()
