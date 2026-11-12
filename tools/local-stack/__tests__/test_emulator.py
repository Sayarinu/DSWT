import importlib.util
import fcntl
import json
import os
import re
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


DIRECTORY = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("local_emulator", DIRECTORY / "emulator.py")
emulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emulator)


class EmulatorTests(unittest.TestCase):
    def test_touch_controls_are_bounded_to_the_ds_screen(self):
        with tempfile.TemporaryDirectory() as temporary:
            local = SimpleNamespace(workspace=Path(temporary).resolve(), manifest={"project": "owned"})
            runner = emulator.Emulator(local, "touch")
            runner.directory.mkdir(parents=True)
            runner.owned_container = lambda: {"State": {"Running": True}}
            valid = [{"op": "touch", "state": "press", "x": 0, "y": 0},
                     {"op": "touch", "state": "release", "x": 255, "y": 191, "ms": 100}]
            runner.events(valid)
            controls = runner.directory / "control.json"
            self.assertEqual(json.loads(controls.read_text()), [dict(valid[0], ms=0), valid[1]])
            controls.unlink()
            for change in ({"x": -1}, {"x": 256}, {"x": True}, {"y": 192}, {"y": 1.5},
                           {"state": "move"}, {"ms": -1}, {"path": "/unowned"}):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    runner.events([dict(valid[0], **change)])
                self.assertFalse(controls.exists())

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
            lock = Path(str(save) + ".dswt.lock"); lock.chmod(0o600)
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

    def test_concurrent_profiles_reuse_only_a_finished_control_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            compiling, release = threading.Event(), threading.Event()
            calls, results, failures = [], [], []
            def compile(arguments, **kwargs):
                calls.append(arguments)
                mount = arguments[arguments.index("--mount") + 1]
                directory = Path(mount.split("source=", 1)[1].split(",target=", 1)[0])
                output = directory / arguments[-1].split("-o /support/", 1)[1].split()[0]
                output.write_bytes(b"partial linker output"); compiling.set()
                if not release.wait(5): raise TimeoutError("synthetic compiler release")
                output.write_bytes(b"complete control library")
                return SimpleNamespace(stdout="", returncode=0)
            local = SimpleNamespace(workspace=root, manifest={"project": "owned"}, run=compile)
            def start(name):
                try: results.append(emulator.Emulator(local, name).support("sha256:" + "1" * 64))
                except Exception as error: failures.append(error)
            first = threading.Thread(target=start, args=("a",)); second = threading.Thread(target=start, args=("b",))
            try:
                first.start(); self.assertTrue(compiling.wait(3))
                self.assertFalse(list(root.glob("emulator-support/*/qt-control.so")))
                second.start(); time.sleep(0.1)
                self.assertEqual(results, [])
            finally:
                release.set(); first.join(5)
                if second.ident is not None: second.join(5)
            self.assertFalse(first.is_alive()); self.assertFalse(second.is_alive())
            self.assertEqual(failures, []); self.assertEqual(len(calls), 1)
            self.assertEqual(len(results), 2); self.assertEqual(results[0], results[1])
            self.assertEqual((results[0] / "qt-control.so").read_bytes(), b"complete control library")
            ready = json.loads((results[0] / "manifest.json").read_text())
            self.assertEqual(ready["library_sha256"], emulator.digest(b"complete control library"))

    def test_save_failure_requires_the_exact_exit_and_checkpoint_without_masking_oom(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            runner = emulator.Emulator(SimpleNamespace(workspace=root, manifest={"project": "owned"}), "a")
            log = root / "run.log"
            log.write_text("PHASE_READY\nManaged save failed; emulator stopped before continuing\n")
            runner.logs = lambda: log
            runner.manifest = lambda: {"last_start": {"container": "instance", "fault_mode": "error", "fault_phase": 6}}
            for code, oom in ((1, False), (134, False), (137, False), (74, True), (74, False)):
                runner.owned_container = lambda: {"Id": "instance", "State": {"Running": False, "ExitCode": code, "OOMKilled": oom}}
                with self.subTest(code=code, oom=oom), patch.object(emulator.stack, "save_manifest"):
                    if code == 74 and not oom: runner.wait_exit(expected=74)
                    else:
                        with self.assertRaisesRegex(ValueError, "Unexpected emulator exit"): runner.wait_exit(expected=74)
            log.write_text("Managed save failed; emulator stopped before continuing\n")
            with self.assertRaisesRegex(ValueError, "selected checkpoint"): runner.wait_exit(expected=74)

    def fork_source(self, root):
        """A stopped source profile with one checked snapshot named "checked"."""
        fixture = emulator.stack.load_module("fork_fixture", emulator.stack.ROOT / "scripts/tests/test_verify_demo_trade.py")
        local = SimpleNamespace(workspace=root, manifest={"project": "owned"})
        source = emulator.Emulator(local, "a")
        source.directory.mkdir(parents=True, mode=0o700)
        source.owned_container = lambda: None
        for name, data in (("platinum.nds", b"rom"), ("melonDS", b"emulator")):
            emulator.write_private(source.directory / name, data)
        source.write_config(source.directory)
        emulator.stack.save_manifest(source.directory, {"version": 1, "profile": "a", "workspace": str(root),
            "target": "us-rev1", "source_save": "original.sav", "source_sha256": "0" * 64, "image": "sha256:image",
            "rom_sha256": emulator.digest(b"rom"), "emulator_sha256": emulator.digest(b"emulator")})
        raw = fixture.retail([fixture.pk(10, 1)], fixture.pk(30, 3))
        fixture.idle(raw, 1)
        fixture.publication(source.directory / "platinum.sav", raw, 1)
        return local, source, source.snapshot("checked")

    def test_fork_publishes_an_exact_copy_with_its_own_adapter(self):
        self.addCleanup(os.umask, os.umask(0o077))  # As the command-line entry point does.
        with tempfile.TemporaryDirectory() as temporary:
            local, source, checked = self.fork_source(Path(temporary).resolve())
            copy = emulator.Emulator(local, "copy")
            copy.fork("a", "checked")
            self.assertEqual({item: emulator.digest(path.read_bytes()) for item, path in emulator.save_files(copy.directory).items()},
                             json.loads((checked / "manifest.json").read_text())["files"])
            self.assertEqual(copy.manifest()["forked_from"]["snapshot"], "checked")
            mac = lambda profile: re.search(r'MAC = "(.*)"', (profile.directory / "portable/melonDS.toml").read_text()).group(1)
            self.assertNotEqual(mac(copy), mac(source))
            self.assertEqual(sorted(path.name for path in copy.directory.parent.iterdir()), ["a", "copy"])

    def test_fork_refuses_changed_linked_busy_or_existing_inputs_without_leaving_a_profile(self):
        self.addCleanup(os.umask, os.umask(0o077))
        with tempfile.TemporaryDirectory() as temporary:
            local, source, checked = self.fork_source(Path(temporary).resolve())
            profiles = source.directory.parent
            with self.assertRaisesRegex(ValueError, "different profile"): emulator.Emulator(local, "a").fork("a", "checked")
            with emulator.private_lock(local.workspace / ".emulator-a.lock", fcntl.LOCK_EX), self.assertRaises(BlockingIOError):
                emulator.Emulator(local, "busy").fork("a", "checked")
            emulator.Emulator(local, "copy").fork("a", "checked")
            with self.assertRaises(FileExistsError): emulator.Emulator(local, "copy").fork("a", "checked")
            original = (source.directory / "melonDS").read_bytes()
            (source.directory / "melonDS").unlink(); (source.directory / "melonDS").symlink_to(profiles / "copy/melonDS")
            with self.assertRaises(OSError): emulator.Emulator(local, "linked").fork("a", "checked")
            (source.directory / "melonDS").unlink(); emulator.write_private(source.directory / "melonDS", original)
            real = emulator.write_private
            def fail_on_head(path, data):
                if path.name == "HEAD": raise OSError("No space left on device")
                real(path, data)
            with patch.object(emulator, "write_private", fail_on_head), self.assertRaisesRegex(OSError, "No space"):
                emulator.Emulator(local, "partial").fork("a", "checked")
            (checked / "A").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "Snapshot changed"): emulator.Emulator(local, "late").fork("a", "checked")
            self.assertEqual(sorted(path.name for path in profiles.iterdir()), ["a", "copy"])

    def test_stale_images_are_refused_before_a_profile_is_provisioned(self):
        names = ("emulator/durable_save.cpp", "emulator/journal_seed.cpp", "emulator/tests/faults.cpp",
                 "client/src/wonder_trade/journal.c", "scripts/run-emulator.py")
        current = "".join(f"{emulator.digest((emulator.stack.ROOT / name).read_bytes())}  /src/{name}\n" for name in names)
        missing = "".join(current.splitlines(keepends=True)[1:])
        with tempfile.TemporaryDirectory() as temporary:
            for code, output, error in ((0, current, None), (1, missing, ValueError), (125, "", emulator.stack.subprocess.CalledProcessError)):
                local = SimpleNamespace(workspace=Path(temporary).resolve(), manifest={"project": "owned"},
                                        run=lambda *args, code=code, output=output, **kwargs: SimpleNamespace(returncode=code, stdout=output, stderr=""))
                runner = emulator.Emulator(local, "a")
                with self.subTest(code=code):
                    if error is None: self.assertEqual(set(runner.image_sources("image")), set(names))
                    else:
                        with self.assertRaises(error): runner.image_sources("image")
            save, rom = Path(temporary) / "original.sav", Path(temporary) / "rom.nds"
            save.write_bytes(bytes(524288)); rom.write_bytes(bytearray(12) + b"CPUE" + bytearray(14) + b"\1" + bytearray(134217728 - 31))
            runner = emulator.Emulator(SimpleNamespace(workspace=Path(temporary).resolve(), manifest={"project": "owned"}), "fresh")
            runner.image = lambda name: "sha256:stale"
            runner.image_sources = lambda image: (_ for _ in ()).throw(ValueError("Rebuild the emulator image from these checked sources"))
            with patch.object(emulator.PROVISION, "provision", lambda data, target: data), self.assertRaisesRegex(ValueError, "Rebuild"):
                runner.create(save, rom, "us-rev1", "dswt-emulator-builder")
            self.assertFalse(runner.directory.exists())
            def copy_binary(arguments, **kwargs):  # Stands in for copying melonDS out of the checked image.
                if arguments[-1] == "/profile/melonDS": (runner.directory / "melonDS").write_bytes(b"emulator")
            runner.local.run = copy_binary
            runner.image_sources = lambda image: {"emulator/durable_save.cpp": "checked"}
            with patch.object(emulator.PROVISION, "provision", lambda data, target: data):
                runner.create(save, rom, "us-rev1", "dswt-emulator-builder")
            self.assertEqual(runner.manifest()["image_source_sha256"], {"emulator/durable_save.cpp": "checked"})
