# Upstream source

Project additions live here. The upstream sources belong only in ignored local
workspaces:

| Game | Upstream | Pin | Workspace |
| --- | --- | --- | --- |
| Platinum | [`pret/pokeplatinum`](https://github.com/pret/pokeplatinum) | [`upstream.json`](upstream.json) | `game/pokeplatinum/` |
| HeartGold, SoulSilver | [`pret/pokeheartgold`](https://github.com/pret/pokeheartgold) | [`upstream-pokeheartgold.json`](upstream-pokeheartgold.json) | `game/pokeheartgold/` |

- Each pin records the upstream revision, tree and pristine US ROM checksums.
- Upstream build and development instructions: `INSTALL.md` and `CONTRIBUTING.md`
  in each local workspace.
- Upstream top-level license: none was present at either recorded revision.

The build script prepares the workspace automatically. To prepare source alone:

```sh
python3 scripts/prepare-game.py               # Platinum
python3 scripts/prepare-game.py --game hgss   # HeartGold and SoulSilver
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

Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
