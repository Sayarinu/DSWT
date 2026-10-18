# Platinum Wonder Trade

This project aims to patch Pokémon Platinum with Wonder Trade support and run a
custom Wonder Trade server.

## Build the game ROM

Project additions and the integration patch are in [`client`](client). The
upstream game source and build inputs stay in an ignored local workspace.
Prepare that workspace using [`client/UPSTREAM.md`](client/UPSTREAM.md).

The simplest build uses Docker and does not install the cross-compiler on your
host:

```sh
./scripts/build-rom.sh
```

The output is `game/pokeplatinum/build/pokeplatinum.us.nds`. To select North
American revision 0, run `ROM_REVISION=0 ./scripts/build-rom.sh`. The image
definition is in `tools/rom-builder/Dockerfile`. This project script builds the
ROM without running the upstream pristine-ROM and filesystem checksum checks;
those checks are available through the upstream `make check` target. The
upstream build guide also documents native Linux and other platform setups.

## Upstream source

The pinned upstream revision is recorded in [`client/UPSTREAM.md`](client/UPSTREAM.md).
Filtered project history preserves the original authors, dates and messages;
[`licensing/migration.json`](licensing/migration.json) records the source boundary
and commit mapping. The complete original repository remains private.
