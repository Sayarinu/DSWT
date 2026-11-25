import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
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
from support import BASES, PINS, TARGETS, USED, arm9, rom, with_arm9
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
                                    ("other", b"x" * 1024, "not a Pokémon Diamond, Pearl, Platinum, HeartGold, SoulSilver, Black, White, Black 2 or White 2 ROM")):
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

    def test_patches_a_gen5_dump_without_an_arm9_footer(self):
        # Black 2 and White 2 compress ARM9 with no Nitro footer after it; the app expands it
        # from the player's dump, as create did, and writes the exact target.
        retail_code = (hashlib.sha512(b"gen5 arm9").digest() * 2)[:81] + bytes(40)
        base = with_arm9(rom(0, "black2", code="IREO"), arm9(retail_code, 40), footer=False)
        target = with_arm9(rom(0, "black2-wt", code="IREO"),
                           arm9(b"wt.dswt.games:443" + PINS[0] + PINS[1] + bytes(40), 40), footer=False)
        self.catalog["variants"].append(
            {"id": "black2-us", "game": "Pokémon Black 2", "name": "USA, Europe", "game_code": "IREO", "revision": 0,
             "size": len(base), "sha256": hashlib.sha256(base).hexdigest(), "pristine_verified": True,
             "planned_patch": "dswt-black2-us.bps"})
        bundle_dir = self.root / "game/gen5"
        self.create(pairs=[(self.write("b2.nds", base), self.write("b2-wt.nds", target))], output=bundle_dir)
        source = self.write("Black 2.nds", base)
        output = self.root / "Black 2 (Wonder Trade).nds"
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = dswt_patcher.main(["--bundle", str(bundle_dir), "--input", str(source), "--output", str(output)])
        self.assertEqual((code, err.getvalue()), (0, ""))
        self.assertIn("Patched Pokémon Black 2 USA, Europe", out.getvalue())
        self.assertEqual(output.read_bytes(), target)
        self.assertEqual(source.read_bytes(), base)

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


UPSTREAMS = {"upstream.json": {"repository": "https://github.com/pret/pokeplatinum.git", "revision": "c" * 40},
             "upstream-aletheia.json": {"repository": "https://github.com/Project-Holon/aletheia.git",
                                        "revision": "d" * 40}}


class FakeGitHub:
    def __init__(self, draft=True, assets=("dswt-0.1.0.zip",), archive=None, releases=(), merges="", body="",
                 upstreams=None):
        self.draft, self.assets, self.archive, self.calls = draft, list(assets), archive, []
        self.releases, self.merges, self.body = list(releases), merges, body
        self.upstreams = dict(upstreams if upstreams is not None else UPSTREAMS)

    def __call__(self, *arguments, capture=True):
        self.calls.append(arguments)
        if arguments[:2] == ("release", "view"):
            return json.dumps({"isDraft": self.draft, "assets": [{"name": name} for name in self.assets],
                               "body": self.body})
        if arguments[:2] == ("release", "list"):
            return json.dumps([{"tagName": tag} for tag in self.releases])
        if arguments[0] == "api" and "/compare/" in arguments[1]:
            return self.merges
        if arguments[0] == "api" and "/contents/client?ref=" in arguments[1]:
            return json.dumps([{"name": name} for name in [*self.upstreams, "README.md", "upstream-notes.txt"]])
        if arguments[0] == "api" and "/contents/client/" in arguments[1]:
            return json.dumps(self.upstreams[arguments[1].split("/contents/client/")[1].split("?")[0]])
        if arguments[:2] == ("release", "download"):
            directory = Path(arguments[arguments.index("--dir") + 1])
            (directory / "dswt-0.1.0.zip").write_bytes(self.archive.read_bytes())
        if arguments[:2] == ("release", "upload"):
            self.assets += [Path(item).name for item in arguments[3:arguments.index("--repo")]]
        if arguments[:2] == ("release", "delete-asset"):
            self.assets.remove(arguments[3])
        return ""


