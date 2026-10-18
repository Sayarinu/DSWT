import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("recovery_probe", Path(__file__).parents[1] / "build-recovery-probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class ProbeRestoration(unittest.TestCase):
    def test_restores_successful_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.c"; path.write_bytes(b"production")
            include = Path(directory) / "probe.inc"
            with probe.probe_edits([(path, b"production", b"probe"), (include, None, b"fixture")]):
                self.assertEqual(path.read_bytes(), b"probe")
                self.assertEqual(include.read_bytes(), b"fixture")
            self.assertEqual(path.read_bytes(), b"production")
            self.assertFalse(include.exists())

    def test_restores_failed_build(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.c"; path.write_bytes(b"production")
            with self.assertRaises(ValueError):
                with probe.probe_edits([(path, b"production", b"probe")]):
                    raise ValueError("Compiler failure")
            self.assertEqual(path.read_bytes(), b"production")

    def test_preserves_concurrent_work(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.c"; path.write_bytes(b"production")
            with self.assertRaisesRegex(RuntimeError, "Preserved concurrent"):
                with probe.probe_edits([(path, b"production", b"probe")]):
                    path.write_bytes(b"developer change")
            self.assertEqual(path.read_bytes(), b"developer change")

    def test_failed_atomic_publish_preserves_source(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.c"; path.write_bytes(b"production")
            with mock.patch.object(probe.os, "replace", side_effect=OSError("Disk full")):
                with self.assertRaises(OSError):
                    with probe.probe_edits([(path, b"production", b"probe")]):
                        self.fail("A failed publish must not start the build")
            self.assertEqual(path.read_bytes(), b"production")
            self.assertEqual(list(Path(directory).iterdir()), [path])


if __name__ == "__main__":
    unittest.main()
