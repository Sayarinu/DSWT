import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
import zipfile
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1] / "bps-patch"))
sys.path.insert(0, str(HERE.parents[1] / "bps-patch/__tests__"))
import build_app
import dswt_patcher
import patcher
import release
from support import BASES, TARGETS, USED, rom
from test_patcher import Fixture


class AppFixture(Fixture):
    def setUp(self):
        super().setUp()
        self.create()
        self.bundle = self.root / "game/bundle"
        for name, relative in patcher.RELEASE_DOCUMENTS.items():
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.root / relative).write_text(f"{name} text")
        self.archive = Path(patcher.package(self.bundle, self.root / "game/dswt-0.1.0.zip", root=self.root)["archive"])

    def run_app(self, *arguments):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = dswt_patcher.main(["--bundle", str(self.bundle), *arguments])
        return code, out.getvalue(), err.getvalue()


class CommandLineTests(AppFixture):
    def test_patches_a_trimmed_dump_beside_it(self):
        source = self.write("Platinum.nds", BASES["us-rev1"][:USED])
        code, out, _ = self.run_app("--input", str(source))
        self.assertEqual(code, 0)
        self.assertIn("Patched Pokémon Platinum us-rev1", out)
        self.assertEqual((self.inputs / "Platinum (Wonder Trade).nds").read_bytes(), TARGETS["us-rev1"])
        self.assertEqual(source.read_bytes(), BASES["us-rev1"][:USED])

    def test_explains_refusals_without_writing(self):
        for name, data, message in (("french", rom(0, "fr", code="CPUF"), "France, French, Rev 0) is not supported yet"),
                                    ("patched", TARGETS["us-rev0"], "already has DSWT"),
                                    ("heartgold", rom(0, "hg", code="IPKE"), "Pokémon HeartGold (North America, English, Rev 0) is not"),
                                    ("other", b"x" * 1024, "not a Pokémon Diamond, Pearl, Platinum, HeartGold or SoulSilver ROM")):
            with self.subTest(name):
                source = self.write(f"{name}.nds", data)
                code, _, err = self.run_app("--input", str(source), "--output", str(self.root / "out.nds"))
                self.assertEqual(code, 1)
                self.assertIn(message, err)
                self.assertFalse((self.root / "out.nds").exists())

    def test_identifies_for_the_window(self):
        manifest = dswt_patcher.load(self.bundle)
        self.assertEqual(dswt_patcher.identify(manifest, self.pairs[0][0])["variant"], "us-rev0")
        with self.assertRaisesRegex(ValueError, "Japan"):
            dswt_patcher.identify(manifest, self.write("jp.nds", rom(0, "jp", code="CPUJ")))

    def test_self_test_checks_every_patch(self):
        code, out, _ = self.run_app("--self-test")
        result = json.loads(out)
        self.assertEqual((code, result["version"], result["variants"]), (0, "0.1.0", ["us-rev0", "us-rev1"]))
        self.assertIsInstance(result["window"], bool)
        path = self.bundle / "dswt-us-rev0.bps"
        data = path.read_bytes()
        path.unlink()
        path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        code, _, err = self.run_app("--self-test")
        self.assertEqual(code, 1)
        self.assertIn("does not match", err)

    def test_output_needs_input_and_missing_bundle_is_reported(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_app("--output", "x.nds")
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(dswt_patcher.main(["--self-test"]), 1)
        self.assertIn("No release bundle", err.getvalue())


class ArchiveTests(AppFixture):
    def rewrite(self, change):
        with zipfile.ZipFile(self.archive) as archive:
            members = {info.filename: archive.read(info) for info in archive.infolist()}
        change(members)
        path = self.root / "game/changed.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        return path

    def test_accepts_the_packaged_archive(self):
        manifest, members = build_app.read_archive(self.archive, "0.1.0")
        self.assertEqual(sorted(members), ["INSTALL.md", "LICENSE", "NOTICE", "dswt-us-rev0.bps",
                                           "dswt-us-rev1.bps", "manifest.json"])
        self.assertEqual(manifest["version"], "0.1.0")

    def test_rejects_changed_archives(self):
        prefix = "dswt-0.1.0/"
        cases = {
            "extra member": lambda m: m.update({prefix + "audit.json": b"{}"}),
            "missing document": lambda m: m.pop(prefix + "NOTICE"),
            "nested path": lambda m: m.update({prefix + "audit/x.bin": b""}),
            "second directory": lambda m: m.update({"other/LICENSE": b""}),
            "changed patch": lambda m: m.update({prefix + "dswt-us-rev1.bps": m[prefix + "dswt-us-rev1.bps"][:-1] + b"\0"}),
            "bad manifest": lambda m: m.update({prefix + "manifest.json": b"{}"}),
        }
        for name, change in cases.items():
            with self.subTest(name), self.assertRaises(ValueError):
                build_app.read_archive(self.rewrite(change))
        with self.assertRaisesRegex(ValueError, "version"):
            build_app.read_archive(self.archive, "0.2.0")


class FakeGitHub:
    def __init__(self, draft=True, assets=("dswt-0.1.0.zip",), archive=None):
        self.draft, self.assets, self.archive, self.calls = draft, list(assets), archive, []

    def __call__(self, *arguments, capture=True):
        self.calls.append(arguments)
        if arguments[:2] == ("release", "view"):
            return json.dumps({"isDraft": self.draft, "assets": [{"name": name} for name in self.assets]})
        if arguments[:2] == ("release", "download"):
            directory = Path(arguments[arguments.index("--dir") + 1])
            (directory / "dswt-0.1.0.zip").write_bytes(self.archive.read_bytes())
        return ""


class ReleaseTests(AppFixture):
    def test_draft_tags_the_manifest_commit(self):
        github = FakeGitHub()
        with patch.object(release, "gh", github):
            result = release.draft("0.1.0", self.archive, "owner/repo")
        self.assertEqual(result["commit"], "a" * 40)
        create = github.calls[-1]
        self.assertEqual(create[:5], ("release", "create", "v0.1.0", str(self.archive), "--repo"))
        self.assertIn("--draft", create)
        self.assertEqual(create[create.index("--target") + 1], "a" * 40)
        notes = create[create.index("--notes") + 1]
        self.assertIn("| Pokémon Platinum us-rev1 |", notes)
        self.assertNotIn("only the DSWT Patcher", notes)
        manifest = {"version": "0.1.0", "source": {"commit": "a" * 40}, "upstream": {"revision": "b" * 40},
                    "entries": [{"name": name, "game_code": code, "input": {"sha256": "c" * 64}, "source_view": view}
                                for name, code, view in (("USA", "CPUE", "rom"), ("USA", "IPKE", "expanded-code"),
                                                         ("USA", "IPGE", "expanded-code"))]}
        self.assertIn("Pokémon HeartGold and Pokémon SoulSilver patches apply to the ROM with its code expanded",
                      release.notes(manifest))
        renamed = self.root / "game/other.zip"
        renamed.write_bytes(self.archive.read_bytes())
        with patch.object(release, "gh", FakeGitHub()), self.assertRaisesRegex(ValueError, "Name the archive"):
            release.draft("0.1.0", renamed, "owner/repo")

    def test_fetch_requires_a_draft_holding_only_the_archive(self):
        output = self.root / "game/fetched"
        with patch.object(release, "gh", FakeGitHub(archive=self.archive)):
            self.assertEqual(release.fetch("0.1.0", "owner/repo", output)["variants"], ["us-rev0", "us-rev1"])
        for github in (FakeGitHub(draft=False, archive=self.archive),
                       FakeGitHub(assets=("dswt-0.1.0.zip", "extra.exe"), archive=self.archive),
                       FakeGitHub(assets=(), archive=self.archive)):
            with patch.object(release, "gh", github), self.assertRaises(ValueError):
                release.fetch("0.1.0", "owner/repo", self.root / "game/again")

    def test_publish_uploads_every_app_with_sums(self):
        directory = self.root / "game/publish"
        directory.mkdir()
        (directory / "dswt-0.1.0.zip").write_bytes(self.archive.read_bytes())
        for name in release.PLATFORMS:
            (directory / f"dswt-patcher-0.1.0-{name}.zip").write_bytes(name.encode())
        github = FakeGitHub()
        with patch.object(release, "gh", github):
            result = release.publish("0.1.0", "owner/repo", directory)
        self.assertEqual(len(result["files"]), 5)
        upload = github.calls[-2]
        self.assertEqual(sorted(Path(item).name for item in upload[3:-2]),
                         ["SHA256SUMS", *(f"dswt-patcher-0.1.0-{name}.zip" for name in release.PLATFORMS)])
        self.assertEqual(github.calls[-1][:3], ("release", "edit", "v0.1.0"))
        sums = (directory / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(sums), 4)
        (directory / "SHA256SUMS").unlink()
        (directory / "dswt-patcher-0.1.0-linux.zip").unlink()
        with patch.object(release, "gh", FakeGitHub()), self.assertRaisesRegex(ValueError, "differ"):
            release.publish("0.1.0", "owner/repo", directory)

    def test_cli_validates_version_and_repository(self):
        for arguments in (["fetch", "--version", "v1", "--output", "x"],
                          ["--repo", "a/b;rm", "fetch", "--version", "0.1.0", "--output", "x"]):
            with self.subTest(arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                release.main(arguments)


if __name__ == "__main__":
    unittest.main()
