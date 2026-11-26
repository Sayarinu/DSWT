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
  Preserve upstream notices, and record the new revision in `client/games/UPSTREAM.md`
  and the matching `client/games/upstream*.json` when refreshing a workspace.

## Layout

| Path | Contents |
| --- | --- |
| `client/core/` | Shared DS client code, save layouts and native tests |
| `client/games/` | Game adapters, integration patches, resources and upstream pins |
| `client/companion/` | DS Companion: a DSi-mode BlocksDS homebrew ROM that trades from a game's save on the SD card, built with `scripts/build/build-companion.sh` on the client core, with host tests in `client/companion/tests/` |
| `server/` | C++ trade service: HTTPS gateway for the DS, gRPC backend, matcher, ledger |
| `validator/` | Isolated PKHeX.Core legality service in .NET (GPL; see its `LICENSE` and `NOTICE`) |
| `protocol/` | Wonder Trade v1 wire contract shared by the DS and server |
| `deploy/` | Docker Compose, Terraform and monitoring for the single VPS |
| `scripts/` | Single-purpose entry points grouped under build/, prepare/ and port/ |
| `tools/` | Local stack, BPS patcher and release tooling, image audit, ROM and companion builder images |
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
# Validate source inputs, client build and port tools, service tooling and CI policy, and patch and release tools
for suite in scripts/build/tests scripts/prepare/tests scripts/port/tests client/core/tests/contracts client/games/tests deploy/__tests__ validator/__tests__; do
  python3 -m unittest discover -s "$suite"
done
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
tls_source="$(python3 scripts/prepare/prepare-tls.py)"
cmake -S client/core/tests -B game/tls-tests -DMBEDTLS_SOURCE="$tls_source"
cmake --build game/tls-tests --parallel 2
ctest --test-dir game/tls-tests --output-on-failure
python3 client/core/tests/test_tls.py game/tls-tests/tls_probe

# Build and test DS companion
cmake -S client/companion/tests -B game/companion-tests
cmake --build game/companion-tests --parallel 2
ctest --test-dir game/companion-tests --output-on-failure
CONNECTION_PROFILE=client/companion/connection-profile.json scripts/build/build-companion.sh
docker run --rm --network none -v "$PWD/game/companion:/out:ro" dswt-companion-builder sh -c '"$BLOCKSDS/tools/ndstool/ndstool" -i /out/dswt-companion.nds'
python3 scripts/build/package-companion.py --check

# Build and test isolated legality validator
docker build -f validator/Dockerfile --target validator-tests -t dswt-validator-tests .
docker run --rm --user 1654:1654 --memory 512m --cpus 2 --pids-limit 128 \
  --network none --read-only --tmpfs /tmp:rw,noexec,nosuid,nodev,size=64m \
  dswt-validator-tests
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
  template with a ticket link, a bullet list explaining what changed and why,
  and any material risks.
- PRs merge into `main` with a merge commit once the required checks pass and
  review conversations are resolved. Squash and rebase merges are disabled.
- Group reusable entry points by job under `scripts/`, with tests beside each group.
  Put self-contained programs and builder images under `tools/`. Keep one-off test scripts in the gitignored
  `testing-scripts/` folder, and don't commit one-off or development scripts.
- Documentation lives in the [wiki](https://github.com/Sayarinu/DSWT/wiki), written in plain terms. Keep
  `README.md` short, like a store page, and update the wiki page a change
  affects. Track only docs something depends on: this file, `README.md`,
  `tools/patcher-app/INSTALL.md` (shipped in releases), `client/games/UPSTREAM.md`,
  `validator/README.md` (in the validator image's GPL source) and license
  notices. Change docs only when necessary or beneficial, and skip routine
  progress notes.
- The wiki explains how to build, develop and install, not how a problem was
  found. Write investigation findings to the issue tracker instead: root
  causes, debugging steps that worked, and what to check next time. Put them in
  a comment on the ticket, or in a project document when they apply beyond one
  ticket, such as notes for every retail port.
