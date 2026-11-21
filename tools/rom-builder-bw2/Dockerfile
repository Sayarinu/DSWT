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
# Metrowerks compilers and the dsd release it names. They are copied into each
# workspace's ignored tools/ at build time and never leave this local image.
ADD --checksum=sha256:d708570bd64cc8c176273a2bb7b5082f4fa849139d0ffe6cd0b96e26f3bc1ef2 https://github.com/fuddlesworth/wibo/releases/download/pokebw2-sjis-1/wibo-x86_64 /opt/pokebw2/tools/wibo
ADD --checksum=sha256:4e9c917ba3f245da74a5a48ec91b459e956189cc1411233aa40046e0f7ac294f https://github.com/fuddlesworth/sjiswrap/releases/download/v1.2.2-pokebw2.1/sjiswrap-windows-x86.exe /opt/pokebw2/tools/sjiswrap.exe
ADD --checksum=sha256:f344007027f6c86c951c81c70d5e7964ef8e6bb56157fbaefb4fc879a39ff2ac https://github.com/fuddlesworth/ds-decomp/releases/download/v0.12.1-dsi.10/dsd-linux-x86_64 /opt/pokebw2/tools/dsd
ADD --checksum=sha256:6f7b16098d2b1bc9b45b278f6e6e24327c4da9b3ebf7924be1afd4a721d83976 https://decomp.aetias.com/files/mwccarm.zip /opt/pokebw2/mwccarm.zip
RUN cd /opt/pokebw2 \
    && unzip -q mwccarm.zip -d tools \
    && rm mwccarm.zip \
    && echo v0.12.1-dsi.10 > tools/dsd.rev \
    && touch tools/mwccarm/dswt-rom-builder-bw2 \
    && chmod a+x tools/wibo tools/dsd \
    && chmod -R a+rX /opt/pokebw2

# Builds run as the invoking host user.
ENV HOME=/tmp
WORKDIR /workspace
