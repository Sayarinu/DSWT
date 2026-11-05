FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c AS certificates
# The minimal Ubuntu base has no CAs; bootstrap HTTPS from a checksum-pinned
# package in the same snapshot, without disabling TLS or APT signature checks.
ADD --checksum=sha256:8a0a754aaafed72cc8efb8c1731808133284a84fbfc76a4fcc73fce9e01b4187 https://snapshot.ubuntu.com/ubuntu/20261027T000000Z/pool/main/c/ca-certificates/ca-certificates_20260816ubuntu0.24.04.1_all.deb /tmp/ca.deb
RUN dpkg-deb --extract /tmp/ca.deb /tmp/ca \
    && cat /tmp/ca/usr/share/ca-certificates/mozilla/*.crt > /tmp/ca-certificates.crt

FROM ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c AS base
COPY --from=certificates /tmp/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY server/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources

FROM base AS build
RUN apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++ pkg-config libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev libboost-system-dev libgtest-dev python3 libpq-dev postgresql-16 libssl-dev openssl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 2 \
    && ctest --test-dir /build --output-on-failure

FROM base AS production
RUN apt-get -o APT::Update::Error-Mode=any update && apt-get install -y --no-install-recommends \
    ca-certificates libgrpc++1.51t64 libprotobuf32t64 libboost-system1.83.0 libpq5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /build/platinumwt-server /usr/local/bin/platinumwt-server
COPY --from=build /build/platinumwt-backend /usr/local/bin/platinumwt-backend
COPY --from=build /build/platinumwt-matcher /usr/local/bin/platinumwt-matcher
COPY --from=build /build/platinumwt-ledger /usr/local/bin/platinumwt-ledger
COPY server/migrations /usr/local/share/platinumwt/migrations
COPY LICENSE NOTICE /usr/share/platinumwt/
LABEL org.opencontainers.image.licenses="MIT"
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/platinumwt-server"]
