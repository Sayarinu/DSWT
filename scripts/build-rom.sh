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
        prepare_args=(--game dp)
        ;;
    black2|white2)
        # fuddlesworth/pokebw2 builds the one USA/Europe release of each version
        # from the player's own dump.
        revision="${ROM_REVISION:-0}"
        if [[ "$revision" != 0 ]]; then
            echo "ROM_REVISION must be 0 for Black 2 and White 2" >&2
            exit 2
        fi
        if [[ ! -f "${DSWT_BW2_ROM:-}" ]]; then
            echo "Set DSWT_BW2_ROM to your own USA/Europe dump of $game" >&2
            exit 2
        fi
        prepare_args=(--game bw2)
        ;;
    black|white)
        # Project-Holon/aletheia builds the USA/Europe cartridge of each
        # version from the player's own dump. Wonder Trade is added to that
        # dump with aletheia's link as the map (scripts/port-bw.py), so a
        # modified build first runs the pristine one.
        revision="${ROM_REVISION:-0}"
        if [[ "$revision" != 0 ]]; then
            echo "ROM_REVISION must be 0 for Black and White" >&2
            exit 2
        fi
        if [[ ! -f "${DSWT_BW_ROM:-}" ]]; then
            echo "Set DSWT_BW_ROM to your own USA/Europe dump of $game" >&2
            exit 2
        fi
        prepare_args=(--game bw)
        ;;
    *)
        echo "ROM_GAME must be platinum, heartgold, soulsilver, diamond, pearl, black2, white2, black or white" >&2
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
elif [[ "$game" == black || "$game" == white ]]; then
    # aletheia runs CodeWarrior under Wine.
    image="dswt-rom-builder-bw"
    builder="$repo_root/tools/rom-builder-bw"
elif [[ "$game" == black2 || "$game" == white2 ]]; then
    # pokebw2 runs Metrowerks tools through wibo, with its own pinned dsd.
    image="dswt-rom-builder-bw2"
    builder="$repo_root/tools/rom-builder-bw2"
fi
if [[ "$build_mode" == modified && -z "${WONDER_TRADE_PROFILE:-}" ]]; then
    echo "Set WONDER_TRADE_PROFILE to your local public endpoint/pins JSON (see https://github.com/Sayarinu/DSWT/wiki/Local-Testing)" >&2
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
elif [[ "$game" == black || "$game" == white ]]; then
    # aletheia reads the dump from <target>.nds in its root, which it ignores.
    cmp -s "$DSWT_BW_ROM" "$workspace/$game.nds" 2>/dev/null || cp "$DSWT_BW_ROM" "$workspace/$game.nds"
elif [[ "$game" == black2 || "$game" == white2 ]]; then
    # pokebw2 reads the dump from its ignored orig/ and checks its SHA-1 first.
    baserom="$workspace/orig/baserom_${game:0:1}2_us.nds"
    mkdir -p "$workspace/orig"
    cmp -s "$DSWT_BW2_ROM" "$baserom" 2>/dev/null || cp "$DSWT_BW2_ROM" "$baserom"
fi
docker_args=(--platform linux/amd64 --rm --user "$(id -u):$(id -g)" -e "ROM_GAME=$game" -e "ROM_REVISION=$revision" -e "ROM_BUILD_JOBS=$build_jobs" -e "DSWT_BUILD_MODE=$build_mode" -v "$workspace:/workspace" -w /workspace)
if [[ "$build_mode" == modified ]]; then
    tls_workspace="$("$python" "$repo_root/scripts/prepare-tls.py")"
    "$python" "$repo_root/scripts/configure-connection.py" "$WONDER_TRADE_PROFILE" "$workspace/.git/dswt-connection.h"
    docker_args+=(-v "$tls_workspace:/tls:ro")
    if [[ "$game" == black || "$game" == white ]]; then
        # The port tool and the client sources, read-only.
        docker_args+=(-v "$repo_root:/dswt:ro")
    fi
fi
docker build --platform linux/amd64 -f "$builder/Dockerfile" -t "$image" "$builder"

