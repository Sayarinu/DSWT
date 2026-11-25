#include "game/text.h"

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

int CpGame_SpeciesName(const CpGame *game, uint16_t species, uint16_t *out, size_t capacity, size_t *length)
{
    CpRomFile bank;
    int result = CpNarc_Member(&game->messages, CpGame_Bank(game, &game->table->speciesNames), &bank);

    return result != CP_OK ? result : CpMessage_Read(&bank, species, out, capacity, length);
}

/* NFGR fonts: a header {size, width table offset, glyph count, max width,
 * max height, tiles wide, tiles high}, the glyphs after it and one width byte
 * per glyph. Glyph n draws character code n + 1. */
int CpGame_Font(const CpGame *game, unsigned font, CpFont *out)
{
    uint8_t header[16];
    int result = CpNarc_Member(&game->font, font, &out->file);

    if (result != CP_OK) return result;
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
