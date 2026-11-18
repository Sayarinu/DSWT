"""Add Wonder Trade to a retail Platinum that pret cannot build, using the US Rev 1 build as the map.

The Wonder Trade objects and the upstream files the client patch changes are
compiled for the target language and linked into a module that loads after the
game's static code. Each replaced upstream function starts with a jump into the
module. Every address comes from matching the pristine US build against the
target, and every check must pass before a ROM is written.
"""

import collections
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from . import elf, link, nds, sdk, symbols, toolchain

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("prepare_game", ROOT / "scripts/prepare-game.py")
prepare_game = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(prepare_game)
RETAIL = ROOT / "client/retail"
# Port-only source changes: keep the retail StartMenu layout that unpatched code reads.
PORT_PATCH = RETAIL / "platinum-port.patch"
# id: (catalogue variant, language enumerator, text column, build deltas)
EUROPEAN = "platinum-european-build.patch"
LANGUAGES = {
    "fr": ("france", "LANGUAGE_FRENCH", "fr_FR", (EUROPEAN,)),
    "de": ("germany", "LANGUAGE_GERMAN", "de_DE", (EUROPEAN,)),
    "it": ("italy", "LANGUAGE_ITALIAN", "it_IT", (EUROPEAN,)),
    "es": ("spain", "LANGUAGE_SPANISH", "es_ES", (EUROPEAN,)),
    "ja": ("japan", "LANGUAGE_JAPANESE", "ja_JP", ("platinum-japanese-build.patch",)),
    "ko": ("platinum-korea", "LANGUAGE_KOREAN", "ko_KR", (EUROPEAN, "platinum-korean-build.patch")),
}
# Game functions that differ from US Rev 1 only in reviewed, language-specific details. Each is
# placed where the retail callers point, and only if exactly this many of its bytes differ.
REVIEWED = {
    "ja": {
        "MessageLoader_GetSpeciesName": 1,  # the species name bank number (412 -> 408)
        "String_FormatInt": 1,  # a literal data address
        "Text_AddPrinterWithParamsAndColor": 17,  # leaves the template's unused last byte alone
    },
    "ko": {
        "MessageLoader_GetSpeciesName": 1,  # the species name bank number (412 -> 408)
    },
}
US_VARIANT = "us-rev1"
TEXT_ARCHIVE = "msgdata/pl_msg.narc"
MENU_LABEL = "WonderTrade_Text_MenuLabel"
MENU_ENTRY = "StartMenu_Text_WonderTrade"
VENEER_SIZE = 6
# InitSystem reserves this after its heaps: four system task managers, each a
# 52-byte header plus a 4-byte pointer and 28-byte task per slot, and at most
# 256 bytes of random padding before the heap tables.
TASK_SLOT, TASK_MANAGER, PRE_HEAP_PADDING = 32, 52, 256
# Main RAM left after the game's own allocations must stay at least this large.
MIN_HEADROOM = 32 * 1024
OS_ARENA_MAIN = 0


class PortError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PortError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git(workspace, *args):
    return subprocess.run(["git", "-C", str(workspace), *args], check=True, capture_output=True).stdout


def enum_values(text, name):
    """{enumerator: value} of a C enum, following explicit values and increments."""
    body = re.search(r"enum\s+" + name + r"\s*{(.*?)}", text, re.S)
    require(body, f"enum {name} not found")
    values, current = {}, -1
    for item in re.sub(r"//.*", "", body.group(1)).split(","):
        item = item.strip()
        if not item:
            continue
        key, _, value = item.partition("=")
        current = int(value.strip(), 0) if value.strip() else current + 1
        values[key.strip()] = current
    return values


def variant_code(versions_header, language_name, header_revision):
    """The enrolment game variant: family << 24 | version << 16 | language << 8 | revision."""
    versions = enum_values(versions_header, "Version")
    languages = enum_values(versions_header, "Language")
    return 2 << 24 | versions["VERSION_PLATINUM"] << 16 | languages[language_name] << 8 | header_revision


def text_bank_ids(header):
    return {name: int(value) for name, value in re.findall(r"#define (TEXT_BANK_\w+)\s+(\d+)", header)}


def text_bank_remap(us_members, members):
    """US text bank number -> retail number. Japanese and Korean leave some banks out, so later
    banks move; every language keeps each bank's encryption key (its second header word)."""
    key = lambda member: struct.unpack_from("<H", member, 2)[0]
    matcher = difflib.SequenceMatcher(None, [key(m) for m in us_members], [key(m) for m in members], autojunk=False)
    remap = {}
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            remap.update(zip(range(i1, i2), range(j1, j2)))
    return remap


def remapped_text_banks(header, remap, wonder_trade):
    """generated/text_banks.h with every bank renumbered; a bank the retail game lacks gets 0xFFFF."""
    def number(name, value):
        return wonder_trade if name == "TEXT_BANK_WONDER_TRADE" else remap.get(int(value), 0xFFFF)
    header = re.sub(r"^(\s+(TEXT_BANK_\w+)\s+= )(\d+),", lambda m: f"{m[1]}{number(m[2], m[3])},", header, flags=re.M)
    return re.sub(r"^(#define (TEXT_BANK_\w+)\s+)(\d+)$", lambda m: f"{m[1]}{number(m[2], m[3])}", header, flags=re.M)


