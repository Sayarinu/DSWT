#!/usr/bin/env python3
"""Add Wonder Trade to a French, German, Italian or Spanish Platinum dump.

pret builds US English Platinum only. This compiles Wonder Trade for the
target language, links it against addresses found by matching the pristine
US Rev 1 build with the maintainer's own dump, and adds it to that dump. The
new ROM is written only if every check passes; see the wiki's Retail Ports page.

Inputs, all prepared with scripts/build/build-rom.sh:
  --pristine   a pristine US Rev 1 workspace (ROM_REVISION=1 build-rom.sh --pristine)
  --workspace  a Wonder Trade Platinum workspace built with the wanted connection profile
"""

import argparse
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from retail_port import platinum  # noqa: E402

ROOT = platinum.ROOT


def write_new(path, data):
    """Write a file that must not exist yet, through a temporary file in the same directory."""
    path = Path(path)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path)
    finally:
        os.unlink(temporary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("language", choices=sorted(platinum.LANGUAGES))
    parser.add_argument("rom", type=Path, help="the maintainer's own dump of that language's Platinum")
    parser.add_argument("output", type=Path, help="new ROM file to write")
    parser.add_argument("--workspace", type=Path, default=ROOT / "game/pokeplatinum")
    parser.add_argument("--pristine", type=Path, default=ROOT / "game/pokeplatinum-pristine")
    parser.add_argument("--work", type=Path, help="scratch directory (default game/retail-port/LANGUAGE)")
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.exit(1, f"{args.output} already exists\n")
    try:
        work = args.work or ROOT / "game/retail-port" / args.language
        target = platinum.Target(args.language, args.rom, args.workspace, args.pristine, work)
        rom = target.build()
        write_new(args.output, rom)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"Port failed; no ROM was written: {error}\n")
    print(f"Wrote {args.output} ({target.variant['game']} {target.variant['name']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
