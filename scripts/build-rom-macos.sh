#!/usr/bin/env bash
# Sourced by build-rom.sh; shares its validated game, mode, paths and job count.

macos_error() {
    echo "$*" >&2
    exit 2
}

check_macos_build() {
    case "$game" in
        diamond|pearl|black2|white2) ;;
        *) macos_error 'ROM_BUILD_HOST=macos supports diamond, pearl, black2 or white2' ;;
    esac
    [[ "$repo_root" != *[[:space:]]* ]] || macos_error 'Native builds require a repository path without whitespace; use a space-free checkout.'
    [[ "$(uname -s)" == Darwin ]] || macos_error 'ROM_BUILD_HOST=macos requires macOS.'
    command -v brew >/dev/null || macos_error 'Install Homebrew, then follow Build-from-Source in the wiki for native prerequisites.'
    local prefix
    prefix="$(brew --prefix)"
    export PATH="$prefix/opt/make/libexec/gnubin:$prefix/opt/coreutils/libexec/gnubin:$prefix/opt/gnu-sed/libexec/gnubin:$prefix/opt/gnu-tar/libexec/gnubin:$prefix/opt/llvm/bin:$PATH"
    [[ "$(tar --version 2>/dev/null)" == *'GNU tar'* ]] || macos_error 'GNU tar is required: brew install gnu-tar (tar must support --skip-old-files).'
    local tool
    for tool in docker clang llvm-objcopy llvm-ar sha1sum; do
        command -v "$tool" >/dev/null || macos_error "Missing $tool. Install Docker Desktop and brew install coreutils llvm; see Build-from-Source in the wiki."
    done
    arch -x86_64 /usr/bin/true 2>/dev/null || macos_error 'x86-64 execution is required. On Apple Silicon install Rosetta 2: softwareupdate --install-rosetta --agree-to-license'
    if [[ "$game" == diamond || "$game" == pearl ]]; then
        [[ "$(make --version)" == 'GNU Make 4.'* ]] || macos_error 'GNU Make 4 is required: brew install make'
        for tool in gcc g++ gsed pkg-config arm-none-eabi-ar arm-none-eabi-objcopy arm-none-eabi-objdump wine wineboot wineserver; do
            command -v "$tool" >/dev/null || macos_error "Missing $tool. Install Xcode command line tools, Wine Stable, and brew install make coreutils gnu-sed pkg-config libpng arm-none-eabi-binutils; see Build-from-Source in the wiki."
        done
        pkg-config --exists libpng zlib || macos_error 'libpng and zlib headers are required: brew install libpng pkg-config'
    else
        for tool in ninja patch; do
            command -v "$tool" >/dev/null || macos_error "Missing $tool. Install Rust (rustup), Xcode command line tools, and brew install ninja llvm; see Build-from-Source in the wiki."
        done
        if [[ "$build_mode" == modified ]]; then
            cargo +1.100 --version >/dev/null 2>&1 || macos_error 'Rust 1.100 is required for native dsd: rustup toolchain install 1.100'
        fi
    fi
}

build_macos_rom() {
    local tools
    for tools in "$workspace" "${tls_workspace:-}"; do
        [[ "$tools" != *[[:space:]]* ]] || macos_error 'Native workspace and TLS paths must not contain whitespace.'
    done
    tools="$("$python" "$repo_root/scripts/prepare-rom-macos.py" "$game" "$build_mode")"
    cd "$workspace"
    export ROM_BUILD_JOBS="$build_jobs"
    if [[ "$game" == diamond || "$game" == pearl ]]; then
        if [[ ! -f tools/mwccarm/license.dat ]]; then
            mkdir -p tools/mwccarm tools/bin
            cp -R "$tools/mwccarm/." tools/mwccarm/
            cp -R "$tools/sdk-bin/." tools/bin/
            cp "$tools/ARM7-TS.lcf.template" arm7/
            cp "$tools/ARM9-TS.lcf.template" arm9/
        fi
        # Modern Apple libc++ removed the experimental alias used by nitrobanner.
        mkdir -p .git/dswt-macos/experimental
        cat > .git/dswt-macos/experimental/filesystem <<'HEADER'
#pragma once
#include <filesystem>
namespace std { namespace experimental { namespace filesystem = std::filesystem; } }
HEADER
        export CPLUS_INCLUDE_PATH="$workspace/.git/dswt-macos${CPLUS_INCLUDE_PATH:+:$CPLUS_INCLUDE_PATH}"
        export WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
        WINEPREFIX="$(mktemp -d "$repo_root/game/wine-XXXXXXXX")"
        export WINEPREFIX
        trap 'wineserver -k 2>/dev/null || true; wineserver -w 2>/dev/null || true; rm -rf "$WINEPREFIX"' EXIT
        wineserver -p
        if ! wineboot -i >"$WINEPREFIX/boot.log" 2>&1; then
            cat "$WINEPREFIX/boot.log" >&2
            macos_error 'Wine initialization failed; check your Wine Stable installation.'
        fi
        make tools
        # Apple clang creates bundles that upstream's recursive tool scan mistakes for sources.
        find tools -name '*.dSYM' -prune -exec rm -rf {} +
        local version
        version="$(echo "$game" | tr '[:lower:]' '[:upper:]')"
        if [[ "$build_mode" == pristine ]]; then
            make -j"$build_jobs" GAME_VERSION="$version" COMPARE=1
        else
            make -j"$build_jobs" GAME_VERSION="$version" COMPARE=0 \
                WT_TLS_DIR="$tls_workspace" WT_MSL_DIR="$tools/msl" \
                WT_PROFILE_HEADER="$workspace/.git/dswt-connection.h"
        fi
    else
        local version="${game:0:1}2_us"
        # configure.py selects and verifies its macOS wibo, rather than Linux's binary.
        if [[ "$build_mode" == pristine ]]; then
            "$python" configure.py "$version"
            ninja -j"$build_jobs"
        else
            export DSD_ALLOW_STALE_SECURE_AREA=1
            "$python" configure.py "$version" --dsd "$tools/dsd" \
                --wonder-trade-tls "$tls_workspace" \
                --wonder-trade-profile "$workspace/.git/dswt-connection.h" \
                --wonder-trade-msl "$tools/msl"
            ninja -j"$build_jobs" "build/poke${game}_us.nds"
        fi
    fi
}
