# Platinum Wonder Trade

This project aims to patch Pokémon Platinum with Wonder Trade support and run a
custom Wonder Trade server.

The [ROM compatibility and patch contract](patches/README.md) records verified
inputs and pending targets. No Wonder Trade patch has been released yet.

## Build the game ROM

Project additions and the integration patch are in [`client`](client). The
upstream game source and build inputs stay in an ignored local workspace.
The build script prepares pinned source automatically; see
[`client/UPSTREAM.md`](client/UPSTREAM.md) for provenance and local-edit handling.

The simplest build uses Docker and does not install the cross-compiler on your
host. First create a local public endpoint/pins profile using the
[secure connection test guide](client/tests/README.md), then pass its path:

```sh
WONDER_TRADE_PROFILE=game/device-test/connection.json ./scripts/build-rom.sh
```

The output is `game/pokeplatinum/build/pokeplatinum.us.nds`. To select North
American revision 0, run `ROM_REVISION=0 ./scripts/build-rom.sh`. The image
definition is in `tools/rom-builder/Dockerfile`. This project script builds the
modified ROM. To reproduce and check the unmodified US baseline, run
`./scripts/build-rom.sh --pristine` (also accepts `ROM_REVISION=0`). This uses
`game/pokeplatinum-pristine/` and runs all upstream pristine checksum checks.
ROMs and downloaded inputs stay local; CI publishes neither ROMs nor game caches.

## Server ledger

`docker build -t platinumwt-server server` builds the C++ server and runs its
tests, including a disposable PostgreSQL 16 cluster with synthetic data.
Final trading remains gated on off-site recovery. The image also contains an operator tool:

```sh
platinumwt-ledger migrate /usr/local/share/platinumwt/migrations
platinumwt-ledger verify
```

Configure its database with libpq's `PGSERVICE`/`PGPASSFILE` settings or the
`PLATINUMWT_DATABASE_URL` environment variable; keep credentials outside Git.
Run migrations as the database owner. They are transactional, ordered and
checksum-verified. Replay uses a consistent snapshot and temporary row images;
it checks live state without repairing it. Audit evidence includes private
verifiers and payloads and stays in the database/backups; the tool prints counts
and SQLSTATE errors. Local WAL durability does not satisfy the separate off-site
recovery barrier required before final trade responses are enabled.

`platinumwt-backend` serves enrollment/trading on loopback with
`PLATINUMWT_DATABASE_URL` and the pinned `WT_LEGALITY_POLICY`; the validator
defaults to `127.0.0.1:50052`. Its recovery provider currently stays closed.
For the HTTPS gateway, set `WT_TRADE_TARGET=127.0.0.1:50053` and provide
`WT_TLS_CERTIFICATE` / `WT_TLS_PRIVATE_KEY` file paths outside Git. Without TLS,
`POST /wt/v1` is refused. Keep every internal service in the same private network
namespace; only HTTPS may be exposed. The gateway bounds connections to 1,024,
pending calls to 64 and request workers to eight; capacity still needs its soak test.

## Upstream source

The pinned upstream revision is recorded in [`client/UPSTREAM.md`](client/UPSTREAM.md).
Filtered project history preserves the original authors, dates and messages;
[`licensing/migration.json`](licensing/migration.json) records the source boundary
and commit mapping. The complete original repository remains private.
