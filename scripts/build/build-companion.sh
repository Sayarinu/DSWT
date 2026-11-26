#!/usr/bin/env bash
# Build the DS Companion ROM in the pinned BlocksDS image
# (tools/builders/companion-builder). The ROM and its build files go to game/companion.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [[ $# != 0 ]]; then
    echo "Usage: [CONNECTION_PROFILE=profile.json] [COMPANION_BUILD_JOBS=n] $0" >&2
    exit 2
fi
# The tracked profile names no real server; set CONNECTION_PROFILE to your
# public endpoint/pins JSON (see https://github.com/Sayarinu/DSWT/wiki/Local-Testing).
profile="${CONNECTION_PROFILE:-$repo_root/client/companion/connection-profile.json}"
build_jobs="${COMPANION_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "COMPANION_BUILD_JOBS must be a positive integer without leading zeros" >&2
    exit 2
fi

python="${PYTHON:-python3}"
if ! "$python" -c 'import sys; sys.exit(sys.version_info < (3, 12))'; then
    echo "Python 3.12 or newer is required. Install it separately and set PYTHON=/path/to/python3.12; leave the system Python unchanged." >&2
    exit 2
fi

output="$repo_root/game/companion"
mkdir -p "$output"
tls_workspace="$("$python" "$repo_root/scripts/prepare/prepare-tls.py")"
# Replace the header only when it changes, so Make rebuilds only what it must.
"$python" "$repo_root/scripts/build/configure-connection.py" "$profile" "$output/dswt-connection.h.new"
if cmp -s "$output/dswt-connection.h.new" "$output/dswt-connection.h"; then
    rm "$output/dswt-connection.h.new"
else
    mv "$output/dswt-connection.h.new" "$output/dswt-connection.h"
fi

# UTF-8 tables and the checksum-verified pixel font stay in the build directory.
"$python" "$repo_root/scripts/build/companion-text.py" --output "$output/build/text"

image="dswt-companion-builder"
builder="$repo_root/tools/builders/companion-builder"
docker build -f "$builder/Dockerfile" -t "$image" "$builder"
# The sources are read-only; everything the build writes goes to /out.
docker run --rm -e "CP_CARD_SAVE_WRITES=${CP_CARD_SAVE_WRITES:-0}" --user "$(id -u):$(id -g)" \
    -v "$repo_root/client:/dswt/client:ro" \
    -v "$tls_workspace:/tls:ro" \
    -v "$output:/out" \
    -w /dswt/client/companion "$image" \
    make -j"$build_jobs" BUILDDIR=/out/build ROM=/out/dswt-companion.nds \
        TLS_DIR=/tls CONNECTION_HEADER=/out/dswt-connection.h
echo "$output/dswt-companion.nds"
