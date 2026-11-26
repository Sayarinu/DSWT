# dsd from pokebw2's pinned release source, with one change: keep the stored DSi
# secure-area digests when ARM9's first 0x4000 bytes change
# (ds-rom-stale-secure-area.patch, used when DSD_ALLOW_STALE_SECURE_AREA is
# set). Without an ARM7 BIOS the build cannot recompute them, and Wonder Trade
# moves ARM9's data. Such a ROM runs in DS mode only. Crates come from
# Cargo.lock, whose crates.io entries carry their checksums.
FROM rust:1.100-slim-bookworm@sha256:7c7d4eb4d05b7781187a327809466cfddf29753f6defdc3d19a0e291951b7a37 AS dsd
ADD --checksum=sha256:41dfddf8ee8ae9153459403ae522db2993724e675c41768132e1a93bd65db3e3 https://codeload.github.com/fuddlesworth/ds-decomp/tar.gz/refs/tags/v0.12.1-dsi.10 /src/ds-decomp.tar.gz
ADD --checksum=sha256:148bd6cc6628fa36ec5e4420a59b9f7276f39a3c35a9f16f3bf489331fd45b49 https://codeload.github.com/fuddlesworth/ds-rom/tar.gz/66e83ef467b1e7a4816e667bf3b86572b1f8072d /src/ds-rom.tar.gz
COPY ds-rom-stale-secure-area.patch /src/
# GNU patch, from a fixed Debian snapshot
RUN printf '%s\n' 'deb https://snapshot.debian.org/archive/debian/20261101T000000Z bookworm main' \
        > /etc/apt/sources.list \
    && rm -f /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Check-Valid-Until=false -o APT::Update::Error-Mode=any update \
    && apt-get install --yes --no-install-recommends patch \
    && rm -rf /var/lib/apt/lists/*
RUN cd /src \
    && tar xzf ds-decomp.tar.gz && tar xzf ds-rom.tar.gz \
    && mv ds-decomp-0.12.1-dsi.10 ds-decomp && mv ds-rom-66e83ef467b1e7a4816e667bf3b86572b1f8072d ds-rom \
    && cd ds-rom && patch -p1 < /src/ds-rom-stale-secure-area.patch && cd ../ds-decomp \
    && sed -i 's|^ds-rom = { git = "https://github.com/fuddlesworth/ds-rom", rev = "66e83ef467b1e7a4816e667bf3b86572b1f8072d" }$|ds-rom = { path = "../ds-rom/lib" }|' Cargo.toml \
    && grep -q '^ds-rom = { path = "../ds-rom/lib" }$' Cargo.toml \
    && cargo build --release --bin dsd \
    && install -m 0755 target/release/dsd /dsd

FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ENV DEBIAN_FRONTEND=noninteractive
# fuddlesworth/pokebw2 builds with its configure.py, Ninja and LLVM, and runs
# Metrowerks compilers (Windows programs) through wibo. Packages come from a
# fixed Ubuntu snapshot so the toolchain stays reproducible. The base has no
# CAs, so HTTPS is bootstrapped from a checksum-pinned package in the same
# snapshot, keeping TLS and APT signature checks enabled.
ADD --checksum=sha256:8a0a754aaafed72cc8efb8c1731808133284a84fbfc76a4fcc73fce9e01b4187 https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.1_all.deb /tmp/ca.deb
RUN dpkg-deb --extract /tmp/ca.deb /tmp/ca \
    && mkdir -p /etc/ssl/certs \
    && cat /tmp/ca/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt \
    && rm -rf /tmp/ca /tmp/ca.deb \
    && printf '%s\n' 'Types: deb' 'URIs: https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/' \
        'Suites: noble noble-updates noble-security' 'Components: main universe' \
        'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' 'Check-Valid-Until: no' \
        > /etc/apt/sources.list.d/ubuntu.sources
RUN apt-get -o APT::Update::Error-Mode=any update \
    && apt-get install --yes --no-install-recommends \
        clang \
        llvm \
        ninja-build \
        python3 \
        unzip \
    && rm -rf /var/lib/apt/lists/*

# The tools pokebw2's configure.py would download at the pinned revision, here
# by checksum: wibo and sjiswrap at the versions and SHA-256s it pins, the
# Metrowerks compilers, and dsd built above from the release it names. They are copied into each
# workspace's ignored tools/ at build time and never leave this local image.
ADD --checksum=sha256:d708570bd64cc8c176273a2bb7b5082f4fa849139d0ffe6cd0b96e26f3bc1ef2 https://github.com/fuddlesworth/wibo/releases/download/pokebw2-sjis-1/wibo-x86_64 /opt/pokebw2/tools/wibo
ADD --checksum=sha256:4e9c917ba3f245da74a5a48ec91b459e956189cc1411233aa40046e0f7ac294f https://github.com/fuddlesworth/sjiswrap/releases/download/v1.2.2-pokebw2.1/sjiswrap-windows-x86.exe /opt/pokebw2/tools/sjiswrap.exe
ADD --checksum=sha256:6f7b16098d2b1bc9b45b278f6e6e24327c4da9b3ebf7924be1afd4a721d83976 https://decomp.aetias.com/files/mwccarm.zip /opt/pokebw2/mwccarm.zip
RUN cd /opt/pokebw2 \
    && unzip -q mwccarm.zip -d tools \
    && rm mwccarm.zip \
    && echo v0.12.1-dsi.10 > tools/dsd.rev \
    && touch tools/mwccarm/dswt-rom-builder-bw2 \
    && chmod a+x tools/wibo \
    && chmod -R a+rX /opt/pokebw2

# The patched dsd replaces the release binary. Its stamp keeps configure.py
# from replacing it.
COPY --from=dsd /dsd /opt/pokebw2/tools/dsd

# Wonder Trade's core and Mbed TLS compile against CodeWarrior's MSL C headers,
# which pokebw2's partial C library lacks, from the Metroskrew release
# pokeplatinum pins (the same archive as tools/builders/rom-builder-dp). Only the
# headers are kept, inside this local image.
ADD --checksum=sha256:2fec5ba308690a25157afc532ed87411a413501d43d85892ae4bfb9eb3975790 https://github.com/mid-kid/metroskrew/releases/download/0.1.3/metroskrew-linux.tar.xz /opt/pokebw2/metroskrew.tar.xz
RUN apt-get -o APT::Update::Error-Mode=any update \
    && apt-get install --yes --no-install-recommends xz-utils \
    && rm -rf /var/lib/apt/lists/* \
    && cd /opt/pokebw2 \
    && tar xJf metroskrew.tar.xz --strip-components=7 \
        metroskrew/lib/metroskrew/sdk/ds/2.0/sp1/msl/MSL_C/MSL_Common/Include \
        metroskrew/lib/metroskrew/sdk/ds/2.0/sp1/msl/MSL_C/MSL_ARM/Include \
    && rm metroskrew.tar.xz \
    && chmod -R a+rX /opt/pokebw2/msl

# Builds run as the invoking host user.
ENV HOME=/tmp
WORKDIR /workspace
