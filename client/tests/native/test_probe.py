"""Exercise native persistence guards without replacing the HTTP acceptance gate."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())


class ProbeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="native client tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.client = self.root / "client one"
        canonical = bytearray(136)
        canonical[8] = 25
        struct.pack_into("<H", canonical, 6, sum(struct.unpack("<64H", canonical[8:])) & 0xffff)
        self.fixture = self.root / "generated.pk4"
        self.fixture.write_bytes(canonical)
        self.digest = hashlib.sha256(canonical).hexdigest()
        self.call("init", self.client, self.fixture)

    def call(self, *args, success=True):
        result = subprocess.run([BINARY, *map(str, args)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_exclusive_initialization_preserves_original_and_mirrored_inventory(self):
        before = {path.name: path.read_bytes() for path in self.client.iterdir()}
        self.call("init", self.client, self.fixture, success=False)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.client.iterdir()})
        state = json.loads(self.call("inspect", self.client).stdout)
        self.assertEqual(state["phase"], "empty")
        self.assertEqual(state["inventory"], [[self.digest], [self.digest]])
        self.assertEqual(self.fixture.read_bytes()[8], 25)

    def test_link_permissions_size_and_concurrent_writer_are_refused(self):
        journal = self.client / "journal.bin"
        original = journal.read_bytes()
        journal.chmod(0o644)
        self.call("inspect", self.client, success=False)
        journal.chmod(0o600)
        os.link(journal, self.root / "alias")
        self.call("inspect", self.client, success=False)
        (self.root / "alias").unlink()
        journal.rename(self.root / "held")
        journal.symlink_to(self.root / "held")
        self.call("inspect", self.client, success=False)
        journal.unlink()
        (self.root / "held").rename(journal)
        with (self.client / "lock").open("rb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.call("inspect", self.client, success=False)
        self.assertEqual(journal.read_bytes(), original)
        journal.write_bytes(original[:-1])
        self.call("inspect", self.client, success=False)

    def test_restart_rotates_entropy_preserving_exact_pending_enrollment(self):
        args = ("run", self.client, "8443", "localhost", "1" * 64, "2" * 64, "reconcile", "error", "wire.1.send", "1")
        self.call(*args, success=False)
        first = json.loads(self.call("inspect", self.client).stdout)
        journal = self.client / "journal.bin"
        before = journal.read_bytes()
        self.call(*args, success=False)
        second = json.loads(self.call("inspect", self.client).stdout)
        after = journal.read_bytes()
        self.assertEqual(first["phase"], "enrolling")
        self.assertTrue(first["pending_sha256"])
        self.assertEqual(first["pending_sha256"], second["pending_sha256"])
        self.assertEqual(first["inventory"], second["inventory"])
        self.assertEqual(before[48:96], after[48:96])  # Credential and enrollment ID.
        self.assertNotEqual(before[1444:1476], after[1444:1476])
        self.assertEqual(after[:4096], after[4096:])


if __name__ == "__main__":
    unittest.main()
