"""Exercise the production matcher process without a database: config, ports and shutdown."""

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


def sockets(pid):
    links = set()
    for fd in Path(f"/proc/{pid}/fd").iterdir():
        try:
            links.add(os.readlink(fd))
        except FileNotFoundError:  # A database retry closed it after the listing; a listener would stay open.
            pass
    return {link for link in links if link.startswith("socket:")}


class MatcherProcessTests(unittest.TestCase):
    def env(self, **extra):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("PG", "WT_")) and key != "DSWT_DATABASE_URL"}
        env.update(extra)
        return env

    def test_requires_a_database_url(self):
        result = subprocess.run([str(self.binary)], env=self.env(), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1)

    def test_rejects_invalid_worker_counts(self):
        for workers in ("0", "9", "x", ""):
            with self.subTest(workers=workers):
                result = subprocess.run([str(self.binary)], capture_output=True, timeout=10, env=self.env(
                    DSWT_DATABASE_URL="host=/nonexistent", WT_MATCHER_WORKERS=workers))
                self.assertEqual(result.returncode, 1)

    def test_stops_promptly_and_opens_no_ports_without_a_database(self):
        with tempfile.TemporaryDirectory() as directory:
            process = subprocess.Popen([str(self.binary)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=self.env(
                DSWT_DATABASE_URL=f"host={directory} port=1 dbname=none", WT_MATCHER_WORKERS="3"))
            try:
                self.assertIn(b"3 worker(s)", process.stdout.readline())
                time.sleep(0.5)
                self.assertIsNone(process.poll())
                for table in ("tcp", "tcp6"):
                    listening = [line for line in Path(f"/proc/{process.pid}/net/{table}").read_text().splitlines()[1:]
                                 if line.split()[3] == "0A"]
                    owned = sockets(process.pid)
                    self.assertFalse([line for line in listening if f"socket:[{line.split()[9]}]" in owned])
                started = time.monotonic()
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertLess(time.monotonic() - started, 3)
            finally:
                if process.poll() is None:
                    process.kill(); process.wait()
                process.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    MatcherProcessTests.binary = parser.parse_args().binary.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(MatcherProcessTests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
