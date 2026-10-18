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

## Upstream source

The pinned upstream revision is recorded in [`client/UPSTREAM.md`](client/UPSTREAM.md).
Filtered project history preserves the original authors, dates and messages;
[`licensing/migration.json`](licensing/migration.json) records the source boundary
and commit mapping. The complete original repository remains private.
