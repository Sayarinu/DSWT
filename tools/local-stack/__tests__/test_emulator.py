import importlib.util
import fcntl
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


DIRECTORY = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("local_emulator", DIRECTORY / "emulator.py")
emulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emulator)


class EmulatorTests(unittest.TestCase):
    def image(self, first, second):
        raw = bytearray(524288)
        raw[0x30000:0x31000], raw[0x70000:0x71000] = first, second
        return raw

    def test_journal_selects_intact_bank_and_refuses_equal_generation_conflicts(self):
        first = emulator.PROVISION.seed_record(b"a" * 32, 7)
        second = emulator.PROVISION.seed_record(b"b" * 32, 8)
        self.assertEqual(emulator.journal(self.image(first, second)), second)
        damaged = bytearray(second); damaged[50] ^= 1
        self.assertEqual(emulator.journal(self.image(first, damaged)), first)
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            emulator.journal(self.image(first, emulator.PROVISION.seed_record(b"b" * 32, 7)))
        for raw in (b"short", bytes(524288)):
            with self.subTest(size=len(raw)), self.assertRaises(ValueError): emulator.journal(raw)

    def test_profile_names_cannot_escape_the_owned_workspace(self):
        local = SimpleNamespace(workspace=Path("/private/game/run"), manifest={"project": "owned"})
        for name in ("../original", "/original", "owner/a", "a,b", "-a", "a" * 33):
            with self.subTest(name=name), self.assertRaises(ValueError): emulator.Emulator(local, name)

    def test_foreign_container_labels_are_never_treated_as_owned(self):
        value = {"Config": {"Labels": {emulator.stack.LABEL: "other", emulator.OWNER_LABEL: "a"}}}
        local = SimpleNamespace(workspace=Path("/private/game/run"), manifest={"project": "owned"},
                                run=lambda *args, **kwargs: SimpleNamespace(returncode=0, stdout=json.dumps([value])))
        runner = emulator.Emulator(local, "a")
        with self.assertRaisesRegex(ValueError, "another owner"): runner.owned_container()
        value["Config"]["Labels"][emulator.stack.LABEL] = "owned"
        self.assertEqual(runner.owned_container(), value)

    def test_failed_container_inspection_does_not_authorize_replacement(self):
        replies = iter([SimpleNamespace(returncode=1, stdout=""), SimpleNamespace(returncode=0, stdout="owned-emulator-a\n")])
        local = SimpleNamespace(workspace=Path("/private/game/run"), manifest={"project": "owned"},
                                run=lambda *args, **kwargs: next(replies))
        with self.assertRaisesRegex(ValueError, "Cannot inspect"):
            emulator.Emulator(local, "a").owned_container()

    def test_snapshot_refuses_an_exclusive_standard_save_lock_then_copies_coherently(self):
        fixture = emulator.stack.load_module("snapshot_fixture", emulator.stack.ROOT / "scripts/tests/test_verify_demo_trade.py")
        with tempfile.TemporaryDirectory() as temporary:
            local = SimpleNamespace(workspace=Path(temporary).resolve(), manifest={"project": "owned"})
            runner = emulator.Emulator(local, "a")
            runner.directory.mkdir(parents=True)
            runner.owned_container = lambda: None
            runner.manifest = lambda: {"profile": "a"}
            raw = fixture.retail([fixture.pk(10, 1), fixture.pk(20, 2)], fixture.pk(30, 3))
            fixture.idle(raw, 1)
            save = runner.directory / "platinum.sav"
            fixture.publication(save, raw, 1)
            lock = Path(str(save) + ".platinumwt.lock"); lock.chmod(0o600)
            with lock.open("r+b") as writer:
                fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaises(BlockingIOError): runner.snapshot("blocked")
                self.assertFalse((runner.directory / "snapshots").exists())
            result = runner.snapshot("released")
            self.assertEqual((result / "platinum.sav").read_bytes(), raw)
            report = json.loads((result / "manifest.json").read_text())
            self.assertEqual(report["files"], {name: emulator.digest((result / name).read_bytes()) for name in ("platinum.sav", "A", "B", "HEAD")})

    def test_independent_profile_controls_share_the_stack_lock_and_exclude_their_own_writer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with emulator.private_lock(root / "stack", fcntl.LOCK_SH), emulator.private_lock(root / "a", fcntl.LOCK_EX):
                with emulator.private_lock(root / "stack", fcntl.LOCK_SH), emulator.private_lock(root / "b", fcntl.LOCK_EX): pass
                for path in (root / "a", root / "stack"):
                    with self.subTest(path=path.name), self.assertRaises(BlockingIOError):
                        with emulator.private_lock(path, fcntl.LOCK_EX): pass
            with emulator.private_lock(root / "stack", fcntl.LOCK_EX): pass

    def test_fault_markers_remain_distinct_when_stdout_has_no_final_newline(self):
        local = SimpleNamespace(workspace=Path("/private/game/run"), manifest={"project": "owned"},
                                run=lambda *args, **kwargs: SimpleNamespace(stdout="partial wifi line", stderr="PHASE_READY\nSTOPPED\n"))
        runner = emulator.Emulator(local, "a")
        runner.manifest = lambda: {"last_start": {"fault_at": 1, "fault_mode": "before", "fault_phase": 3, "container": "instance"}}
        runner.owned_container = lambda: {"Id": "instance", "State": {"Running": True}}
        killed = []
        runner.kill = lambda: killed.append(True)
        with patch.object(emulator.stack, "save_manifest") as save:
            runner.kill_at_fault(1)
        self.assertEqual(killed, [True])
        self.assertTrue(save.call_args.args[1]["last_start"]["observed_fault_and_sigkill"])
