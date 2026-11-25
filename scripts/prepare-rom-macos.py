#!/usr/bin/env python3
"""Prepare native macOS ROM tools in game/, using the Docker build's pinned inputs."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


# Keep these source archives identical to tools/rom-builder-bw2/Dockerfile.
SOURCES = (
    ("https://codeload.github.com/fuddlesworth/ds-decomp/tar.gz/refs/tags/v0.12.1-dsi.10",
     "41dfddf8ee8ae9153459403ae522db2993724e675c41768132e1a93bd65db3e3", "ds-decomp-0.12.1-dsi.10"),
    ("https://codeload.github.com/fuddlesworth/ds-rom/tar.gz/66e83ef467b1e7a4816e667bf3b86572b1f8072d",
     "148bd6cc6628fa36ec5e4420a59b9f7276f39a3c35a9f16f3bf489331fd45b49", "ds-rom-66e83ef467b1e7a4816e667bf3b86572b1f8072d"),
)


def run(*args, **kwargs):
    subprocess.run(args, check=True, stdout=sys.stderr, **kwargs)


def prepare_assets(root, destination):
    builder = root / "tools/rom-builder-dp"
    stamp = hashlib.sha256((builder / "Dockerfile").read_bytes()).hexdigest()
    marker = destination / ".assets-version"
    if marker.is_file() and marker.read_text() == stamp:
        return
    if destination.exists():
        raise ValueError(f"Unmanaged or outdated native tools; move {destination} aside and retry")
    run("docker", "build", "--platform", "linux/amd64", "-f", str(builder / "Dockerfile"),
        "-t", "dswt-rom-builder-dp", str(builder))
    container = subprocess.check_output([
        "docker", "create", "--platform", "linux/amd64", "dswt-rom-builder-dp", "true"], text=True).strip()
    try:
        with tempfile.TemporaryDirectory(prefix=".mac-assets-", dir=destination.parent) as temporary:
            stage = Path(temporary) / "assets"
            run("docker", "cp", f"{container}:/opt/pokediamond", str(stage))
            (stage / ".assets-version").write_text(stamp)
            stage.rename(destination)
    finally:
        run("docker", "rm", container)


def prepare_dsd(root, destination):
    patch = root / "tools/rom-builder-bw2/ds-rom-stale-secure-area.patch"
    stamp = hashlib.sha256(patch.read_bytes() + repr(SOURCES).encode()).hexdigest()
    marker = destination / ".dsd-version"
    if (destination / "dsd").is_file() and marker.is_file() and marker.read_text() == stamp:
        return
    with tempfile.TemporaryDirectory(prefix=".mac-dsd-", dir=destination.parent) as temporary:
        stage = Path(temporary)
        for url, digest, directory in SOURCES:
            archive = stage / "source.tar.gz"
            with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
                raise ValueError(f"Native dsd source checksum mismatch: {url}")
            with tarfile.open(archive) as source:
                source.extractall(stage, filter="data")
            (stage / directory).rename(stage / ("ds-decomp" if "ds-decomp" in directory else "ds-rom"))
        run("patch", "-p1", "-i", str(patch), cwd=stage / "ds-rom")
        manifest = stage / "ds-decomp/Cargo.toml"
        original = 'ds-rom = { git = "https://github.com/fuddlesworth/ds-rom", rev = "66e83ef467b1e7a4816e667bf3b86572b1f8072d" }'
        text = manifest.read_text()
        if text.count(original) != 1:
            raise ValueError("Native dsd source no longer names the pinned ds-rom dependency")
        manifest.write_text(text.replace(original, 'ds-rom = { path = "../ds-rom/lib" }'))
        run("cargo", "+1.100", "build", "--release", "--bin", "dsd", cwd=manifest.parent)
        shutil.copy2(stage / "ds-decomp/target/release/dsd", destination / "dsd")
        marker.write_text(stamp)


def prepare(root, game, mode):
    parent = root / "game/rom-macos"
    parent.mkdir(parents=True, exist_ok=True)
    destination = parent / "assets"
    prepare_assets(root, destination)
    if game in ("black2", "white2") and mode == "modified":
        prepare_dsd(root, destination)
    return destination


if __name__ == "__main__":
    try:
        print(prepare(Path(__file__).resolve().parents[1], *sys.argv[1:]))
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        sys.exit(f"Cannot prepare native macOS tools: {error}")
