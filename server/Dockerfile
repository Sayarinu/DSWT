FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ pkg-config libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev libboost-system-dev libgtest-dev python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build \
    && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51t64 libprotobuf32t64 libboost-system1.83.0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/platinumwt-server /usr/local/bin/platinumwt-server
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/platinumwt-server"]
