import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "prepare_game", Path(__file__).resolve().parents[1] / "prepare-game.py"
)
prepare_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_game)


class PrepareGameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="platinumwt-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "project"
        self.upstream = Path(self.temp.name) / "upstream"
        self.upstream.mkdir()
        self.git("init", "--quiet")
        for name, text in {
            "src/start_menu.c": "original\n",
            "untouched.txt": "unchanged\n",
            "platinum.us/rom_rev0.sha1": "0" * 40 + " *pokeplatinum.us.nds\n",
            "platinum.us/rom_rev1.sha1": "1" * 40 + " *pokeplatinum.us.nds\n",
        }.items():
            path = self.upstream / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        self.git("add", ".")
        self.git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "--quiet", "-m", "Synthetic upstream")
        self.pin = {
            "repository": str(self.upstream),
            "revision": self.git("rev-parse", "HEAD").decode().strip(),
            "tree": self.git("rev-parse", "HEAD^{tree}").decode().strip(),
            "rom_sha1": {"0": "0" * 40, "1": "1" * 40},
        }
        client = self.root / "client"
        (client / "src/wonder_trade").mkdir(parents=True)
        (client / "src/wonder_trade/application.c").write_text("project addition\n")
        (client / "upstream.json").write_text(json.dumps(self.pin))
        (client / "pokeplatinum.patch").write_text(
            "diff --git a/src/start_menu.c b/src/start_menu.c\n"
            "--- a/src/start_menu.c\n+++ b/src/start_menu.c\n"
            "@@ -1 +1 @@\n-original\n+patched\n"
        )

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.upstream), *args])

    def test_fresh_and_repeated_setup_without_network(self):
        workspace = prepare_game.prepare(self.root)
        self.assertEqual((workspace / "src/start_menu.c").read_text(), "patched\n")
        self.assertEqual((workspace / "src/wonder_trade/application.c").read_text(), "project addition\n")
        self.assertEqual((self.upstream / "src/start_menu.c").read_text(), "original\n")
        shutil.rmtree(self.upstream)
        self.assertEqual(prepare_game.prepare(self.root), workspace)

    def test_local_edits_are_preserved(self):
        workspace = prepare_game.prepare(self.root)
        for name in ("src/start_menu.c", "src/wonder_trade/application.c", "untouched.txt"):
            with self.subTest(name=name):
                path = workspace / name
                original = path.read_bytes()
                path.write_text("developer changes\n")
                with self.assertRaises(RuntimeError):
                    prepare_game.prepare(self.root)
                self.assertEqual(path.read_text(), "developer changes\n")
                path.write_bytes(original)

    def test_local_deletion_is_preserved(self):
        workspace = prepare_game.prepare(self.root)
        path = workspace / "src/start_menu.c"
        path.unlink()
        with self.assertRaisesRegex(RuntimeError, "Local deletion"):
            prepare_game.prepare(self.root)
        self.assertFalse(path.exists())

    def test_unmanaged_files_are_preserved(self):
        workspace = prepare_game.prepare(self.root)
        path = workspace / "local-notes.txt"
        path.write_text("developer notes\n")
        with self.assertRaisesRegex(RuntimeError, "Unmanaged local files"):
            prepare_game.prepare(self.root)
        self.assertEqual(path.read_text(), "developer notes\n")

    def test_incompatible_revision_is_rejected(self):
        workspace = prepare_game.prepare(self.root)
        subprocess.check_call([
            "git", "-C", str(workspace), "-c", "user.name=Fixture",
            "-c", "user.email=fixture@example.invalid", "commit", "--allow-empty",
            "--quiet", "-m", "Different revision",
        ])
        with self.assertRaisesRegex(RuntimeError, "incompatible upstream revision"):
            prepare_game.prepare(self.root)

    def test_failed_patch_does_not_install_a_partial_workspace(self):
        patch = self.root / "client/pokeplatinum.patch"
        patch.write_text(patch.read_text().replace("-original", "-wrong input"))
        with self.assertRaises(subprocess.CalledProcessError):
            prepare_game.prepare(self.root)
        self.assertFalse((self.root / "game/pokeplatinum").exists())

    def test_pristine_workspace_is_separate_and_unmodified(self):
        modified = prepare_game.prepare(self.root)
        pristine = prepare_game.prepare(self.root, pristine=True)
        self.assertNotEqual(modified, pristine)
        self.assertEqual((pristine / "src/start_menu.c").read_text(), "original\n")
        self.assertFalse((pristine / "src/wonder_trade/application.c").exists())
        self.assertEqual(prepare_game.prepare(self.root, pristine=True), pristine)

    def test_workspace_symlink_does_not_write_outside_project(self):
        outside = Path(self.temp.name) / "outside"
        outside.mkdir()
        (self.root / "game").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            prepare_game.prepare(self.root)
        self.assertEqual(list(outside.iterdir()), [])

    def test_overlay_symlink_does_not_change_external_file(self):
        workspace = prepare_game.prepare(self.root)
        outside = Path(self.temp.name) / "outside.c"
        outside.write_text("external content\n")
        path = workspace / "src/wonder_trade/application.c"
        path.unlink()
        path.symlink_to(outside)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            prepare_game.prepare(self.root)
        self.assertEqual(outside.read_text(), "external content\n")


if __name__ == "__main__":
    unittest.main()
