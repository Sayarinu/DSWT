import base64
import errno
import hashlib
import struct
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps
import patcher
from support import (BASES, PINS, PROFILE, TARGETS, arm9, assemble, blz, rom, source_read, target_read, with_arm9,
                     with_overlay)

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

    def stop(self):
        self.calls.append("stop")


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
        self.assertEqual(sorted(flips.calls), ["apply"] * 2 + ["create"] * 4)
        self.assertEqual((manifest["schema_version"], manifest["kind"]), (3, "dswt-bps-bundle"))
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

    def test_parallel_and_serial_runs_make_the_same_bundle(self):
        pairs = self.pairs + [(base, self.write(f"{key}-wt.nds", target)) for key, base, target in self.ports()]
        lines = []
        self.create(pairs=pairs, jobs=1, output=self.root / "game/serial")
        self.create(pairs=pairs, jobs=4, output=self.root / "game/parallel", progress=lines.append)
        serial, parallel = self.root / "game/serial", self.root / "game/parallel"
        names = sorted(str(path.relative_to(serial)) for path in serial.rglob("*") if path.is_file())
        self.assertEqual(names, sorted(str(path.relative_to(parallel)) for path in parallel.rglob("*") if path.is_file()))
        for name in names:
            self.assertEqual((serial / name).read_bytes(), (parallel / name).read_bytes(), name)
        self.assertEqual(sorted(line.split(":")[0] for line in lines[:-1]), sorted([*BASES, "port-0", "port-1", "port-2"]))
        self.assertTrue(all(line.endswith(f"/{len(pairs)})") for line in lines[:-1]), lines)
        self.assertEqual(lines[-1], f"Checking the patches against {len(pairs)} retail cartridges")

    def ports(self):
        """More catalogued cartridges, so several are patched at once."""
        for index in range(3):
            key, base = f"port-{index}", rom(2 + index, f"port-{index}")
            path = self.write(f"{key}.nds", base)
            self.catalog["variants"].append(dict(self.catalog["variants"][0], id=key, revision=2 + index,
                                                 planned_patch=f"dswt-{key}.bps", sha256=hashlib.sha256(base).hexdigest()))
            yield key, path, rom(2 + index, f"target-{key}", payload=b"wt.dswt.games:443" + PINS[0] + PINS[1])

    def test_a_failure_stops_the_other_cartridges(self):
        class Failing(FakeFlips):
            def create(inner, stage, source, *arguments):
                if source.startswith("us-rev0."):
                    raise patcher.PatchError("boom")
                super().create(stage, source, *arguments)
        flips = Failing()
        with self.assertRaisesRegex(patcher.PatchError, "boom"):
            self.create(flips=flips)
        self.assertIn("stop", flips.calls)
        self.assertFalse((self.root / "game/bundle").exists())
        self.assertEqual(self.leftovers(self.root / "game"), [])

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

    def test_checks_search_compressed_code(self):
        # Like HeartGold, the base keeps its code in a compressed overlay; the patch
        # applies to the base followed by that code expanded.
        retail_code = hashlib.sha512(b"retail overlay").digest() + bytes(40)
        base = with_overlay(rom(1, "compressed-base"), blz(retail_code, 40))
        self.catalog["variants"].append(dict(self.catalog["variants"][1], id="compressed", planned_patch="dswt-c.bps",
                                             sha256=hashlib.sha256(base).hexdigest()))
        base_path = self.write("compressed-base.nds", base)
        profile = b"wt.dswt.games:443" + PINS[0] + PINS[1]

        def compressed(payload, damage=False):
            code = bytearray(blz(payload + bytes(40), 40))
            if damage:
                code[-8:-4] = (0x08FFFFFF).to_bytes(4, "little")
            return [(base_path, self.write("compressed.nds", with_overlay(rom(1, "compressed"), bytes(code))))]

        def uncompressed(payload):  # the same overlay, stored uncompressed
            data = bytearray(with_overlay(rom(1, "compressed", payload=payload), bytes(64)))
            data[0x35C:0x360] = bytes(4)
            return [(base_path, self.write("compressed.nds", bytes(data)))]
        self.assertNotIn(PINS[0], compressed(profile)[0][1].read_bytes())
        manifest = self.create(pairs=compressed(profile))
        self.assertEqual([entry["variant"] for entry in manifest["entries"]], ["compressed"])
        patch_file = self.root / "game/bundle/dswt-c.bps"
        expanded = base + retail_code
        self.assertEqual(bps.apply(expanded, patch_file.read_bytes()), (self.inputs / "compressed.nds").read_bytes())
        (self.inputs / "current.key").write_bytes(KEY)
        for name, pairs, message in (
                ("key", lambda: compressed(profile + SCALAR), "private key current.key"),
                ("profile twice", lambda: compressed(profile + PINS[0]), "connection profile"),
                ("damaged", lambda: compressed(profile, damage=True), "malformed compressed code"),
                ("recompressed retail code", lambda: compressed(profile + retail_code[:48]), "retail code recompressed"),
                ("expanded retail code", lambda: uncompressed(profile + retail_code[:48]), "retail ROM content")):
            with self.subTest(name):
                (self.inputs / "compressed.nds").unlink()
                with self.assertRaisesRegex(patcher.PatchError, message):
                    self.create(pairs=pairs(), output=self.root / "game/other")
                self.assertFalse((self.root / "game/other").exists())

    def test_checks_search_footerless_arm9(self):
        # Like Black, White, Black 2 and White 2: a compressed ARM9 with no Nitro footer.
        retail_code = (hashlib.sha512(b"retail arm9").digest() * 2)[:81] + bytes(40)
        base = with_arm9(rom(1, "gen5-base"), arm9(retail_code, 40), footer=False)
        self.catalog["variants"].append(dict(self.catalog["variants"][1], id="gen5", planned_patch="dswt-g.bps",
                                             sha256=hashlib.sha256(base).hexdigest()))
        base_path = self.write("gen5-base.nds", base)
        profile = b"wt.dswt.games:443" + PINS[0] + PINS[1]

        def pairs(binary, payload=b""):
            # The header keeps the ARM9 size, so every target's binary is as long as the base's.
            self.assertEqual(len(binary), len(arm9(retail_code, 40)))
            return [(base_path, self.write("gen5.nds", with_arm9(rom(1, "gen5", payload=payload), binary, footer=False)))]
        # Black 2 and White 2: Wonder Trade, and so the profile, only inside the compressed ARM9.
        self.assertNotIn(PINS[0], pairs(arm9(profile + bytes(40), 40))[0][1].read_bytes())
        manifest = self.create(pairs=pairs(arm9(profile + bytes(40), 40)))
        self.assertEqual([entry["variant"] for entry in manifest["entries"]], ["gen5"])
        expanded = base + arm9(retail_code, 40)[:0x24] + retail_code
        self.assertEqual(bps.apply(expanded, (self.root / "game/bundle/dswt-g.bps").read_bytes()),
                         (self.inputs / "gen5.nds").read_bytes())
        # Black and White: the port stores ARM9 uncompressed, so retail ARM9 code in it must come from the base.
        (self.inputs / "gen5.nds").unlink()
        stored = retail_code[:48].ljust(len(blz(retail_code, 40)), b"\2")
        with self.assertRaisesRegex(patcher.PatchError, "retail ROM content"):
            self.create(pairs=pairs(arm9(stored, 0, compressed=False), payload=profile), output=self.root / "game/other")
        self.assertFalse((self.root / "game/other").exists())

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
                if stage.path.name.startswith(".bundle.partial"):
                    try:  # the cartridges' applies may race here
                        existing.mkdir()
                    except FileExistsError:
                        return
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
        for relative in patcher.PATCH_NOTICES:
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.root / relative).write_text(relative)
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
            for relative in patcher.PATCH_NOTICES:
                self.assertIn((self.root / relative).read_bytes(), archive.read("dswt-0.1.0/NOTICE"))
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

