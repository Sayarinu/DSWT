"""Add Wonder Trade to Pokémon Black or White (USA/Europe), with aletheia as the map.

aletheia links every region of both cartridges from source at its original
address, so its link output is the retail game's own symbol map. Wonder Trade
(the shared core, the Black/White adapter and Mbed TLS) and the aletheia
sources client/games/aletheia.patch changes are compiled with aletheia's compiler,
linked against those addresses into one module, and loaded by a new ARM9
autoload entry at the start of the main arena. Each changed game function
starts with a jump into the module: in ARM9 main the jump is written into the
cartridge; an overlay's are written into RAM each time it loads, after the
SDK checks its digest, so every overlay file stays the cartridge's own.

Every check must pass before a ROM is written: the dump is the catalogued
one, aletheia's link of every region equals the dump, each changed source
compiles unchanged to the retail bytes, every module reference resolves,
and the new cartridge differs from the dump only where the port wrote.
"""

import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

from .catalog import variants_by_id
from . import aletheia, elf, link, nds

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "client/games/aletheia.patch"
PIN = ROOT / "client/games/upstream-aletheia.json"
# Enroll's game variant: family 4 (Black/White) << 24, the game version (White 20, Black 21)
# << 16, the language (English 2) << 8 and the revision.
TARGETS = {"black": ("black-us", 21), "white": ("white-us", 20)}
FAMILY, LANGUAGE_ENGLISH = 4, 2
MWCC = "tools/mwccarm/mwccarm/dsi/1.1p1/mwccarm.exe"
# The flags pokebw2 builds Wonder Trade with; aletheia's game code uses the same compiler.
CC_FLAGS = ["-O4,p", "-proc", "arm946e", "-thumb", "-interworking", "-enum", "int", "-char", "signed", "-fp", "soft",
            "-lang=c99", "-Cpp_exceptions", "off", "-gccext,on", "-gccinc", "-inline", "on,noauto", "-ipa", "file",
            "-sym", "on", "-requireprotos", "-nolink", "-msgstyle", "gcc"]
TLS_SOURCE_DIRS = ["library", "tf-psa-crypto/core", "tf-psa-crypto/extras", "tf-psa-crypto/platform",
                   "tf-psa-crypto/utilities", "tf-psa-crypto/drivers/builtin/src"]
TLS_INCLUDE_DIRS = ["include", "library", "tf-psa-crypto/include", "tf-psa-crypto/core", "tf-psa-crypto/dispatch",
                    "tf-psa-crypto/extras", "tf-psa-crypto/platform", "tf-psa-crypto/utilities",
                    "tf-psa-crypto/drivers/builtin/include", "tf-psa-crypto/drivers/builtin/src",
                    "tf-psa-crypto/drivers/p256-m", "tf-psa-crypto/drivers/pqcp/include",
                    "tf-psa-crypto/drivers/everest/include"]
# The adapter, with the Black 2/White 2 files it shares: the musical area
# check and the English text.
ADAPTER_SOURCES = ["client/games/src/wonder_trade/bw/*.c", "client/games/src/wonder_trade/bw2/musical_area.c",
                   "client/games/src/wonder_trade/bw2/text.c"]
# Load-time overlay hooks: the module's table and the SDK function they follow.
HOOK_TABLE, HOOK_MAX, HOOK_SIZE = "gWtOverlayHooks", 48, 20
OVERLAY_START = "FS_StartOverlay"
OVERLAY_DETOUR = "WtOverlay_Detour"
ARM_VENEER, THUMB_VENEER = 8, 6
ARENA_FUNCTION = "OS_GetInitArenaLo"
# Main RAM left free after the module and the game's own allocations from the
# main arena must stay at least this large. Measured in melonDS on White with a
# 131 KiB module: 41,680 bytes (0x023D5D30 to 0x023E0000 once the game set up
# its heaps). 16 KiB leaves room for the game's own variation and stops a
# module that grows by about 25 KiB more.
MIN_HEADROOM = 16 * 1024
# The main arena's end in DS mode (OS_GetInitArenaHi for the main arena).
ARENA_HI = 0x023E0000
# The game's heaps (GFL_USE_InitSystem: GFL_HEAP_Init with the DS table of two
# heaps), and what it takes from the arena besides them (the overlay table, the
# heap manager's tables and their alignment), measured with the figure above.
HEAP_TABLE, HEAP_COUNT, ARENA_OTHER = "_ArenaTableDs", 2, 0x15D0
TWL_ALIGN = 0x80000
# Wonder Trade's X-menu icon: Black 2/White 2's art, as a new member of the
# field menu's graphics archive in the format of the Pokémon button's characters.
ICON_ARCHIVE, ICON_TEMPLATE, ICON_MEMBER = "a/0/7/1", 8, 39
ICON_PNG = ROOT / "client/games/res-bw2/graphics/field_menu/43.png"
# The icon's colours 1 to 3 are Black 2/White 2's near black, brown and orange
# (their Pokémon button's palette). Black/White's Pokémon cells draw the first
# frame with palette 6 and the second with 7, whose colours 1 to 3 are brown and
# two oranges, so each frame takes the nearest colours those palettes hold:
# black 14 (palette 7 has grey there, so near black 13), brown 1 and orange 3.
ICON_COLORS = ((0, 14, 1, 3), (0, 13, 1, 3))


