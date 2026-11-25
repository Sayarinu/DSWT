"""Test native tool installation with local archives and fake Docker/Cargo tools."""

import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_rom_macos', ROOT / 'scripts/prepare-rom-macos.py')
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)


class PrepareRomMacosTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for path in ('tools/rom-builder-dp/Dockerfile', 'tools/rom-builder-bw2/ds-rom-stale-secure-area.patch'):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / path, target)
        self.destination = self.root / 'game/rom-macos/assets'
        self.destination.parent.mkdir(parents=True)

    def test_sources_match_the_docker_build(self):
        dockerfile = (ROOT / 'tools/rom-builder-bw2/Dockerfile').read_text()
        for url, digest, _ in native.SOURCES:
            self.assertIn(f'ADD --checksum=sha256:{digest} {url} ', dockerfile)

    def test_assets_are_copied_from_pinned_image_once_and_container_is_removed(self):
        def run(*args, **kwargs):
            if args[:2] == ('docker', 'cp'):
                self.assertEqual(args[2], 'container-id:/opt/pokediamond')
                Path(args[3]).mkdir()
        with patch.object(native, 'run', side_effect=run) as commands, \
                patch.object(native.subprocess, 'check_output', return_value='container-id\n'):
            native.prepare_assets(self.root, self.destination)
            calls = [call.args for call in commands.call_args_list]
            self.assertEqual(calls[0][:4], ('docker', 'build', '--platform', 'linux/amd64'))
            self.assertEqual(calls[-1], ('docker', 'rm', 'container-id'))
            commands.reset_mock()
            native.prepare_assets(self.root, self.destination)
            commands.assert_not_called()
            (self.root / 'tools/rom-builder-dp/Dockerfile').write_text('changed pin')
            with self.assertRaisesRegex(ValueError, 'move .* aside'):
                native.prepare_assets(self.root, self.destination)
            commands.assert_not_called()

    def test_failed_asset_copy_cleans_container_and_leaves_no_installation(self):
        def run(*args, **kwargs):
            if args[:2] == ('docker', 'cp'):
                raise subprocess.CalledProcessError(9, args)
        with patch.object(native, 'run', side_effect=run) as commands, \
                patch.object(native.subprocess, 'check_output', return_value='container-id\n'):
            with self.assertRaises(subprocess.CalledProcessError):
                native.prepare_assets(self.root, self.destination)
            self.assertEqual(commands.call_args.args, ('docker', 'rm', 'container-id'))
        self.assertFalse(self.destination.exists())

    def source_archives(self):
        archives, pins = {}, []
        for index, (url, _, directory) in enumerate(native.SOURCES):
            output = io.BytesIO()
            with tarfile.open(fileobj=output, mode='w:gz') as archive:
                body = (b'ds-rom = { git = "https://github.com/fuddlesworth/ds-rom", '
                        b'rev = "66e83ef467b1e7a4816e667bf3b86572b1f8072d" }\n')
                info = tarfile.TarInfo(directory + '/Cargo.toml')
                info.size = len(body)
                archive.addfile(info, io.BytesIO(body))
            archives[url] = output.getvalue()
            pins.append((url, hashlib.sha256(archives[url]).hexdigest(), directory))
        return archives, tuple(pins)

    def test_dsd_verifies_sources_applies_patch_builds_and_reuses_native_binary(self):
        self.destination.mkdir()
        archives, pins = self.source_archives()
        def run(*args, **kwargs):
            if args[0] == 'cargo':
                manifest = kwargs['cwd'] / 'Cargo.toml'
                self.assertIn('ds-rom = { path = "../ds-rom/lib" }', manifest.read_text())
                binary = kwargs['cwd'] / 'target/release/dsd'
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b'native executable')
                binary.chmod(0o755)
        with patch.object(native, 'SOURCES', pins), \
                patch.object(native.urllib.request, 'urlopen', side_effect=lambda url, **kw: io.BytesIO(archives[url])) as download, \
                patch.object(native, 'run', side_effect=run) as commands:
            native.prepare_dsd(self.root, self.destination)
            self.assertEqual(download.call_count, 2)
            self.assertEqual(commands.call_args_list[0].args[:3], ('patch', '-p1', '-i'))
            self.assertEqual(commands.call_args_list[1].args, ('cargo', '+1.100', 'build', '--release', '--bin', 'dsd'))
            self.assertEqual((self.destination / 'dsd').read_bytes(), b'native executable')
            self.assertTrue((self.destination / 'dsd').stat().st_mode & 0o111)
            commands.reset_mock()
            download.reset_mock()
            native.prepare_dsd(self.root, self.destination)
            commands.assert_not_called()
            download.assert_not_called()

    def test_bad_source_checksum_stops_before_extraction_or_compilation(self):
        self.destination.mkdir()
        with patch.object(native.urllib.request, 'urlopen', return_value=io.BytesIO(b'bad archive')), \
                patch.object(native, 'run') as commands:
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                native.prepare_dsd(self.root, self.destination)
            commands.assert_not_called()
        self.assertFalse((self.destination / 'dsd').exists())

    def test_only_modified_bw2_requires_a_native_dsd(self):
        with patch.object(native, 'prepare_assets') as assets, patch.object(native, 'prepare_dsd') as dsd:
            for game in ('diamond', 'pearl', 'black2', 'white2'):
                for mode in ('pristine', 'modified'):
                    dsd.reset_mock()
                    native.prepare(self.root, game, mode)
                    self.assertEqual(dsd.call_count, int(game in ('black2', 'white2') and mode == 'modified'))
            self.assertEqual(assets.call_count, 8)


if __name__ == '__main__':
    unittest.main()
