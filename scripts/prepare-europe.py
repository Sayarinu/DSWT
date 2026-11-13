#!/usr/bin/env python3
"""Stage European English Platinum build inputs from a verified local dump.

The European cartridge runs US Rev 1 code; only its header area and a few
NitroFS files differ. This copies exactly those parts from the maintainer's own
dump into the ignored workspace record `.git/dswt-europe/`, which the revision
10 build reads. Nothing is written until the dump matches the catalogue SHA-256.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
VARIANT = "europe"
REVISION = 10
OUTPUT = ".git/dswt-europe"
# pret's template keeps fields nitrorom rewrites; the cartridge's own bytes follow.
HEADER_FIELDS, HEADER_AREA = 0x160, 0x4000
# Whole archives that differ from US Rev 1 and that DSWT leaves unchanged.
FILES = (
    "/data/slot.narc",
    "/demo/title/op_demo.narc",
    "/fielddata/land_data/land_data.narc",
    "/fielddata/script/scr_seq.narc",
)
# Their sources in the workspace; DSWT overlays must not touch them.
FILE_SOURCES = (
    "res/prebuilt/data/slot.narc",
    "res/prebuilt/demo/title/op_demo.narc",
    "res/field/maps/",
    "res/field/scripts/",
)
# DSWT edits other text banks and appends its own, so these are merged per bank.
TEXT = "/msgdata/pl_msg.narc"
TEXT_BANKS = (143, 147, 366, 417, 544, 623)


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def load(path):
    """Read a full-size dump, or a trimmed one restored with its 0xFF padding."""
    catalog = json.loads((ROOT / "patches/compatibility.json").read_text())
    variant = next(row for row in catalog["variants"] if row["id"] == VARIANT)
    data = Path(path).read_bytes()
    if len(data) < variant["size"]:
        data += b"\xff" * (variant["size"] - len(data))
    if len(data) != variant["size"] or hashlib.sha256(data).hexdigest() != variant["sha256"]:
        raise ValueError(f"{path} is not the verified {variant['game']} {variant['name']} dump")
    pin = json.loads((ROOT / "client/upstream.json").read_text())
    if (data[0x0C:0x10], data[0x1E]) != (b"CPUE", REVISION) or variant["sha1"] != pin["rom_sha1"][str(REVISION)]:
        raise ValueError("Catalogue and build records disagree on the European cartridge")
    return data, variant


def nitrofs(rom):
    """Map each NitroFS path to its file bytes."""
    fnt, fat = u32(rom, 0x40), u32(rom, 0x48)
    files = {}

    def walk(directory, prefix):
        entry = fnt + 8 * (directory & 0xFFF)
        cursor, file_id = fnt + u32(rom, entry), u16(rom, entry + 4)
        while length := rom[cursor]:
            name = rom[cursor + 1:cursor + 1 + (length & 0x7F)].decode("ascii")
            cursor += 1 + (length & 0x7F)
            if length & 0x80:
                walk(u16(rom, cursor), f"{prefix}{name}/")
                cursor += 2
            else:
                start, end = u32(rom, fat + 8 * file_id), u32(rom, fat + 8 * file_id + 4)
                files[prefix + name] = rom[start:end]
                file_id += 1

    walk(0xF000, "/")
    return files


def narc_members(archive):
    if archive[:4] != b"NARC":
        raise ValueError("Expected a NARC archive")
    table = u16(archive, 0x0C)
    count = u16(archive, table + 8)
    ranges = [(u32(archive, table + 12 + 8 * i), u32(archive, table + 16 + 8 * i)) for i in range(count)]
    names = table + u32(archive, table + 4)
    images = names + u32(archive, names + 4) + 8
    return [archive[images + start:images + end] for start, end in ranges]


def stage(workspace, data, variant):
    files = nitrofs(data)
    platinum = workspace / "platinum.us"
    banks = [line.split()[0].lower().removeprefix("text_bank_")
             for line in (workspace / "generated/text_banks.txt").read_text().splitlines()
             if line and not line.startswith("#")]
    marker = workspace / ".git/dswt-overlays.json"
    managed = json.loads(marker.read_text()) if marker.exists() else []
    if any(name.startswith(FILE_SOURCES) for name in managed):
        raise RuntimeError("DSWT changes a European archive's sources; merge it per member first")

    out = {"rom_header_template.sbin":
           (platinum / "rom_header_template_rev1.sbin").read_bytes()[:HEADER_FIELDS]
           + data[HEADER_FIELDS:HEADER_AREA]}
    ini = (platinum / "rom_rev1.ini").read_text()
    if ini.count("revision   = 1\n") != 1:
        raise RuntimeError("Unexpected upstream Rev 1 ROM definition")
    out["rom.ini"] = ini.replace("revision   = 1\n", f"revision   = {REVISION}\n").encode()
    out["rom.sha1"] = f"{variant['sha1']} *pokeplatinum.us.nds\n".encode()
    for path in FILES:
        out["files" + path] = files[path]
    text = narc_members(files[TEXT])
    for index in TEXT_BANKS:
        out[f"text/{banks[index]}"] = text[index]

    # Point the ROM layout and its checks at the European archives.
    moved = {path: f"../{OUTPUT}/files{path}" for path in FILES}
    targets, layout = {}, []
    for line in (platinum / "filesys.csv").read_text().splitlines():
        source, _, target = line.partition(",")
        targets[source] = target
        layout.append(f"{moved.get(target, source)},{target}")
    if not set(FILES + (TEXT,)) <= set(targets.values()):
        raise RuntimeError("Upstream filesystem layout lacks a European file")
    checks, european = [], 0
    for line in (platinum / "filesys.sha1").read_text().splitlines():
        digest, _, source = line.partition(" *")
        target = targets.get(source)
        if target in moved or target == TEXT:
            digest, source = hashlib.sha1(files[target]).hexdigest(), moved.get(target, source)
            european += 1
        checks.append(f"{digest} *{source}")
    if european != len(FILES) + 1:
        raise RuntimeError("Upstream filesystem checksums lack a European file")
    out["filesys.csv"] = ("\n".join(layout) + "\n").encode()
    out["filesys.sha1"] = ("\n".join(checks) + "\n").encode()
    return out


def prepare(dump, workspace):
    workspace = Path(workspace).resolve()
    if not (workspace / ".git").is_dir() or (workspace / ".git").is_symlink():
        raise RuntimeError("Prepare the game workspace first")
    data, variant = load(dump)
    staged = stage(workspace, data, variant)
    target = workspace / OUTPUT
    if target.is_symlink():
        raise RuntimeError("European input record must not be a symlink")
    with tempfile.TemporaryDirectory(prefix=".dswt-europe-", dir=workspace / ".git") as temp:
        temp = Path(temp)
        for name, content in staged.items():
            path = temp / "inputs" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        if target.exists():
            shutil.rmtree(target)
        (temp / "inputs").rename(target)
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", help="your own European English Platinum dump")
    parser.add_argument("workspace", help="prepared game workspace")
    args = parser.parse_args()
    try:
        print(prepare(args.dump, args.workspace))
    except (OSError, ValueError, RuntimeError, KeyError, IndexError) as error:
        print(f"Cannot prepare European inputs: {error}", file=sys.stderr)
        sys.exit(1)
