"""Copy SDK routines that the retail game never linked out of the SDK archives, as GNU as source."""

from pathlib import Path
import struct

from . import elf

NAMES = {elf.R_ARM_ABS32: "R_ARM_ABS32", elf.R_ARM_THM_CALL: "R_ARM_THM_CALL", elf.R_ARM_PC24: "R_ARM_PC24",
         elf.R_ARM_CALL: "R_ARM_CALL", elf.R_ARM_JUMP24: "R_ARM_JUMP24", elf.R_ARM_REL32: "R_ARM_REL32"}


def members(path):
    """(name, bytes) of each member of an ar archive."""
    data = Path(path).read_bytes()
    if data[:8] != b"!<arch>\n":
        raise ValueError(f"{path} is not an ar archive")
    position, long_names = 8, b""
    while position < len(data):
        name = data[position:position + 16].decode("latin-1").rstrip()
        size = int(data[position + 48:position + 58])
        body = data[position + 60:position + 60 + size]
        position += 60 + size + (size & 1)
        if name == "//":
            long_names = body
            continue
        if name in ("/", "/SYM64/", "__.SYMDEF"):
            continue
        if name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            name = long_names[start:long_names.index(b"/\n", start)].decode("latin-1")
        yield name.rstrip("/"), body


class Library:
    """Global definitions across SDK archives and loose objects; the first to define a name wins.

    The Diamond and Pearl build compiles the SDK into separate objects instead of archives."""

    def __init__(self, paths):
        self.objects, self.globals = [], {}
        for path in paths:
            data = Path(path).read_bytes()
            for _, body in ([(Path(path).name, data)] if data[:4] == b"\x7fELF" else members(path)):
                if body[:4] != b"\x7fELF":
                    continue
                obj = elf.Elf(body)
                self.objects.append(obj)
                for k, symbol in enumerate(obj.symbols):
                    if symbol["name"] and symbol["bind"] and 0 < symbol["shndx"] < elf.SHN_LORESERVE:
                        self.globals.setdefault(symbol["name"], (obj, k))

    def extract(self, wanted, external):
        """Assembler source defining wanted and everything it needs, leaving names in external undefined."""
        todo = [self.globals[name] for name in wanted]
        done, out, tags = set(), [], {}

        def section_label(obj, shndx):
            tag = tags.setdefault(id(obj), len(tags))
            return f"Lsdk{tag}_{shndx}"

        while todo:
            obj, k = todo.pop()
            shndx = obj.symbols[k]["shndx"]
            if (id(obj), shndx) in done:
                continue
            done.add((id(obj), shndx))
            section = obj.sections[shndx]
            body = bytearray(obj.section_bytes(shndx))
            flags = "ax" if section["flags"] & elf.SHF_EXECINSTR else ("aw" if section["flags"] & elf.SHF_WRITE else "a")
            kind = (".bss" if section["type"] == elf.SHT_NOBITS else
                    ".text" if "x" in flags else ".data" if "w" in flags else ".rodata")
            base = section_label(obj, shndx)
            lines = [f'.section {kind}.sdk{len(out)},"{flags}"' + (",%nobits" if section["type"] == elf.SHT_NOBITS else ""),
                     ".balign 4", f"{base}:"]
            labels = {}
            for symbol in obj.symbols:
                if symbol["shndx"] == shndx and symbol["name"] and symbol["name"][0] not in "$.":
                    labels.setdefault(symbol["value"] & ~1, []).append(symbol)
            relocations = []
            for offset, kind_code, index, addend in obj.relocs.get(shndx, ()):
                symbol = obj.symbols[index]
                if symbol["shndx"] == 0:
                    target = symbol["name"]
                    if target not in external and target in self.globals:
                        todo.append(self.globals[target])
                elif symbol["bind"] and symbol["name"] in external:
                    target = symbol["name"]
                else:
                    todo.append((obj, index))
                    target = section_label(obj, symbol["shndx"])
                    if symbol["type"] != elf.STT_SECTION:
                        if symbol["bind"]:
                            target = symbol["name"]
                        else:
                            addend += symbol["value"] & ~1
                if kind_code in (elf.R_ARM_ABS32, elf.R_ARM_REL32):
                    struct.pack_into("<i", body, offset, addend)
                elif kind_code == elf.R_ARM_THM_CALL:
                    struct.pack_into("<HH", body, offset, *elf.thumb_call_field(struct.unpack_from("<H", body, offset + 2)[0], addend))
                elif kind_code in elf.ARM_BRANCHES:
                    word = struct.unpack_from("<I", body, offset)[0]
                    struct.pack_into("<I", body, offset, (word & 0xFF000000) | ((addend >> 2) & 0xFFFFFF))
                else:
                    raise ValueError(f"unsupported relocation {kind_code} in an SDK routine")
                relocations.append(f".reloc {base}+{offset}, {NAMES[kind_code]}, {target}")
            if section["type"] == elf.SHT_NOBITS:
                lines.append(f".space {len(body)}")
            for position in range(len(body)):
                for symbol in labels.get(position, ()):
                    if not symbol["bind"]:
                        continue
                    code = symbol["type"] == elf.STT_FUNC or "x" in flags
                    lines.append(f".global {symbol['name']}")
                    # Metrowerks marks Thumb code only with $t mapping symbols, not the address.
                    thumb = symbol["value"] & 1 or elf.mode_at(obj.mapping_modes().get(shndx, ()), position) == "$t"
                    if symbol["type"] == elf.STT_FUNC and thumb:
                        lines += [".thumb", ".thumb_func"]
                    elif code:
                        lines.append(".arm")
                    lines += [f".type {symbol['name']}, %{'function' if code else 'object'}", f"{symbol['name']}:"]
                if section["type"] != elf.SHT_NOBITS:
                    lines.append(f".byte {body[position]}")
            out.append("\n".join(lines + relocations))
        return ".syntax unified\n" + "\n".join(out) + "\n"
