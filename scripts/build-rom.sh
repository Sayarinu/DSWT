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
game="${ROM_GAME:-platinum}"
prepare_args=()
case "$game" in
    platinum)
        revision="${ROM_REVISION:-1}"
        if [[ ! "$revision" =~ ^(0|1|10)$ ]]; then
            echo "ROM_REVISION must be 0, 1 or 10 (European English)" >&2
            exit 2
        fi
        if [[ "$revision" == 10 && ! -f "${DSWT_EUROPE_ROM:-}" ]]; then
            echo "Set DSWT_EUROPE_ROM to your own European English Platinum dump" >&2
            exit 2
        fi
        ;;
    heartgold|soulsilver)
        # pret/pokeheartgold builds the one US revision of each version.
        revision="${ROM_REVISION:-0}"
        if [[ "$revision" != 0 ]]; then
            echo "ROM_REVISION must be 0 for HeartGold and SoulSilver" >&2
            exit 2
        fi
        prepare_args=(--game hgss)
        ;;
    diamond|pearl)
        # pret/pokediamond builds the US revision 5 of each version.
        revision="${ROM_REVISION:-5}"
        if [[ "$revision" != 5 ]]; then
            echo "ROM_REVISION must be 5 for Diamond and Pearl" >&2
            exit 2
        fi
        if [[ "$build_mode" != pristine ]]; then
            echo "Diamond and Pearl support only --pristine builds" >&2
            exit 2
        fi
        prepare_args=(--game dp)
        ;;
    *)
        echo "ROM_GAME must be platinum, heartgold, soulsilver, diamond or pearl" >&2
        exit 2
        ;;
esac
build_jobs="${ROM_BUILD_JOBS-1}"
if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "ROM_BUILD_JOBS must be a positive integer without leading zeros" >&2
    exit 2
fi
image="dswt-rom-builder"
builder="$repo_root/tools/rom-builder"
if [[ "$game" == diamond || "$game" == pearl ]]; then
    # pokediamond's Make build runs Metrowerks and Nitro SDK 3.2 tools under Wine.
    image="dswt-rom-builder-dp"
    builder="$repo_root/tools/rom-builder-dp"
fi
if [[ "$build_mode" == modified && -z "${WONDER_TRADE_PROFILE:-}" ]]; then
    echo "Set WONDER_TRADE_PROFILE to your local public endpoint/pins JSON (see client/tests/README.md)" >&2
    exit 2
fi

python="${PYTHON:-python3}"
if ! "$python" -c 'import sys; sys.exit(sys.version_info < (3, 12))'; then
    echo "Python 3.12 or newer is required. Install it separately and set PYTHON=/path/to/python3.12; leave the system Python unchanged." >&2
    exit 2
fi

workspace="$("$python" "$repo_root/scripts/prepare-game.py" ${prepare_args[@]+"${prepare_args[@]}"} "$@")"
if [[ "$game" == platinum && "$revision" == 10 ]]; then
    "$python" "$repo_root/scripts/prepare-europe.py" "$DSWT_EUROPE_ROM" "$workspace" >/dev/null
fi
docker_args=(--platform linux/amd64 --rm --user "$(id -u):$(id -g)" -e "ROM_GAME=$game" -e "ROM_REVISION=$revision" -e "ROM_BUILD_JOBS=$build_jobs" -e "DSWT_BUILD_MODE=$build_mode" -v "$workspace:/workspace" -w /workspace)
if [[ "$build_mode" == modified ]]; then
    tls_workspace="$("$python" "$repo_root/scripts/prepare-tls.py")"
    "$python" "$repo_root/scripts/configure-connection.py" "$WONDER_TRADE_PROFILE" "$workspace/.git/dswt-connection.h"
    docker_args+=(-v "$tls_workspace:/tls:ro")
fi
docker build --platform linux/amd64 -f "$builder/Dockerfile" -t "$image" "$builder"

exec docker run "${docker_args[@]}" "$image" bash -c '
    set -euo pipefail
    meson=subprojects/meson-1.12.0/meson.py
    wonder_trade=(-Dwonder_trade_tls_dir=/tls -Dwonder_trade_profile_header=/workspace/.git/dswt-connection.h)
    if [[ "$ROM_GAME" == diamond || "$ROM_GAME" == pearl ]]; then
        # Install the image'"'"'s pinned compilers and SDK tools into ignored paths.
        if [[ ! -f tools/mwccarm/license.dat ]]; then
            mkdir -p tools/mwccarm tools/bin
            cp -R /opt/pokediamond/mwccarm/. tools/mwccarm/
            cp -R /opt/pokediamond/sdk-bin/. tools/bin/
            cp /opt/pokediamond/ARM7-TS.lcf.template arm7/
            cp /opt/pokediamond/ARM9-TS.lcf.template arm9/
        fi
        # Wine needs a prefix owned by the build user, created before parallel
        # jobs start, and one persistent server for those jobs.
        WINEPREFIX="$(mktemp -d)"
        export WINEPREFIX
        wineserver -p
        wineboot -i 2>/dev/null
        # COMPARE=1 checks the ARM9, ARM7 and ROM against upstream'"'"'s SHA-1s.
        make -j"$ROM_BUILD_JOBS" GAME_VERSION="${ROM_GAME^^}" COMPARE=1
        exit
    fi
    # Make exports NINJA; clear its command-with-flags before Meson regeneration.
    if [[ "$ROM_GAME" != platinum ]]; then
        # One HeartGold/SoulSilver workspace keeps a build directory per version.
        version="${ROM_GAME^^}"
        build="build/$ROM_GAME.us"
        if [[ "$DSWT_BUILD_MODE" == pristine ]]; then
            make GAME_VERSION="$version" setup_rom
            make GAME_VERSION="$version" release NINJA="env -u NINJA ninja -j${ROM_BUILD_JOBS}"
            # Meson checks require NINJA to name one executable, without flags.
            make GAME_VERSION="$version" check NINJA=ninja
        else
            if [[ ! -f "$build/build.ninja" ]]; then
                make GAME_VERSION="$version" meson skrew "$build/lsfgen.stamp"
                python3 "$meson" setup "$build" \
                    --wrap-mode=nopromote \
                    --native-file=meson/native.ini \
                    --cross-file=meson/cross_unix.ini \
                    -Dgame_version="$version" "${wonder_trade[@]}"
            fi
            python3 "$meson" configure "$build" -Dgame_version="$version" "${wonder_trade[@]}"
            make GAME_VERSION="$version" release NINJA="env -u NINJA ninja -j${ROM_BUILD_JOBS}"
        fi
    elif [[ "$DSWT_BUILD_MODE" == pristine ]]; then
        make configure
        python3 "$meson" configure build -Drevision="$ROM_REVISION"
        make release NINJA="env -u NINJA ninja -j${ROM_BUILD_JOBS}"
        # Meson checks require NINJA to name one executable, without flags.
        make check NINJA=ninja
    else
        if [[ ! -f build/build.ninja ]]; then
            make meson skrew build/lsfgen.stamp
            python3 "$meson" setup build \
                --wrap-mode=nopromote \
                --native-file=meson/native.ini \
                --cross-file=meson/cross_unix.ini \
                -Drevision="$ROM_REVISION" "${wonder_trade[@]}"
        fi
        python3 "$meson" configure build -Drevision="$ROM_REVISION" "${wonder_trade[@]}"
        make release NINJA="env -u NINJA ninja -j${ROM_BUILD_JOBS}"
    fi
'
