"""Bounded BPS1 walker and applier: validates framing, actions and CRC32 checks."""

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


def _parse(patch):
    """Check the framing; return the header and a generator of bounded actions.

    Each action is (kind, output offset, length, offset), where offset is the
    patch position of a target_read literal, or the absolute source/target offset
    of a copy. The generator raises BPSError for anything outside the bounds.
    """
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
    header = {"source_size": source_size, "target_size": target_size, "source_crc": source_crc,
              "target_crc": target_crc, "patch_crc": patch_crc, "metadata_offset": position,
              "metadata_size": metadata_size}

    def actions():
        position = header["metadata_offset"] + metadata_size
        output = source_relative = target_relative = 0
        while position < end:
            value, position = _number(data, position, end)
            action, length = value & 3, (value >> 2) + 1
            if length > target_size - output:
                raise BPSError(f"{ACTIONS[action]} at target offset {output} writes past the target")
            if action == 0:
                if output + length > source_size:
                    raise BPSError(f"source_read at target offset {output} reads past the source")
                offset = output
            elif action == 1:
                if position + length > end:
                    raise BPSError(f"target_read at target offset {output} overruns the patch")
                offset = position
                position += length
            else:
                relative, position = _number(data, position, end)
                delta = -(relative >> 1) if relative & 1 else relative >> 1
                if action == 2:
                    source_relative += delta
                    if source_relative < 0 or source_relative + length > source_size:
                        raise BPSError(f"source_copy at target offset {output} is outside the source")
                    offset = source_relative
                    source_relative += length
                else:
                    target_relative += delta
                    if target_relative < 0 or target_relative >= output:
                        raise BPSError(f"target_copy at target offset {output} reads unwritten target data")
                    offset = target_relative
                    target_relative += length
            yield action, output, length, offset
            output += length
        if position != end:
            raise BPSError("Actions overrun the footer")
        if output != target_size:
            raise BPSError("Actions do not produce the declared target size")

    return header, actions()


def walk(patch):
    """Return a structural report; raise BPSError for anything outside the bounds."""
    header, actions = _parse(patch)
    counts = dict.fromkeys(ACTIONS, 0)
    literals = []
    for action, output, length, offset in actions:
        if action == 1:
            literals.append({"target_offset": output, "patch_offset": offset, "length": length,
                             "sha256": hashlib.sha256(patch[offset:offset + length]).hexdigest()})
        counts[ACTIONS[action]] += 1
    metadata = patch[header["metadata_offset"]:header["metadata_offset"] + header["metadata_size"]]
    return {
        "source": {"size": header["source_size"], "crc32": f"{header['source_crc']:08x}"},
        "target": {"size": header["target_size"], "crc32": f"{header['target_crc']:08x}"},
        "patch": {"size": len(patch), "crc32": f"{header['patch_crc']:08x}", "sha256": hashlib.sha256(patch).hexdigest()},
        "metadata": {"offset": header["metadata_offset"], "size": header["metadata_size"],
                     "sha256": hashlib.sha256(metadata).hexdigest()},
        "actions": counts,
        "literals": {"count": len(literals), "bytes": sum(item["length"] for item in literals), "items": literals},
    }


def apply(source, patch):
    """Return the patched bytes after checking the source, every action and the target CRC32."""
    header, actions = _parse(patch)
    if len(source) != header["source_size"] or crc32(source) != header["source_crc"]:
        raise BPSError("Patch was not made for this input")
    target = bytearray(header["target_size"])
    for action, output, length, offset in actions:
        if action == 0:
            target[output:output + length] = source[output:output + length]
        elif action == 1:
            target[output:output + length] = patch[offset:offset + length]
        elif action == 2:
            target[output:output + length] = source[offset:offset + length]
        else:
            # An overlapping target_copy repeats the bytes between its offset and the output.
            period = output - offset
            if period >= length:
                target[output:output + length] = target[offset:offset + length]
            else:
                pattern = bytes(target[offset:output])
                target[output:output + length] = (pattern * (length // period + 1))[:length]
    if crc32(target) != header["target_crc"]:
        raise BPSError("Patched output CRC32 mismatch")
    return bytes(target)
