# Project Goal

Pokemon Platinum Patch is a project to modify Pokémon Platinum so that it can
participate in Wonder Trade. The long-term goal is to pair the game patch with
a custom Wonder Trade server operated by this project.

Keep the pinned Pokémon Platinum decompilation local in ignored
`game/pokeplatinum/`. Project additions and integration changes live in `client/`.
Preserve upstream notices and record its revision when refreshing the workspace.

## Planned Platform

- The server implementation will be written in C++.
- Server protocols will use gRPC.
- The legality validator is an isolated pinned PKHeX.Core gRPC service in .NET,
  with its GPL source and notice obligations.
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

## Contribution Workflow

- Track each change with a ticket. Use only its identifier for the branch name,
  such as `ABC-123`.
- Format new commits as `{TicketID-#} - Message`, such as
  `ABC-123 - Fix trade recovery`. Preserve historical messages during migration.
- Keep PRs focused and self-contained: problem, changes, validation, material
  risks, and ticket identifier. Omit links to private trackers; use the PR
  template's automation opt-out to suppress private tracker comments.
- Start each implementation branch from the latest `main`. Review and merge
  one PR at a time after automated review, resolved findings, and passing
  PR checks. Honor explicitly requested approval checkpoints.
- Change documentation and `AGENTS.md` only when necessary or beneficial;
  keep those updates brief and avoid routine progress documentation.
