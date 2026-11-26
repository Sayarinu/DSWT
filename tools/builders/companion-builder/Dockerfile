# BlocksDS v1.25.0 (slim: toolchain, libnds, dswifi, maxmod, ndstool),
# pinned by its multi-architecture index digest.
FROM skylyrac/blocksds:slim-v1.25.0@sha256:c5f19b95d9c2f312e6a68ea244d2ed5120203b4b82af9785937a349096f8e5e4

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /dswt/companion
