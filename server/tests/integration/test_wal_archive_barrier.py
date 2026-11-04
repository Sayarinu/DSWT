"""Exercise the WAL archive barrier against a real PostgreSQL archiving through a controllable command."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import unittest


class ArchiveScriptTests(unittest.TestCase):
    SYSTEM = 7300000000000000001

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="platinumwt-archive-script-", dir="/tmp")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.archive = self.root / "archive"
        self.archive.mkdir()

    def segment(self, name, system=None, size=64):
        path = self.root / name
        path.write_bytes(bytes(24) + (system or self.SYSTEM).to_bytes(8, "little") + bytes(size - 32))
        return path

    def run_script(self, *arguments):
        return subprocess.run([str(WalArchiveBarrierTests.script), *map(str, arguments)], capture_output=True, text=True)

    def ship(self, name, **options):
        return self.run_script("archive", self.archive, self.segment(name, **options), name)

    def init(self, first="000000010000000000000003"):
        self.assertEqual(self.run_script("init", self.archive, self.SYSTEM, first).returncode, 0)
        self.assertEqual(self.run_script("arm", self.archive).returncode, 0)

    def test_nothing_ships_before_init(self):
        self.assertEqual(self.ship("000000010000000000000003").returncode, 1)
        self.assertEqual(list(self.archive.iterdir()), [])

    def test_nothing_ships_until_armed(self):
        self.assertEqual(self.run_script("arm", self.archive).returncode, 1)
        self.assertEqual(self.run_script("init", self.archive, self.SYSTEM, "000000010000000000000003").returncode, 0)
        self.assertEqual(self.ship("000000010000000000000003").returncode, 1)
        self.assertFalse((self.archive / "000000010000000000000003").exists())
        self.assertEqual(self.run_script("arm", self.archive).returncode, 0)
        self.assertEqual(self.ship("000000010000000000000003").returncode, 0)

    def test_init_cannot_replace_a_chain(self):
        self.init()
        self.assertEqual(self.run_script("init", self.archive, self.SYSTEM, "000000010000000000000009").returncode, 1)
        self.assertIn("000000010000000000000003", (self.archive / "chain.start").read_text())

    def test_contiguous_segments_ship_and_gaps_are_refused(self):
        self.init()
        self.assertEqual(self.ship("000000010000000000000003").returncode, 0)
        self.assertEqual(self.ship("000000010000000000000005").returncode, 1)
        self.assertFalse((self.archive / "000000010000000000000005").exists())
        self.assertEqual(self.ship("000000010000000000000004").returncode, 0)
        self.assertEqual(self.ship("000000010000000000000005").returncode, 0)

    def test_segments_before_the_chain_start_are_skipped_without_shipping(self):
        self.init()
        for name in ("000000010000000000000002", "000000000000000000000009"):
            self.assertEqual(self.ship(name).returncode, 0)
            self.assertFalse((self.archive / name).exists())

    def test_predecessor_crosses_logical_files_by_segment_size(self):
        # A 64-byte fixture stands in for its size: 2^32 / 64 segments per logical file.
        self.init("000000010000000003FFFFFF")
        self.assertEqual(self.ship("000000010000000003FFFFFF").returncode, 0)
        self.assertEqual(self.ship("000000010000000100000000").returncode, 0)

    def test_other_clusters_and_timelines_are_refused(self):
        self.init()
        self.assertEqual(self.ship("000000010000000000000003", system=self.SYSTEM + 1).returncode, 1)
        self.assertEqual(self.ship("000000020000000000000004").returncode, 1)

    def test_repeating_a_file_succeeds_only_with_identical_contents(self):
        self.init()
        self.assertEqual(self.ship("000000010000000000000003").returncode, 0)
        self.assertEqual(self.ship("000000010000000000000003").returncode, 0)
        path = self.root / "000000010000000000000003"
        path.write_bytes(path.read_bytes() + b"x")
        self.assertEqual(self.run_script("archive", self.archive, path, path.name).returncode, 1)

    def test_history_files_ship_outside_the_chain(self):
        self.init()
        for name in ("00000002.history", "000000010000000000000003.00000028.backup"):
            path = self.root / name
            path.write_text("history")
            self.assertEqual(self.run_script("archive", self.archive, path, name).returncode, 0)
            self.assertEqual((self.archive / name).read_text(), "history")


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
        command = (f"test ! -e {shlex.quote(str(cls.hold))} && "
                   f"{shlex.quote(str(cls.script))} archive {shlex.quote(str(cls.archive))} %p %f")
        with (cls.data / "postgresql.conf").open("a") as config:
            config.write(f"archive_mode = on\narchive_command = '{command}'\n")
        cls.ctl("start", "-l", str(cls.root / "postgres.log"),
                "-o", f"-k {cls.root} -p 55433 -c listen_addresses='' -c shared_buffers=16MB")
        subprocess.run([str(cls.script), "init", str(cls.archive),
                        cls.sql("SELECT system_identifier FROM pg_control_system()"),
                        cls.sql("SELECT pg_walfile_name(pg_current_wal_insert_lsn())")], check=True, capture_output=True)
        subprocess.run([str(cls.script), "arm", str(cls.archive)], check=True, capture_output=True)
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
        # A test that left the archive failing must not slow the next one: a new
        # segment wakes the archiver, which otherwise sleeps up to a minute between retries.
        if self.hold.exists():
            self.hold.unlink()
        if self.failing():
            self.write()
            self.sql("SELECT pg_switch_wal()")
            deadline = time.monotonic() + 30
            while self.failing():
                self.assertLess(time.monotonic(), deadline, "archive did not recover")
                time.sleep(0.1)

    def failing(self):
        return self.sql("SELECT last_failed_time IS NOT NULL AND (last_archived_time IS NULL "
                        "OR last_failed_time > last_archived_time) FROM pg_stat_archiver") == "t"

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
        # With nothing forcing WAL out, this also waits out the archiver's retry sleep.
        self.assertEqual(self.probe(position, 75000), 0)

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

    def test_a_skipped_segment_keeps_later_segments_from_covering_it(self):
        # PostgreSQL logs a failed .ready file and moves on, then retries at a checkpoint.
        self.assertEqual(self.probe(self.write(), 20000), 0)
        status = self.data / "pg_wal/archive_status"
        status.chmod(0o500)
        try:
            skipped = self.write()
            self.sql("SELECT pg_switch_wal()")
        finally:
            status.chmod(0o700)
        self.write()
        self.sql("SELECT pg_switch_wal()")
        self.assertEqual(self.probe(skipped, 4000), 1)
        self.assertGreater(int(self.sql("SELECT failed_count FROM pg_stat_archiver")), 0)
        self.sql("CHECKPOINT")
        self.assertEqual(self.probe(skipped, 20000), 0)

    def test_failing_archive_does_not_pile_up_segments(self):
        self.assertEqual(self.probe(self.write(), 20000), 0)
        self.hold.touch()
        position = self.write()
        self.sql("SELECT pg_switch_wal()")
        self.write()
        before = self.sql("SELECT pg_walfile_name(pg_current_wal_flush_lsn())")
        self.assertEqual(self.probe(position, 3000), 1)
        self.assertEqual(self.sql("SELECT pg_walfile_name(pg_current_wal_flush_lsn())"), before)

    def test_unknown_coverage_on_an_idle_boundary_still_recovers(self):
        self.assertEqual(self.probe(self.write(), 20000), 0)
        self.sql("SELECT pg_switch_wal()")
        self.assertEqual(self.probe(self.sql("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text"), 20000), 0)
        self.sql("SELECT pg_stat_reset_shared('archiver')")
        position = self.sql("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text")
        self.assertEqual(self.probe(position, 20000), 0)

    def test_a_base_backup_does_not_strand_a_running_barrier(self):
        # A .backup name can hide a segment archived between polls (the order is not fixed); a waiter must still progress.
        self.assertEqual(self.probe(self.write(), 20000), 0)
        target = int(self.sql("SELECT (pg_current_wal_flush_lsn() - '0/0'::pg_lsn)::text")) + 4000000
        waiter = subprocess.Popen([str(self.probe_binary), str(target), "30000"], env=self.env, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        try:
            time.sleep(1)
            subprocess.run([str(self.pg_bin / "pg_basebackup"), "-D", str(self.root / "backup"), "-X", "none", "-c", "fast"],
                           check=True, capture_output=True, env=self.env, timeout=30)
            self.sql("INSERT INTO activity SELECT generate_series(1, 200000)")
            self.assertEqual(waiter.wait(timeout=40), 0)
        finally:
            if waiter.poll() is None:
                waiter.kill(); waiter.wait()

    def test_no_chain_start_ships_nothing(self):
        data, empty = self.root / "unchained", self.root / "unchained-archive"
        empty.mkdir()
        env = dict(self.env, PGPORT="55435")
        subprocess.run([str(self.pg_bin / "initdb"), "-D", str(data), "-A", "trust", "-U", "barrier_owner",
                        "--no-locale", "--encoding=UTF8"], check=True, capture_output=True, env=env)
        with (data / "postgresql.conf").open("a") as config:
            config.write(f"archive_mode = on\narchive_command = '{self.script} archive {empty} %p %f'\n")
        control = [str(self.pg_bin / "pg_ctl"), "-D", str(data), "-w"]
        subprocess.run(control + ["-l", str(self.root / "unchained.log"), "-o",
                                  f"-k {self.root} -p 55435 -c listen_addresses='' -c shared_buffers=16MB", "start"],
                       check=True, capture_output=True, env=env)
        try:
            self.assertEqual(self.probe(1, 4000, PGPORT="55435"), 1)
            self.assertEqual(list(empty.iterdir()), [])
        finally:
            subprocess.run(control + ["-m", "immediate", "stop"], check=True, capture_output=True, env=env)

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
    parser.add_argument("script", type=Path)
    args = parser.parse_args()
    WalArchiveBarrierTests.probe_binary = args.probe_binary.resolve()
    WalArchiveBarrierTests.script = args.script.resolve()
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite(loader.loadTestsFromTestCase(case) for case in (ArchiveScriptTests, WalArchiveBarrierTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
