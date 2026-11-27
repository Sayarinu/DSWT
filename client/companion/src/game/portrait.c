#include "game/portrait.h"

#include <stdlib.h>
#include <string.h>

/* Nitro LZ11, bounded to the single 96x96 character sheet we request. */
static int Expand(const uint8_t *src, size_t len, uint8_t *out, size_t limit)
{
    if (len < 4 || src[0] != 0x11 || (CpLe32(src) >> 8) != limit) return -1;
    size_t in = 4, pos = 0;
    while (pos < limit && in < len) {
        unsigned flags = src[in++];
        for (unsigned bit = 128; bit && pos < limit; bit >>= 1) {
            if (in >= len) return -1;
            unsigned token = src[in++];
            if (!(flags & bit)) { out[pos++] = (uint8_t)token; continue; }
            unsigned n = token >> 4;
            if (n < 2) {
                if (in >= len) return -1;
                unsigned extra = n ? 0x111 : 0x11;
                unsigned encoded = token & 15;
                if (n) {
                    encoded = encoded * 256 + src[in++];
                    if (in >= len) return -1;
                }
                token = src[in++];
                n = extra + encoded * 16 + (token >> 4);
            } else ++n;
            if (in >= len) return -1;
            unsigned distance = 1 + (token & 15) * 256 + src[in++];
            if (distance > pos || n > limit - pos) return -1;
            while (n--) { out[pos] = out[pos - distance]; ++pos; }
        }
    }
    return pos == limit ? 0 : -1;
}

/* Gen IV's form archive places the palettes after the character sheets.
 * The first palette is member 134 in DP, 154 in Platinum, 158 in HGSS. */
static int Gen4Members(CpFamily family, unsigned species, unsigned form, int female,
                       int shiny, int egg, unsigned *chr, unsigned *pal)
{
    static const struct { unsigned species, count, front, step, palette, palStep, shinyStep; } forms[] = {
        {386,4,1,2,0,0,1}, {201,28,9,2,2,0,1}, {351,4,68,1,4,1,4},
        {412,3,73,2,12,2,1}, {413,3,79,2,18,2,1}, {422,2,86,1,24,2,1},
        {423,2,90,1,28,2,1}, {421,2,94,1,32,1,2}, {493,18,97,2,36,2,1},
        {492,2,135,2,74,2,1}, {479,6,139,2,78,2,1}, {487,2,151,2,90,2,1},
        {172,2,155,2,94,2,1}
    };
    unsigned base = family == CP_FAMILY_HGSS ? 158 : family == CP_FAMILY_DP ? 134 : 154;
    if (egg) {
        *chr = 132 + (species == 490);
        *pal = base + 72 + (species == 490);
        return 1;
    }
    for (unsigned i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
        if (forms[i].species != species || (family == CP_FAMILY_DP && forms[i].front >= 135) ||
            (species == 172 && family != CP_FAMILY_HGSS)) continue;
        if (form >= forms[i].count) form = 0;
        *chr = forms[i].front + form * forms[i].step;
        *pal = base + forms[i].palette + form * forms[i].palStep + shiny * forms[i].shinyStep;
        return 1;
    }
    *chr = species * 6 + 2 + !female;
    *pal = species * 6 + 4 + shiny;
    return 0;
}

static int Gen5Members(const CpGame *game, const CpNarc *arc, unsigned species, unsigned form,
                       int female, int shiny, int egg, unsigned *chr, unsigned *pal)
{
    unsigned index = egg ? 650 + (species == 490) : species;
    unsigned palette = index * 20 + 18;
    if (form && !egg) {
        CpRomFile stats;
        uint8_t data[4];
        if (CpNarc_Member(&game->personal, species, &stats) ||
            CpRomFile_Read(&stats, 0x1e, data, sizeof(data))) return -1;
        if (form < data[2]) {
            unsigned variant = CpLe16(data) + form - 1;
            if (data[3] & 128) palette = (game->rom.family == CP_FAMILY_BW ? 14253u : 15033u) + variant * 2;
            else { index = 652 + variant; palette = index * 20 + 18; }
        }
    }
    *chr = index * 20;
    *pal = palette + shiny;
    if (female) {
        CpRomFile variant;
        if (CpNarc_Member(arc, *chr + 1, &variant)) return -1;
        if (variant.size) ++*chr;
    }
    return 0;
}

