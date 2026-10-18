#!/usr/bin/env python3
"""Install the checksum-pinned upstream TLS archive only in the ignored game directory."""

import hashlib
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import urllib.request


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            if not path.resolve().is_relative_to(directory.resolve()):
                raise ValueError("TLS source symlink escapes its installation")
            result[path.relative_to(directory).as_posix()] = "link:" + str(path.readlink())
        elif not (path.is_dir() or path.is_file()):
            raise ValueError("TLS source contains a special file")
        elif path.is_file() and path != directory / ".platinumwt-files.json":
            result[path.relative_to(directory).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def prepare(root):
    pin = json.loads((root / "client/tls-dependency.json").read_text())
    parent = root / "game"
    if parent.is_symlink():
        raise ValueError("TLS cache must not be a symlink")
    parent.mkdir(exist_ok=True)
    destination = parent / f"mbedtls-{pin['version']}"
    if destination.exists() or destination.is_symlink():
        manifest = destination / ".platinumwt-files.json"
        if destination.is_symlink() or manifest.is_symlink() or not manifest.is_file():
            raise ValueError("Existing TLS source is unmanaged; keep it aside")
        recorded = json.loads(manifest.read_text())
        if recorded.get("archive_sha256") != pin["sha256"] or recorded.get("files") != inventory(destination):
            raise ValueError("Existing TLS source differs from its pinned installation; keep local edits aside")
        return destination
    with tempfile.TemporaryDirectory(prefix=".tls-", dir=parent) as temp:
        stage = Path(temp)
        archive = stage / "source.tar.bz2"
        with urllib.request.urlopen(pin["url"], timeout=60) as response, archive.open("xb") as output:
            count = 0
            while block := response.read(1024 * 1024):
                count += len(block)
                if count > 32 * 1024 * 1024:
                    raise ValueError("TLS archive exceeds download limit")
                output.write(block)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != pin["sha256"]:
            raise ValueError("TLS archive checksum mismatch")
        with tarfile.open(archive) as source:
            source.extractall(stage, filter="data")
        extracted = stage / destination.name
        files = inventory(extracted)
        (extracted / ".platinumwt-files.json").write_text(json.dumps({"archive_sha256": pin["sha256"], "files": files}) + "\n")
        extracted.rename(destination)
    return destination


if __name__ == "__main__":
    try:
        print(prepare(Path(__file__).resolve().parent.parent))
    except (OSError, ValueError, tarfile.TarError) as error:
        print(f"Cannot prepare TLS source: {error}", file=sys.stderr)
        sys.exit(1)
