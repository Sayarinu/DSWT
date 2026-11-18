"""Map symbols of the pristine US build onto another retail build of the same game.

Functions are anchored where their bytes, with relocated fields masked, occur
exactly once in the target module. Calls and data references inside anchored
functions then place what they point at, and a function between two placed
neighbours is placed at its US distance if its bytes agree.
"""

import bisect
import collections
import re
import struct

from . import elf, nds

FUNCTION_KINDS = (elf.STT_FUNC, elf.STT_ARM_TFUNC)
MAPPED_KINDS = FUNCTION_KINDS + (elf.STT_OBJECT,)
CALLS = {elf.R_ARM_THM_CALL: "thumb", elf.R_ARM_PC24: "arm", elf.R_ARM_CALL: "arm", elf.R_ARM_JUMP24: "arm"}


class Symbol(collections.namedtuple("Symbol", "num address size kind section name")):
    """A defined symbol of the US image. address keeps the Thumb bit as the ELF stores it."""

    @property
    def start(self):
        return self.address & ~1


class UsImage:
    """Symbols, sections and relocations of the linked pristine US image (main.nef)."""

    def __init__(self, data, rom):
        image = elf.Elf(data)
        self.secname = {i: section["name"] for i, section in enumerate(image.sections)}
        self.secmod = self._modules(image, rom)
        marks = image.mapping_modes()
        self.syms, self.thumb = [], set()
        for num, raw in enumerate(image.symbols):
            # $b marks a branch target inside a function (Metrowerks); it is neither a symbol nor a mode.
            if not raw["name"] or raw["name"] in (*elf.MAPPING, "$b") or not 0 < raw["shndx"] < elf.SHN_LORESERVE:
                continue
            if raw["type"] in (elf.STT_FUNC, elf.STT_OBJECT, elf.STT_ARM_TFUNC, elf.STT_NOTYPE):
                symbol = Symbol(num, raw["value"], raw["size"], raw["type"], raw["shndx"], raw["name"])
                self.syms.append(symbol)
                if self.is_code(symbol) and elf.mode_at(marks.get(symbol.section, ()), symbol.start) == "$t":
                    self.thumb.add(num)
        self.byname = collections.defaultdict(list)
        for symbol in self.syms:
            self.byname[symbol.name].append(symbol)
        self.absolute = {raw["name"]: raw["value"] for raw in image.symbols
                         if raw["shndx"] == 0xFFF1 and raw["name"]}
        names = {i: raw for i, raw in enumerate(image.symbols)}
        # (section name, address) -> [(type, symbol name, symbol value, addend)]
        self.relocs = collections.defaultdict(list)
        for shndx, rows in image.relocs.items():
            target = self.secname[shndx]
            for offset, kind, index, addend in rows:
                symbol = names[index]
                name = symbol["name"] or self.secname.get(symbol["shndx"], "")
                self.relocs[(target, offset)].append((kind, name, symbol["value"], addend))
        self._starts = {}

    @staticmethod
    def _modules(image, rom):
        """Section index -> "arm9" or overlay ID, checked against the US cartridge's overlay table.

        After main (arm9 in the make builds), the TCMs and the link tables, overlay sections follow overlay ID order."""
        names = [(i, section["name"]) for i, section in enumerate(image.sections)]
        start = next(i for i, name in names if name in ("main_table", "arm9_table"))  # meson, make
        order = []
        for i, name in names:
            if i > start and name and not name.startswith(".") and "." not in name and name not in order:
                order.append(name)
        found = {i: "arm9" for i, name in names if name in ("main", "arm9")}
        found.update({i: order.index(name) for i, name in names if name in order})
        table = {row[0]: row for row in nds.overlays(rom)}
        if len(order) != len(table):
            raise ValueError(f"US image has {len(order)} overlay sections, the cartridge {len(table)}")
        for module, name in enumerate(order):
            first = next(image.sections[i] for i, other in names if other == name)
            if first["addr"] != table[module][1]:
                raise ValueError(f"US overlay section {name} does not load at overlay {module}'s address")
        return found

    def is_code(self, symbol):
        return symbol.kind in FUNCTION_KINDS or (symbol.kind == elf.STT_NOTYPE and symbol.section in self.secmod)

    def module(self, symbol):
        return self.secmod.get(symbol.section)

    def unique(self, name):
        found = self.byname.get(name, [])
        return found[0] if len(found) == 1 else None

    def extent(self, symbol):
        """A symbol's size, or for an unsized one the distance to the next symbol in its section."""
        if symbol.size:
            return symbol.size
        if symbol.section not in self._starts:
            self._starts[symbol.section] = sorted({s.start for s in self.syms if s.section == symbol.section})
        starts = self._starts[symbol.section]
        k = bisect.bisect_right(starts, symbol.start)
        return starts[k] - symbol.start if k < len(starts) else 0


