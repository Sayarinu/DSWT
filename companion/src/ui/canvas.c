#include "ui/canvas.h"

#include <string.h>

#include "companion_text.h"

void CpCanvas_Clear(CpCanvas *canvas)
{
    memset(canvas->tiles, 0, CP_CANVAS_BYTES);
}

void CpCanvas_Pixel(CpCanvas *canvas, int x, int y, uint8_t colour)
{
    uint8_t *byte;
    if (x < 0 || y < 0 || x >= CP_CANVAS_WIDTH || y >= CP_CANVAS_HEIGHT) return;
    byte = canvas->tiles + ((y >> 3) * 32 + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1);
    if (x & 1) *byte = (uint8_t)((*byte & 0x0F) | (colour << 4));
    else *byte = (uint8_t)((*byte & 0xF0) | (colour & 0x0F));
}

uint8_t CpCanvas_Get(const CpCanvas *canvas, int x, int y)
{
    uint8_t byte;
    if (x < 0 || y < 0 || x >= CP_CANVAS_WIDTH || y >= CP_CANVAS_HEIGHT) return 0;
    byte = canvas->tiles[((y >> 3) * 32 + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
    return (x & 1) ? byte >> 4 : byte & 0x0F;
}

void CpCanvas_Fill(CpCanvas *canvas, int x, int y, int width, int height, uint8_t colour)
{
    int i, j;
    for (j = y; j < y + height; j++)
        for (i = x; i < x + width; i++) CpCanvas_Pixel(canvas, i, j, colour);
}

/* The next Unicode scalar of UTF-8 text; malformed bytes read as '?'. */
static uint32_t NextCodepoint(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t value;
    int extra, i;
    if (s[0] < 0x80) { *text += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0) { value = s[0] & 0x1F; extra = 1; }
    else if ((s[0] & 0xF0) == 0xE0) { value = s[0] & 0x0F; extra = 2; }
    else if ((s[0] & 0xF8) == 0xF0) { value = s[0] & 0x07; extra = 3; }
    else { *text += 1; return '?'; }
    for (i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { *text += i; return '?'; }
        value = value << 6 | (s[i] & 0x3F);
    }
    *text += extra + 1;
    return value;
}

static const CompanionFontGlyph *Glyph(uint32_t codepoint)
{
    uint32_t low = 0, high = Companion_FontGlyphCount;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        if (Companion_FontCodepoints[middle] == codepoint) return &Companion_FontGlyphs[middle];
        if (Companion_FontCodepoints[middle] < codepoint) low = middle + 1;
        else high = middle;
    }
    return codepoint == '?' ? NULL : Glyph('?');
}

static void DrawBits(CpCanvas *canvas, const CompanionFontGlyph *glyph, int left, int top, uint8_t colour)
{
    int stride = (glyph->width + 7) / 8, x, y;
    const uint8_t *bits = Companion_FontBitmap + glyph->offset;
    for (y = 0; y < glyph->height; y++)
        for (x = 0; x < glyph->width; x++)
            if (bits[y * stride + x / 8] & (0x80 >> (x % 8))) CpCanvas_Pixel(canvas, left + x, top + y, colour);
}

/* One glyph with its baseline at `baseline`; returns its advance. */
static int DrawGlyph(CpCanvas *canvas, int pen, int baseline, uint32_t codepoint, CpInk ink)
{
    const CompanionFontGlyph *glyph = Glyph(codepoint);
    int left, top;
    if (!glyph) return 0;
    if (canvas) {
        left = pen + glyph->x;
        top = baseline - glyph->y;
        if (ink.shadow) {
            DrawBits(canvas, glyph, left + 1, top, ink.shadow);
            DrawBits(canvas, glyph, left, top + 1, ink.shadow);
            DrawBits(canvas, glyph, left + 1, top + 1, ink.shadow);
        }
        DrawBits(canvas, glyph, left, top, ink.fg);
    }
    return glyph->advance;
}

/* The companion font's baseline sits this far below the top of a line, so
 * its letters fill a 16-pixel line the way the game's do. */
static int Baseline(void)
{
    return Companion_FontAscent - 1;
}

static int Draw(CpCanvas *canvas, int x, int y, const char *text, CpInk ink)
{
    int pen = x, widest = 0, baseline = y + Baseline();
    while (*text) {
        uint32_t codepoint = NextCodepoint(&text);
        if (codepoint == '\n') {
            if (pen - x > widest) widest = pen - x;
            pen = x;
            baseline += Companion_FontLineHeight;
            continue;
        }
        pen += DrawGlyph(canvas, pen, baseline, codepoint, ink);
    }
    return pen - x > widest ? pen - x : widest;
}

int CpCanvas_Text(CpCanvas *canvas, int x, int y, const char *text, CpInk ink)
{
    return Draw(canvas, x, y, text, ink);
}

int CpText_Width(const char *text)
{
    CpInk none = { 0, 0 };
    return Draw(NULL, 0, 0, text, none);
}

void CpText_Replace(char *out, size_t size, const char *text, const char *token, const char *value)
{
    size_t used = 0, tokenLength = strlen(token);
    if (!size) return;
    while (*text && used + 1 < size) {
        if (!strncmp(text, token, tokenLength)) {
            const char *v = value;
            while (*v && used + 1 < size) out[used++] = *v++;
            text += tokenLength;
        } else {
            out[used++] = *text++;
        }
    }
    out[used] = 0;
}

/* Game character codes of Generation IV's charmap for plain ASCII. */
static uint32_t GameToUnicode(uint16_t code)
{
    if (code >= 0x0121 && code <= 0x012A) return '0' + (code - 0x0121);
    if (code >= 0x012B && code <= 0x0144) return 'A' + (code - 0x012B);
    if (code >= 0x0145 && code <= 0x015E) return 'a' + (code - 0x0145);
    switch (code) {
    case 0x01DE: return ' ';
    case 0x01AB: return '!';
    case 0x01AD: return ',';
    case 0x01AE: return '.';
    case 0x01BE: return '-';
    case 0x01B3: return 0x2019;
    case 0x0188: return 0xE9;
    default: return '?';
    }
}

void CpGameText_FromAscii(uint16_t *out, size_t capacity, const char *ascii)
{
    size_t i = 0;
    if (!capacity) return;
    for (; *ascii && i + 1 < capacity; ascii++) {
        char c = *ascii;
        uint16_t code = CP_CHAR_QUESTION;
        if (c >= '0' && c <= '9') code = (uint16_t)(0x0121 + (c - '0'));
        else if (c >= 'A' && c <= 'Z') code = (uint16_t)(0x012B + (c - 'A'));
        else if (c >= 'a' && c <= 'z') code = (uint16_t)(0x0145 + (c - 'a'));
        else if (c == ' ') code = 0x01DE;
        else if (c == '.') code = 0x01AE;
        else if (c == '-') code = 0x01BE;
        out[i++] = code;
    }
    out[i] = CP_CHAR_EOS;
}

int CpCanvas_GameText(CpCanvas *canvas, int x, int y, const uint16_t *text, size_t capacity, const CpFont *font, CpInk ink)
{
    int pen = x;
    size_t i;
    for (i = 0; i < capacity && text[i] != CP_CHAR_EOS; i++) {
        CpGlyph glyph;
        unsigned gx, gy;
        if (!font || CpFont_Glyph(font, text[i], &glyph) != CP_OK) {
            pen += DrawGlyph(canvas, pen, y + Baseline(), GameToUnicode(text[i]), ink);
            continue;
        }
        /* The game's glyphs carry their own shadow (2); fill (3) stays clear. */
        for (gy = 0; gy < glyph.tilesHigh * 8u; gy++)
            for (gx = 0; gx < glyph.tilesWide * 8u; gx++) {
                uint8_t pixel = CpGlyph_Pixel(&glyph, gx, gy);
                if (pixel == 1) CpCanvas_Pixel(canvas, pen + (int)gx, y + (int)gy, ink.fg);
                else if (pixel == 2 && ink.shadow) CpCanvas_Pixel(canvas, pen + (int)gx, y + (int)gy, ink.shadow);
            }
        pen += glyph.width;
    }
    return pen - x;
}
