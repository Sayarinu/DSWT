"""Link Wonder Trade objects at a fixed address against a retail game's symbol addresses."""

import collections
from pathlib import Path
import re
import struct

from . import elf


class LinkError(ValueError):
    pass


def defined_and_needed(objects):
    """(names defined, names used but not defined) across ELF objects."""
    defined, used = set(), set()
    for data in objects:
        for symbol in elf.Elf(data).symbols:
            if symbol["name"] and symbol["bind"]:
                (used if symbol["shndx"] == 0 else defined).add(symbol["name"])
    return defined, used - defined


def symbol_source(addresses, functions):
    """GNU as source defining each external name at its address; functions get a code type."""
    lines = [".syntax unified"]
    for name, address in sorted(addresses.items()):
        lines += [f".global {name}"]
        if name in functions:
            lines.append(f".type {name}, %function")
            lines.append(f".thumb_set {name}, {address:#x}" if address & 1 else f".set {name}, {address:#x}")
        else:
            lines.append(f".set {name}, {address:#x}")
    return "\n".join(lines) + "\n"


def linker_script(base, first):
    """Place the replaced upstream files first, then everything else; BSS follows the loaded image."""
    keep_text = " ".join(f"KEEP(*{name}(.text.*))" for name in first)
    keep_rodata = " ".join(f"KEEP(*{name}(.rodata.*))" for name in first)
    return f"""SECTIONS {{
  . = {base:#x};
  .text : {{ {keep_text} *(.text) *(.text.*) }}
  .rodata : ALIGN(4) {{ {keep_rodata} *(.rodata) *(.rodata.*) }}
  .data : ALIGN(4) {{ *(.data) *(.data.*) }}
  . = ALIGN(32);
  __blob_load_end = .;
  .bss (NOLOAD) : ALIGN(32) {{ *(.bss) *(.bss.*) *(COMMON) }}
  . = ALIGN(32);
  __blob_end = .;
  /DISCARD/ : {{ *(.debug*) *(.comment) *(.rel.debug*) *(.rela.debug*) }}
}}
"""


SECTION_LINE = re.compile(r"^ (\.[\w.]+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*\S)\s*$")


def placements(map_text):
    """{(object path, section name): [address, ...]} of input sections in a GNU ld map, in map order."""
    text = map_text[map_text.index("Linker script and memory map"):]
    found = collections.defaultdict(list)
    pending = None
    for line in text.splitlines():
        if re.match(r"^ \.[\w.]+$", line):
            pending = line.strip()
            continue
        match = SECTION_LINE.match(line)
        if match and (match.group(1) or pending) and int(match.group(3), 16):
            name = match.group(1) or pending
            if not match.group(4).startswith("*"):
                found[(match.group(4), name)].append((int(match.group(2), 16), int(match.group(3), 16)))
        pending = None
    return found


def nm_symbols(nm_text):
    return {parts[2]: int(parts[0], 16) for parts in (line.split() for line in nm_text.splitlines()) if len(parts) == 3}


class Linked:
    """A linked blob: its loaded bytes, symbol values and where each input section went."""

    def __init__(self, base, binary, nm_text, map_text, objects):
        self.base, self.binary = base, binary
        self.symbols = nm_symbols(nm_text)
        self.sections = placements(map_text)
        self.objects = objects  # path -> Elf of each linked input

    def section_address(self, path, obj, shndx):
        """Address of an input section named by numbered_sections, or None if it was not linked."""
        placed = self.sections.get((path, obj.sections[shndx]["name"]), [])
        if len(placed) > 1:
            raise LinkError(f"{path}: {obj.sections[shndx]['name']} was placed twice")
        if placed and placed[0][1] != obj.sections[shndx]["size"]:
            raise LinkError(f"{path}: {obj.sections[shndx]['name']} changed size when linked")
        return placed[0][0] if placed else None

    def function_addresses(self, path):
        """{name: address with Thumb bit} for each function of one linked object."""
        obj = self.objects[path]
        out = {}
        for name, shndx, offset, _, thumb, _ in obj.functions():
            at = self.section_address(path, obj, shndx)
            if at is None:
                raise LinkError(f"{path}: {name} was not linked")
            out[name] = (at + offset) | int(thumb)
        return out

    def word(self, address):
        return self.binary[address - self.base:address - self.base + 4]


