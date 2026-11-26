#ifndef COMPANION_TEST_STOCK_GAME_H
#define COMPANION_TEST_STOCK_GAME_H

/* The stock (unpatched) game played between companion sessions, on a
 * synthetic save: savedata.c SaveData_LoadCheck chooses the blocks, the player
 * plays (the play time changes) and SaveData_Save writes the general block,
 * and both blocks with a new global counter when the load asked for a full
 * save. Written from the game's code, independently of save/save_image.c. */
#include "save_fixture.h"

static const unsigned kFxBlockOffsets[2] = { FX_GENERAL, FX_BOXES };
static const unsigned kFxBlockSizes[2] = { FX_GENERAL_SIZE, FX_BOXES_SIZE };

/* The footer CRC (CRC-16/CCITT-FALSE, as FxCrc), table driven: the sweeps
 * check and seal many blocks. */
static inline unsigned FxStockCrc(const unsigned char *p, unsigned size)
{
    static uint16_t table[256];
    static int ready;
    unsigned crc = 0xffff;
    if (!ready) {
        unsigned byte, bit;
        for (byte = 0; byte < 256; ++byte) {
            crc = byte << 8;
            for (bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0)) & 0xffff;
            table[byte] = (uint16_t)crc;
        }
        ready = 1; crc = 0xffff;
    }
    while (size--) crc = ((crc << 8) ^ table[((crc >> 8) ^ *p++) & 0xff]) & 0xffff;
    return crc;
}

static inline void FxStockSeal(FxFile *file, unsigned half, unsigned block, uint32_t global, uint32_t counter)
{
    unsigned char *body = file->bytes + half * CP_SAVE_HALF_SIZE + kFxBlockOffsets[block];
    unsigned char *footer = body + kFxBlockSizes[block] - 20;
    unsigned crc = FxStockCrc(body, kFxBlockSizes[block] - 20);
    FxPut32(footer, global); FxPut32(footer + 4, counter); FxPut32(footer + 8, kFxBlockSizes[block]);
    FxPut32(footer + 12, CP_SAVE_MAGIC_INTERNATIONAL); footer[16] = (unsigned char)block; footer[17] = 0;
    footer[18] = (unsigned char)crc; footer[19] = (unsigned char)(crc >> 8);
}

/* SaveCheckFooter: size, magic, block ID and CRC. */
static inline int FxStockValid(const FxFile *file, unsigned half, unsigned block, uint32_t *global, uint32_t *counter)
{
    const unsigned char *body = file->bytes + half * CP_SAVE_HALF_SIZE + kFxBlockOffsets[block];
    const unsigned char *footer = body + kFxBlockSizes[block] - 20;
    *global = Fx32(footer); *counter = Fx32(footer + 4);
    return Fx32(footer + 8) == kFxBlockSizes[block] && Fx32(footer + 12) == CP_SAVE_MAGIC_INTERNATIONAL
        && footer[16] == block && (unsigned)(footer[18] | (footer[19] << 8)) == FxStockCrc(body, kFxBlockSizes[block] - 20);
}

/* SaveDataState_CompareCounters, with its wrap from 0xFFFFFFFF to 0. */
static inline int FxStockCompare(uint32_t a, uint32_t b)
{
    if (a == UINT32_MAX && b == 0) return -1;
    if (a == 0 && b == UINT32_MAX) return 1;
    return (a > b) - (a < b);
}

/* CompareSectors: 2 both valid, 1 one, 0 none; cur and stale halves. */
static inline int FxStockSectors(const FxFile *file, unsigned block, int *cur, int *stale, uint32_t g[2], uint32_t c[2])
{
    int v0 = FxStockValid(file, 0, block, &g[0], &c[0]), v1 = FxStockValid(file, 1, block, &g[1], &c[1]);
    if (v0 && v1) {
        int gd = FxStockCompare(g[0], g[1]), cd = FxStockCompare(c[0], c[1]);
        *cur = gd > 0 ? 0 : gd < 0 ? 1 : cd >= 0 ? 0 : 1; *stale = !*cur;
        return 2;
    }
    *cur = v0 ? 0 : 1; *stale = -1;
    return v0 || v1;
}

/* LoadCheck's choice of halves; full when the game will save both blocks
 * (a CORRUPT load). 0 when the game would refuse to load. */
