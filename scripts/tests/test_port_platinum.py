"""Retail Platinum port building blocks, checked on synthetic images; no retail data is used."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
from retail_port import dp, elf, link, nds, platinum, sdk, symbols, toolchain, writeout  # noqa: E402

spec = importlib.util.spec_from_file_location("port_platinum", SCRIPTS / "port-platinum.py")
port_platinum = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port_platinum)


def make_object(sections, symbols_, relocations=(), rel=False):
    """A minimal ELF32 ARM object.

    sections: [(name, type, flags, bytes)]; symbols_: [(name, value, size, bind, type, section index)];
    relocations: [(target section index, offset, type, symbol index, addend)]."""
    names = bytearray(b"\0")
    headers = [(0, 0, 0, 0, 0, 0, 0, 0)]
    bodies = [b""]
    for name, kind, flags, body in sections:
        headers.append([len(names), kind, flags, 0, len(body), 0, 0, 0])
        names += name.encode() + b"\0"
        bodies.append(body)
    strings = bytearray(b"\0")
    table = bytes(16)
    for name, value, size, bind, kind, shndx in symbols_:
        table += struct.pack("<IIIBBH", len(strings), value, size, bind << 4 | kind, 0, shndx)
        strings += name.encode() + b"\0"
    symtab = len(headers)
    headers.append([len(names), elf.SHT_SYMTAB, 0, 0, len(table), symtab + 1, 1, 16])
    names += b".symtab\0"
    bodies.append(table)
    headers.append([len(names), 3, 0, 0, len(strings), 0, 0, 0])
    names += b".strtab\0"
    bodies.append(bytes(strings))
    by_target = {}
    for target, offset, kind, index, addend in relocations:
        row = struct.pack("<II", offset, index << 8 | kind) + (b"" if rel else struct.pack("<i", addend))
        by_target.setdefault(target, b"")
        by_target[target] += row
    for target, rows in by_target.items():
        headers.append([len(names), elf.SHT_REL if rel else elf.SHT_RELA, 0, 0, len(rows), symtab, target, 8 if rel else 12])
        names += f".rela{target}\0".encode()
        bodies.append(rows)
    shstrndx = len(headers)
    headers.append([len(names), 3, 0, 0, len(names) + 10, 0, 0, 0])
    bodies.append(bytes(names) + bytes(10))
    data = bytearray(52)
    for i, body in enumerate(bodies):
        if i:
            headers[i][3] = len(data)
            data += body
            while len(data) % 4:
                data.append(0)
    shoff = len(data)
    for i, (name, kind, flags, offset, size, link_, info, entsize) in enumerate(headers):
        data += struct.pack("<10I", name, kind, flags, 0, offset, size, link_, info, 4, entsize)
    data[0:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 1, 40, 1, 0, 0, shoff, 0x05000000, 52, 0, 0, 40, len(headers), shstrndx)
    return bytes(data)


def thumb_object():
    """A Thumb function caller() calling an external callee() and holding a pointer to data+8."""
    code = struct.pack("<HHHH", 0xB500, 0xF000, 0xF800, 0xBD00) + bytes(4)
    return make_object(
        [(".text", 1, elf.SHF_EXECINSTR | 2, code), (".data", 1, elf.SHF_WRITE | 2, bytes(16))],
        [("$t", 0, 0, 0, elf.STT_FUNC, 1), ("caller", 0, 12, 1, elf.STT_FUNC, 1),
         ("callee", 0, 0, 1, elf.STT_NOTYPE, 0), ("table", 0, 16, 1, elf.STT_OBJECT, 2)],
        [(1, 2, elf.R_ARM_THM_CALL, 3, -4), (1, 8, elf.R_ARM_ABS32, 4, 8)])


def bl(site, target):
    """A Thumb BL (to a Thumb target) or BLX (to an ARM target) at site."""
    offset = (target & ~1) - (site + 4)
    return struct.pack("<HH", 0xF000 | ((offset >> 12) & 0x7FF), (0xF800 if target & 1 else 0xE800) | ((offset >> 1) & 0x7FF))


class RomTest(unittest.TestCase):
    def test_backward_lz_literals_and_copies(self):
        # Read backwards from the flag byte 0x18: literals Z, Y, X, then two copies three bytes back.
        stream = b"\x00\x60\x00\xf0XYZ\x18"
        data = b"PRE" + stream + struct.pack("<II", 8 << 24 | (len(stream) + 8), 14)
        self.assertEqual(nds.blz_decode(data), b"PRE" + b"XYZ" * 10)
        with self.assertRaises(nds.RomError):
            nds.blz_decode(b"\0" * 8)

    def test_compressed_arm9_is_expanded_from_its_module_parameters(self):
        stream = b"\x00\x60\x00\xf0XYZ\x18"
        packed = b"PRE" + stream + struct.pack("<II", 8 << 24 | (len(stream) + 8), 14)
        def arm9(compressed_end, body):
            params = struct.pack("<9I", 0, 0, 0, 0, 0, compressed_end, 0, 0xDEC00621, 0x2106C0DE)
            return params + body
        # The parameters sit before the compressed stream here, as in the static binary's header.
        plain = arm9(0, b"rest")
        self.assertEqual(nds.arm9_expanded(plain), plain)
        head = struct.pack("<9I", 0, 0, 0, 0, 0, 0, 0, 0xDEC00621, 0x2106C0DE)
        image = head + packed
        compressed = bytearray(image)
        struct.pack_into("<I", compressed, 0x14, nds.ARM9_RAM + len(image))
        self.assertEqual(nds.arm9_expanded(bytes(compressed)), nds.blz_decode(bytes(compressed)))
        self.assertEqual(nds.arm9_expanded(b"no parameters"), b"no parameters")

    def test_archive_round_trip(self):
        members = [b"one", b"", b"three!"]
        names = b"BTNF" + struct.pack("<IIHH", 16, 4, 0, 1)
        archive = nds.narc_join(members, names)
        self.assertEqual(nds.narc_split(archive), (members, names))
        with self.assertRaises(nds.RomError):
            nds.narc_split(b"JUNK" + archive[4:])

    def test_veneer_reaches_its_trampoline(self):
        at, stub = 0x02001000, 0x02200000
        veneer = nds.thumb_entry_veneer(at, stub)
        self.assertEqual(veneer[:2], b"\x00\xb5")  # push {lr}
        self.assertEqual(elf.thumb_bl_target(veneer[2:], at + 2), ("blx", stub))
        with self.assertRaises(nds.RomError):
            nds.thumb_entry_veneer(at, at + (1 << 23))
        self.assertEqual(struct.unpack("<III", nds.arm_trampoline(0x02201235)), (0xE49DE004, 0xE51FF004, 0x02201235))

    def test_crc16_matches_the_header_checksum(self):
        self.assertEqual(nds.crc16(b"123456789"), 0x4B37)


def cartridge():
    """A small cartridge: ARM9 with one autoload and an arena literal, two files, one overlay."""
    rom = bytearray(0x4000)
    rom[0x0C:0x10] = b"CPUF"
    rom[0x14] = 0  # 128 KiB capacity
    arm9_offset, arena = 0x400, 0x02000600
    code = bytearray(0x200)
    struct.pack_into("<I", code, 0x40, arena)
    autoload_data = b"\x11" * 0x20
    table_start = len(code) + len(autoload_data)
    table = struct.pack("<III", 0x027E0000, 0x20, 0)
    params = 0x80
    struct.pack_into("<II", code, params, nds.ARM9_RAM + table_start, nds.ARM9_RAM + table_start + len(table))
    arm9 = bytes(code) + autoload_data + table
    footer = struct.pack("<III", nds.NITRO_FOOTER, params, 0)
    rom[arm9_offset:arm9_offset + len(arm9) + 12] = arm9 + footer
    struct.pack_into("<IIII", rom, 0x20, arm9_offset, 0x02000800, nds.ARM9_RAM, len(arm9))
    files_at = nds.align(arm9_offset + len(arm9) + 12, nds.FILE_ALIGN)
    fnt = files_at
    fnt_body = struct.pack("<IHH", 8, 0, 1) + b"\x04text" + b"\x05other" + b"\0"
    rom[fnt:fnt + len(fnt_body)] = fnt_body
    fat = fnt + 0x40
    a, b = fat + 0x40, fat + 0x80
    rom[a:a + 4] = b"AAAA"
    rom[b:b + 6] = b"BBBBBB"
    struct.pack_into("<IIII", rom, fat, a, a + 4, b, b + 6)
    overlay_table = b + 0x40
    struct.pack_into("<8I", rom, overlay_table, 0, 0x02000700, 0x100, 0x40, 0, 0, 1, 0)
    struct.pack_into("<II", rom, 0x40, fnt, len(fnt_body))
    struct.pack_into("<II", rom, 0x48, fat, 16)
    struct.pack_into("<II", rom, 0x50, overlay_table, 32)
    struct.pack_into("<I", rom, 0x80, overlay_table + 32)
    return bytes(rom) + b"\xff" * (0x20000 - len(rom)), arena


class LayoutTest(unittest.TestCase):
    def test_module_is_added_as_an_autoload_and_files_move(self):
        rom, arena = cartridge()
        self.assertEqual(nds.file_ids(rom), {"text": 0, "other": 1})
        self.assertEqual(platinum.retail_arena(rom), 0x02000700 + 0x100 + 0x40)
        offset, size = nds.u32(rom, 0x20), nds.u32(rom, 0x2C)
        literal = platinum.arena_literal(rom[offset:offset + size], arena)
        segment = b"\x22" * 0x600
        arm9 = nds.add_autoload(rom, segment, arena, literal, arena + 0x600)
        params, start, end = nds.autoload_table(arm9)
        self.assertEqual([struct.unpack_from("<III", arm9, k) for k in range(start, end, 12)],
                         [(0x027E0000, 0x20, 0), (arena, 0x600, 0)])
        self.assertEqual(arm9[start - 0x600:start], segment)
        self.assertEqual(nds.u32(arm9, literal), arena + 0x600)
        out = nds.relayout(rom, arm9, {0: b"NEWTEXT"})
        self.assertEqual(len(out), len(rom))
        self.assertEqual(nds.u16(out, nds.HEADER_CRC), nds.crc16(out[:nds.HEADER_CRC]))
        self.assertEqual(nds.file_ids(out), {"text": 0, "other": 1})
        self.assertEqual(out[slice(*nds.file_span(out, 0))], b"NEWTEXT")
        self.assertEqual(out[slice(*nds.file_span(out, 1))], b"BBBBBB")
        self.assertGreater(nds.file_span(out, 1)[0], nds.file_span(rom, 1)[0])
        self.assertEqual(nds.modules(out)["arm9"][1][:0x40], rom[offset:offset + 0x40])
        with self.assertRaises(nds.RomError):
            nds.add_autoload(rom, segment, arena + 4, literal, arena)

    def test_arena_literal_must_be_unique(self):
        with self.assertRaises(platinum.PortError):
            platinum.arena_literal(struct.pack("<II", 5, 5), 5)
        with self.assertRaises(platinum.PortError):
            platinum.arena_literal(b"\0" * 8, 5)


class ElfTest(unittest.TestCase):
    def test_objects_are_read_with_thumb_modes(self):
        obj = elf.Elf(thumb_object())
        self.assertEqual([f[0] for f in obj.functions()], ["caller"])
        self.assertTrue(obj.functions()[0][4])
        self.assertEqual(obj.relocs[1], [(2, elf.R_ARM_THM_CALL, 3, -4), (8, elf.R_ARM_ABS32, 4, 8)])
        with self.assertRaises(elf.ElfError):
            elf.Elf(b"\x7fELF\x02" + bytes(60))

    def test_gnu_ld_copies_carry_addends_and_thumb_types(self):
        copy = elf.Elf(elf.with_inline_addends(thumb_object()))
        by_name = {s["name"]: s for s in copy.symbols}
        self.assertEqual(by_name["caller"]["type"], elf.STT_ARM_TFUNC)
        self.assertEqual(by_name["$t"]["type"], elf.STT_NOTYPE)
        text = copy.section_bytes(1)
        self.assertEqual(struct.unpack_from("<i", text, 8)[0], 8)
        self.assertEqual(struct.unpack_from("<HH", text, 2), elf.thumb_call_field(0xF800, -4))
        unsupported = make_object([(".text", 1, elf.SHF_EXECINSTR, bytes(8))], [("x", 0, 0, 1, 0, 0)],
                                  [(1, 0, elf.R_ARM_REL32, 1, 0)])
        with self.assertRaises(elf.ElfError):
            elf.with_inline_addends(unsupported)

    def test_section_relative_code_addresses_keep_their_mode(self):
        # ARM code at 0, Thumb code at 8, then a literal pool; addresses taken through the section symbol.
        obj = make_object(
            [(".text", 1, elf.SHF_EXECINSTR | 2, bytes(24))],
            [("$a", 0, 0, 0, elf.STT_NOTYPE, 1), ("$t", 8, 0, 0, elf.STT_NOTYPE, 1), ("$d", 12, 0, 0, elf.STT_NOTYPE, 1),
             ("", 0, 0, 0, elf.STT_SECTION, 1)],
            [(1, 12, elf.R_ARM_ABS32, 4, 8), (1, 16, elf.R_ARM_ABS32, 4, 0), (1, 20, elf.R_ARM_NONE, 4, 0)])
        copy = elf.Elf(elf.with_inline_addends(obj))
        text = copy.section_bytes(1)
        self.assertEqual(struct.unpack_from("<ii", text, 12), (9, 0))
        self.assertEqual(copy.relocs[1], [(12, elf.R_ARM_ABS32, 4, 9), (16, elf.R_ARM_ABS32, 4, 0), (20, elf.R_ARM_NONE, 4, 0)])

    def test_unsized_functions_run_to_the_next_function(self):
        # Two assembly functions without sizes, the first with an internal branch label.
        obj = make_object(
            [(".text", 1, elf.SHF_EXECINSTR | 2, bytes(20))],
            [("$t", 0, 0, 0, elf.STT_FUNC, 1), ("first", 0, 0, 1, elf.STT_FUNC, 1), ("$b", 4, 0, 0, elf.STT_FUNC, 1),
             ("second", 12, 0, 1, elf.STT_FUNC, 1), ("sized", 16, 2, 1, elf.STT_FUNC, 1)])
        sizes = {f[0]: f[3] for f in elf.Elf(elf.with_function_sizes(obj)).functions()}
        self.assertEqual(sizes, {"first": 12, "second": 4, "sized": 2})

    def test_numbered_sections_name_each_input_section(self):
        copy = elf.Elf(elf.numbered_sections(thumb_object()))
        self.assertEqual([s["name"] for s in copy.sections[1:3]], [".text.1", ".data.2"])
        self.assertEqual(copy.section_bytes(1), elf.Elf(thumb_object()).section_bytes(1))

    def test_branch_decoding(self):
        bl = struct.pack("<HH", 0xF000, 0xF800 | 0x10)
        self.assertEqual(elf.thumb_bl_target(bl, 0x1000), ("bl", 0x1000 + 4 + 0x20 | 1))
        self.assertIsNone(elf.thumb_bl_target(b"\0\0\0\0", 0))
        self.assertEqual(elf.arm_call(0xEB000001, 0x1000), ("bl", 0x100C))
        self.assertEqual(elf.arm_call(0xFB000001, 0x1000), ("blx", 0x100E | 1))
        self.assertIsNone(elf.arm_call(0xE1A00000, 0))


class LinkTest(unittest.TestCase):
    MAP = """Discarded input sections

 .text.9        0x00000000        0x4 /w/a.o

