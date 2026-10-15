#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="platinumwt-rom-builder"

docker build -f "$repo_root/tools/rom-builder/Dockerfile" -t "$image" "$repo_root/tools/rom-builder"

docker_args=(--rm --user "$(id -u):$(id -g)" -v "$repo_root/game/pokeplatinum:/workspace" -w /workspace)
if [[ -n "${ROM_REVISION:-}" ]]; then
    docker_args+=(-e "ROM_REVISION=$ROM_REVISION")
fi

exec docker run "${docker_args[@]}" "$image" make release
