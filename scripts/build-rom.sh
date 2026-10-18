#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prepare_args=()
build_mode="modified"
if [[ $# == 1 && $1 == --pristine ]]; then
    prepare_args+=(--pristine)
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
trade_url="${WONDER_TRADE_BASE_URL:-http://10.64.0.1:8080}"
if [[ "$build_mode" == modified && ! "$trade_url" =~ ^http://[a-zA-Z0-9.-]+(:[0-9]+)?$ ]]; then
    echo "WONDER_TRADE_BASE_URL must be http://host[:port] without a trailing slash" >&2
    exit 1
fi

workspace="$(python3 "$repo_root/scripts/prepare-game.py" "${prepare_args[@]}")"
docker build -f "$repo_root/tools/rom-builder/Dockerfile" -t "$image" "$repo_root/tools/rom-builder"

docker_args=(--rm --user "$(id -u):$(id -g)" -e "WONDER_TRADE_BASE_URL=$trade_url" -e "ROM_REVISION=$revision" -e "PLATINUMWT_BUILD_MODE=$build_mode" -v "$workspace:/workspace" -w /workspace)

exec docker run "${docker_args[@]}" "$image" bash -c '
    set -euo pipefail
    make configure
    if [[ "$PLATINUMWT_BUILD_MODE" == pristine ]]; then
        python3 subprojects/meson-1.12.0/meson.py configure build -Drevision="$ROM_REVISION"
        make release check
    else
        python3 subprojects/meson-1.12.0/meson.py configure build \
            -Drevision="$ROM_REVISION" \
            -Dwonder_trade_base_url="$WONDER_TRADE_BASE_URL"
        make release
    fi
'
