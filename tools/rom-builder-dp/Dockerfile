FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ENV DEBIAN_FRONTEND=noninteractive
# pret/pokediamond builds with Metrowerks compilers and Nitro SDK 3.2 tools,
# which are Windows programs run under Wine. Packages come from a fixed Ubuntu
# snapshot so the toolchain stays reproducible. The base has no CAs, so HTTPS is
# bootstrapped from a checksum-pinned package in the same snapshot, keeping TLS
# and APT signature checks enabled.
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
        g++ \
        libpng-dev \
        libpugixml-dev \
        make \
        p7zip-full \
        pkg-config \
        python3 \
        unzip \
        wine \
        wine32:i386 \
    && rm -rf /var/lib/apt/lists/*

# The compilers and SDK tools upstream's own CI uses, from a fixed commit of its
# assets/workflow branch. They are copied into each workspace's ignored tools/
# at build time and never leave this local image.
ADD --checksum=sha256:dc386e37b2176e960954a733de928337d9b7e7620b9e94228008c31774339ace https://raw.githubusercontent.com/pret/pokediamond/2019dffc6455d29036d04c233abdfb0d9e7c5894/mwccarm.zip /opt/pokediamond/mwccarm.zip
ADD --checksum=sha256:0ee935e34237cb1954e657083cd80a31ec9de1aeddd9468cceef684cac75b491 https://raw.githubusercontent.com/pret/pokediamond/2019dffc6455d29036d04c233abdfb0d9e7c5894/NitroSDK-3_2-060901.7z /opt/pokediamond/NitroSDK-3_2-060901.7z
RUN cd /opt/pokediamond \
    && unzip -q mwccarm.zip \
    && 7z x -bd -y NitroSDK-3_2-060901.7z \
        NitroSDK-3_2-060901/tools/bin \
        NitroSDK-3_2-060901/include/nitro/specfiles/ARM7-TS.lcf.template \
        NitroSDK-3_2-060901/include/nitro/specfiles/ARM9-TS.lcf.template >/dev/null \
    && mv NitroSDK-3_2-060901/tools/bin sdk-bin \
    && mv NitroSDK-3_2-060901/include/nitro/specfiles/*.lcf.template . \
    && rm -rf NitroSDK-3_2-060901 mwccarm.zip NitroSDK-3_2-060901.7z \
    && chmod -R a+rX /opt/pokediamond

# Builds run as the invoking host user, who creates a Wine prefix per run.
ENV WINEDEBUG=-all WINEDLLOVERRIDES=mscoree,mshtml= HOME=/tmp
WORKDIR /workspace