static inline int FxStockLoad(const FxFile *file, int selected[2], int *full, uint32_t *global, uint32_t counters[2])
{
    uint32_t ng[2], nc[2], bg[2], bc[2];
    int nCur, nStale, bCur, bStale;
    int n = FxStockSectors(file, 0, &nCur, &nStale, ng, nc), b = FxStockSectors(file, 1, &bCur, &bStale, bg, bc);
    if (!n || !b) return 0;
    if (n == 2 && b == 2) {
        if (ng[nCur] == bg[bCur]) { selected[0] = nCur; selected[1] = bCur; *full = 0; }
        else { selected[0] = nStale; selected[1] = bCur; *full = 1; }
    } else if (n == 1 && b == 2) {
        if (ng[nCur] == bg[bCur]) { selected[0] = nCur; selected[1] = bCur; }
        else if (ng[nCur] == bg[bStale]) { selected[0] = nCur; selected[1] = bStale; }
        else return 0;
        *full = 1;
    } else if (n == 2 && b == 1) {
        if (ng[nCur] == bg[bCur]) { selected[0] = nCur; selected[1] = bCur; *full = 0; }
        else { selected[0] = nStale; selected[1] = bCur; *full = 1; }
    } else {
        selected[0] = nCur; selected[1] = bCur; *full = nCur != bCur;
    }
    *global = ng[selected[0]]; counters[0] = nc[selected[0]]; counters[1] = bc[selected[1]];
    return 1;
}

/* What the player does before saving, on the loaded general block and PC. */
typedef void (*FxStockAction)(unsigned char *general, unsigned char *boxes);

/* Load, play (`action` may be NULL) and save `saves` times; forceFull saves
 * both blocks each time even after a clean load. 0 when the game would refuse
 * to load the save. */
static inline int FxStockPlay(FxFile *file, unsigned saves, int forceFull, FxStockAction action)
{
    static unsigned char ram[CP_SAVE_HALF_SIZE];
    uint32_t global, counters[2];
    int selected[2], full;
    unsigned pass, block;
    if (!FxStockLoad(file, selected, &full, &global, counters)) return 0;
    for (block = 0; block < 2; ++block)
        memcpy(ram + kFxBlockOffsets[block], file->bytes + selected[block] * CP_SAVE_HALF_SIZE + kFxBlockOffsets[block],
            kFxBlockSizes[block]);
    ram[FX_GENERAL + 0x10] ^= 0x5a; /* the play time */
    if (action) action(ram + FX_GENERAL, ram + FX_BOXES);
    for (pass = 0; pass < saves; ++pass) {
        int both = full || forceFull || action;
        if (both) ++global;
        for (block = 0; block < (both ? 2u : 1u); ++block) {
            unsigned char *body;
            selected[block] ^= 1;
            body = file->bytes + selected[block] * CP_SAVE_HALF_SIZE + kFxBlockOffsets[block];
            memcpy(body, ram + kFxBlockOffsets[block], kFxBlockSizes[block] - 20);
            FxStockSeal(file, (unsigned)selected[block], block, global, ++counters[block]);
        }
        full = 0; /* a full save leaves both blocks current */
    }
    return 1;
}

/* Copies of a Pokémon, by personality and trainer ID, in the blocks the game
 * would load: the party, then every PC slot. */
static inline unsigned FxStockCopies(const FxFile *file, const unsigned char canonical[136])
{
    uint32_t global, counters[2];
    int selected[2], full;
    unsigned count, i, copies = 0;
    const unsigned char *general, *boxes;
    unsigned char found[136];
    assert(FxStockLoad(file, selected, &full, &global, counters));
    general = file->bytes + selected[0] * CP_SAVE_HALF_SIZE + FX_GENERAL;
    boxes = file->bytes + selected[1] * CP_SAVE_HALF_SIZE + FX_BOXES;
    count = Fx32(general + FX_PARTY + 4);
    assert(count <= 6);
    for (i = 0; i < count + 18 * 30; ++i) {
        const unsigned char *slot = i < count ? general + FX_PARTY + 8 + i * FX_MON : boxes + FX_PC + (i - count) * FX_BOX_MON;
        if (WtPokemon_Decode(slot, found) != 0) continue;
        if ((found[0x08] | (found[0x09] << 8)) == 0) continue; /* empty */
        if (memcmp(found, canonical, 4) == 0 && memcmp(found + 0x0C, canonical + 0x0C, 4) == 0) ++copies;
    }
    return copies;
}

#endif
