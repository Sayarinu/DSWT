"""Read 32-bit little-endian ARM ELF files: Metrowerks objects and the linked game image."""

import struct

SHT_SYMTAB, SHT_NOBITS, SHT_RELA, SHT_REL = 2, 8, 4, 9
SHF_WRITE, SHF_EXECINSTR = 1, 4
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_ARM_TFUNC = 0, 1, 2, 3, 13
SHN_UNDEF, SHN_LORESERVE = 0, 0xFF00
R_ARM_PC24, R_ARM_ABS32, R_ARM_REL32, R_ARM_THM_CALL = 1, 2, 3, 10
R_ARM_CALL, R_ARM_JUMP24 = 28, 29
ARM_BRANCHES = (R_ARM_PC24, R_ARM_CALL, R_ARM_JUMP24)
MAPPING = ("$a", "$t", "$d")
# GNU as objects need these EABI flags to link with Metrowerks objects.
MWCC_EABI_FLAGS = 0x02100000


class ElfError(ValueError):
    pass


class Elf:
    """Sections, symbols and relocations of one ELF file.

    relocs maps a section index to (offset, type, symbol index, addend) rows. In
    an object the offset is section-relative; in the linked image it is an address.
    """

    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ElfError("not a 32-bit little-endian ELF file")
        self.data = data
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
        self.sections = []
        for i in range(shnum):
            name, kind, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<10I", data, shoff + i * shentsize)
            self.sections.append(dict(name=name, type=kind, flags=flags, addr=addr, off=off, size=size,
                                      link=link, info=info))
        names = self.sections[shstrndx]
        for section in self.sections:
            section["name"] = self._string(names, section["name"])
        self.symbols = []
        for section in self.sections:
            if section["type"] != SHT_SYMTAB:
                continue
            strings = self.sections[section["link"]]
            for name, value, size, info, _, shndx in struct.iter_unpack(
                    "<IIIBBH", data[section["off"]:section["off"] + section["size"]]):
                self.symbols.append(dict(name=self._string(strings, name), value=value, size=size,
                                         bind=info >> 4, type=info & 15, shndx=shndx))
            break
        self.relocs, self.inline_addends = {}, set()
        for section in self.sections:
            if section["type"] not in (SHT_RELA, SHT_REL):
                continue
            body = data[section["off"]:section["off"] + section["size"]]
            rows = self.relocs.setdefault(section["info"], [])
            if section["type"] == SHT_REL:
                self.inline_addends.add(section["info"])
            if section["type"] == SHT_RELA:
                rows.extend((off, info & 0xFF, info >> 8, addend) for off, info, addend in struct.iter_unpack("<IIi", body))
            else:
                rows.extend((off, info & 0xFF, info >> 8, 0) for off, info in struct.iter_unpack("<II", body))

    def _string(self, section, offset):
        start = section["off"] + offset
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def section_bytes(self, index):
        section = self.sections[index]
        if section["type"] == SHT_NOBITS:
            return bytes(section["size"])
        return self.data[section["off"]:section["off"] + section["size"]]

    def index(self, name):
        found = [i for i, section in enumerate(self.sections) if section["name"] == name]
        if len(found) != 1:
            raise ElfError(f"expected one section {name}, found {len(found)}")
        return found[0]

    def mapping_modes(self):
        """Sorted ($a/$t/$d offset, mode) rows per section."""
        marks = {}
        for symbol in self.symbols:
            if symbol["name"] in MAPPING and 0 < symbol["shndx"] < SHN_LORESERVE:
                marks.setdefault(symbol["shndx"], []).append((symbol["value"], symbol["name"]))
        for rows in marks.values():
            rows.sort()
        return marks

    def functions(self):
        """(name, section, offset, size, thumb, bind) for named functions in executable sections.

        Metrowerks marks Thumb code only with $t mapping symbols, so the mode comes
        from the nearest code mapping symbol at or before each function."""
        marks = self.mapping_modes()
        out = []
        for symbol in self.symbols:
            shndx = symbol["shndx"]
            if (symbol["type"] not in (STT_FUNC, STT_ARM_TFUNC) or not symbol["name"] or symbol["name"].startswith("$")
                    or not 0 < shndx < len(self.sections) or not self.sections[shndx]["flags"] & SHF_EXECINSTR):
                continue
            offset = symbol["value"] & ~1
            out.append((symbol["name"], shndx, offset, symbol["size"],
                        mode_at(marks.get(shndx, ()), offset) == "$t" or bool(symbol["value"] & 1), symbol["bind"]))
        return out


