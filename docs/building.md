# Building from source

This covers the patched ROM. To run the same checks as CI, see
[Checks in AGENTS.md](../AGENTS.md#checks).

## Game ROM

Project additions and the integration patch are in [`client`](../client). The
upstream game source and build inputs stay in an ignored local workspace.
The build script prepares pinned source automatically; see
[`client/UPSTREAM.md`](../client/UPSTREAM.md) for provenance and local-edit handling.

The simplest build uses Docker and does not install the cross-compiler on your
host. Local preparation requires Python 3.12 or newer; set `PYTHON=python3.12`
when needed to select a separately installed interpreter. Docker uses
`linux/amd64` for the ROM toolchain, including on Apple Silicon.
First create a local public endpoint/pins profile using the
[secure connection test guide](../client/tests/README.md), then pass its path:

```sh
WONDER_TRADE_PROFILE=game/device-test/connection.json ./scripts/build-rom.sh
```

The output is `game/pokeplatinum/build/pokeplatinum.us.nds`. To select North
American revision 0, run `ROM_REVISION=0 ./scripts/build-rom.sh`. The image
definition is in `tools/rom-builder/Dockerfile`. This project script builds the
modified ROM. To reproduce and check the unmodified US baseline, run
`./scripts/build-rom.sh --pristine` (also accepts `ROM_REVISION=0`). This uses
`game/pokeplatinum-pristine/` and runs all upstream pristine checksum checks.
European English Platinum is `ROM_REVISION=10`: it runs Rev 1 code, and its
header area and the few files that differ come from your own dump, named by
`DSWT_EUROPE_ROM`. The build checks the dump's SHA-256 first and stages those
parts only in the ignored workspace (`.git/dswt-europe/`); `--pristine` then
reproduces the dump exactly.
Both build modes default to one compiler job; set `ROM_BUILD_JOBS=2` (a positive
integer without leading zeros) to opt into parallel compilation. Serial builds
avoid the compiler failures observed with parallel macOS Docker builds.
HeartGold, SoulSilver, Diamond and Pearl select their workspace with `ROM_GAME`
([`client/UPSTREAM.md`](../client/UPSTREAM.md)); Diamond and Pearl use the
Wine-based image in `tools/rom-builder-dp/Dockerfile` and, like the other games,
need `WONDER_TRADE_PROFILE` unless built with `--pristine`.
ROMs and downloaded inputs stay local; CI publishes neither ROMs nor game caches.

## Upstream source

The pinned upstream revision is recorded in [`client/UPSTREAM.md`](../client/UPSTREAM.md).
Filtered project history preserves the original authors, dates and messages.
