"""Read the symbol map of a Black or White build of Project-Holon/aletheia.

aletheia links every region of the cartridge (ARM9 main, ITCM and each
overlay) from source to its original address. Each region's ELF in
build/gamelink_data/ defines that region's functions and data at their
addresses, and the region's image must equal the retail module byte for byte,
so its symbols are the retail game's own: no matching is needed.
"""

import collections
import json
from pathlib import Path
import re

from . import elf, nds

# Region directories of aletheia's data-mode links: build/gamelink_data/NAME[_white]/.
REGION = re.compile(r"^(arm9_main|ov\d{3})$")
# Overlay 230 keeps most of its code encrypted in the cartridge and decrypts it
# when it loads (aletheia's tools/ovcrypt.py); its link holds the decrypted code.
# Wonder Trade neither uses nor changes it.
ENCRYPTED = {230}
# Sections a region defines its own code and data in: text units (t<N>) and data (d...).
OWN_SECTION = re.compile(r"^(t\d+|dtail|ddata|dbss|d\w*)$")


class AletheiaError(ValueError):
    pass


Symbol = collections.namedtuple("Symbol", "name address size code thumb region")


def region_key(name):
    """'arm9' for ARM9 main, else the overlay number."""
    return "arm9" if name == "arm9_main" else int(name[2:])


class SymbolMap:
    """The functions and data of every linked region of one target ("black" or "white")."""

    def __init__(self, workspace, target, modules):
        if target not in ("black", "white"):
            raise AletheiaError(f"unsupported target {target}")
        self.workspace = Path(workspace)
        suffix = "" if target == "black" else "_white"
        root = self.workspace / "build/gamelink_data"
        self.regions, self.byname = {}, collections.defaultdict(list)
        names = sorted({p.name.removesuffix(suffix) for p in root.iterdir()
                        if p.is_dir() and p.name.endswith(suffix) and REGION.match(p.name.removesuffix(suffix))})
        if "arm9_main" not in names:
            raise AletheiaError(f"{root} has no {target} ARM9 link; build it with build-rom.sh --pristine")
        for name in names:
            directory = root / (name + suffix)
            info = json.loads((directory / "ov.json").read_text())
            if not info.get("ok") or info.get("version", target) != target:
                raise AletheiaError(f"{directory} is not a finished {target} link")
            key = region_key(name)
            if key in ENCRYPTED:
                continue
            image = (directory / "ov.bin").read_bytes()
            if key not in modules:
                raise AletheiaError(f"the cartridge has no region {key}")
            base, body = modules[key]
            if base != info["base"] or body[:len(image)] != image or (key != "arm9" and len(body) != len(image)):
                raise AletheiaError(f"aletheia's {name} does not match the cartridge byte for byte")
            self.regions[key] = (info["base"], len(image))
            self._read(key, elf.Elf((directory / "ov.elf").read_bytes()))

    def _read(self, region, image):
        marks = image.mapping_modes()
        for raw in image.symbols:
            shndx = raw["shndx"]
            if (not raw["name"] or raw["name"].startswith("$") or not 0 < shndx < elf.SHN_LORESERVE
                    or not OWN_SECTION.match(image.sections[shndx]["name"])
                    or raw["type"] not in (elf.STT_FUNC, elf.STT_OBJECT, elf.STT_ARM_TFUNC)):
                continue
            code = raw["type"] != elf.STT_OBJECT
            thumb = code and (raw["value"] & 1 or raw["type"] == elf.STT_ARM_TFUNC
                              or elf.mode_at(marks.get(shndx, ()), raw["value"] & ~1) == "$t")
            self.byname[raw["name"]].append(Symbol(raw["name"], raw["value"] & ~1, raw["size"], code, bool(thumb), region))

    def get(self, name, region=None):
        """The one symbol of that name (in that region, if given), or None."""
        found = [s for s in self.byname.get(name, ()) if region is None or s.region == region]
        found = list({(s.address, s.region): s for s in found}.values())
        if len(found) > 1:
            raise AletheiaError(f"{name} names {len(found)} symbols: {[(s.region, hex(s.address)) for s in found]}")
        return found[0] if found else None

    def address(self, name, region=None):
        """Address with the Thumb bit for a Thumb function."""
        symbol = self.get(name, region)
        if symbol is None:
            raise AletheiaError(f"{name} is not in the aletheia link")
        return symbol.address | int(symbol.thumb)


def modules(rom):
    """{"arm9" or overlay id: (RAM address, expanded bytes)} of a cartridge."""
    return nds.modules(rom)
