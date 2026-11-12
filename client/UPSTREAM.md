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
resources (`res/` for Platinum, `files/` for HeartGold and SoulSilver), never
another game's. One HeartGold/SoulSilver workspace builds both versions into
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

Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
