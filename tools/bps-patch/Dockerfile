FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac AS build

ENV DEBIAN_FRONTEND=noninteractive
# Flips links statically, so the toolchain comes from a fixed Ubuntu snapshot:
# archive updates (such as a new glibc) must not change the pinned binary. The
# base has no CAs, so HTTPS is bootstrapped from a checksum-pinned package in
# the same snapshot, keeping TLS and APT signature checks enabled.
ADD --checksum=sha256:8a0a754aaafed72cc8efb8c1731808133284a84fbfc76a4fcc73fce9e01b4187 https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.1_all.deb /tmp/ca.deb
RUN dpkg-deb --extract /tmp/ca.deb /tmp/ca \
    && mkdir -p /etc/ssl/certs \
    && cat /tmp/ca/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt \
    && rm -rf /tmp/ca /tmp/ca.deb \
    && printf '%s\n' 'Types: deb' 'URIs: https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/' \
        'Suites: noble noble-updates noble-security' 'Components: main' \
        'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' 'Check-Valid-Until: no' \
        > /etc/apt/sources.list.d/ubuntu.sources
RUN apt-get -o APT::Update::Error-Mode=any update \
    && apt-get install --yes --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*
COPY source/ /src/
WORKDIR /src
# Upstream's CLI Makefile target, optimized and statically linked without network access.
RUN --network=none make TARGET=cli CFLAGS="-O2 -ffile-prefix-map=/src=." LFLAGS=-static \
    && dpkg-query -W -f='${Package}=${Version}\n' g++ g++-13 libstdc++-13-dev libc6-dev binutils make > /toolchain.txt

FROM scratch
COPY --from=build /src/flips /flips
COPY --from=build /toolchain.txt /provenance/toolchain.txt
COPY source/COPYING source/COPYING.gpl3 /licenses/flips/
COPY source/libdivsufsort-2.0.1/COPYING /licenses/libdivsufsort/
ENTRYPOINT ["/flips"]
