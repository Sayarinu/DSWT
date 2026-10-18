# Upstream source

Project additions live here. The upstream source from
[`pret/pokeplatinum`](https://github.com/pret/pokeplatinum) belongs only in the
ignored local `game/pokeplatinum/` workspace.

- Upstream revision, tree and pristine US ROM checksums: [`upstream.json`](upstream.json).
- Upstream build and development instructions: `INSTALL.md` and `CONTRIBUTING.md`
  in that local workspace.
- Upstream top-level license: none was present at the recorded revision.

The build script prepares the workspace automatically. To prepare source alone:

```sh
python3 scripts/prepare-game.py
```

Preparation is repeatable and rejects incompatible source or local edits. Keep
an edited workspace aside before preparing changed project sources. Use
`--pristine` for a separate unmodified `game/pokeplatinum-pristine/` workspace.
Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
