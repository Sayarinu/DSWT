#include "game/text.h"

#include <string.h>

/* Message banks: a count and seed, then an {offset, length} pair per entry,
 * each XORed with a key from the seed; the text is XORed with a running key
 * from the entry number. Offsets are from the bank start, lengths in codes. */
int CpMessage_Read(const CpRomFile *bank, uint16_t entry, uint16_t *out, size_t capacity, size_t *length)
{
    uint8_t header[4], pair[8], raw[2];
    uint32_t key, offset, count, i;
    uint16_t textKey;

    if (capacity == 0) return CP_ERR_RANGE;
    if (CpRomFile_Read(bank, 0, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    if (entry >= CpLe16(header)) return CP_ERR_NOT_FOUND;
    if (CpRomFile_Read(bank, 4 + (uint32_t)entry * 8, pair, sizeof pair) != CP_OK) return CP_ERR_FORMAT;
    key = (uint32_t)CpLe16(header + 2) * 765u * (entry + 1u) & 0xFFFF;
    key |= key << 16;
    offset = CpLe32(pair) ^ key;
    count = CpLe32(pair + 4) ^ key;
    textKey = (uint16_t)((entry + 1u) * 596947u);
    for (i = 0; i < count; ++i) {
        uint16_t code;

        if (CpRomFile_Read(bank, offset + i * 2, raw, 2) != CP_OK) return CP_ERR_FORMAT;
        code = CpLe16(raw) ^ textKey;
        textKey = (uint16_t)(textKey + 18749u);
        if (code == CP_CHAR_EOS) break;
        if (i + 1 >= capacity) return CP_ERR_RANGE;
        out[i] = code;
    }
    out[i] = CP_CHAR_EOS;
    if (length) *length = i;
    return CP_OK;
}

/* Generation V banks: {block count, entry count, largest block, 0}, the
 * blocks' offsets, then in each block its size and an {offset from the block,
 * length in codes, 0} per entry. Entry n's codes are XORed with a key that
 * starts at (n + 3) * 0x2983 and turns left by 3 bits after each code. */
int CpMessage_Read5(const CpRomFile *bank, uint16_t entry, uint16_t *out, size_t capacity, size_t *length)
{
    uint8_t header[16], pair[8], raw[2];
    uint32_t block, offset, count, i;
    uint16_t key;

    if (capacity == 0) return CP_ERR_RANGE;
    if (CpRomFile_Read(bank, 0, header, sizeof header) != CP_OK || CpLe16(header) == 0) return CP_ERR_FORMAT;
    if (entry >= CpLe16(header + 2)) return CP_ERR_NOT_FOUND;
    block = CpLe32(header + 12);
    if (CpRomFile_Read(bank, block + 4 + (uint32_t)entry * 8, pair, sizeof pair) != CP_OK) return CP_ERR_FORMAT;
    offset = block + CpLe32(pair);
    count = CpLe16(pair + 4);
    key = (uint16_t)((entry + 3u) * 0x2983u);
    for (i = 0; i < count; ++i) {
        uint16_t code;

        if (CpRomFile_Read(bank, offset + i * 2, raw, 2) != CP_OK) return CP_ERR_FORMAT;
        code = (uint16_t)(CpLe16(raw) ^ key);
        key = (uint16_t)(key << 3 | key >> 13);
        if (code == CP_CHAR_EOS) break;
        if (i + 1 >= capacity) return CP_ERR_RANGE;
        out[i] = code;
    }
    out[i] = CP_CHAR_EOS;
    if (length) *length = i;
    return CP_OK;
}

int CpGame_Message(const CpGame *game, uint16_t number, uint16_t entry, uint16_t *out, size_t capacity, size_t *length)
{
    CpRomFile bank;
    int result = CpNarc_Member(&game->messages, number, &bank);

    if (result != CP_OK) return result;
    return game->table->gen5PersonalSize ? CpMessage_Read5(&bank, entry, out, capacity, length)
                                         : CpMessage_Read(&bank, entry, out, capacity, length);
}

int CpGame_SpeciesName(const CpGame *game, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    return CpGame_Message(game, CpGame_Bank(game, &game->table->speciesNames), species, out, capacity, length);
}

int CpGame_ItemName(const CpGame *game, uint16_t item, uint16_t *out, size_t capacity, size_t *length)
{
    return CpGame_Message(game, CpGame_Bank(game, &game->table->itemNames), item, out, capacity, length);
}

/* NFTR: the header at 0x18 {type, line feed, alternate glyph, default left,
 * width and advance, encoding, glyph, width and map offsets}; the offsets
 * are past each block's 8-byte header. The glyph block starts {cell width,
 * cell height, cell size, ...}. */
static int Nftr(CpFont *out)
{
    uint8_t info[20], cells[4];

    if (CpRomFile_Read(&out->file, 0x18, info, sizeof info) != CP_OK) return CP_ERR_FORMAT;
    out->alternate = CpLe16(info + 2);
    out->headerSize = CpLe32(info + 8) + 8;
    out->widthOffset = CpLe32(info + 16);
    if (CpRomFile_Read(&out->file, CpLe32(info + 8), cells, sizeof cells) != CP_OK) return CP_ERR_FORMAT;
    out->cellWidth = cells[0];
    out->cellHeight = cells[1];
    out->cellSize = CpLe16(cells + 2);
    out->maxWidth = cells[0];
    out->maxHeight = info[1];
    out->tilesWide = (uint8_t)((cells[0] + 7) / 8);
    out->tilesHigh = (uint8_t)((cells[1] + 7) / 8);
    out->glyphCount = (out->file.size - out->headerSize) / (out->cellSize ? out->cellSize : 1);
    out->nftr = 1;
    if (out->tilesWide > 2 || out->tilesHigh > 2 || out->cellSize < 3
        || out->cellSize < 3u + ((unsigned)cells[0] * cells[1] * 2 + 7) / 8) return CP_ERR_FORMAT;
    return CP_OK;
}

/* The glyph of a code through the NFTR maps: direct (0), a table (1) or
 * sorted pairs (2). */
static int NftrIndex(const CpFont *font, uint16_t code, uint16_t *glyph)
{
    uint32_t map = font->widthOffset;
    unsigned guard;
    for (guard = 0; guard < 64 && map && map + 12 <= font->file.size; ++guard) {
        uint8_t head[12], entry[4];
        uint16_t first, last, method;
        if (CpRomFile_Read(&font->file, map, head, sizeof head) != CP_OK) return CP_ERR_FORMAT;
        first = CpLe16(head);
        last = CpLe16(head + 2);
        method = CpLe16(head + 4);
        if (code >= first && code <= last) {
            if (method == 0) {
                if (CpRomFile_Read(&font->file, map + 12, entry, 2) != CP_OK) return CP_ERR_FORMAT;
                *glyph = (uint16_t)(CpLe16(entry) + code - first);
                return CP_OK;
            }
            if (method == 1) {
                if (CpRomFile_Read(&font->file, map + 12 + 2u * (code - first), entry, 2) != CP_OK) return CP_ERR_FORMAT;
                if (CpLe16(entry) != 0xFFFF) { *glyph = CpLe16(entry); return CP_OK; }
            } else if (method == 2) {
                uint16_t count, low = 0, high;
                if (CpRomFile_Read(&font->file, map + 12, entry, 2) != CP_OK) return CP_ERR_FORMAT;
                high = count = CpLe16(entry);
                while (low < high) {
                    uint16_t middle = (uint16_t)((low + high) / 2), at;
                    if (CpRomFile_Read(&font->file, map + 14 + 4u * middle, entry, 4) != CP_OK) return CP_ERR_FORMAT;
                    at = CpLe16(entry);
                    if (at == code) { *glyph = CpLe16(entry + 2); return CP_OK; }
                    if (at < code) low = (uint16_t)(middle + 1); else high = middle;
                }
            }
        }
        map = CpLe32(head + 8);
    }
    return CP_ERR_NOT_FOUND;
}

static void SetPixel(CpGlyph *glyph, unsigned x, unsigned y, uint8_t value)
{
    uint8_t *tile = glyph->tiles[(y / 8) * glyph->tilesWide + x / 8];
    uint8_t *byte = &tile[(y % 8) * 2 + ((x % 8) < 4 ? 1 : 0)];
    *byte = (uint8_t)(*byte | (value & 3) << (6 - (x % 4) * 2));
}

static int NftrGlyph(const CpFont *font, uint16_t code, CpGlyph *out)
{
    uint8_t cell[3 + (16 * 16 * 2) / 8];
    uint16_t index;
    unsigned x, y;

    if (NftrIndex(font, code, &index) != CP_OK) index = font->alternate;
    if (index >= font->glyphCount
        || CpRomFile_Read(&font->file, font->headerSize + (uint32_t)index * font->cellSize, cell,
               3u + ((unsigned)font->cellWidth * font->cellHeight * 2 + 7) / 8) != CP_OK) return CP_ERR_FORMAT;
    memset(out, 0, sizeof(*out));
    out->tilesWide = font->tilesWide;
    out->tilesHigh = font->tilesHigh;
    out->width = cell[2];
    out->height = font->maxHeight;
    for (y = 0; y < font->cellHeight; ++y)
        for (x = 0; x < font->cellWidth; ++x) {
            unsigned i = y * font->cellWidth + x, at = x + cell[0];
            uint8_t value = (uint8_t)(cell[3 + i / 4] >> (6 - 2 * (i % 4)) & 3);
            if (value && at < font->tilesWide * 8u) SetPixel(out, at, y, value);
        }
    return CP_OK;
}

/* NFGR fonts: a header {size, width table offset, glyph count, max width,
 * max height, tiles wide, tiles high}, the glyphs after it and one width byte
 * per glyph. Glyph n draws character code n + 1. */
int CpGame_Font(const CpGame *game, unsigned font, CpFont *out)
{
    uint8_t header[16];
    int result = CpNarc_Member(&game->font, font, &out->file);

    if (result != CP_OK) return result;
    out->nftr = 0;
    if (game->table->gen5PersonalSize) return Nftr(out);
    if (CpRomFile_Read(&out->file, 0, header, sizeof header) != CP_OK) return CP_ERR_FORMAT;
    out->headerSize = CpLe32(header);
    out->widthOffset = CpLe32(header + 4);
    out->glyphCount = CpLe32(header + 8);
    out->maxWidth = header[12];
    out->maxHeight = header[13];
    out->tilesWide = header[14];
    out->tilesHigh = header[15];
    if (out->tilesWide < 1 || out->tilesWide > 2 || out->tilesHigh < 1 || out->tilesHigh > 2 ||
        out->glyphCount == 0 || out->glyphCount > 0xFFFF || out->headerSize < sizeof header)
        return CP_ERR_FORMAT;
    return CP_OK;
}

int CpFont_Glyph(const CpFont *font, uint16_t code, CpGlyph *out)
{
    uint32_t size = 16u * font->tilesWide * font->tilesHigh;
    if (font->nftr) return NftrGlyph(font, code, out);
    uint32_t glyph = (code >= 1 && code <= font->glyphCount ? code : CP_CHAR_QUESTION) - 1u;
    uint8_t width = font->maxWidth;
    uint32_t tile;

    if (glyph >= font->glyphCount) return CP_ERR_NOT_FOUND;
    for (tile = 0; tile < (uint32_t)font->tilesWide * font->tilesHigh; ++tile)
        if (CpRomFile_Read(&font->file, font->headerSize + glyph * size + tile * 16, out->tiles[tile], 16) != CP_OK)
            return CP_ERR_FORMAT;
    if (font->widthOffset && CpRomFile_Read(&font->file, font->widthOffset + glyph, &width, 1) != CP_OK)
        return CP_ERR_FORMAT;
    out->width = width;
    out->height = font->maxHeight;
    out->tilesWide = font->tilesWide;
    out->tilesHigh = font->tilesHigh;
    return CP_OK;
}

/* Tiles run left to right, then top to bottom. Each tile row is a 16-bit
 * little-endian word: its high byte holds the left four pixels, its low byte
 * the right four, each from the top bits down. */
uint8_t CpGlyph_Pixel(const CpGlyph *glyph, unsigned x, unsigned y)
{
    const uint8_t *tile;
    uint8_t byte;

    if (x >= glyph->tilesWide * 8u || y >= glyph->tilesHigh * 8u) return 0;
    tile = glyph->tiles[(y / 8) * glyph->tilesWide + x / 8];
    byte = tile[(y % 8) * 2 + ((x % 8) < 4 ? 1 : 0)];
    return (uint8_t)(byte >> (6 - (x % 4) * 2) & 3);
}
