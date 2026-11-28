#!/usr/bin/env python3
"""Check the built DS/DSi homebrew and zip only its reviewed release files."""

import argparse
import base64
import io
from pathlib import Path
import re
import struct
import zipfile

UNIT_CODE_OFFSET = 0x12
TWL_FLAGS_OFFSET = 0x1C
ARM9I_HEADER_OFFSET = 0x1C0
ARM7I_HEADER_OFFSET = 0x1D0
PAYLOAD_LENGTH_OFFSET = 12
NDS_HEADER_REGION = 0x4000
DUAL_MODE_UNIT_CODE = 0x02


ROOT = Path(__file__).resolve().parents[2]
LICENSES = (
    "LICENSES/Apache-2.0.txt",
    "LICENSES/CC0-1.0.txt",
    "LICENSES/GCC-exception-3.1.txt",
    "LICENSES/GPL-2.0-or-later.txt",
    "LICENSES/GPL-3.0-or-later.txt",
    "LICENSES/OFL-1.1.txt",
    "LICENSES/WTFPL.txt",
    "licensing/ds-companion-blocksds.txt",
    "licensing/ds-companion-dswifi.txt",
    "licensing/ds-companion-fatfs.txt",
    "licensing/ds-companion-libnds.txt",
    "licensing/ds-companion-lwip.txt",
    "licensing/ds-companion-mbedtls.txt",
    "licensing/ds-companion-picolibc.txt",
    "licensing/mbedtls.txt",
)
INSTALL = """DSWT

Open Install-DSWT.html in your browser and download your own SD card kit.
Copy its roms and _nds folders to the root of your SD card. The browser creates
private seeds for first-time setup. Share the public installer with other players.
Pick your language on first launch. Highlight a Game Card or SD save and press A
to open it. Press Y on a party member to see its stats, IVs, EVs and moves.
Choose Exit from the game list to leave DSWT.

For a manual installation, back up your save first. Copy dswt.nds to your SD card
and launch it in DSi mode through TWiLight Menu++ on your DSi or 3DS.
For a supported retail Game Card, insert it before launching DSWT. The app can
use the cartridge session opened by TWiLight Menu++ without any BIOS files.
For an SD save, keep your game ROM and its save on the SD card. The app opens
the active TWiLight save slot for that game.
If first-time setup asks you to draw loops, follow the prompt to generate a
private seed for that save. Future trades reuse the initialized save.

Setup, supported games and recovery:
https://github.com/Sayarinu/DSWT/wiki/DS-Companion
"""


def checked_rom(build):
    data = (build / "dswt.nds").read_bytes()
    if len(data) < NDS_HEADER_REGION or data[UNIT_CODE_OFFSET] != DUAL_MODE_UNIT_CODE or not data[TWL_FLAGS_OFFSET] & 1:
        raise ValueError("ROM must support both DS and DSi and enable DSi mode")
    for name, base in (("ARM9i", ARM9I_HEADER_OFFSET), ("ARM7i", ARM7I_HEADER_OFFSET)):
        offset = struct.unpack_from("<I", data, base)[0]
        size = struct.unpack_from("<I", data, base + PAYLOAD_LENGTH_OFFSET)[0]
        if offset < NDS_HEADER_REGION or size == 0 or offset + size > len(data):
            raise ValueError(f"Missing or truncated {name} section")
    print("DS/DSi header and payload checks passed")
    return data


def download_installer(members):
    """Embed the complete SD kit. Its first, uncompressed ZIP entry is filled by the browser."""
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as bundle:
        seed_path = "_nds/dswt/seeds.bin"
        bundle.writestr(zipfile.ZipInfo(seed_path), bytes(32 * 64))
        for name, content in sorted(members.items()):
            path = "roms/nds/dswt.nds" if name == "dswt.nds" else f"_nds/dswt/{name}"
            if name != "INSTALL.txt":
                bundle.writestr(zipfile.ZipInfo(path), content, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(stream) as bundle:
        central_crc = bundle.start_dir + 16
    # No ZIP extra field or data descriptor on this seekable output.
    seed_offset = 30 + len(seed_path.encode("ascii"))
    template = Path(__file__).with_name("companion-download.html").read_text(encoding="utf-8")
    return (template.replace("__ZIP_BASE64__", base64.b64encode(stream.getvalue()).decode("ascii"))
            .replace("__SEED_OFFSET__", str(seed_offset)).replace("__CENTRAL_CRC__", str(central_crc)))


def package(version, build, output):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.]+)?", version):
        raise ValueError("Use a release version such as 0.4.1")
    data = checked_rom(build)
    members = {"dswt.nds": data, "INSTALL.txt": INSTALL.encode(),
               "NOTICE": (ROOT / "NOTICE").read_bytes(), "LICENSE": (ROOT / "LICENSE").read_bytes(),
               "font-manifest.json": (build / "build/text/font-manifest.json").read_bytes()}
    members.update({name: (ROOT / name).read_bytes() for name in LICENSES})
    output.mkdir(parents=True, exist_ok=True)
    (output / "dswt.nds").write_bytes(data)
    notices = b"\n\n".join(name.encode() + b"\n\n" + members[name]
                           for name in ("NOTICE", "LICENSE", *LICENSES, "font-manifest.json"))
    (output / "THIRD-PARTY-NOTICES.txt").write_bytes(notices)
    installer = download_installer(members).encode("utf-8")
    (output / f"dswt-companion-{version}.html").write_bytes(installer)
    members["Install-DSWT.html"] = installer
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
