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
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bps

KIND = "dswt-bps-bundle"
SCHEMA_VERSION = 2
MAX_MANIFEST = 1024 * 1024
FULL_SIZE = 128 * 1024 * 1024
PATCH_NAME = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}\.bps")
VARIANT = re.compile(r"[a-z0-9][a-z0-9-]{0,31}")
SHA256 = re.compile(r"[0-9a-f]{64}")
CRC32 = re.compile(r"[0-9a-f]{8}")
HEADER_SIZE = 0x200
GAME_CODE = slice(0x0C, 0x10)
ROM_VERSION = 0x1E
USED_SIZE = 0x80
# Retail Pokémon Platinum cartridges by game code.
CARTRIDGES = {
    "CPUJ": ("Japan", "Japanese"),
    "CPUE": ("North America", "English"),
    "CPUP": ("Europe", "English"),
    "CPUF": ("France", "French"),
    "CPUD": ("Germany", "German"),
    "CPUI": ("Italy", "Italian"),
    "CPUS": ("Spain", "Spanish"),
    "CPUK": ("Korea", "Korean"),
}


class BundleError(ValueError):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def crc32_hex(data):
    return f"{bps.crc32(data):08x}"


def cartridge(data):
    """Describe a Platinum dump from its header, or return None for anything else."""
    if len(data) < HEADER_SIZE:
        return None
    try:
        code = bytes(data[GAME_CODE]).decode("ascii")
    except UnicodeDecodeError:
        return None
    if code not in CARTRIDGES:
        return None
    region, language = CARTRIDGES[code]
    return {"game_code": code, "region": region, "language": language, "revision": data[ROM_VERSION]}


def describe(info):
    return f"Pokémon Platinum ({info['region']}, {info['language']}, Rev {info['revision']})"


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
        raise BundleError("This file is not a Pokémon Platinum ROM.")
    supported = [entry for entry in entries if entry["game_code"] == info["game_code"]
                 and entry["revision"] == info["revision"]]
    if not supported:
        names = ", ".join(sorted({entry["name"] for entry in entries}))
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
        target = bps.apply(source, patch)
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
