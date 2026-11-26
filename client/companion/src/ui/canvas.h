#ifndef COMPANION_UI_CANVAS_H
#define COMPANION_UI_CANVAS_H

#include <stddef.h>
#include <stdint.h>

#include "game/text.h"

/* One screen of 4-bit text pixels laid out as 8x8 background tiles: tile
 * (tx, ty) is number ty * 32 + tx, 32 bytes each, two pixels a byte with the
 * left one in the low nibble, as the DS draws 16-colour tiles. The front end
 * maps every cell of a background to its own tile, so drawing here is drawing
 * on the screen. Colour 0 is transparent. */
#define CP_CANVAS_WIDTH 256
#define CP_CANVAS_HEIGHT 192
#define CP_CANVAS_TILES ((CP_CANVAS_WIDTH / 8) * (CP_CANVAS_HEIGHT / 8))
#define CP_CANVAS_BYTES (CP_CANVAS_TILES * 32)

typedef struct CpCanvas {
    uint8_t *tiles; /* CP_CANVAS_BYTES */
    /* Text draws only inside this rectangle while clipWidth is nonzero. */
    int clipX, clipY, clipWidth, clipHeight;
} CpCanvas;

/* Two colours of text: the letters and their shadow (0 for none). */
typedef struct CpInk {
    uint8_t fg, shadow;
} CpInk;

void CpCanvas_Clear(CpCanvas *);
void CpCanvas_Fill(CpCanvas *, int x, int y, int width, int height, uint8_t colour);
void CpCanvas_Pixel(CpCanvas *, int x, int y, uint8_t colour);
uint8_t CpCanvas_Get(const CpCanvas *, int x, int y);

/* Keep text inside a rectangle (a window), or anywhere again. */
void CpCanvas_Clip(CpCanvas *, int x, int y, int width, int height);
void CpCanvas_Unclip(CpCanvas *);

/* Companion-font text (UTF-8, '\n' starts a line), drawn from the top-left
 * corner of its first line. Characters the font lacks draw as '?'. Returns the
 * width of the widest line. */
int CpCanvas_Text(CpCanvas *, int x, int y, const char *text, CpInk);
/* The same measurement without drawing. */
int CpText_Width(const char *text);
/* Copy `text` into `out` with line breaks so no line is wider than `width`
 * pixels: at spaces (or ideographic spaces) where it can, and between
 * characters for a word, such as a file name, wider than a line. Existing
 * line breaks stay. Returns the number of lines. */
int CpText_Wrap(char *out, size_t size, const char *text, int width);
/* Copy `text` into `out` replacing one {token} with `value`. */
void CpText_Replace(char *out, size_t size, const char *text, const char *token, const char *value);

/* Game-encoded text (CP_CHAR_EOS-terminated). With a font it draws the game's
 * own glyphs; without one it draws letters, digits and spaces with the
 * companion font and other codes as '?'. Returns the width drawn. */
int CpCanvas_GameText(CpCanvas *, int x, int y, const uint16_t *text, size_t capacity, const CpFont *, CpInk);
/* Convert ASCII letters, digits and spaces to game codes, for test data. */
void CpGameText_FromAscii(uint16_t *out, size_t capacity, const char *ascii);

#endif
