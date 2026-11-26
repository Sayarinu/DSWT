"""Add Wonder Trade to a retail Diamond or Pearl that pret cannot build, using US Rev 5 as the map.

The same matching and checks as the Platinum port (platinum.py) apply, and the module
loads at the arena start the same way. Diamond and Pearl differ in how they are built:

- the decompilation builds with make under Wine, so each object is compiled with the
  command make itself would run, read from a dry run;
- the SDK is compiled into separate objects instead of archives;
- text is in .gmm files: the start menu bank gets one more entry and Wonder Trade's
  bank is appended, encoded with the bank keys the build uses;
- the menu icon is a new member of graphic/menu_gra.narc;
- the game keeps its total heap count, so no heap count changes.
"""

import json
from pathlib import Path
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET

from .catalog import variants_by_id
from . import elf, link, nds, platinum, sdk, symbols, toolchain, writeout
from .platinum import PortError, require, git, sha256, ROOT, RETAIL, prepare_game

# game: (make GAME_VERSION and define, decompilation version name)
GAMES = {"diamond": ("DIAMOND", "VERSION_DIAMOND"), "pearl": ("PEARL", "VERSION_PEARL")}
# id: (catalogue country, language enumerator, .gmm and Wonder Trade text column, build deltas)
# Korean was built later than US, with a newer compiler for the game's own code, and
# Japanese, the first release, earlier, with Metrowerks 1.2.
COMPILERS = {"ko": "2.0/sp1p6", "ja": "1.2/sp4"}
# A language whose game code differs from US in the compiler alone is mapped with an image of
# the pristine US build that compiles the game's C code the same way: this patch to the
# pristine tree. The port builds it once in game/retail-port.
MATCHING = {"ja": "diamond-japanese-matching.patch"}
# A language whose start menu the US assembly cannot express (another compiler's output): the
# port writes the player's own copy out as assembly (writeout.py) and makes Wonder Trade's
# changes to it with this patch.
START_MENUS = {"ja": "diamond-japanese-start-menu.patch"}
# The header the written-out start menu starts with; the patch's context includes it.
START_MENU_HEADER = """\t; The start menu of Japanese Diamond and Pearl (asm/unk_02035068.s), written out from the
\t; cartridge: the Japanese build lays the functions out in reverse order and compiled
\t; them with Metrowerks 1.2. Calls and addresses are by name; labels carry the Japanese
\t; addresses. Both versions run these bytes.
"""
# Objects of the workspace build whose routines the port may copy into the module when the
# language's own copy is not found: sub_02002F40, which centres a string by its width.
COPIED = {"ja": ("src/font.o",)}
# Names resolved as another: Japanese links an older MSL without the exit that mbedTLS calls on a
# corrupted heap. On a DS exit only stops the program, as OS_Terminate does.
ALIASES = {"ja": {"exit": "OS_Terminate"}}
# Wrappers Japanese compiled from C where US has them in assembly: name -> the Japanese Thumb
# code and its literal pool (numbers, or names resolved in the retail game).
WRAPPERS = {
    "ja": {
        "sub_02088858": ((0x4801, 0x2102, 0x4B01, 0x4718), (4, "HandleLoadOverlay")),  # load overlay 4, mode 2
        "sub_02088868": ((0x4801, 0x4B02, 0x4718, 0x46C0), (4, "UnloadOverlayByID")),  # unload overlay 4
    },
}
LANGUAGES = {
    "fr": ("france", "LANGUAGE_FRENCH", "French", "fr_FR", ()),
    "de": ("germany", "LANGUAGE_GERMAN", "German", "de_DE", ()),
    "it": ("italy", "LANGUAGE_ITALIAN", "Italian", "it_IT", ()),
    "es": ("spain", "LANGUAGE_SPANISH", "Spanish", "es_ES", ()),
    "ko": ("korea", "LANGUAGE_KOREAN", "Korean", "ko_KR", ("diamond-korean-build.patch",)),
    "ja": ("japan", "LANGUAGE_JAPANESE", "Japanese", "ja_JP", ("diamond-japanese-build.patch",)),
}
# Game functions a language changes in reviewed details: name -> bytes that differ from US.
REVIEWED = {
    "ko": {
        "GetSpeciesName": 1,  # the species name bank number
        "ov81_02237E40": 1,  # a literal data address
        "sub_020372D4": 1,  # the call to its neighbour, which grew
        "sub_02037304": 7,  # two added calls after the trade sequence ends
        "ov04_021DD904": 24,  # DWC_Free, a NitroDWC between Diamond's and Platinum's
    },
}
# Korean ships the newer libraries Platinum has (NitroSDK 4.2.30001, NitroWiFi 2.1.30003 and
# NitroDWC 2.2.30008), with the same structures and calls: the functions the module uses are
# found by Platinum's build of them, so it never mixes in Diamond's older copies.
# Diamond name -> Platinum name.
LIBRARIES = {
    "ko": {
        "NNS_FndAllocFromExpHeapEx": "NNS_FndAllocFromExpHeapEx",
        "NNS_FndCreateExpHeapEx": "NNS_FndCreateExpHeapEx",
        "NNS_FndFreeToExpHeap": "NNS_FndFreeToExpHeap",
        "OS_CreateThread": "OS_CreateThread",
        "OS_GetLowEntropyData": "OS_GetLowEntropyData",
        "OS_GetThreadPriority": "OS_GetThreadPriority",
        "OS_GetTick": "OS_GetTick",
        "OS_JoinThread": "OS_JoinThread",
        "OS_Sleep": "OS_Sleep",
        "OS_WakeupThreadDirect": "OS_WakeupThreadDirect",
        "SOCL_CreateSocket": "SOCL_CreateSocket",
        "SOCL_Connect": "SOCL_Connect",
        "SOCLi_ReadFrom": "SOCL_ReadFrom",  # 2.1 reads in SOCL_ReadFrom itself, same arguments
        "SOCL_WriteTo": "SOCL_WriteTo",
        "SOCL_Close": "SOCL_Close",
        "SOCL_IsClosed": "SOCL_IsClosed",
        "SOCLi_TrashSocket": "SOCLi_TrashSocket",
        "WCM_DisconnectAsync": "WCM_DisconnectAsync",
        "WCM_GetPhase": "WCM_GetPhase",
        "ov04_021DD968": "DWC_SetMemFunc",
        "ov04_021DD944": "DWC_Alloc",
        "ov04_021DD6F0": "DWC_ClearError",
        "ov04_021EBC60": "DWC_AC_Create",
        "ov04_021EBB80": "DWC_AC_Process",
        "ov04_021EBAE8": "DWC_AC_GetStatus",
        "ov04_021EB9D8": "DWC_AC_Destroy",
    },
}
# Platinum library functions a language changes in reviewed details: name -> bytes that differ.
LIBRARY_REVIEWED = {
    "ko": {
        "DWC_AC_Create": 16,  # calls into its own file, whose functions moved
    },
}
MESSAGES = "msgdata/msg.narc"
MENU_GRAPHICS = "graphic/menu_gra.narc"
START_MENU_BANK = 323
WT_BANK = 624  # narc_0624, the bank the decompilation appends
MENU_ICON = 11  # menu_gra member narc_0011, the one the decompilation appends
MESSAGE_NARC = 0x1A  # NARC_msgdata_msg, the message archive's number
ASM = "arm9/asm/unk_02035068.s"
PORT_PATCH = RETAIL / "diamond-port.patch"  # port-only source changes, if any
FAMILY = 1
MIN_HEADROOM = 32 * 1024


