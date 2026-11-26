/* LUNE-880: the Diamond/Pearl and HeartGold/SoulSilver save layouts, against
 * independent models of each game's LoadCheck written from the decomps
 * (pokediamond arm9/src/save.c sub_02022AD8, pokeheartgold src/save.c
 * Save_GetSaveFilesStatus) and of each game's write order. */
#include "save/save_image.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_EVENTS = 128, DP = 0, HGSS = 1 };

typedef struct Game {
    const char *name;
    const CpSaveLayout *layout;
    unsigned offsets[2], sizes[2], footer, shared;
} Game;

static const Game games[2] = {
    { "Diamond/Pearl", &CpSaveLayout_DiamondPearl, { 0, 0xC100 }, { 0xC100, 0x121E0 }, 20, 0 },
    { "HeartGold/SoulSilver", &CpSaveLayout_HeartGoldSoulSilver, { 0, 0xF700 }, { 0xF628, 0x12310 }, 16, 1 },
};

typedef struct {
    unsigned offset;
    size_t size;
} Event;

typedef struct {
    unsigned char bytes[CP_SAVE_FILE_SIZE];
    Event events[MAX_EVENTS];
    unsigned count, failEvent;
    int partialWrite;
} MemoryFile;

static MemoryFile memory;
static CpSaveImage image;
static unsigned char before[CP_SAVE_FILE_SIZE];
static const Game *game;

