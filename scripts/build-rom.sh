#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_mode="modified"
if [[ $# == 1 && $1 == --pristine ]]; then
    build_mode="pristine"
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--pristine]" >&2
    exit 2
fi
revision="${ROM_REVISION:-1}"
if [[ ! "$revision" =~ ^[01]$ ]]; then
    echo "ROM_REVISION must be 0 or 1" >&2
    exit 2
fi
image="platinumwt-rom-builder"
if [[ "$build_mode" == modified && -z "${WONDER_TRADE_PROFILE:-}" ]]; then
    echo "Set WONDER_TRADE_PROFILE to your local public endpoint/pins JSON (see client/tests/README.md)" >&2
    exit 2
fi

python="${PYTHON:-python3}"
if ! "$python" -c 'import sys; sys.exit(sys.version_info < (3, 12))'; then
    echo "Python 3.12 or newer is required. Install it separately and set PYTHON=/path/to/python3.12; leave the system Python unchanged." >&2
    exit 2
fi

workspace="$("$python" "$repo_root/scripts/prepare-game.py" "$@")"
docker_args=(--platform linux/amd64 --rm --user "$(id -u):$(id -g)" -e "ROM_REVISION=$revision" -e "PLATINUMWT_BUILD_MODE=$build_mode" -v "$workspace:/workspace" -w /workspace)
if [[ "$build_mode" == modified ]]; then
    tls_workspace="$("$python" "$repo_root/scripts/prepare-tls.py")"
    "$python" "$repo_root/scripts/configure-connection.py" "$WONDER_TRADE_PROFILE" "$workspace/.git/platinumwt-connection.h"
    docker_args+=(-v "$tls_workspace:/tls:ro")
fi
docker build --platform linux/amd64 -f "$repo_root/tools/rom-builder/Dockerfile" -t "$image" "$repo_root/tools/rom-builder"

exec docker run "${docker_args[@]}" "$image" bash -c '
    set -euo pipefail
    if [[ "$PLATINUMWT_BUILD_MODE" == pristine ]]; then
        make configure
        python3 subprojects/meson-1.12.0/meson.py configure build -Drevision="$ROM_REVISION"
        make release check
    else
        if [[ ! -f build/build.ninja ]]; then
            make meson skrew build/lsfgen.stamp
            python3 subprojects/meson-1.12.0/meson.py setup build \
                --wrap-mode=nopromote \
                --native-file=meson/native.ini \
                --cross-file=meson/cross_unix.ini \
                -Drevision="$ROM_REVISION" \
                -Dwonder_trade_tls_dir=/tls \
                -Dwonder_trade_profile_header=/workspace/.git/platinumwt-connection.h
        fi
        python3 subprojects/meson-1.12.0/meson.py configure build \
            -Drevision="$ROM_REVISION" \
            -Dwonder_trade_tls_dir=/tls \
            -Dwonder_trade_profile_header=/workspace/.git/platinumwt-connection.h
        make release
    fi
'
