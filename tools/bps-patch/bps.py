"""Bounded BPS1 walker: validates framing, actions and CRC32 without producing output."""

import hashlib
import struct
import zlib

MAX_FILE = 256 * 1024 * 1024
MAX_PATCH = 64 * 1024 * 1024
MAX_METADATA = 64 * 1024
FOOTER = 12
ACTIONS = ("source_read", "target_read", "source_copy", "target_copy")


class BPSError(ValueError):
    pass


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def _number(data, position, end):
    # BPS numbers are bijective: each value has exactly one encoding.
    value, shift = 0, 1
    for _ in range(10):
        if position >= end:
            raise BPSError("Truncated number")
        byte = data[position]
        position += 1
        value += (byte & 0x7F) * shift
        if byte & 0x80:
            return value, position
        shift <<= 7
        value += shift
    raise BPSError("Number encoding is too long")


def walk(patch):
    """Return a structural report; raise BPSError for anything outside the bounds."""
    data = memoryview(patch)
    if not 4 + 3 + FOOTER <= len(data) <= MAX_PATCH:
        raise BPSError("Patch size is outside the supported range")
    if bytes(data[:4]) != b"BPS1":
        raise BPSError("Missing BPS1 header")
    source_crc, target_crc, patch_crc = struct.unpack("<III", data[-FOOTER:])
    if crc32(data[:-4]) != patch_crc:
        raise BPSError("Patch CRC32 mismatch")
    end = len(data) - FOOTER
    source_size, position = _number(data, 4, end)
    target_size, position = _number(data, position, end)
    metadata_size, position = _number(data, position, end)
    if source_size > MAX_FILE or target_size > MAX_FILE or target_size == 0:
        raise BPSError("Declared file size is outside the supported range")
    if metadata_size > MAX_METADATA or position + metadata_size > end:
        raise BPSError("Metadata is outside the supported range")
    metadata_offset = position
    metadata = bytes(data[position:position + metadata_size])
    position += metadata_size
    counts = dict.fromkeys(ACTIONS, 0)
    literals = []
    output = source_relative = target_relative = 0
    while position < end:
        value, position = _number(data, position, end)
        action, length = value & 3, (value >> 2) + 1
        if length > target_size - output:
            raise BPSError(f"{ACTIONS[action]} at target offset {output} writes past the target")
        if action == 0:
            if output + length > source_size:
                raise BPSError(f"source_read at target offset {output} reads past the source")
        elif action == 1:
            if position + length > end:
                raise BPSError(f"target_read at target offset {output} overruns the patch")
            literal = data[position:position + length]
            literals.append({"target_offset": output, "patch_offset": position, "length": length,
                             "sha256": hashlib.sha256(literal).hexdigest()})
            position += length
        else:
            offset, position = _number(data, position, end)
            delta = -(offset >> 1) if offset & 1 else offset >> 1
            if action == 2:
                source_relative += delta
                if source_relative < 0 or source_relative + length > source_size:
                    raise BPSError(f"source_copy at target offset {output} is outside the source")
                source_relative += length
            else:
                target_relative += delta
                if target_relative < 0 or target_relative >= output:
                    raise BPSError(f"target_copy at target offset {output} reads unwritten target data")
                target_relative += length
        counts[ACTIONS[action]] += 1
        output += length
    if position != end:
        raise BPSError("Actions overrun the footer")
    if output != target_size:
        raise BPSError("Actions do not produce the declared target size")
    return {
        "source": {"size": source_size, "crc32": f"{source_crc:08x}"},
        "target": {"size": target_size, "crc32": f"{target_crc:08x}"},
        "patch": {"size": len(data), "crc32": f"{patch_crc:08x}", "sha256": hashlib.sha256(data).hexdigest()},
        "metadata": {"offset": metadata_offset, "size": metadata_size,
                     "sha256": hashlib.sha256(metadata).hexdigest()},
        "actions": counts,
        "literals": {"count": len(literals), "bytes": sum(item["length"] for item in literals), "items": literals},
    }
