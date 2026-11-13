# Upstream source

Project additions live here. The upstream sources belong only in ignored local
workspaces:

| Game | Upstream | Pin | Workspace |
| --- | --- | --- | --- |
| Platinum | [`pret/pokeplatinum`](https://github.com/pret/pokeplatinum) | [`upstream.json`](upstream.json) | `game/pokeplatinum/` |
| HeartGold, SoulSilver | [`pret/pokeheartgold`](https://github.com/pret/pokeheartgold) | [`upstream-pokeheartgold.json`](upstream-pokeheartgold.json) | `game/pokeheartgold/` |
| Diamond, Pearl | [`pret/pokediamond`](https://github.com/pret/pokediamond) | [`upstream-pokediamond.json`](upstream-pokediamond.json) | `game/pokediamond/` |

- Each pin records the upstream revision, tree and pristine US ROM checksums.
- Platinum revision `10` is European English, which upstream does not build:
  [`europe.patch`](europe.patch) adds that revision to every Platinum workspace
  without changing US output, and `scripts/prepare-europe.py` stages its
  differing inputs from a verified local dump.
- Upstream build and development instructions: `INSTALL.md` and `CONTRIBUTING.md`
  in each local workspace.
- Upstream top-level license: none was present at either recorded revision.

The build script prepares the workspace automatically. To prepare source alone:

```sh
python3 scripts/prepare-game.py               # Platinum
python3 scripts/prepare-game.py --game hgss   # HeartGold and SoulSilver
python3 scripts/prepare-game.py --game dp     # Diamond and Pearl
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

Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
