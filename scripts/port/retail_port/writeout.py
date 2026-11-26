"""Write a retail Thumb file out as assembly, from the player's own dump, named after its US twin.

Used for a file the decompilation keeps as assembly (the US compiler's output) when a language
compiled it with another compiler: the functions are the same, in the same file, but their bytes
differ. The text is made at port time from the cartridge, so no retail code is carried in this
repository, and it must assemble back to the cartridge's bytes (the port checks that).

- The language's functions are found between the addresses the game's own tables give for some
  of them, and lined up with the US functions in reverse order (Metrowerks 1.2 emits a file's
  functions last first).
- A call is named after the US function's call in the same place, when both make the same number
  of calls; an address literal likewise. Calls into the file use the file's names. Any name the
  symbol map already placed must agree.
- Labels carry the language's addresses; instructions use the decompilation's syntax.
"""

import collections
import struct

from . import elf

from . import nds

THUMB_ADD_SUB = 3
THUMB_ALU = 0x10
THUMB_HIGH_REGISTER = 0x11
HIGH_BRANCH_EXCHANGE = 3
THUMB_REGISTER_TRANSFER = 5
THUMB_IMMEDIATE_TRANSFER = 3
THUMB_HALFWORD_TRANSFER = 8
THUMB_SP_TRANSFER = 9
THUMB_LOAD_ADDRESS = 0xA
THUMB_ADJUST_SP = 0xB0
THUMB_PUSH = 0x5A
THUMB_POP = 0x5E
THUMB_MULTIPLE_TRANSFER = 0xC
THUMB_CONDITIONAL_BRANCH = 0xD
FIRST_RESERVED_CONDITION = 14
THUMB_BRANCH = 0x1C
THUMB_PUSH_ENCODING = 0xB400
THUMB_LITERAL_LOAD = 0x09
THUMB_CMP_IMMEDIATE = 0x05
THUMB_ADD_PC_ENCODING = 0x4487


CONDITIONS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
ALU = ["and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn"]
LOADS = ["str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"]


class WriteOutError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise WriteOutError(message)


def _registers(mask, extra=None):
    """A register list as the decompilation writes it, such as {r3-r5, lr}."""
    regs = [i for i in range(8) if mask >> i & 1]
    parts, i = [], 0
    while i < len(regs):
        j = i
        while j + 1 < len(regs) and regs[j + 1] == regs[j] + 1:
            j += 1
        parts.append(f"r{regs[i]}" if i == j else f"r{regs[i]}-r{regs[j]}")
        i = j + 1
    if extra:
        parts.append(extra)
    return "{" + ", ".join(parts) + "}"


def _high(r):
    return {13: "sp", 14: "lr", 15: "pc"}.get(r, f"r{r}")


