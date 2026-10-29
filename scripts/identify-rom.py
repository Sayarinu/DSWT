#!/usr/bin/env python3
"""Identify a verified pristine patch baseline without changing the input."""

import argparse
import hashlib
import json
from pathlib import Path
import sys


CATALOG = Path(__file__).resolve().parents[1] / "patches/compatibility.json"


def identify(path, catalog):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as rom:
        for chunk in iter(lambda: rom.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    return match(size, digest.hexdigest(), catalog)


def match(size, sha256, catalog):
    """Select the single verified pristine catalogue entry for an input's size and SHA-256."""
    matches = [
        variant for variant in catalog["variants"]
        if variant["size"] == size and variant["sha256"] == sha256
    ]
    if len(matches) != 1:
        raise ValueError(
            f"Unrecognized or ambiguous input: size={size}, SHA-256={sha256}. "
            "A full-size, unmodified decrypted English retail dump is required."
        )
    variant = matches[0]
    if not variant["pristine_verified"]:
        raise ValueError(
            f"Recognized {variant['name']}, but no verified compatible build exists yet."
        )
    return variant


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    try:
        catalog = json.loads(CATALOG.read_text())
        variant = identify(args.rom, catalog)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps({
        "variant": variant["id"],
        "sha256": variant["sha256"],
        "planned_patch": variant["planned_patch"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
