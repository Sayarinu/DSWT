"""Fault the production save store with synthetic, independent owner images."""

import argparse
import hashlib
import os
from pathlib import Path
import selectors
import shutil
import signal
import struct
import subprocess
import tempfile
import unittest


SIZE = 524288


def image(owner, generation):
    data = bytearray(b'\xff' * SIZE)
    # Preserve distinct identity, escrow, incoming, pre/post image and receipt
    # bytes through every host fault, independent of later game state handling.
    for offset, label, size in ((32, b'identity', 64), (160, b'escrow', 256),
                                (416, b'incoming/pre/post', 1024), (1952, b'operation/receipt', 264)):
        seed = hashlib.sha256(owner + label + struct.pack('<Q', generation)).digest()
        data[0x30000 + offset:0x30000 + offset + size] = (seed * (size // 32 + 1))[:size]
    data[0x70000:0x71000] = data[0x30000:0x31000]
    return bytes(data)


class PersistenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='platinumwt-host-save-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.save = self.root / 'owner-a.sav'
        self.before, self.after = image(b'owner-a', 0), image(b'owner-a', 1)
        self.save.write_bytes(self.before)
        self.save.chmod(0o600)
        self.input = self.root / 'next-image'
        self.input.write_bytes(self.after)
        self.tool('prepare')
        self.baseline = self.root / 'baseline'
        shutil.copytree(str(self.save) + '.platinumwt', self.baseline)

    def tool(self, mode, *, save=None, ok=True):
        result = subprocess.run([str(self.store), mode, str(save or self.save)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def reset(self):
        self.save.write_bytes(self.before)
        store = Path(str(self.save) + '.platinumwt')
        shutil.rmtree(store)
        shutil.copytree(self.baseline, store)

    def test_every_persistence_fence_survives_process_kill_and_write_failure(self):
        # Four atomic publications each have write/fsync/close/rename/dir-fsync.
        for mode in ('before', 'after', 'error'):
            for stage in range(1, 21):
                with self.subTest(mode=mode, stage=stage):
                    self.reset()
                    env = dict(os.environ, LD_PRELOAD=str(self.faults), PWT_FAULT_AT=str(stage), PWT_FAULT_MODE=mode)
                    process = subprocess.Popen([str(self.probe), str(self.save), str(self.input)], env=env,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    try:
                        if mode != 'error':
                            with selectors.DefaultSelector() as selector:
                                selector.register(process.stderr, selectors.EVENT_READ)
                                self.assertTrue(selector.select(5), 'fault checkpoint did not report')
                                self.assertEqual(process.stderr.readline(), b'STOPPED\n')
                            process.kill()
                        out, err = process.communicate(timeout=5)
                        self.assertNotIn(b'COMMITTED', out)
                        if mode == 'error':
                            self.assertEqual(process.returncode, 74, err)
                        else:
                            self.assertEqual(process.returncode, -signal.SIGKILL)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.communicate()
                    self.tool('prepare')
                    self.tool('check')
                    self.assertIn(self.save.read_bytes(), (self.before, self.after))

    def test_success_is_immediately_durable_and_repeated_prepare_is_idempotent(self):
        for _ in range(20):
            result = subprocess.run([str(self.probe), str(self.save), str(self.input)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b'COMMITTED\n')
            self.assertEqual(self.save.read_bytes(), self.after)
        self.tool('prepare')
        self.tool('prepare')
        self.tool('check')
        self.assertEqual(self.save.read_bytes(), self.after)

    def test_truncated_raw_file_and_one_damaged_snapshot_recover_exact_bytes(self):
        self.save.write_bytes(self.before[:113])
        (Path(str(self.save) + '.platinumwt') / 'A').write_bytes(b'PWT')
        self.tool('prepare')
        self.assertEqual(self.save.read_bytes(), self.before)
        self.tool('check')

    def test_mixed_owner_snapshots_and_full_foreign_save_fail_closed(self):
        second = self.root / 'owner-b.sav'
        second.write_bytes(image(b'owner-b', 0))
        second.chmod(0o600)
        self.tool('prepare', save=second)
        foreign = Path(str(second) + '.platinumwt') / 'A'
        current = Path(str(self.save) + '.platinumwt') / 'A'
        current.write_bytes(foreign.read_bytes())
        self.tool('prepare', ok=False)
        self.assertEqual(self.save.read_bytes(), self.before)
        self.reset()
        self.save.write_bytes(second.read_bytes())
        self.tool('prepare', ok=False)
        self.assertEqual(self.save.read_bytes(), second.read_bytes())

    def test_stale_snapshot_pair_cannot_move_behind_the_durable_head(self):
        result = subprocess.run([str(self.probe), str(self.save), str(self.input)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        current = Path(str(self.save) + '.platinumwt')
        for bank in ('A', 'B'):
            (current / bank).write_bytes((self.baseline / bank).read_bytes())
        self.tool('prepare', ok=False)
        self.assertEqual(self.save.read_bytes(), self.after)

    def test_symlinks_hardlinks_and_overlapping_owners_are_refused(self):
        alias = self.root / 'linked.sav'
        alias.symlink_to(self.save)
        self.tool('prepare', save=alias, ok=False)
        alias.unlink()
        os.link(self.save, alias)
        self.tool('prepare', ok=False)
        alias.unlink()
        import fcntl
        with open(str(self.save) + '.platinumwt.lock', 'r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            self.tool('prepare', ok=False)
        self.tool('check')

    def test_a_memory_backed_save_cannot_advertise_host_durability(self):
        if not Path('/dev/shm').is_dir():
            self.skipTest('Linux shared-memory mount is unavailable')
        with tempfile.TemporaryDirectory(prefix='platinumwt-volatile-', dir='/dev/shm') as directory:
            save = Path(directory) / 'volatile.sav'
            save.write_bytes(self.before)
            save.chmod(0o600)
            self.tool('prepare', save=save)
            result = subprocess.run([str(self.probe), '--supports-host', str(save)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b'UNSUPPORTED\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('store', type=Path)
    parser.add_argument('probe', type=Path)
    parser.add_argument('faults', type=Path)
    args = parser.parse_args()
    PersistenceTests.store, PersistenceTests.probe, PersistenceTests.faults = args.store.resolve(), args.probe.resolve(), args.faults.resolve()
    unittest.main(argv=[__file__], verbosity=2)
