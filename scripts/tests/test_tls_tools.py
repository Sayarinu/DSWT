import binascii
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


connection = module("configure-connection")
provision = module("provision-save")
prepare = module("prepare-tls")
launcher = module("run-emulator")


def synthetic_save():
    data = bytearray(b"\xff" * 524288)
    for half in (0, 262144):
        for block, (offset, size) in enumerate(((0, 53036), (53036, 74212))):
            start = half + offset
            data[start:start + size - 20] = bytes(size - 20)
            crc = binascii.crc_hqx(data[start:start + size - 20], 0xffff)
            struct.pack_into("<IIIIBxH", data, start + size - 20, 1, 1, size, 0x20060623, block, crc)
    return bytes(data)


class TlsToolTests(unittest.TestCase):
    def test_profile_rejects_injection_invalid_ports_and_missing_pins(self):
        good = {"hostname": "wt.example.invalid", "ipv4": "192.0.2.1", "port": 8443,
                "current_spki_sha256": "12" * 32, "next_spki_sha256": "34" * 32}
        output = connection.header(good)
        self.assertIn('"wt.example.invalid:8443"', output)
        self.assertIn("0xc0000201UL", output)
        for key, value in (("hostname", 'x"\n#define BYPASS 1'), ("hostname", "http://x"),
                           ("ipv4", "0.0.0.0"), ("ipv4", "224.0.0.1"), ("port", 0),
                           ("port", True), ("port", 65536), ("current_spki_sha256", "00" * 32),
                           ("next_spki_sha256", "12" * 32), ("next_spki_sha256", "xx" * 32)):
            with self.subTest(key=key, value=value), self.assertRaises((ValueError, TypeError)):
                connection.header({**good, key: value})

    def test_provision_only_changes_new_journal_records(self):
        original = synthetic_save()
        result = provision.provision(original, "us-rev1")
        another = provision.provision(original, "us-rev1")
        first, second = 0x30000, 0x70000
        self.assertEqual(result[first:first + 4096], result[second:second + 4096])
        self.assertEqual(result[:first], original[:first])
        self.assertEqual(result[first + 4096:second], original[first + 4096:second])
        self.assertEqual(result[second + 4096:], original[second + 4096:])
        record = result[first:first + 4096]
        self.assertEqual(record[4060:4092], hashlib.sha256(record[:4060]).digest())
        self.assertEqual(record[4092:], b"CMIT")
        self.assertNotEqual(record[1444:1476], another[first + 1444:first + 1476])
        with self.assertRaisesRegex(ValueError, "not blank"):
            provision.provision(result, "us-rev1")
        with self.assertRaises(ValueError):
            provision.provision(original, "diamond-us-rev5")
        with self.assertRaises(ValueError):
            provision.provision(original[:262144], "us-rev0")

    def test_hgss_saves_are_checked_against_their_own_layout(self):
        hgss = bytearray(b"\xff" * 524288)
        for half in (0, 262144):
            for block, (offset, size) in enumerate(((0, 63016), (63232, 74512))):
                start = half + offset
                hgss[start:start + size - 16] = bytes(size - 16)
                crc = binascii.crc_hqx(hgss[start:start + size - 16], 0xffff)
                struct.pack_into("<IIIHH", hgss, start + size - 16, 1, size, 0x20060623, block, crc)
        for target in ("heartgold-us-rev0", "soulsilver-us-rev0"):
            with self.subTest(target=target):
                result = provision.provision(bytes(hgss), target)
                self.assertEqual(result[0x30000:0x31000], result[0x70000:0x71000])
                self.assertEqual(result[:0x30000], bytes(hgss[:0x30000]))
                # A Platinum footer layout is not an HGSS one, and vice versa.
                with self.assertRaisesRegex(ValueError, "no valid copy"):
                    provision.provision(synthetic_save(), target)
        with self.assertRaisesRegex(ValueError, "no valid copy"):
            provision.provision(bytes(hgss), "us-rev1")

    def test_dp_saves_are_checked_against_their_own_layout(self):
        dp = bytearray(b"\xff" * 524288)
        for half in (0, 262144):
            for block, (offset, size) in enumerate(((0, 49408), (49408, 74208))):
                start = half + offset
                dp[start:start + size - 20] = bytes(size - 20)
                crc = binascii.crc_hqx(dp[start:start + size - 20], 0xffff)
                struct.pack_into("<IIIIBxH", dp, start + size - 20, 1, 1, size, 0x20060623, block, crc)
        for target in ("diamond-us-rev5", "pearl-us-rev5"):
            with self.subTest(target=target):
                result = provision.provision(bytes(dp), target)
                self.assertEqual(result[0x30000:0x31000], result[0x70000:0x71000])
                self.assertEqual(result[:0x30000], bytes(dp[:0x30000]))
                # Platinum shares the footer format but not the block offsets.
                with self.assertRaisesRegex(ValueError, "no valid copy"):
                    provision.provision(synthetic_save(), target)
        with self.assertRaisesRegex(ValueError, "no valid copy"):
            provision.provision(bytes(dp), "us-rev1")

    def test_corrupt_retail_or_nonblank_reserved_sectors_are_preserved(self):
        for offset in (0, 53036, 0x31000):
            data = bytearray(synthetic_save())
            data[offset] ^= 1
            if offset < 53037:
                data[262144 + offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                provision.provision(bytes(data), "us-rev0")

    def test_provision_output_cannot_replace_a_file_symlink_or_hardlink(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "original.sav"
            source.write_bytes(b"preserve")
            linked = root / "linked.sav"
            linked.symlink_to(source)
            hardlink = root / "hard.sav"
            hardlink.hardlink_to(source)
            for path in (source, linked, hardlink):
                with self.assertRaises(FileExistsError):
                    provision.write_new(path, b"changed")
            self.assertEqual(source.read_bytes(), b"preserve")
            output = root / "new.sav"
            provision.write_new(output, b"new")
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)

    def test_tls_cache_detects_edits_and_escaping_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "client").mkdir()
            (root / "client/tls-dependency.json").write_text(json.dumps({"version": "test", "sha256": "expected"}))
            source = root / "game/mbedtls-test"
            source.mkdir(parents=True)
            (source / "source.c").write_text("original")
            (source / "alias.c").symlink_to("source.c")
            recorded = {"archive_sha256": "expected", "files": prepare.inventory(source)}
            (source / ".dswt-files.json").write_text(json.dumps(recorded))
            self.assertEqual(prepare.prepare(root), source)
            (source / "source.c").write_text("changed")
            with self.assertRaisesRegex(ValueError, "differs"):
                prepare.prepare(root)
            (source / "escape").symlink_to(root / "client")
            with self.assertRaisesRegex(ValueError, "escapes"):
                prepare.inventory(source)


