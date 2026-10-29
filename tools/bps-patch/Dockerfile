FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
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
