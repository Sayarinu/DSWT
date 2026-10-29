import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[1]))
sys.path.insert(0, str(HERE.parents[2] / "bps-patch/__tests__"))
import candidate
import patcher
from test_patcher import BASES, PROFILE, TARGETS, FakeFlips

DOCUMENTS = {"README.md": b"Apply only to a verified dump.\n", "LICENSE": b"MIT\n", "NOTICE": b"Notices\n"}
CONTENTS = {"instructions": ["README.md"], "notices": ["LICENSE", "NOTICE"]}


class CandidateFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for name, data in {**DOCUMENTS, "client/upstream.json": b'{"repository": "upstream", "revision": "' + b"b" * 40 + b'"}\n',
                           ".gitignore": b"game/\n", "contents.json": json.dumps(CONTENTS).encode()}.items():
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_bytes(data)
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qm", "fixture")
        (self.root / "game").mkdir()
        inputs = self.root / "game/inputs"
        inputs.mkdir()
        (inputs / "profile.json").write_text(json.dumps(PROFILE))
        pairs = []
        for key in BASES:
            (inputs / f"{key}.nds").write_bytes(BASES[key])
            (inputs / f"{key}-wt.nds").write_bytes(TARGETS[key])
            pairs.append((inputs / f"{key}.nds", inputs / f"{key}-wt.nds"))
        catalog = {"variants": [{"id": key, "name": key, "size": len(data), "sha256": candidate.sha256(data),
                                 "pristine_verified": True, "planned_patch": f"platinum-wonder-trade-{key}.bps"}
                                for key, data in BASES.items()]}
        self.bundle = self.root / "game/bundle"
        patcher.create(pairs, inputs / "profile.json", self.bundle, "0.1.0-rc1", FakeFlips(),
                       patcher.source_state(self.root), catalog, self.root)

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.root), *arguments], check=True, capture_output=True, text=True).stdout

    def assemble(self, name="candidate.tar"):
        return candidate.assemble(self.bundle, self.root / "game" / name, self.root / "contents.json", self.root)

    def verify(self, path):
        return candidate.verify(path, self.root / "contents.json", self.root)

    def members(self, data):
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            return {info.name: archive.extractfile(info).read() for info in archive}


