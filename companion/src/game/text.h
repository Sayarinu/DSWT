#ifndef COMPANION_GAME_TEXT_H
#define COMPANION_GAME_TEXT_H

#include "game/gamedata.h"

/* Game-encoded text: 16-bit character codes the game's fonts draw, ended by
 * CP_CHAR_EOS. The codes depend on the cartridge's language. */
#define CP_CHAR_EOS 0xFFFFu
#define CP_CHAR_QUESTION 0x01ACu

/* Entry `entry` of an encrypted message bank (an archive member). Writes at
 * most capacity - 1 codes and CP_CHAR_EOS; `length` (optional) gets the code
 * count before the end. CP_ERR_RANGE when the text does not fit. */
int CpMessage_Read(const CpRomFile *bank, uint16_t entry, uint16_t *out, size_t capacity, size_t *length);

/* A species name in the cartridge's language, from its message archive. */
int CpGame_SpeciesName(const CpGame *, uint16_t species, uint16_t *out, size_t capacity, size_t *length);

/* Fonts of the font archive. */
enum { CP_FONT_SYSTEM = 0, CP_FONT_MESSAGE = 1 };

typedef struct CpFont {
    CpRomFile file;
    uint32_t headerSize, widthOffset, glyphCount;
    uint8_t maxWidth, maxHeight, tilesWide, tilesHigh;
} CpFont;

int CpGame_Font(const CpGame *, unsigned font, CpFont *out);

/* One glyph: up to 2x2 8x8 tiles at 2 bits per pixel, as the game stores them. */
typedef struct CpGlyph {
    uint8_t width, height; /* the advance width and line height in pixels */
    uint8_t tilesWide, tilesHigh;
    uint8_t tiles[4][16];
} CpGlyph;

/* The glyph for a character code; codes past the font draw as '?', like the game. */
int CpFont_Glyph(const CpFont *, uint16_t code, CpGlyph *out);

/* A glyph pixel: 0 background, 1 foreground, 2 shadow, 3 the fill colour. */
uint8_t CpGlyph_Pixel(const CpGlyph *, unsigned x, unsigned y);

#endif
