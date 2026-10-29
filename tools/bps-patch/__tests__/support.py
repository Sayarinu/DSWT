"""Test-only BPS byte builders for crafted action streams; not used by the tool."""

import struct
import zlib


def number(value):
    out = bytearray()
    while True:
        low, value = value & 0x7F, value >> 7
        if not value:
            out.append(0x80 | low)
            return bytes(out)
        out.append(low)
        value -= 1


def source_read(length):
    return number((length - 1) << 2)


def target_read(data):
    return number((len(data) - 1) << 2 | 1) + data


def source_copy(length, delta):
    return number((length - 1) << 2 | 2) + number(abs(delta) << 1 | (delta < 0))


def target_copy(length, delta):
    return number((length - 1) << 2 | 3) + number(abs(delta) << 1 | (delta < 0))


def assemble(source, target, actions, metadata=b"", target_size=None):
    body = (b"BPS1" + number(len(source)) + number(len(target) if target_size is None else target_size)
            + number(len(metadata)) + metadata + b"".join(actions))
    body += struct.pack("<II", zlib.crc32(source), zlib.crc32(target))
    return body + struct.pack("<I", zlib.crc32(body))


def recrc(patch):
    """Recompute the patch CRC so structural checks, not the checksum, must reject a mutation."""
    return patch[:-4] + struct.pack("<I", zlib.crc32(patch[:-4]))


def literal_patch(source, target):
    return assemble(source, target, [target_read(target)])