def arena_literal(arm9, arena):
    """The single word in the static binary that holds the arena start."""
    found = [i for i in range(0, len(arm9) - 3, 4) if nds.u32(arm9, i) == arena]
    require(len(found) == 1, f"expected one arena literal for {arena:#x}, found {len(found)}")
    return found[0]


def retail_arena(rom):
    """Where main RAM is free after the static binary, its autoloads and every overlay."""
    ends = [ram + size + bss for _, ram, size, bss, _, _ in nds.overlays(rom) if nds.ARM9_RAM <= ram < 0x02400000]
    return max(ends)


def heap_count(nef, arm9, function="InitHeapSystem"):
    """(address, value) of the movs r2, #HEAP_ID_MAX that passes the heap count to Heap_InitSystem."""
    found = [s for s in nef.symbols if s["name"] == function and s["type"] in symbols.FUNCTION_KINDS]
    require(len(found) == 1, f"the build has no single {function}")
    start, size = found[0]["value"] & ~1, found[0]["size"]
    calls = [offset for offset, _, index, _ in nef.relocs.get(found[0]["shndx"], ())
             if start <= offset < start + size and nef.symbols[index]["name"] == "Heap_InitSystem"]
    require(len(calls) == 1, f"{function} does not call Heap_InitSystem once")
    base, body = arm9
    for at in range(calls[0] - 2, max(start, calls[0] - 8) - 1, -2):
        half = nds.u16(body, at - base)
        if half >> 8 == 0x22:  # movs r2, #imm8: the third argument
            return at, half & 0xFF
    raise PortError("the heap count argument of Heap_InitSystem was not found")