static uint32_t Read32(const unsigned char *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Write32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static unsigned Crc(const unsigned char *p, unsigned size)
{
    unsigned crc = 0xffff, i;
    while (size--) {
        unsigned byte = *p++;
        for (i = 0; i < 8; ++i) {
            unsigned carry = ((crc >> 15) ^ (byte >> 7)) & 1;
            crc = ((crc << 1) ^ (carry ? 0x1021 : 0)) & 65535;
            byte <<= 1;
        }
    }
    return crc;
}

static unsigned char *Body(unsigned half, unsigned block)
{
    return memory.bytes + half * CP_SAVE_HALF_SIZE + game->offsets[block];
}

static unsigned char *Footer(unsigned half, unsigned block)
{
    return Body(half, block) + game->sizes[block] - game->footer;
}

/* A footer as the game builds it: DP SaveChunkFooter {global, block counter,
 * size, magic, u8 block, pad, crc}; HGSS {count, size, magic, u16 block, crc}. */
/* The selection matrix reseals unchanged bodies thousands of times: their CRCs
 * are kept while crcCached is set. */
static int crcCached;
static unsigned crcCache[2][2];

static unsigned BodyCrc(unsigned half, unsigned block)
{
    return crcCached ? crcCache[half][block] : Crc(Body(half, block), game->sizes[block] - game->footer);
}

static void Seal(unsigned half, unsigned block, uint32_t global, uint32_t counter, uint32_t magic)
{
    unsigned char *footer = Footer(half, block);
    unsigned crc = BodyCrc(half, block);
    if (game->shared) {
        Write32(footer, global); Write32(footer + 4, game->sizes[block]); Write32(footer + 8, magic);
        footer[12] = (unsigned char)block; footer[13] = 0;
        footer[14] = (unsigned char)crc; footer[15] = (unsigned char)(crc >> 8);
    } else {
        Write32(footer, global); Write32(footer + 4, counter); Write32(footer + 8, game->sizes[block]);
        Write32(footer + 12, magic); footer[16] = (unsigned char)block; footer[17] = 0x5c;
        footer[18] = (unsigned char)crc; footer[19] = (unsigned char)(crc >> 8);
    }
}

static int Valid(unsigned half, unsigned block)
{
    const unsigned char *footer = Footer(half, block);
    if (game->shared)
        return Read32(footer + 4) == game->sizes[block] && Read32(footer + 8) == CP_SAVE_MAGIC_INTERNATIONAL
            && (unsigned)(footer[12] | footer[13] << 8) == block && (unsigned)(footer[14] | footer[15] << 8) == BodyCrc(half, block);
    return Read32(footer + 8) == game->sizes[block] && Read32(footer + 12) == CP_SAVE_MAGIC_INTERNATIONAL
        && footer[16] == block && (unsigned)(footer[18] | footer[19] << 8) == BodyCrc(half, block);
}

static int ReadAt(void *context, uint32_t offset, void *data, size_t size)
{
    MemoryFile *file = context;
    if (offset > sizeof(file->bytes) || size > sizeof(file->bytes) - offset) return -1;
    memcpy(data, file->bytes + offset, size);
    return 0;
}

static int WriteAt(void *context, uint32_t offset, const void *data, size_t size)
{
    MemoryFile *file = context;
    assert(file->count < MAX_EVENTS);
    file->events[file->count].offset = offset;
    file->events[file->count++].size = size;
    if (file->count == file->failEvent) {
        if (file->partialWrite) memcpy(file->bytes + offset, data, size / 2);
        return -1;
    }
    memcpy(file->bytes + offset, data, size);
    return 0;
}

static int Sync(void *context)
{
    MemoryFile *file = context;
    assert(file->count < MAX_EVENTS);
    file->events[file->count].offset = 0;
    file->events[file->count++].size = 0;
    return file->count == file->failEvent ? -1 : 0;
}

static int Size(void *context, uint64_t *size)
{
    (void)context;
    *size = CP_SAVE_FILE_SIZE;
    return 0;
}

static CpFile file = { &memory, ReadAt, WriteAt, Sync, Size };

static CpSaveResult Load(void)
{
    return CpSaveImage_LoadLayout(&image, &file, game->layout, CP_SAVE_MAGIC_INTERNATIONAL);
}

static void Fixture(void)
{
    unsigned i, half, block;
    memset(&memory, 0, sizeof(memory));
    for (i = 0; i < sizeof(memory.bytes); ++i) memory.bytes[i] = (unsigned char)(i * 29 + 5);
    for (half = 0; half < 2; ++half) for (block = 0; block < 2; ++block)
        Seal(half, block, 40 + half, 70 + half + 10 * block, CP_SAVE_MAGIC_INTERNATIONAL);
}

/* ---- The games' LoadCheck ------------------------------------------------- */

static int CounterCompare(uint32_t a, uint32_t b)
{
    if (a == UINT32_MAX && b == 0) return -1;
    if (a == 0 && b == UINT32_MAX) return 1;
    return (a > b) - (a < b);
}

/* The halves the game loads, or -1: CpSaveImage refuses what the game would
 * refuse, assert on, or load as mismatched blocks. */
static int DpLoadCheck(unsigned halves[2])
{
    unsigned block, half, n[2], newest[2], other[2];
    uint32_t global[2][2], counter[2][2];
    int valid[2][2], assertion = 0;
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            valid[block][half] = Valid(half, block);
            global[block][half] = Read32(Footer(half, block));
            counter[block][half] = Read32(Footer(half, block) + 4);
        }
        /* sub_020229F0 */
        if (valid[block][0] && valid[block][1]) {
            int g = CounterCompare(global[block][0], global[block][1]);
            int c = CounterCompare(counter[block][0], counter[block][1]);
            if (g > 0) { assertion |= !(c > 0); newest[block] = 0; }
            else if (g < 0) { assertion |= !(c < 0); newest[block] = 1; }
            else newest[block] = c < 0;
            other[block] = !newest[block];
            n[block] = 2;
        } else if (valid[block][0] || valid[block][1]) {
            newest[block] = valid[block][1]; other[block] = 2; n[block] = 1;
        } else {
            n[block] = 0;
        }
    }
    if (n[0] == 0 || n[1] == 0 || assertion) return -1;
    halves[0] = newest[0]; halves[1] = newest[1];
    if (global[0][newest[0]] != global[1][newest[1]]) {
        if (n[0] == 2) halves[0] = other[0];          /* (2,2) and (2,1): older general */
        else if (n[1] == 2) halves[1] = other[1];     /* (1,2): older boxes if they match */
    }
    return global[0][halves[0]] == global[1][halves[1]] ? 0 : -1;
}

