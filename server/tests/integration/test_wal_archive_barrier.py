"""Exercise the WAL archive barrier against a real PostgreSQL archiving through a controllable command."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


class WalArchiveBarrierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="platinumwt-barrier-", dir="/tmp")
        cls.root = Path(cls.directory.name)
        cls.data = cls.root / "data"
        cls.archive = cls.root / "archive"
        cls.archive.mkdir()
        cls.hold = cls.root / "hold"
        cls.pg_bin = Path(subprocess.check_output(["pg_config", "--bindir"], text=True).strip())
        cls.env = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
        cls.env.update(PGHOST=str(cls.root), PGPORT="55433", PGUSER="barrier_owner", PGDATABASE="postgres")
        subprocess.run([str(cls.pg_bin / "initdb"), "-D", str(cls.data), "-A", "trust", "-U", "barrier_owner",
                        "--no-locale", "--encoding=UTF8"], check=True, capture_output=True, env=cls.env)
        command = f"test ! -e {shlex.quote(str(cls.hold))} && cp %p {shlex.quote(str(cls.archive))}/%f"
        with (cls.data / "postgresql.conf").open("a") as config:
            config.write(f"archive_mode = on\narchive_command = '{command}'\n")
        cls.ctl("start", "-l", str(cls.root / "postgres.log"),
                "-o", f"-k {cls.root} -p 55433 -c listen_addresses='' -c shared_buffers=16MB")
        cls.sql("CREATE TABLE activity(value int)")

    @classmethod
    def ctl(cls, action, *options):
        subprocess.run([str(cls.pg_bin / "pg_ctl"), "-D", str(cls.data), "-w", *options, action],
                       check=True, capture_output=True, env=cls.env)

    @classmethod
    def sql(cls, text):
        return subprocess.run([str(cls.pg_bin / "psql"), "-XAtq", "-v", "ON_ERROR_STOP=1", "-c", text],
                              check=True, capture_output=True, text=True, env=cls.env).stdout.strip()

    @classmethod
    def tearDownClass(cls):
        cls.ctl("stop", "-m", "immediate")
        cls.directory.cleanup()

    def setUp(self):
        self.hold.unlink(missing_ok=True)

    def write(self):
        """Commit some WAL and return the flushed position it must be covered through."""
        self.sql("INSERT INTO activity VALUES (1)")
        return self.sql("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text")

    def probe(self, position, milliseconds, **env):
        return subprocess.run([str(self.probe_binary), str(position), str(milliseconds)], env=dict(self.env, **env),
                              timeout=60, capture_output=True).returncode

    def test_archived_position_is_covered(self):
        self.assertEqual(self.probe(self.write(), 20000), 0)

    def test_stalled_archive_stays_closed_until_it_resumes(self):
        self.hold.touch()
        # Let anything already in flight finish before the position under test is written.
        self.sql("SELECT pg_switch_wal()")
        position = self.write()
        self.assertEqual(self.probe(position, 4000), 1)
        self.hold.unlink()
        self.assertEqual(self.probe(position, 20000), 0)

    def test_position_past_the_archive_is_not_covered(self):
        position = int(self.write())
        self.assertEqual(self.probe(position, 20000), 0)
        self.assertEqual(self.probe(position + 2 ** 40, 1500), 1)

    def test_a_waiting_request_forces_its_segment_out(self):
        # With no forced switch, this position would wait for the segment to fill.
        position = self.write()
        before = self.sql("SELECT last_archived_wal FROM pg_stat_archiver")
        self.assertEqual(self.probe(position, 20000), 0)
        self.assertNotEqual(self.sql("SELECT last_archived_wal FROM pg_stat_archiver"), before)

    def test_an_idle_barrier_does_not_switch_segments(self):
        self.assertEqual(self.probe(self.write(), 20000), 0)
        before = self.sql("SELECT pg_walfile_name(pg_current_wal_flush_lsn())")
        self.assertEqual(self.probe(1, 1500), 0)
        self.assertEqual(self.sql("SELECT pg_walfile_name(pg_current_wal_flush_lsn())"), before)

    def test_role_needs_the_switch_grant_and_otherwise_stays_closed(self):
        self.sql("DROP ROLE IF EXISTS barrier_plain")
        self.sql("CREATE ROLE barrier_plain LOGIN")
        try:
            position = self.write()
            self.assertEqual(self.probe(position, 2000, PGUSER="barrier_plain"), 1)
            self.sql("GRANT EXECUTE ON FUNCTION pg_switch_wal() TO barrier_plain")
            self.assertEqual(self.probe(position, 20000, PGUSER="barrier_plain"), 0)
        finally:
            self.sql("REVOKE EXECUTE ON FUNCTION pg_switch_wal() FROM barrier_plain")
            self.sql("DROP ROLE barrier_plain")

    def test_disabled_archiving_never_covers(self):
        data = self.root / "unarchived"
        env = dict(self.env, PGPORT="55434")
        subprocess.run([str(self.pg_bin / "initdb"), "-D", str(data), "-A", "trust", "-U", "barrier_owner",
                        "--no-locale", "--encoding=UTF8"], check=True, capture_output=True, env=env)
        control = [str(self.pg_bin / "pg_ctl"), "-D", str(data), "-w"]
        subprocess.run(control + ["-l", str(self.root / "unarchived.log"), "-o",
                                  f"-k {self.root} -p 55434 -c listen_addresses='' -c shared_buffers=16MB", "start"],
                       check=True, capture_output=True, env=env)
        try:
            result = subprocess.run([str(self.probe_binary), "1", "1500"], env=env, timeout=60, capture_output=True)
            self.assertEqual(result.returncode, 1)
        finally:
            subprocess.run(control + ["-m", "immediate", "stop"], check=True, capture_output=True, env=env)


def main():
    if os.geteuid() == 0:
        os.execvp("runuser", ["runuser", "-u", "postgres", "--", sys.executable, __file__, *sys.argv[1:]])
    parser = argparse.ArgumentParser()
    parser.add_argument("probe_binary", type=Path)
    args = parser.parse_args()
    WalArchiveBarrierTests.probe_binary = args.probe_binary.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(WalArchiveBarrierTests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