static int Decode(const CpRomFile *graphic, int gen5, int reverse, CpPortrait *out)
{
    enum { HEADER = 48, DS_BYTES = 6400, BW_BYTES = 4608 };
    size_t size = HEADER + (gen5 ? BW_BYTES : DS_BYTES);
    uint8_t sheet[HEADER + DS_BYTES];
    if (gen5) {
        if (graphic->size < 4 || graphic->size > size + size / 8 + 8) return -1;
        uint8_t *packed = malloc(graphic->size);
        if (!packed) return -1;
        int result = CpRomFile_Read(graphic, 0, packed, graphic->size);
        if (!result) result = Expand(packed, graphic->size, sheet, size);
        free(packed);
        if (result) return -1;
    } else if (CpRomFile_Read(graphic, 0, sheet, size)) return -1;
    if (memcmp(sheet, "RGCN", 4) || memcmp(sheet + 16, "RAHC", 4) ||
        CpLe32(sheet + 28) != 3 || CpLe32(sheet + 40) != size - HEADER ||
        CpLe32(sheet + 44) != 24) return -1;
    uint8_t *pixels = sheet + HEADER;
    if (!gen5) {
        uint32_t seed = CpLe16(pixels + (reverse ? DS_BYTES - 2 : 0));
        for (unsigned i = 0; i < DS_BYTES; i += 2) {
            unsigned at = reverse ? DS_BYTES - 2 - i : i;
            pixels[at] ^= (uint8_t)seed;
            pixels[at + 1] ^= (uint8_t)(seed >> 8);
            seed = seed * 1103515245u + 24691;
        }
    }
    for (unsigned y = 0; y < CP_PORTRAIT_SIDE; ++y) {
        for (unsigned x = 0; x < CP_PORTRAIT_SIDE; ++x) {
            unsigned sx = x, at = y * 80 + x / 2;
            if (gen5) {
                /* The 96px sheet is packed as four objects (64+32 in each axis). */
                sx = x * 96 / CP_PORTRAIT_SIDE;
                unsigned sy = y * 96 / CP_PORTRAIT_SIDE, tx = sx / 8, ty = sy / 8;
                unsigned tile = (ty < 8 ? 0 : 96) + (tx < 8 ? 0 : (ty < 8 ? 64 : 32))
                    + (ty % 8) * (tx < 8 ? 8 : 4) + tx % 8;
                at = tile * 32 + sy % 8 * 4 + sx % 8 / 2;
            }
            unsigned color = (pixels[at] >> ((sx & 1) * 4)) & 15;
            unsigned dest = ((y / 8) * 10 + x / 8) * 32 + y % 8 * 4 + x % 8 / 2;
            if (!(x & 1)) out->tiles[dest] = (uint8_t)color;
            else out->tiles[dest] |= (uint8_t)(color << 4);
        }
    }
    return 0;
}

int CpPortrait_Load(const CpGame *game, unsigned species, unsigned form, int female,
                    int shiny, int egg, CpPortrait *out)
{
    out->ready = 0;
    if (!game->table || !game->personal.file.reader || !species) return -1;
    CpFamily family = game->rom.family;
    int gen5 = family == CP_FAMILY_BW || family == CP_FAMILY_B2W2;
    if (species > (gen5 ? 649u : 493u)) return -1;
    unsigned chr = 0, pal = 0;
    const char *path = "a/0/0/4";
    if (!gen5) {
        int other = Gen4Members(family, species, form, female, shiny, egg, &chr, &pal);
        if (family == CP_FAMILY_DP) path = other ? "poketool/pokegra/otherpoke.narc" : "poketool/pokegra/pokegra.narc";
        if (family == CP_FAMILY_PLATINUM) path = other ? "poketool/pokegra/pl_otherpoke.narc" : "poketool/pokegra/pl_pokegra.narc";
        if (family == CP_FAMILY_HGSS && other) path = "a/1/1/4";
    }
    CpNarc arc;
    CpRomFile graphic, palette;
    uint8_t colors[72];
    if (CpNarc_OpenPath(game->personal.file.reader, path, &arc) ||
        (gen5 && Gen5Members(game, &arc, species, form, female, shiny, egg, &chr, &pal)) ||
        CpNarc_Member(&arc, chr, &graphic) || CpNarc_Member(&arc, pal, &palette) ||
        CpRomFile_Read(&palette, 0, colors, sizeof(colors)) ||
        memcmp(colors, "RLCN", 4) || memcmp(colors + 16, "TTLP", 4) ||
        CpLe32(colors + 36) != 16 || Decode(&graphic, gen5, family == CP_FAMILY_DP, out)) return -1;
    for (unsigned i = 0; i < 16; ++i) out->palette[i] = CpLe16(colors + 40 + 2 * i);
    out->ready = 1;
    return 0;
}
