# Upstream source

Game adapters, resources and integration patches live in `client/games/`.
The preparation script combines them with `client/core/` in each ignored
upstream workspace.

| Game | Upstream | Pin | Workspace |
| --- | --- | --- | --- |
| Platinum | [`pret/pokeplatinum`](https://github.com/pret/pokeplatinum) | [`upstream.json`](upstream.json) | `game/pokeplatinum/` |
| HeartGold, SoulSilver | [`pret/pokeheartgold`](https://github.com/pret/pokeheartgold) | [`upstream-pokeheartgold.json`](upstream-pokeheartgold.json) | `game/pokeheartgold/` |
| Diamond, Pearl | [`pret/pokediamond`](https://github.com/pret/pokediamond) | [`upstream-pokediamond.json`](upstream-pokediamond.json) | `game/pokediamond/` |
| Black 2, White 2 | [`fuddlesworth/pokebw2`](https://github.com/fuddlesworth/pokebw2) | [`upstream-pokebw2.json`](upstream-pokebw2.json) | `game/pokebw2-pristine/` |
| Black, White | [`Project-Holon/aletheia`](https://github.com/Project-Holon/aletheia) | [`upstream-aletheia.json`](upstream-aletheia.json) | `game/aletheia-pristine/` |

- Each pin records the upstream revision, tree and pristine US ROM checksums.
- Platinum revision `10` is European English, which upstream does not build:
  [`europe.patch`](europe.patch) adds that revision to every Platinum workspace
  without changing US output, and `scripts/prepare-europe.py` stages its
  differing inputs from a verified local dump.
- Upstream build and development instructions: `INSTALL.md` and `CONTRIBUTING.md`
  in each local workspace.
- Upstream top-level license: none was present at the pret revisions.
  pokebw2 is GPL-3.0 (its `LICENSE`); see `licensing/redistribution.json`
  for what that requires of anything built from it.

The build script prepares the workspace automatically. To prepare source alone:

```sh
python3 scripts/prepare-game.py               # Platinum
python3 scripts/prepare-game.py --game hgss   # HeartGold and SoulSilver
python3 scripts/prepare-game.py --game dp     # Diamond and Pearl
python3 scripts/prepare-game.py --game bw2 --pristine  # Black 2 and White 2
python3 scripts/prepare-game.py --game bw --pristine   # Black and White
```

Preparation is repeatable and rejects incompatible source or local edits. Keep
an edited workspace aside before preparing changed project sources. Use
`--pristine` for a separate unmodified `game/<workspace>-pristine/` workspace.
Each workspace receives the shared core, its own adapter directory and its own
resources (`res/` for Platinum, `files/` for HeartGold and SoulSilver,
`files-dp/` installed as `files/` for Diamond and Pearl), never another game's. One HeartGold/SoulSilver workspace builds both versions into
`build/heartgold.us/` and `build/soulsilver.us/`:

```sh
ROM_GAME=heartgold ./scripts/build-rom.sh --pristine
ROM_GAME=soulsilver ./scripts/build-rom.sh --pristine
```

The Diamond/Pearl workspace builds `build/diamond.us/` and `build/pearl.us/`
with upstream's Make build (`ROM_GAME=diamond` or `ROM_GAME=pearl`). Its ARM9
sources live under `arm9/`, so the shared core and `dp` adapter land in
`arm9/src/`. Upstream requires Metrowerks compilers and Nitro SDK 3.2 tools it
does not ship in source; `tools/rom-builder-dp/Dockerfile` fetches the archives
upstream's own CI uses, by checksum from a fixed commit of its `assets/workflow`
branch, and runs them under Wine. They are copied into the workspace's ignored
`tools/` and `arm*/` paths and stay local.

pokediamond's partial C library lacks the standard headers the shared core and
Mbed TLS need, so the integration patch builds them with CodeWarrior's MSL C
headers for the same compiler, which the image takes by checksum from the
Metroskrew release pokeplatinum pins. Its ARM9 Makefile builds the client
sources and Mbed TLS (as `libwonder_trade_tls.a`), and `arm9.lsf` links them
into the static ARM9 binary. The Wi-Fi Connection library exists only as
assembly in overlay 4, so `include/wonder_trade/dp/sdk/` maps the few calls the
core makes to its labels; each mapping was read from that assembly.

The start menu is undecompiled assembly (`arm9/asm/unk_02035068.s`). The
patch adds Wonder Trade to it as entry 9, after Pokémon, as in Platinum: its
label is row 12 of the menu's text bank, its action launches the screen the
way Options does, and it shows only where Platinum's does. Eight entries need
Platinum's layout, a wider window with rows 20 pixels apart, so the menu's task
data grows by its sprite count and row spacing. The icon is a project drawing,
`files-dp/graphic/menu_gra/narc_0011.png`, built into a new member of the menu
graphics archive and animated with the first icon's cell and animations.
Pressing Select while the menu is open still starts Wonder Trade as a
fallback, refused wherever the entry is hidden.

The Black 2/White 2 pin is the newest pokebw2 revision whose own CI rebuilt both
ROMs; the two commits after it did not build when it was pinned. pokebw2 has no Wonder Trade integration
yet, so only its pristine workspace exists, and it builds one USA/Europe ROM
per version from the player's own dump, which `build-rom.sh` copies into the
ignored `orig/`:

```sh
ROM_GAME=black2 DSWT_BW2_ROM=/path/to/black2.nds ./scripts/build-rom.sh --pristine
ROM_GAME=white2 DSWT_BW2_ROM=/path/to/white2.nds ./scripts/build-rom.sh --pristine
```

Its `configure.py` and Ninja extract and delink the dump, compile with
Metrowerks compilers run through wibo, rebuild the ROM to
`build/pokeblack2_us.nds` or `build/pokewhite2_us.nds` and check its SHA-1.
`tools/rom-builder-bw2/Dockerfile` fetches the tools `configure.py` would
download, by checksum, and the build runs it with `--no-download`. wibo needs
32-bit code segments, which Docker's Rosetta and QEMU emulation lack, so on an
Apple silicon Mac build natively instead: `python3 configure.py` and `ninja`
in the workspace, as pokebw2's `docs/building.md` describes.

The Black/White pin is aletheia's newest revision; its own CI only reports
progress, so the pin was checked by rebuilding both ROMs. aletheia's tools are
MIT, while its reverse-engineered `src/` carries no licence grant (its
`LEGAL.md`); the owner accepted building on it. It needs the CodeWarrior
compilers its README lists (`tools/mwccarm/mwccarm/`, with `1.2` the 1.2 base
release) and Wine, and builds each cartridge from the player's dump:

```sh
ROM_GAME=black DSWT_BW_ROM=/path/to/black.nds ./scripts/build-rom.sh --pristine
ROM_GAME=white DSWT_BW_ROM=/path/to/white.nds ./scripts/build-rom.sh --pristine
```

The script extracts the dump, disassembles it for the data links and relinks
every region with `tools/romlink.py`, which writes `build/rom/aletheia.nds`
(Black) or `build/rom/white.nds` (White) and checks its SHA-1. White keeps its
extract in `extract_white/`. `client/games/aletheia-build.patch` runs the linker and
the generated data includes through Wine off Windows, and leaves out the
DSi-only data when no DSi key is given, so the ROM keeps the dump's DSi block.
A full relink takes one to two hours.

Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
