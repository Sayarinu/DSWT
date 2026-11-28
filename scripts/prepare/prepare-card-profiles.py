"""Regenerate cartridge identity metadata from locally owned, catalog-verified dumps."""
from contextlib import ExitStack
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "client/companion/src/platform/nds/card_profiles.inc"
PAGE = 512
MAIN_DATA_START = 0x8000
# Main-program and data pages which distinguish regional builds and revisions.
COMMON = (MAIN_DATA_START, 0xa400, 0x107000, 0x432e00, 0xd0fc00)


def page(file, offset):
    file.seek(offset)
    data = file.read(PAGE)
    if len(data) != PAGE:
        raise ValueError(f"Short cartridge page at {offset:#x}")
    return data


def literal(data):
    return '"' + ''.join(chr(b) if b in range(ord(" "), ord("~") + 1) and b not in (34, 92)
                         else f'\\{b:03o}' for b in data) + '"'


def hex_array(data):
    return "{ " + ", ".join(f"0x{byte:02x}" for byte in data) + " }"


def separation_page(paths, limit):
    """Find a page that distinguishes builds with the same common pages."""
    with ExitStack() as stack:
        files = [stack.enter_context(p.open("rb")) for p in paths]
        for f in files:
            f.seek(MAIN_DATA_START)
        for start in range(MAIN_DATA_START, limit, 65536):
            blocks = [f.read(min(65536, limit - start)) for f in files]
            if len(set(blocks)) == 1:
                continue
            for i in range(0, len(blocks[0]) - PAGE + 1, PAGE):
                if len({block[i:i + PAGE] for block in blocks}) == len(paths):
                    return start + i
    raise ValueError(f"No distinguishing main-data page for {paths}")


def main():
    variants = json.loads((ROOT / "client/games/compatibility.json").read_text())["variants"]
    groups = {}
    for variant in variants:
        name = variant["id"]
        path = ROOT / "game/dumps" / name / (name + ".nds")
        if not path.is_file():
            path = ROOT / "game/dumps-gen5" / (name + ".nds")
        with path.open("rb") as f:
            if hashlib.file_digest(f, "sha256").hexdigest() != variant["sha256"]:
                raise ValueError(f"Unrecognized dump for {name}")
            header = page(f, 0)
            base = hashlib.sha256()
            for at in COMMON:
                base.update(page(f, at))
        groups.setdefault(base.digest()[:4], []).append((path, header, base, name))
    out = ["/* Cartridge identity and filesystem metadata. Regenerate with prepare-card-profiles.py. */",
           "static const uint32_t commonPages[] = { " + ", ".join(hex(i) for i in COMMON) + " };",
           "static const CpCardProfile profiles[] = {"]
    for selector, group in groups.items():
        extra = None
        if len(group) > 1:
            limit = min(struct.unpack_from("<I", header, 0x80)[0] for _, header, _, _ in group)
            extra = separation_page([path for path, _, _, _ in group], limit)
        for path, header, base, name in group:
            fnt, _, fat, _ = struct.unpack_from("<4I", header, 0x40)
            capacity, = struct.unpack_from("<I", header, 0x80)
            banner, = struct.unpack_from("<I", header, 0x68)
            probe = extra if extra is not None else banner // PAGE * PAGE
            digest = base.copy()
            with path.open("rb") as f:
                for at in (probe, fnt // PAGE * PAGE, fat // PAGE * PAGE):
                    if not MAIN_DATA_START <= at <= capacity - PAGE:
                        raise ValueError(f"Page outside NTR data in {name}")
                    digest.update(page(f, at))
            out.extend([f"    /* {name} */",
                        f"    {{ 0x{int.from_bytes(selector, 'little'):08x}, {probe:#x}, {capacity:#x},",
                        f"      {literal(header[:32].rstrip(bytes(1)))}, {hex_array(header[0x40:0x50])},",
                        f"      {hex_array(digest.digest())} }},"])
    OUT.write_text("\n".join([*out, "};", ""]))
    print(f"Generated {len(variants)} verified cartridge profiles")


if __name__ == "__main__":
    main()