class PortError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PortError(message)


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def run(command, cwd=None, env=None):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    if result.returncode:
        noise = re.compile(r"VK_|mvk-|Vulkan|MoltenVK")
        text = "\n".join(line for line in (result.stdout + result.stderr).splitlines() if not noise.search(line))
        raise PortError(f"{Path(command[0]).name} failed:\n{text[-6000:]}")
    return result.stdout


def wine_command(exe):
    """A Windows tool's command: through Wine except on Windows."""
    return [exe] if os.name == "nt" else [os.environ.get("WINE", "wine"), exe]


def tool(name):
    return os.environ.get("ARM_NONE_EABI_PREFIX", "arm-none-eabi-") + name


def arm_veneer(target):
    """ldr pc, [pc, #-4]; .word target: replaces an ARM function's entry."""
    return struct.pack("<II", 0xE51FF004, target)


def relocatable_arm(word):
    """True for an ARM instruction that runs the same at any address: not a branch and no PC operand."""
    if word >> 28 != 0xE or (word >> 25) & 7 in (5,) or (word >> 24) & 0xF == 0xF:
        return False
    if (word >> 26) & 3 == 1 and (word >> 16) & 0xF == 15:
        return False  # a load or store relative to the PC
    if (word >> 26) & 3 == 0 and ((word >> 16) & 0xF == 15 or (word >> 12) & 0xF == 15
                                  or not (word >> 25) & 1 and word & 0xF == 15):
        return False
    if (word >> 25) & 7 == 4 and word >> 15 & 1:
        return False  # a block transfer that loads the PC
    return True


