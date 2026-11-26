#!/usr/bin/env python3
"""Build UTF-8 companion tables and the OFL-1.1 DSWT Companion Pixel subset.

No normalization or transliteration: table bytes match the two JSON catalogues.
Named {placeholders} are literal tokens for callers to replace before rendering.
The generated header documents bitmap packing and metrics for UI consumers.
Only Python's standard library is needed; full upstream font data stays in game/.
"""
import argparse
import base64
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
LANGUAGES = ('en_US', 'fr_FR', 'de_DE', 'it_IT', 'es_ES', 'ja_JP', 'ko_KR')
# Release pinned in LUNE-864; Condensed source Git blob cf954a44a98aa50a679d4af5b2ee5efed5ac89d0.
FONT_REVISION = 'bdb86ae89466a361eb8df861222736b81ff975ef'
FONT_URL = f'https://raw.githubusercontent.com/quiple/galmuri/{FONT_REVISION}/src/Galmuri11-Condensed.kbitx'
FONT_SHA256 = '7718e0faef8a9593e8f94307f57c3d77bfeaaeed08bdd381bd2b23a2d85e09b3'
PLACEHOLDER = re.compile(r'\{[a-z][a-z0-9_]*\}')


def load_messages(paths):
    messages, ids = [], set()
    for path in paths:
        document = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(document, dict) or not isinstance(document.get('messages'), list) or not document['messages']:
            raise ValueError(f'{path}: expected a nonempty messages list')
        for row in document['messages']:
            if not isinstance(row, dict) or not isinstance(row.get('id'), str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', row['id']):
                raise ValueError(f'{path}: invalid string id')
            name = row['id']
            if name in ids:
                raise ValueError(f'{path}: duplicate string id {name}')
            ids.add(name)
            placeholders = None
            for language in LANGUAGES:
                value = row.get(language)
                if not isinstance(value, str) or not value.strip():
                    raise ValueError(f'{name}: missing or empty translation for {language}')
                if any((ord(c) < 32 and c != '\n') or 127 <= ord(c) < 160 for c in value):
                    raise ValueError(f'{name}: unsupported control character in {language}')
                remainder = PLACEHOLDER.sub('', value)
                tokens = Counter(PLACEHOLDER.findall(value))
                if '{' in remainder or '}' in remainder or (placeholders is not None and tokens != placeholders):
                    raise ValueError(f'{name}: placeholders differ or are malformed in {language}')
                placeholders = tokens
            messages.append(row)
    return messages


def font_source(cache):
    source = cache if cache.exists() else FONT_URL
    if cache.exists():
        data = cache.read_bytes()
    else:
        with urllib.request.urlopen(FONT_URL, timeout=60) as response:
            data = response.read(2 * 1024 * 1024 + 1)
    if hashlib.sha256(data).hexdigest() != FONT_SHA256:
        cache.unlink(missing_ok=True)
        raise ValueError(f'font checksum mismatch: {source} rejected; expected SHA-256 {FONT_SHA256}. '
                         f'Delete {cache} if present and rerun to re-fetch.')
    write_if_changed(cache, data)
    return data


def decode_glyph(attributes):
    """Decode KBITX's base64, LEB128 dimensions and white/ink/byte runs."""
    data = iter(base64.b64decode(attributes['d'] + '==='))

    def dimension():
        value = 0
        for shift in range(0, 21, 7):
            byte = next(data)
            value |= (byte & 127) << shift
            if byte < 128:
                return value
        raise ValueError('invalid bitmap dimension')

    try:
        height, width = dimension(), dimension()
        if width > 64 or height > 64:
            raise ValueError('bitmap exceeds glyph bounds')
        pixels = []
        for command in data:
            count = command & 31
            if command & 32:
                count <<= 5
            kind = command >> 6
            if kind == 0:
                pixels.extend([0] * count)
            elif kind == 1:
                pixels.extend([255] * count)
            elif kind == 2:
                pixels.extend([next(data)] * count)
            else:
                pixels.extend(next(data) for _ in range(count))
            if len(pixels) > width * height:
                raise ValueError('bitmap run exceeds dimensions')
        if len(pixels) != width * height or any(p not in (0, 255) for p in pixels):
            raise ValueError('bitmap must contain exactly width * height monochrome pixels')
    except (StopIteration, RuntimeError) as error:
        raise ValueError('truncated bitmap') from error
    stride = (width + 7) // 8
    packed = bytearray(stride * height)
    for index, pixel in enumerate(pixels):
        if pixel:
            y, x = divmod(index, width)
            packed[y * stride + x // 8] |= 128 >> (x % 8)
    x, y, advance = (int(attributes[key]) for key in ('x', 'y', 'w'))
    if not (-128 <= x <= 127 and -128 <= y <= 127 and 0 <= advance <= 255):
        raise ValueError('bitmap metrics exceed C field bounds')
    return x, y, advance, width, height, bytes(packed)


def write_if_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        temporary = path.with_suffix(path.suffix + '.new')
        temporary.write_bytes(data)
        temporary.replace(path)


def emit(output, messages, glyphs, ascent, line_height):
    header = '''/* UTF-8 strings, indexed by text id and language. Replace named {tokens} before drawing.
 * DSWT Companion Pixel: OFL-1.1 subset of Galmuri11 Condensed v2.40.4.
 * Copyright 2019-2025 Lee Minseo. See dswt-companion-pixel.ofl.txt.
 */
#ifndef DSWT_COMPANION_TEXT_H
#define DSWT_COMPANION_TEXT_H
#include <stdint.h>

typedef enum {
'''
    header += ''.join(f'    Companion_Language_{language},\n' for language in LANGUAGES)
    header += '    Companion_Language_Count\n} CompanionLanguage;\n\ntypedef enum {\n'
    header += ''.join(f'    {row["id"]},\n' for row in messages)
    header += '''    Companion_Text_Count
} CompanionTextId;

/* Codepoints and glyphs share a sorted index (binary search by Unicode scalar).
 * Bitmap offset is in bytes; each row is ceil(width/8) bytes, MSB is leftmost.
 * Set pixels are ink. Draw at pen_x + x, baseline_y - y; advance pen by advance.
 * Newline has no glyph: reset pen_x and advance baseline by FontLineHeight.
 * Other absent codepoints are unsupported; callers must handle dynamic text.
 */
typedef struct {
    uint32_t offset;
    int8_t x, y;
    uint8_t advance, width, height;
} CompanionFontGlyph;
extern const char *const Companion_Text[Companion_Text_Count][Companion_Language_Count];
extern const uint32_t Companion_FontCodepoints[];
extern const CompanionFontGlyph Companion_FontGlyphs[];
extern const uint8_t Companion_FontBitmap[];
extern const uint32_t Companion_FontGlyphCount, Companion_FontBitmapSize;
extern const uint8_t Companion_FontAscent, Companion_FontLineHeight;
#endif
'''
    source = '#include "companion_text.h"\n\n'
    source += 'const char *const Companion_Text[Companion_Text_Count][Companion_Language_Count] = {\n'
    for row in messages:
        source += f'    [{row["id"]}] = {{\n'
        for language in LANGUAGES:
            literal = ''.join(f'\\{byte:03o}' for byte in row[language].encode('utf-8'))
            source += f'        "{literal}",\n'
        source += '    },\n'
    source += '};\n\nconst uint32_t Companion_FontCodepoints[] = {\n'
    source += ''.join(f'    0x{cp:04x},\n' for cp in glyphs) + '};\n'
    source += 'const CompanionFontGlyph Companion_FontGlyphs[] = {\n'
    bitmap = bytearray()
    for x, y, advance, width, height, data in glyphs.values():
        source += f'    {{{len(bitmap)}, {x}, {y}, {advance}, {width}, {height}}},\n'
        bitmap.extend(data)
    source += '};\nconst uint8_t Companion_FontBitmap[] = {\n'
    for start in range(0, len(bitmap), 16):
        source += '    ' + ', '.join(f'0x{b:02x}' for b in bitmap[start:start + 16]) + ',\n'
    source += '};\n'
    source += f'const uint32_t Companion_FontGlyphCount = {len(glyphs)}, Companion_FontBitmapSize = {len(bitmap)};\n'
    source += f'const uint8_t Companion_FontAscent = {ascent}, Companion_FontLineHeight = {line_height};\n'
    manifest = {
        'name': 'DSWT Companion Pixel', 'license': 'OFL-1.1',
        'copyright': 'Copyright 2019-2025 Lee Minseo',
        'source_face': 'Galmuri11 Condensed v2.40.4', 'source_revision': FONT_REVISION,
        'source_url': FONT_URL, 'source_sha256': FONT_SHA256,
        'changes': 'Subset to catalogue codepoints plus printable ASCII; monochrome row-packed MSB-first bitmap conversion; original bearings and advances retained.',
        'encoding': 'UTF-8, literal named {tokens}; newline is layout only',
        'codepoints': list(glyphs), 'bitmap_bytes': len(bitmap),
    }
    for name, data in (
        ('companion_text.h', header.encode()), ('companion_text.c', source.encode()),
        ('font-manifest.json', (json.dumps(manifest, indent=2) + '\n').encode()),
        ('dswt-companion-pixel.ofl.txt', (ROOT / 'licensing/ds-companion-galmuri.txt').read_bytes()),
    ):
        write_if_changed(output / name, data)


def generate(paths, cache, output):
    messages = load_messages(paths)
    font = ET.fromstring(font_source(cache))
    points = set(range(32, 127))
    for row in messages:
        for language in LANGUAGES:
            points.update(ord(c) for c in row[language] if c != '\n')
    available = {int(g.attrib['u']): g.attrib for g in font.findall('g') if 'u' in g.attrib}
    missing = sorted(points - available.keys())
    if missing:
        raise ValueError('font missing glyph(s): ' + ', '.join(f'U+{cp:04X}' for cp in missing))
    glyphs = {cp: decode_glyph(available[cp]) for cp in sorted(points)}
    properties = {p.attrib['id']: int(p.attrib['value']) for p in font.findall('prop')}
    ascent = properties['lineAscent']
    line_height = ascent + properties['lineDescent'] + properties.get('lineGap', 0)
    emit(output, messages, glyphs, ascent, line_height)
    return messages, glyphs, ascent, line_height


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wonder-trade', type=Path, default=ROOT / 'client/games/res/text/wonder_trade.json')
    parser.add_argument('--companion', type=Path, default=ROOT / 'client/companion/res/text/companion.json')
    parser.add_argument('--font-cache', type=Path, default=ROOT / 'game/companion/font/Galmuri11-Condensed.kbitx')
    parser.add_argument('--output', type=Path, default=ROOT / 'game/companion/build/text')
    args = parser.parse_args()
    try:
        result = generate([args.wonder_trade, args.companion], args.font_cache, args.output)
        print(f'Companion text: {len(result[0])} strings x {len(LANGUAGES)} languages, {len(result[1])} glyphs -> {args.output}')
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        print(f'Cannot generate companion text: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
