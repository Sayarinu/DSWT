#!/usr/bin/env python3
"""Add Wonder Trade to your own retail Diamond or Pearl in a language pret cannot build.

Build the pristine US game (ROM_GAME=diamond ./scripts/build-rom.sh --pristine) and the
Wonder Trade workspace against your connection profile first. The ROM is written only if
every check passes; see scripts/retail_port/dp.py.
"""

import argparse
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from retail_port import dp  # noqa: E402

_spec = importlib.util.spec_from_file_location("port_platinum", ROOT / "scripts/port-platinum.py")
port_platinum = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(port_platinum)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", choices=sorted(dp.GAMES))
    parser.add_argument("language", choices=sorted(dp.LANGUAGES))
    parser.add_argument("rom", type=Path, help="the maintainer's own dump of that game and language")
    parser.add_argument("output", type=Path, help="new ROM file to write")
    parser.add_argument("--workspace", type=Path, default=ROOT / "game/pokediamond")
    parser.add_argument("--pristine", type=Path, default=ROOT / "game/pokediamond-pristine")
    parser.add_argument("--work", type=Path, help="scratch directory (default game/retail-port/GAME-LANGUAGE)")
    args = parser.parse_args(argv)
    if args.output.exists():
        parser.exit(1, f"{args.output} already exists\n")
    try:
        work = args.work or ROOT / "game/retail-port" / f"{args.game}-{args.language}"
        target = dp.Target(args.game, args.language, args.rom, args.workspace, args.pristine, work)
        rom = target.build()
        port_platinum.write_new(args.output, rom)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"Port failed; no ROM was written: {error}\n")
    print(f"Wrote {args.output} ({target.variant['game']} {target.variant['name']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
