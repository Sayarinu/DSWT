"""Add Wonder Trade to a retail HeartGold or SoulSilver that pret cannot build, using US Rev 0 as the map.

The same matching and checks as the Platinum port (platinum.py) apply. HeartGold
and SoulSilver differ in how the decompilation adds Wonder Trade, so this port:

- expands the compressed ARM9 binary and the overlays it patches, and stores
  them uncompressed (as the Wonder Trade decompilation builds do);
- links two regions at once: resident code (the replaced functions, hooks,
  launcher, menu icon and SHA-256) loaded after the game's code like the
  Platinum module, and the wonder_trade overlay, placed after overlay 0 as in
  the decompilation and added to the overlay and file tables;
- relinks the overlay 27 functions the patch changes in assembly;
- appends the Wonder Trade text bank to the message archive and the menu icon
  to the NARC its palette comes from, instead of adding named files.
"""

import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from . import elf, link, nds, platinum, sdk, symbols, toolchain
from .platinum import PortError, require, git, sha256, ROOT, RETAIL, prepare_game

# game: (define, decompilation version name)
GAMES = {"heartgold": ("HEARTGOLD", "VERSION_HEARTGOLD"), "soulsilver": ("SOULSILVER", "VERSION_SOULSILVER")}
# id: (catalogue country, language enumerator, text column, build deltas)
LANGUAGES = {
    "fr": ("france", "LANGUAGE_FRENCH", "fr_FR", ()),
    "de": ("germany", "LANGUAGE_GERMAN", "de_DE", ()),
    "es": ("spain", "LANGUAGE_SPANISH", "es_ES", ()),
}
MESSAGES = "a/0/2/7"  # NARC_msgdata_msg
ICONS = "a/0/1/4"  # NARC_a_0_1_4, where the menu icon's palette comes from
WT_BANK = 829  # msg_0829, the bank the decompilation appends
HEAP_FUNCTION = "sub_0201A1B4"  # calls Heap_InitSystem(..., HEAP_ID_MAX, ...)
ASM = "asm/overlay_27.s"
PORT_PATCH = RETAIL / "heartgold-port.patch"  # port-only source changes
# Sources of the wonder_trade overlay (main.lsf.in); everything else is resident.
OVERLAY_SOURCES = ("application.c", "network.c", "tls.c", "http.c", "wire.c", "seed.c", "journal.c",
                   "engine.c", "pokemon_codec.c", "game_save.c", "flash.c", "boot.c")
OVERLAY_SOURCE_PATHS = tuple(f"src/wonder_trade/{'hgss/' if name in ('application.c', 'game_save.c') else ''}{name}"
                             for name in OVERLAY_SOURCES)
OVERLAY_TLS = "overlay-tls-"
FAMILY = 3
MIN_HEADROOM = 32 * 1024
COMPRESSED = 1 << 24


def defines(text, prefix):
    return {name: int(value, 0) for name, value in re.findall(rf"#define ({prefix}\w+)\s+(\w+)", text)}