class BudgetTests(unittest.TestCase):
    def run_all(self, budget, costs):
        """Hold each cost in turn on its own thread; return the costs held at once, as each starts."""
        held, seen, lock = [], [], threading.Lock()
        release = threading.Event()

        def work(cost):
            with budget.hold(cost):
                with lock:
                    held.append(cost)
                    seen.append(list(held))
                release.wait(5)
                with lock:
                    held.remove(cost)
        threads = []
        for cost in costs:
            threads.append(threading.Thread(target=work, args=(cost,)))
            threads[-1].start()
            time.sleep(0.05)  # arrive in order
        release.set()
        for thread in threads:
            thread.join(5)
        return seen

    def test_work_starts_in_arrival_order_while_it_fits(self):
        # 6 and 3 fit together; 2 must wait behind 5 rather than pass it.
        seen = self.run_all(patcher.Budget(10), [6, 3, 5, 2])
        self.assertEqual(seen[:2], [[6], [6, 3]])
        self.assertEqual([starting[-1] for starting in seen], [6, 3, 5, 2])

    def test_work_larger_than_the_budget_runs_alone(self):
        seen = self.run_all(patcher.Budget(10), [4, 25, 4])
        self.assertIn([25], seen)
        self.assertNotIn(25, [cost for starting in seen if len(starting) > 1 for cost in starting])