def gmm_key(msg_mk, bank):
    """The encryption key the build gives a text bank, from files/msgdata/msg.mk."""
    prefix = f"files/msgdata/msg/narc_{bank:04d}.bin: MSGENCFLAGS = -k "
    line = next((line for line in msg_mk.splitlines() if line.startswith(prefix)), None)
    require(line is not None, f"msg.mk gives bank {bank} no key")
    return int(line[len(prefix):], 0)


def localized_gmm(text, language):
    """A .gmm whose English entries hold another language's text, for an English-only msgenc."""
    root = ET.fromstring(text)
    for row in root.iter("row"):
        english = next(node for node in row.iter("language") if node.get("name") == "English")
        target = next((node for node in row.iter("language") if node.get("name") == language), None)
        require(target is not None, f"{row.get('id')} has no {language} text")
        english.text = target.text
    return '<?xml version="1.0"?>\n' + ET.tostring(root, encoding="unicode")


class Target(platinum.Target):
    """One retail Diamond or Pearl to port."""

    def __init__(self, game, language, rom_path, workspace, pristine, work, log=print, library=None):
        require(game in GAMES, f"unsupported game {game}")
        require(language in LANGUAGES, f"unsupported language {language}")
        self.game, self.language = game, language
        country, self.language_name, self.gmm_language, self.text_key, deltas = LANGUAGES[language]
        self.variant_id = f"{game}-{country}"
        self.deltas = [RETAIL / name for name in deltas]
        layout = json.loads((ROOT / "protocol/save-layout-dp.json").read_text())
        korean = self.variant_id.endswith(layout["korean_target_suffix"])
        self.save_magic = layout["footer_magic"]["korean" if korean else "international"]
        self.workspace, self.pristine, self.work = (Path(p).resolve() for p in (workspace, pristine, work))
        self.log = log
        self.version = GAMES[game][0]
        self.arm9_build = f"arm9/build/{game}.us"
        catalogue = variants_by_id(ROOT)
        require(self.variant_id in catalogue, f"{self.variant_id} is not catalogued")
        self.variant = catalogue[self.variant_id]
        self.rom = Path(rom_path).read_bytes()
        require(len(self.rom) == self.variant["size"] and sha256(self.rom) == self.variant["sha256"],
                f"{rom_path} is not the catalogued {self.variant['game']} {self.variant['name']} dump")
        base = f"{game}-us-rev5"
        require(self.variant.get("port_base") == base, f"{self.variant_id} is not ported from {base}")
        us_path = self.pristine / f"build/{game}.us/poke{game}.us.nds"
        self.us_rom = us_path.read_bytes()
        require(sha256(self.us_rom) == catalogue[base]["sha256"],
                f"{us_path} is not a pristine {base} build; build it with ROM_GAME={game} --pristine")
        pin = json.loads((ROOT / "client/games/upstream-pokediamond.json").read_text())["revision"]
        for tree in (self.workspace, self.pristine):
            require(git(tree, "rev-parse", "HEAD").decode().strip() == pin, f"{tree} is not at upstream {pin}")
        self.modules = nds.modules(self.rom)
        self.library = None
        if language in LIBRARIES:
            require(library is not None, f"{self.variant_id} also needs the pristine Platinum build")
            self.library = Path(library).resolve()
            self.library_rom = (self.library / "build/pokeplatinum.us.nds").read_bytes()
            require(sha256(self.library_rom) == catalogue[platinum.US_VARIANT]["sha256"],
                    f"{self.library} is not a pristine {platinum.US_VARIANT} build; build it with ROM_REVISION=1 --pristine")
        self.banks_header = None
        self.object_names = {}

    # --- Mapping --------------------------------------------------------------------------------------
    def map_symbols(self):
        tree, rom = (self.pristine, self.us_rom) if self.language not in MATCHING else self._matching_image()
        self.image = symbols.UsImage((tree / self.arm9_build / "arm9.elf").read_bytes(), rom)
        self.map = symbols.SymbolMap(self.image, rom, self.rom)
        if self.language in MATCHING:
            # Functions in assembly (US compiler output) cannot match a build with another compiler:
            # place them by where matched code calls them.
            symbols.trust_calls(self.map)
            symbols.align_calls(self.map)
        self.log(f"mapped {len(self.map.addr)} US symbols")
        self.library_addr = self._map_library() if self.library is not None else {}

    def _matching_image(self):
        """(tree, ROM) of the pristine US build with the matching patch, built here when missing."""
        patch = RETAIL / MATCHING[self.language]
        tree = self.work.parent / f"matching-{self.language}"
        stamp = tree / ".dswt-matching"
        want = sha256(patch.read_bytes() + git(self.pristine, "rev-parse", "HEAD"))
        rom_path = tree / f"build/{self.game}.us/poke{self.game}.us.nds"
        if not stamp.is_file() or stamp.read_text() != want:
            shutil.rmtree(tree, ignore_errors=True)
            tree.parent.mkdir(parents=True, exist_ok=True)
            # The pristine tree's tools and generated files come along; only the code is rebuilt.
            subprocess.run(["cp", "-a", str(self.pristine), str(tree)], check=True)
            result = subprocess.run(["git", "-C", str(tree), "apply", str(patch)], capture_output=True, text=True)
            require(result.returncode == 0, f"{patch.name} does not apply: {result.stderr}")
            for game in GAMES:
                shutil.rmtree(tree / f"arm9/build/{game}.us", ignore_errors=True)
                (tree / f"build/{game}.us/poke{game}.us.nds").unlink(missing_ok=True)
            stamp.write_text(want)
        if not rom_path.is_file():
            self.log(f"building the matching image for {self.game} ({patch.name}); this takes a while")
            toolchain.docker({str(tree): "/workspace"}, f"{toolchain.DP_WINE}\nmake -j\"$(nproc)\" "
                             f"GAME_VERSION={self.version} COMPARE=0", workdir="/workspace", image=toolchain.DP_IMAGE)
        return tree, rom_path.read_bytes()

    def _map_library(self):
        """Diamond name -> address of each listed library function, found by Platinum's bytes in
        the module Diamond keeps it in (Platinum keeps the socket library in an overlay)."""
        image = symbols.UsImage((self.library / "build/main.nef").read_bytes(), self.library_rom)
        modules = nds.modules(self.library_rom)
        found, pending = {}, {}
        for name, theirs in LIBRARIES[self.language].items():
            ours, symbol = self.image.unique(name), image.unique(theirs)
            require(ours is not None and symbol is not None, f"{name} or Platinum's {theirs} is not a single symbol")
            got = symbols.masked(image, modules, symbol, symbol.size)
            require(got is not None, f"Platinum's {theirs} has no bytes to match")
            module, thumb = self.image.module(ours), symbol.num in image.thumb
            at = symbols.unique_match(self.modules, module, got[2], got[3], 2 if thumb else 4)
            if at is None:
                pending[name] = (symbol, module, got[2], got[3])
            else:
                found[name] = (symbol, module, at | int(thumb))
        reviewed = LIBRARY_REVIEWED.get(self.language, {})
        for name, (symbol, module, data, keep) in pending.items():
            if symbol.name not in reviewed:
                continue
            # Placed at Platinum's distance from a placed function of its Platinum section, where
            # its bytes differ in exactly the reviewed number of places.
            base, body = self.modules[module]
            hits = set()
            for other, where, at in found.values():
                guess = (at & ~1) + symbol.start - other.start
                offset = guess - base
                if (where, other.section) != (module, symbol.section) or not 0 <= offset <= len(body) - len(data):
                    continue
                if sum(keep[i] and body[offset + i] != data[i] for i in range(len(data))) == reviewed[symbol.name]:
                    hits.add(guess | int(symbol.num in image.thumb))
            require(len(hits) == 1, f"{symbol.name} is not where its neighbours put it with the reviewed differences")
            found[name] = (symbol, module, hits.pop())
        self.log(f"{len(found)} of {len(LIBRARIES[self.language])} library functions found by Platinum's build")
        return {name: at for name, (_, _, at) in found.items()}

    def reviewed(self):
        return REVIEWED.get(self.language, {})

    def _resolve(self, name):
        if name in LIBRARIES.get(self.language, {}):
            at = self.library_addr.get(name)
            return None if at is None else (at, True)
        alias = ALIASES.get(self.language, {}).get(name)
        if alias is not None:
            return self._resolve(alias)
        if name in WRAPPERS.get(self.language, {}):
            return self._wrapper(*WRAPPERS[self.language][name])
        found = super()._resolve(name)
        us = self.image.unique(name)
        if found is None and us is not None and self.language in MATCHING and isinstance(self.image.module(us), int):
            at = self._template_member(us)
            found = None if at is None else (at, True)
        return found

    def _wrapper(self, code, pool):
        """The one place in the static module holding a wrapper's code and literal pool."""
        words = []
        for item in pool:
            found = item if isinstance(item, int) else self._resolve(item)
            if found is None:
                return None
            words.append(found if isinstance(item, int) else found[0])
        data = struct.pack(f"<{len(code)}H{len(words)}I", *code, *words)
        at = symbols.unique_match(self.modules, "arm9", data, [True] * len(data), 4)
        return None if at is None else (at | 1, True)

    def _app_templates(self, modules, oid):
        """Every app template (init, main and exit, then the overlay ID) of an overlay's functions."""
        start, code = modules[oid]
        found = []
        for base, body in modules.values():
            for k in range(0, len(body) - 15, 4):
                words = struct.unpack_from("<4I", body, k)
                if words[3] == oid and all(w & 1 and start <= w - 1 < start + len(code) for w in words[:3]):
                    found.append(words[:3])
        return found

    def _template_member(self, us):
        """An overlay's init, main or exit, by its place in the game's app templates of that overlay,
        which must all agree."""
        oid = self.image.module(us)
        theirs = {words for words in self._app_templates(self.map.us_modules, oid) if us.start | 1 in words}
        ours = set(self._app_templates(self.modules, oid))
        if len(theirs) != 1 or len(ours) != 1:
            return None
        return ours.pop()[theirs.pop().index(us.start | 1)]

    def replaced_files(self):
        _, desired = prepare_game.overlay_files(ROOT, self.workspace, prepare_game.GAMES["dp"])
        stale = sorted(name for name, content in desired.items()
                       if not (self.workspace / name).is_file() or (self.workspace / name).read_bytes() != content)
        require(not stale, f"the workspace is older than client/ ({', '.join(stale[:3])}); rebuild it with build-rom.sh")
        changed = git(self.workspace, "diff", "--name-only", "HEAD", "--", "arm9/src", "arm9/overlays",
                      "arm9/asm", "include").decode().split()
        self.files = sorted(name for name in changed if name.startswith("arm9/") and name.endswith(".c"))
        self.headers = sorted(name for name in changed if name.startswith("include/") and name.endswith(".h"))
        require(ASM in changed, f"the workspace does not change {ASM}")
        require(self.files, "the workspace changes no upstream sources; prepare it with prepare-game.py")
        for name in [*self.files, ASM]:
            self.object_names[name.replace("/", "_") + ".o"] = name

    def text_banks(self):
        """The message and menu graphics archives, which must number like US (Japanese and Korean do not)."""
        ids = nds.file_ids(self.rom)
        self.text_fid, self.icon_fid = ids[MESSAGES], ids[MENU_GRAPHICS]
        members, self.text_names = nds.narc_split(self.rom[slice(*nds.file_span(self.rom, self.text_fid))])
        us_members, _ = nds.narc_split(self.us_rom[slice(*nds.file_span(self.us_rom, nds.file_ids(self.us_rom)[MESSAGES]))])
        require(len(us_members) == WT_BANK, "the US message archive does not end where Wonder Trade's bank goes")
        remap = platinum.text_bank_remap(us_members, members)
        self.text_members = members
        self.naix, self.bank_remap, self.start_menu_bank = None, None, START_MENU_BANK
        if len(members) != WT_BANK or any(remap.get(i) != i for i in range(WT_BANK)):
            # Japanese and Korean leave some banks out: number the ones the code uses like the
            # cartridge does, by each bank's key, and append Wonder Trade's after the last.
            self.bank_remap = dict(remap)
            self.bank_remap[WT_BANK] = len(members)
            require(remap.get(START_MENU_BANK) is not None, "the retail game has no start menu text bank")
            self.start_menu_bank = remap[START_MENU_BANK]
            lines = []
            for line in (self.workspace / "files/msgdata/msg.naix").read_text().splitlines():
                words = line.replace(",", " ").split()
                if len(words) == 3 and words[0].startswith("NARC_msg_narc_") and words[1] == "=" and words[2].isdigit():
                    number = self.bank_remap.get(int(words[2]))
                    if number is not None:
                        lines.append(f"    {words[0]} = {number},")
                    continue  # a bank the cartridge does not have stays undefined
                lines.append(line)
            self.naix = "\n".join(lines) + "\n"
            self.log(f"message banks renumbered: {len(members)} banks, start menu {self.start_menu_bank}, "
                     f"Wonder Trade's {len(members)}")
        self.icon_members, self.icon_names = nds.narc_split(self.rom[slice(*nds.file_span(self.rom, self.icon_fid))])
        require(len(self.icon_members) == MENU_ICON, f"the retail menu graphics archive does not end at member {MENU_ICON}")

    def _stage(self, tree, files, read, patches):
        super()._stage(tree, files, read, patches)
        config = git(self.workspace, "show", "HEAD:include/config.h").decode()
        english = "#define GAME_LANGUAGE LANGUAGE_ENGLISH"
        require(english in config, "include/config.h no longer selects English this way")
        (tree / "include").mkdir(parents=True, exist_ok=True)
        (tree / "include/config.h").write_text(config.replace(english, f"#define GAME_LANGUAGE {self.language_name}"))
        # -gccinc looks next to the including file first, so a header in the workspace's
        # include/ would find the workspace's config.h. The whole tree comes along, keeping
        # the staged headers.
        archive = subprocess.run(["git", "-C", str(self.workspace), "archive", "HEAD", "include"],
                                 check=True, capture_output=True).stdout
        subprocess.run(["tar", "-x", "--skip-old-files", "-C", str(tree)], input=archive, check=True)
        if self.naix is not None:
            (tree / "include/msgdata").mkdir(parents=True, exist_ok=True)
            (tree / "include/msgdata/msg.naix").write_text(self.naix)

    def _file_of(self, path):
        return self.object_names.get(Path(path).name, Path(path).name)

    def _compile(self, tree, out, **options):
        return toolchain.compile_dp_objects(self.workspace, tree, tree / "include", out, self.files, self.version,
                                            tls=self._tls(), compiler=COMPILERS.get(self.language), **options)

    @staticmethod
    def _tls():
        return subprocess.run(["python3", str(ROOT / "scripts/prepare-tls.py")], check=True,
                              capture_output=True, text=True).stdout.strip()

    # --- 1. Upstream code, including the assembly, must match the retail bytes --------------------------
    def check_variant(self):
        tree = self.work / "pristine"
        self._stage(tree, self.files + self.headers, lambda name: git(self.workspace, "show", f"HEAD:{name}"), self.deltas)
        objects = self._compile(tree, self.work / "pristine-obj")
        self.pristine_objects = {self._file_of(path): elf.Elf(path.read_bytes()) for path in objects}
        self.pristine_asm = self._start_menu(self.work / "pristine-asm")
        self.pristine_objects[ASM] = elf.Elf(self._renumbered_banks(elf.with_function_sizes(self.pristine_asm)))
        self.located, self.located_data, problems = {}, {}, []
        for name in [*self.files, ASM]:
            problems += self._locate(name, self.pristine_objects[name])
        require(not problems, "upstream code differs from the retail game:\n  " + "\n  ".join(problems))
        self.files = [*self.files, ASM]
        self.log(f"{len(self.located)} functions and {len(self.located_data)} data objects match the retail game")

    def witnesses(self):
        super().witnesses()
        # The start menu's tables name other game functions (sub-menu actions): read them from the
        # game's copy of the tables too.
        pristine = elf.Elf(self.pristine_asm)
        ro = next(i for i, s in enumerate(pristine.sections) if s["name"] == ".rodata")
        body = pristine.section_bytes(ro)
        keep = [True] * len(body)
        for offset, *_ in pristine.relocs.get(ro, ()):
            keep[offset:offset + 4] = [False] * 4
        block = symbols.unique_match(self.modules, "arm9", body, keep, 4)
        require(block is not None, f"the tables of {ASM} were not found once in the retail game")
        base, image = self.modules["arm9"]
        problems, added = [], 0
        for offset, kind, index, addend in pristine.relocs.get(ro, ()):
            symbol = pristine.symbols[index]
            if kind != elf.R_ARM_ABS32 or symbol["shndx"] != 0 or not symbol["name"]:
                continue
            target = struct.unpack_from("<I", image, block - base + offset)[0] - addend
            name = symbol["name"]
            if self.witnessed.setdefault(name, target) & ~1 != target & ~1:
                problems.append(f"{name} is referenced at {self.witnessed[name]:#x} and {target:#x}")
            us = self.image.unique(name)
            mapped = self.map.addr.get(us.num) if us else None
            if mapped is not None and mapped & ~1 != target & ~1:
                problems.append(f"{name}: the start menu's tables point at {target:#x}, the symbol map says {mapped:#x}")
            added += 1
        require(not problems, "retail references disagree:\n  " + "\n  ".join(problems))
        self.log(f"{added} references of the start menu's tables read from the retail game")

    # --- 2. Compile Wonder Trade for this language ------------------------------------------------------
    def port_defines(self):
        config = git(self.pristine, "show", "HEAD:include/config.h").decode()
        numbers = {words[1]: int(words[2]) for words in (line.split() for line in config.splitlines())
                   if len(words) >= 3 and words[0] == "#define" and words[2].isdigit()}
        versions = {name: value for name, value in numbers.items() if name.startswith("VERSION_")}
        languages = {name: value for name, value in numbers.items() if name.startswith("LANGUAGE_")}
        self.code = (FAMILY << 24 | versions[GAMES[self.game][1]] << 16 | languages[self.language_name] << 8
                     | self.rom[0x1E])
        return {"WT_SAVE_MAGIC": f"{self.save_magic:#010x}", "WT_VARIANT": f"{self.code:#010x}"}

    def compile_port(self):
        tree = self.work / "port"
        sources = [name for name in self.files if name != ASM]
        patches = [*self.deltas] + ([PORT_PATCH] if PORT_PATCH.is_file() else [])
        self._stage(tree, sources + self.headers, lambda name: (self.workspace / name).read_bytes(), patches)
        objects = toolchain.compile_dp_objects(self.workspace, tree, tree / "include", self.work / "port-obj", sources,
                                               self.version, wonder_trade=True, tls=self._tls(),
                                               defines=self.port_defines(), compiler=COMPILERS.get(self.language))
        build = self.workspace / self.arm9_build
        tls = sorted((build / "wt_tls").rglob("*.o"))
        require(tls, f"{build / 'wt_tls'} has no objects; build the workspace first")
        out = self.work / "link"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        self.objects = {}
        inputs = [(path, path.name) for path in objects]
        inputs += [(path, "tls_" + path.relative_to(build / "wt_tls").as_posix().replace("/", "_")) for path in tls]
        asm = ASM.replace("/", "_") + ".o"
        port_asm = self.work / "port-asm" / asm
        port_asm.parent.mkdir(parents=True, exist_ok=True)
        port_asm.write_bytes(self._start_menu(self.work / "port-asm", wonder_trade=True))
        inputs.append((port_asm, asm))
        _, wanted = link.defined_and_needed([path.read_bytes() for path, name in inputs if name != asm])
        for path, name in inputs:
            data = path.read_bytes()
            if name == asm:
                data = self._shared_tables(self._renumbered_banks(elf.with_function_sizes(data)), wanted)
            (out / name).write_bytes(elf.numbered_sections(elf.with_inline_addends(data)))
            self.objects[str(out / name)] = elf.Elf((out / name).read_bytes())
        self.port_objects = {self._file_of(name): self.objects[str(out / name)] for _, name in inputs
                             if self._file_of(name) in self.files}
        self.log(f"compiled {len(objects)} objects for variant {self.code:#010x}")

    def _start_menu(self, out, wonder_trade=False):
        """The start menu's object: the build's, or, for a language whose start menu is written
        out from the cartridge, that assembly (with Wonder Trade's changes) assembled here."""
        if self.language not in START_MENUS:
            build = self.workspace if wonder_trade else self.pristine
            return (build / self.arm9_build / "asm/unk_02035068.o").read_bytes()
        changes = START_MENUS[self.language]
        tree = out / "src"
        shutil.rmtree(tree, ignore_errors=True)
        (tree / ASM).parent.mkdir(parents=True)
        (tree / ASM).write_text(self._written_start_menu())
        if wonder_trade:
            result = subprocess.run(["patch", "-s", "-p1", "--fuzz=0", "--no-backup-if-mismatch", "-d", str(tree),
                                     "-i", str(RETAIL / changes)], capture_output=True, text=True)
            require(result.returncode == 0, f"{changes} does not apply: {result.stdout}{result.stderr}")
        return toolchain.assemble_dp(self.workspace, tree, out, ASM, self.version, self._tls())

    def _written_start_menu(self):
        """The player's own start menu as assembly, made from the dump (never stored in the repository)."""
        if getattr(self, "start_menu_source", None) is None:
            us = elf.Elf(elf.with_function_sizes((self.pristine / self.arm9_build / "asm/unk_02035068.o").read_bytes()))
            ro = next(i for i, s in enumerate(us.sections) if s["name"] == ".rodata")
            body = us.section_bytes(ro)
            keep = [True] * len(body)
            for offset, *_ in us.relocs.get(ro, ()):
                keep[offset:offset + 4] = [False] * 4
            rodata_at = symbols.unique_match(self.modules, "arm9", body, keep, 4)
            require(rodata_at is not None, f"the tables of {ASM} were not found once in the retail game")
            placed = {}
            for num, at in self.map.addr.items():
                symbol = self.map._bynum[num]
                if self.image.module(symbol) == "arm9" and symbol.kind in symbols.FUNCTION_KINDS:
                    placed.setdefault(at & ~1, set()).add(symbol.name)
            try:
                self.start_menu_source = writeout.write_out(
                    us, git(self.workspace, "show", f"HEAD:{ASM}").decode(), writeout.Module(*self.modules["arm9"]),
                    rodata_at, placed, START_MENU_HEADER)
            except writeout.WriteOutError as error:
                raise PortError(f"{ASM} could not be written out from the retail game: {error}") from None
            (self.work / "start-menu.s").write_text(self.start_menu_source)
        return self.start_menu_source

    def _renumbered_banks(self, data):
        """The start menu assembly with the cartridge's message bank numbers.

        It loads banks as NewMsgDataFromNarc(..., NARC_msgdata_msg, bank, ...) with the bank
        in a literal (ldr r2, =bank); the C code gets them from msg.naix. A start menu written
        out from the cartridge already has them."""
        if self.bank_remap is None or self.language in START_MENUS:
            return data
        obj = elf.Elf(data)
        text = next(i for i, s in enumerate(obj.sections) if s["name"] == ".text")
        body = obj.section_bytes(text)
        start = obj.sections[text]["off"]
        out = bytearray(data)
        changed = 0
        for offset, kind, index, _ in obj.relocs.get(text, ()):
            if kind != elf.R_ARM_THM_CALL or obj.symbols[index]["name"] != "NewMsgDataFromNarc":
                continue
            words = {k: struct.unpack_from("<H", body, k)[0] for k in range(offset - 2, max(offset - 12, -2), -2) if k >= 0}
            if not any(word == 0x2100 | MESSAGE_NARC for word in words.values()):
                continue  # another archive
            site = next((k for k, word in words.items() if word >> 8 == 0x4A), None)  # ldr r2, [pc, #imm]
            require(site is not None, f"{ASM} loads a message bank at {offset:#x} other than from a literal")
            literal = ((site + 4) & ~3) + (words[site] & 0xFF) * 4
            bank = struct.unpack_from("<I", body, literal)[0]
            new = self.bank_remap.get(bank)
            require(new is not None, f"{ASM} uses message bank {bank}, which the cartridge does not have")
            struct.pack_into("<I", out, start + literal, new)
            changed += new != bank
        self.log(f"{ASM}: {changed} message bank numbers renumbered")
        return bytes(out)

    def _shared_tables(self, data, wanted):
        """Tables of the relinked assembly that other objects name stay the game's own.

        Relinked C code that the patch leaves alone must point where the retail code does,
        so an unchanged table it names keeps its retail address: the assembly's own
        references, made through its .rodata section symbol, get that address, and the name
        becomes undefined so other objects resolve it from the retail game. Every other
        table stays in the module with the assembly that reads it: its labels do not mark
        where a table ends (the start menu's entries span two labels), so splitting them
        could pair the module's entries with the game's."""
        pristine = elf.Elf(self.pristine_asm)
        port = elf.Elf(data)

        def tables(obj):
            ro = next(i for i, s in enumerate(obj.sections) if s["name"] == ".rodata")
            body = obj.section_bytes(ro)
            named = sorted((s["value"], k) for k, s in enumerate(obj.symbols)
                           if s["shndx"] == ro and s["bind"] and s["name"])
            spans = {}
            for i, (value, k) in enumerate(named):
                end = named[i + 1][0] if i + 1 < len(named) else len(body)
                spans[obj.symbols[k]["name"]] = (k, value, end, body[value:end])
            return ro, spans

        pristine_ro, before = tables(pristine)
        ro, after = tables(port)
        # The tables sit together in the game: find the pristine block once, relocated words masked.
        body = pristine.section_bytes(pristine_ro)
        keep = [True] * len(body)
        for offset, *_ in pristine.relocs.get(pristine_ro, ()):
            keep[offset:offset + 4] = [False] * 4
        block = symbols.unique_match(self.modules, "arm9", body, keep, 4)
        require(block is not None, f"the tables of {ASM} were not found once in the retail game")
        kept = {name: span for name, span in after.items()
                if name in wanted and name in before and before[name][3] == span[3]}
        data = bytearray(data)
        symtab = next(s for s in port.sections if s["type"] == elf.SHT_SYMTAB)
        rewritten = 0
        for name, (k, start, end, _) in kept.items():
            found = (block + before[name][1], False)
            for rela in port.sections:
                if rela["type"] != elf.SHT_RELA or rela["info"] not in port.relocs:
                    continue
                target = port.sections[rela["info"]]
                for j, (offset, info, addend) in enumerate(struct.iter_unpack("<IIi", port.data[rela["off"]:rela["off"] + rela["size"]])):
                    symbol = port.symbols[info >> 8]
                    if symbol["shndx"] != ro or symbol["bind"] or not start <= addend < end:
                        continue
                    require(info & 0xFF == elf.R_ARM_ABS32, f"{ASM} reaches {name} other than by address")
                    struct.pack_into("<I", data, target["off"] + offset, found[0] + addend - start)
                    struct.pack_into("<I", data, rela["off"] + 12 * j + 4, info & ~0xFF | elf.R_ARM_NONE)
                    rewritten += 1
            entry = symtab["off"] + 16 * k  # Elf32_Sym: name, value, size, info, other, shndx
            struct.pack_into("<II", data, entry + 4, 0, 0)
            data[entry + 12] = data[entry + 12] & 0xF0 | elf.STT_NOTYPE
            struct.pack_into("<H", data, entry + 14, 0)
        self.log(f"{len(kept)} tables of {ASM} stay the game's ({rewritten} references); "
                 f"{len(after) - len(kept)} move to the module")
        return bytes(data)

    # --- 3. Resolve and link ------------------------------------------------------------------------------
    def link(self):
        build = self.workspace / self.arm9_build
        library = sdk.Library([*sorted((build / "lib/NitroSDK").rglob("*.o")), *sorted((build / "lib/MSL_C").rglob("*.o")),
                               *[build / name for name in COPIED.get(self.language, ())],
                               *sorted((build / "lib/libnns").rglob("*.o")), build / "lib/libsyscall.a"])
        out = self.work / "link"
        self.base = platinum.retail_arena(self.rom)
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
        script = out / "blob.ld"
        firsts = [name.replace("/", "_") + ".o" for name in self.files]
        script.write_text(link.linker_script(self.base, firsts))
        paths = list(self.objects) + extracted + [str(game)]
        output = toolchain.link(paths, script, out / "blob.elf")
        objects = dict(self.objects)
        for path in extracted:
            objects[path] = elf.Elf(Path(path).read_bytes())
        self.linked = link.Linked(self.base, Path(f"{output}.bin").read_bytes(), Path(f"{output}.nm").read_text(),
                                  Path(f"{output}.map").read_text(), objects)
        self.port_paths = {name: str(out / (name.replace("/", "_") + ".o")) for name in self.files}

    # --- 4. Layout ----------------------------------------------------------------------------------------
    def derive_layout(self):
        self.heap_site = None
        work = self.work / "text"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        msg_mk = (self.workspace / "files/msgdata/msg.mk").read_text()
        # The start menu bank gains the menu label as its last entry.
        (work / "start_menu.bin").write_bytes(self.text_members[self.start_menu_bank])
        self._msgenc(work, "-d", "start_menu.bin", "start_menu.txt")
        lines = (work / "start_menu.txt").read_text(encoding="utf-8").splitlines()
        wt_rows = list(ET.parse(self.workspace / f"files/msgdata/msg/narc_{START_MENU_BANK:04d}.gmm").getroot().iter("row"))
        require(len(wt_rows) == len(lines) + 1, f"the retail start menu has {len(lines)} entries, not {len(wt_rows) - 1}")
        strings = json.loads((ROOT / "client/games/res/text/wonder_trade.json").read_text())
        self.menu_label = next(m for m in strings["messages"] if m["id"] == platinum.MENU_LABEL)[self.text_key]
        (work / "start_menu_wt.txt").write_text("\n".join([*lines, self.menu_label]) + "\n", encoding="utf-8")
        self._msgenc(work, "-e", "start_menu_wt.txt", "start_menu_wt.bin", "-k", hex(gmm_key(msg_mk, START_MENU_BANK)))
        # Wonder Trade's own bank, in the cartridge's language.
        source = (ROOT / f"client/games/files-dp/msgdata/msg/narc_{WT_BANK:04d}.gmm").read_text()
        (work / "wonder_trade.gmm").write_text(localized_gmm(source, self.gmm_language))
        self._msgenc(work, "-e", "wonder_trade.gmm", "wonder_trade.bin", "--gmm", "-H", "wonder_trade.h",
                     "-k", hex(gmm_key(msg_mk, WT_BANK)))
        members = list(self.text_members)
        members[self.start_menu_bank] = (work / "start_menu_wt.bin").read_bytes()
        members.append((work / "wonder_trade.bin").read_bytes())
        self.text = nds.narc_join(members, self.text_names)
        # The menu icon, appended to the menu graphics.
        wt_rom = (self.workspace / f"build/{self.game}.us/poke{self.game}.us.nds").read_bytes()
        wt_icons, _ = nds.narc_split(wt_rom[slice(*nds.file_span(wt_rom, nds.file_ids(wt_rom)[MENU_GRAPHICS]))])
        us_icons, _ = nds.narc_split(self.us_rom[slice(*nds.file_span(self.us_rom, nds.file_ids(self.us_rom)[MENU_GRAPHICS]))])
        require(len(wt_icons) == MENU_ICON + 1 and wt_icons[:MENU_ICON] == us_icons,
                "the Wonder Trade build's menu graphics differ from US apart from the icon")
        self.icons = nds.narc_join([*self.icon_members, wt_icons[MENU_ICON]], self.icon_names)

    def _msgenc(self, work, mode, source, output, *options):
        tool = "/workspace/tools/msgenc/msgenc"
        toolchain.docker({str(self.workspace): "/workspace", str(work.resolve()): "/text"},
                         " ".join([tool, mode, *options, "-c", "/workspace/charmap.txt", source, output]),
                         workdir="/text", image=toolchain.DP_IMAGE)

    # --- 5. Assemble the ROM --------------------------------------------------------------------------------
    def assemble_rom(self):
        rom = bytearray(self.rom)
        arm9_offset, arm9_size = nds.u32(rom, 0x20), nds.u32(rom, 0x2C)
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
        overlays = {row[0]: row for row in nds.overlays(rom)}
        for module, at, code in patches:
            if module == "arm9":
                offset = arm9_offset + at - nds.ARM9_RAM
            else:
                oid, ram, _, _, fid, compressed = overlays[module]
                require(not compressed, f"overlay {oid} is compressed")
                offset = nds.file_span(rom, fid)[0] + at - ram
            rom[offset:offset + len(code)] = code
        arm9 = rom[arm9_offset:arm9_offset + arm9_size]
        literal = platinum.arena_literal(arm9, self.base)
        new_arm9 = nds.add_autoload(bytes(rom), segment, self.base, literal, end)
        self.arena, self.trampolines = end, len(patches)
        return nds.relayout(bytes(rom), new_arm9, {self.text_fid: self.text, self.icon_fid: self.icons})

    def headroom(self, arena):
        """Main RAM left after the heaps InitSystem sets up, with room for its tables and task managers."""
        params = self.image.unique("UNK_020EDB10")
        require(params is not None and params.size % 8 == 0, "the heap table UNK_020EDB10 was not found")
        base, body = self.map.us_modules["arm9"]
        rows = [struct.unpack_from("<II", body, params.start - base + k) for k in range(0, params.size, 8)]
        require(all(arena_id == platinum.OS_ARENA_MAIN for _, arena_id in rows), "a heap is outside main RAM")
        tables = 92 * 16 + 0x100  # the heap system's per-heap tables, generously
        tasks = 0x4000  # the task managers and the file system tables, generously
        reserved = platinum.PRE_HEAP_PADDING + tables + sum(size for size, _ in rows) + tasks
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