def mode_at(marks, offset):
    """The code mode ($a or $t) of the last code mapping symbol at or before offset."""
    mode = "$a"
    for at, name in marks:
        if at > offset:
            break
        if name != "$d":
            mode = name
    return mode


def thumb_bl_target(halves, at):
    """Decode a Thumb BL/BLX pair at address at; None if it is not one."""
    hi, lo = struct.unpack("<HH", halves)
    if (hi & 0xF800) != 0xF000 or (lo & 0xE800) != 0xE800:
        return None
    offset = ((hi & 0x7FF) << 12) | ((lo & 0x7FF) << 1)
    if offset & 0x400000:
        offset -= 0x800000
    target = at + 4 + offset
    return ("bl", target | 1) if (lo & 0xF800) == 0xF800 else ("blx", target & ~3)


def arm_branch_target(word, at):
    """Decode an ARM B/BL at address at; None if it is not one."""
    if (word >> 25) & 7 != 5:
        return None
    offset = (word & 0xFFFFFF) << 2
    if offset & 0x2000000:
        offset -= 0x4000000
    return at + 8 + offset


def arm_call(word, at):
    """("bl", ARM target), ("blx", Thumb target) or ("b", target) for an ARM branch, else None."""
    target = arm_branch_target(word, at)
    if target is None:
        return None
    if word >> 28 == 0xF:
        return "blx", (target + ((word >> 23) & 2)) | 1
    return ("bl" if word & 0x01000000 else "b"), target


def with_inline_addends(data):
    """A copy of a Metrowerks object for GNU ld, which reads ARM addends from the field.

    Metrowerks keeps addends in RELA rows and marks Thumb code only with $t, so each
    Thumb function becomes STT_ARM_TFUNC, mapping symbols become NOTYPE, and each
    code addend is written into its field."""
    data = bytearray(data)
    obj = Elf(bytes(data))
    symtab = next(section for section in obj.sections if section["type"] == SHT_SYMTAB)
    marks = obj.mapping_modes()
    for k, symbol in enumerate(obj.symbols):
        info = symtab["off"] + 16 * k + 12
        if symbol["name"] in MAPPING:
            data[info] = symbol["bind"] << 4 | STT_NOTYPE
        elif symbol["type"] == STT_FUNC and mode_at(marks.get(symbol["shndx"], ()), symbol["value"]) == "$t":
            data[info] = symbol["bind"] << 4 | STT_ARM_TFUNC
    for shndx, rows in obj.relocs.items():
        section = obj.sections[shndx]
        if section["name"].startswith(".debug") or section["name"].startswith(".rel"):
            continue
        for offset, kind, _, addend in rows:
            at = section["off"] + offset
            if kind == R_ARM_ABS32:
                struct.pack_into("<i", data, at, addend)
            elif kind == R_ARM_THM_CALL:
                struct.pack_into("<HH", data, at, *thumb_call_field(struct.unpack_from("<H", data, at + 2)[0], addend))
            else:
                raise ElfError(f"relocation type {kind} needs an in-place addend")
    return bytes(data)


def numbered_sections(data):
    """A copy whose .text, .rodata, .data and .bss sections are named .text.N and so on.

    Each section index N then names exactly one input section in a linker map,
    even after --gc-sections drops some of them. The new names go in a fresh
    section name table at the end of the file."""
    obj = Elf(data)
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, _, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    table = bytearray(obj.section_bytes(shstrndx))
    out = bytearray(data)
    for i, section in enumerate(obj.sections):
        if section["name"] in (".text", ".rodata", ".data", ".bss"):
            name = f"{section['name']}.{i}".encode()
            struct.pack_into("<I", out, shoff + i * shentsize, len(table))
            table += name + b"\0"
    while len(out) % 4:
        out.append(0)
    struct.pack_into("<II", out, shoff + shstrndx * shentsize + 16, len(out), len(table))
    return bytes(out + table)


def thumb_call_field(low_half, addend):
    """The BL pair encoding addend, keeping the second half's BL/BLX opcode bits."""
    value = addend & 0x7FFFFF
    return 0xF000 | ((value >> 12) & 0x7FF), (low_half & 0xF800) | ((value >> 1) & 0x7FF)


def set_eabi_flags(data, flags=MWCC_EABI_FLAGS):
    data = bytearray(data)
    data[0x24:0x28] = flags.to_bytes(4, "little")
    return bytes(data)
