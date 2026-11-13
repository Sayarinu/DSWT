"""Test-only BPS byte builders and ROM-shaped fixtures; not used by the tool."""

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


def blz(data, run=0):
    """Compress data, whose last run bytes are zero, as BLZ: literals, then 18-byte copies for the zeros."""
    tokens = [("literal", 0)] * 3 if run else []  # in decode order, from the end backwards
    copies = (run - 3) // 18 if run else 0
    tokens += [("copy", 15 << 12)] * copies  # length 15 + 3, distance 0 + 3
    tokens += [("literal", byte) for byte in reversed(data[:len(data) - 3 - 18 * copies if run else len(data)])]
    backwards = bytearray()
    for group in range(0, len(tokens), 8):
        chunk = tokens[group:group + 8]
        backwards.append(sum(0x80 >> index for index, (kind, _) in enumerate(chunk) if kind == "copy"))
        for kind, value in chunk:
            backwards += bytes([value >> 8, value & 0xFF]) if kind == "copy" else bytes([value])
    stream = bytes(reversed(backwards))
    size = len(stream) + 8
    return stream + struct.pack("<II", 8 << 24 | size, len(data) - size)


def with_overlay(base, code):
    """Add an ARM9 overlay table and FAT naming one compressed overlay file at 0x400."""
    data = bytearray(base)
    data[0x48:0x58] = struct.pack("<IIII", 0x300, 8, 0x340, 32)
    data[0x300:0x308] = struct.pack("<II", 0x400, 0x400 + len(code))
    data[0x340:0x360] = struct.pack("<8I", 0, 0, 0, 0, 0, 0, 0, 1 << 24 | len(code))
    data[0x400:0x400 + len(code)] = code
    return bytes(data)


# ROM-shaped fixtures: a cartridge header, a pseudo-random body and erased-flash padding.
SIZE, USED = 4096, 4096 - 0x100
PINS = [bytes([0x31] * 32), bytes([0x32] * 32)]
PROFILE = {"hostname": "wt.dswt.games", "ipv4": "1.1.1.1", "port": 443,
           "current_spki_sha256": PINS[0].hex(), "next_spki_sha256": PINS[1].hex()}


def rom(revision, seed, code="CPUE", payload=b""):
    import hashlib
    data = bytearray(SIZE)
    data[0x0C:0x10] = code.encode()
    data[0x1E] = revision
    data[0x80:0x84] = USED.to_bytes(4, "little")
    body = b"".join(hashlib.sha256(f"{seed}:{index}".encode()).digest() for index in range((USED - 0x200) // 32 + 1))
    data[0x200:USED] = body[:USED - 0x200]
    data[0x200:0x200 + len(payload)] = payload
    data[USED:] = b"\xff" * (SIZE - USED)
    return bytes(data)


BASES = {"us-rev0": rom(0, "base-0"), "us-rev1": rom(1, "base-1")}
TARGETS = {key: rom(value[0x1E], f"target-{key}", payload=b"wt.dswt.games:443" + PINS[0] + PINS[1])
           for key, value in BASES.items()}