Linker script and memory map

                0x02100000                . = 0x2100000
 .text.1        0x02100000        0xc /w/a.o
 .data.2        0x02100010       0x10 /w/a.o
 .rodata.123456789012
                0x02100020        0x8 /w/b.o
 *(.text.*)
"""

    def test_map_placements(self):
        found = link.placements(self.MAP)
        self.assertEqual(found[("/w/a.o", ".text.1")], [(0x02100000, 0xC)])
        self.assertEqual(found[("/w/b.o", ".rodata.123456789012")], [(0x02100020, 8)])
        self.assertNotIn(("/w/a.o", ".text.9"), found)

    def linked(self, call_target, word):
        obj = elf.Elf(elf.numbered_sections(elf.with_inline_addends(thumb_object())))
        code = bytearray(0x20)
        code[0:2] = b"\x00\xb5"
        code[2:6] = bl(0x02100002, call_target)
        struct.pack_into("<I", code, 8, word)
        return link.Linked(0x02100000, bytes(code), "02100000 T caller\n02100010 D table\n", self.MAP, {"/w/a.o": obj})

    def test_relocations_must_land_on_their_symbols(self):
        good = self.linked(0x02300001, 0x02100018)
        self.assertEqual(link.check_relocations(good, {"callee": 0x02300001}), [])
        self.assertEqual(good.function_addresses("/w/a.o"), {"caller": 0x02100001})
        wrong_target = link.check_relocations(self.linked(0x02300005, 0x02100018), {"callee": 0x02300001})
        self.assertEqual(len(wrong_target), 1)
        self.assertIn("callee", wrong_target[0])
        # A call to an ARM function must be a BLX, not a BL.
        self.assertEqual(len(link.check_relocations(good, {"callee": 0x02300000})), 1)
        self.assertEqual(len(link.check_relocations(self.linked(0x02300001, 0x02100010), {"callee": 0x02300001})), 1)
        self.assertIn("unresolved", link.check_relocations(good, {})[0])

    def test_symbol_source_marks_functions(self):
        source = link.symbol_source({"Thumb": 0x02000001, "Arm": 0x02000004, "data": 0x02100000}, {"Thumb", "Arm"})
        self.assertIn(".thumb_set Thumb, 0x2000001", source)
        self.assertIn(".type Arm, %function\n.set Arm, 0x2000004", source)
        self.assertIn(".set data, 0x2100000", source)
        self.assertNotIn(".type data", source)
        self.assertIn("KEEP(*src_main.c.o(.text.*))", link.linker_script(0x02200000, ["src_main.c.o"]))


class MatchTest(unittest.TestCase):
    def test_masked_bytes_match_once(self):
        modules = {"arm9": (0x02000000, b"\x01\x02\xAA\xBB\x05\x06" + b"\x01\x02\xCC\xDD\x07\x06")}
        keep = [True, True, False, False, True, True]
        self.assertEqual(symbols.unique_match(modules, "arm9", b"\x01\x02\0\0\x05\x06", keep), 0x02000000)
        self.assertIsNone(symbols.unique_match(modules, "arm9", b"\x01\x02\0\0\x05\x06", [True, True, False, False, False, True]))
        self.assertIsNone(symbols.unique_match(modules, "arm9", b"\x09", [True]))
        self.assertEqual(symbols.reference_target(elf.R_ARM_ABS32, struct.pack("<I", 0x02000010), 0, 0x10), 0x02000000)

    def test_reviewed_function_is_the_one_guess_with_the_reviewed_difference(self):
        class Table:
            us = type("Image", (), {"thumb": {7}})()

            def __init__(self, guesses, differing):
                self.guesses = lambda symbol: guesses
                self.differing = lambda symbol, at: differing[at]
        symbol = symbols.Symbol(7, 0x02000101, 24, elf.STT_FUNC, 1, "sub_02000100")
        # A caller and the neighbour before it agree; the neighbour after it moved with added code.
        table = Table({0x02000560, 0x02000570}, {0x02000560: 7, 0x02000570: 16})
        self.assertEqual(symbols.reviewed_address(table, symbol, 7), 0x02000561)
        with self.assertRaisesRegex(ValueError, "no call or neighbour"):
            symbols.reviewed_address(table, symbol, 4)
        with self.assertRaisesRegex(ValueError, "0x2000560, 0x2000570"):
            symbols.reviewed_address(Table({0x02000560, 0x02000570}, {0x02000560: 7, 0x02000570: 7}), symbol, 7)

    def test_ar_members(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lib.a"
            entry = lambda name, body: name.ljust(16).encode() + b"0".ljust(12) + b"0".ljust(6) * 2 + b"644".ljust(8) + \
                str(len(body)).ljust(10).encode() + b"`\n" + body + (b"\n" if len(body) % 2 else b"")
            path.write_bytes(b"!<arch>\n" + entry("//", b"long_object_name.o/\n") + entry("/0", b"abc") + entry("b.o/", b"xy"))
            self.assertEqual(list(sdk.members(path)), [("long_object_name.o", b"abc"), ("b.o", b"xy")])


class PlatinumTest(unittest.TestCase):
    VERSIONS = """enum Version {
    VERSION_NONE = 0,
    VERSION_SAPPHIRE,
    VERSION_PLATINUM = 12, // comment
    VERSION_GAMECUBE = 15,
};
enum Language {
    LANGUAGE_NONE = 0,
    LANGUAGE_JAPANESE,
    LANGUAGE_ENGLISH,
    LANGUAGE_FRENCH,
};"""

    def test_variant_code_uses_the_games_own_ids(self):
        self.assertEqual(platinum.variant_code(self.VERSIONS, "LANGUAGE_FRENCH", 0), 0x020C0300)
        self.assertEqual(platinum.enum_values(self.VERSIONS, "Version")["VERSION_SAPPHIRE"], 1)
        with self.assertRaises(platinum.PortError):
            platinum.enum_values(self.VERSIONS, "Missing")

    def test_text_bank_ids(self):
        header = "#define TEXT_BANK_START_MENU    367\n#define TEXT_BANK_WONDER_TRADE  724\n"
        self.assertEqual(platinum.text_bank_ids(header), {"TEXT_BANK_START_MENU": 367, "TEXT_BANK_WONDER_TRADE": 724})

    def test_every_language_has_a_menu_label_and_build_deltas(self):
        strings = json.loads((platinum.ROOT / "client/res/text/wonder_trade.json").read_text())
        label = next(message for message in strings["messages"] if message["id"] == platinum.MENU_LABEL)
        self.assertIs(strings["messages"][-1], label)  # appended: earlier message IDs stay put
        patch = (platinum.ROOT / "client/pokeplatinum.patch").read_text()
        self.assertIn(f'"en_US": "{label["en_US"]}"', patch)
        catalogue = {row["id"]: row for row in json.loads((platinum.ROOT / "patches/compatibility.json").read_text())["variants"]}
        layout = json.loads((platinum.ROOT / "protocol/save-layout.json").read_text())
        for language, (variant, _, key, deltas) in platinum.LANGUAGES.items():
            with self.subTest(language=language):
                self.assertTrue(label[key])
                self.assertLessEqual(len(label[key]), 15)
                self.assertTrue(all((platinum.RETAIL / name).is_file() for name in deltas))
                self.assertEqual(catalogue[variant]["port_base"], platinum.US_VARIANT)
                self.assertIn(variant, layout["verified_targets"])
        self.assertTrue(platinum.PORT_PATCH.is_file())

    def test_text_banks_are_aligned_by_their_keys(self):
        bank = lambda count, key: struct.pack("<HH", count, key)
        us = [bank(1, 10), bank(2, 11), bank(3, 12), bank(4, 13), bank(5, 14)]
        # A Japanese archive lacks bank 1 and translates bank 3 to a different length.
        japanese = [bank(1, 10), bank(3, 12), bank(9, 13), bank(5, 14)]
        self.assertEqual(platinum.text_bank_remap(us, japanese), {0: 0, 2: 1, 3: 2, 4: 3})
        self.assertEqual(platinum.text_bank_remap(us, us), {i: i for i in range(5)})

    def test_text_bank_header_is_renumbered(self):
        header = ("enum TextBank {\n    TEXT_BANK_A                          = 0,\n    TEXT_BANK_GONE                       = 1,\n"
                  "    TEXT_BANK_START_MENU                 = 2,\n    TEXT_BANK_WONDER_TRADE               = 3,\n};\n"
                  "#define TEXT_BANK_A                          0\n#define TEXT_BANK_GONE                       1\n"
                  "#define TEXT_BANK_START_MENU                 2\n#define TEXT_BANK_WONDER_TRADE               3\n")
        out = platinum.remapped_text_banks(header, {0: 0, 2: 1}, 2)
        self.assertEqual(platinum.text_bank_ids(out),
                         {"TEXT_BANK_A": 0, "TEXT_BANK_GONE": 0xFFFF, "TEXT_BANK_START_MENU": 1, "TEXT_BANK_WONDER_TRADE": 2})
        self.assertIn("TEXT_BANK_START_MENU                 = 1,", out)
        self.assertIn("TEXT_BANK_GONE                       = 65535,", out)

    def test_reviewed_differences_are_counted_exactly(self):
        for language, names in platinum.REVIEWED.items():
            with self.subTest(language=language):
                self.assertIn(language, platinum.LANGUAGES)
                self.assertTrue(all(type(count) is int and count > 0 for count in names.values()))

    def test_game_overlays_may_load_elsewhere_but_symbols_stay_inside(self):
        def rom(ram):
            data = bytearray(0x200)
            struct.pack_into("<II", data, 0x50, 0x100, 32)
            struct.pack_into("<8I", data, 0x100, 95, ram, 0x1000, 0x100, 0, 0, 1, 0)
            return bytes(data)

        class Symbol:
            def __init__(self, address):
                self.address, self.num, self.name = address, address, f"symbol_{address:x}"

        class Image:
            def unique(self, name):
                return {"SDK_OVERLAY_trade_sequence_ID": Symbol(95), "TradeSequence_Init": Symbol(0x02250000)}.get(name)

            def module(self, symbol):
                return 95

        target = object.__new__(platinum.Target)
        target.us_rom, target.rom, target.image, target.witnessed = rom(0x02250000), rom(0x02260000), Image(), {}
        # US Rev 1 loads overlay 95 at 0x02250000, the retail game at 0x02260000.
        self.assertEqual(target._overlay_id(Symbol(95)), 95)
        target._resolve = lambda name: {"SDK_OVERLAY_trade_sequence_ID": (95, False)}.get(name, (0x02260101, True))
        self.assertEqual(target.resolve("SDK_OVERLAY_trade_sequence_ID"), (95, False))
        self.assertEqual(target.resolve("TradeSequence_Init"), (0x02260101, True))
        target._resolve = lambda name: (0x02250101, True)
        with self.assertRaisesRegex(platinum.PortError, "outside overlay 95"):
            target.resolve("TradeSequence_Init")
        target.rom = bytes(0x200)
        with self.assertRaisesRegex(platinum.PortError, "not in the retail game"):
            target._overlay_id(Symbol(95))

    def test_template_only_inits_are_found_through_agreeing_templates(self):
        class Symbol:
            def __init__(self, num):
                self.num = num

        class Image:
            thumb = {1, 2}

            def unique(self, name):
                return Symbol({"Main": 1, "Exit": 2}[name])

        class Map:
            addr = {1: 0x02246100, 2: 0x02246200}

        def template(init):
            return struct.pack("<4I", init, 0x02246101, 0x02246201, 95)

        target = object.__new__(platinum.Target)
        target.image, target.map = Image(), Map()
        target.modules = {"arm9": (0x02000000, bytes(8) + template(0x02246001) + template(0x02246001)),
                          94: (0x02240000, template(0x02246001))}
        self.assertEqual(target._template_init("Main", "Exit", 95), 0x02246000)
        target.modules[94] = (0x02240000, template(0x02246401))
        with self.assertRaisesRegex(platinum.PortError, "disagree"):
            target._template_init("Main", "Exit", 95)
        target.modules = {"arm9": (0x02000000, template(0x02246001)[:12] + struct.pack("<I", 94))}
        self.assertIsNone(target._template_init("Main", "Exit", 95))
        # Without an explicit overlay, the one main is in: overlay 71, as in HeartGold.
        target.image.module = lambda symbol: 71
        target.modules = {"arm9": (0x02000000, template(0x02246001)[:12] + struct.pack("<I", 71))}
        self.assertEqual(target._template_init("Main", "Exit"), 0x02246000)

    def test_placeable_names_are_looked_up_once(self):
        calls = []

        class Target:
            def resolve(self, name):
                calls.append(name)
                return (1, False) if name == "found" else None
        placeable = platinum.Placeable(Target(), {"defined"})
        self.assertIn("defined", placeable)
        self.assertIn("found", placeable)
        self.assertIn("found", placeable)
        self.assertNotIn("missing", placeable)
        self.assertEqual(calls, ["found", "missing"])


class DiamondPearlTest(unittest.TestCase):
    def test_make_objects_follow_their_sources(self):
        self.assertEqual(toolchain.dp_object("arm9/src/main.c", "DIAMOND"), "build/diamond.us/src/main.o")
        self.assertEqual(toolchain.dp_object("arm9/overlays/52/src/overlay_52.c", "PEARL"),
                         "build/pearl.us/overlays/52/src/overlay_52.o")

    def test_bank_keys_come_from_msg_mk(self):
        msg_mk = ("files/msgdata/msg/narc_0323.bin: MSGENCFLAGS = -k 0xd3a0\n"
                  "files/msgdata/msg/narc_0624.bin: MSGENCFLAGS = -k 0x5754\n")
        self.assertEqual((dp.gmm_key(msg_mk, 323), dp.gmm_key(msg_mk, 624)), (0xD3A0, 0x5754))
        with self.assertRaises(platinum.PortError):
            dp.gmm_key(msg_mk, 1)

    def test_gmm_text_moves_into_the_english_slot(self):
        source = ('<?xml version="1.0"?>\n<body language="English"><row id="a" index="0">'
                  '<language name="English">WONDER TRADE</language><language name="French">ECHANGE MIRACLE</language>'
                  '</row></body>')
        out = dp.localized_gmm(source, "French")
        self.assertTrue(out.startswith('<?xml version="1.0"?>'))
        self.assertIn('<language name="English">ECHANGE MIRACLE</language>', out)
        with self.assertRaises(platinum.PortError):
            dp.localized_gmm(source, "German")

    def test_reviewed_and_library_tables_name_known_languages(self):
        for table in (dp.REVIEWED, dp.LIBRARIES, dp.LIBRARY_REVIEWED):
            for language, names in table.items():
                with self.subTest(language=language):
                    self.assertIn(language, dp.LANGUAGES)
                    self.assertTrue(names)
        for language, names in dp.LIBRARY_REVIEWED.items():
            self.assertTrue(set(names) <= set(dp.LIBRARIES[language].values()))
            self.assertTrue(all(type(count) is int and count > 0 for count in names.values()))

    def test_sdk_library_reads_loose_objects(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "OS_init.o"
            path.write_bytes(thumb_object())
            library = sdk.Library([path])
            self.assertIn("caller", library.globals)
            self.assertNotIn("callee", library.globals)


    def test_copied_thumb_routines_keep_their_mode(self):
        # Metrowerks marks Thumb code only with $t: the copy must still be Thumb.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "font.o"
            path.write_bytes(thumb_object())
            source = sdk.Library([path]).extract(["caller"], external={"callee"})
            self.assertIn(".thumb_func\n.type caller, %function", source)

    def test_japanese_tables_name_known_languages_and_files(self):
        for table in (dp.MATCHING, dp.START_MENUS, dp.COPIED, dp.ALIASES, dp.WRAPPERS):
            self.assertTrue(set(table) <= set(dp.LANGUAGES))
        for language in dp.MATCHING:
            self.assertTrue((platinum.RETAIL / dp.MATCHING[language]).is_file())
            self.assertIn(language, dp.COMPILERS)
        for patch in dp.START_MENUS.values():
            self.assertTrue((platinum.RETAIL / patch).is_file())
        for wrappers in dp.WRAPPERS.values():
            for code, pool in wrappers.values():
                self.assertTrue(all(0 <= half < 0x10000 for half in code) and pool)


class CallPlacementTest(unittest.TestCase):
    def test_functions_are_placed_only_where_every_matched_call_agrees(self):
        Symbol = symbols.Symbol
        table = type("Table", (), {})()
        table._bynum = {1: Symbol(1, 0x2000101, 8, elf.STT_FUNC, 1, "agreed"),
                        2: Symbol(2, 0x2000201, 8, elf.STT_FUNC, 1, "disputed"),
                        3: Symbol(3, 0x2000300, 8, elf.STT_OBJECT, 1, "data")}
        table.addr, table.how = {}, {}
        table.votes = {1: {0x2100101: 3}, 2: {0x2100201: 1, 0x2100301: 1}, 3: {0x2100400: 2}}
        symbols.trust_calls(table)
        self.assertEqual(table.addr, {1: 0x2100101})
        self.assertEqual(table.how[1], "calls x3")


class WriteOutTest(unittest.TestCase):
    def test_instructions_use_the_decompilations_syntax(self):
        label = lambda at: f"_{at:08X}"
        self.assertEqual(writeout.instruction(0xB570, 0, label), "push {r4-r6, lr}")
        self.assertEqual(writeout.instruction(0xBD08, 0, label), "pop {r3, pc}")
        self.assertEqual(writeout.instruction(0x1C05, 0, label), "add r5, r0, #0x0")
        self.assertEqual(writeout.instruction(0x2112, 0, label), "mov r1, #0x12")
        self.assertEqual(writeout.instruction(0x5C40, 0, label), "ldrb r0, [r0, r1]")
        self.assertEqual(writeout.instruction(0x46C0, 0, label), "mov r8, r8")
        self.assertEqual(writeout.instruction(0xD101, 0x2000000, label), "bne _02000006")
        self.assertEqual(writeout.instruction(0xE7FE, 0x2000000, label), "b _02000000")
        self.assertIsNone(writeout.instruction(0xDF00, 0, label))  # swi: written as .short

    def test_function_starts_are_calls_pointers_and_pushes(self):
        body = bytearray(0x40)
        struct.pack_into("<H", body, 0x10, 0xB510)  # push {r4, lr}
        body[0:4] = bl(0x2000000, 0x2000021)  # a call to 0x2000020
        struct.pack_into("<I", body, 0x38, 0x2000031)  # a pointer to Thumb code at 0x2000030
        module = writeout.Module(0x2000000, bytes(body))
        self.assertEqual(module.function_starts(0x2000004, 0x2000040), [0x2000010, 0x2000020, 0x2000030])


class CliTest(unittest.TestCase):
    def test_output_is_new_and_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "out.nds"
            port_platinum.write_new(path, b"rom")
            self.assertEqual(path.read_bytes(), b"rom")
            with self.assertRaises(FileExistsError):
                port_platinum.write_new(path, b"other")
            self.assertEqual(sorted(p.name for p in Path(directory).iterdir()), ["out.nds"])

    def test_refuses_an_existing_output_or_unknown_dump(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            existing = Path(directory) / "exists.nds"
            existing.write_bytes(b"keep")
            with self.assertRaises(SystemExit) as stop:
                port_platinum.main(["fr", str(existing), str(existing)])
            self.assertEqual(stop.exception.code, 1)
            dump = Path(directory) / "dump.nds"
            dump.write_bytes(b"not a cartridge")
            with self.assertRaises(SystemExit):
                port_platinum.main(["fr", str(dump), str(Path(directory) / "new.nds")])
            self.assertFalse((Path(directory) / "new.nds").exists())
            self.assertEqual(existing.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
