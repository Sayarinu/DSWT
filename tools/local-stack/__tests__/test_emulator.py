import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest


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
