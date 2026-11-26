#!/usr/bin/env python3
"""Check the built DS/DSi homebrew and zip only its reviewed release files."""

import argparse
from pathlib import Path
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[2]
LICENSES = (
    "LICENSES/Apache-2.0.txt",
    "LICENSES/CC0-1.0.txt",
    "LICENSES/GCC-exception-3.1.txt",
    "LICENSES/GPL-2.0-or-later.txt",
    "LICENSES/GPL-3.0-or-later.txt",
    "LICENSES/OFL-1.1.txt",
    "licensing/ds-companion-blocksds.txt",
    "licensing/ds-companion-dswifi.txt",
    "licensing/ds-companion-fatfs.txt",
    "licensing/ds-companion-libnds.txt",
    "licensing/ds-companion-lwip.txt",
    "licensing/ds-companion-mbedtls.txt",
    "licensing/ds-companion-picolibc.txt",
    "licensing/mbedtls.txt",
)
INSTALL = """DSWT Companion

Back up your save before you start. Copy dswt-companion.nds to your SD card
and launch it in DSi mode through TWiLight Menu++ on your DSi or 3DS.
Keep your own game ROM and its save on the SD card. The app reads game data
from your ROM; no game data is included in this download.

Setup, supported games and recovery:
https://github.com/Sayarinu/DSWT/wiki/DS-Companion
"""


def checked_rom(build):
    data = (build / "dswt-companion.nds").read_bytes()
    if len(data) < 0x4000 or data[0x12] != 0x02 or not data[0x1C] & 1:
        raise ValueError("ROM must support both DS and DSi and enable DSi mode")
    for name, base in (("ARM9i", 0x1C0), ("ARM7i", 0x1D0)):
        offset = struct.unpack_from("<I", data, base)[0]
        size = struct.unpack_from("<I", data, base + 12)[0]
        if offset < 0x4000 or size == 0 or offset + size > len(data):
            raise ValueError(f"Missing or truncated {name} section")
    print("DS/DSi header and payload checks passed")
    return data


def package(version, build, output):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.]+)?", version):
        raise ValueError("Use a release version such as 0.4.1")
    data = checked_rom(build)
    members = {"dswt-companion.nds": data, "INSTALL.txt": INSTALL.encode(),
               "NOTICE": (ROOT / "NOTICE").read_bytes(), "LICENSE": (ROOT / "LICENSE").read_bytes(),
               "font-manifest.json": (build / "build/text/font-manifest.json").read_bytes()}
    members.update({name: (ROOT / name).read_bytes() for name in LICENSES})
    output.mkdir(parents=True, exist_ok=True)
    target = output / f"dswt-companion-{version}.zip"
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, content in sorted(members.items()):
            info = zipfile.ZipInfo(f"dswt-companion-{version}/{name}")
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content)
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--version")
    action.add_argument("--check", action="store_true")
    parser.add_argument("--build", type=Path, default=ROOT / "game/companion")
    parser.add_argument("--output", type=Path, default=ROOT / "game/apps")
    args = parser.parse_args()
    try:
        if args.check:
            checked_rom(args.build)
        else:
            print(package(args.version, args.build, args.output))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Cannot package companion: {error}\n")
