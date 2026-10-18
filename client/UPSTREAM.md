# Upstream source

Project additions live here. The upstream source from
[`pret/pokeplatinum`](https://github.com/pret/pokeplatinum) belongs only in the
ignored local `game/pokeplatinum/` workspace.

- Upstream revision: `1dc13dcea40c04ab53f1f607de46023dc409ef20`
- Upstream build and development instructions: `INSTALL.md` and `CONTRIBUTING.md`
  in that local workspace.
- Upstream top-level license: none was present at the recorded revision.

For a fresh local workspace, run from the project root:

```sh
git init game/pokeplatinum
git -C game/pokeplatinum fetch --depth=1 https://github.com/pret/pokeplatinum.git 1dc13dcea40c04ab53f1f607de46023dc409ef20
git -C game/pokeplatinum checkout --detach FETCH_HEAD
git -C game/pokeplatinum apply --unidiff-zero ../../client/pokeplatinum.patch
cp -R client/include/. game/pokeplatinum/include/
cp -R client/src/. game/pokeplatinum/src/
cp -R client/res/. game/pokeplatinum/res/
```

Apply the patch once to a clean pinned checkout. It contains only the current
integration changes; original Wonder Trade files are copied separately.
Upstream notices stay in the local workspace. Its source, SDKs, toolchain and
ROM output are excluded from this repository and require their own rights review.
