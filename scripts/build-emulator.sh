#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
emulator_source="$(python3 "$repo_root/scripts/prepare-emulator.py")"
emulator_build="${emulator_source}-build"
cmake -S "$emulator_source" -B "$emulator_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_WAYLAND=OFF -DENABLE_LTO_RELEASE=OFF \
    -DENABLE_GDBSTUB=OFF -DUSE_SYSTEM_LIBSLIRP=ON
cmake --build "$emulator_build" --parallel 2
cmake -S "$repo_root/emulator" -B "$repo_root/game/save-store-build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$repo_root/game/save-store-build" --parallel 2
ctest --test-dir "$repo_root/game/save-store-build" --output-on-failure
printf 'Emulator: %s/melonDS\nSave tool: %s/game/save-store-build/dswt-save-store\n' "$emulator_build" "$repo_root"
