# Private BPS candidates

Local tooling for checksum-specific BPS patches of the verified US Rev 0 and
Rev 1 bases. Bundles are private: every manifest has
`publication_permitted: false` and `distribution_blocked: true`. This does not
clear redistribution rights, release a patch, or cover Europe or other English bases.

Requires Python 3.12+ and Docker. `prepare` downloads the checksum-pinned Flips
v198 source (`flips.json`) and builds its upstream CLI target, statically linked
and without network access, into `game/tools/`. The retained source archive,
GPL/libdivsufsort notices and `provenance.json` (toolchain, binary and image
hashes) stay beside it. All Flips runs use that image with networking disabled,
`--exact`, and never `--ignore-checksum`.

## Generate

Commit first; generation refuses a dirty tree. Build each pristine base and its
modified target from that commit with the same connection profile, copying each
ROM out of `game/pokeplatinum*/build/` before the next build:

```sh
ROM_REVISION=1 ./scripts/build-rom.sh --pristine
WONDER_TRADE_PROFILE=game/profile.json ROM_REVISION=1 ./scripts/build-rom.sh
python3 tools/bps-patch/patcher.py create --version 0.1.0-rc1 \
  --profile game/profile.json --output game/bundles/0.1.0-rc1 \
  --pair game/roms/us-rev0.nds game/roms/us-rev0-wt.nds \
  --pair game/roms/us-rev1.nds game/roms/us-rev1-wt.nds
```

Each base must match a pristine-verified catalogue entry. Each target must keep
its base's game code and revision and contain both profile pins exactly once.
Every patch is created twice (bytes must match), walked, and round-tripped to the
target before the bundle directory is created. `manifest.json` is linked last;
a directory without it is incomplete and is never overwritten. `audit/` holds the
walker report and extracted TargetRead literals for private review.

## Apply

```sh
python3 tools/bps-patch/patcher.py apply \
  --manifest game/bundles/0.1.0-rc1/manifest.json \
  --input /path/to/platinum.nds --output /path/to/platinum-wonder-trade.nds
```

The input selects exactly one entry by size and SHA-256. The patch digest, the
walker's framing/action/range/CRC32 checks and the output CRC32/SHA-256 are all
verified in a private directory beside the output, then the output is published
as a new hard link. Existing outputs, symlinks, hard-linked inputs, unmatched or
modified inputs and malformed bundles are rejected; the input is never written.
The output directory must support hard links (not FAT/exFAT; copy afterwards).
`audit PATCH [--evidence game/DIR]` walks any patch without applying it.

Patching changes only the ROM. Keep each save with its `.platinumwt` recovery
directory and lock, and never restore an older copy. A new endpoint or profile
needs a newly generated patch. Emulator and physical DS behavior of the patched
ROM is validated separately; this tool proves only byte-exact target reproduction.

```sh
python3 -m unittest discover -s tools/bps-patch/__tests__
```

Set `PLATINUMWT_FLIPS_PROVENANCE=game/tools/flips-v198-*/provenance.json` to
also exercise the real pinned container.
