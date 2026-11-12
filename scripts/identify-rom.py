#!/usr/bin/env python3
"""Identify a verified pristine patch baseline without changing the input."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "patches/compatibility.json"
SCHEMA_VERSION = 2
CHUNK = 1024 * 1024


def _load_bundle():
    spec = importlib.util.spec_from_file_location("dswt_bundle", ROOT / "tools/bps-patch/bundle.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


bundle = _load_bundle()


def title(variant):
    return f"{variant['game']} {variant['name']}"


def identify(path, catalog):
    """Match a dump, or a trimmed dump restored with its erased 0xFF padding, to one catalogue entry."""
    digest = hashlib.sha256()
    size, header = 0, b""
    with path.open("rb") as rom:
        for chunk in iter(lambda: rom.read(CHUNK), b""):
            if len(header) < bundle.HEADER_SIZE:
                header += chunk[:bundle.HEADER_SIZE - len(header)]
            digest.update(chunk)
            size += len(chunk)
    used = int.from_bytes(header[bundle.USED_SIZE:bundle.USED_SIZE + 4], "little")
    trimmed = len(header) == bundle.HEADER_SIZE and 0 < used <= size
    candidates = {size: digest.hexdigest()}
    for full in sorted({variant["size"] for variant in catalog["variants"]}):
        if trimmed and full > size:
            padded = digest.copy()
            for offset in range(size, full, CHUNK):
                padded.update(b"\xff" * min(CHUNK, full - offset))
            candidates[full] = padded.hexdigest()
    found = [match(full, sha256, catalog) for full, sha256 in candidates.items() if _listed(full, sha256, catalog)]
    if found:
        return found[0]
    return match(size, candidates[size], catalog, bundle.cartridge(header))


def _listed(size, sha256, catalog):
    return any(variant["size"] == size and variant["sha256"] == sha256 for variant in catalog["variants"])


def match(size, sha256, catalog, info=None):
    """Select the single supported catalogue entry for an input's size and SHA-256, or explain why not.

    info is the input's decoded cartridge header, used only to explain a refusal."""
    matches = [
        variant for variant in catalog["variants"]
        if variant["size"] == size and variant["sha256"] == sha256
    ]
    if len(matches) > 1:
        raise ValueError(f"Unrecognized input: ambiguous catalogue entries for size={size}, SHA-256={sha256}.")
    if not matches:
        if info is None:
            raise ValueError(
                f"Unrecognized input: size={size}, SHA-256={sha256}. "
                f"A clean, decrypted {bundle.GAME_NAMES} dump is required."
            )
        known = [variant for variant in catalog["variants"]
                 if variant["game_code"] == info["game_code"] and variant["revision"] == info["revision"]]
        if not known:
            raise ValueError(f"{bundle.describe(info)} is not supported yet.")
        raise ValueError(
            f"Unrecognized {bundle.describe(info)} dump: size={size}, SHA-256={sha256}. "
            "It is modified, encrypted, damaged or not full size."
        )
    variant = matches[0]
    if not variant["pristine_verified"] or variant["planned_patch"] is None:
        raise ValueError(f"Recognized {title(variant)}, but it is not supported yet.")
    return variant


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    try:
        catalog = json.loads(CATALOG.read_text())
        if catalog.get("schema_version") != SCHEMA_VERSION:
            raise ValueError(f"{CATALOG} has an unsupported schema version")
        variant = identify(args.rom, catalog)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps({
        "variant": variant["id"],
        "game": variant["game"],
        "game_code": variant["game_code"],
        "sha256": variant["sha256"],
        "planned_patch": variant["planned_patch"],
    }, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
