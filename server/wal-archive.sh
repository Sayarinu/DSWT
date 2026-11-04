#!/bin/bash
# PostgreSQL archive_command that keeps the archive a contiguous prefix of WAL.
#
# pg_stat_archiver alone cannot prove that: a segment whose .ready file could not
# be created is skipped until a later checkpoint, while newer segments still
# ship. The recovery barrier trusts the last archived name, so this refuses any
# segment whose predecessor is not already here. PostgreSQL then retries, and
# trades stay PROCESSING.
#
#   wal-archive.sh init DESTINATION SYSTEM_IDENTIFIER FIRST_SEGMENT
#   wal-archive.sh archive DESTINATION %p %f
#
# Nothing ships until init records the chain start. Run init, then take a base
# backup, before serving: covered WAL is only recoverable from that backup onward.
set -euo pipefail

fail() { echo "wal-archive: $*" >&2; exit 1; }

is_segment() { [[ $1 =~ ^[0-9A-F]{24}$ ]]; }

predecessor() { # NAME SEGMENT_SIZE
    local timeline=${1:0:8} log=$((16#${1:8:8})) number=$((16#${1:16:8}))
    if ((number == 0)); then
        ((log > 0)) || fail "no segment precedes $1"
        log=$((log - 1)) number=$((0x100000000 / $2 - 1))
    else
        number=$((number - 1))
    fi
    printf '%s%08X%08X' "$timeline" "$log" "$number"
}

# Copy durably, never replacing a different file. Repeating a shipped file succeeds.
publish() { # SOURCE DESTINATION NAME
    local target=$2/$3 temporary=$2/.$3.$$
    if [[ -e $target ]]; then
        cmp -s "$1" "$target" || fail "$3 already archived with different contents"
        return 0
    fi
    trap 'rm -f "$temporary"' EXIT
    cp "$1" "$temporary"
    sync "$temporary"
    ln "$temporary" "$target" || fail "cannot publish $3"
    rm -f "$temporary"
    trap - EXIT
    sync "$2"
}

action=${1:-}
case $action in
init)
    (($# == 4)) || fail "usage: init DESTINATION SYSTEM_IDENTIFIER FIRST_SEGMENT"
    [[ $3 =~ ^[0-9]+$ ]] || fail "invalid system identifier"
    is_segment "$4" || fail "invalid first segment"
    [[ ! -e $2/chain.start ]] || fail "this archive already has a chain"
    temporary=$(mktemp "$2/.chain.XXXXXX")
    printf '%s %s\n' "$3" "$4" >"$temporary"
    sync "$temporary"
    chmod 0600 "$temporary"
    ln "$temporary" "$2/chain.start" || fail "cannot record the chain start"
    rm -f "$temporary"
    sync "$2"
    ;;
archive)
    (($# == 4)) || fail "usage: archive DESTINATION PATH NAME"
    destination=$2 path=$3 name=$4
    [[ -r $destination/chain.start ]] || fail "no chain start; run init and take a base backup"
    # Backup and timeline history files are not part of the segment chain.
    if is_segment "$name"; then
        read -r system first <"$destination/chain.start"
        # xlp_sysid in the segment's first long page header.
        actual=$(od -An -t u8 -j 24 -N 8 "$path" | tr -d ' ')
        [[ $actual == "$system" ]] || fail "$name belongs to another cluster"
        [[ ${name:0:8} == "${first:0:8}" ]] || fail "$name is on another timeline; start a new archive"
        [[ ! $name < $first ]] || fail "$name precedes the chain start"
        if [[ $name != "$first" ]]; then
            previous=$(predecessor "$name" "$(stat -c %s "$path")")
            [[ -e $destination/$previous ]] || fail "$name has no archived predecessor $previous"
        fi
    fi
    publish "$path" "$destination" "$name"
    ;;
*)
    fail "usage: $0 init|archive ..."
    ;;
esac