class RetailIndexTests(unittest.TestCase):
    """The bundle-wide retail check: one pass over each cartridge, the same 47-byte limit."""

    class Item(patcher.Checked):
        def __init__(self, data):  # pylint: disable=super-init-not-called
            self.variant, self.literals, self.modules = {"id": "probe"}, [(0x100, data)], []
            self.copied, self.recompressed = set(), []

    @staticmethod
    def noise(seed, size):
        out = b""
        while len(out) < size:
            out += hashlib.sha512(seed + len(out).to_bytes(4, "little")).digest()
        return out[:size]

    def check(self, literal, retail):
        item = self.Item(literal)
        index = patcher.RetailIndex([item])
        index.search(retail)
        return item.problem()

    def test_runs_of_47_bytes_are_found_at_any_alignment(self):
        retail = self.noise(b"retail", 4096)
        for start in range(0, 48):
            for where in (0, 5, 17):
                literal = self.noise(b"before", where) + retail[1000 + start:1000 + start + 47] + self.noise(b"after", 9)
                with self.subTest(start=start, where=where):
                    self.assertIn("retail ROM content", self.check(literal, retail))

    def test_shorter_runs_and_padding_pass(self):
        retail = self.noise(b"retail", 4096) + bytes(200)
        for start in range(0, 32):
            literal = self.noise(b"x", 3) + retail[2000 + start:2000 + start + 46] + self.noise(b"y", 3)
            with self.subTest(start=start):
                self.assertIsNone(self.check(literal, retail))
        # 40 zero bytes are shorter than the limit, though the retail padding is longer.
        self.assertIsNone(self.check(self.noise(b"a", 7) + bytes(40) + self.noise(b"b", 7), retail))


class DigestTableTests(unittest.TestCase):
    """DSi hash tables hold digests, so the retail check leaves them out of literals."""

    @staticmethod
    def rom(dsi=True):
        header = bytearray(0x200)
        header[0x12] = 2 if dsi else 0
        struct.pack_into("<4I", header, 0x1F0, 0x1000, 0x400, 0x2000, 0x100)
        return bytes(header) + bytes(0x3000)

    def test_tables_come_from_the_dsi_header(self):
        self.assertEqual(patcher.digest_tables(self.rom()), [(0x1000, 0x1400), (0x2000, 0x2100)])
        self.assertEqual(patcher.digest_tables(self.rom(dsi=False)), [])

    def test_literals_keep_only_the_bytes_outside_the_tables(self):
        tables = patcher.digest_tables(self.rom())
        data = bytes(range(256)) * 8
        self.assertEqual(patcher.outside(0x1000, data[:0x400], tables), [])
        self.assertEqual(patcher.outside(0x0F00, data, tables),
                         [(0x0F00, data[:0x100]), (0x1400, data[0x500:0x800])])
        self.assertEqual(patcher.outside(0x3000, data[:16], tables), [(0x3000, data[:16])])

    def test_a_copied_table_run_passes_and_the_same_run_elsewhere_fails(self):
        retail = RetailIndexTests.noise(b"retail", 4096)
        run = retail[1000:1100]
        rom = self.rom()
        for offset, expected in ((0x1100, None), (0x1800, "retail ROM content")):
            item = RetailIndexTests.Item(b"")
            item.literals = [piece for piece in patcher.outside(offset, run, patcher.digest_tables(rom))
                             if len(piece[1]) >= patcher.VERBATIM_WINDOW + patcher.VERBATIM_STRIDE - 1]
            index = patcher.RetailIndex([item])
            index.search(retail)
            with self.subTest(offset=offset):
                if expected is None:
                    self.assertIsNone(item.problem())
                else:
                    self.assertIn(expected, item.problem())


class AddressTableTests(unittest.TestCase):
    """A literal may match another cartridge only with a run of ARM9 code addresses."""

    table = b"".join((0x0205F445 + 8 * k).to_bytes(4, "little") for k in range(16))

    def check(self, literal, retail, base, offset=0x100):
        item = RetailIndexTests.Item(literal)
        item.base, item.literals = "own", [(offset, literal)]
        index = patcher.RetailIndex([item])
        index.search(retail, base)
        return item.problem()

    def test_an_address_table_may_match_another_cartridge(self):
        retail = RetailIndexTests.noise(b"r", 1000) + self.table + RetailIndexTests.noise(b"s", 1000)
        literal = RetailIndexTests.noise(b"a", 9) + self.table[1:] + RetailIndexTests.noise(b"b", 9)
        self.assertIsNone(self.check(literal, retail, "other", offset=0x100 - 9 + 1 - 4))
        self.assertIn("retail ROM content", self.check(literal, retail, "own", offset=0x100 - 9 + 1 - 4))

    def test_other_runs_still_fail_against_another_cartridge(self):
        retail = RetailIndexTests.noise(b"r", 4096)
        literal = RetailIndexTests.noise(b"a", 5) + retail[100:160] + RetailIndexTests.noise(b"b", 5)
        self.assertIn("retail ROM content", self.check(literal, retail, "other"))
        mixed = self.table[:24] + retail[100:140]
        self.assertIn("retail ROM content", self.check(mixed, self.table[:24] + retail[100:140] + bytes(8), "other"))

    def test_addresses_only_reads_aligned_words(self):
        self.assertTrue(patcher.addresses_only(0x1000, self.table))
        self.assertTrue(patcher.addresses_only(0x1003, self.table[3:]))
        self.assertFalse(patcher.addresses_only(0x1000, self.table[2:]))
        self.assertFalse(patcher.addresses_only(0x1000, b"\x00\x00\x00\x03" + self.table))


if __name__ == "__main__":
    unittest.main()
