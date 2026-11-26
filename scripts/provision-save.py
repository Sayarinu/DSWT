#!/usr/bin/env python3
"""Create a separate save copy with a host CSPRNG seed in verified unused banks."""

import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import secrets
import struct
import sys


ROOT = Path(__file__).resolve().parent.parent


def seed_record(seed, generation):
    if len(seed) != 32 or not any(seed) or not 0 <= generation < 2**64:
        raise ValueError("Invalid seed or journal generation")
    record = bytearray(4096)
    struct.pack_into("<4sHHIIQ", record, 0, b"DSWJ", 1, 0, 4096, 0, generation)
    struct.pack_into("<HH", record, 1440, 1, 32)
    record[1444:1476] = seed
    record[4060:4092] = hashlib.sha256(record[:4060]).digest()
    record[4092:] = b"CMIT"
    return bytes(record)


LAYOUTS = ("protocol/save-layout.json", "protocol/save-layout-hgss.json", "protocol/save-layout-dp.json",
           "protocol/save-layout-bw.json", "protocol/save-layout-b2w2.json")
TARGETS = ("us-rev0", "us-rev1", "europe", "france", "germany", "italy", "spain", "japan", "platinum-korea", "heartgold-us-rev0", "soulsilver-us-rev0",
           "diamond-us-rev5", "pearl-us-rev5", "heartgold-europe", "soulsilver-europe", "heartgold-france", "heartgold-germany", "heartgold-italy", "heartgold-spain", "soulsilver-france", "soulsilver-germany", "soulsilver-italy", "soulsilver-spain", "heartgold-korea", "soulsilver-korea", "heartgold-japan", "soulsilver-japan", "diamond-europe-rev5",
           "pearl-europe-rev5", "diamond-france", "diamond-germany", "diamond-italy", "diamond-spain", "pearl-france", "pearl-germany", "pearl-italy", "pearl-spain", "diamond-korea", "pearl-korea", "diamond-japan", "pearl-japan", "black-us", "white-us", "black2-us", "white2-us")

# Generation V main-save facts the layout files leave out, by the layout's
# decompilation pin (aletheia's _MainSaveDesc, system_data.c and
# backup_system.c; pokebw2 src/system/save_table.c, backup_system.c and
# trainer_data_savedata.c): the block count (a CRC of each block precedes the
# save footer, whose own CRC covers them), the trainer data block, and where it
# keeps the musical distribution's marker (set: the game reads the area that
# holds the journal banks).
MAIN_SAVES = {
    "client/games/upstream-aletheia.json": {"blocks": 69, "trainer_block": 27, "trainer_offset": 0x19400,
                                      "trainer_size": 0x68, "musical_marker": 0x3C},
    "client/games/upstream-pokebw2.json": {"blocks": 74, "trainer_block": 27, "trainer_offset": 0x19400,
                                     "trainer_size": 0xB0, "musical_marker": 0x3C},
}
MARKER_SET = 0xC21E


def layout_for(target):
    """The verified save layout of a ROM target; unknown targets have none."""
    for path in LAYOUTS:
        layout = json.loads((ROOT / path).read_text())
        if target in layout["verified_targets"]:
            return layout
    raise ValueError("Only a verified target's save layout is supported")


def footer_magic(layout, target):
    """Every Korean cartridge marks its save blocks with its own magic number."""
    magic = layout["footer_magic"]
    return magic["korean"] if target.endswith(layout["korean_target_suffix"]) else magic["international"]


def musical_marker_set(data, layout):
    """Check a Generation V main save; true when it still reads the musical distribution area.

    A block can be current in either copy: the game loads the one whose CRC the
    save's CRC table lists, so every such copy of the trainer data is read."""
    facts = MAIN_SAVES[layout["upstream_pin"]]
    regions = {region["name"]: region for region in layout["retail_regions"]}
    used, footer = regions["main"]["used"], layout["save_footer"]
    crcs = facts["blocks"] * 2 + facts["blocks"] % 2 * 2
    copies = (regions["main"]["offset"], regions["main_backup"]["offset"])
    markers = []
    for copy in copies:
        table = data[copy + used - footer["size"] - crcs:copy + used - footer["size"]]
        fields = dict(zip(footer["fields"], struct.unpack_from(footer["format"], data, copy + used - footer["size"])))
        if (fields["size"] != used or fields["magic"] != layout["footer_magic"]["international"]
                or fields["crc"] != binascii.crc_hqx(table, 0xffff)):
            continue
        expected = struct.unpack_from("<H", table, 2 * facts["trainer_block"])[0]
        for other in copies:
            start = other + facts["trainer_offset"]
            body = data[start:start + facts["trainer_size"]]
            if (struct.unpack_from("<H", data, start + facts["trainer_size"] + 2)[0] == expected
                    and binascii.crc_hqx(body, 0xffff) == expected):
                markers.append(struct.unpack_from("<H", body, facts["musical_marker"])[0])
    if not markers:
        raise ValueError("Retail save has no valid copy; preserve it for recovery")
    return MARKER_SET in markers


def check_retail(data, layout, target):
    """Refuse a save without a valid retail copy; never rewrite one."""
    if "save_footer" in layout:
        if musical_marker_set(data, layout):
            raise ValueError("The save keeps a downloaded musical where Wonder Trade stores its journal; "
                             "it is preserved and not provisioned")
        return
    magic = footer_magic(layout, target)
    footer_format = layout["block_footer"]
    fields, footer_size = footer_format["fields"], footer_format["size"]
    # Both retail blocks need at least one valid fallback.
    for block_id, region in enumerate(layout["retail_regions_per_half"][:2]):
        valid = False
        for half in (0, layout["half_size"]):
            offset, size = half + region["offset"], region["size"]
            body = data[offset:offset + size - footer_size]
            footer = dict(zip(fields, struct.unpack(footer_format["format"], data[offset + size - footer_size:offset + size])))
            valid |= (footer["size"] == size and footer["magic"] == magic and footer["block"] == block_id
                      and binascii.crc_hqx(body, 0xffff) == footer["crc"])
        if not valid:
            raise ValueError("Retail save block has no valid copy; preserve it for recovery")


def provision(data, target):
    layout = layout_for(target)
    if len(data) != layout["flash_size"]:
        raise ValueError("Only a full 512 KiB save for a verified target is supported")
    check_retail(data, layout, target)
    for bank in layout["journal_banks"]:
        area = data[bank:bank + layout["bank_reserved_size"]]
        if area in (b"\0" * len(area), b"\xff" * len(area)):
            continue
        # Generation V banks lie in the musical distribution area. With its
        # marker clear (checked above) the game never reads it again, so a
        # stale distribution there is free space; a journal never is.
        if "journal_host_region" not in layout or area.startswith(b"DSWJ"):
            raise ValueError("Reserved bank is not blank; refusing to replace seeds, identities or recovery evidence")
    record = seed_record(secrets.token_bytes(32), 1)
    result = bytearray(data)
    for bank in layout["journal_banks"]:
        result[bank:bank + len(record)] = record
    return bytes(result)


def write_new(path, data):
    # O_EXCL also rejects symlinks, aliases and an existing source/destination.
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if path.read_bytes() != data:
            raise OSError("Provisioned copy did not verify")
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        path.unlink()
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--target", choices=TARGETS, required=True,
                        help="verified ROM target reported by identify-rom.py")
    args = parser.parse_args()
    try:
        write_new(args.destination, provision(args.source.read_bytes(), args.target))
        print(f"Provisioned a separate save copy: {args.destination}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Cannot provision save: {error}\n")
