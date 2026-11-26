"""Read and rebuild Nintendo DS cartridge images: code modules, files, archives and layout."""

import struct

BLZ_FOOTER_BYTES = 8
NITRO_PARAMS_SIZE = 32
ARM9_RAM_END = 0x02400000


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
    if len(data) < BLZ_FOOTER_BYTES:
        raise RomError("compressed module is too short")
    footer, extra = struct.unpack_from("<II", data, len(data) - 8)
    header, encoded = footer >> 24, footer & 0xFFFFFF
    if not BLZ_FOOTER_BYTES <= header <= encoded <= len(data):
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


NITRO_CODE = 0x2106C0DE


def module_params(arm9):
    """Offset of the ARM9 module parameters: autoload list, BSS, compressed end, SDK version, magic."""
    at = arm9.find(struct.pack("<I", NITRO_CODE))
    if at < NITRO_PARAMS_SIZE or u32(arm9, at - 4) != NITRO_FOOTER:
        raise RomError("ARM9 module parameters not found")
    return at - 32


def arm9_expanded(arm9, ram=ARM9_RAM):
    """The ARM9 static binary as it runs: a compressed one is expanded (MI_UncompressBackward)."""
    try:
        end = u32(arm9, module_params(arm9) + 0x14)
    except RomError:
        return arm9  # No module parameters: a test image, never compressed.
    if not end:
        return arm9
    return blz_decode(arm9[:end - ram]) + arm9[end - ram:]


def modules(rom):
    """The ARM9 static binary and each overlay, expanded: {"arm9" or overlay id: (RAM address, bytes)}."""
    offset, _, ram, size = struct.unpack_from("<IIII", rom, 0x20)
    found = {"arm9": (ram, arm9_expanded(rom[offset:offset + size], ram))}
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


BANNER_SIZES = {1: 0x840, 2: 0x940, 3: 0xA40, 0x103: 0x23C0}


def rebuild(rom, arm9, overlay_table, files):
    """Lay out a cartridge again: header, ARM9 (footer included), its overlay table, ARM7 and
    its table, the file name table, a file table grown to hold every file, the banner, then
    every file in ID order. files replaces or adds file bodies by ID; the rest are copied."""
    out = bytearray(rom[:0x4000])
    def place(data, offset_field, size_field=None, size=None):
        start = align(len(out), FILE_ALIGN)
        out.extend(b"\xff" * (start - len(out)) + data)
        struct.pack_into("<I", out, offset_field, start if data else 0)
        if size_field is not None:
            struct.pack_into("<I", out, size_field, len(data) if size is None else size)
        return start
    place(arm9, 0x20, 0x2C, len(arm9) - 12)
    place(overlay_table, 0x50, 0x54)
    for offset_field, size_field in ((0x30, 0x3C), (0x58, 0x5C)):
        start, size = u32(rom, offset_field), u32(rom, size_field)
        place(rom[start:start + size], offset_field, size_field)
    place(rom[u32(rom, 0x40):u32(rom, 0x40) + u32(rom, 0x44)], 0x40, 0x44)
    count = max(u32(rom, 0x4C) // 8, max(files, default=-1) + 1)
    fat_at = place(bytes(8 * count), 0x48, 0x4C)
    banner = u32(rom, 0x68)
    place(rom[banner:banner + BANNER_SIZES.get(u16(rom, banner), 0x840)], 0x68)
    for fid in range(count):
        if fid in files:
            body = files[fid]
        else:
            start, end = file_span(rom, fid)
            body = rom[start:end]
        start = align(len(out), FILE_ALIGN)
        out.extend(b"\xff" * (start - len(out)) + body)
        struct.pack_into("<II", out, fat_at + 8 * fid, start, start + len(body))
    struct.pack_into("<I", out, 0x80, len(out))
    capacity = 0x20000 << out[0x14]
    if len(out) > capacity:
        raise RomError("the rebuilt cartridge does not fit its capacity")
    out += b"\xff" * (capacity - len(out))
    struct.pack_into("<H", out, HEADER_CRC, crc16(out[:HEADER_CRC]))
    return bytes(out)


def overlay_entry(oid, ram, size, bss, fid):
    """One uncompressed ARM9 overlay table entry without static initialisers."""
    return struct.pack("<8I", oid, ram, size, bss, 0, 0, fid, 0)