static int HgssLoadCheck(unsigned halves[2])
{
    unsigned block, half, good[2], newer[2], older[2], chosen = 2;
    uint32_t count[2][2];
    int valid[2][2];
    for (block = 0; block < 2; ++block) {
        for (half = 0; half < 2; ++half) {
            valid[block][half] = Valid(half, block);
            count[block][half] = valid[block][half] ? Read32(Footer(half, block)) : 0;
        }
        /* SaveSlotCheckCompare */
        if (valid[block][0] && valid[block][1]) {
            newer[block] = CounterCompare(count[block][0], count[block][1]) < 0;
            older[block] = !newer[block]; good[block] = 2;
        } else if (valid[block][0] || valid[block][1]) {
            newer[block] = valid[block][1]; older[block] = 2; good[block] = 1;
        } else {
            good[block] = 0;
        }
    }
    if (good[0] == 0 || good[1] == 0) return -1;
    if (good[0] == 2 && good[1] == 2) {
        if (count[0][newer[0]] == count[1][newer[0]]) chosen = newer[0];
        else if (count[0][older[0]] == count[1][older[0]]) chosen = older[0];
    } else if (good[0] == 1 && good[1] == 2) {
        if (count[0][newer[0]] == count[1][newer[0]]) chosen = newer[0];
    } else if (good[0] == 2 && good[1] == 1) {
        if (count[0][newer[0]] == count[1][newer[0]]) chosen = newer[0];
        else if (count[0][older[0]] == count[1][older[0]]) chosen = older[0];
    } else if (newer[0] == newer[1] && count[0][newer[0]] == count[1][newer[1]]) {
        chosen = newer[0];
    }
    /* Save_LoadDynamicRegion validates both blocks of the chosen half. */
    if (chosen == 2 || !valid[0][chosen] || !valid[1][chosen]) return -1;
    halves[0] = halves[1] = chosen;
    return 0;
}

static int GameLoad(unsigned halves[2])
{
    return game->shared ? HgssLoadCheck(halves) : DpLoadCheck(halves);
}

/* Every footer valid or not, with counters around a wrap, against the model. */
static void SelectionMatrix(void)
{
    static const uint32_t values[] = { 8, 9, UINT32_MAX, 0 };
    unsigned combination, cases = 0, loaded = 0, block, half;
    Fixture();
    for (block = 0; block < 2; ++block) for (half = 0; half < 2; ++half) crcCache[half][block] = BodyCrc(half, block);
    crcCached = 1; /* the loop changes footers only */
    for (combination = 0; combination < 16 * 256; ++combination) {
        unsigned mask = combination % 16, rest = combination / 16, halves[2];
        uint32_t globals[2][2];
        CpSaveResult result;
        for (block = 0; block < 2; ++block) for (half = 0; half < 2; ++half) {
            globals[block][half] = values[rest % 4]; rest /= 4;
            /* DP block counters follow the global so that LoadCheck holds them consistent. */
            Seal(half, block, globals[block][half], globals[block][half] * 3 + block, CP_SAVE_MAGIC_INTERNATIONAL);
            if (!(mask >> (block * 2 + half) & 1)) Footer(half, block)[game->shared ? 14 : 18] ^= 1;
        }
        result = Load();
        ++cases;
        if (GameLoad(halves) == 0) {
            assert(result == CP_SAVE_OK);
            assert(image.currentHalf[0] == halves[0] && image.currentHalf[1] == halves[1]);
            assert(image.globalCounter == Read32(Footer(halves[0], 0)));
            ++loaded;
        } else {
            assert(result == CP_SAVE_INVALID || result == CP_SAVE_NEEDS_GAME_SAVE);
            assert(!image.loaded);
            if ((mask & 3) == 0 || (mask & 12) == 0) assert(result == CP_SAVE_INVALID);
        }
    }
    crcCached = 0;
    /* DP's general-only saves: two general copies share the global counter. */
    if (!game->shared) {
        Fixture();
        Seal(0, 0, 5, 9, CP_SAVE_MAGIC_INTERNATIONAL); Seal(1, 0, 5, 10, CP_SAVE_MAGIC_INTERNATIONAL);
        Seal(0, 1, 5, 4, CP_SAVE_MAGIC_INTERNATIONAL); Seal(1, 1, 4, 2, CP_SAVE_MAGIC_INTERNATIONAL);
        assert(Load() == CP_SAVE_OK && image.currentHalf[0] == 1 && image.currentHalf[1] == 0);
        assert(image.blockCounters[0] == 10 && image.blockCounters[1] == 4 && image.globalCounter == 5);
    } else {
        /* One counter for both blocks of the half. */
        Fixture();
        assert(Load() == CP_SAVE_OK && image.currentHalf[0] == 1 && image.currentHalf[1] == 1);
        assert(image.globalCounter == 41 && image.blockCounters[0] == 41 && image.blockCounters[1] == 41);
    }
    /* The other magic is refused, the Korean one loads with its own. */
    Fixture();
    assert(CpSaveImage_LoadLayout(&image, &file, game->layout, CP_SAVE_MAGIC_KOREAN) == CP_SAVE_INVALID);
    for (block = 0; block < 4; ++block) Seal(block & 1, block >> 1, 40 + (block & 1), 70 + (block & 1) + 10 * (block >> 1), CP_SAVE_MAGIC_KOREAN);
    assert(CpSaveImage_LoadLayout(&image, &file, game->layout, CP_SAVE_MAGIC_KOREAN) == CP_SAVE_OK);
    assert(CpSaveImage_LoadAny(&image, &file) == CP_SAVE_OK && image.layout == game->layout
        && image.magic == CP_SAVE_MAGIC_KOREAN);
    /* Neither Platinum's layout nor the other game's reads this save. */
    assert(CpSaveImage_Load(&image, &file, CP_SAVE_MAGIC_KOREAN) == CP_SAVE_INVALID);
    assert(CpSaveImage_LoadLayout(&image, &file, games[!(game - games)].layout, CP_SAVE_MAGIC_KOREAN) == CP_SAVE_INVALID);
    printf("PASS %s selection: %u footer combinations (%u loadable) match the game's LoadCheck; magic and layout checks\n",
           game->name, cases, loaded);
}