def instruction(x, at, label):
    """One 16-bit Thumb instruction (not BL, a literal load or a switch) as text; label(address)
    names a branch target. None if it is not one the decompilation's syntax writes."""
    if x >> 13 == 0 and x >> 11 != THUMB_ADD_SUB:
        return f"{['lsl', 'lsr', 'asr'][x >> 11]} r{x & 7}, r{x >> 3 & 7}, #0x{x >> 6 & 31:x}"
    if x >> 11 == THUMB_ADD_SUB:
        operand = f"#0x{x >> 6 & 7:x}" if x >> 10 & 1 else f"r{x >> 6 & 7}"
        return f"{'sub' if x >> 9 & 1 else 'add'} r{x & 7}, r{x >> 3 & 7}, {operand}"
    if x >> 13 == 1:
        return f"{['mov', 'cmp', 'add', 'sub'][x >> 11 & 3]} r{x >> 8 & 7}, #0x{x & 0xFF:x}"
    if x >> 10 == THUMB_ALU:
        return f"{ALU[x >> 6 & 15]} r{x & 7}, r{x >> 3 & 7}"
    if x >> 10 == THUMB_HIGH_REGISTER:
        op, rd, rs = x >> 8 & 3, (x & 7) | (x >> 4 & 8), x >> 3 & 15
        if op == HIGH_BRANCH_EXCHANGE:
            return ("blx " if x & 0x80 else "bx ") + _high(rs)
        return f"{['add', 'cmp', 'mov'][op]} {_high(rd)}, {_high(rs)}"
    if x >> 12 == THUMB_REGISTER_TRANSFER:
        return f"{LOADS[x >> 9 & 7]} r{x & 7}, [r{x >> 3 & 7}, r{x >> 6 & 7}]"
    if x >> 13 == THUMB_IMMEDIATE_TRANSFER:
        byte, load = x >> 12 & 1, x >> 11 & 1
        return f"{('ldr' if load else 'str') + ('b' if byte else '')} r{x & 7}, [r{x >> 3 & 7}, #0x{(x >> 6 & 31) * (1 if byte else 4):x}]"
    if x >> 12 == THUMB_HALFWORD_TRANSFER:
        return f"{'ldrh' if x >> 11 & 1 else 'strh'} r{x & 7}, [r{x >> 3 & 7}, #0x{(x >> 6 & 31) * 2:x}]"
    if x >> 12 == THUMB_SP_TRANSFER:
        return f"{'ldr' if x >> 11 & 1 else 'str'} r{x >> 8 & 7}, [sp, #0x{(x & 0xFF) * 4:x}]"
    if x >> 12 == THUMB_LOAD_ADDRESS:
        return f"add r{x >> 8 & 7}, {'sp' if x >> 11 & 1 else 'pc'}, #0x{(x & 0xFF) * 4:x}"
    if x >> 8 == THUMB_ADJUST_SP:
        return f"{'sub' if x & 0x80 else 'add'} sp, #0x{(x & 0x7F) * 4:x}"
    if x >> 9 == THUMB_PUSH:
        return "push " + _registers(x & 0xFF, "lr" if x & 0x100 else None)
    if x >> 9 == THUMB_POP:
        return "pop " + _registers(x & 0xFF, "pc" if x & 0x100 else None)
    if x >> 12 == THUMB_MULTIPLE_TRANSFER:
        return f"{'ldmia' if x >> 11 & 1 else 'stmia'} r{x >> 8 & 7}!, {_registers(x & 0xFF)}"
    if x >> 12 == THUMB_CONDITIONAL_BRANCH and x >> 8 & 15 < FIRST_RESERVED_CONDITION:
        offset = (x & 0xFF) - (0x100 if x & 0x80 else 0)
        return f"b{CONDITIONS[x >> 8 & 15]} {label(at + 4 + offset * 2)}"
    if x >> 11 == THUMB_BRANCH:
        offset = (x & 0x7FF) - (0x800 if x & 0x400 else 0)
        return f"b {label(at + 4 + offset * 2)}"
    return None


class Module:
    """A retail module's code: (RAM base, bytes)."""

    def __init__(self, base, body):
        self.base, self.body = base, body

    def half(self, at):
        return struct.unpack_from("<H", self.body, at - self.base)[0]

    def word(self, at):
        return struct.unpack_from("<I", self.body, at - self.base)[0]

    def call(self, at):
        """The target of a BL or BLX at at, or None."""
        decoded = elf.thumb_bl_target(self.body[at - self.base:at - self.base + 4], at)
        return None if decoded is None else decoded[1] & ~1

    def function_starts(self, low, high):
        """Addresses in [low, high) that something calls or points to, or that open with a push of lr."""
        found = set()
        for k in range(0, len(self.body) - 3, 2):
            target = self.call(self.base + k)
            if target is not None and low <= target < high:
                found.add(target)
        for k in range(0, len(self.body) - 3, 4):
            word = struct.unpack_from("<I", self.body, k)[0]
            if word & 1 and low <= word - 1 < high:
                found.add(word - 1)
        for at in range(low & ~3, high, 4):
            x = self.half(at)
            if x & 0xFE00 == THUMB_PUSH_ENCODING and x & 0x100:
                found.add(at)
        return sorted(found)


