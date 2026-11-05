#!/usr/bin/env python3
"""Prepare a pinned, locally patched melonDS source tree in the ignored game cache."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('prepare_tls', ROOT / 'scripts/prepare-tls.py')
tls_source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tls_source)


def prepare(root=ROOT, archive=None):
    pin = json.loads((root / 'emulator/upstream.json').read_text())
    patch = root / 'emulator/melonds.patch'
    additions = {
        'dswt-MIT-LICENSE': root / 'LICENSE',
        'src/frontend/qt_sdl/dswt/durable_save.cpp': root / 'emulator/durable_save.cpp',
        'src/frontend/qt_sdl/dswt/durable_save.h': root / 'emulator/durable_save.h',
        'src/DSWTPersistence.h': root / 'client/include/wonder_trade/persistence.h',
    }
    inputs = {'revision': pin['revision'], 'archive_sha256': pin['sha256'],
              'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
              'additions': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in additions.items()}}
    key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    parent = root / 'game'
    if parent.is_symlink():
        raise ValueError('Emulator cache must not be a symlink')
    parent.mkdir(exist_ok=True)
    destination = parent / f'melonds-{pin["version"]}-{key[:16]}'
    if destination.exists() or destination.is_symlink():
        manifest = destination / '.dswt-files.json'
        if destination.is_symlink() or manifest.is_symlink() or not manifest.is_file():
            raise ValueError('Existing emulator source is unmanaged; preserve it')
        recorded = json.loads(manifest.read_text())
        if recorded.get('inputs') != inputs or recorded.get('files') != tls_source.inventory(destination):
            raise ValueError('Existing emulator source has local changes; preserve them')
        return destination
    with tempfile.TemporaryDirectory(prefix='.emulator-', dir=parent) as temporary:
        stage = Path(temporary)
        downloaded = stage / 'source.tar.gz'
        if archive is None:
            with urllib.request.urlopen(pin['url'], timeout=60) as response, downloaded.open('xb') as output:
                count = 0
                while block := response.read(1024 * 1024):
                    count += len(block)
                    if count > 32 * 1024 * 1024:
                        raise ValueError('Emulator archive exceeds download limit')
                    output.write(block)
        else:
            if archive.stat().st_size > 32 * 1024 * 1024:
                raise ValueError('Emulator archive exceeds input limit')
            shutil.copyfile(archive, downloaded)
        if hashlib.sha256(downloaded.read_bytes()).hexdigest() != pin['sha256']:
            raise ValueError('Emulator archive checksum mismatch')
        with tarfile.open(downloaded) as source:
            if source.pax_headers.get('comment') != pin['revision']:
                raise ValueError('Emulator archive revision mismatch')
            source.extractall(stage, filter='data')
        extracted = stage / f'melonDS-{pin["version"]}'
        if not extracted.is_dir() or extracted.is_symlink():
            raise ValueError('Emulator source root mismatch')
        result = subprocess.run(['patch', '-p1', '--batch', '--forward', '--fuzz=0', '-i', str(patch.resolve())],
                                cwd=extracted, capture_output=True, text=True)
        if result.returncode:
            raise ValueError('Pinned emulator integration patch did not apply')
        for name, path in additions.items():
            target = extracted / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        files = tls_source.inventory(extracted)
        (extracted / '.dswt-files.json').write_text(json.dumps({'inputs': inputs, 'files': files}, sort_keys=True) + '\n')
        extracted.rename(destination)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Optional local copy of the exact pinned source archive')
    args = parser.parse_args()
    try:
        print(prepare(archive=args.archive))
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f'Cannot prepare emulator source: {error}\n')
