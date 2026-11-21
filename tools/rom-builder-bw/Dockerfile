FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ENV DEBIAN_FRONTEND=noninteractive
# Project-Holon/aletheia builds Black and White with its Python tools, GNU ARM
# binutils and the CodeWarrior compilers and linker, Windows programs run
# under Wine. Packages come from a fixed Ubuntu snapshot so the toolchain stays
# reproducible. The base has no CAs, so HTTPS is bootstrapped from a
# checksum-pinned package in the same snapshot, keeping TLS and APT signature
# checks enabled.
ADD --checksum=sha256:8a0a754aaafed72cc8efb8c1731808133284a84fbfc76a4fcc73fce9e01b4187 https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.1_all.deb /tmp/ca.deb
RUN dpkg-deb --extract /tmp/ca.deb /tmp/ca \
    && mkdir -p /etc/ssl/certs \
    && cat /tmp/ca/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt \
    && rm -rf /tmp/ca /tmp/ca.deb \
    && printf '%s\n' 'Types: deb' 'URIs: https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/' \
        'Suites: noble noble-updates noble-security' 'Components: main universe' \
        'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' 'Check-Valid-Until: no' \
        > /etc/apt/sources.list.d/ubuntu.sources
RUN dpkg --add-architecture i386 \
    && apt-get -o APT::Update::Error-Mode=any update \
    && apt-get install --yes --no-install-recommends \
        binutils-arm-none-eabi \
        python3 \
        unzip \
        wine \
        wine32:i386 \
    && rm -rf /var/lib/apt/lists/*

# The CodeWarrior releases aletheia's README lists (dsi/1.1p1, 2.0/sp2,
# 2.0/sp1p2, 2.0/sp2p2, 1.2/sp2 and 1.2, the 1.2 base release), by checksum
# from the same archive pokebw2 downloads. They are copied into each
# workspace's ignored tools/mwccarm/ at build time and never leave this local
# image.
ADD --checksum=sha256:6f7b16098d2b1bc9b45b278f6e6e24327c4da9b3ebf7924be1afd4a721d83976 https://decomp.aetias.com/files/mwccarm.zip /opt/aletheia/mwccarm.zip
RUN cd /opt/aletheia \
    && unzip -q mwccarm.zip -d tools \
    && rm mwccarm.zip \
    && cp -R tools/mwccarm/1.2/base/. tools/mwccarm/1.2/ \
    && chmod -R a+rX /opt/aletheia

# Builds run as the invoking host user, who creates a Wine prefix per run.
ENV WINEDEBUG=-all WINEDLLOVERRIDES=mscoree,mshtml= HOME=/tmp
WORKDIR /workspace
