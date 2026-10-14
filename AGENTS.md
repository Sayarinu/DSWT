# Project Goal

Pokemon Platinum Patch is a project to modify Pokémon Platinum so that it can
participate in Wonder Trade. The long-term goal is to pair the game patch with
a custom Wonder Trade server operated by this project.

The Pokémon Platinum decompilation is maintained in `game/pokeplatinum/` as
ordinary project files, not as a Git fork or submodule. Keep upstream build
files and notices intact; record the upstream revision when refreshing it.

## Planned Platform

- The server implementation will be written in C++.
- Server protocols will use gRPC.
- Docker will package services, and Kubernetes (k3s) will orchestrate them.
- The deployment target will be a VPS with AWS resources managed through
  Terraform.
- GitHub Actions will support CI/CD, Argo CD will manage deployment from Git,
  and GHCR will store container images.
- Prometheus and Grafana will provide monitoring and dashboards.
- Dependabot will help keep dependencies and actions up to date.

## Contribution Direction

Keep changes aligned with the game patch and the custom Wonder Trade service.
Favor clear, maintainable C++ and gRPC interfaces for server code, and keep
deployment and infrastructure changes reproducible through the stated tooling.

Treat the listed platform components as the intended stack; avoid introducing
additional infrastructure or protocols without a project need.

The DS game client and server are separate protocol endpoints. Do not assume
the game itself can use gRPC; keep its wire protocol compatible with the
Nintendo DS networking constraints and use gRPC for server-side services.
