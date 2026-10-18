FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++ pkg-config python3 patch libssl-dev \
    qt6-base-dev qt6-multimedia-dev libqt6svg6-dev libqt6opengl6-dev \
    libsdl2-dev libarchive-dev libzstd-dev libfaad-dev libenet-dev libslirp-dev libegl-dev libx11-dev \
    && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends qt6-base-private-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY LICENSE LICENSE
COPY scripts/prepare-emulator.py scripts/prepare-tls.py scripts/
COPY scripts/run-emulator.py scripts/provision-save.py scripts/
COPY client/include/wonder_trade/persistence.h client/include/wonder_trade/persistence.h
COPY client/include/wonder_trade/journal.h client/include/wonder_trade/journal.h
COPY client/src/wonder_trade/journal.c client/src/wonder_trade/journal.c
COPY client/include/wonder_trade/wire.h client/include/wonder_trade/wire.h
COPY client/src/wonder_trade/wire.c client/src/wonder_trade/wire.c
COPY client/tests/journal_fixture.h client/tests/journal_fixture.h
COPY emulator/ emulator/
RUN cmake -S emulator -B /checks -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /checks --parallel 2 \
    && ctest --test-dir /checks --output-on-failure
RUN python3 scripts/prepare-emulator.py > /source-path \
    && cmake -S "$(cat /source-path)" -B /build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release -DENABLE_WAYLAND=OFF -DENABLE_LTO_RELEASE=OFF \
       -DENABLE_GDBSTUB=OFF -DUSE_SYSTEM_LIBSLIRP=ON \
    && cmake --build /build --parallel 2