class EmulatorLauncherTests(unittest.TestCase):
    def test_child_observes_fresh_mirrors_and_retail_bytes_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            save, observed = root / "test.sav", root / "observed"
            original = provision.provision(synthetic_save(), "us-rev1")
            provision.write_new(save, original)
            code = ("import hashlib,struct,sys; from pathlib import Path; "
                    "d=Path(sys.argv[1]).read_bytes(); "
                    "assert struct.unpack_from('<Q',d,0x30010)[0]==2; "
                    "assert d[0x30000:0x31000]==d[0x70000:0x71000]; "
                    "Path(sys.argv[2]).write_text(hashlib.sha256(d).hexdigest())")
            self.assertEqual(launcher.run(save, "us-rev1", [sys.executable, "-c", code, str(save), str(observed)]), 0)
            data = save.read_bytes()
            self.assertEqual(observed.read_text(), hashlib.sha256(data).hexdigest())
            self.assertNotEqual(data[0x305a4:0x305c4], original[0x305a4:0x305c4])
            for start, end in ((0, 0x30000), (0x31000, 0x70000), (0x71000, 0x80000)):
                self.assertEqual(data[start:end], original[start:end])

    def test_abrupt_child_rollback_still_gets_fresh_entropy_on_next_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            save, older = root / "test.sav", root / "older.sav"
            original = provision.provision(synthetic_save(), "us-rev1")
            provision.write_new(save, original)
            provision.write_new(older, original)
            code = ("import os,shutil,signal,sys; "
                    "shutil.copyfile(sys.argv[1],sys.argv[2]); os.kill(os.getpid(),signal.SIGKILL)")
            self.assertEqual(launcher.run(save, "us-rev1", [sys.executable, "-c", code, str(older), str(save)]), 137)
            self.assertEqual(save.read_bytes(), original)
            self.assertEqual(launcher.run(save, "us-rev1", [sys.executable, "-c", "pass"]), 0)
            self.assertNotEqual(save.read_bytes()[0x305a4:0x305c4], original[0x305a4:0x305c4])

    def test_unsupported_or_conflicting_journals_never_start_child(self):
        original = provision.provision(synthetic_save(), "us-rev1")
        variants = [synthetic_save()]
        for offset in (0x30004, 0x30006, 0x305a0, 0x31000):
            data = bytearray(original)
            data[offset] ^= 2
            variants.append(bytes(data))
            if offset < 0x31000:
                data[0x30fdc:0x30ffc] = hashlib.sha256(data[0x30000:0x30fdc]).digest()
            variants.append(bytes(data))
        data = bytearray(original)
        data[0x70000:0x71000] = provision.seed_record(b"\x12" * 32, 1)
        variants.append(bytes(data))
        with tempfile.TemporaryDirectory() as temp:
            for index, data in enumerate(variants):
                save = Path(temp) / f"test{index}.sav"
                provision.write_new(save, data)
                with self.subTest(index=index), patch.object(launcher.subprocess, "run") as child:
                    with self.assertRaises(ValueError):
                        launcher.run(save, "us-rev1", ["unused"])
                    child.assert_not_called()
                    self.assertEqual(save.read_bytes(), data)

    def test_failed_flush_preserves_selected_bank_and_blocks_child(self):
        with tempfile.TemporaryDirectory() as temp:
            save = Path(temp) / "test.sav"
            data = bytearray(provision.provision(synthetic_save(), "us-rev1"))
            # A torn bank's untrusted generation must not choose the overwrite order.
            struct.pack_into("<Q", data, 0x30010, 2**64 - 1)
            provision.write_new(save, data)
            with patch.object(launcher.os, "fsync", side_effect=OSError("injected flush failure")), \
                    patch.object(launcher.subprocess, "run") as child:
                with self.assertRaises(OSError):
                    launcher.run(save, "us-rev1", ["unused"])
                child.assert_not_called()
            self.assertEqual(save.read_bytes()[0x70000:0x71000], data[0x70000:0x71000])
            self.assertEqual(launcher.run(save, "us-rev1", [sys.executable, "-c", "pass"]), 0)
            recovered = save.read_bytes()
            self.assertEqual(recovered[0x30000:0x31000], recovered[0x70000:0x71000])

    def test_live_lock_symlinks_and_hardlinks_cannot_refresh_save(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            save = root / "test.sav"
            original = provision.provision(synthetic_save(), "us-rev1")
            provision.write_new(save, original)
            with open(str(save) + ".dswt.lock", "w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaises(BlockingIOError):
                    launcher.run(save, "us-rev1", ["unused"])
            linked = root / "linked.sav"
            linked.symlink_to(save)
            with self.assertRaises(OSError):
                launcher.run(linked, "us-rev1", ["unused"])
            linked.unlink()
            linked.hardlink_to(save)
            with self.assertRaises(ValueError):
                launcher.run(linked, "us-rev1", ["unused"])
            self.assertEqual(save.read_bytes(), original)

    def test_child_keeps_save_locked_if_launcher_is_killed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            save, ready = root / "test.sav", root / "ready"
            provision.write_new(save, provision.provision(synthetic_save(), "us-rev1"))
            code = ("import os,sys,time; from pathlib import Path; "
                    "Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(10)")
            parent = subprocess.Popen([sys.executable, str(ROOT / "scripts/run-emulator.py"),
                                       "--save", str(save), "--target", "us-rev1", "--",
                                       sys.executable, "-c", code, str(ready)])
            child_pid = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists())
                child_pid = int(ready.read_text())
                parent.kill()
                parent.wait(timeout=5)
                with self.assertRaises(BlockingIOError):
                    launcher.run(save, "us-rev1", ["unused"])
            finally:
                if parent.poll() is None:
                    parent.kill()
                parent.wait(timeout=5)
                if child_pid is None and ready.exists():
                    child_pid = int(ready.read_text())
                if child_pid is not None:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main()