def write_out(us, us_source, module, rodata_at, placed, header=""):
    """Assembly for the language's copy of one US assembly file.

    us: the US object (elf.Elf, functions sized); us_source: its source text, whose part before
    .text (externs and tables) is kept; module: the language's Module; rodata_at: where the
    file's .rodata sits there; placed: address -> names the symbol map already put there."""
    text = next(i for i, s in enumerate(us.sections) if s["name"] == ".text")
    ro = next(i for i, s in enumerate(us.sections) if s["name"] == ".rodata")
    functions = sorted((f for f in us.functions() if f[1] == text), key=lambda f: f[2])
    names = [f[0] for f in functions]
    own = set(names)
    relocations = collections.defaultdict(list)
    for offset, kind, index, _ in us.relocs.get(text, ()):
        relocations[offset].append((kind, us.symbols[index]["name"]))
    ro_size = us.sections[ro]["size"]
    ro_labels = sorted((s["value"], s["name"]) for s in us.symbols
                       if s["shndx"] == ro and s["name"] and not s["name"].startswith((".", "$")))

    # Anchors: the game's tables point at some of the file's functions.
    anchors = {}
    starts_us = {f[2]: i for i, f in enumerate(functions)}
    for offset, kind, index, addend in us.relocs.get(ro, ()):
        symbol = us.symbols[index]
        if kind != elf.R_ARM_ABS32:
            continue
        if symbol["name"] in own:
            anchors[names.index(symbol["name"])] = (module.word(rodata_at + offset) - addend) & ~1
        elif symbol["shndx"] == text and symbol["type"] == elf.STT_SECTION and (addend & ~1) in starts_us:
            anchors[starts_us[addend & ~1]] = module.word(rodata_at + offset) & ~1
    _require(anchors, "the file's tables point at none of its functions")
    reach = 2 * sum(f[3] for f in functions)
    starts = module.function_starts(min(anchors.values()) - reach, max(anchors.values()) + reach)
    ends = {starts.index(at) + k for k, at in anchors.items() if at in starts}
    _require(len(anchors) == len({at for at in anchors.values()}) and len(ends) == 1
             and all(at in starts for at in anchors.values()),
             "the file's functions do not line up in reverse order with its tables")
    last = ends.pop()  # the index of the US first function, which the language put last
    _require(last - len(names) + 1 >= 0 and last + 1 < len(starts), "the file runs past the functions found")
    order = starts[last - len(names) + 1:last + 2]  # the language's functions, then where the next file begins
    address = {name: order[len(names) - 1 - i] for i, name in enumerate(names)}
    by_address = {at: name for name, at in address.items()}

    def ro_name(value):
        offset = value - rodata_at
        start, name = max((o, n) for o, n in ro_labels if o <= offset)
        return name if offset == start else f"{name}+{offset - start:#x}"

    called, pointed, lines = {}, {}, []
    for name, _, offset, size, _, _ in sorted(functions, key=lambda f: address[f[0]]):  # in the language's order
        at = address[name]
        end = order[order.index(at) + 1]
        us_calls = [rows[0][1] for k, rows in sorted(relocations.items())
                    if offset <= k < offset + size and rows[0][0] == elf.R_ARM_THM_CALL and rows[0][1] not in own]
        us_words = [rows[0][1] for k, rows in sorted(relocations.items())
                    if offset <= k < offset + size and rows[0][0] == elf.R_ARM_ABS32 and rows[0][1] not in own
                    and not any(rows[0][1] == n for _, n in ro_labels) and not rows[0][1].startswith(".")]
        # Pass 1: literal pools, switch tables, branch targets and calls.
        pool, table, labels, calls = set(), {}, set(), []
        k, last_compare = at, 0
        while k < end:
            if k in pool:
                k += 4
                continue
            if k in table:
                k += 2
                continue
            x = module.half(k)
            if x >> 11 == THUMB_LITERAL_LOAD:
                literal = ((k + 4) & ~3) + (x & 0xFF) * 4
                if at <= literal < end:
                    pool.add(literal)
            if x >> 11 == THUMB_CMP_IMMEDIATE:
                last_compare = x & 0xFF
            if x & 0xFF87 == THUMB_ADD_PC_ENCODING:  # add pc, rN: a switch's table of offsets follows
                base = k + 2
                labels.add(base)
                for case in range(last_compare + 1):
                    table[base + 2 * case] = base
                    labels.add(base + struct.unpack_from("<h", module.body, base + 2 * case - module.base)[0] + 2)
            if x >> 12 == THUMB_CONDITIONAL_BRANCH and x >> 8 & 15 < FIRST_RESERVED_CONDITION:
                labels.add(k + 4 + ((x & 0xFF) - (0x100 if x & 0x80 else 0)) * 2)
            if x >> 11 == THUMB_BRANCH:
                labels.add(k + 4 + ((x & 0x7FF) - (0x800 if x & 0x400 else 0)) * 2)
            target = module.call(k) if k + 4 <= end else None
            if target is not None:
                calls.append((k, target))
                k += 4
                continue
            k += 2
        outside = [(k, target) for k, target in calls if target not in by_address]
        _require(len(outside) == len(us_calls), f"{name} calls {len(outside)} functions outside the file, US {len(us_calls)}")
        for (k, target), us_name in zip(outside, us_calls):
            _require(called.setdefault(target, us_name) == us_name, f"{target:#x} is both {called[target]} and {us_name}")
            if target in placed and us_name not in placed[target]:
                raise WriteOutError(f"{name} calls {us_name} at {target:#x}, which the symbol map gives to {sorted(placed[target])}")
        words = [module.word(p) for p in sorted(pool)]
        addresses = [w for w in words if nds.ARM9_RAM <= w < nds.ARM9_RAM_END and not rodata_at <= w < rodata_at + ro_size
                     and not (w & 1 and w - 1 in by_address)]
        _require(len(addresses) == len(us_words), f"{name} has {len(addresses)} address literals, US {len(us_words)}")
        for value, us_name in zip(addresses, us_words):
            _require(pointed.setdefault(value, us_name) == us_name, f"{value:#x} is both {pointed[value]} and {us_name}")

        def literal(value):
            if rodata_at <= value < rodata_at + ro_size:
                return ro_name(value)
            if value & 1 and value - 1 in by_address:
                return by_address[value - 1]
            return pointed.get(value)

        # Pass 2: the text.
        lines += [f"\tthumb_func_start {name}", f"{name}: ; {at:#010x}"]
        k = at
        while k < end:
            if k in labels and k != at:
                lines.append(f"_{k:08X}:")
            if k in pool:
                value = module.word(k)
                lines.append(f"_{k:08X}: .word {literal(value) or f'0x{value:08X}'}")
                k += 4
                continue
            if k in table:
                lines.append(f"\t.short _{table[k] + struct.unpack_from('<h', module.body, k - module.base)[0] + 2:08X} - _{table[k]:08X} - 2")
                k += 2
                continue
            target = module.call(k) if k + 4 <= end else None
            if target is not None:
                lines.append(f"\tbl {by_address.get(target) or called[target]}")  # the linker makes BLX for ARM code
                k += 4
                continue
            x = module.half(k)
            if x >> 11 == THUMB_LITERAL_LOAD and ((k + 4) & ~3) + (x & 0xFF) * 4 in pool:
                literal_at = ((k + 4) & ~3) + (x & 0xFF) * 4
                value = module.word(literal_at)
                op = f"ldr r{x >> 8 & 7}, _{literal_at:08X}" + ("" if literal(value) else f" ; =0x{value:08X}")
            else:
                op = instruction(x, k, lambda target: f"_{target:08X}")
            lines.append("\t" + (op or f".short 0x{x:04X}"))
            k += 2
        lines += [f"\tthumb_func_end {name}", ""]
    return header + us_source[:us_source.index("\t.text\n")] + "\t.text\n\n" + "\n".join(lines)
