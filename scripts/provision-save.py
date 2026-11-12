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


LAYOUTS = ("protocol/save-layout.json", "protocol/save-layout-hgss.json", "protocol/save-layout-dp.json")
TARGETS = ("us-rev0", "us-rev1", "europe", "heartgold-us-rev0", "soulsilver-us-rev0", "diamond-us-rev5", "pearl-us-rev5")


def layout_for(target):
    """The verified save layout of a ROM target; unknown targets have none."""
    for path in LAYOUTS:
        layout = json.loads((ROOT / path).read_text())
        if target in layout["verified_targets"]:
            return layout
    raise ValueError("Only a verified target's save layout is supported")


def provision(data, target):
    layout = layout_for(target)
    if len(data) != layout["flash_size"]:
        raise ValueError("Only a full 512 KiB save for a verified target is supported")
    footer_format = layout["block_footer"]
    fields, footer_size = footer_format["fields"], footer_format["size"]
    # Both retail blocks need at least one valid fallback. Do not rewrite either.
    for block_id, region in enumerate(layout["retail_regions_per_half"][:2]):
        valid = False
        for half in (0, layout["half_size"]):
            offset, size = half + region["offset"], region["size"]
            body = data[offset:offset + size - footer_size]
            footer = dict(zip(fields, struct.unpack(footer_format["format"], data[offset + size - footer_size:offset + size])))
            valid |= (footer["size"] == size and footer["magic"] == 0x20060623 and footer["block"] == block_id
                      and binascii.crc_hqx(body, 0xffff) == footer["crc"])
        if not valid:
            raise ValueError("Retail save block has no valid copy; preserve it for recovery")
    for bank in layout["journal_banks"]:
        area = data[bank:bank + layout["bank_reserved_size"]]
        if area not in (b"\0" * len(area), b"\xff" * len(area)):
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
