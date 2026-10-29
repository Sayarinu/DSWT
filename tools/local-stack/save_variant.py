#!/usr/bin/env python3
"""Derive a disposable retail test save with edited party Pokémon.

Only blank-journal originals are accepted, so every variant is enrolled as a new
identity by `emulator.py create`. Both retail copies receive the same edits and
their block CRCs are recomputed; the source is never modified.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stack

VERIFY = stack.load_module("variant_verify", stack.ROOT / "scripts/verify-demo-trade.py")
PARTY, SLOT, BOX = 152, 236, 136
FIELDS = {"egg", "hp", "item", "capsule", "move"}


def stream(data, seed):
    """Gen IV LCG stream cipher over 16-bit words; it is its own inverse."""
    result = bytearray()
    for offset in range(0, len(data), 2):
        seed = (seed * 0x41C64E6D + 0x6073) & 0xFFFFFFFF
        result += struct.pack("<H", struct.unpack_from("<H", data, offset)[0] ^ (seed >> 16))
    return result


def encode(canonical, extension):
    """Encrypt a canonical box image and its plain party extension."""
    pid = struct.unpack_from("<I", canonical)[0]
    plain = canonical[8:]
    checksum = sum(struct.unpack("<64H", plain)) & 0xFFFF
    order = VERIFY.ORDERS[((pid >> 13) & 31) % 24]
    shuffled = b"".join(plain[block * 32:block * 32 + 32] for block in order)
    return canonical[:4] + struct.pack("<HH", 0, checksum) + stream(shuffled, checksum) + stream(extension, pid)


def edit(slot_bytes, change):
    canonical = bytearray(VERIFY.canonical(bytes(slot_bytes[:BOX])))
    pid = struct.unpack_from("<I", canonical)[0]
    extension = stream(slot_bytes[BOX:], pid)
    if "egg" in change:  # Block B IV word: bit 30 is the Egg flag.
        word = struct.unpack_from("<I", canonical, 0x38)[0]
        struct.pack_into("<I", canonical, 0x38, word | 1 << 30 if change["egg"] else word & ~(1 << 30))
    if "item" in change: struct.pack_into("<H", canonical, 0x0A, change["item"])
    if "move" in change:
        index, move = change["move"]
        struct.pack_into("<H", canonical, 0x28 + 2 * index, move)
    if "hp" in change: struct.pack_into("<H", extension, 0x8E - BOX, change["hp"])
    if "capsule" in change: extension[0x8D - BOX] = change["capsule"]
    return encode(bytes(canonical), bytes(extension))


def variant(raw, changes):
    raw = bytearray(raw)
    offset, size = VERIFY.BLOCKS[0]
    for bank in (0, 0x40000):
        VERIFY.inventory(bytes(raw), bank)  # Refuse damaged or unfamiliar retail copies first.
        count = struct.unpack_from("<I", raw, bank + PARTY + 4)[0]
        for slot, change in changes.items():
            if not 0 <= slot < count: raise ValueError("Edit an occupied party slot")
            start = bank + PARTY + 8 + slot * SLOT
            raw[start:start + SLOT] = edit(raw[start:start + SLOT], change)
        body = raw[bank + offset:bank + offset + size - 20]
        struct.pack_into("<H", raw, bank + offset + size - 2, VERIFY.crc(body))
        VERIFY.inventory(bytes(raw), bank)
    return bytes(raw)


def parse(text):
    value = json.loads(text)
    if not isinstance(value, dict) or not value: raise ValueError("Describe at least one party slot")
    changes = {}
    for slot, change in value.items():
        if not isinstance(change, dict) or not change or set(change) - FIELDS: raise ValueError("Unsupported party edit")
        if "move" in change:
            index, move = change["move"]
            if not (0 <= index < 4 and 0 < move < 468): raise ValueError("Use a move index 0-3 and a Platinum move")
        for key, limit in (("hp", 0xFFFF), ("item", 467), ("capsule", 0xFF)):
            if key in change and not (type(change[key]) is int and 0 <= change[key] <= limit): raise ValueError("Invalid " + key)
        if "egg" in change and type(change["egg"]) is not bool: raise ValueError("Invalid egg")
        changes[int(slot)] = change
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--edit", required=True, help='JSON such as {"1": {"egg": true}, "2": {"hp": 0, "item": 137}}')
    args = parser.parse_args()
    raw = args.source.read_bytes()
    if VERIFY.journal(raw, baseline=True) is not None:  # Never derive from an enrolled save.
        raise ValueError("Use a blank-journal original")
    data = variant(raw, parse(args.edit))
    with os.fdopen(os.open(args.destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb") as output:
        output.write(data); output.flush(); os.fsync(output.fileno())


if __name__ == "__main__":
    try: main()
    except (ValueError, OSError) as error: raise SystemExit("Save variant refused: " + str(error))