class AssemblyTests(CandidateFixture):
    def test_candidate_is_deterministic_private_and_explicit(self):
        first, second = self.assemble(), self.assemble("again.tar")
        data = Path(first["archive"]).read_bytes()
        self.assertEqual(data, Path(second["archive"]).read_bytes())
        self.assertEqual(self.verify(Path(first["archive"]))["sha256"], first["sha256"])
        names = sorted(self.members(data))
        prefix = "platinumwt-0.1.0-rc1/"
        self.assertEqual(names, sorted(prefix + name for name in [
            "CANDIDATE.json", "bundle/manifest.json", "bundle/platinum-wonder-trade-us-rev0.bps",
            "bundle/platinum-wonder-trade-us-rev1.bps", "instructions/README.md", "notices/LICENSE", "notices/NOTICE"]))
        self.assertFalse(any("audit/" in name or name.endswith(".nds") for name in names))
        record = first["manifest"]
        self.assertIs(record["publication_permitted"], False)
        self.assertIs(record["distribution_blocked"], True)
        self.assertEqual(record["source"]["commit"], self.git("rev-parse", "HEAD").strip())

    def test_dirty_untracked_or_mismatched_sources_are_refused(self):
        (self.root / "README.md").write_bytes(b"changed\n")
        with self.assertRaisesRegex(candidate.CandidateError, "clean"):
            self.assemble()
        self.git("checkout", "--", "README.md")
        (self.root / "contents.json").write_text(json.dumps({"instructions": ["game/inputs/us-rev0.nds"], "notices": []}))
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qam", "list a rom")
        with self.assertRaisesRegex(candidate.CandidateError, "different source commit"):
            self.assemble()

    def test_untracked_or_prohibited_documents_are_refused(self):
        source = patcher.source_state(self.root)
        with patch.object(candidate.patcher, "source_state", return_value=source):
            (self.root / "contents.json").write_text(json.dumps({"instructions": ["save.sav"], "notices": []}))
            self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-qam", "list")
            source["commit"] = self.git("rev-parse", "HEAD").strip()
            manifest = json.loads((self.bundle / "manifest.json").read_text())
            with patch.object(candidate, "bundle_files", return_value=(dict(manifest, source={"commit": source["commit"]}), {})), \
                    self.assertRaisesRegex(candidate.CandidateError, "tracked regular file"):
                self.assemble()
        with self.assertRaisesRegex(candidate.CandidateError, "prohibited"):
            candidate.member_name("platinumwt-1", "instructions/game.nds")
        with self.assertRaises(ValueError):
            candidate.member_name("platinumwt-1", "../escape")

    def test_interrupted_assembly_publishes_nothing_and_keeps_existing_outputs(self):
        output = self.root / "game/candidate.tar"
        with patch.object(candidate, "render", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            self.assemble()
        self.assertEqual(sorted(path.name for path in (self.root / "game").iterdir()), ["bundle", "inputs"])
        output.write_bytes(b"keep")
        with self.assertRaises(patcher.PatchError):
            self.assemble()
        self.assertEqual(output.read_bytes(), b"keep")
        with patch.object(candidate.patcher.Stage, "publish", side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
            self.assemble("other.tar")
        self.assertFalse(any(".partial-" in path.name for path in (self.root / "game").iterdir()))


class TamperTests(CandidateFixture):
    def setUp(self):
        super().setUp()
        self.archive = Path(self.assemble()["archive"])
        self.original = self.members(self.archive.read_bytes())
        self.prefix = "platinumwt-0.1.0-rc1/"

    def rejected(self, members, message):
        path = self.root / "game/tampered.tar"
        path.unlink(missing_ok=True)
        path.write_bytes(candidate.render(members))
        path.chmod(0o600)
        with self.assertRaisesRegex((candidate.CandidateError, patcher.PatchError), message):
            self.verify(path)

    def test_modified_extra_missing_or_enabled_members_are_rejected(self):
        patch_name = self.prefix + "bundle/platinum-wonder-trade-us-rev0.bps"
        changed = dict(self.original)
        changed[self.prefix + "notices/NOTICE"] = b"other\n"
        self.rejected(changed, "modified")
        self.rejected(dict(self.original, **{self.prefix + "notices/EXTRA": b"x"}), "file list differs")
        missing = dict(self.original)
        del missing[patch_name]
        self.rejected(missing, "file list differs")
        record = json.loads(self.original[self.prefix + "CANDIDATE.json"])
        self.rejected(dict(self.original, **{self.prefix + "CANDIDATE.json": candidate.canonical_json(
            dict(record, publication_permitted=True))}), "blocked from publication")
        corrupt = bytearray(self.original[patch_name])
        corrupt[-5] ^= 1
        entry = next(item for item in record["files"] if item["path"] == "bundle/platinum-wonder-trade-us-rev0.bps")
        entry["sha256"] = candidate.sha256(bytes(corrupt))
        self.rejected(dict(self.original, **{patch_name: bytes(corrupt),
                                             self.prefix + "CANDIDATE.json": candidate.canonical_json(record)}), "invalid")

    def test_non_canonical_links_and_traversal_are_rejected(self):
        data = bytearray(self.archive.read_bytes())
        data[136:148] = b"00000000001\x00"  # The first member's mtime field.
        path = self.root / "game/headers.tar"
        path.write_bytes(bytes(data))
        path.chmod(0o600)
        with self.assertRaisesRegex(candidate.CandidateError, "canonical|Unreadable"):
            self.verify(path)
        for kind, name in ((tarfile.SYMTYPE, self.prefix + "notices/link"), (tarfile.REGTYPE, "platinumwt-0.1.0-rc1/../x")):
            output = io.BytesIO()
            with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                info = tarfile.TarInfo(name)
                info.type = kind
                info.linkname = "/etc/passwd" if kind == tarfile.SYMTYPE else ""
                archive.addfile(info, io.BytesIO(b""))
            path = self.root / f"game/{kind!r}.tar"
            path.write_bytes(output.getvalue())
            path.chmod(0o600)
            with self.assertRaises((candidate.CandidateError, ValueError)):
                self.verify(path)


if __name__ == "__main__":
    unittest.main()
