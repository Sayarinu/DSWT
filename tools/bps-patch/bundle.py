"""Portable release bundle logic shared by the patcher CLI and the desktop app.

Standard library only, and no Docker: it identifies a cartridge dump, restores a
trimmed dump, selects exactly one manifest entry, applies and verifies its patch,
and writes the result to a new file without replacing anything.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps

KIND = "dswt-bps-bundle"
SCHEMA_VERSION = 3
MAX_MANIFEST = 1024 * 1024
PATCH_NAME = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}\.bps")
VARIANT = re.compile(r"[a-z0-9][a-z0-9-]{0,31}")
SHA256 = re.compile(r"[0-9a-f]{64}")
CRC32 = re.compile(r"[0-9a-f]{8}")
HEADER_SIZE = 0x200
GAME_CODE = slice(0x0C, 0x10)
ROM_VERSION = 0x1E
USED_SIZE = 0x80
# Gen IV Pokémon games by the first three letters of the header game code, with
# their full cartridge size in bytes.
GAMES = {
    "ADA": ("Pokémon Diamond", 64 * 1024 * 1024),
    "APA": ("Pokémon Pearl", 64 * 1024 * 1024),
    "CPU": ("Pokémon Platinum", 128 * 1024 * 1024),
    "IPK": ("Pokémon HeartGold", 128 * 1024 * 1024),
    "IPG": ("Pokémon SoulSilver", 128 * 1024 * 1024),
}
# The last letter of the game code: region and language.
REGIONS = {
    "J": ("Japan", "Japanese"),
    "E": ("North America", "English"),
    "P": ("Europe", "English"),
    "F": ("France", "French"),
    "D": ("Germany", "German"),
    "I": ("Italy", "Italian"),
    "S": ("Spain", "Spanish"),
    "K": ("Korea", "Korean"),
}
# Retail cartridges by game code: (game, region, language).
CARTRIDGES = {prefix + letter: (game, *REGIONS[letter]) for prefix, (game, _) in GAMES.items() for letter in REGIONS}
GAME_NAMES = "Pokémon Diamond, Pearl, Platinum, HeartGold or SoulSilver"
# European English cartridges that reuse the US game code, told apart by their
# header revision: Platinum Europe reads CPUE revision 10.
EUROPE_AS_US = {("CPUE", 10)}
# What a patch applies to: the ROM itself, or the ROM followed by its compressed
# ARM9 binary and overlays expanded in ROM order. HeartGold and SoulSilver
# compress their code, so their patches copy unchanged code from the player's
# own expanded ROM instead of carrying retail code, compressed or not.
SOURCE_VIEWS = ("rom", "expanded-code")
# The ARM9 binary ends with this marker and the offset of its module parameters.
NITRO_FOOTER = 0xDEC00621


class BundleError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def crc32_hex(data):
    return f"{bps.crc32(data):08x}"


def cartridge(data):
    """Describe a Gen IV Pokémon dump from its header, or return None for anything else."""
    if len(data) < HEADER_SIZE:
        return None
    try:
        code = bytes(data[GAME_CODE]).decode("ascii")
    except UnicodeDecodeError:
        return None
    if code not in CARTRIDGES:
        return None
    game, region, language = CARTRIDGES[code]
    revision = data[ROM_VERSION]
    if (code, revision) in EUROPE_AS_US:
        region = "Europe"
    return {"game": game, "game_code": code, "region": region, "language": language,
            "revision": revision, "size": GAMES[code[:3]][1]}


def describe(info):
    return f"{info['game']} ({info['region']}, {info['language']}, Rev {info['revision']})"


def title(entry):
    """Name a manifest entry with its game, such as "Pokémon HeartGold USA Rev 0"."""
    return f"{GAMES[entry['game_code'][:3]][0]} {entry['name']}"


def restore(data, size):
    """Pad a trimmed dump back to the cartridge size with the erased-flash value it held."""
    if len(data) >= size or len(data) < HEADER_SIZE:
        return data
    used = int.from_bytes(data[USED_SIZE:USED_SIZE + 4], "little")
    if not used or len(data) < used:
        return data
    return data + b"\xff" * (size - len(data))


def _require(condition, message):
    if not condition:
        raise BundleError(f"Malformed manifest: {message}")


def _blz_expand(data, label):
    """Expand one backward-LZ (BLZ) compressed binary, as the DS loader does."""
    if len(data) < 8:
        raise BundleError(f"{label} has malformed compressed code")
    end, extra = struct.unpack_from("<II", data, len(data) - 8)
    header, compressed = end >> 24, end & 0xFFFFFF
    if header < 8 or header > compressed or compressed > len(data):
        raise BundleError(f"{label} has malformed compressed code")
    out = bytearray(data) + bytearray(extra)
    source, target, stop = len(data) - header, len(out), len(data) - compressed
    while target > stop:
        if source <= stop:
            raise BundleError(f"{label} has malformed compressed code")
        source -= 1
        flags = data[source]
        for _ in range(8):
            if target <= stop:
                break
            if flags & 0x80:
                source -= 2
                token = data[source + 1] << 8 | data[source] if source >= stop else 0
                length, distance = (token >> 12) + 3, (token & 0xFFF) + 3
                if source < stop or length > target - stop or target - 1 + distance >= len(out):
                    raise BundleError(f"{label} has malformed compressed code")
                for _ in range(length):
                    target -= 1
                    out[target] = out[target + distance]
            else:
                source -= 1
                if source < stop:
                    raise BundleError(f"{label} has malformed compressed code")
                target -= 1
                out[target] = data[source]
            flags = flags << 1 & 0xFF
    return bytes(out)


def compressed_code(rom, label):
    """Return [(name, compressed bytes, expanded bytes)] for the ARM9 binary and each compressed overlay."""
    modules = []
    try:
        offset, _, ram, size = struct.unpack_from("<IIII", rom, 0x20)
        if offset + size + 8 <= len(rom) and struct.unpack_from("<I", rom, offset + size)[0] == NITRO_FOOTER:
            params = struct.unpack_from("<I", rom, offset + size + 4)[0]
            end = struct.unpack_from("<I", rom, offset + params + 0x14)[0] if params + 0x18 <= size else 0
            if end:
                if not 0 < end - ram <= size:
                    raise BundleError(f"{label} has malformed compressed code")
                code = rom[offset:offset + end - ram]
                modules.append(("ARM9", code, _blz_expand(code, label)))
        fat, fat_size, table, table_size = struct.unpack_from("<IIII", rom, 0x48)
        for entry in range(table, table + table_size - 31, 32):
            overlay = struct.unpack_from("<I", rom, entry)[0]
            file_id, flags = struct.unpack_from("<II", rom, entry + 24)
            if flags >> 24 & 1:
                if file_id * 8 + 8 > fat_size:
                    raise BundleError(f"{label} has malformed compressed code")
                start, stop = struct.unpack_from("<II", rom, fat + file_id * 8)
                if not start <= stop <= len(rom):
                    raise BundleError(f"{label} has malformed compressed code")
                code = rom[start:stop]
                modules.append((f"overlay {overlay}", code, _blz_expand(code, label)))
    except struct.error:
        raise BundleError(f"{label} has malformed compressed code") from None
    return modules


def source_view(rom, view, label="ROM"):
    """Return the bytes a patch applies to: the ROM, or the ROM followed by its expanded code."""
    if view == "rom":
        return rom
    return rom + b"".join(expanded for _, _, expanded in compressed_code(rom, label))


def load_manifest(data):
    """Parse and validate a release manifest; every later check relies on these shapes."""
    if len(data) > MAX_MANIFEST:
        raise BundleError("Malformed manifest: too large")
    try:
        manifest = json.loads(data)
    except ValueError:
        raise BundleError("Malformed manifest: not JSON") from None
    _require(isinstance(manifest, dict) and manifest.get("schema_version") == SCHEMA_VERSION
             and manifest.get("kind") == KIND, "unsupported schema")
    _require(isinstance(manifest.get("version"), str) and manifest["version"], "missing version")
    entries = manifest.get("entries")
    _require(isinstance(entries, list) and entries, "no entries")
    seen = {"variant": set(), "file": set(), "input": set(), "output": set()}
    for entry in entries:
        _require(isinstance(entry, dict) and isinstance(entry.get("variant"), str)
                 and VARIANT.fullmatch(entry["variant"]) and isinstance(entry.get("name"), str), "invalid entry")
        _require(entry.get("game_code") in CARTRIDGES and type(entry.get("revision")) is int, "invalid cartridge")
        _require(entry.get("source_view") in SOURCE_VIEWS, "invalid source view")
        for part, limit in (("input", bps.MAX_FILE), ("patch", bps.MAX_PATCH), ("output", bps.MAX_FILE)):
            value = entry.get(part)
            _require(isinstance(value, dict) and type(value.get("size")) is int and 0 < value["size"] <= limit
                     and isinstance(value.get("sha256"), str) and SHA256.fullmatch(value["sha256"])
                     and isinstance(value.get("crc32"), str) and CRC32.fullmatch(value["crc32"]), f"invalid {part}")
        filename = entry["patch"].get("file")
        _require(isinstance(filename, str) and PATCH_NAME.fullmatch(filename), "unsafe patch filename")
        for key, value in (("variant", entry["variant"]), ("file", filename),
                           ("input", entry["input"]["sha256"]), ("output", entry["output"]["sha256"])):
            _require(value not in seen[key], f"duplicate {key}")
            seen[key].add(value)
    _require(not seen["input"] & seen["output"], "an output is also an input")
    return manifest


def select(manifest, data):
    """Return (entry, full-size source) for an unmodified supported dump, or explain why not."""
    entries = manifest["entries"]
    for entry in entries:
        candidate = restore(data, entry["input"]["size"])
        if len(candidate) == entry["input"]["size"] and sha256(candidate) == entry["input"]["sha256"]:
            return entry, candidate
    for entry in entries:
        candidate = restore(data, entry["output"]["size"])
        if len(candidate) == entry["output"]["size"] and sha256(candidate) == entry["output"]["sha256"]:
            raise BundleError(f"This ROM already has DSWT {manifest['version']} applied.")
    info = cartridge(data)
    if info is None:
        raise BundleError(f"This file is not a {GAME_NAMES} ROM.")
    supported = [entry for entry in entries if entry["game_code"] == info["game_code"]
                 and entry["revision"] == info["revision"]]
    if not supported:
        names = ", ".join(sorted({title(entry) for entry in entries}))
        raise BundleError(f"{describe(info)} is not supported yet. Supported: {names}.")
    if len(data) < HEADER_SIZE + 1 or len(data) > supported[0]["input"]["size"]:
        raise BundleError(f"{describe(info)} has an unexpected size; use a clean, decrypted dump.")
    raise BundleError(f"{describe(info)} was recognised, but this dump is modified, encrypted or damaged. "
                      "Use a clean dump of your own cartridge.")


def patch_rom(entry, source, patch):
    """Apply one entry's patch and verify every recorded digest of the patch and output."""
    if len(patch) != entry["patch"]["size"] or sha256(patch) != entry["patch"]["sha256"]:
        raise BundleError("The patch file does not match the manifest; download the release again.")
    if crc32_hex(source) != entry["input"]["crc32"]:
        raise BundleError("Input CRC32 does not match the manifest")
    try:
        target = bps.apply(source_view(source, entry["source_view"]), patch)
    except bps.BPSError as error:
        raise BundleError(f"Patch failed: {error}") from None
    if (len(target) != entry["output"]["size"] or crc32_hex(target) != entry["output"]["crc32"]
            or sha256(target) != entry["output"]["sha256"]):
        raise BundleError("Patched output failed verification; nothing was written.")
    return target


