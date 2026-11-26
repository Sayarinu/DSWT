/* LUNE-881: the Black/White (FX_GAME 0) and Black 2/White 2 (FX_GAME 1) save
 * layouts, against an independent model of the games' backup-system load
 * (aletheia arm9/backup_system.c BACKUP_SYSTEM_LoadDoubleCopy, pokebw2
 * src/gfl/backup_system.c SaveData_LoadWithBackup) and of their save control.
 * Saves are synthetic: every block filled, sealed as the game seals it. Given
 * retail saves as arguments, it also loads, hashes and verifies them. */
#include "save/gen5_save.h"
#include "save/sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FX_GAME
#define FX_GAME 0
#endif

enum { MAX_EVENTS = 1024, MAIN = 1, BACKUP = 2 };

typedef struct Game {
    const char *name;
    const CpSaveLayout *layout;
    unsigned backup, used, blocks, end, party, trainer, musical, extraCounts;
    uint32_t musicalAddress, musicalUsed, musicalBlock;
} Game;

/* client/core/save-layouts/save-layout-bw.json and -b2w2.json: main "used" and the backup's
 * offset; the musical distribution region; the blocks end where the CRC
 * table begins. */
static const Game games[2] = {
    { "Black/White", &CpSaveLayout_BlackWhite, 0x24000, 147356, 69, 0x23F00, 0x1A, 0x1B, 0x2A, 0x44,
      0x56000, 0x1FD14, 0x1FC00 },
    { "Black 2/White 2", &CpSaveLayout_Black2White2, 0x26000, 155556, 73, 0x25F00, 0x1A, 0x1B, 0x2A, 0x5C,
      0x55800, 0x17D14, 0x17C00 },
};

static const Game *game = &games[FX_GAME];

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
static unsigned offsets[CP_SAVE_GEN5_BLOCKS_MAX];

static unsigned R16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t R32(const unsigned char *p) { return R16(p) | (uint32_t)R16(p + 2) << 16; }
static void W16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void W32(unsigned char *p, uint32_t v) { W16(p, v & 0xFFFF); W16(p + 2, v >> 16); }

/* CRC-16/CCITT from a table, as MATH_CalcCRC16CCITT computes it. */
static unsigned Crc(const unsigned char *p, unsigned size)
{
    static unsigned table[256];
    unsigned crc = 0xFFFF, i, bit;
    if (!table[1]) for (i = 0; i < 256; ++i) {
        unsigned value = i << 8;
        for (bit = 0; bit < 8; ++bit) value = (value << 1 ^ (value & 0x8000 ? 0x1021 : 0)) & 0xFFFF;
        table[i] = value;
    }
    while (size--) crc = (crc << 8 ^ table[(crc >> 8 ^ *p++) & 0xFF]) & 0xFFFF;
    return crc;
}

static unsigned Size(unsigned id) { return game->layout->gen5->blockSizes[id]; }
static unsigned TableSize(void) { return game->blocks * 2 + (game->blocks & 1) * 2; }
static unsigned Table(void) { return game->used - 16 - TableSize(); }
static unsigned char *Copy(unsigned area) { return memory.bytes + (area == BACKUP ? game->backup : 0); }

static void Layout(void)
{
    unsigned id, at = 0;
    const CpSaveGen5 *g = game->layout->gen5;
    assert(g->blockCount == game->blocks && g->dataSize == game->used && g->backupOffset == game->backup);
    for (id = 0; id < game->blocks; ++id) {
        offsets[id] = at;
        at = (at + Size(id) + 4 + 0xFF) & ~0xFFu;
    }
    assert(at == game->end && Table() == game->end);
    assert(Size(game->party) == 0x534 && Size(game->trainer) == (FX_GAME ? 0xB0u : 0x68u)
        && Size(game->musical) == 0x2A4 && Size(0x31) == 0x35C);
}

/* Seal block `id` of a copy: its footer {count, CRC} and its CRC table entry. */
static void SealBlock(unsigned area, unsigned id, unsigned count)
{
    unsigned char *copy = Copy(area);
    unsigned crc = Crc(copy + offsets[id], Size(id));
    W16(copy + offsets[id] + Size(id), count);
    W16(copy + offsets[id] + Size(id) + 2, crc);
    W16(copy + Table() + 2 * id, crc);
}