class Target:
    """One retail Platinum to port: its dump, the pristine US map and the prepared workspace."""

    # Passed to every toolchain.compile_objects call; other games override it.
    compile_options = {}

    def __init__(self, language, rom_path, workspace, pristine, work, log=print):
        require(language in LANGUAGES, f"unsupported language {language}")
        self.language = language
        self.variant_id, self.language_name, self.text_key, deltas = LANGUAGES[language]
        self.deltas = [RETAIL / name for name in deltas]
        layout = json.loads((ROOT / "protocol/save-layout.json").read_text())
        korean = self.variant_id.endswith(layout["korean_target_suffix"])
        self.save_magic = layout["footer_magic"]["korean" if korean else "international"]
        self.workspace, self.pristine, self.work = (Path(p).resolve() for p in (workspace, pristine, work))
        self.log = log
        catalogue = {row["id"]: row for row in json.loads((ROOT / "patches/compatibility.json").read_text())["variants"]}
        self.variant = catalogue[self.variant_id]
        self.rom = Path(rom_path).read_bytes()
        require(len(self.rom) == self.variant["size"] and sha256(self.rom) == self.variant["sha256"],
                f"{rom_path} is not the catalogued {self.variant['game']} {self.variant['name']} dump")
        require(self.variant.get("port_base") == US_VARIANT, f"{self.variant_id} is not ported from {US_VARIANT}")
        us_path = self.pristine / "build/pokeplatinum.us.nds"
        self.us_rom = us_path.read_bytes()
        require(sha256(self.us_rom) == catalogue[US_VARIANT]["sha256"],
                f"{us_path} is not a pristine {US_VARIANT} build; build it with ROM_REVISION=1 --pristine")
        pin = json.loads((ROOT / "client/upstream.json").read_text())["revision"]
        for tree in (self.workspace, self.pristine):
            require(git(tree, "rev-parse", "HEAD").decode().strip() == pin, f"{tree} is not at upstream {pin}")
        self.modules = nds.modules(self.rom)

    # --- Mapping -------------------------------------------------------------------------------
    def map_symbols(self):
        self.image = symbols.UsImage((self.pristine / "build/main.nef").read_bytes(), self.us_rom)
        self.map = symbols.SymbolMap(self.image, self.us_rom, self.rom)
        counts = collections.Counter(how.split()[0] for how in self.map.how.values())
        self.log(f"mapped {len(self.map.addr)} US symbols: {dict(counts)}")

    def replaced_files(self):
        """Upstream sources and headers the client patch changes, from the workspace's own diff.

        The workspace must hold exactly what prepare-game.py puts there from client/ now."""
        _, desired = prepare_game.overlay_files(ROOT, self.workspace)
        stale = sorted(name for name, content in desired.items()
                       if not (self.workspace / name).is_file() or (self.workspace / name).read_bytes() != content)
        require(not stale, f"the workspace is older than client/ ({', '.join(stale[:3])}); rebuild it with build-rom.sh")
        changed = git(self.workspace, "diff", "--name-only", "HEAD", "--", "src", "include").decode().split()
        self.files = sorted(name for name in changed if name.startswith("src/") and name.endswith(".c"))
        self.headers = sorted(name for name in changed if name.startswith("include/") and name.endswith(".h"))
        require(self.files, "the workspace changes no upstream sources; prepare it with prepare-game.py")

    def text_banks(self):
        """Renumber the text banks the module uses when the retail archive numbers them differently."""
        header = (self.workspace / "build/generated/text_banks.h").read_text()
        ids = text_bank_ids(header)
        self.text_fid = nds.file_ids(self.rom)[TEXT_ARCHIVE]
        start, end = nds.file_span(self.rom, self.text_fid)
        self.text_members, self.text_names = nds.narc_split(self.rom[start:end])
        us_start, us_end = nds.file_span(self.us_rom, nds.file_ids(self.us_rom)[TEXT_ARCHIVE])
        us_members, _ = nds.narc_split(self.us_rom[us_start:us_end])
        require(len(us_members) == ids["TEXT_BANK_WONDER_TRADE"], "the US text archive does not end where Wonder Trade's bank goes")
        remap = text_bank_remap(us_members, self.text_members)
        if all(remap.get(i) == i for i in range(len(us_members))) and len(self.text_members) == len(us_members):
            self.banks_header, self.bank_ids = None, ids
            return
        require(remap.get(ids["TEXT_BANK_START_MENU"]) is not None, "the retail game has no start menu text bank")
        self.banks_header = remapped_text_banks(header, remap, len(self.text_members))
        self.bank_ids = text_bank_ids(self.banks_header)
        self.log(f"text banks renumbered: start menu {ids['TEXT_BANK_START_MENU']} -> {self.bank_ids['TEXT_BANK_START_MENU']}, "
                 f"Wonder Trade {ids['TEXT_BANK_WONDER_TRADE']} -> {self.bank_ids['TEXT_BANK_WONDER_TRADE']}")

    def _stage(self, tree, files, read, patches):
        shutil.rmtree(tree, ignore_errors=True)
        touched = [name for patch in self.deltas for name in prepare_game.patched_paths(patch)]
        for name in dict.fromkeys([*files, *touched]):
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            (tree / name).write_bytes(read(name))
        (tree / "include").mkdir(exist_ok=True)
        for patch in patches:
            result = subprocess.run(["patch", "-s", "-p1", "--fuzz=0", "--no-backup-if-mismatch", "-d", str(tree), "-i", str(patch)],
                                    capture_output=True, text=True)
            require(result.returncode == 0, f"{patch.name} does not apply: {result.stdout}{result.stderr}")
        if self.banks_header is not None:
            (tree / "include/generated").mkdir(parents=True, exist_ok=True)
            (tree / "include/generated/text_banks.h").write_text(self.banks_header)

    # --- 1. The language's upstream code must compile to the retail bytes ------------------------
    def check_variant(self):
        tree = self.work / "pristine"
        self._stage(tree, self.files + self.headers, lambda name: git(self.workspace, "show", f"HEAD:{name}"), self.deltas)
        objects = toolchain.compile_objects(self.workspace, tree, tree / "include", self.work / "pristine-obj",
                                            self.language_name, self.files, **self.compile_options)
        self.pristine_objects = {self._file_of(path): elf.Elf(path.read_bytes()) for path in objects}
        self.located, self.located_data, problems = {}, {}, []
        for name in self.files:
            problems += self._locate(name, self.pristine_objects[name])
        places = collections.Counter((module, at) for at, _, module, _ in self.located.values())
        problems += [f"two functions were placed at {at:#x}" for (_, at), count in places.items() if count > 1]
        require(not problems, "upstream code differs from the retail game:\n  " + "\n  ".join(problems))
        self.log(f"{len(self.located)} functions and {len(self.located_data)} data objects match the retail game")

    @staticmethod
    def _file_of(path):
        return Path(path).name.removesuffix(".o").replace("src_", "src/", 1)

    def _us_candidates(self, name, kinds, module=None):
        found = [c for c in self.image.byname.get(name, ()) if c.kind in kinds]
        return [c for c in found if module is None or self.image.module(c) == module]

    def _locate(self, filename, obj):
        """Find each function and data object of one compiled file in the retail game."""
        problems = []
        functions = obj.functions()
        unique = [self._us_candidates(n, symbols.FUNCTION_KINDS) for n, *_ in functions]
        anchors = [c[0] for c in unique if len(c) == 1]
        require(anchors, f"{filename} has no uniquely named function")
        modules = {self.image.module(c) for c in anchors}
        require(len(modules) == 1, f"{filename} spans modules {modules}")
        module = modules.pop()
        near = sorted(c.start for c in anchors)[len(anchors) // 2]
        base, image = self.modules[module]
        for name, shndx, offset, size, _, _ in functions:
            candidates = sorted(self._us_candidates(name, symbols.FUNCTION_KINDS, module), key=lambda c: abs(c.start - near))
            if not candidates:
                problems.append(f"{filename}: {name} is not in the US build")
                continue
            us = candidates[0]
            at = self.map.addr.get(us.num)
            if at is None:
                at = self._by_neighbour(us)
            if at is None:
                problems.append(f"{filename}: {name} was not found in the retail game")
                continue
            at &= ~1
            body = obj.section_bytes(shndx)[offset:offset + size]
            mask = relocated(obj, shndx)
            got = image[at - base:at - base + size]
            differ = [i for i in range(size) if offset + i not in mask and (i >= len(got) or body[i] != got[i])]
            if differ:
                problems.append(f"{filename}: {name} at {at:#x} differs from +{differ[0]:#x} ({len(differ)} bytes)")
            self.located[(filename, name)] = (at, size, module, us)
        for symbol in obj.symbols:
            shndx = symbol["shndx"]
            if (symbol["type"] != elf.STT_OBJECT or not symbol["name"] or symbol["name"].startswith(".") or not symbol["size"]
                    or not 0 < shndx < len(obj.sections) or obj.sections[shndx]["type"] == elf.SHT_NOBITS):
                continue
            candidates = sorted(self._us_candidates(symbol["name"], (elf.STT_OBJECT,)), key=lambda c: abs(c.start - near))
            at = self.map.addr.get(candidates[0].num) if candidates else None
            if at is None:
                problems.append(f"{filename}: data {symbol['name']} was not found in the retail game")
                continue
            body = obj.section_bytes(shndx)[symbol["value"]:symbol["value"] + symbol["size"]]
            mask = relocated(obj, shndx)
            # Overlays share addresses: look in the file's own module first.
            got = next((data[at - start:at - start + symbol["size"]] for start, data in [self.modules[module], *self.modules.values()]
                        if start <= at < start + len(data)), b"")
            differ = [i for i in range(symbol["size"]) if symbol["value"] + i not in mask and (i >= len(got) or body[i] != got[i])]
            if differ:
                problems.append(f"{filename}: data {symbol['name']} at {at:#x} differs from +{differ[0]:#x}")
            self.located_data[(filename, symbol["name"])] = (at, candidates[0])
        return problems

    def _by_neighbour(self, us):
        """Place a function at its US distance from the nearest mapped function before it."""
        mapped = [s for s in self.image.syms if s.section == us.section and s.start < us.start
                  and s.num in self.map.addr and s.kind in symbols.FUNCTION_KINDS]
        if not mapped:
            return None
        other = max(mapped, key=lambda s: s.start)
        return (self.map.addr[other.num] & ~1) + (us.start - other.start)

    def witnesses(self):
        """Addresses of what the matched retail functions call and point at, read from the retail bytes."""
        seen, problems = {}, []
        for (filename, name), (at, size, module, _) in self.located.items():
            obj = self.pristine_objects[filename]
            entry = next(f for f in obj.functions() if f[0] == name)
            _, shndx, offset, _, _, _ = entry
            base, image = self.modules[module]
            for position, kind, index, addend in obj.relocs.get(shndx, ()):
                if not offset <= position < offset + size:
                    continue
                symbol = obj.symbols[index]
                if not symbol["bind"] or not symbol["name"] or symbol["shndx"] != 0 and symbol["type"] == elf.STT_SECTION:
                    continue
                site = at + position - offset
                word = image[site - base:site - base + 4]
                if kind == elf.R_ARM_THM_CALL:
                    decoded = elf.thumb_bl_target(word, site)
                    target = decoded[1] if decoded else None
                elif kind == elf.R_ARM_ABS32:
                    target = struct.unpack("<I", word)[0] - addend
                else:
                    target = None
                if target is None:
                    continue
                if seen.setdefault(symbol["name"], target) != target:
                    problems.append(f"{symbol['name']} is referenced at {seen[symbol['name']]:#x} and {target:#x}")
        for name, target in seen.items():
            us = self.image.unique(name)
            mapped = self.map.addr.get(us.num) if us else None
            if mapped is not None and mapped & ~1 != target & ~1:
                problems.append(f"{name}: retail calls {target:#x}, the symbol map says {mapped:#x}")
        require(not problems, "retail references disagree:\n  " + "\n  ".join(problems))
        self.witnessed = seen
        self.log(f"{len(seen)} symbols confirmed by retail references")

    # --- 2. Compile Wonder Trade for this language ------------------------------------------------
    def compile_port(self):
        tree = self.work / "port"
        self._stage(tree, self.files + self.headers, lambda name: (self.workspace / name).read_bytes(), [*self.deltas, PORT_PATCH])
        self.code = variant_code((self.pristine / "include/constants/versions.h").read_text(), self.language_name, self.rom[0x1E])
        tls = subprocess.run(["python3", str(ROOT / "scripts/prepare-tls.py")], check=True, capture_output=True, text=True).stdout.strip()
        objects = toolchain.compile_objects(self.workspace, tree, tree / "include", self.work / "port-obj", self.language_name,
                                            self.files, wonder_trade=True, variant=self.code, tls=tls,
                                            defines=self.port_defines(), **self.compile_options)
        library = self.workspace / "build/src/wonder_trade/libwonder_trade_tls.a.p"
        sources = list(objects) + sorted(library.glob("*.o"))
        require(len(sources) > len(objects), f"{library} has no objects; build the workspace first")
        out = self.work / "link"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        self.objects = {}
        for path in sources:
            name = out / (path.name if path.parent == self.work / "port-obj" else "tls_" + path.name)
            name.write_bytes(elf.numbered_sections(elf.with_inline_addends(path.read_bytes())))
            self.objects[str(name)] = elf.Elf(name.read_bytes())
        self.port_objects = {self._file_of(path): self.objects[str(out / path.name)] for path in objects
                             if self._file_of(path) in self.files}
        self.log(f"compiled {len(objects)} objects for variant {self.code:#010x}")

    def port_defines(self):
        """Defines added when compiling Wonder Trade for this target."""
        return {"WT_SAVE_MAGIC": f"{self.save_magic:#010x}"}

    # --- 3. Resolve and link ------------------------------------------------------------------------
    def resolve(self, name):
        """(address, is code) of a game symbol the module uses, or None."""
        us = self.image.unique(name)
        if name in self.witnessed:
            return self.witnessed[name], us is None or self.image.is_code(us)
        if name.startswith("SDK_OVERLAY_") and name.endswith("_ID") and us is not None:
            return self._overlay_id(us), False
        if name.startswith("SDK_AUTOLOAD_") and name.endswith("_START") and us is not None:
            return self._autoload_start(us), False
        if us is None:
            return None
        if us.num not in self.map.addr and name in REVIEWED.get(self.language, {}):
            at = self.map.voted(us)
            require(at is not None, f"no retail call agrees where {name} is")
            differ = self.map.differing(us, at)
            expected = REVIEWED[self.language][name]
            require(differ == expected, f"{name} at {at:#x} differs from US in {differ} bytes, not the {expected} reviewed")
            return (at & ~1) | int(us.num in self.image.thumb), True
        if us.num in self.map.addr:
            at = self.map.addr[us.num]
            return ((at & ~1) | int(us.num in self.image.thumb)) if self.image.is_code(us) else at, self.image.is_code(us)
        size = self.image.extent(us)
        got = symbols.masked(self.image, self.us_modules, us, size) if size >= 16 else None
        if got is None:
            return None
        module, _, data, keep = got
        at = symbols.unique_match(self.modules, module, data, keep, 2 if us.num in self.image.thumb else 4)
        if at is None:
            return None
        return (at | int(us.num in self.image.thumb)) if self.image.is_code(us) else at, self.image.is_code(us)

    @property
    def us_modules(self):
        return self.map.us_modules

    def _overlay_id(self, symbol):
        oid = symbol.address
        us = {row[0]: row for row in nds.overlays(self.us_rom)}
        target = {row[0]: row for row in nds.overlays(self.rom)}
        require(oid in us and oid in target and us[oid][1] == target[oid][1],
                f"overlay {oid} ({symbol.name}) does not load at the same address in the retail game")
        return oid

    def _autoload_start(self, symbol):
        arm9 = self.rom[nds.u32(self.rom, 0x20):nds.u32(self.rom, 0x20) + nds.u32(self.rom, 0x2C) + 12]
        _, start, end = nds.autoload_table(arm9)
        starts = [nds.u32(arm9, at) for at in range(start, end, 12)]
        require(symbol.address in starts, f"{symbol.name} {symbol.address:#x} is not an autoload of the retail game")
        return symbol.address

    def link(self):
        libraries = [self.workspace / "build/subprojects/NitroSDK-4.2.30001/libraries/os/libos.a",
                     self.workspace / "build/subprojects/NitroSDK-4.2.30001/libraries/card/libcard.a",
                     self.workspace / "build/subprojects/NitroWiFi-2.1.30003/libraries/soc/libsoc.a",
                     self.workspace / "build/MSL_C_NITRO_Ai_LE.a",
                     self.workspace / "tools/metroskrew/lib/metroskrew/sdk/ds/2.0/sp2/Mathlib/lib/FP_fastI_v5t_LE.a"]
        library = sdk.Library(libraries)
        out = self.work / "link"
        self.base = retail_arena(self.rom)
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
            source.write_text(library.extract(missing, external=Placeable(self, defined)))
            extracted.append(str(toolchain.assemble(source, source.with_suffix(".o"))))
            self.log(f"copied SDK routines {', '.join(missing)}")
        else:
            raise PortError("SDK routines kept needing more routines")
        self.resolved = resolved
        source = out / "game-symbols.s"
        source.write_text(link.symbol_source(resolved, functions))
        game = toolchain.assemble(source, source.with_suffix(".o"))
        script = out / "blob.ld"
        firsts = [Path(name).name.replace("/", "_") + ".o" for name in self.files]
        script.write_text(link.linker_script(self.base, firsts))
        paths = list(self.objects) + extracted + [str(game)]
        output = toolchain.link(paths, script, out / "blob.elf")
        objects = dict(self.objects)
        for path in extracted:
            objects[path] = elf.Elf(Path(path).read_bytes())
        self.linked = link.Linked(self.base, Path(f"{output}.bin").read_bytes(), Path(f"{output}.nm").read_text(),
                                  Path(f"{output}.map").read_text(), objects)
        self.port_paths = {name: str(out / (name.replace("/", "_") + ".o")) for name in self.files}

    # --- 4. Gates --------------------------------------------------------------------------------------
    def check_link(self):
        problems = link.check_relocations(self.linked, self.resolved)
        self.blob_functions = {}
        for filename, path in self.port_paths.items():
            for name, at in self.linked.function_addresses(path).items():
                self.blob_functions[(filename, name)] = at
        problems += self._check_unchanged()
        problems += self._check_shared_state()
        require(not problems, "the linked module is wrong:\n  " + "\n  ".join(problems[:40]))
        self.log(f"checked {sum(len(r) for o in self.linked.objects.values() for r in o.relocs.values())} relocations")

    def _same(self, filename, name):
        """True if a function compiles identically with and without the client patch."""
        port, pristine = self.port_objects[filename], self.pristine_objects[filename]
        a = next((f for f in port.functions() if f[0] == name), None)
        b = next((f for f in pristine.functions() if f[0] == name), None)
        if a is None or b is None or a[3] != b[3]:
            return False

        def shape(obj, function):
            _, shndx, offset, size, _, _ = function
            mask = relocated(obj, shndx)
            body = obj.section_bytes(shndx)[offset:offset + size]
            names = [(r[0] - offset, r[1], obj.symbols[r[2]]["name"]) for r in obj.relocs.get(shndx, ())
                     if offset <= r[0] < offset + size]
            return bytes(0 if offset + i in mask else body[i] for i in range(size)), names
        port_shape, pristine_shape = shape(port, a), shape(pristine, b)
        # with_inline_addends rewrote relocated fields, which the masks hide.
        return port_shape == pristine_shape

    def _check_unchanged(self):
        """Functions the client patch leaves alone must call and point where retail does.

        Functions too small for a veneer stay retail, so they must not have changed."""
        problems = []
        moved = {at: self.blob_functions[key] & ~1 for key, (at, *_rest) in self.located.items() if key in self.blob_functions}
        moved.update({at: self.linked.symbols.get(name) for (_, name), (at, _) in self.located_data.items()
                      if self.linked.symbols.get(name) is not None})
        self.redirected = [key for key, (_, size, _, _) in sorted(self.located.items())
                           if key in self.blob_functions and size >= VENEER_SIZE]
        for key, (at, size, module, us) in sorted(self.located.items()):
            if key not in self.blob_functions:
                continue
            same = self._same(*key)
            if size < VENEER_SIZE:
                callers = self._retail_users(us.name, us.address)
                if not same and callers:
                    problems.append(f"{key[0]}: {key[1]} changed, is too small to redirect, "
                                    f"and retail code at {callers[0]:#x} still calls it")
                continue
            if not same:
                continue
            base, image = self.modules[module]
            blob_at = self.blob_functions[key] & ~1
            obj = self.port_objects[key[0]]
            _, shndx, offset, _, _, _ = next(f for f in obj.functions() if f[0] == key[1])
            for position, kind, index, _ in obj.relocs.get(shndx, ()):
                if not offset <= position < offset + size:
                    continue
                k = position - offset
                retail_word = image[at - base + k:at - base + k + 4]
                blob_word = self.linked.word(blob_at + k)
                if kind == elf.R_ARM_THM_CALL:
                    old, new = elf.thumb_bl_target(retail_word, at + k), elf.thumb_bl_target(blob_word, blob_at + k)
                    if old and new and old[0] == new[0] and moved.get(old[1] & ~1, old[1] & ~1) == new[1] & ~1:
                        continue
                    problems.append(f"{key[0]}: {key[1]}+{k:#x} retail {old}, module {new}")
                elif kind == elf.R_ARM_ABS32:
                    old, new = struct.unpack("<I", retail_word)[0], struct.unpack("<I", blob_word)[0]
                    if moved.get(old & ~1, old & ~1) | (old & 1) == new:
                        continue
                    symbol = obj.symbols[index]
                    if (symbol["bind"] == 0 and self.base <= new < self.base + len(self.linked.binary) + 0x10000):
                        continue  # the file's own constant data, copied into the module
                    problems.append(f"{key[0]}: {key[1]}+{k:#x} retail word {old:#x}, module {new:#x} ({symbol['name']})")
        return problems

    def _retail_users(self, name, value):
        """US addresses that reference a symbol from code the module does not replace."""
        if not hasattr(self, "_users"):
            self._users = collections.defaultdict(list)
            for (_, site), rows in self.image.relocs.items():
                for _, other, other_value, _ in rows:
                    self._users[(other, other_value)].append(site)
            self._replaced = [(us.start, us.start + size) for key, (_, size, _, us) in self.located.items()
                              if key in self.redirected]
        return sorted(site for site in self._users.get((name, value), ())
                      if not any(start <= site < end for start, end in self._replaced))

    def _check_shared_state(self):
        """Writable data the module defines must not also be used by retail code that stays."""
        problems = []
        writable = {}
        for filename, obj in self.port_objects.items():
            for symbol in obj.symbols:
                shndx = symbol["shndx"]
                if (symbol["type"] == elf.STT_OBJECT and symbol["name"] and 0 < shndx < len(obj.sections)
                        and obj.sections[shndx]["flags"] & elf.SHF_WRITE):
                    for us in self.image.byname.get(symbol["name"], ()):
                        if us.kind == elf.STT_OBJECT:
                            writable[(symbol["name"], us.address)] = filename
        for (name, value), filename in sorted(writable.items()):
            users = self._retail_users(name, value)
            if users:
                problems.append(f"{filename}: {name} is also used by retail code at {users[0]:#x}; declare it extern")
        return problems

    # --- 5. Assemble the ROM --------------------------------------------------------------------------
    def assemble_rom(self):
        rom = bytearray(self.rom)
        arm9_offset, arm9_size = nds.u32(rom, 0x20), nds.u32(rom, 0x2C)
        symbols_ = self.linked.symbols
        load_end, blob_end = symbols_["__blob_load_end"], symbols_["__blob_end"]
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
        overlays = {row[0]: row for row in nds.overlays(rom)}
        for module, at, code in patches:
            if module == "arm9":
                offset = arm9_offset + at - nds.ARM9_RAM
            else:
                oid, ram, _, _, fid, compressed = overlays[module]
                require(not compressed, f"overlay {oid} is compressed")
                offset = nds.file_span(rom, fid)[0] + at - ram
            rom[offset:offset + len(code)] = code
        # One more heap ID for Wonder Trade.
        site, value = self.heap_site
        require(rom[arm9_offset + site - nds.ARM9_RAM:arm9_offset + site - nds.ARM9_RAM + 2] == bytes([value, 0x22]),
                "unexpected heap count instruction")
        rom[arm9_offset + site - nds.ARM9_RAM] = self.heap_count
        arm9 = rom[arm9_offset:arm9_offset + arm9_size]
        literal = arena_literal(arm9, self.base)
        new_arm9 = nds.add_autoload(bytes(rom), segment, self.base, literal, end)
        self.arena = end
        self.trampolines = len(patches)
        files = {self.text_fid: self.text}
        return nds.relayout(bytes(rom), new_arm9, files)

    def derive_layout(self):
        """The heap count instruction, the text banks and the main RAM headroom."""
        us_at, us_value = heap_count(elf.Elf((self.pristine / "build/main.nef").read_bytes()), self.map.us_modules["arm9"])
        workspace_rom = (self.workspace / "build/pokeplatinum.us.nds").read_bytes()
        _, wt_value = heap_count(elf.Elf((self.workspace / "build/main.nef").read_bytes()), nds.modules(workspace_rom)["arm9"])
        require(us_value < wt_value < 256, f"heap count {us_value} -> {wt_value} is not an increase")
        us_function = self.image.unique("InitHeapSystem")
        at = self.map.addr.get(us_function.num) if us_function else None
        require(at is not None, "InitHeapSystem was not found in the retail game")
        self.heap_site, self.heap_count = ((at & ~1) + us_at - us_function.start, us_value), wt_value
        # Text banks.
        ids = self.bank_ids
        menu = json.loads((self.workspace / "res/text/start_menu.json").read_text())["messages"]
        entry = next(i for i, message in enumerate(menu) if message["id"] == MENU_ENTRY)
        require(entry == len(menu) - 1, f"{MENU_ENTRY} is not the last start menu message")
        members, names = self.text_members, self.text_names
        require(len(members) == ids["TEXT_BANK_WONDER_TRADE"], "the retail text archive does not end where Wonder Trade's bank goes")
        self.text = self._text(members, names, ids["TEXT_BANK_START_MENU"], entry)

    def _text(self, members, names, menu_bank, entry):
        work = self.work / "text"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        (work / "start_menu.bin").write_bytes(members[menu_bank])
        msgenc(self.workspace, work, ["-d", "--json", "start_menu.json", "start_menu.bin"])
        bank = json.loads((work / "start_menu.json").read_text())
        require(len(bank["messages"]) == entry, f"the retail start menu has {len(bank['messages'])} messages, not {entry}")
        strings = json.loads((self.workspace / "res/text/wonder_trade.json").read_bytes())
        label = next(message for message in strings["messages"] if message["id"] == MENU_LABEL)[self.text_key]
        bank["messages"].append({"id": MENU_ENTRY, "en_US": label})
        (work / "start_menu_wt.json").write_text(json.dumps(bank, ensure_ascii=False, indent=2))
        msgenc(self.workspace, work, ["-e", "--json", "start_menu_wt.json", "start_menu_wt.bin"])
        for message in strings["messages"]:
            message["en_US"] = message[self.text_key]
        (work / "wonder_trade.json").write_text(json.dumps(strings, ensure_ascii=False, indent=2))
        msgenc(self.workspace, work, ["-e", "--json", "wonder_trade.json", "wonder_trade.bin"])
        members = list(members)
        members[menu_bank] = (work / "start_menu_wt.bin").read_bytes()
        members.append((work / "wonder_trade.bin").read_bytes())
        self.menu_label = label
        return nds.narc_join(members, names)

    def headroom(self, arena):
        """Main RAM left after InitSystem's heaps, tables and task managers, for a given arena start."""
        params = self.image.unique("sHeapInitParams")
        require(params is not None and params.size % 8 == 0, "sHeapInitParams was not found")
        base, body = self.map.us_modules["arm9"]
        rows = [struct.unpack_from("<II", body, params.start - base + k) for k in range(0, params.size, 8)]
        require(all(arena_id == OS_ARENA_MAIN for _, arena_id in rows), "a heap is outside main RAM")
        usable = len(rows) + 24
        tables = (usable + 1) * 4 + usable * 4 + usable * 4 + self.heap_count * 2 + self.heap_count
        source = (self.workspace / "src/system.c").read_text()
        tasks = [int(n) for n in re.findall(r"#define \w+_TASK_MAX\s+(\d+)", source)]
        require(len(tasks) == 4, "system.c no longer defines four task managers")
        reserved = PRE_HEAP_PADDING + nds.align(tables, 4) + sum(size for size, _ in rows) + sum(
            TASK_MANAGER + TASK_SLOT * n for n in tasks)
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
        self.log(f"module {self.base:#x}-{self.arena:#x}, {self.trampolines} redirected functions, "
                 f"{free // 1024} KiB of main RAM free, start menu label {self.menu_label!r}")
        return rom

    def verify(self, rom):
        """Read the new ROM back: every redirect reaches its module function, all else is unchanged."""
        new = nds.modules(rom)
        require(set(new) == set(self.modules), "the module list changed")
        for module, (base, body) in self.modules.items():
            nbase, nbody = new[module]
            require(nbase == base, f"{module} moved")
            if module == "arm9":
                nbody = nbody[:len(body)]
            changed = [i for i in range(min(len(body), len(nbody))) if body[i] != nbody[i]]
            allowed = set()
            for key in self.redirected:
                at, _, owner, _ = self.located[key]
                if owner == module:
                    allowed.update(range(at - base, at - base + VENEER_SIZE))
            if module == "arm9":
                if self.heap_site is not None:  # Diamond and Pearl keep their heap count
                    allowed.add(self.heap_site[0] - base)
                params, start, end = nds.autoload_table(body + self.rom[nds.u32(self.rom, 0x20) + len(body):][:12])
                allowed.update(range(params, params + 8))  # the autoload list moved
                allowed.update(range(start, end))
                literal = arena_literal(body, self.base)
                allowed.update(range(literal, literal + 4))
            stray = [i for i in changed if i not in allowed]
            require(not stray, f"{module} changed outside the expected sites at {base + stray[0]:#x}" if stray else "")
        arm9 = rom[nds.u32(rom, 0x20):nds.u32(rom, 0x20) + nds.u32(rom, 0x2C) + 12]
        _, start, end = nds.autoload_table(arm9)
        entries = [struct.unpack_from("<III", arm9, at) for at in range(start, end, 12)]
        require(entries[-1][:2] == (self.base, self.arena - self.base), "the module autoload entry is wrong")
        segment = arm9[start - entries[-1][1]:start]
        for key in self.redirected:
            at, _, module, _ = self.located[key]
            base, body = new[module]
            decoded = elf.thumb_bl_target(body[at - base + 2:at - base + 6], at + 2)
            require(decoded and decoded[0] == "blx", f"{key[1]} has no veneer")
            stub = decoded[1] - self.base
            require(nds.u32(segment, stub + 8) == self.blob_functions[key], f"{key[1]} jumps to the wrong place")


class Placeable:
    """Names an SDK copy may leave to the game or the module: defined there, or found in the retail game."""

    def __init__(self, target, defined):
        self.target, self.defined, self.cache = target, defined, {}

    def __contains__(self, name):
        if name not in self.cache:
            self.cache[name] = name in self.defined or self.target.resolve(name) is not None
        return self.cache[name]


def relocated(obj, shndx):
    """Offsets of a section's relocated bytes."""
    mask = set()
    for position, *_ in obj.relocs.get(shndx, ()):
        mask.update(range(position, position + 4))
    return mask


def msgenc(workspace, work, arguments):
    tool = "/workspace/build/tools/msgenc"
    toolchain.docker({str(Path(workspace).resolve()): "/workspace", str(Path(work).resolve()): "/text"},
                     " ".join([f"{tool}/msgenc", *arguments[:2], "-c", f"{tool}/charmap.txt", *arguments[2:]]),
                     workdir="/text")
