"""Validate companion translations, bitmap conversion and the generated C interface."""
import base64
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load_generator():
    spec = importlib.util.spec_from_file_location('companion_text', ROOT / 'scripts/companion-text.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CompanionTextTests(unittest.TestCase):
    def setUp(self):
        self.tool = load_generator()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.message = {'id': 'Test_Text_Hello', **dict.fromkeys(self.tool.LANGUAGES, 'A {game}')}

    def catalogue(self, message=None):
        path = self.directory / 'text.json'
        path.write_text(json.dumps({'messages': [message or self.message]}), encoding='utf-8')
        return path

    def test_every_real_message_has_seven_languages(self):
        messages = self.tool.load_messages([ROOT / 'client/res/text/wonder_trade.json',
                                           ROOT / 'companion/res/text/companion.json'])
        self.assertGreater(len(messages), 60)
        companion = json.loads((ROOT / 'companion/res/text/companion.json').read_text())
        self.assertIn('native review', companion['comment'])
        self.assertTrue(all(set(self.tool.LANGUAGES) <= row.keys() for row in messages))

    def test_rejects_missing_empty_or_non_string_translations(self):
        for value in (None, '', 4):
            row = copy.deepcopy(self.message)
            row['ko_KR'] = value
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'Test_Text_Hello.*ko_KR'):
                self.tool.load_messages([self.catalogue(row)])
        del row['ko_KR']
        with self.assertRaisesRegex(ValueError, 'ko_KR'):
            self.tool.load_messages([self.catalogue(row)])

    def test_placeholder_names_and_multiplicity_must_match(self):
        for value in ('A {name}', 'A {game} {game}', 'A {game', 'A {game!r}'):
            row = dict(self.message, fr_FR=value)
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'placeholder.*fr_FR'):
                self.tool.load_messages([self.catalogue(row)])
        row = {k: v.replace('A {game}', '{game} A') for k, v in self.message.items()}
        self.tool.load_messages([self.catalogue(row)])

    def test_duplicate_and_invalid_ids_fail(self):
        path = self.catalogue()
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.tool.load_messages([path, path])
        with self.assertRaisesRegex(ValueError, 'id'):
            self.tool.load_messages([self.catalogue(dict(self.message, id='bad-id'))])

    def test_only_newline_is_a_nonprinting_control(self):
        for value in ('A\0', 'A\t', 'A\r', 'A\x7f'):
            row = dict(self.message, en_US=value)
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'control'):
                self.tool.load_messages([self.catalogue(row)])

    def test_decode_bitmap_preserves_pixels_and_metrics(self):
        # 2x3, white x1, black x2, literal [white, black, white].
        encoded = base64.b64encode(bytes([2, 3, 1, 0x42, 0xC3, 0, 255, 0])).decode()
        glyph = self.tool.decode_glyph({'d': encoded, 'x': '-1', 'y': '2', 'w': '4'})
        self.assertEqual(glyph, (-1, 2, 4, 3, 2, b'`@'))
        for raw in (b'', bytes([2, 3, 0x47]), bytes([2, 3, 0x41])):
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'bitmap'):
                self.tool.decode_glyph({'d': base64.b64encode(raw).decode(), 'x': '0', 'y': '0', 'w': '4'})

    def test_checksum_is_checked_on_download_and_cached_source(self):
        cache = self.directory / 'font.kbitx'
        cache.write_bytes(b'bad font')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            self.tool.font_source(cache)
        cache.unlink()
        with patch.object(self.tool.urllib.request, 'urlopen') as download:
            download.return_value.__enter__.return_value.read.return_value = b'bad font'
            with self.assertRaisesRegex(ValueError, 'checksum'):
                self.tool.font_source(cache)
        self.assertFalse(cache.exists())

    def test_missing_glyph_is_a_clear_build_failure(self):
        row = dict.fromkeys(self.tool.LANGUAGES, 'A')
        row['id'] = 'Test_Text_Hello'
        font = b'<kbits><prop id="lineAscent" value="14"/><prop id="lineDescent" value="2"/></kbits>'
        with patch.object(self.tool, 'font_source', return_value=font):
            with self.assertRaisesRegex(ValueError, r'missing glyph.*U\+0020'):
                self.tool.generate([self.catalogue(row)], self.directory / 'font', self.directory / 'out')

    def test_cli_failure_is_nonzero_and_does_not_emit_tables(self):
        row = dict(self.message)
        del row['ja_JP']
        output = self.directory / 'output'
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/companion-text.py'),
                                 '--wonder-trade', str(self.catalogue(row)), '--companion', str(self.catalogue(row)),
                                 '--output', str(output)], text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cannot generate companion text', result.stderr)
        self.assertIn('ja_JP', result.stderr)
        self.assertFalse((output / 'companion_text.c').exists())

    def test_generate_selects_exactly_catalogue_glyphs_and_printable_ascii(self):
        row = {**dict.fromkeys(self.tool.LANGUAGES, 'A\n日한'), 'id': 'Test_Text_Hello'}
        points = set(range(32, 127)) | {ord(c) for c in '日한'}
        xml = '<kbits><prop id="lineAscent" value="14"/><prop id="lineDescent" value="2"/>'
        # An extra available glyph must not leak into the subset.
        xml += ''.join(f'<g u="{cp}" x="0" y="1" w="2" d="AQFB"/>' for cp in points | {0x2603})
        xml += '</kbits>'
        with patch.object(self.tool, 'font_source', return_value=xml.encode()):
            messages, glyphs, ascent, height = self.tool.generate(
                [self.catalogue(row)], self.directory / 'font', self.directory / 'out')
        self.assertEqual(list(glyphs), sorted(points))
        self.assertEqual((ascent, height), (14, 16))
        self.assertEqual(glyphs[65][-1], b'\x80')

    def test_bitmap_decoder_handles_repeated_bytes_long_runs_and_empty_space(self):
        for raw, expected in (([2, 2, 0x84, 255], b'\xc0\xc0'),
                              ([4, 8, 0x61], b'\xff' * 4), ([0, 0], b'')):
            encoded = base64.b64encode(bytes(raw)).decode()
            glyph = self.tool.decode_glyph({'d': encoded, 'x': '0', 'y': '0', 'w': '5'})
            self.assertEqual(glyph[-1], expected)
        with self.assertRaisesRegex(ValueError, 'monochrome'):
            self.tool.decode_glyph({'d': 'AQGBAQ', 'x': '0', 'y': '0', 'w': '5'})

    def test_generated_c_round_trips_utf8_and_font_with_exact_subset(self):
        # A synthetic, locally constructed font keeps the unit suite offline.
        messages = [{**dict.fromkeys(self.tool.LANGUAGES, 'A "\\\né日한'), 'id': 'Test_Text_Hello'}]
        points = sorted(set(range(32, 127)) | {ord(c) for c in 'é日한'})
        glyphs = {cp: (0, 1, 2, 1, 1, b'\x80') for cp in points}
        output = self.directory / 'output'
        self.tool.emit(output, messages, glyphs, 14, 16)
        consumer = self.directory / 'consumer.c'
        consumer.write_text('''#include "companion_text.h"
#include <stdio.h>
int main(void) {
    for (unsigned i = 0; i < Companion_Text_Count; ++i)
        for (unsigned j = 0; j < Companion_Language_Count; ++j) {
            const char *s = Companion_Text[i][j];
            do { putchar(*s); } while (*s++);
        }
    fwrite(Companion_FontCodepoints, sizeof(uint32_t), Companion_FontGlyphCount, stdout);
    fwrite(Companion_FontBitmap, 1, Companion_FontBitmapSize, stdout);
    return 0;
}
''')
        cc = shutil.which('cc')
        self.assertIsNotNone(cc, 'a C compiler is needed to verify emitted C')
        executable = self.directory / 'consumer'
        subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(output),
                        str(consumer), str(output / 'companion_text.c'), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, check=True)
        import struct
        expected = b''.join((messages[0][language] + '\0').encode() for language in self.tool.LANGUAGES)
        expected += b''.join(struct.pack('=I', cp) for cp in points) + b'\x80' * len(points)
        self.assertEqual(result.stdout, expected)
        self.assertEqual((output / 'dswt-companion-pixel.ofl.txt').read_bytes(),
                         (ROOT / 'licensing/ds-companion-galmuri.txt').read_bytes())
        manifest = json.loads((output / 'font-manifest.json').read_text())
        self.assertEqual(manifest['codepoints'], points)
        self.assertEqual(manifest['name'], 'DSWT Companion Pixel')
        before = {p.name: p.stat().st_mtime_ns for p in output.iterdir()}
        self.tool.emit(output, messages, glyphs, 14, 16)
        self.assertEqual(before, {p.name: p.stat().st_mtime_ns for p in output.iterdir()})


if __name__ == '__main__':
    unittest.main()
