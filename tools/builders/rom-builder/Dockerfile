FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        bison \
        flex \
        g++ \
        gcc-arm-none-eabi \
        git \
        lib32z1 \
        libpng-dev \
        make \
        nasm \
        ninja-build \
        p7zip-full \
        pkg-config \
        python3 \
        wget \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
