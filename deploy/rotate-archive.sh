#!/bin/sh
# Prunes the local WAL archive. Once it passes DSWT_ARCHIVE_LIMIT_MB, this takes a fresh,
# verified base backup and rotates to it; wal-archive.sh rotate then deletes the WAL the old
# base needed, keeping any file not yet copied off the server. The deploy user's crontab
# runs it every 10 minutes; `dswt.py tidy` runs it with "now" after each release.
#
#   rotate-archive.sh [now]
#
# One rotation at a time: cron skips while another holds the lock, tidy waits for it.
set -eu
cd "$(dirname "$0")"
limit_mb=${DSWT_ARCHIVE_LIMIT_MB:-4096}
exec 9>>.archive.lock
if [ "${1:-}" = now ]; then
    flock -w 900 9
else
    flock -n 9 || exit 0
fi
database() { docker compose -p dswt exec -T --user postgres database "$@"; }
if [ "${1:-}" != now ]; then
    size_mb=$(database du -sm /archive | cut -f1)
    [ "$size_mb" -ge "$limit_mb" ] || exit 0
    echo "WAL archive is ${size_mb} MB; rotating to a new base backup"
fi
database sh -euc '
    rm -rf /archive/base.next
    pg_basebackup -h /var/run/postgresql -U dswt_admin -D /archive/base.next -X stream -c fast
    pg_verifybackup -q /archive/base.next
    /usr/local/bin/wal-archive.sh rotate /archive /archive/base.next'
database du -sm /archive