static void SealFooter(unsigned area, uint32_t count)
{
    unsigned char *copy = Copy(area), *footer = copy + game->used - 16;
    W32(footer, count); W32(footer + 4, game->used); W32(footer + 8, CP_SAVE_MAGIC_GEN5);
    W16(footer + 14, Crc(copy + Table(), TableSize()));
}

static unsigned seed = 1;
static unsigned char Random8(void) { seed = seed * 1103515245u + 12345u; return (unsigned char)(seed >> 16); }

/* Two identical copies of a played save: every block holds data, the intro
 * flag is clear and no marker is set. */
static void Fixture(uint32_t count)
{
    unsigned id, i;
    memset(memory.bytes, 0xFF, sizeof(memory.bytes));
    for (id = 0; id < game->blocks; ++id) {
        for (i = 0; i < Size(id); ++i) Copy(MAIN)[offsets[id] + i] = Random8();
        if (id == game->trainer) memset(Copy(MAIN) + offsets[id] + 0x2C, 0, Size(id) - 0x2C);
        if (id == game->musical) Copy(MAIN)[offsets[id] + 0x29E] = 0;
        SealBlock(MAIN, id, 7 + id);
    }
    memset(Copy(MAIN) + Table() + 2 * game->blocks, 0, TableSize() - 2 * game->blocks);
    W16(Copy(MAIN) + game->used - 4, 0);
    SealFooter(MAIN, count);
    memcpy(Copy(BACKUP), Copy(MAIN), game->used);
    memory.count = memory.failEvent = 0;
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
    if (file->failEvent && file->count >= file->failEvent) {
        if (file->count == file->failEvent && file->partialWrite) memcpy(file->bytes + offset, data, size / 2);
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
    return file->failEvent && file->count >= file->failEvent ? -1 : 0;
}

static int FileSize(void *context, uint64_t *size)
{
    (void)context;
    *size = CP_SAVE_FILE_SIZE;
    return 0;
}

static CpFile file = { &memory, ReadAt, WriteAt, Sync, FileSize };

static CpSaveResult Load(void)
{
    return CpSaveImage_LoadLayout(&image, &file, game->layout, CP_SAVE_MAGIC_GEN5);
}

/* ---- The model ------------------------------------------------------------ */

/* What the game holds after loading the card: 1 and its RAM (blocks with their
 * footers, CRC table and footer) and save count; 0 no save; 3 broken. The
 * model takes each block from the copy that is good for it, preferring main,
 * judging every block by one copy's CRC table: the newer good one first. */
static unsigned char modelRam[0x26000];
static uint32_t modelCount;
/* The copy each block, then the CRC table and footer, must be written to next. */
static unsigned char modelAreas[CP_SAVE_GEN5_BLOCKS_MAX + 2];

static int FooterState(const unsigned char *copy)
{
    const unsigned char *f = copy + game->used - 16;
    if (R32(f + 4) != game->used || R32(f + 8) != CP_SAVE_MAGIC_GEN5) return 2;
    return R16(f + 14) == Crc(copy + Table(), TableSize()) ? 0 : 1;
}

static int BlockGood(const unsigned char *copy, unsigned id, unsigned want)
{
    const unsigned char *f = copy + offsets[id] + Size(id);
    return R16(f + 2) == want && Crc(copy + offsets[id], Size(id)) == want;
}

static int ModelLoad(void)
{
    const unsigned char *copy[2] = { Copy(MAIN), Copy(BACKUP) };
    int state[2] = { FooterState(copy[0]), FooterState(copy[1]) }, order[2], tries, id;
    uint32_t counts[2] = { R32(copy[0] + game->used - 16), R32(copy[1] + game->used - 16) };
    const unsigned char *tables[2];
    if (state[0] && state[1]) return state[0] == 1 || state[1] == 1 ? 3 : 0;
    /* A copy with a bad footer is judged by the other's table and footer. */
    tables[0] = state[0] ? copy[1] : copy[0];
    tables[1] = state[1] ? copy[0] : copy[1];
    if (!state[0] && (state[1] || counts[0] >= counts[1] || (counts[0] == 0 && counts[1] == UINT32_MAX))) {
        order[0] = 0; order[1] = 1;
    } else {
        order[0] = 1; order[1] = 0;
    }
    for (tries = 0; tries < 2; ++tries) {
        const unsigned char *table = tables[order[tries]] + Table();
        const unsigned char *chosen = copy[order[tries]];
        int ok = 1;
        for (id = 0; id < (int)game->blocks && ok; ++id) {
            unsigned want = R16(table + 2 * id), end = offsets[id] + Size(id);
            int good[2] = { BlockGood(copy[0], (unsigned)id, want), BlockGood(copy[1], (unsigned)id, want) };
            int fromBackup;
            /* A block neither copy holds, or holds with the table's CRC, fails this table. */
            if (R16(copy[0] + end + 2) != want && R16(copy[1] + end + 2) != want) { ok = 0; break; }
            if (!good[0] && !good[1]) { ok = 0; break; }
            if (good[0] && good[1]) {
                /* Both good: their counts must match the count of the copy
                 * whose table is tried (its own block, whatever its footer). */
                unsigned c = R16(chosen + end);
                int mainOff = R16(copy[0] + end) != c, backupOff = R16(copy[1] + end) != c;
                if (mainOff && backupOff) { ok = 0; break; }
                fromBackup = mainOff;
                modelAreas[id] = (unsigned char)(mainOff ? MAIN : backupOff ? BACKUP : 0);
            } else {
                fromBackup = !good[0];
                modelAreas[id] = (unsigned char)(good[0] ? BACKUP : MAIN);
            }
            memcpy(modelRam + offsets[id], copy[fromBackup] + offsets[id], Size(id) + 4);
        }
        if (ok) {
            modelAreas[game->blocks] = modelAreas[game->blocks + 1] = (unsigned char)(order[tries] == 0 ? BACKUP : MAIN);
            memcpy(modelRam + Table(), tables[order[tries]] + Table(), TableSize() + 16);
            modelCount = counts[order[tries]];
            return 1;
        }
    }
    return 3;
}

/* The save control's part of the load: RAM as gameplay sees it. */
static int ModelGame(void)
{
    int result = ModelLoad();
    unsigned char *trainer = modelRam + offsets[game->trainer], *musical = modelRam + offsets[game->musical];
    if (result != 1) return result;
    if (trainer[0x2C]) return 0;
    if (R16(trainer + 0x30 + 12) == 0xC21E) {
        int loads = CpGen5Save_MusicalLoads(game->layout, &file, R32(trainer + game->extraCounts + 24));
        assert(loads >= 0);
        musical[0x29E] = (unsigned char)loads;
        if (!loads) W16(trainer + 0x30 + 12, 0);
    } else {
        musical[0x29E] = 0;
    }
    return 1;
}

static void Digest(const unsigned char *ram, unsigned char out[32])
{
    CpSaveSha256 hash;
    unsigned id;
    CpSaveSha256_Init(&hash);
    for (id = 0; id < game->blocks; ++id) CpSaveSha256_Update(&hash, ram + offsets[id], Size(id));
    CpSaveSha256_Finish(&hash, out);
}

/* The image loads exactly as the model: the same result, RAM and count. */
static void Agree(void)
{
    int model = ModelGame();
    CpSaveResult result = Load();
    unsigned id;
    if (model == 1) {
        assert(result == CP_SAVE_OK && image.globalCounter == modelCount);
        for (id = 0; id < game->blocks; ++id)
            assert(memcmp(CpSaveImage_Block(&image, id), modelRam + offsets[id], Size(id) + 4) == 0);
        assert(memcmp(image.bytes + Table(), modelRam + Table(), TableSize() + 16) == 0);
        assert(memcmp(image.areas, modelAreas, game->blocks + 2) == 0);
    } else {
        assert(result == (model == 0 && FooterState(Copy(MAIN)) == 2 && FooterState(Copy(BACKUP)) == 2
            ? CP_SAVE_INVALID : CP_SAVE_NEEDS_GAME_SAVE));
        assert(!image.loaded);
    }
}

/* ---- Selection ------------------------------------------------------------ */

/* Every combination of what can be wrong with each copy: footer (good, CRC,
 * size), save counts (equal, main newer, backup newer, wrap), and for two
 * blocks (box 1 and the party) a torn body, a newer block, or a count the
 * other copy does not have, in either copy. */
static void SelectionMatrix(void)
{
    static const unsigned blocks[2] = { 1, 0x1A };
    unsigned footers, counts, faults, cases = 0, loaded = 0;
    for (footers = 0; footers < 9; ++footers)
    for (counts = 0; counts < 4; ++counts)
    for (faults = 0; faults < 7 * 7; ++faults) {
        unsigned f = faults, b;
        uint32_t c[2] = { 20, 20 };
        if (counts == 1) c[0] = 21;
        if (counts == 2) c[1] = 21;
        if (counts == 3) { c[0] = 0; c[1] = UINT32_MAX; }
        Fixture(c[0]);
        SealFooter(BACKUP, c[1]);
        for (b = 0; b < 2; ++b, f /= 7) {
            unsigned id = blocks[b], kind = f % 7, area = kind <= 3 ? MAIN : BACKUP;
            unsigned char *body = Copy(area) + offsets[id];
            if (kind == 0) continue;
            switch ((kind - 1) % 3) {
            case 0: body[5] ^= 0x40; break;                                    /* torn body */
            case 1: body[5] ^= 0x40; SealBlock(area, id, R16(body + Size(id)) + 1); break; /* newer block */
            default: W16(body + Size(id), R16(body + Size(id)) + 1); break;  /* count only */
            }
            /* A newer block also needs its copy's CRC table and footer. */
            SealFooter(area, area == MAIN ? c[0] : c[1]);
        }
        if (footers % 3 == 1) Copy(MAIN)[game->used - 1] ^= 1;
        if (footers % 3 == 2) Copy(MAIN)[game->used - 12] ^= 1;
        if (footers / 3 == 1) Copy(BACKUP)[game->used - 1] ^= 1;
        if (footers / 3 == 2) Copy(BACKUP)[game->used - 12] ^= 1;
        Agree();
        loaded += image.loaded;
        ++cases;
    }
    printf("PASS %s selection: %u footer, count and block combinations load as the game's backup system (%u load)\n",
           game->name, cases, loaded);
}

/* ---- Save control ---------------------------------------------------------- */

static void SealMusical(uint32_t count)
{
    unsigned char *base = memory.bytes + game->musicalAddress, *f = base + game->musicalUsed - 16, *table = f - 4;
    unsigned i, crc;
    for (i = 0; i < game->musicalBlock; ++i) base[i] = (unsigned char)(i * 7);
    crc = Crc(base, game->musicalBlock);
    W16(base + game->musicalBlock, 1); W16(base + game->musicalBlock + 2, crc);
    W16(table, crc); W16(table + 2, 0);
    W32(f, count); W32(f + 4, game->musicalUsed); W32(f + 8, CP_SAVE_MAGIC_GEN5);
    W16(f + 14, Crc(table, 4));
}

static void SaveControl(void)
{
    unsigned char *trainer, *musical;
    unsigned area;
    /* An intro save loads as none. */
    Fixture(5);
    for (area = MAIN; area <= BACKUP; ++area) {
        Copy(area)[offsets[game->trainer] + 0x2C] = 1;
        SealBlock(area, game->trainer, 9);
        SealFooter(area, 5);
    }
    assert(Load() == CP_SAVE_NEEDS_GAME_SAVE && ModelGame() == 0);
    /* A distributed program flag left without its marker reads as none. */
    Fixture(5);
    for (area = MAIN; area <= BACKUP; ++area) {
        Copy(area)[offsets[game->musical] + 0x29E] = 1;
        SealBlock(area, game->musical, 9);
        SealFooter(area, 5);
    }
    Agree();
    assert(CpSaveImage_Block(&image, game->musical)[0x29E] == 0);
    /* The marker with a musical that loads with the recorded count, then
     * with a different count, then with a broken block. */
    for (area = 0; area < 3; ++area) {
        unsigned where;
        Fixture(5);
        SealMusical(area == 1 ? 77 : 76);
        if (area == 2) memory.bytes[game->musicalAddress + 0x1234] ^= 1;
        for (where = MAIN; where <= BACKUP; ++where) {
            trainer = Copy(where) + offsets[game->trainer];
            W16(trainer + 0x30 + 12, 0xC21E);
            W32(trainer + game->extraCounts + 24, 76);
            SealBlock(where, game->trainer, 9);
            SealFooter(where, 5);
        }
        assert(CpGen5Save_MusicalLoads(game->layout, &file, 76) == (area == 0));
        Agree();
        trainer = CpSaveImage_Block(&image, game->trainer);
        musical = CpSaveImage_Block(&image, game->musical);
        assert(musical[0x29E] == (area == 0));
        assert(R16(trainer + 0x30 + 12) == (area == 0 ? 0xC21Eu : 0u));
    }
    printf("PASS %s save control: intro saves need a game save; the musical flag and marker follow the extra save\n",
           game->name);
}

/* ---- Saving -------------------------------------------------------------- */

static void HashAndVerify(void)
{
    unsigned char digest[32], before[32];
    Fixture(9);
    assert(Load() == CP_SAVE_OK && image.globalCounter == 9);
    assert(CpSaveImage_Hash(&image, before) == CP_SAVE_OK);
    assert(CpSaveImage_Verify(&image, &file, before) == CP_SAVE_OK);
    /* A byte of either copy's block, footer, CRC table or save footer. */
    {
        static const unsigned where[4] = { 0x1A, 0, 1, 2 };
        unsigned area, k;
        for (area = MAIN; area <= BACKUP; ++area) for (k = 0; k < 4; ++k) {
            unsigned at = k == 0 ? offsets[where[k]] + 9 : k == 1 ? offsets[0x1A] + Size(0x1A)
                : k == 2 ? Table() + 4 : game->used - 16;
            Copy(area)[at] ^= 1;
            assert(CpSaveImage_Verify(&image, &file, before) == CP_SAVE_VERIFY_FAILED);
            Copy(area)[at] ^= 1;
        }
    }
    assert(CpSaveImage_Verify(&image, &file, before) == CP_SAVE_OK);
    /* An unchanged image writes nothing new but the CRC table and footers. */
    CpSaveImage_Block(&image, game->party)[0x20] ^= 1;
    assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
    assert(CpSaveImage_CommitBoth(&image, &file, before) == CP_SAVE_VERIFY_FAILED);
    assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_OK);
    /* Two passes, each synced write by write: the party with its footer, the
     * CRC table, the footer but its last byte, the last byte. A load from the
     * main copy left its CRC table and footer to the backup: the first pass
     * writes the changed block to main and the table and footer to the
     * backup, the second the other way round. */
    assert(memory.count == 2 * 2 * (1 + 1 + 2));
    {
        unsigned pass, k;
        for (pass = 0; pass < 2; ++pass) {
            unsigned base = pass ? game->backup : 0, other = pass ? 0 : game->backup;
            const Event *e = memory.events + pass * 8;
            const unsigned want[4][2] = { { base + offsets[game->party], Size(game->party) + 4 },
                { other + Table(), TableSize() }, { other + game->used - 16, 15 }, { other + game->used - 1, 1 } };
            for (k = 0; k < 4; ++k)
                assert(e[2 * k].offset == want[k][0] && e[2 * k].size == want[k][1] && e[2 * k + 1].size == 0);
        }
    }
    assert(image.globalCounter == 10 && R32(Copy(MAIN) + game->used - 16) == 10
        && R32(Copy(BACKUP) + game->used - 16) == 10);
    assert(memcmp(Copy(MAIN), Copy(BACKUP), game->used) == 0);
    assert(R16(Copy(MAIN) + offsets[game->party] + Size(game->party)) == 7 + game->party + 1u);
    assert(R16(Copy(MAIN) + offsets[1] + Size(1)) == 8);
    Agree();
    assert(CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK);
    printf("PASS %s hash and verify: either copy's blocks, footers, CRC table and footer are checked; a save writes only the changed block\n",
           game->name);
}