def read_file(path, limit, label):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise BundleError(f"{label} must be a regular file")
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    if len(data) > limit:
        raise BundleError(f"{label} is too large")
    return data


def write_new(path, data):
    """Write a new file through a flushed temporary file; never replace an existing path."""
    path = Path(path)
    if path.name in ("", ".", "..") or os.path.lexists(path):
        raise BundleError(f"{path} already exists; choose a new file name")
    parent = path.parent.resolve(strict=True)
    temporary = parent / f".{path.name}.{secrets.token_hex(8)}.partial"
    try:
        with open(temporary, "xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        if os.name == "nt":
            os.rename(temporary, parent / path.name)  # Windows rename never replaces.
        else:
            try:
                os.link(temporary, parent / path.name)  # Fails if the name appeared meanwhile.
            except FileExistsError:
                raise BundleError(f"{path} appeared while patching; it was preserved") from None
            except OSError:
                raise BundleError("This folder cannot create files safely; choose a folder on a local disk") from None
    except FileExistsError:
        raise BundleError(f"{path} appeared while patching; it was preserved") from None
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    if os.name != "nt":
        fd = os.open(parent, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def default_output(input_path):
    input_path = Path(input_path)
    return input_path.with_name(f"{input_path.stem} (Wonder Trade).nds")


def patch_file(bundle_dir, input_path, output_path=None):
    """Patch one dump with a release bundle directory; return (entry, output path)."""
    bundle_dir = Path(bundle_dir)
    manifest = load_manifest(read_file(bundle_dir / "manifest.json", MAX_MANIFEST, "Manifest"))
    data = read_file(input_path, bps.MAX_FILE, "ROM")
    entry, source = select(manifest, data)
    patch = read_file(bundle_dir / entry["patch"]["file"], bps.MAX_PATCH, "Patch")
    target = patch_rom(entry, source, patch)
    output_path = Path(output_path) if output_path else default_output(input_path)
    write_new(output_path, target)
    return entry, output_path
