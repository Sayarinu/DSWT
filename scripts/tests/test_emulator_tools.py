import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_emulator', ROOT / 'scripts/prepare-emulator.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class EmulatorSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ('emulator', 'client/include/wonder_trade'):
            (self.root / name).mkdir(parents=True)
        for name in ('emulator/durable_save.cpp', 'emulator/durable_save.h', 'client/include/wonder_trade/persistence.h', 'LICENSE'):
            (self.root / name).write_text('synthetic project input\n')
        (self.root / 'emulator/melonds.patch').write_text('--- a/file\n+++ b/file\n@@ -1 +1 @@\n-before\n+after\n')
        self.archive = self.root / 'source.tar.gz'
        self.make_archive('melonDS-1.1/file')

    def make_archive(self, name):
        with tarfile.open(self.archive, 'w:gz', format=tarfile.PAX_FORMAT, pax_headers={'comment': 'pinned-revision'}) as output:
            member = tarfile.TarInfo(name)
            member.size = 7
            output.addfile(member, io.BytesIO(b'before\n'))
        self.pin = dict(version='1.1', revision='pinned-revision', sha256=hashlib.sha256(self.archive.read_bytes()).hexdigest())
        self.write_pin()

    def write_pin(self):
        (self.root / 'emulator/upstream.json').write_text(json.dumps(self.pin))

    def test_pinned_patch_copies_and_reuses_only_verified_inputs(self):
        installed = prepare.prepare(self.root, self.archive)
        self.assertEqual((installed / 'file').read_text(), 'after\n')
        self.assertEqual((installed / 'dswt-MIT-LICENSE').read_text(), 'synthetic project input\n')
        self.assertEqual(prepare.prepare(self.root, self.archive), installed)

    def test_modified_cached_source_is_preserved_and_rejected(self):
        installed = prepare.prepare(self.root, self.archive)
        (installed / 'file').write_text('keep local work\n')
        with self.assertRaisesRegex(ValueError, 'local changes'):
            prepare.prepare(self.root, self.archive)
        self.assertEqual((installed / 'file').read_text(), 'keep local work\n')

    def test_changed_project_inputs_prepare_a_distinct_tree(self):
        first = prepare.prepare(self.root, self.archive)
        (self.root / 'emulator/durable_save.cpp').write_text('changed project input\n')
        second = prepare.prepare(self.root, self.archive)
        self.assertNotEqual(first, second)
        self.assertEqual((first / 'src/frontend/qt_sdl/dswt/durable_save.cpp').read_text(), 'synthetic project input\n')

    def test_archive_hash_revision_and_path_escape_are_refused(self):
        self.pin['sha256'] = '0' * 64
        self.write_pin()
        with self.assertRaisesRegex(ValueError, 'checksum'):
            prepare.prepare(self.root, self.archive)
        self.pin['sha256'] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.pin['revision'] = 'different-revision'
        self.write_pin()
        with self.assertRaisesRegex(ValueError, 'revision'):
            prepare.prepare(self.root, self.archive)
        self.make_archive('../../escape')
        with self.assertRaises(tarfile.TarError):
            prepare.prepare(self.root, self.archive)

    def test_a_symlink_cache_cannot_redirect_source_writes(self):
        elsewhere = self.root / 'elsewhere'
        elsewhere.mkdir()
        (self.root / 'game').symlink_to(elsewhere, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            prepare.prepare(self.root, self.archive)
        self.assertEqual(list(elsewhere.iterdir()), [])
