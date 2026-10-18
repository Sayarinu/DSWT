FROM ubuntu:24.04

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
