"""Exercise deploy/backup-uploader.sh against a fake rclone that uses local directories."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "deploy/backup-uploader.sh"
FAKE = """#!/bin/sh
# Maps backup:PATH onto $REMOTE/PATH; FAIL makes every call fail.
[ -z "${FAIL:-}" ] || exit 1
map() { case $1 in backup:*) printf '%s/%s' "$REMOTE" "${1#backup:}" ;; *) printf '%s' "$1" ;; esac; }
command=$1; shift
case $command in
copyto) mkdir -p "$(dirname "$(map "$2")")"; cp "$1" "$(map "$2")" ;;
deletefile) rm -f "$(map "$1")" ;;
delete) shift 2; keep=$2; path=$(map "$3"); for entry in "$path"/*; do [ "${entry##*/}" = "$keep" ] || rm -f "$entry"; done ;;
lsf)
  while [ "${1#-}" != "$1" ]; do [ "$1" = --max-depth ] && shift; dirs=$1; shift; done
  path=$(map "$1"); [ -d "$path" ] || exit 0
  ls -1 "$path" | while read -r entry; do [ -d "$path/$entry" ] && echo "$entry/" || echo "$entry"; done ;;
esac
"""


class UploaderTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.archive, self.metrics, self.remote, tools = root / "archive", root / "metrics", root / "remote", root / "tools"
        for path in (self.archive, self.metrics, self.remote, tools):
            path.mkdir()
        (tools / "rclone").write_text(FAKE)
        (tools / "rclone").chmod(0o755)
        self.env = dict(os.environ, DSWT_ARCHIVE=str(self.archive), DSWT_METRICS=str(self.metrics), REMOTE=str(self.remote),
                        PATH=f"{tools}:{os.environ['PATH']}")
        (self.archive / "chain.start").write_text("1 000000010000000000000003\n")
        (self.archive / "chain.armed").touch()
        (self.archive / ".000000010000000000000009.123").touch()

    def run_once(self, **extra):
        subprocess.run(["/bin/sh", str(SCRIPT), "once"], env=dict(self.env, **extra), check=True, timeout=30)

    def segment(self, name):
        (self.archive / name).write_text(name)

    def base(self, first):
        base = self.archive / "base"
        base.mkdir(exist_ok=True)
        (base / "backup_label").write_text(f"START WAL LOCATION: 0/C00028 (file {first})\n")

    def test_uploads_and_marks_only_archive_files(self):
        self.segment("000000010000000000000003")
        self.segment("000000010000000000000003.00000028.backup")
        self.base("000000010000000000000003")
        self.run_once()
        self.assertEqual({path.name for path in (self.remote / "wal").iterdir()},
                         {"000000010000000000000003", "000000010000000000000003.00000028.backup"})
        self.assertTrue((self.remote / "bases/000000010000000000000003.tar.gz").exists())
        marks = {path.name for path in (self.archive / ".uploaded").iterdir()}
        self.assertEqual(marks, {"000000010000000000000003", "000000010000000000000003.00000028.backup", "base-000000010000000000000003"})
        self.assertIn("dswt_backup_last_success_timestamp_seconds", (self.metrics / "backup.prom").read_text())

    def test_failed_upload_leaves_no_mark_or_metric(self):
        self.segment("000000010000000000000003")
        self.run_once(FAIL="1")
        self.assertFalse((self.archive / ".uploaded/000000010000000000000003").exists())
        self.assertFalse((self.metrics / "backup.prom").exists())

    def test_rotation_forgets_marks_for_deleted_files(self):
        self.segment("000000010000000000000003")
        self.run_once()
        (self.archive / "000000010000000000000003").unlink()
        self.run_once()
        self.assertFalse((self.archive / ".uploaded/000000010000000000000003").exists())
        self.assertTrue((self.remote / "wal/000000010000000000000003").exists())

    def test_retention_keeps_the_newest_base_and_the_wal_it_needs(self):
        (self.remote / "bases").mkdir()
        for first in ("000000010000000000000003", "000000010000000000000008"):
            (self.remote / "bases" / f"{first}.tar.gz").touch()
        (self.remote / "wal").mkdir()
        for name in ("000000010000000000000007", "000000010000000000000008", "000000010000000000000008.00000028.backup",
                     "000000010000000000000009"):
            (self.remote / "wal" / name).touch()
        self.run_once()
        self.assertEqual({path.name for path in (self.remote / "bases").iterdir()}, {"000000010000000000000008.tar.gz"})
        self.assertEqual({path.name for path in (self.remote / "wal").iterdir()},
                         {"000000010000000000000008", "000000010000000000000008.00000028.backup", "000000010000000000000009"})


if __name__ == "__main__":
    unittest.main()