def masked(image, modules, symbol, size=None):
    """(module, US address, bytes, keep mask) of a symbol's bytes, relocated fields masked."""
    module = image.module(symbol)
    if module is None or module not in modules:
        return None
    base, body = modules[module]
    size = size if size is not None else symbol.size
    if not size or not 0 <= symbol.start - base <= len(body) - size:
        return None
    data = body[symbol.start - base:symbol.start - base + size]
    section = image.secname[symbol.section]
    keep = [True] * size
    for k in range(size):
        if (section, symbol.start + k) in image.relocs:
            for j in range(k, min(k + 4, size)):
                keep[j] = False
    return module, symbol.start, data, keep


def pattern(data, keep):
    return b"".join(re.escape(data[i:i + 1]) if keep[i] else b"." for i in range(len(data)))


def unique_match(modules, module, data, keep, step=2):
    """The single address in a module where data occurs with masked bytes ignored, else None."""
    base, body = modules[module]
    hits = []
    for match in re.finditer(pattern(data, keep), body, re.S):
        if match.start() % step == 0:
            hits.append(base + match.start())
            if len(hits) > 1:
                return None
    return hits[0] if hits else None


class SymbolMap:
    """US symbol number -> target address (Thumb bit kept), with how each was placed."""

    def __init__(self, image, us_rom, target_rom):
        self.us = image
        self.us_modules, self.modules = nds.modules(us_rom), nds.modules(target_rom)
        self.addr, self.how = {}, {}
        self._bynum = {symbol.num: symbol for symbol in image.syms}
        self._anchor()
        self._propagate()
        self._neighbours()

    def _size(self, symbol):
        """A symbol's size; code from assembly records none, so it runs to the next symbol."""
        if symbol.size:
            return symbol.size
        return self.us.extent(symbol) if symbol.kind in FUNCTION_KINDS else 0

    def _masked(self, symbol):
        if symbol.kind not in MAPPED_KINDS:
            return None
        got = masked(self.us, self.us_modules, symbol, self._size(symbol))
        if got is None or got[0] not in self.modules:
            return None
        return got

    def _anchor(self):
        for symbol in self.us.syms:
            if symbol.kind not in FUNCTION_KINDS or self._size(symbol) < 16:
                continue
            got = self._masked(symbol)
            if got is None:
                continue
            module, _, data, keep = got
            hit = unique_match(self.modules, module, data, keep)
            if hit is not None:
                self.addr[symbol.num] = hit | (symbol.address & 1)
                self.how[symbol.num] = "bytes"
        # Two different US functions can never share an address: when byte matching puts two there
        # (short functions that differ only in what they call), neither placement is trusted.
        seen = collections.defaultdict(list)
        for num, at in self.addr.items():
            seen[(self.us.module(self._bynum[num]), at & ~1)].append(num)  # overlays share addresses
        for nums in seen.values():
            if len({self._bynum[num].start for num in nums}) > 1:  # aliases of one function may share
                for num in nums:
                    del self.addr[num], self.how[num]

    def matches(self, symbol, address):
        got = self._masked(symbol)
        if got is None:
            return False
        module, _, data, keep = got
        base, body = self.modules[module]
        offset = (address & ~1) - base
        if not 0 <= offset <= len(body) - len(data):
            return False
        return all(not keep[i] or body[offset + i] == data[i] for i in range(len(data)))

    def differing(self, symbol, address):
        """How many unrelocated bytes of a US function differ from the target at an address."""
        got = self._masked(symbol)
        if got is None:
            return None
        module, _, data, keep = got
        base, body = self.modules[module]
        offset = (address & ~1) - base
        if not 0 <= offset <= len(body) - len(data):
            return None
        return sum(1 for i in range(len(data)) if keep[i] and body[offset + i] != data[i])

    def voted(self, symbol):
        """The address most calls and references from placed functions agree on, or None if tied."""
        counter = self.votes.get(symbol.num)
        if not counter:
            return None
        (best, count), *rest = counter.most_common(2)
        return None if rest and count <= rest[0][1] else best

    def _propagate(self):
        votes = collections.defaultdict(collections.Counter)
        frontier = list(self.addr)
        while frontier:
            for num in frontier:
                symbol = self._bynum[num]
                got = self._masked(symbol)
                if got is None or symbol.kind not in FUNCTION_KINDS:
                    continue
                module, start, data, _ = got
                section = self.us.secname[symbol.section]
                base, body = self.modules[module]
                placed = self.addr[num] & ~1
                for k in range(len(data)):
                    for kind, name, value, addend in self.us.relocs.get((section, start + k), ()):
                        candidates = [c for c in self.us.byname.get(name, ()) if c.address == value]
                        if len(candidates) != 1:
                            continue
                        offset = placed - base + k
                        word = body[offset:offset + 4]
                        if len(word) < 4:
                            continue
                        target = reference_target(kind, word, placed + k, addend)
                        if target is not None:
                            votes[candidates[0].num][target] += 1
            frontier = []
            for num, counter in votes.items():
                if num in self.addr:
                    continue
                (best, count), *rest = counter.most_common(2)
                if rest and count <= rest[0][1]:
                    continue
                symbol = self._bynum[num]
                if symbol.kind in FUNCTION_KINDS and self._masked(symbol) is not None:
                    if not self.matches(symbol, best):
                        continue
                    self.how[num] = "calls+bytes"
                    frontier.append(num)
                else:
                    self.how[num] = f"refs x{count}"
                self.addr[num] = best
        self.votes = votes

    def _neighbours(self):
        order = collections.defaultdict(list)
        for symbol in self.us.syms:
            if self._masked(symbol) is not None:
                order[symbol.section].append(symbol)
        for rows in order.values():
            rows.sort(key=lambda symbol: symbol.address)
        changed = True
        while changed:
            changed = False
            for rows in order.values():
                for i, symbol in enumerate(rows):
                    if symbol.num in self.addr or symbol.size == 0:
                        continue
                    for j in (i - 1, i + 1, i - 2, i + 2):
                        if 0 <= j < len(rows) and rows[j].num in self.addr:
                            other = rows[j]
                            guess = ((self.addr[other.num] & ~1) + (symbol.start - other.start)) | (symbol.address & 1)
                            if self.matches(symbol, guess):
                                self.addr[symbol.num] = guess
                                self.how[symbol.num] = "neighbour"
                                changed = True
                            break


def reference_target(kind, word, at, addend):
    """Where a relocated field at address at points, given its relocation."""
    if kind == elf.R_ARM_THM_CALL:
        decoded = elf.thumb_bl_target(word, at)
        return decoded[1] if decoded else None
    if kind in elf.ARM_BRANCHES:
        return elf.arm_branch_target(struct.unpack("<I", word)[0], at)
    if kind == elf.R_ARM_ABS32:
        return struct.unpack("<I", word)[0] - addend
    return None
