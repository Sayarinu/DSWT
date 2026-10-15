#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="platinumwt-rom-builder"
trade_url="${WONDER_TRADE_BASE_URL:-http://10.64.0.1:8080}"
if [[ ! "$trade_url" =~ ^http://[a-zA-Z0-9.-]+(:[0-9]+)?$ ]]; then
    echo "WONDER_TRADE_BASE_URL must be http://host[:port] without a trailing slash" >&2
    exit 1
fi

docker build -f "$repo_root/tools/rom-builder/Dockerfile" -t "$image" "$repo_root/tools/rom-builder"

docker_args=(--rm --user "$(id -u):$(id -g)" -e "WONDER_TRADE_BASE_URL=$trade_url" -v "$repo_root/game/pokeplatinum:/workspace" -w /workspace)
if [[ -n "${ROM_REVISION:-}" ]]; then
    docker_args+=(-e "ROM_REVISION=$ROM_REVISION")
fi

exec docker run "${docker_args[@]}" "$image" bash -c '
    set -euo pipefail
    make configure
    python3 subprojects/meson-1.12.0/meson.py configure build \
        -Drevision="${ROM_REVISION:-1}" \
        -Dwonder_trade_base_url="$WONDER_TRADE_BASE_URL"
    make release
'