class Target:
    """One retail Black or White to port, with a built aletheia workspace as its map."""

    def __init__(self, target, rom_path, workspace, tls, connection, msl, work, log=print):
        require(target in TARGETS, f"unsupported target {target}")
        self.target, (self.variant_id, self.version) = target, TARGETS[target]
        self.workspace, self.tls, self.connection, self.msl, self.work = (
            Path(p).resolve() for p in (workspace, tls, connection, msl, work))
        self.log = log
        catalogue = variants_by_id(ROOT)
        self.variant = catalogue[self.variant_id]
        self.rom = Path(rom_path).read_bytes()
        require(len(self.rom) == self.variant["size"] and sha1(self.rom) == self.variant["sha1"],
                f"{rom_path} is not the catalogued {self.variant['game']} {self.variant['name']} dump")
        self.code = FAMILY << 24 | self.version << 16 | LANGUAGE_ENGLISH << 8 | self.rom[0x1E]
        pin = json.loads(PIN.read_text())
        head = subprocess.run(["git", "-C", str(self.workspace), "rev-parse", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
        require(head == pin["revision"], f"{self.workspace} is not at aletheia {pin['revision']}")
        require(pin["rom_sha1"][target] == self.variant["sha1"], "the aletheia pin and the catalogue disagree")
        self.modules = nds.modules(self.rom)

    # --- 1. The map -----------------------------------------------------------------------------
    def map_symbols(self):
        self.map = aletheia.SymbolMap(self.workspace, self.target, self.modules)
        self.log(f"aletheia's link of {len(self.map.regions)} regions matches the cartridge")

    # --- 2. aletheia's changed sources ---------------------------------------------------------
    def compile_game_sources(self):
        """Compile each source client/games/aletheia.patch changes, as aletheia has it and as changed."""
        self.sources = [name.removeprefix("src/game/") for name in patched_paths(PATCH)]
        require(all(name.endswith(".c") for name in self.sources), "client/games/aletheia.patch changes a header")
        dirty = subprocess.run(["git", "-C", str(self.workspace), "status", "--porcelain", "--", "src"],
                               capture_output=True, text=True).stdout
        require(not dirty.strip(), f"{self.workspace}/src has local changes")
        changed = self.work / "changed"
        shutil.rmtree(changed, ignore_errors=True)
        for name in self.sources:
            (changed / "src/game" / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.workspace / "src/game" / name, changed / "src/game" / name)
        run(["patch", "-s", "-p1", "--fuzz=0", "--no-backup-if-mismatch", "-d", str(changed), "-i", str(PATCH)])
        self.pristine_units = self._aletheia_cc(self.work / "pristine-units", self.work / "none")
        self.port_units = self._aletheia_cc(self.work / "port-units", changed)
        self.log(f"compiled {len(self.sources)} aletheia sources, unchanged and changed")

    def _aletheia_cc(self, out, changed):
        shutil.rmtree(out, ignore_errors=True)
        lines = run([sys.executable, str(Path(__file__).with_name("aletheia_cc.py")), str(self.workspace),
                     self.target, str(out), str(changed), *self.sources])
        reports = [json.loads(line) for line in lines.splitlines() if line.startswith("{")]
        units = {}
        for report in reports:
            region = aletheia.region_key("arm9_main" if report["region"] == "arm9" else report["region"])
            units[report["source"]] = (region, elf.Elf(Path(report["object"]).read_bytes()), report["object"])
        return units

    def check_game_sources(self):
        """Each unchanged source must be the cartridge's code, and each function the patch
        changes is found: those are redirected into the module."""
        self.redirects, problems, self.witnessed, self.unmapped = [], [], {}, {}
        for name in self.sources:
            region, pristine, _ = self.pristine_units[name]
            _, port, _ = self.port_units[name]
            problems += self._data_defined(name, port)
            for function, shndx, offset, size, thumb, _ in pristine.functions():
                problems += self._matches(name, region, pristine, function, shndx, offset, size, thumb)
            before = {f[0]: f for f in pristine.functions()}
            for function in port.functions():
                if function[0] in before and not same_function(pristine, before[function[0]], port, function):
                    symbol = self.map.get(function[0], region)
                    if symbol is None:
                        problems.append(f"{name}: {function[0]} is not in the cartridge")
                        continue
                    need = THUMB_VENEER if symbol.thumb else ARM_VENEER
                    if symbol.size < need:
                        problems.append(f"{name}: {function[0]} is too small to redirect")
                    self.redirects.append((function[0], symbol))
        problems += [f"{n} has the values {', '.join(hex(a) for a in sorted(v))}" for n, v in self.unmapped.items() if len(v) > 1]
        problems += [f"{n} is read at {', '.join(hex(a) for a in sorted(v))}" for n, v in self.witnessed.items() if len(v) > 1]
        require(not problems, "aletheia's sources differ from the cartridge:\n  " + "\n  ".join(problems[:40]))
        moved = {n: next(iter(a)) for n, a in self.witnessed.items()
                 if self._resolve(n) is not None and a != {self._resolve(n).address}}
        self.data_fixes = {n: aletheia.Symbol(n, a, 0, False, False, self._resolve(n).region) for n, a in moved.items()}
        if self.data_fixes:
            self.log("data the code reads elsewhere than aletheia names it: "
                     + ", ".join(f"{n} {a:#x}" for n, a in moved.items()))
        self.log(f"every function of the {len(self.sources)} sources matches the cartridge; "
                 f"{len(self.redirects)} changed: {', '.join(n for n, _ in self.redirects)}")

    def _data_defined(self, name, obj):
        """A changed source may define no writable data: its copy in the module would not be the game's."""
        problems = []
        for symbol in obj.symbols:
            shndx = symbol["shndx"]
            if (symbol["type"] == elf.STT_OBJECT and symbol["name"] and 0 < shndx < len(obj.sections)
                    and obj.sections[shndx]["flags"] & elf.SHF_WRITE and not symbol["name"].startswith("Wt")):
                problems.append(f"{name}: defines writable {symbol['name']}; declare the game's extern")
        return problems

    def _matches(self, name, region, obj, function, shndx, offset, size, thumb):
        symbol = self.map.get(function, region)
        if symbol is None:
            return [f"{name}: {function} is not in aletheia's link"]
        if symbol.size != size or symbol.thumb != thumb:
            return [f"{name}: {function} is {size} bytes, the cartridge's {symbol.size}"]
        base, image = self.modules[region]
        retail = image[symbol.address - base:symbol.address - base + size]
        body = obj.section_bytes(shndx)[offset:offset + size]
        rows = [r for r in obj.relocs.get(shndx, ()) if offset <= r[0] < offset + size]
        mask = set()
        for position, *_ in rows:
            mask.update(range(position - offset, position - offset + 4))
        if any(i not in mask and body[i] != retail[i] for i in range(size)):
            return [f"{name}: {function} differs from the cartridge"]
        problems = []
        for position, kind, index, addend in rows:
            target = obj.symbols[index]
            if target["type"] == elf.STT_SECTION or not target["name"] or kind == elf.R_ARM_NONE:
                continue
            found = self.map.get(target["name"], region) or self.map.get(target["name"])
            site = symbol.address + position - offset
            word = retail[position - offset:position - offset + 4]
            if found is None:
                # A linker constant or data aletheia's link does not name: the word is its value.
                if kind == elf.R_ARM_ABS32 and target["shndx"] == 0:
                    self.unmapped.setdefault(target["name"], set()).add(struct.unpack("<I", word)[0] - addend)
                continue
            if kind == elf.R_ARM_ABS32:
                got = struct.unpack("<I", word)[0] - addend
                want = found.address | int(found.thumb)
            elif kind == elf.R_ARM_THM_CALL:
                decoded = elf.thumb_bl_target(word, site)
                got, want = (decoded[1] & ~1) if decoded else None, found.address
            elif kind in elf.ARM_BRANCHES:
                decoded = elf.arm_call(struct.unpack("<I", word)[0], site)
                got, want = (decoded[1] & ~1) if decoded else None, found.address
            else:
                continue
            if got is None:
                problems.append(f"{name}: {function}+{position - offset:#x} is no call")
            elif got != want and not found.code and kind == elf.R_ARM_ABS32:
                # aletheia's data-mode link can name a data object a byte or so off where the
                # code reads it; the code's own word is the address.
                self.witnessed.setdefault(target["name"], set()).add(got)
            elif got != want:
                problems.append(f"{name}: {function}+{position - offset:#x} points at {got:#x}, "
                                f"{target['name']} is at {want:#x}")
            elif not found.code:
                self.witnessed.setdefault(target["name"], set()).add(got)
        return problems

    # --- 3. Wonder Trade --------------------------------------------------------------------------
    def compile_wonder_trade(self):
        mwcc = self.workspace / MWCC
        require(mwcc.is_file(), f"{mwcc} is missing; build the workspace with build-rom.sh --pristine")
        include = self.work / "wt-include"
        include.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self.connection, include / "wonder_trade_config.h")
        flags = [*CC_FLAGS, "-ir", str(self.msl / "MSL_C/MSL_Common/Include"),
                 "-ir", str(self.msl / "MSL_C/MSL_ARM/Include"), "-i", str(ROOT / "client/core/include"), "-i", str(ROOT / "client/games/include"), "-i", str(include),
                 *[a for d in TLS_INCLUDE_DIRS for a in ("-i", str(self.tls / d))],
                 '-DMBEDTLS_CONFIG_FILE="wonder_trade/tls_config.h"',
                 '-DTF_PSA_CRYPTO_CONFIG_FILE="wonder_trade/crypto_config.h"']
        game = ["-i", str(ROOT / "client/games/include/wonder_trade/bw/sdk"), "-i", str(self.workspace / "src/game"),
                "-prefix", "wonder_trade/bw/sdk_prefix.h", f"-DWT_GAME_VARIANT={self.code:#010x}u",
                f"-DWT_GAME_VERSION={self.version}"]
        core = sorted([*(ROOT / "client/core/src/wonder_trade").glob("*.c"),
                       *(ROOT / "client/games/src/wonder_trade").glob("*.c")], key=lambda path: path.name)
        adapter = sorted(p for pattern in ADAPTER_SOURCES for p in ROOT.glob(pattern))
        tls = sorted(p for d in TLS_SOURCE_DIRS for p in (self.tls / d).glob("*.c"))
        out = self.work / "wt-obj"
        shutil.rmtree(out, ignore_errors=True)
        env = dict(os.environ, WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=")
        self.wt_objects = []
        for group, sources, extra in (("core", core, game), ("bw", adapter, game), ("tls", tls, [])):
            require(len({p.stem for p in sources}) == len(sources), f"two {group} sources share a name")
            (out / group).mkdir(parents=True)
            run([*wine_command(str(mwcc)), *flags, *extra, "-c", *map(str, sources), "-o", f"{out / group}{os.sep}"],
                cwd=ROOT, env=env)
            objects = sorted((out / group).glob("*.o"))
            require(len(objects) == len(sources), f"the compiler did not write every {group} object")
            self.wt_objects += objects
        self.log(f"compiled Wonder Trade for variant {self.code:#010x}: {len(core) + len(adapter)} sources and Mbed TLS")

    # --- 4. Link ----------------------------------------------------------------------------------
    def link(self):
        out = self.work / "link"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        self.objects = {}
        # In a changed source, only the changed functions are the module's: the others stay
        # the game's, so the module's copies of them are weak and calls go to the cartridge.
        redirected = {name for name, _ in self.redirects}
        retail = {}
        for source, (region, obj, path) in self.port_units.items():
            for function, *_ in obj.functions():
                symbol = self.map.get(function, region)
                if function not in redirected and symbol is not None:
                    retail[function] = symbol
        paths = [*(Path(u[2]) for u in self.port_units.values()), *self.wt_objects]
        units = {Path(u[2]) for u in self.port_units.values()}
        for path in paths:
            name = out / f"{path.parent.name}_{path.name}"
            data = elf.numbered_sections(elf.with_inline_addends(path.read_bytes()))
            name.write_bytes(weakened(data, retail) if path in units else data)
            self.objects[str(name)] = elf.Elf(name.read_bytes())
        defined, needed = link.defined_and_needed([o.data for o in self.objects.values()])
        needed |= set(retail)
        self.retail = retail
        self.base = self._arena()
        resolved, functions, missing = {}, set(), []
        for name in sorted(needed):
            symbol = self._resolve(name)
            if symbol is None:
                missing.append(name)
                continue
            resolved[name] = symbol.address | int(symbol.thumb)
            if symbol.code:
                functions.add(name)
        self.resolved = resolved
        source = out / "game-symbols.s"
        source.write_text(link.symbol_source(resolved, functions))
        game = out / "game-symbols.o"
        run([tool("as"), "-march=armv5te", "-o", str(game), str(source)])
        game.write_bytes(elf.set_eabi_flags(game.read_bytes()))
        script = out / "module.ld"
        script.write_text(link.linker_script(self.base, []))
        roots = sorted({name for name, _ in self.redirects} | {HOOK_TABLE, OVERLAY_DETOUR})
        output = out / "module.elf"
        command = [tool("ld"), "-T", str(script), "-o", str(output), "-Map", f"{output}.map",
                   "--no-warn-rwx-segments", "--use-blx", "--gc-sections",
                   *(f"--require-defined={root}" for root in roots), *self.objects, str(game)]
        try:
            run(command)
        except PortError as error:
            unresolved = sorted(set(re.findall(r"undefined reference to `([^']+)'", str(error))))
            if unresolved:
                raise PortError(f"cannot place {', '.join(unresolved)} in the cartridge "
                                f"(not in aletheia's link: {', '.join(n for n in unresolved if n in missing)})")
            raise
        run([tool("objcopy"), "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", str(output), f"{output}.bin"])
        nm = run([tool("nm"), str(output)])
        self.objects[str(game)] = elf.Elf(game.read_bytes())
        self.linked = link.Linked(self.base, Path(f"{output}.bin").read_bytes(), nm, Path(f"{output}.map").read_text(),
                                  self.objects)
        problems = link.check_relocations(self.linked, resolved)
        require(not problems, "the linked module is wrong:\n  " + "\n  ".join(problems[:40]))
        self.log(f"linked the module at {self.base:#x}: {len(self.linked.binary)} bytes, "
                 f"{len(resolved)} game symbols")

    def _resolve(self, name):
        if name in getattr(self, "retail", {}):
            return self.retail[name]
        if name in getattr(self, "data_fixes", {}):
            return self.data_fixes[name]
        if name in getattr(self, "unmapped", {}) and name not in self.map.byname:
            value = next(iter(self.unmapped[name]))
            return aletheia.Symbol(name, value, 0, False, False, None)
        found = self.map.byname.get(name, ())
        if len({(s.address, s.region) for s in found}) == 1:
            return found[0]
        arm9 = [s for s in found if s.region == "arm9"]
        return arm9[0] if len(arm9) == 1 else None

    def _arena(self):
        """Where main RAM is free after the static binary, its BSS and every overlay."""
        ends = [ram + size + bss for _, ram, size, bss, _, _ in nds.overlays(self.rom) if nds.ARM9_RAM <= ram < 0x02400000]
        return nds.align(max(ends), 32)

    # --- 5. The cartridge ---------------------------------------------------------------------------
    def assemble(self):
        symbols = self.linked.symbols
        load_end, end = symbols["__blob_load_end"], symbols["__blob_end"]
        require(len(self.linked.binary) <= load_end - self.base, "the module image overruns its load end")
        # The module's BSS is loaded as zeros: the stubs follow it.
        stub_base = nds.align(end, 32)
        image = bytearray(self.linked.binary + bytes(stub_base - self.base - len(self.linked.binary)))
        # Thumb entries call a stub after the image: it restores lr and jumps on.
        stubs, arm9_patches, hooks = bytearray(), [], []
        for name, symbol in self.redirects:
            target = symbols[name] | int(symbol.thumb)
            if symbol.thumb:
                stub = stub_base + len(stubs)
                stubs += nds.arm_trampoline(target)
                code = nds.thumb_entry_veneer(symbol.address, stub)
            else:
                code = arm_veneer(target)
            (arm9_patches if symbol.region == "arm9" else hooks).append((symbol, code))
        # The overlay hooks follow the SDK's start of every overlay.
        start = self.map.get(OVERLAY_START, "arm9")
        base, arm9 = self.modules["arm9"]
        displaced = arm9[start.address - base:start.address - base + 8]
        require(all(relocatable_arm(w) for w in struct.unpack("<II", displaced)),
                f"{OVERLAY_START} does not begin with two relocatable instructions")
        resume = stub_base + len(stubs)
        stubs += displaced + struct.pack("<II", 0xE51FF004, start.address + 8)
        arm9_patches.append((start, arm_veneer(symbols[OVERLAY_DETOUR])))
        self.resume = resume
        require(len(hooks) <= HOOK_MAX, f"{len(hooks)} overlay hooks; the module holds {HOOK_MAX}")
        table = symbols[HOOK_TABLE] - self.base
        rows = b"".join(struct.pack("<III8s", symbol.region, symbol.address, len(code), code.ljust(8, b"\0"))
                        for symbol, code in hooks)
        rows += struct.pack("<III8s", 0xFFFFFFFF, 0, 0, b"")
        require(image[table:table + len(rows)] == struct.pack("<III8s", 0xFFFFFFFF, 0, 0, b"") + bytes(len(rows) - HOOK_SIZE),
                f"{HOOK_TABLE} is not the empty table the adapter defines")
        image[table:table + len(rows)] = rows
        resume_word = symbols["gWtOverlayResume"] - self.base
        require(image[resume_word:resume_word + 4] == b"\xff\xff\xff\xff", "gWtOverlayResume is not the adapter's")
        image[resume_word:resume_word + 4] = struct.pack("<I", resume)
        segment = bytes(image) + bytes(stubs)
        segment += bytes(nds.align(len(segment), 32) - len(segment))
        bss = 0
        self.arena = self.base + len(segment)
        self.hooks, self.arm9_patches = hooks, arm9_patches
        self.segment, self.bss = segment, bss
        new_arm9 = self._arm9(segment, bss, arm9_patches)
        rom = relayout(self.rom, new_arm9, {self.icon_fid: self.icons})
        self.log(f"module {self.base:#x}-{self.arena:#x}: {len(arm9_patches)} ARM9 entries, "
                 f"{len(hooks)} overlay entries")
        return rom

    def _arm9(self, segment, bss, patches):
        """The static binary, expanded, with the entries patched, the module autoloaded and the arena moved."""
        base, body = self.modules["arm9"]
        body = bytearray(body)
        for symbol, code in patches:
            body[symbol.address - base:symbol.address - base + len(code)] = code
        params = nds.module_params(body)
        start, finish = nds.u32(body, params) - base, nds.u32(body, params + 4) - base
        require(finish == len(body) and (finish - start) % 16 == 0, "the autoload list does not end the binary")
        entries = body[start:finish] + struct.pack("<IIII", self.base, len(segment), 0, bss)
        body = body[:start] + segment + entries
        struct.pack_into("<II", body, params, base + start + len(segment), base + len(body))
        struct.pack_into("<I", body, params + 0x14, 0)  # no longer compressed
        arena = self.map.get(ARENA_FUNCTION, "arm9")
        literals = [at for at in range(arena.address - base, arena.address - base + arena.size, 4)
                    if nds.u32(body, at) == self.base]
        require(len(literals) == 1, f"{ARENA_FUNCTION} holds {len(literals)} words {self.base:#x}")
        struct.pack_into("<I", body, literals[0], self.arena)
        self.arena_literal = base + literals[0]
        return bytes(body)

    def check_headroom(self):
        """Main RAM the game leaves free after the module, its heaps and its other arena use."""
        tables = self.unmapped.get(HEAP_TABLE, set())
        require(len(tables) == 1, f"the game's heap table {HEAP_TABLE} was not found")
        base, body = self.modules["arm9"]
        at = next(iter(tables)) - base
        sizes = [nds.u32(body, at + 8 * i) for i in range(HEAP_COUNT)]
        arenas = [nds.u32(body, at + 8 * i + 4) for i in range(HEAP_COUNT)]
        require(arenas == [0] * HEAP_COUNT, "a game heap is outside the main arena")
        self.headroom = ARENA_HI - self.arena - sum(sizes) - ARENA_OTHER
        require(self.headroom >= MIN_HEADROOM,
                f"only {self.headroom} bytes of main RAM stay free; at least {MIN_HEADROOM} are needed")
        self.log(f"{self.headroom} bytes of main RAM stay free (heaps {', '.join(hex(s) for s in sizes)})")

    def menu_icon(self):
        """The field menu's graphics archive with Wonder Trade's icon as its next member."""
        self.icon_fid = nds.file_ids(self.rom)[ICON_ARCHIVE]
        members, names = nds.narc_split(self.rom[slice(*nds.file_span(self.rom, self.icon_fid))])
        require(len(members) == ICON_MEMBER, f"{ICON_ARCHIVE} does not end at member {ICON_MEMBER}")
        template = members[ICON_TEMPLATE]
        require(template[:4] == b"RGCN" and template[0x10:0x14] == b"RAHC" and nds.u32(template, 0x1C) == 3,
                f"{ICON_ARCHIVE} member {ICON_TEMPLATE} is not 4-bit characters")
        size = nds.u32(template, 0x28)
        tiles = recolor(icon_tiles(ICON_PNG.read_bytes()), ICON_COLORS)
        require(len(tiles) == size and 0x30 + size == len(template), f"{ICON_PNG.name} does not fit the character data")
        self.icons = nds.narc_join([*members, template[:0x30] + tiles], names)
        self.log(f"Wonder Trade's icon is member {ICON_MEMBER} of {ICON_ARCHIVE}")

    # --- 6. Read the cartridge back -------------------------------------------------------------------
    def verify(self, rom):
        new = nds.modules(rom)
        require(set(new) == set(self.modules), "the module list changed")
        for key, (base, body) in self.modules.items():
            nbase, nbody = new[key]
            require(nbase == base, f"{key} moved")
            if key != "arm9":
                require(nbody == body, f"overlay {key} changed")
        base, body = self.modules["arm9"]
        _, nbody = new["arm9"]
        allowed = set()
        for symbol, code in self.arm9_patches:
            allowed.update(range(symbol.address - base, symbol.address - base + len(code)))
        params = nds.module_params(body)
        allowed.update(range(params, params + 8))
        allowed.update(range(params + 0x14, params + 0x18))
        allowed.update(range(self.arena_literal - base, self.arena_literal - base + 4))
        start = nds.u32(body, params) - base
        stray = [i for i in range(start) if body[i] != nbody[i] and i not in allowed]
        require(not stray, f"ARM9 changed outside the expected sites at {base + stray[0]:#x}" if stray else "")
        require(nbody[start:start + len(body) - start] != b"" and nds.u32(nbody, params + 0x14) == 0, "ARM9 is compressed")
        nstart, nend = nds.u32(nbody, params) - base, nds.u32(nbody, params + 4) - base
        require(nbody[nstart:nend - 16] == body[start:len(body)], "the game's autoload entries changed")
        address, size, sinit, bss = struct.unpack_from("<IIII", nbody, nend - 16)
        require((address, address + size + bss, sinit) == (self.base, self.arena, 0), "the module autoload entry is wrong")
        segment = nbody[nstart - size:nstart]
        for symbol, code in self.arm9_patches + self.hooks:
            if symbol.thumb:
                decoded = elf.thumb_bl_target(code[2:6], symbol.address + 2)
                require(decoded and decoded[0] == "blx", f"{symbol.name} has no veneer")
                target = nds.u32(segment, decoded[1] - self.base + 8)
            else:
                target = struct.unpack_from("<I", code, 4)[0]
            if symbol.name == OVERLAY_START:
                require(target == self.linked.symbols[OVERLAY_DETOUR], f"{OVERLAY_START} jumps to the wrong place")
            else:
                require(target & ~1 == self.linked.symbols[symbol.name], f"{symbol.name} jumps to the wrong place")
        files = lambda r: [r[slice(*nds.file_span(r, fid))] for fid in range(nds.u32(r, 0x4C) // 8)]
        old, new = files(self.rom), files(rom)
        require(new[self.icon_fid] == self.icons, "the icon archive was not written")
        new[self.icon_fid] = old[self.icon_fid]
        require(new == old, "a cartridge file changed")
        self.log("the new cartridge differs from the dump only where the port wrote")

    def build(self):
        self.map_symbols()
        self.compile_game_sources()
        self.check_game_sources()
        self.compile_wonder_trade()
        self.link()
        self.menu_icon()
        rom = self.assemble()
        self.check_headroom()
        self.verify(rom)
        return rom


def patched_paths(patch):
    return [line.split(" b/", 1)[1] for line in Path(patch).read_text().splitlines() if line.startswith("diff --git ")]


def weakened(data, names):
    """A copy of an object whose named global symbols are weak."""
    obj = elf.Elf(data)
    out = bytearray(data)
    symtab = next(section for section in obj.sections if section["type"] == elf.SHT_SYMTAB)
    for k, symbol in enumerate(obj.symbols):
        if symbol["name"] in names and symbol["bind"] == 1 and symbol["shndx"] != 0:
            out[symtab["off"] + 16 * k + 12] = 2 << 4 | symbol["type"]
    return bytes(out)


def same_function(a_obj, a, b_obj, b):
    """True if two compiled functions have the same bytes and relocations."""
    if a[3] != b[3]:
        return False

    def shape(obj, function):
        _, shndx, offset, size, _, _ = function
        rows = [(r[0] - offset, r[1], obj.symbols[r[2]]["name"] or obj.sections[obj.symbols[r[2]]["shndx"]]["name"]
                 if obj.symbols[r[2]]["shndx"] < len(obj.sections) else "", r[3])
                for r in obj.relocs.get(shndx, ()) if offset <= r[0] < offset + size]
        mask = set()
        for position, *_ in rows:
            mask.update(range(position, position + 4))
        body = obj.section_bytes(shndx)[offset:offset + size]
        return bytes(0 if i in mask else body[i] for i in range(size)), rows
    return shape(a_obj, a) == shape(b_obj, b)


def icon_tiles(png):
    """4-bit characters, 8x8 tiles in rows, from an 8-bit indexed PNG whose pixels use colors 0 to 15."""
    require(png[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG")
    at, data, header = 8, b"", None
    while at < len(png):
        length, kind = struct.unpack_from(">I4s", png, at)
        body = png[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            data += body
        at += 12 + length
    width, height, depth, color, _, _, interlace = header
    require(depth == 8 and color == 3 and not interlace and width % 8 == 0 and height % 8 == 0,
            "the icon must be an 8-bit indexed, non-interlaced PNG of whole tiles")
    raw, rows, previous = zlib.decompress(data), [], bytes(width)
    for y in range(height):
        kind, line = raw[y * (width + 1)], bytearray(raw[y * (width + 1) + 1:(y + 1) * (width + 1)])
        for x in range(width):
            left, up = line[x - 1] if x else 0, previous[x]
            corner = previous[x - 1] if x else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                guess = left + up - corner
                best = min((abs(guess - left), 0, left), (abs(guess - up), 1, up), (abs(guess - corner), 2, corner))[2]
                line[x] = (line[x] + best) & 0xFF
        rows.append(bytes(line))
        previous = line
    require(all(value < 16 for row in rows for value in row), "the icon uses more than 16 colors")
    out = bytearray()
    for ty in range(0, height, 8):
        for tx in range(0, width, 8):
            for y in range(ty, ty + 8):
                for x in range(tx, tx + 8, 2):
                    out.append(rows[y][x] | rows[y][x + 1] << 4)
    return bytes(out)


def recolor(tiles, frames):
    """4-bit characters with each pixel's colour mapped, the characters split evenly into
    frames, each with its own map from the colours the icon uses."""
    require(len(tiles) % len(frames) == 0, "the characters do not split into the frames")
    size = len(tiles) // len(frames)
    out = bytearray()
    for at, byte in enumerate(tiles):
        colors = frames[at // size]
        low, high = byte & 0xF, byte >> 4
        require(low < len(colors) and high < len(colors), "the icon uses a colour without a mapping")
        out.append(colors[low] | colors[high] << 4)
    return bytes(out)


def relayout(rom, arm9, files=None):
    """The cartridge with a new ARM9 static binary.

    Everything after it in the DS area moves by its growth, and each replaced
    file moves to the end of the DS area; the DSi area moves
    to the next 512 KiB boundary after the DS area if it no longer fits where it
    was. Its digests and signature are left as they were: the cartridge runs in
    DS mode only."""
    a9_offset, a9_size = nds.u32(rom, 0x20), nds.u32(rom, 0x2C)
    old_end = nds.align(a9_offset + a9_size, nds.FILE_ALIGN)
    new_end = nds.align(a9_offset + len(arm9), nds.FILE_ALIGN)
    delta = new_end - old_end
    used = nds.u32(rom, 0x80)
    out = bytearray(rom[:a9_offset]) + arm9
    out += b"\xff" * (new_end - len(out))
    out += rom[old_end:used]
    for field in (0x30, 0x40, 0x48, 0x50, 0x68):
        if nds.u32(out, field) >= old_end:
            struct.pack_into("<I", out, field, nds.u32(out, field) + delta)
    struct.pack_into("<I", out, 0x2C, len(arm9))
    fat = nds.u32(out, 0x48)
    for entry in range(fat, fat + nds.u32(out, 0x4C), 8):
        start, end = struct.unpack_from("<II", out, entry)
        if start >= old_end:
            struct.pack_into("<II", out, entry, start + delta, end + delta)
    for fid, body in sorted((files or {}).items()):
        place = nds.align(len(out), nds.FILE_ALIGN)
        out += b"\xff" * (place - len(out)) + body
        struct.pack_into("<II", out, fat + 8 * fid, place, place + len(body))
    struct.pack_into("<I", out, 0x80, len(out))
    if out[0x12] & 2:
        twl_start, twl_end = nds.u16(rom, 0x92) * TWL_ALIGN, nds.u32(rom, 0x210)
        require(twl_start >= used and twl_end > twl_start, "the DSi area is not after the DS area")
        place = max(twl_start, nds.align(len(out), TWL_ALIGN))
        out += b"\xff" * (place - len(out)) + rom[twl_start:twl_end]
        move = place - twl_start
        if move:
            struct.pack_into("<H", out, 0x92, place // TWL_ALIGN)
            for field in (0x1C0, 0x1D0, 0x1E8, 0x1F0, 0x1F8, 0x220, 0x228):
                if twl_start <= nds.u32(out, field) < twl_end:
                    struct.pack_into("<I", out, field, nds.u32(out, field) + move)
        struct.pack_into("<I", out, 0x210, len(out))
    capacity = 0x20000 << out[0x14]
    require(len(out) <= capacity, "the rebuilt cartridge does not fit its capacity")
    out += b"\xff" * (capacity - len(out))
    struct.pack_into("<H", out, nds.HEADER_CRC, nds.crc16(out[:nds.HEADER_CRC]))
    return bytes(out)