exec docker run "${docker_args[@]}" "$image" bash -c '
    set -euo pipefail
    meson=subprojects/meson-1.12.0/meson.py
    wonder_trade=(-Dwonder_trade_tls_dir=/tls -Dwonder_trade_profile_header=/workspace/.git/dswt-connection.h)
    if [[ "$ROM_GAME" == black || "$ROM_GAME" == white ]]; then
        # Install the image'"'"'s CodeWarrior releases into the ignored tools/.
        if [[ ! -d tools/mwccarm/mwccarm/dsi/1.1p1 ]]; then
            mkdir -p tools/mwccarm/mwccarm
            cp -R /opt/aletheia/tools/mwccarm/. tools/mwccarm/mwccarm/
        fi
        WINEPREFIX="$(mktemp -d)"
        export WINEPREFIX
        wineserver -p
        wineboot -i 2>/dev/null
        # White keeps aletheia'"'"'s older paths (docs/ROMS.md). romlink.py checks
        # the rebuilt ROM'"'"'s SHA-1.
        extract=extract; [[ "$ROM_GAME" == white ]] && extract=extract_white
        report=build/rom/report.json; [[ "$ROM_GAME" == white ]] && report=build/rom/report_white.json
        # A modified build reuses a finished pristine link of the same game.
        if [[ "$DSWT_BUILD_MODE" == pristine ]] || ! grep -q "\"identical\": true" "$report" 2>/dev/null; then
            python3 tools/ndsrom.py extract "$ROM_GAME.nds" -o "$extract"
            python3 tools/delink.py -e "$extract" -l "build/$ROM_GAME/layout"
            # The data-mode links take their companions from the disassembly, which
            # recounts a tracked layout file; put it back so the workspace stays clean.
            cp layout/units_arm9.json "$WINEPREFIX/units_arm9.json"
            python3 tools/disasm.py arm9 --split
            python3 tools/disasm.py overlay --all
            python3 tools/romlink.py --target "$ROM_GAME" --relink
            cp "$WINEPREFIX/units_arm9.json" layout/units_arm9.json
        fi
        if [[ "$DSWT_BUILD_MODE" == modified ]]; then
            # Wonder Trade goes into the dump; the port checks it against the aletheia link.
            mkdir -p build/dswt
            rm -f "build/dswt/$ROM_GAME.nds"
            python3 /dswt/scripts/port-bw.py "$ROM_GAME" "$ROM_GAME.nds" "build/dswt/$ROM_GAME.nds" \
                --workspace /workspace --profile /workspace/.git/dswt-connection.h --tls /tls \
                --msl /opt/aletheia/msl --work "build/dswt/port-$ROM_GAME"
        fi
        exit
    fi
    if [[ "$ROM_GAME" == black2 || "$ROM_GAME" == white2 ]]; then
        # Install the image'"'"'s pinned tools into the ignored tools/, unlike
        # the ones configure.py downloads for a native build. Ninja builds the
        # version whose dump is in orig/ and checks its SHA-1.
        if [[ ! -f tools/mwccarm/dswt-rom-builder-bw2 ]]; then
            cp -R /opt/pokebw2/tools/. tools/
        fi
        version="${ROM_GAME:0:1}2_us"
        if [[ "$DSWT_BUILD_MODE" == pristine ]]; then
            python3 configure.py --no-download "$version"
            ninja -j"$ROM_BUILD_JOBS"
        else
            # Wonder Trade links into ARM9 main, which changes its first 0x4000
            # bytes; the image'"'"'s dsd then keeps the stored DSi digests, so the
            # ROM runs in DS mode only. A modified build skips the SHA-1 checks.
            export DSD_ALLOW_STALE_SECURE_AREA=1
            python3 configure.py --no-download "$version" --wonder-trade-tls /tls \
                --wonder-trade-profile /workspace/.git/dswt-connection.h --wonder-trade-msl /opt/pokebw2/msl
            if [[ "$ROM_GAME" == black2 ]]; then rom=build/pokeblack2_us.nds; else rom=build/pokewhite2_us.nds; fi
            ninja -j"$ROM_BUILD_JOBS" "$rom"
        fi
        exit
    fi
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
        if [[ "$DSWT_BUILD_MODE" == pristine ]]; then
            # COMPARE=1 checks the ARM9, ARM7 and ROM against upstream'"'"'s SHA-1s.
            make -j"$ROM_BUILD_JOBS" GAME_VERSION="${ROM_GAME^^}" COMPARE=1
        else
            # The integration patch builds Mbed TLS and the client against the
            # image'"'"'s MSL C headers and the mounted TLS sources.
            make -j"$ROM_BUILD_JOBS" GAME_VERSION="${ROM_GAME^^}" COMPARE=0 \
                WT_TLS_DIR=/tls WT_MSL_DIR=/opt/pokediamond/msl \
                WT_PROFILE_HEADER=/workspace/.git/dswt-connection.h
        fi
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
