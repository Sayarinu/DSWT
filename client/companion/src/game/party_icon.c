#include "game/party_icon.h"
#include <stdlib.h>
#include <string.h>

/* Only the tail of ARM9 contains the game's icon-to-palette index. Some
 * releases store ARM9 backwards-LZ compressed. Decode just this window. */
enum { WINDOW = 128 * 1024 };

static int ExpandTail(const uint8_t *input, unsigned size, unsigned header, uint8_t *output)
{
    unsigned src = size - header, dest = WINDOW;
    while (dest && src) {
        unsigned flags = input[--src];
        for (unsigned mask = 128; mask && dest; mask >>= 1) {
            if (!src) return -1;
            if (!(flags & mask)) {
                output[--dest] = input[--src];
                continue;
            }
            if (src < 2) return -1;
            src -= 2;
            unsigned pair = CpLe16(input + src), distance = 3 + (pair & 4095);
            if (distance > WINDOW - dest) return -1;
            for (unsigned n = 3 + (pair >> 12); n && dest; --n) {
                --dest;
                output[dest] = output[dest + distance];
            }
        }
    }
    return dest ? -1 : 0;
}

static void ReadIndices(CpPartyIcons *icons, const CpReader *reader, int gen5, unsigned count)
{
    CpRomFile rom;
    uint8_t hdr[16];
    if (CpRomFile_Whole(reader, &rom) || CpRomFile_Read(&rom, 32, hdr, sizeof(hdr))) return;
    uint32_t base = CpLe32(hdr), size = CpLe32(hdr + 12);
    if (size < WINDOW || base > rom.size || size > rom.size - base) return;
    uint8_t *raw = malloc(WINDOW), *expanded = NULL;
    if (!raw || CpRomFile_Read(&rom, base + size - WINDOW, raw, WINDOW)) goto done;
    unsigned footer = CpLe32(raw + WINDOW - 8), header = footer >> 24;
    const uint8_t *data = raw;
    if (header >= 8 && header <= 11 && (footer & 0xffffff) <= size && (footer & 0xffffff) >= header) {
        expanded = malloc(WINDOW);
        if (!expanded || ExpandTail(raw, WINDOW, header, expanded)) goto done;
        data = expanded;
    }
    /* The same first 16 species identify the beginning in each language.
     * Gen V packs male and female palette numbers into the two nibbles. */
    static const uint8_t first[] = {0,1,1,1,0,0,0,0,2,2,1,1,0,1,2,2};
    uint8_t prefix[sizeof(first)];
    for (unsigned i = 0; i < sizeof(first); ++i) prefix[i] = first[i] * (gen5 ? 17 : 1);
    for (unsigned i = 0; i + count <= WINDOW; i += 4) {
        if (memcmp(data + i, prefix, sizeof(prefix))) continue;
        memcpy(icons->paletteIndex, data + i, count);
        icons->count = count;
        break;
    }
done:
    free(expanded);
    free(raw);
}

void CpPartyIcons_Open(CpPartyIcons *icons, const CpGame *game)
{
    icons->count = 0;
    if (!game->table || !game->personal.file.reader) return;
    int gen5 = game->table->gen5PersonalSize != 0;
    CpFamily family = game->rom.family;
    const char *path = gen5 ? "a/0/0/7" : family == CP_FAMILY_HGSS ? "a/0/2/0"
        : family == CP_FAMILY_DP ? "poketool/icongra/poke_icon.narc" : "poketool/icongra/pl_poke_icon.narc";
    if (CpNarc_OpenPath(game->personal.file.reader, path, &icons->archive)) return;
    unsigned first = family == CP_FAMILY_B2W2 ? 8 : 7;
    if (icons->archive.count <= first) return;
    unsigned count = (icons->archive.count - first) / (gen5 ? 2 : 1);
    if (count >= 16 && count <= sizeof(icons->paletteIndex))
        ReadIndices(icons, game->personal.file.reader, gen5, count);
}

static unsigned Gen4Index(CpFamily family, unsigned species, unsigned form)
{
    static const struct { unsigned species, first, forms; } variants[] = {
        {386,496,3}, {201,500,27}, {412,527,2}, {413,529,2}, {422,531,1}, {423,532,1},
        {487,533,1}, {492,534,1}, {479,535,5}
    };
    for (unsigned i = 0; i < sizeof(variants)/sizeof(*variants); ++i)
        if (species == variants[i].species && form && form <= variants[i].forms &&
            (family != CP_FAMILY_DP || variants[i].first < 533)) return variants[i].first + form - 1;
    return species;
}

int CpPartyIcon_Load(const CpPartyIcons *icons, const CpGame *game, unsigned species, unsigned form,
                     int female, int egg, CpPartyIcon *out)
{
    out->ready = 0;
    if (!icons->count || !species) return -1;
    CpFamily family = game->rom.family;
    int gen5 = game->table->gen5PersonalSize != 0;
    unsigned index = species;
    if (egg) index = (gen5 ? 650 : 494) + (species == 490);
    else if (!gen5) index = Gen4Index(family, species, form);
    else if (form) {
        CpRomFile record;
        uint8_t data[4];
        if (CpNarc_Member(&game->personal, species, &record) || CpRomFile_Read(&record, 0x1e, data, 4)) return -1;
        if (form < data[2] && !(data[3] & 128)) index = 651 + CpLe16(data) + form;
    }
    if (index >= icons->count) return -1;
    unsigned member = (family == CP_FAMILY_B2W2 ? 8 : 7) + index * (gen5 ? 2 : 1);
    CpRomFile chr, pal;
    if (gen5 && female && !CpNarc_Member(&icons->archive, member + 1, &chr) && chr.size) ++member;
    unsigned bank = icons->paletteIndex[index];
    if (gen5) bank = female ? bank >> 4 : bank & 15;
    uint8_t header[48], colors[32];
    if (bank > 2 || CpNarc_Member(&icons->archive, member, &chr) ||
        CpRomFile_Read(&chr, 0, header, sizeof(header)) || memcmp(header, "RGCN", 4) ||
        CpLe32(header+28) != 3 || CpLe32(header+44) != 24 ||
        CpRomFile_Read(&chr, 48, out->tiles, sizeof(out->tiles)) || CpNarc_Member(&icons->archive, 0, &pal) ||
        CpRomFile_Read(&pal, 40 + 32 * bank, colors, sizeof(colors))) return -1;
    for (unsigned i = 0; i < 16; ++i) out->palette[i] = CpLe16(colors + 2*i);
    out->ready = 1;
    return 0;
}
