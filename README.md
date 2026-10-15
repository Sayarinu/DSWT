# Platinum Wonder Trade

This project aims to patch Pokémon Platinum with Wonder Trade support and run a
custom Wonder Trade server.

## Build the game ROM

The Pokémon Platinum decompilation is in [`game/pokeplatinum`](game/pokeplatinum).
Its setup and platform-specific dependencies are documented in
[`game/pokeplatinum/INSTALL.md`](game/pokeplatinum/INSTALL.md).

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

The decompilation source snapshot and its provenance are documented in
[`game/pokeplatinum/UPSTREAM.md`](game/pokeplatinum/UPSTREAM.md).