/* ---- Writes --------------------------------------------------------------- */

static unsigned ExpectBlock(unsigned event, unsigned block, unsigned target)
{
    unsigned start = target * CP_SAVE_HALF_SIZE + game->offsets[block], body = game->sizes[block] - game->footer, box;
    if (!game->shared) {
        assert(memory.events[event].offset == start && memory.events[event].size == body);
        assert(memory.events[event + 1].size == 0);
        assert(memory.events[event + 2].offset == start + body + 8 && memory.events[event + 2].size == 8);
        assert(memory.events[event + 3].size == 0);
        assert(memory.events[event + 4].offset == start + body && memory.events[event + 4].size == 20);
        assert(memory.events[event + 5].size == 0);
        return event + 6;
    }
    if (block == 0) {
        assert(memory.events[event].offset == start && memory.events[event].size == body);
        assert(memory.events[event + 1].size == 0);
        event += 2;
    } else {
        /* Save_WritePCBoxes: all 18 boxes, then Save_WritePCFooter's tail. */
        for (box = 0; box < 18; ++box, event += 2) {
            assert(memory.events[event].offset == start + box * 0x1000 && memory.events[event].size == 0x1000);
            assert(memory.events[event + 1].size == 0);
        }
        assert(memory.events[event].offset == start + 0x12000 && memory.events[event].size == body - 0x12000);
        assert(memory.events[event + 1].size == 0);
        event += 2;
    }
    assert(memory.events[event].offset == start + body && memory.events[event].size == 16);
    assert(memory.events[event + 1].size == 0);
    return event + 2;
}

static unsigned Events(void) { return game->shared ? 2 * (4 + 40) : 24; }

static void CheckUntouched(void)
{
    unsigned half;
    for (half = 0; half < 2; ++half) {
        unsigned start = half * CP_SAVE_HALF_SIZE + game->offsets[1] + game->sizes[1];
        assert(memcmp(memory.bytes + start, before + start, CP_SAVE_HALF_SIZE - game->offsets[1] - game->sizes[1]) == 0);
        start = half * CP_SAVE_HALF_SIZE + game->sizes[0];
        assert(memcmp(memory.bytes + start, before + start, game->offsets[1] - game->sizes[0]) == 0);
    }
}

