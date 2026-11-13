#!/usr/bin/env python3
"""Make a regional target from a US build when the cartridges share their code.

European English Diamond, Pearl, HeartGold and SoulSilver run exactly the US
code: only the header revision and a few data files differ. This copies those
differences from the maintainer's own regional dump onto a US build, so the
regional target carries the same Wonder Trade code. Both dumps must match the
catalogue; anything that cannot be placed exactly is refused and nothing is
written. Applied to the US dump itself, the result is the regional dump.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys


ROOT = Path(__file__).resolve().parents[1]
HEADER_AREA = 0x4000
HEADER_CRC = 0x15E


class RegionError(ValueError):
    pass


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def crc16(data):
    """The DS header checksum: CRC-16/MODBUS."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def files(rom):
    """Map each NitroFS path to its (start, end) in the ROM."""
    fnt, fat = u32(rom, 0x40), u32(rom, 0x48)
    out = {}

    def walk(directory, prefix):
        entry = fnt + 8 * (directory & 0xFFF)
        cursor, file_id = fnt + u32(rom, entry), u16(rom, entry + 4)
        while length := rom[cursor]:
            name = rom[cursor + 1:cursor + 1 + (length & 0x7F)].decode("latin-1")
            cursor += 1 + (length & 0x7F)
            if length & 0x80:
                walk(u16(rom, cursor), f"{prefix}{name}/")
                cursor += 2
            else:
                out[prefix + name] = (u32(rom, fat + 8 * file_id), u32(rom, fat + 8 * file_id + 4))
                file_id += 1

    walk(0xF000, "/")
    return out


def code(rom):
    """The ARM9 and ARM7 binaries, the ARM9 overlay table and every overlay."""
    parts = [rom[u32(rom, 0x20):u32(rom, 0x20) + u32(rom, 0x2C)], rom[u32(rom, 0x30):u32(rom, 0x30) + u32(rom, 0x3C)]]
    table, size, fat = u32(rom, 0x50), u32(rom, 0x54), u32(rom, 0x48)
    parts.append(rom[table:table + size])
    for entry in range(table, table + size, 32):
        file_id = u32(rom, entry + 24)
        parts.append(rom[u32(rom, fat + 8 * file_id):u32(rom, fat + 8 * file_id + 4)])
    return parts


def narc(archive):
    """Each member's (start, end) in a NARC archive, or None if it is not one."""
    if archive[:4] != b"NARC":
        return None
    table = u16(archive, 0x0C)
    count = u16(archive, table + 8)
    names = table + u32(archive, table + 4)
    images = names + u32(archive, names + 4) + 8
    return [(images + u32(archive, table + 12 + 8 * i), images + u32(archive, table + 16 + 8 * i)) for i in range(count)]


def load(path, variant):
    data = Path(path).read_bytes()
    if len(data) < variant["size"]:
        data += b"\xff" * (variant["size"] - len(data))
    if len(data) != variant["size"] or hashlib.sha256(data).hexdigest() != variant["sha256"]:
        raise RegionError(f"{path} is not the verified {variant['game']} {variant['name']} dump")
    return data


def regionalize(build, us, region):
    """Copy the regional cartridge's differences from the US cartridge onto a US build."""
    if code(us) != code(region):
        raise RegionError("The regional cartridge runs different code; it needs its own port")
    out = bytearray(build)
    for offset in range(HEADER_AREA):
        if us[offset] != region[offset] and offset not in (HEADER_CRC, HEADER_CRC + 1):
            if build[offset] != us[offset]:
                raise RegionError(f"The build changed header byte {offset:#x}, which the region also changes")
            out[offset] = region[offset]
    built, retail, regional = files(build), files(us), files(region)
    if set(retail) != set(regional) or not set(retail) <= set(built):
        raise RegionError("The cartridges' file systems differ")
    for name, (start, end) in retail.items():
        old, new = us[start:end], region[slice(*regional[name])]
        if old == new:
            continue
        here = slice(*built[name])
        current = build[here]
        if current == old:
            if len(new) != len(old):
                raise RegionError(f"{name} changes size in the region")
            out[here] = new
            continue
        # The build changed this archive too: replace only the members the region changes.
        old_members, new_members, members = narc(old), narc(new), narc(current)
        if not old_members or not new_members or not members or len(old_members) != len(new_members):
            raise RegionError(f"The build and the region both change {name}")
        for index, (a, b) in enumerate(zip(old_members, new_members)):
            if old[slice(*a)] == new[slice(*b)]:
                continue
            if index >= len(members) or current[slice(*members[index])] != old[slice(*a)] or a[1] - a[0] != b[1] - b[0]:
                raise RegionError(f"The build and the region both change member {index} of {name}")
            first = built[name][0] + members[index][0]
            out[first:first + (b[1] - b[0])] = new[slice(*b)]
    struct.pack_into("<H", out, HEADER_CRC, crc16(out[:HEADER_CRC]))
    return bytes(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Make a regional target from a US build.")
    parser.add_argument("--variant", required=True, help="regional catalogue ID, such as heartgold-europe")
    parser.add_argument("--us", required=True, help="your own dump of the US base cartridge")
    parser.add_argument("--region", required=True, help="your own dump of the regional cartridge")
    parser.add_argument("build", help="US build to start from, such as the Wonder Trade ROM")
    parser.add_argument("output", help="new file to write")
    args = parser.parse_args(argv)
    try:
        variants = {row["id"]: row for row in json.loads((ROOT / "patches/compatibility.json").read_text())["variants"]}
        variant = variants.get(args.variant)
        if not variant or not variant.get("build_base"):
            raise RegionError(f"{args.variant} is not a regional target built from a US cartridge")
        base = variants[variant["build_base"]]
        us, region = load(args.us, base), load(args.region, variant)
        build = Path(args.build).read_bytes()
        if len(build) != len(us) or build[0x0C:0x10] != us[0x0C:0x10] or build[0x1E] != us[0x1E]:
            raise RegionError(f"{args.build} is not a build of {base['game']} {base['name']}")
        result = regionalize(build, us, region)
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        with os.fdopen(descriptor, "wb") as output:
            output.write(result)
    except (OSError, RegionError, KeyError) as error:
        print(f"Cannot make the regional target: {error}", file=sys.stderr)
        return 1
    print(args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
