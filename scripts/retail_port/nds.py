"""Read and rebuild Nintendo DS cartridge images: code modules, files, archives and layout."""

import struct

ARM9_RAM = 0x02000000
NITRO_FOOTER = 0xDEC00621
HEADER_CRC = 0x15E
FILE_ALIGN = 0x200


class RomError(ValueError):
    pass


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def crc16(data):
    """The DS header checksum: CRC-16/MODBUS."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def blz_decode(data):
    """Undo Nitro SDK backward LZ compression (MI_UncompressBackward)."""
    if len(data) < 8:
        raise RomError("compressed module is too short")
    footer, extra = struct.unpack_from("<II", data, len(data) - 8)
    header, encoded = footer >> 24, footer & 0xFFFFFF
    if not 8 <= header <= encoded <= len(data):
        raise RomError("compressed module has an invalid footer")
    out = bytearray(data) + bytearray(extra)
    source, target, end = len(data) - header, len(out), len(data) - encoded
    while source > end:
        source -= 1
        flags = data[source]
        for _ in range(8):
            if source <= end:
                break
            if flags & 0x80:
                source -= 2
                pair = data[source] | data[source + 1] << 8
                for _ in range((pair >> 12) + 3):
                    target -= 1
                    out[target] = out[target + (pair & 0xFFF) + 3]
            else:
                source -= 1
                target -= 1
                out[target] = data[source]
            flags = flags << 1 & 0xFF
    return bytes(out)


def overlays(rom):
    """(id, RAM address, RAM size, BSS size, file ID, compressed) for each ARM9 overlay."""
    table, size = struct.unpack_from("<II", rom, 0x50)
    rows = []
    for entry in range(table, table + size, 32):
        oid, ram, ram_size, bss, _, _, fid, flags = struct.unpack_from("<8I", rom, entry)
        rows.append((oid, ram, ram_size, bss, fid, bool(flags >> 24 & 1)))
    return rows


def file_span(rom, fid):
    fat = u32(rom, 0x48)
    return struct.unpack_from("<II", rom, fat + 8 * fid)


def modules(rom):
    """The ARM9 static binary and each overlay, expanded: {"arm9" or overlay id: (RAM address, bytes)}."""
    offset, _, ram, size = struct.unpack_from("<IIII", rom, 0x20)
    found = {"arm9": (ram, rom[offset:offset + size])}
    for oid, ram, _, _, fid, compressed in overlays(rom):
        start, end = file_span(rom, fid)
        body = rom[start:end]
        found[oid] = (ram, blz_decode(body) if compressed else body)
    return found


def file_ids(rom):
    """{path: file ID} from the cartridge's file name table."""
    fnt, found = u32(rom, 0x40), {}

    def walk(directory, prefix):
        entry = fnt + 8 * (directory & 0xFFF)
        cursor, fid = fnt + u32(rom, entry), u16(rom, entry + 4)
        while length := rom[cursor]:
            name = rom[cursor + 1:cursor + 1 + (length & 0x7F)].decode("latin-1")
            cursor += 1 + (length & 0x7F)
            if length & 0x80:
                walk(u16(rom, cursor), prefix + name + "/")
                cursor += 2
            else:
                found[prefix + name] = fid
                fid += 1

    walk(0xF000, "")
    return found


def narc_split(archive):
    """(members, name table chunk) of a NARC archive."""
    if archive[:4] != b"NARC":
        raise RomError("not a NARC archive")
    btaf = u16(archive, 0x0C)
    count = u16(archive, btaf + 8)
    btnf = btaf + u32(archive, btaf + 4)
    gmif = btnf + u32(archive, btnf + 4)
    members = [archive[gmif + 8 + u32(archive, btaf + 12 + 8 * i):gmif + 8 + u32(archive, btaf + 16 + 8 * i)]
               for i in range(count)]
    return members, archive[btnf:gmif]


def narc_join(members, names):
    table, body = b"", b""
    for member in members:
        table += struct.pack("<II", len(body), len(body) + len(member))
        body += member + b"\xff" * (align(len(member), 4) - len(member))
    btaf = b"BTAF" + struct.pack("<IHH", 12 + len(table), len(members), 0) + table
    gmif = b"GMIF" + struct.pack("<I", 8 + len(body)) + body
    return b"NARC" + struct.pack("<HHIHH", 0xFFFE, 0x0100, 16 + len(btaf) + len(names) + len(gmif), 16, 3) + btaf + names + gmif