class Target(platinum.Target):
    """One retail HeartGold or SoulSilver to port."""

    def __init__(self, game, language, rom_path, workspace, pristine, work, log=print):
        require(game in GAMES, f"unsupported game {game}")
        require(language in LANGUAGES, f"unsupported language {language}")
        self.game, self.language = game, language
        country, self.language_name, self.text_key, deltas = LANGUAGES[language]
        self.variant_id = f"{game}-{country}"
        self.deltas = [RETAIL / name for name in deltas]
        layout = json.loads((ROOT / "protocol/save-layout-hgss.json").read_text())
        korean = self.variant_id.endswith(layout["korean_target_suffix"])
        self.save_magic = layout["footer_magic"]["korean" if korean else "international"]
        self.workspace, self.pristine, self.work = (Path(p).resolve() for p in (workspace, pristine, work))
        self.log = log
        self.build_dir = f"build/{game}.us"
        self.compile_options = {"build": self.build_dir, "pch": False, "required": f"-D{GAMES[game][0]}"}
        catalogue = {row["id"]: row for row in json.loads((ROOT / "patches/compatibility.json").read_text())["variants"]}
        require(self.variant_id in catalogue, f"{self.variant_id} is not catalogued")
        self.variant = catalogue[self.variant_id]
        self.rom = Path(rom_path).read_bytes()
        require(len(self.rom) == self.variant["size"] and sha256(self.rom) == self.variant["sha256"],
                f"{rom_path} is not the catalogued {self.variant['game']} {self.variant['name']} dump")
        base = f"{game}-us-rev0"
        require(self.variant.get("port_base") == base, f"{self.variant_id} is not ported from {base}")
        us_path = self.pristine / self.build_dir / f"poke{game}.us.nds"
        self.us_rom = us_path.read_bytes()
        require(sha256(self.us_rom) == catalogue[base]["sha256"],
                f"{us_path} is not a pristine {base} build; build it with ROM_GAME={game} --pristine")
        pin = json.loads((ROOT / "client/upstream-pokeheartgold.json").read_text())["revision"]
        for tree in (self.workspace, self.pristine):
            require(git(tree, "rev-parse", "HEAD").decode().strip() == pin, f"{tree} is not at upstream {pin}")
        self.modules = nds.modules(self.rom)
        self.banks_header = None

    # --- Mapping --------------------------------------------------------------------------------------
    def map_symbols(self):
        self.image = symbols.UsImage((self.pristine / self.build_dir / "main.nef").read_bytes(), self.us_rom)
        self.map = symbols.SymbolMap(self.image, self.us_rom, self.rom)
        self.log(f"mapped {len(self.map.addr)} US symbols")

    def replaced_files(self):
        _, desired = prepare_game.overlay_files(ROOT, self.workspace, prepare_game.GAMES["hgss"])
        stale = sorted(name for name, content in desired.items()
                       if not (self.workspace / name).is_file() or (self.workspace / name).read_bytes() != content)
        require(not stale, f"the workspace is older than client/ ({', '.join(stale[:3])}); rebuild it with build-rom.sh")
        changed = git(self.workspace, "diff", "--name-only", "HEAD", "--", "src", "include", "asm").decode().split()
        self.files = sorted(name for name in changed if name.startswith("src/") and name.endswith(".c"))
        self.headers = sorted(name for name in changed if name.startswith("include/") and name.endswith(".h"))
        require(ASM in changed, f"the workspace does not change {ASM}")
        require(self.files, "the workspace changes no upstream sources; prepare it with prepare-game.py")

    def text_banks(self):
        """The message and icon archives, which must number like US (Japanese and Korean do not)."""
        ids = nds.file_ids(self.rom)
        self.text_fid, self.icon_fid = ids[MESSAGES], ids[ICONS]
        members, self.text_names = nds.narc_split(self.rom[slice(*nds.file_span(self.rom, self.text_fid))])
        us_members, _ = nds.narc_split(self.us_rom[slice(*nds.file_span(self.us_rom, nds.file_ids(self.us_rom)[MESSAGES]))])
        remap = platinum.text_bank_remap(us_members, members)
        require(len(members) == len(us_members) == WT_BANK and all(remap.get(i) == i for i in range(WT_BANK)),
                "the retail message archive numbers its banks differently from US")
        self.text_members = members
        self.icon_members, self.icon_names = nds.narc_split(self.rom[slice(*nds.file_span(self.rom, self.icon_fid))])
        self.icon_member = len(self.icon_members)

    def _stage(self, tree, files, read, patches):
        super()._stage(tree, files, read, patches)
        config = git(self.workspace, "show", "HEAD:include/config.h").decode()
        english = "#ifdef ENGLISH\n#define GAME_LANGUAGE LANGUAGE_ENGLISH"
        require(english in config, "include/config.h no longer selects English this way")
        (tree / "include").mkdir(parents=True, exist_ok=True)
        (tree / "include/config.h").write_text(config.replace(english, f"#if 1\n#define GAME_LANGUAGE {self.language_name}"))

    # --- 1. Upstream code, including the overlay 27 assembly, must match the retail bytes --------------
    def check_variant(self):
        super().check_variant()
        asm = elf.Elf((self.pristine / self.build_dir / "main.nef.p/asm_overlay_27.s.o").read_bytes())
        self.pristine_objects[ASM] = asm
        problems = self._locate(ASM, asm)
        require(not problems, "overlay 27 differs from the retail game:\n  " + "\n  ".join(problems))
        self.files = [*self.files, ASM]
        self.log(f"{len(self.located)} functions and {len(self.located_data)} data objects match the retail game")

    @staticmethod
    def _file_of(path):
        name = Path(path).name.removesuffix(".o")
        return ASM if name == "asm_overlay_27.s" else name.replace("src_", "src/", 1)

    # --- 2. Compile Wonder Trade for this language ------------------------------------------------------
    def port_defines(self):
        versions = defines(git(self.pristine, "show", "HEAD:include/config.h").decode(), "VERSION_")
        languages = defines(git(self.pristine, "show", "HEAD:include/config.h").decode(), "LANGUAGE_")
        self.code = (FAMILY << 24 | versions[GAMES[self.game][1]] << 16 | languages[self.language_name] << 8
                     | self.rom[0x1E])
        return {"WT_SAVE_MAGIC": f"{self.save_magic:#010x}", "WT_VARIANT": f"{self.code:#010x}",
                "WT_ICON_NARC": "NARC_a_0_1_4", "WT_ICON_NARC_MEMBER": str(self.icon_member)}

    def compile_port(self):
        tree = self.work / "port"
        self._stage(tree, [name for name in self.files if name != ASM] + self.headers,
                    lambda name: (self.workspace / name).read_bytes(), [*self.deltas, PORT_PATCH])
        tls = subprocess.run(["python3", str(ROOT / "scripts/prepare-tls.py")], check=True, capture_output=True, text=True).stdout.strip()
        sources = [name for name in self.files if name != ASM]
        objects = toolchain.compile_objects(self.workspace, tree, tree / "include", self.work / "port-obj", self.language_name,
                                            sources, wonder_trade=True, tls=tls, defines=self.port_defines(),
                                            **self.compile_options)
        build = self.workspace / self.build_dir
        # The prefixes keep the overlay's library apart from the resident one in link patterns.
        libraries = {OVERLAY_TLS: build / "src/wonder_trade/libwonder_trade_tls.a.p",
                     "resident-tls-": build / "src/wonder_trade/libwonder_trade_tls_resident.a.p"}
        out = self.work / "link"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        self.objects = {}
        inputs = [(path, path.name) for path in objects]
        for prefix, directory in libraries.items():
            found = sorted(directory.glob("*.o"))
            require(found, f"{directory} has no objects; build the workspace first")
            inputs += [(path, prefix + path.name) for path in found]
        inputs.append((build / "main.nef.p/asm_overlay_27.s.o", "asm_overlay_27.s.o"))
        for path, name in inputs:
            data = path.read_bytes()
            if name == "asm_overlay_27.s.o":
                data = self._shared_overlay_data(data)
            (out / name).write_bytes(elf.numbered_sections(elf.with_inline_addends(data)))
            self.objects[str(out / name)] = elf.Elf((out / name).read_bytes())
        self.port_objects = {self._file_of(out / name): self.objects[str(out / name)] for path, name in inputs
                             if self._file_of(out / name) in self.files}
        self.log(f"compiled {len(objects)} objects for variant {self.code:#010x}")

    def _shared_overlay_data(self, data):
        """The relinked overlay 27 functions keep using the game's own overlay 27 variables.

        The assembly reaches its writable .data through its section symbol, so a relinked
        copy would otherwise get private variables the rest of overlay 27 never sees. The
        patch leaves .data unchanged; each reference is resolved to the retail copy."""
        pristine = elf.Elf((self.pristine / self.build_dir / "main.nef.p/asm_overlay_27.s.o").read_bytes())
        port = elf.Elf(data)
        section = lambda obj: next(i for i, s in enumerate(obj.sections) if s["name"] == ".data")
        old, new = section(pristine), section(port)
        body = pristine.section_bytes(old)
        require(port.section_bytes(new) == body, "the patch changes overlay 27's .data")
        # Where the retail functions point: each literal the pristine code relocates
        # against .data holds the retail address plus its addend.
        found = set()
        text = next(i for i, s in enumerate(pristine.sections) if s["name"] == ".text")
        for name, shndx, start, size, _, _ in pristine.functions():
            if shndx != text or (ASM, name) not in self.located:
                continue
            at, _, module, _ = self.located[(ASM, name)]
            base, image = self.modules[module]
            for offset, kind, index, addend in pristine.relocs.get(shndx, ()):
                symbol = pristine.symbols[index]
                if start <= offset < start + size and symbol["shndx"] == old and kind == elf.R_ARM_ABS32:
                    word = nds.u32(image, at - base + offset - start)
                    found.add(word - addend - symbol["value"])
        require(len(found) == 1, f"retail overlay 27 points at its .data inconsistently: {sorted(map(hex, found))}")
        retail = found.pop()
        data = bytearray(data)
        changed = 0
        for rela in port.sections:
            if rela["type"] != elf.SHT_RELA or rela["info"] not in port.relocs:
                continue
            target = port.sections[rela["info"]]
            for k, (offset, info, addend) in enumerate(struct.iter_unpack("<IIi", port.data[rela["off"]:rela["off"] + rela["size"]])):
                symbol = port.symbols[info >> 8]
                if symbol["shndx"] != new:
                    continue
                require(info & 0xFF == elf.R_ARM_ABS32, "overlay 27 reaches its .data other than by address")
                struct.pack_into("<I", data, target["off"] + offset, retail + symbol["value"] + addend)
                struct.pack_into("<I", data, rela["off"] + 12 * k + 4, info & ~0xFF | elf.R_ARM_NONE)
                changed += 1
        self.log(f"overlay 27 uses the game's .data at {retail:#x} ({changed} references)")
        return bytes(data)

    # --- 3. Resolve and link both regions ----------------------------------------------------------------
    def resolve(self, name):
        if name == "SDK_OVERLAY_wonder_trade_ID":
            return self.overlay_id, False
        return super().resolve(name)

    def _overlay_id(self, symbol):
        oid = symbol.address
        require(any(row[0] == oid for row in nds.overlays(self.rom)), f"overlay {oid} ({symbol.name}) is not in the retail game")
        return oid

    def _autoload_start(self, symbol):
        rom = self.rom
        footer = rom[nds.u32(rom, 0x20) + nds.u32(rom, 0x2C):][:12]
        arm9 = self.modules["arm9"][1] + footer
        _, start, end = nds.autoload_table(arm9)
        starts = [nds.u32(arm9, at) for at in range(start, end, 12)]
        require(symbol.address in starts, f"{symbol.name} {symbol.address:#x} is not an autoload of the retail game")
        return symbol.address

    def link(self):
        build = self.workspace / self.build_dir
        metroskrew = self.workspace / "tools/metroskrew/lib/metroskrew/sdk/ds/2.0"
        library = sdk.Library([build / "subprojects/NitroSDK-4.2.30001/libraries/os/libos.a",
                               build / "subprojects/NitroSDK-4.2.30001/libraries/card/libcard.a",
                               build / "subprojects/NitroWiFi-2.1.30003/libraries/soc/libsoc.a",
                               metroskrew / "sp1p2/msl/MSL_C/MSL_ARM/Lib/MSL_C_NITRO_Ai_LE.a",
                               metroskrew / "sp2/Mathlib/lib/FP_fastI_v5t_LE.a"])
        out = self.work / "link"
        rows = {row[0]: row for row in nds.overlays(self.rom)}
        self.overlay_id = max(rows) + 1
        self.overlay_fid = nds.u32(self.rom, 0x4C) // 8
        self.base = platinum.retail_arena(self.rom)
        _, ram, size, bss, _, _ = rows[0]
        self.overlay_base = nds.align(ram + size + bss, 32)
        extracted, resolved, functions = [], {}, set()
        for round_ in range(6):
            datas = [obj.data for obj in self.objects.values()] + [Path(p).read_bytes() for p in extracted]
            defined, needed = link.defined_and_needed(datas)
            missing = []
            for name in sorted(needed):
                if name in resolved:
                    continue
                found = self.resolve(name)
                if found is None:
                    missing.append(name)
                else:
                    resolved[name] = found[0]
                    if found[1]:
                        functions.add(name)
            if not missing:
                break
            unknown = [name for name in missing if name not in library.globals]
            require(not unknown, f"cannot place {', '.join(unknown)} in the retail game")
            source = out / f"sdk{round_}.s"
            source.write_text(library.extract(missing, external=platinum.Placeable(self, defined)))
            extracted.append(str(toolchain.assemble(source, source.with_suffix(".o"))))
            self.log(f"copied SDK routines {', '.join(missing)}")
        else:
            raise PortError("SDK routines kept needing more routines")
        self.resolved = resolved
        source = out / "game-symbols.s"
        source.write_text(link.symbol_source(resolved, functions))
        game = toolchain.assemble(source, source.with_suffix(".o"))
        overlay = [f"*/{path.replace('/', '_')}.o" for path in OVERLAY_SOURCE_PATHS] + [f"*/{OVERLAY_TLS}*.o"]
        firsts = [Path(name).name.replace("/", "_") + ".o" for name in self.files if name != ASM] + ["asm_overlay_27.s.o"]
        script = out / "blob.ld"
        script.write_text(link.region_script([("blob", self.base, None, firsts), ("ovl", self.overlay_base, overlay, [])]))
        paths = list(self.objects) + extracted + [str(game)]
        images = {"blob": (".blob_text", ".blob_rodata", ".blob_data"), "ovl": (".ovl_text", ".ovl_rodata", ".ovl_data")}
        output = toolchain.link(paths, script, out / "blob.elf", images=images)
        objects = dict(self.objects)
        for path in extracted:
            objects[path] = elf.Elf(Path(path).read_bytes())
        nm = Path(f"{output}.nm").read_text()
        self.overlay_binary = Path(f"{output}.ovl").read_bytes()
        self.linked = link.Linked(self.base, Path(f"{output}.blob").read_bytes(), nm, Path(f"{output}.map").read_text(),
                                  objects, segments=[(self.overlay_base, self.overlay_binary)])
        self.port_paths = {name: str(out / (name.replace("/", "_") + ".o")) for name in self.files if name != ASM}
        self.port_paths[ASM] = str(out / "asm_overlay_27.s.o")
        names = self.linked.symbols
        self.overlay_end = names["__ovl_end"]
        # Each input must be where it was meant to go: the resident code runs while the
        # overlay is not loaded.
        in_overlay = lambda path: Path(path).name.startswith(OVERLAY_TLS) or Path(path).name in {
            f"{name.replace('/', '_')}.o" for name in OVERLAY_SOURCE_PATHS}
        misplaced = []
        for (path, section), places in self.linked.sections.items():
            if not section.startswith((".text", ".rodata", ".data", ".bss")):
                continue
            low, high = (self.overlay_base, self.overlay_end) if in_overlay(path) else (self.base, names["__blob_end"])
            misplaced += [f"{Path(path).name} at {at:#x}" for at, size in places if not low <= at and at + size <= high]
        require(not misplaced, "inputs linked into the wrong region: " + ", ".join(misplaced[:5]))
        require(self.overlay_end <= self.base, f"the wonder_trade overlay ends at {self.overlay_end:#x}, after the arena start {self.base:#x}")
        self.log(f"overlay {self.overlay_id} {self.overlay_base:#x}-{self.overlay_end:#x}")

    # --- 4. Layout ----------------------------------------------------------------------------------------
    def derive_layout(self):
        us_at, us_value = platinum.heap_count(elf.Elf((self.pristine / self.build_dir / "main.nef").read_bytes()),
                                              self.map.us_modules["arm9"], HEAP_FUNCTION)
        workspace_rom = (self.workspace / self.build_dir / f"poke{self.game}.us.nds").read_bytes()
        _, wt_value = platinum.heap_count(elf.Elf((self.workspace / self.build_dir / "main.nef").read_bytes()),
                                          nds.modules(workspace_rom)["arm9"], HEAP_FUNCTION)
        require(us_value < wt_value < 256, f"heap count {us_value} -> {wt_value} is not an increase")
        us_function = self.image.unique(HEAP_FUNCTION)
        at = self.map.addr.get(us_function.num) if us_function else None
        require(at is not None, f"{HEAP_FUNCTION} was not found in the retail game")
        self.heap_site, self.heap_count = ((at & ~1) + us_at - us_function.start, us_value), wt_value
        self.text = self._message_archive()
        icon = self.workspace / self.build_dir / "files/wonder_trade/menu_icon.narc"
        icon_members, _ = nds.narc_split(icon.read_bytes())
        require(len(icon_members) == 1, "the menu icon NARC should hold one graphic")
        self.icons = nds.narc_join([*self.icon_members, icon_members[0]], self.icon_names)
        self.menu_label = "(X menu icon)"

    def _message_archive(self):
        work = self.work / "text"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        strings = json.loads((ROOT / "client/files/msgdata/msg/msg_0829.json").read_bytes())
        for message in strings["messages"]:
            message["en_US"] = message[self.text_key]
        (work / "wonder_trade.json").write_text(json.dumps(strings, ensure_ascii=False, indent=2))
        tool = f"/workspace/{self.build_dir}/tools/msgenc"
        toolchain.docker({str(self.workspace): "/workspace", str(work.resolve()): "/text"},
                         f"{tool}/msgenc -e --json -c {tool}/charmap.txt wonder_trade.json wonder_trade.bin", workdir="/text")
        return nds.narc_join([*self.text_members, (work / "wonder_trade.bin").read_bytes()], self.text_names)

    # --- 5. Assemble the ROM --------------------------------------------------------------------------------
    def assemble_rom(self):
        rom = self.rom
        a9_offset, a9_size = nds.u32(rom, 0x20), nds.u32(rom, 0x2C)
        footer = rom[a9_offset + a9_size:a9_offset + a9_size + 12]
        require(nds.u32(footer, 0) == nds.NITRO_FOOTER, "the ARM9 binary has no Nitro footer")
        arm9 = bytearray(self.modules["arm9"][1])
        struct.pack_into("<I", arm9, nds.module_params(arm9) + 0x14, 0)  # stored expanded from now on
        load_end, blob_end = self.linked.symbols["__blob_load_end"], self.linked.symbols["__blob_end"]
        require(len(self.linked.binary) <= load_end - self.base, "module image overruns its load end")
        trampolines, patches = b"", []
        first = nds.align(blob_end, 32)
        for key in self.redirected:
            at, _, module, _ = self.located[key]
            stub = first + len(trampolines)
            trampolines += nds.arm_trampoline(self.blob_functions[key])
            patches.append((module, at, nds.thumb_entry_veneer(at, stub)))
        end = nds.align(first + len(trampolines), 32)
        segment = self.linked.binary + bytes(first - self.base - len(self.linked.binary)) + trampolines
        segment += bytes(end - self.base - len(segment))
        overlays = {}
        for module, at, code in patches:
            if module == "arm9":
                arm9[at - nds.ARM9_RAM:at - nds.ARM9_RAM + len(code)] = code
            else:
                base, body = self.modules[module]
                body = overlays.setdefault(module, bytearray(body))
                body[at - base:at - base + len(code)] = code
        site, value = self.heap_site
        require(arm9[site - nds.ARM9_RAM:site - nds.ARM9_RAM + 2] == bytes([value, 0x22]), "unexpected heap count instruction")
        arm9[site - nds.ARM9_RAM] = self.heap_count
        literal = platinum.arena_literal(bytes(arm9), self.base)
        image = add_autoload(bytes(arm9) + footer, segment, self.base, literal, end)
        self.arena, self.trampolines = end, len(patches)
        table_at, table_size = nds.u32(rom, 0x50), nds.u32(rom, 0x54)
        table = bytearray(rom[table_at:table_at + table_size])
        files = {self.text_fid: self.text, self.icon_fid: self.icons}
        for oid, body in overlays.items():
            entry = next(i for i in range(0, len(table), 32) if nds.u32(table, i) == oid)
            struct.pack_into("<I", table, entry + 8, len(body))
            struct.pack_into("<I", table, entry + 28, nds.u32(table, entry + 28) & ~0xFFFFFF & ~COMPRESSED)
            files[nds.u32(table, entry + 24)] = bytes(body)
        bss = self.linked.symbols["__ovl_end"] - self.linked.symbols["__ovl_load_end"]
        table += nds.overlay_entry(self.overlay_id, self.overlay_base, len(self.overlay_binary), bss, self.overlay_fid)
        files[self.overlay_fid] = self.overlay_binary
        return nds.rebuild(rom, image, bytes(table), files)

    def headroom(self, arena):
        """Main RAM left after the heaps, heap tables, task queues and the file system table HGSS sets up."""
        params = self.image.unique("sDefaultHeapSpec")
        require(params is not None and params.size % 8 == 0, "sDefaultHeapSpec was not found")
        base, body = self.map.us_modules["arm9"]
        rows = [struct.unpack_from("<II", body, params.start - base + k) for k in range(0, params.size, 8)]
        require(all(arena_id == platinum.OS_ARENA_MAIN for _, arena_id in rows), "a heap is outside main RAM")
        usable = len(rows) + 24
        tables = (usable + 1) * 4 + usable * 4 + usable * 4 + self.heap_count * 2 + self.heap_count
        # Task queues of 160, 32, 32 and 4 tasks (a 32-byte task each, generously), and the
        # file system's overlay and file tables.
        queues = 32 * (160 + 32 + 32 + 4) + 4 * 64
        fs = nds.u32(self.rom, 0x44) + nds.u32(self.rom, 0x4C) + 8 * 32 + 0x100
        reserved = platinum.PRE_HEAP_PADDING + nds.align(tables, 4) + sum(size for size, _ in rows) + queues + fs
        arena_hi = self.image.byname["SDK_SECTION_ARENA_EX_START"][0].address
        return arena_hi - arena - reserved

    def build(self):
        self.replaced_files()
        self.text_banks()
        self.map_symbols()
        self.check_variant()
        self.witnesses()
        self.compile_port()
        self.link()
        self.check_link()
        self.derive_layout()
        rom = self.assemble_rom()
        free = self.headroom(self.arena)
        require(free >= MIN_HEADROOM, f"only {free} bytes of main RAM stay free; at least {MIN_HEADROOM} are needed")
        self.verify(rom)
        self.log(f"module {self.base:#x}-{self.arena:#x}, overlay {self.overlay_id} at {self.overlay_base:#x}-"
                 f"{self.overlay_end:#x}, {self.trampolines} redirected functions, {free // 1024} KiB of main RAM free")
        return rom

    def verify(self, rom):
        """Read the new ROM back: only veneers, the heap count, the arena and the autoload list changed,
        each redirect reaches its function, and the new overlay holds the linked image."""
        new = nds.modules(rom)
        require(set(new) == set(self.modules) | {self.overlay_id}, "the module list changed unexpectedly")
        for module, (base, body) in self.modules.items():
            nbase, nbody = new[module]
            require(nbase == base, f"{module} moved")
            if module == "arm9":
                nbody = nbody[:len(body)]
            allowed = set()
            for key in self.redirected:
                at, _, owner, _ = self.located[key]
                if owner == module:
                    allowed.update(range(at - base, at - base + platinum.VENEER_SIZE))
            if module == "arm9":
                params = nds.module_params(body)
                allowed.update(range(params, params + 8))  # the autoload list moved
                allowed.update(range(params + 0x14, params + 0x18))  # no longer compressed
                allowed.add(self.heap_site[0] - base)
                literal = platinum.arena_literal(body, self.base)
                allowed.update(range(literal, literal + 4))
                _, start, stop = autoload_bounds(body, params)
                allowed.update(range(start, stop))
            stray = [i for i in range(min(len(body), len(nbody))) if body[i] != nbody[i] and i not in allowed]
            require(not stray, f"{module} changed outside the expected sites at {base + stray[0]:#x}" if stray else "")
        require(new[self.overlay_id] == (self.overlay_base, self.overlay_binary), "the wonder_trade overlay is wrong")
        for key in self.redirected:
            at, _, module, _ = self.located[key]
            base, body = new[module]
            decoded = elf.thumb_bl_target(body[at - base + 2:at - base + 6], at + 2)
            require(decoded and decoded[0] == "blx", f"{key[1]} has no veneer")


def autoload_bounds(arm9, params):
    return params, nds.u32(arm9, params) - nds.ARM9_RAM, nds.u32(arm9, params + 4) - nds.ARM9_RAM


def add_autoload(arm9, segment, address, arena_literal, arena):
    """nds.add_autoload for an ARM9 binary (footer included) that is not in a ROM yet."""
    size = len(arm9) - 12
    arm9 = bytearray(arm9)
    params, start, end = nds.autoload_table(arm9)
    require(nds.u32(arm9, arena_literal) == address, "arena literal does not hold the arena start")
    struct.pack_into("<I", arm9, arena_literal, arena)
    footer = arm9[size:]
    table = arm9[start:end] + struct.pack("<III", address, len(segment), 0)
    arm9 = arm9[:start] + segment
    new_start = nds.ARM9_RAM + len(arm9)
    arm9 += table
    struct.pack_into("<II", arm9, params, new_start, new_start + len(table))
    return bytes(arm9) + footer
