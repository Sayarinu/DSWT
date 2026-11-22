#!/usr/bin/env python3
"""Add Wonder Trade to your own Pokémon Black or White (USA/Europe) dump.

aletheia rebuilds both cartridges byte for byte, so its link output is the
game's own symbol map. This compiles Wonder Trade and the aletheia sources
client/aletheia.patch changes, links them against those addresses and adds
them to the dump; see scripts/retail_port/bw.py. The ROM is written only if
every check passes. It runs in DS mode only: the DSi digests stay stale.

Inputs:
  --workspace  a built aletheia workspace (ROM_GAME=black|white build-rom.sh --pristine)
  --profile    the connection header from configure-connection.py
"""

import argparse
import os
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from retail_port import bw  # noqa: E402

ROOT = bw.ROOT


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
    parser.add_argument("game", choices=sorted(bw.TARGETS))
    parser.add_argument("rom", type=Path, help="your own dump of that game")
    parser.add_argument("output", type=Path, help="new ROM file to write")
    parser.add_argument("--workspace", type=Path, default=ROOT / "game/aletheia")
    parser.add_argument("--profile", type=Path, required=True, help="connection header (configure-connection.py)")
    parser.add_argument("--tls", type=Path, required=True, help="Mbed TLS source tree (prepare-tls.py)")
    parser.add_argument("--msl", type=Path, required=True, help="CodeWarrior MSL C headers: the directory holding MSL_C/")
    parser.add_argument("--work", type=Path, help="scratch directory (default game/retail-port/GAME)")
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.exit(1, f"{args.output} already exists\n")
    try:
        work = args.work or ROOT / "game/retail-port" / args.game
        target = bw.Target(args.game, args.rom, args.workspace, args.tls, args.profile, args.msl, work)
        rom = target.build()
        write_new(args.output, rom)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"Port failed; no ROM was written: {error}\n")
    print(f"Wrote {args.output} ({target.variant['game']} {target.variant['name']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