static void Commit(void)
{
    unsigned block, half, pass, event = 0, initial[2];
    unsigned char expected[32], actual[32];
    uint32_t counters[2], global;
    Fixture();
    if (!game->shared) {
        /* Blocks in different halves, as DP's general-only saves leave them. */
        Seal(0, 0, 41, 90, CP_SAVE_MAGIC_INTERNATIONAL);
        Seal(1, 0, 41, 89, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    assert(Load() == CP_SAVE_OK);
    global = image.globalCounter;
    for (block = 0; block < 2; ++block) {
        initial[block] = image.currentHalf[block];
        counters[block] = image.blockCounters[block];
        CpSaveImage_Block(&image, block)[300 + block] ^= 0x6b;
    }
    assert(CpSaveImage_Hash(&image, expected) == CP_SAVE_OK);
    memcpy(before, memory.bytes, sizeof(before));
    assert(CpSaveImage_CommitBoth(&image, &file, expected) == CP_SAVE_OK);
    assert(memory.count == Events());
    for (pass = 0; pass < 2; ++pass) for (block = 0; block < 2; ++block)
        event = ExpectBlock(event, block, initial[block] ^ (pass == 0));
    assert(image.globalCounter == global + 2);
    for (block = 0; block < 2; ++block) {
        assert(image.currentHalf[block] == initial[block]);
        assert(image.blockCounters[block] == (game->shared ? global : counters[block]) + 2);
        assert(memcmp(Body(0, block), Body(1, block), game->sizes[block] - game->footer) == 0);
        for (half = 0; half < 2; ++half) {
            uint32_t advance = half == initial[block] ? 2 : 1;
            unsigned char *footer = Footer(half, block);
            assert(Valid(half, block));
            assert(Read32(footer) == global + advance);
            if (!game->shared) {
                assert(Read32(footer + 4) == counters[block] + advance);
                assert(footer[17] == before[footer - memory.bytes + 17]);
            }
        }
    }
    if (game->shared) {
        /* Save_SetAllPCBoxesModified before each save: BOX_ALL_MODIFIED_FLAG in both halves. */
        for (half = 0; half < 2; ++half) assert(Read32(Body(half, 1) + 0x12004) == 0x3FFFF);
    }
    CheckUntouched();
    assert(CpSaveImage_Hash(&image, actual) == CP_SAVE_OK && memcmp(expected, actual, 32) == 0);
    assert(Load() == CP_SAVE_OK);
    assert(CpSaveImage_Verify(&image, &file, expected) == CP_SAVE_OK);
    printf("PASS %s commit: the game's write order over %u events, footers, counters and identical bodies\n",
           game->name, Events());
}

static void HashAndVerify(void)
{
    unsigned char digest[32], other[32];
    unsigned half, block, field;
    static const unsigned dpFields[] = { 0, 4, 8, 12, 16, 18 }, hgssFields[] = { 0, 4, 8, 12, 14 };
    const unsigned *fields = game->shared ? hgssFields : dpFields;
    unsigned fieldCount = game->shared ? 5 : 6;
    Fixture();
    assert(Load() == CP_SAVE_OK);
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_OK);
    /* The PC's modified flag differs between halves after the game's own saves. */
    if (game->shared) {
        Write32(Body(0, 1) + 0x12004, 0x15); Seal(0, 1, Read32(Footer(0, 1)), 0, CP_SAVE_MAGIC_INTERNATIONAL);
        Write32(Body(1, 1) + 0x12004, 0); Seal(1, 1, Read32(Footer(1, 1)), 0, CP_SAVE_MAGIC_INTERNATIONAL);
        assert(Load() == CP_SAVE_OK);
        assert(CpSaveImage_Hash(&image, other) == CP_SAVE_OK && memcmp(digest, other, 32) == 0);
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK);
        /* The byte after it is hashed. */
        CpSaveImage_Block(&image, 1)[0x12008] ^= 1;
        assert(CpSaveImage_Hash(&image, other) == CP_SAVE_OK && memcmp(digest, other, 32) != 0);
        CpSaveImage_Block(&image, 1)[0x12008] ^= 1;
        /* The two blocks of a half must carry the same counter. */
        Seal(0, 1, Read32(Footer(0, 1)) + 1, 0, CP_SAVE_MAGIC_INTERNATIONAL);
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
        Seal(0, 1, Read32(Footer(0, 1)) - 1, 0, CP_SAVE_MAGIC_INTERNATIONAL);
    }
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK);
    for (half = 0; half < 2; ++half) for (block = 0; block < 2; ++block) {
        for (field = 0; field < fieldCount; ++field) {
            unsigned char *p = Footer(half, block) + fields[field];
            *p ^= 0x40;
            assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
            *p ^= 0x40;
        }
        Body(half, block)[77] ^= 1;
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
        Seal(half, block, Read32(Footer(half, block)), game->shared ? 0 : Read32(Footer(half, block) + 4), CP_SAVE_MAGIC_INTERNATIONAL);
        assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_VERIFY_FAILED);
        Body(half, block)[77] ^= 1;
        Seal(half, block, Read32(Footer(half, block)), game->shared ? 0 : Read32(Footer(half, block) + 4), CP_SAVE_MAGIC_INTERNATIONAL);
    }
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK);
    printf("PASS %s verify: every footer field, body and repaired CRC%s\n", game->name,
           game->shared ? ", masked PC flag and one counter per half" : "");
}

/* A commit cut at every write and sync, whole or half written: the game then
 * loads the old image or the new one, never a mix, and a resumed commit settles. */
