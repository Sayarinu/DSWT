#!/bin/sh
# Copies each finished WAL file and base backup to R2, encrypted by the `backup` rclone
# remote, then marks it. wal-archive.sh waits for that mark, so PostgreSQL only counts a
# segment as archived, and trades only complete, once it is off the server.
#
#   backup-uploader.sh [once]
set -u

archive=${DSWT_ARCHIVE:-/archive}
metrics=${DSWT_METRICS:-/metrics}
marks=$archive/.uploaded
retention_days=30
heartbeat_seconds=300
retention_seconds=3600
checked=0
pruned=0
umask 022
mkdir -p "$marks"

ship_files() {
    for path in "$archive"/*; do
        name=${path##*/}
        case $name in
            ????????????????????????|????????????????????????.*.backup|????????.history) ;;
            *) continue ;;
        esac
        [ -f "$path" ] && [ ! -e "$marks/$name" ] || continue
        rclone copyto "$path" "backup:wal/$name" || return 1
        : >"$marks/$name"
    done
}

base_start() {
    sed -n 's/^START WAL LOCATION: .*(file \([0-9A-F]\{24\}\))$/\1/p' "$archive/base/backup_label" 2>/dev/null
}

ship_base() {
    first=$(base_start)
    [ -n "$first" ] && [ ! -e "$marks/base-$first" ] || return 0
    # One file, because object storage would lose the empty directories PostgreSQL needs.
    bundle=$marks/.base.tar.gz
    tar -C "$archive/base" -czf "$bundle" . || { rm -f "$bundle"; return 1; }
    # A rotation during the archive moved the directory, so it may be partial.
    if [ "$(base_start)" != "$first" ] || ! rclone copyto "$bundle" "backup:bases/$first.tar.gz"; then
        rm -f "$bundle"
        return 1
    fi
    rm -f "$bundle"
    : >"$marks/base-$first"
}

forget_marks() {
    for path in "$marks"/*; do
        name=${path##*/}
        case $name in
            'base-'*|'*') continue ;;
        esac
        [ -e "$archive/$name" ] || rm -f "$path"
    done
    current=$(base_start)
    for path in "$marks"/base-*; do
        [ "$path" = "$marks/base-$current" ] || rm -f "$path"
    done
}

# Always keep the newest base, and every WAL file a kept base needs.
prune_remote() {
    newest=$(rclone lsf --files-only backup:bases | sort | tail -n 1) || return 1
    [ -n "$newest" ] || return 0
    rclone delete --min-age "${retention_days}d" --exclude "$newest" backup:bases || return 1
    oldest=$(rclone lsf --files-only backup:bases | sort | head -n 1) || return 1
    oldest=${oldest%%.*}
    [ -n "$oldest" ] || return 0
    rclone lsf --files-only backup:wal | while read -r name; do
        if [ "$(printf '%s\n%s\n' "${name%%.*}" "$oldest" | sort | head -n 1)" != "$oldest" ]; then
            rclone deletefile "backup:wal/$name" || exit 1
        fi
    done
}

write_metric() {
    printf 'dswt_backup_last_success_timestamp_seconds %s\n' "$1" >"$metrics/.backup.prom.tmp" &&
        mv "$metrics/.backup.prom.tmp" "$metrics/backup.prom"
}

while :; do
    now=$(date +%s)
    if ship_files && ship_base; then
        forget_marks
        # With nothing to upload, proves R2 and the key still work.
        if [ $((now - checked)) -ge "$heartbeat_seconds" ] && rclone lsf --max-depth 1 backup: >/dev/null; then
            checked=$now
            write_metric "$now"
        fi
        if [ $((now - pruned)) -ge "$retention_seconds" ] && prune_remote; then
            pruned=$now
        fi
    fi
    [ "${1:-}" != once ] || exit 0
    sleep 1
done