class SyntheticBundleTests(unittest.TestCase):
    """The frozen apps' self-test cartridge must patch through the player's applier.
    CI skips building a frozen app, so this runs the same bundle unfrozen."""

    def test_self_test_bundle_patches(self):
        for attempt in range(20):  # the cartridge is random: try several
            with self.subTest(attempt=attempt), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp) / "bundle"
                source, target = build_app.synthetic_bundle(directory)
                rom = Path(temp) / "trimmed.nds"
                rom.write_bytes(source)
                self.assertEqual(build_app.bundle.compressed_code(source, "ROM"), [])
                _, output = build_app.bundle.patch_file(directory, rom)
                self.assertEqual(output.read_bytes(), target)


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
        self.assertNotIn("SHA-256", notes)
        self.assertIn("## Changes\n\nFirst release.", notes)
        self.assertIn("/wiki/Language-support", notes)
        self.assertIn("Download one file, DSWT Patcher for your computer", notes)
        self.assertIn("`dswt-patcher-0.1.0-windows.zip`", notes)
        self.assertIn("apply only through DSWT Patcher", notes)
        self.assertNotIn("any\nBPS patcher", notes)
        self.assertIn("SoulSilver, and the USA/Europe versions of Black, White, Black 2 and White 2. Black, White,\n"
                      "Black 2 and White 2 run in DS mode only.", notes)
        self.assertIn(f"Source: {'a' * 40}. Upstreams: aletheia ddddddd, pokeplatinum ccccccc.\n", notes)
        self.assertIn(("api", "repos/owner/repo/contents/client?ref=" + "a" * 40), github.calls)
        renamed = self.root / "game/other.zip"
        renamed.write_bytes(self.archive.read_bytes())
        with patch.object(release, "gh", FakeGitHub()), self.assertRaisesRegex(ValueError, "Name the archive"):
            release.draft("0.1.0", renamed, "owner/repo")

    def test_notes_list_pull_requests_since_the_previous_release(self):
        merges = "\n".join(["LUNE-1 - Merge current main", "LUNE-2 - Fix trade recovery (#7)",
                            "Bump actions/checkout (#8)"])
        github = FakeGitHub(releases=["v0.2.0", "v0.1.1", "v0.1.0", "v0.1.1-rc.1", "latest"], merges=merges)
        with patch.object(release, "gh", github):
            previous, titles = release.changes("0.2.0", "b" * 40, "owner/repo")
        self.assertEqual(previous, "v0.1.1")
        self.assertIn(("api", "repos/owner/repo/compare/v0.1.1..." + "b" * 40), [call[:2] for call in github.calls])
        self.assertEqual(titles, ["Fix trade recovery (#7)", "Bump actions/checkout (#8)"])
        manifest = {"version": "0.2.0", "source": {"commit": "b" * 40}}
        notes = release.notes(manifest, previous, titles)
        self.assertIn("## Changes since v0.1.1\n\n- Fix trade recovery (#7)\n- Bump actions/checkout (#8)\n", notes)
        self.assertIn("No changes to the game patches.", release.notes(manifest, previous, []))
        with patch.object(release, "gh", FakeGitHub(releases=["v0.1.0"])):
            self.assertEqual(release.changes("0.1.0", "b" * 40, "owner/repo"), (None, []))
        with patch.object(release, "gh", FakeGitHub(releases=["v0.1.0"])):
            self.assertEqual(release.previous_release("0.1.0-rc.1", "owner/repo"), None)
            self.assertEqual(release.previous_release("0.1.1-rc.1", "owner/repo"), "v0.1.0")

    def test_notes_rewrites_a_published_release(self):
        # Older notes name one upstream; the rewrite lists every upstream the source commit pins.
        rewritten = f"Source: {'b' * 40}. Upstreams: aletheia ddddddd, pokeplatinum ccccccc."
        for source in (f"Source: {'b' * 40}, upstream {'c' * 40}.", rewritten):
            with self.subTest(source=source):
                github = FakeGitHub(draft=False, releases=["v0.1.0"], merges="LUNE-3 - Add a thing (#9)",
                                    body=f"| table |\n\n{source}\n")
                with patch.object(release, "gh", github):
                    result = release.rewrite("0.1.1", "owner/repo")
                edit = github.calls[-1]
                self.assertEqual(edit[:5], ("release", "edit", "v0.1.1", "--repo", "owner/repo"))
                self.assertEqual(edit[-1], result["notes"])
                self.assertIn("- Add a thing (#9)", result["notes"])
                self.assertIn(rewritten + "\n", result["notes"])
                self.assertIn(("api", "repos/owner/repo/contents/client?ref=" + "b" * 40), github.calls)
                self.assertNotIn("| table |", result["notes"])
        with patch.object(release, "gh", FakeGitHub(draft=False, releases=["v0.1.0"], body=rewritten, upstreams={})):
            with self.assertRaisesRegex(ValueError, "pins no upstream"):
                release.rewrite("0.1.1", "owner/repo")
        with patch.object(release, "gh", FakeGitHub(body="no source")), self.assertRaisesRegex(ValueError, "no source line"):
            release.rewrite("0.1.1", "owner/repo")

    def test_fetch_requires_a_draft_holding_only_the_archive(self):
        output = self.root / "game/fetched"
        with patch.object(release, "gh", FakeGitHub(archive=self.archive)):
            self.assertEqual(release.fetch("0.1.0", "owner/repo", output)["variants"], ["us-rev0", "us-rev1"])
        for github in (FakeGitHub(draft=False, archive=self.archive),
                       FakeGitHub(assets=("dswt-0.1.0.zip", "extra.exe"), archive=self.archive),
                       FakeGitHub(assets=(), archive=self.archive)):
            with patch.object(release, "gh", github), self.assertRaises(ValueError):
                release.fetch("0.1.0", "owner/repo", self.root / "game/again")

    def test_publish_offers_only_the_apps_with_sums(self):
        directory = self.root / "game/publish"
        directory.mkdir()
        (directory / "dswt-0.1.0.zip").write_bytes(self.archive.read_bytes())
        for name in release.PLATFORMS:
            (directory / f"dswt-patcher-0.1.0-{name}.zip").write_bytes(name.encode())
        github = FakeGitHub()
        with patch.object(release, "gh", github):
            result = release.publish("0.1.0", "owner/repo", directory)
        apps = [f"dswt-patcher-0.1.0-{name}.zip" for name in release.PLATFORMS]
        self.assertEqual(result["files"], sorted(["SHA256SUMS", *apps]))
        # Each app carries the patches, so the archive is removed before publishing.
        self.assertEqual(sorted(github.assets), sorted(["SHA256SUMS", *apps]))
        self.assertIn(("release", "delete-asset", "v0.1.0", "dswt-0.1.0.zip", "--repo", "owner/repo", "--yes"), github.calls)
        self.assertEqual(github.calls[-1][:3], ("release", "edit", "v0.1.0"))
        sums = (directory / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(sorted(line.split()[1] for line in sums), sorted(apps))
        self.assertEqual((directory / "SHA256SUMS").read_bytes(),
                         "".join(f"{hashlib.sha256(name.encode()).hexdigest()}  dswt-patcher-0.1.0-{name}.zip\n"
                                 for name in sorted(release.PLATFORMS)).encode())
        for name in release.PLATFORMS:
            self.assertEqual((directory / f"dswt-patcher-0.1.0-{name}.zip").read_bytes(), name.encode())
        (directory / "SHA256SUMS").unlink()
        (directory / "dswt-patcher-0.1.0-linux.zip").unlink()
        with patch.object(release, "gh", FakeGitHub()), self.assertRaisesRegex(ValueError, "differ"):
            release.publish("0.1.0", "owner/repo", directory)

    def test_companion_is_uploaded_and_hashed_only_with_explicit_opt_in(self):
        directory = self.root / "game/publish"
        directory.mkdir()
        (directory / "dswt-0.1.0.zip").write_bytes(self.archive.read_bytes())
        for name in release.PLATFORMS:
            (directory / f"dswt-patcher-0.1.0-{name}.zip").write_bytes(name.encode())
        companion = directory / "dswt-companion-0.1.0.zip"
        with patch.object(release, "gh", FakeGitHub()) as github, self.assertRaisesRegex(ValueError, "differ"):
            release.publish("0.1.0", "owner/repo", directory, companion=True)
        self.assertEqual(github.calls, [])
        companion.write_bytes(b"companion zip")
        for opt_in in (None, False):
            with self.subTest(companion=opt_in):
                github = FakeGitHub()
                kwargs = {} if opt_in is None else {"companion": opt_in}
                with patch.object(release, "gh", github), self.assertRaisesRegex(ValueError, "differ"):
                    release.publish("0.1.0", "owner/repo", directory, **kwargs)
                self.assertEqual(github.calls, [])
                self.assertFalse((directory / "SHA256SUMS").exists())
        github = FakeGitHub()
        with patch.object(release, "gh", github):
            result = release.publish("0.1.0", "owner/repo", directory, companion=True)
        expected = {"SHA256SUMS", companion.name, *(f"dswt-patcher-0.1.0-{p}.zip" for p in release.PLATFORMS)}
        self.assertEqual(set(result["files"]), expected)
        self.assertEqual(set(github.assets), expected)
        sums = (directory / "SHA256SUMS").read_text()
        self.assertIn(f"{hashlib.sha256(companion.read_bytes()).hexdigest()}  {companion.name}\n", sums)
        self.assertEqual(len(sums.splitlines()), 4)

    def test_publish_cli_defaults_companion_to_false_and_passes_explicit_boolean(self):
        for flag, expected in (([], False), (["--companion", "false"], False), (["--companion", "true"], True)):
            with self.subTest(flag=flag), patch.object(release, "publish", return_value={}) as publish:
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(release.main(["publish", "--version", "0.1.0", "--directory", "x", *flag]), 0)
                publish.assert_called_once_with("0.1.0", "Sayarinu/DSWT", Path("x"), companion=expected)

    def test_cli_validates_version_and_repository(self):
        for arguments in (["fetch", "--version", "v1", "--output", "x"],
                          ["--repo", "a/b;rm", "fetch", "--version", "0.1.0", "--output", "x"]):
            with self.subTest(arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                release.main(arguments)


if __name__ == "__main__":
    unittest.main()
