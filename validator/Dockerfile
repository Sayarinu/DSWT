FROM mcr.microsoft.com/dotnet/sdk:10.0-noble@sha256:0d99ebf1ccad15037b5c98c9fe8979d6b8dbac23812feae2277a0b277b5c5ffa AS build
WORKDIR /source
COPY validator validator
COPY server/proto/trade.proto server/proto/trade.proto
COPY LICENSE NOTICE ./
RUN sh validator/fetch-core.sh
RUN dotnet restore validator/tests/Validator.Tests.csproj --locked-mode
RUN dotnet build validator/tests/Validator.Tests.csproj -c Release --no-restore \
    -p:SourceRevisionId=74b88906e935e4a52d6d9243b8e373056409c738
RUN dotnet run --project validator/tests -c Release --no-build --no-restore

FROM build AS validator-tests
ENTRYPOINT ["dotnet", "/source/validator/tests/bin/Release/net10.0/Validator.Tests.dll"]

FROM build AS publish
RUN dotnet publish validator/Validator.csproj -c Release --no-restore --no-build -o /app
# Complete adapter/core source and build controls, without build outputs,
# NuGet package caches, UI projects, icons, upstream test saves or game files.
RUN find /source -type d \( -name bin -o -name obj \) -prune -exec rm -rf '{}' +

FROM mcr.microsoft.com/dotnet/aspnet:10.0-noble@sha256:d5633778c754e89a386882eddf8dbee6942447a8a4b8267c19cf1e1158d0b0ac
WORKDIR /app
COPY --from=publish /app /app
COPY --from=publish /source /usr/share/dswt-validator/source
ENV DOTNET_EnableDiagnostics=0 \
    DOTNET_PROCESSOR_COUNT=2 \
    DOTNET_GCHeapHardLimit=8000000
USER 1654:1654
LABEL org.opencontainers.image.licenses="GPL-3.0-or-later"
ENTRYPOINT ["dotnet", "Validator.dll"]