def thumb_functions(objects):
    """Names of the Thumb functions the objects define."""
    found = set()
    for obj in objects:
        marks = obj.mapping_modes()
        for symbol in obj.symbols:
            if (symbol["bind"] and symbol["name"] and 0 < symbol["shndx"] < elf.SHN_LORESERVE
                    and (symbol["type"] == elf.STT_ARM_TFUNC or symbol["type"] == elf.STT_FUNC and (
                        symbol["value"] & 1 or elf.mode_at(marks.get(symbol["shndx"], ()), symbol["value"]) == "$t"))):
                found.add(symbol["name"])
    return found


def check_relocations(linked, resolved):
    """Every relocated field in the blob points where its symbol resolved. Returns the problems."""
    problems = []
    thumb = thumb_functions(linked.objects.values())
    for path, obj in linked.objects.items():
        for shndx, rows in obj.relocs.items():
            section = obj.sections[shndx]
            if section["name"].startswith((".debug", ".rel", ".comment")) or not rows:
                continue
            at = linked.section_address(path, obj, shndx)
            if at is None:
                continue  # discarded by --gc-sections
            for offset, kind, index, addend in rows:
                symbol = obj.symbols[index]
                if symbol["shndx"] == 0 or (symbol["bind"] and symbol["name"] in linked.symbols):
                    name = symbol["name"]
                    if name in resolved:
                        value = resolved[name]
                    elif name in linked.symbols:
                        value = linked.symbols[name] | int(name in thumb)
                    else:
                        problems.append(f"{path}: {name} is unresolved")
                        continue
                else:
                    base = linked.section_address(path, obj, symbol["shndx"])
                    if base is None:
                        problems.append(f"{path}: reference into a discarded section")
                        continue
                    value = base + (symbol["value"] if symbol["type"] != elf.STT_SECTION else 0)
                    if symbol["type"] in (elf.STT_FUNC, elf.STT_ARM_TFUNC):
                        value = base + (symbol["value"] & ~1)
                        modes = obj.mapping_modes().get(symbol["shndx"], ())
                        value |= int(elf.mode_at(modes, symbol["value"] & ~1) == "$t")
                    name = symbol["name"] or section["name"]
                site = at + offset
                word = linked.word(site)
                if shndx in obj.inline_addends and kind == elf.R_ARM_ABS32:
                    addend = struct.unpack_from("<i", obj.section_bytes(shndx), offset)[0]
                if kind == elf.R_ARM_ABS32:
                    got = struct.unpack("<I", word)[0]
                    if got != (value + addend) & 0xFFFFFFFF:
                        problems.append(f"{path}+{offset:#x}: word {got:#x}, expected {name}{addend:+#x} = {value + addend:#x}")
                elif kind == elf.R_ARM_THM_CALL:
                    decoded = elf.thumb_bl_target(word, site)
                    expected = ("bl", value) if value & 1 else ("blx", value & ~3)
                    if decoded is None or (decoded[0], decoded[1]) != expected:
                        problems.append(f"{path}+{offset:#x}: call {decoded}, expected {name} at {value:#x}")
                elif kind in elf.ARM_BRANCHES:
                    decoded = elf.arm_call(struct.unpack("<I", word)[0], site)
                    if decoded is None or decoded[1] != value or (decoded[0] == "blx") != bool(value & 1):
                        problems.append(f"{path}+{offset:#x}: branch {decoded}, expected {name} at {value:#x}")
                else:
                    problems.append(f"{path}+{offset:#x}: unchecked relocation type {kind}")
    return problems