static void Interruptions(void)
{
    unsigned fixture, partial, fail, stops = 0;
    for (fixture = 0; fixture < 4; ++fixture)
    for (partial = 0; partial < 2; ++partial)
    for (fail = 1; fail <= Events(); ++fail) {
        unsigned char oldDigest[32], digest[32], actual[32];
        unsigned initial[2], halves[2], block, changed[2];
        /* HeartGold/SoulSilver: a cut at boxes 1..16 (events 7..38 of each
         * 44-event save) leaves the states of a cut at box 0 or 17, which run. */
        if (game->shared && (fail - 1) % 44 + 1 >= 7 && (fail - 1) % 44 + 1 <= 38) continue;
        Fixture();
        if (fixture == 1) {
            /* Wrap: the newer half's counter is 0. */
            for (block = 0; block < 2; ++block) {
                Seal(0, block, UINT32_MAX, UINT32_MAX - block, CP_SAVE_MAGIC_INTERNATIONAL);
                Seal(1, block, 0, 0 - (uint32_t)block, CP_SAVE_MAGIC_INTERNATIONAL);
            }
        } else if (fixture >= 2) {
            /* A torn newer half: the game falls back to the older one. */
            Body(0, 0)[20] = 0x10; Body(1, 0)[20] = 0x12; Body(0, 1)[20] = 0x14; Body(1, 1)[20] = 0x16;
            Seal(0, 0, 40, 70, CP_SAVE_MAGIC_INTERNATIONAL); Seal(0, 1, 40, 80, CP_SAVE_MAGIC_INTERNATIONAL);
            Seal(1, 0, 41, 71, CP_SAVE_MAGIC_INTERNATIONAL);
            if (fixture == 2) Seal(1, 1, 39, 79, CP_SAVE_MAGIC_INTERNATIONAL);
            else Footer(1, 1)[game->shared ? 14 : 18] ^= 1;
        }
        assert(Load() == CP_SAVE_OK);
        assert(GameLoad(halves) == 0 && halves[0] == image.currentHalf[0] && halves[1] == image.currentHalf[1]);
        for (block = 0; block < 2; ++block) initial[block] = image.currentHalf[block];
        assert(CpSaveImage_Hash(&image, oldDigest) == CP_SAVE_OK);
        CpSaveImage_Block(&image, 0)[20] ^= 1;
        CpSaveImage_Block(&image, 1)[20] ^= 1;
        assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        memcpy(before, memory.bytes, sizeof(before));
        memory.failEvent = fail; memory.partialWrite = (int)partial;
        assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_IO_ERROR);
        assert(memory.count == fail && !image.loaded);
        CheckUntouched();
        assert(GameLoad(halves) == 0);
        for (block = 0; block < 2; ++block) {
            const unsigned char *old = before + initial[block] * CP_SAVE_HALF_SIZE + game->offsets[block];
            const unsigned char *body = Body(halves[block], block);
            unsigned size = game->sizes[block] - game->footer;
            assert(memcmp(body, old, 20) == 0);
            if (game->shared && block == 1) {
                assert(memcmp(body + 21, old + 21, 0x12004 - 21) == 0);
                assert(memcmp(body + 0x12008, old + 0x12008, size - 0x12008) == 0);
            } else {
                assert(memcmp(body + 21, old + 21, size - 21) == 0);
            }
            assert(body[20] == old[20] || body[20] == (unsigned char)(old[20] ^ 1));
            changed[block] = body[20] != old[20];
        }
        assert(changed[0] == changed[1]);
        assert(Load() == CP_SAVE_OK);
        assert(image.currentHalf[0] == halves[0] && image.currentHalf[1] == halves[1]);
        assert(CpSaveImage_Hash(&image, actual) == CP_SAVE_OK);
        assert(memcmp(actual, changed[0] ? digest : oldDigest, 32) == 0);
        memory.count = memory.failEvent = 0;
        assert(CpSaveImage_CommitBoth(&image, &file, actual) == CP_SAVE_OK);
        assert(Load() == CP_SAVE_OK && CpSaveImage_Verify(&image, &file, actual) == CP_SAVE_OK);
        ++stops;
    }
    printf("PASS %s interruptions: %u cut commits (every write and sync, whole or half written, wrap, torn newer half) load old or new, never mixed, and resume\n",
           game->name, stops);
}

int main(void)
{
    unsigned i;
    for (i = 0; i < 2; ++i) {
        game = &games[i];
        SelectionMatrix();
        Commit();
        HashAndVerify();
        Interruptions();
    }
    return 0;
}
