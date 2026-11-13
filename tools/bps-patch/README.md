# BPS release bundles

Builds and checks the release bundle: one checksum-specific BPS patch per
supported retail base, plus a `manifest.json`. `bundle.py` and `bps.py` are the
portable, standard-library-only applier shared with the player app. Generation
needs Python 3.12+ and Docker; applying needs only Python.

`prepare` downloads the checksum-pinned Flips v198 source (`flips.json`) and
builds its upstream CLI target, statically linked and without network access,
into `game/tools/`. The retained source archive, GPL/libdivsufsort notices and
`provenance.json` (toolchain, binary and image hashes) stay beside it; the built
binary must match its per-platform pin (only linux/amd64 is pinned), and `create`
re-verifies all of these before use. All Flips runs use that image with
networking disabled, `--exact`, and never `--ignore-checksum`.

## Generate

Commit first; generation refuses a dirty tree. Build each pristine base and its
modified target from that commit with the production profile, copying each ROM
out of `game/pokeplatinum*/build/` before the next build (repeat for revision 0,
and for revision 10 with `DSWT_EUROPE_ROM` naming your European dump; for
Diamond and Pearl, set `ROM_GAME` and copy from `game/pokediamond*/`):

```sh
ROM_REVISION=1 ./scripts/build-rom.sh --pristine
WONDER_TRADE_PROFILE=game/dswt-production/tls/connection.json ROM_REVISION=1 ./scripts/build-rom.sh
python3 tools/bps-patch/patcher.py create --version 0.1.0 \
  --profile game/dswt-production/tls/connection.json --output game/bundles/0.1.0 \
  --pair game/roms/us-rev0.nds game/roms/us-rev0-wt.nds \
  --pair game/roms/us-rev1.nds game/roms/us-rev1-wt.nds \
  --pair game/roms/europe.nds game/roms/europe-wt.nds
python3 tools/bps-patch/patcher.py package game/bundles/0.1.0 --output game/bundles/dswt-0.1.0.zip
```

Each base must match a pristine-verified catalogue entry with a planned patch
([`patches/compatibility.json`](../../patches/compatibility.json)). Each target must
keep its base's game code and revision and contain both profile pins exactly once.
Every patch is created twice (bytes must match), walked, and reproduced by both
Flips and `bps.py`. The bundle is staged and renamed into place without replacing
anything; `audit/` keeps the walker report and literals for review and is never
packaged.

Generation fails, publishing nothing, if any patch or target contains data that
does not belong in a release:

- a local or test endpoint: the profile hostname must not use a reserved name
  such as `.test` or `localhost`, and its IPv4 address must be global;
- private key material: each `*.key` beside the profile (or each `--forbid-key`
  file), as PEM text, DER or 32-byte key scalars;
- PEM or private key markers, the repository path, the home directory or another
  build-host path absent from the retail base;
- retail ROM content: any patch literal holding a run of 47 or more bytes copied
  verbatim from any supplied base. Unchanged content must come from the
  player's own ROM, so patches carry only rebuilt code and project data.

`package` writes the reproducible player archive: manifest, patches,
[`INSTALL.md`](../../patches/INSTALL.md), `LICENSE` and `NOTICE`, after
re-checking every patch against the manifest.

## Apply

```sh
python3 tools/bps-patch/patcher.py apply --manifest game/bundles/0.1.0/manifest.json \
  --input /path/to/platinum.nds --output /path/to/dswt.nds
```

The input selects exactly one entry by SHA-256. A trimmed dump is restored to
full size with its erased `0xFF` padding first, and must then match exactly. An
unsupported version, an already patched ROM and a modified or encrypted dump
each get their own message. The patch digest, every BPS action and CRC32, and the
output CRC32 and SHA-256 are verified before anything is written. The output is
flushed to a temporary file and linked to a new name, so an existing path is
never replaced; the input is never written. `audit PATCH [--evidence game/DIR]`
walks any patch and can extract its literals and metadata privately.

Patching changes only the ROM. Keep each save with its `.dswt` recovery
directory and lock, and never restore an older copy. A new endpoint or profile
needs a newly generated patch.

```sh
python3 -m unittest discover -s tools/bps-patch/__tests__
```

Set `DSWT_FLIPS_PROVENANCE=game/tools/flips-v198-*/provenance.json` to
also exercise the real pinned container.