def autoload_table(arm9, ram=ARM9_RAM):
    """(module parameter offset, list start, list end) of an ARM9 binary with its Nitro footer appended."""
    if u32(arm9, len(arm9) - 12) != NITRO_FOOTER:
        raise RomError("ARM9 binary has no Nitro footer")
    params = u32(arm9, len(arm9) - 8)
    return params, u32(arm9, params) - ram, u32(arm9, params + 4) - ram


def thumb_entry_veneer(at, trampoline):
    """push {lr}; blx trampoline: six bytes that replace a Thumb function's entry."""
    call = at + 2
    offset = trampoline - ((call + 4) & ~3)
    if offset % 4 or not -(1 << 22) <= offset < (1 << 22):
        raise RomError(f"trampoline {trampoline:#x} is out of BLX range of {at:#x}")
    return struct.pack("<HHH", 0xB500, 0xF000 | ((offset >> 12) & 0x7FF), 0xE800 | ((offset >> 1) & 0x7FF))


def arm_trampoline(target):
    """ldr lr, [sp], #4; ldr pc, [pc, #-4]; .word target: restores lr and jumps."""
    return struct.pack("<III", 0xE49DE004, 0xE51FF004, target)


def add_autoload(rom, segment, address, arena_literal, arena):
    """Insert a segment into the ARM9 autoload list and point the arena literal at arena.

    The segment loads at address before main runs; the static binary keeps its
    address. Returns the new ARM9 binary with its footer."""
    offset, size = u32(rom, 0x20), u32(rom, 0x2C)
    arm9 = bytearray(rom[offset:offset + size + 12])
    params, start, end = autoload_table(arm9)
    if u32(arm9, arena_literal) != address:
        raise RomError("arena literal does not hold the arena start")
    struct.pack_into("<I", arm9, arena_literal, arena)
    footer = arm9[size:]
    table = arm9[start:end] + struct.pack("<III", address, len(segment), 0)
    arm9 = arm9[:start] + segment
    new_start = ARM9_RAM + len(arm9)
    arm9 += table
    struct.pack_into("<II", arm9, params, new_start, new_start + len(table))
    return bytes(arm9) + footer


def relayout(rom, arm9, files):
    """Rebuild a cartridge with a new ARM9 binary (footer included) and replaced files.

    Everything after the ARM9 binary moves by its growth; each replaced file moves
    to the end. The result is padded to the cartridge capacity with a fresh header CRC."""
    a9_offset, a9_size = u32(rom, 0x20), u32(rom, 0x2C)
    old_end = align(a9_offset + a9_size + 12, FILE_ALIGN)
    new_end = align(a9_offset + len(arm9), FILE_ALIGN)
    delta = new_end - old_end
    used = u32(rom, 0x80)
    out = bytearray(rom[:a9_offset]) + arm9
    out += bytes(new_end - len(out))
    out += rom[old_end:used]
    for field in (0x30, 0x40, 0x48, 0x50, 0x68):
        if u32(out, field) >= old_end:
            struct.pack_into("<I", out, field, u32(out, field) + delta)
    struct.pack_into("<I", out, 0x2C, len(arm9) - 12)
    fat = u32(out, 0x48)
    for entry in range(fat, fat + u32(out, 0x4C), 8):
        start, end = struct.unpack_from("<II", out, entry)
        if start >= old_end:
            struct.pack_into("<II", out, entry, start + delta, end + delta)
    for fid, body in sorted(files.items()):
        place = align(len(out), FILE_ALIGN)
        out += b"\xff" * (place - len(out)) + body
        struct.pack_into("<II", out, fat + 8 * fid, place, place + len(body))
    struct.pack_into("<I", out, 0x80, len(out))
    capacity = 0x20000 << out[0x14]
    if len(out) > capacity:
        raise RomError("the rebuilt cartridge does not fit its capacity")
    out += b"\xff" * (capacity - len(out))
    struct.pack_into("<H", out, HEADER_CRC, crc16(out[:HEADER_CRC]))
    return bytes(out)