/* Every write and sync of a save, cut whole or half written, from four
 * starting cards: clean, a backup behind by one block, a main copy whose
 * footer is torn, and counts wrapping. The card always loads the old or the
 * new save, never a mix, and the companion resumes from it. */
static void Interruptions(void)
{
    unsigned fixture, partial, fail, events, stops = 0;
    for (fixture = 0; fixture < 4; ++fixture) {
        unsigned char start[CP_SAVE_FILE_SIZE];
        Fixture(fixture == 3 ? UINT32_MAX : 30);
        if (fixture == 1) {
            /* The last save was cut in the backup copy's pass: box 3 is stale there. */
            Copy(MAIN)[offsets[3] + 4] ^= 0x55;
            SealBlock(MAIN, 3, 11);
            SealFooter(MAIN, 31);
        } else if (fixture == 2) {
            Copy(BACKUP)[offsets[3] + 4] ^= 0x55;
            SealBlock(BACKUP, 3, 11);
            Copy(BACKUP)[game->used - 1] ^= 0x80;
        }
        memcpy(start, memory.bytes, sizeof(start));
        /* Count the events of an uncut save. */
        assert(Load() == CP_SAVE_OK);
        {
            unsigned char digest[32];
            CpSaveImage_Block(&image, game->party)[0x30] ^= 1;
            CpSaveImage_Block(&image, 5)[0x40] ^= 1;
            assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
            memory.count = 0;
            assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_OK);
            events = memory.count;
        }
        for (partial = 0; partial < 2; ++partial)
        for (fail = 1; fail <= events; ++fail) {
            unsigned char oldDigest[32], digest[32], now[32];
            memcpy(memory.bytes, start, sizeof(start));
            memory.count = memory.failEvent = 0;
            assert(Load() == CP_SAVE_OK);
            assert(CpSaveImage_Hash(&image, oldDigest) == CP_SAVE_OK);
            CpSaveImage_Block(&image, game->party)[0x30] ^= 1;
            CpSaveImage_Block(&image, 5)[0x40] ^= 1;
            assert(CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
            memory.failEvent = fail; memory.partialWrite = (int)partial;
            assert(CpSaveImage_CommitBoth(&image, &file, digest) == CP_SAVE_IO_ERROR && !image.loaded);
            memory.failEvent = 0;
            /* What the game loads now: the old or the new save. */
            assert(ModelGame() == 1);
            Digest(modelRam, now);
            assert(memcmp(now, oldDigest, 32) == 0 || memcmp(now, digest, 32) == 0);
            Agree();
            /* The companion resumes: a checkpoint leaves both copies that save. */
            memory.count = 0;
            assert(CpSaveImage_Verify(&image, &file, now) == CP_SAVE_OK
                || CpSaveImage_CommitBoth(&image, &file, now) == CP_SAVE_OK);
            assert(memcmp(Copy(MAIN), Copy(BACKUP), game->used) == 0);
            Agree();
            assert(CpSaveImage_Verify(&image, &file, now) == CP_SAVE_OK);
            ++stops;
        }
    }
    printf("PASS %s interruptions: %u cut saves (every write and sync, whole or half written; clean, stale backup, torn footer, wrapping counts) load old or new and resume\n",
           game->name, stops);
}

/* Retail saves: they load as the model, and their copies verify. */
static void Retail(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i) {
        FILE *in = fopen(argv[i], "rb");
        unsigned char digest[32];
        assert(in && fread(memory.bytes, 1, sizeof(memory.bytes), in) == sizeof(memory.bytes));
        fclose(in);
        Agree();
        assert(image.loaded && CpSaveImage_Hash(&image, digest) == CP_SAVE_OK);
        printf("retail %s: save count %lu, %s\n", argv[i], (unsigned long)image.globalCounter,
               CpSaveImage_Verify(&image, &file, digest) == CP_SAVE_OK ? "both copies hold it" : "copies differ");
    }
}

int main(int argc, char **argv)
{
    Layout();
    if (argc > 1) {
        Retail(argc, argv);
        return 0;
    }
    SelectionMatrix();
    SaveControl();
    HashAndVerify();
    Interruptions();
    return 0;
}
