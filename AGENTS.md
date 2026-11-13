# Contributor guide

This guide is for people and coding agents working on DSWT. Read it before
changing code. [`README.md`](README.md) covers what the project does for players,
and the [wiki](https://github.com/Sayarinu/DSWT/wiki) explains how to install, build, test, release and deploy it.

## Project

- DSWT modifies Pokémon Diamond, Pearl, Platinum, HeartGold and SoulSilver so
  they can take part in Wonder Trade, paired with a custom Wonder Trade server.
- The pinned [pret](https://github.com/pret) decompilations stay local in the
  ignored `game/` directory. Never commit
  anything under `game/`, or any ROM, BIOS, firmware, save, key or `.env` file.
- Project additions and integration changes to the game live in `client/`.
  Preserve upstream notices, and record the new revision in `client/UPSTREAM.md`
  and the matching `client/upstream*.json` when refreshing a workspace.

## Layout

| Path | Contents |
| --- | --- |
| `client/` | Game-independent client core, the Platinum, HeartGold/SoulSilver and Diamond/Pearl adapters (`platinum/`, `hgss/` and `dp/`, behind `game_adapter.h`), their integration patches and native client tests |
| `server/` | C++ trade service: HTTPS gateway for the DS, gRPC backend, matcher, ledger |
| `validator/` | Isolated PKHeX.Core legality service in .NET (GPL; see its `LICENSE` and `NOTICE`) |
| `protocol/` | Wonder Trade v1 contract, DS wire envelope oracle and save layout |
| `emulator/` | Optional durable-persistence patch for melonDS (GPL) |
| `deploy/` | Docker Compose, Terraform and monitoring for the single VPS |
| `patches/` | Supported cartridges and the install guide shipped in each release |
| `scripts/` | Reused scripts: ROM build, workspace preparation and TLS tooling |
| `tools/` | Local stack, BPS patcher and release tooling, image audit |
| `licensing/` | Redistribution inventory and bundled third-party licenses |

## Platform

- The server is C++. Server-side protocols use gRPC.
- The legality validator is an isolated, pinned PKHeX.Core gRPC service in .NET.
  Keep its GPL source and notice obligations intact.
- Docker Compose runs every service on one Hetzner VPS.
- GitHub Actions runs CI; Dependabot keeps dependencies and actions current.
- Prometheus and Grafana provide monitoring and dashboards.
- Treat this as the intended stack. Do not add infrastructure, services or
  protocols without a project need.

## Direction

- Keep changes aligned with the game patch and the Wonder Trade service.
- Favor clear, maintainable C++ and gRPC interfaces for server code.
- Keep deployment and infrastructure reproducible through the stated tooling.
- The DS client and the server are separate protocol endpoints. The game cannot
  use gRPC: keep its wire protocol compatible with Nintendo DS networking
  constraints, and use gRPC only between server-side services.

## Checks

CI runs these on every pull request; run the ones your change touches before
opening it. Commands run from the repository root with Python 3.12 or newer and
Docker. Workflows are in `.github/workflows/`.

```sh
# Validate source inputs
python3 -m unittest discover -s scripts/tests
python3 -m venv game/policy-venv
game/policy-venv/bin/pip install --require-hashes -r tools/local-stack/requirements.txt
game/policy-venv/bin/python tools/local-stack/isolation.py policy
game/policy-venv/bin/python -m unittest discover -s tools/local-stack/__tests__
python3 -m unittest discover -s tools/image-audit/__tests__
python3 tools/bps-patch/patcher.py prepare
DSWT_FLIPS_PROVENANCE="$(ls "$PWD"/game/tools/flips-v198-*/provenance.json)" \
  python3 -m unittest discover -s tools/bps-patch/__tests__
python3 -m unittest discover -s tools/patcher-app/__tests__

# Build and test connectivity server
docker build -f server/Dockerfile -t dswt-server .

# Test bounded TLS transport (client host/native tests)
tls_source="$(python3 scripts/prepare-tls.py)"
cmake -S client/tests -B game/tls-tests -DMBEDTLS_SOURCE="$tls_source"
cmake --build game/tls-tests --parallel 2
ctest --test-dir game/tls-tests --output-on-failure
python3 client/tests/test_tls.py game/tls-tests/tls_probe

# Build and test isolated legality validator
docker build -f validator/Dockerfile --target validator-tests -t dswt-validator-tests .
docker run --rm --user 1654:1654 --memory 512m --cpus 2 --pids-limit 128 \
  --network none --read-only --tmpfs /tmp:rw,noexec,nosuid,nodev,size=64m \
  dswt-validator-tests

# Test durable emulator persistence
docker build -f emulator/Dockerfile -t dswt-emulator-builder .
```

CI also scans the full history with gitleaks and runs `git diff --check` on the
pull request. Pushes to `main` that change the server, validator, `protocol/` or
deployment files publish images and deploy them to the VPS over Tailscale
(`.github/workflows/deploy.yml`, see the [Deployment](https://github.com/Sayarinu/DSWT/wiki/Deployment) page), so
only merge what is ready to serve players. The required check names are listed in
`.github/repository-settings.json`; renaming a job means updating that file.

## Workflow

- Every change needs a ticket in your issue tracker. Its key, such as `ABC-123`,
  is the ticket ID.
- Start each branch from the latest `main`. Name it exactly the ticket ID, with
  no prefix or suffix.
- Commit subjects are `<ticket> - <message>`, such as `ABC-123 - Fix trade recovery`.
  The message describes the individual change in the imperative.
- Commit with your GitHub noreply email address, never a personal one.
- Do not add AI or tool attribution anywhere: no `Co-Authored-By` trailers,
  "Generated with" lines or tool mentions in commits, PRs, code, comments or docs.
- One ticket per PR. Keep PRs focused and self-contained, and fill in the PR
  template: problem, changes, validation, material risks and ticket ID. Do not
  link to the tracker; the ID is enough.
- PRs merge into `main` with a merge commit once the required checks pass and
  review conversations are resolved. Squash and rebase merges are disabled.
- `scripts/` holds only scripts that are reused, with their tests in
  `scripts/tests/`. Put scripts used only for testing in the gitignored
  `testing-scripts/` folder, and don't commit one-off or development scripts.
- Documentation lives in the [wiki](https://github.com/Sayarinu/DSWT/wiki), written in plain terms. Keep
  `README.md` short, like a store page, and update the wiki page a change
  affects. Track only docs something depends on: this file, `README.md`,
  `patches/INSTALL.md` (shipped in releases), `client/UPSTREAM.md`,
  `validator/README.md` (in the validator image's GPL source) and license
  notices. Change docs only when necessary or beneficial, and skip routine
  progress notes.
