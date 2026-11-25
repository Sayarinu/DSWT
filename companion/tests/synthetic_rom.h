#ifndef COMPANION_TEST_SYNTHETIC_ROM_H
#define COMPANION_TEST_SYNTHETIC_ROM_H

/* Small synthetic ROMs for the game data and session model tests: NitroFS,
 * NARCs, encrypted message banks and a synthetic Platinum. Every value in
 * them is made up; no game data is here. */
#include "game/gamedata.h"
#include "game/text.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ---- A growable byte buffer --------------------------------------------- */

typedef struct Bytes {
    uint8_t *data;
    uint32_t size, capacity;
} Bytes;

static inline void Reserve(Bytes *b, uint32_t size)
{
    if (size <= b->capacity) return;
    b->capacity = size * 2 + 64;
    b->data = realloc(b->data, b->capacity);
    assert(b->data);
}

static inline void Put(Bytes *b, const void *data, uint32_t size)
{
    if (size == 0) return;
    Reserve(b, b->size + size);
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static inline void Put8(Bytes *b, unsigned v) { uint8_t x = (uint8_t)v; Put(b, &x, 1); }
static inline void Put16(Bytes *b, unsigned v) { Put8(b, v & 0xFF); Put8(b, v >> 8 & 0xFF); }
static inline void Put32(Bytes *b, uint32_t v) { Put16(b, v & 0xFFFF); Put16(b, v >> 16); }
static inline void Align(Bytes *b, uint32_t to) { while (b->size % to) Put8(b, 0xFF); }

static inline void Set32(Bytes *b, uint32_t at, uint32_t v)
{
    b->data[at] = (uint8_t)v;
    b->data[at + 1] = (uint8_t)(v >> 8);
    b->data[at + 2] = (uint8_t)(v >> 16);
    b->data[at + 3] = (uint8_t)(v >> 24);
}

/* ---- An in-memory reader that records how much each read asks for ------- */

typedef struct MemoryReader {
    CpReader reader;
    const Bytes *bytes;
    uint32_t largestRead, reads;
} MemoryReader;

static inline int MemoryReadAt(void *context, uint32_t offset, void *destination, uint32_t size)
{
    MemoryReader *self = context;

    if (offset > self->bytes->size || size > self->bytes->size - offset) return -1;
    memcpy(destination, self->bytes->data + offset, size);
    if (size > self->largestRead) self->largestRead = size;
    ++self->reads;
    return 0;
}

static inline uint32_t MemorySize(void *context)
{
    return ((MemoryReader *)context)->bytes->size;
}

static inline void MemoryReader_Init(MemoryReader *self, const Bytes *bytes)
{
    memset(self, 0, sizeof *self);
    self->reader.context = self;
    self->reader.readAt = MemoryReadAt;
    self->reader.size = MemorySize;
    self->bytes = bytes;
}

/* ---- NARC builder -------------------------------------------------------- */

static inline Bytes Narc(const Bytes *members, uint32_t count)
{
    Bytes narc = { 0 }, data = { 0 };
    uint32_t i, *starts = calloc(count, sizeof *starts);

    for (i = 0; i < count; ++i) {
        starts[i] = data.size;
        Put(&data, members[i].data, members[i].size);
        Align(&data, 4);
    }
    Put(&narc, "NARC", 4);
    Put16(&narc, 0xFFFE);
    Put16(&narc, 0x0100);
    Put32(&narc, 0); /* the total size, set below */
    Put16(&narc, 0x10);
    Put16(&narc, 3);
    Put(&narc, "BTAF", 4);
    Put32(&narc, 12 + count * 8);
    Put16(&narc, count);
    Put16(&narc, 0);
    for (i = 0; i < count; ++i) {
        Put32(&narc, starts[i]);
        Put32(&narc, starts[i] + members[i].size);
    }
    Put(&narc, "BTNF", 4);
    Put32(&narc, 16);
    Put32(&narc, 4);
    Put16(&narc, 0);
    Put16(&narc, 1);
    Put(&narc, "GMIF", 4);
    Put32(&narc, 8 + data.size);
    Put(&narc, data.data, data.size);
    Set32(&narc, 8, narc.size);
    free(starts);
    free(data.data);
    return narc;
}

/* ---- NitroFS ROM builder ------------------------------------------------- */

typedef struct RomFile {
    const char *path;
    Bytes data;
} RomFile;

#define MAX_DIRS 16

typedef struct Dir {
    char name[32];
    int parent;
} Dir;

static inline int DirIndex(Dir *dirs, int *count, int parent, const char *name, size_t length)
{
    int i;

    for (i = 1; i < *count; ++i)
        if (dirs[i].parent == parent && strlen(dirs[i].name) == length && memcmp(dirs[i].name, name, length) == 0)
            return i;
    assert(*count < MAX_DIRS && length < sizeof dirs[0].name);
    memcpy(dirs[*count].name, name, length);
    dirs[*count].name[length] = '\0';
    dirs[*count].parent = parent;
    return (*count)++;
}

/* The directory of a path, creating its folders; `leaf` is the file name. */
static inline int PathDir(Dir *dirs, int *count, const char *path, const char **leaf)
{
    int dir = 0;
    const char *slash;

    while ((slash = strchr(path, '/'))) {
        dir = DirIndex(dirs, count, dir, path, (size_t)(slash - path));
        path = slash + 1;
    }
    *leaf = path;
    return dir;
}

/* A ROM with `code`, `revision` and the files, in NitroFS. Within each
 * directory files come first, then subdirectories, as the format requires
 * consecutive file IDs per directory. */
static inline Bytes Rom(const char code[4], uint8_t revision, const RomFile *files, int fileCount)
{
    Dir dirs[MAX_DIRS];
    int dirCount = 1, d, f, i, *fileDir = calloc((size_t)fileCount, sizeof(int)), *order = calloc((size_t)fileCount, sizeof(int));
    int ordered = 0;
    uint16_t firstFile[MAX_DIRS];
    Bytes rom = { 0 }, fnt = { 0 }, sub = { 0 };
    uint32_t fntAt, fatAt, dataAt;
    const char *leaf;

    memset(dirs, 0, sizeof dirs);
    for (f = 0; f < fileCount; ++f) fileDir[f] = PathDir(dirs, &dirCount, files[f].path, &leaf);
    for (d = 0; d < dirCount; ++d) {
        firstFile[d] = (uint16_t)ordered;
        for (f = 0; f < fileCount; ++f)
            if (fileDir[f] == d) order[ordered++] = f;
    }

    /* The FNT: the main table, then each directory's entries. */
    for (d = 0; d < dirCount; ++d) {
        uint32_t at = sub.size;

        for (i = 0; i < ordered; ++i) {
            if (fileDir[order[i]] != d) continue;
            PathDir(dirs, &dirCount, files[order[i]].path, &leaf);
            Put8(&sub, (unsigned)strlen(leaf));
            Put(&sub, leaf, (uint32_t)strlen(leaf));
        }
        for (i = 1; i < dirCount; ++i) {
            if (dirs[i].parent != d) continue;
            Put8(&sub, 0x80 | (unsigned)strlen(dirs[i].name));
            Put(&sub, dirs[i].name, (uint32_t)strlen(dirs[i].name));
            Put16(&sub, 0xF000 | (unsigned)i);
        }
        Put8(&sub, 0);
        Put32(&fnt, (uint32_t)dirCount * 8 + at);
        Put16(&fnt, firstFile[d]);
        Put16(&fnt, d == 0 ? (unsigned)dirCount : 0xF000 | (unsigned)dirs[d].parent);
    }
    Put(&fnt, sub.data, sub.size);

    Reserve(&rom, 0x200);
    memset(rom.data, 0, 0x200);
    rom.size = 0x200;
    memcpy(rom.data + 0x0C, code, 4);
    rom.data[0x1E] = revision;
    fntAt = rom.size;
    Put(&rom, fnt.data, fnt.size);
    Align(&rom, 4);
    fatAt = rom.size;
    for (i = 0; i < ordered; ++i) { Put32(&rom, 0); Put32(&rom, 0); }
    for (i = 0; i < ordered; ++i) {
        Align(&rom, 0x200);
        dataAt = rom.size;
        Put(&rom, files[order[i]].data.data, files[order[i]].data.size);
        Set32(&rom, fatAt + (uint32_t)i * 8, dataAt);
        Set32(&rom, fatAt + (uint32_t)i * 8 + 4, rom.size);
    }
    Set32(&rom, 0x40, fntAt);
    Set32(&rom, 0x44, fnt.size);
    Set32(&rom, 0x48, fatAt);
    Set32(&rom, 0x4C, (uint32_t)ordered * 8);
    free(fnt.data);
    free(sub.data);
    free(fileDir);
    free(order);
    return rom;
}

/* ---- Message bank encoder: the game's cipher, run forwards -------------- */

static inline Bytes MessageBank(uint16_t seed, const uint16_t *const *texts, uint16_t count)
{
    Bytes bank = { 0 };
    uint32_t i, j, offset = 4 + (uint32_t)count * 8;

    Put16(&bank, count);
    Put16(&bank, seed);
    for (i = 0; i < count; ++i) {
        uint32_t length = 0, key = (uint32_t)seed * 765u * (i + 1) & 0xFFFF;

        while (texts[i][length] != CP_CHAR_EOS) ++length;
        ++length; /* the end code is stored, encrypted, too */
        key |= key << 16;
        Put32(&bank, offset ^ key);
        Put32(&bank, length ^ key);
        offset += length * 2;
    }
    for (i = 0; i < count; ++i) {
        uint16_t key = (uint16_t)((i + 1) * 596947u);

        for (j = 0;; ++j) {
            Put16(&bank, texts[i][j] ^ key);
            key = (uint16_t)(key + 18749u);
            if (texts[i][j] == CP_CHAR_EOS) break;
        }
    }
    return bank;
}

/* ---- A synthetic Platinum ------------------------------------------------ */

static inline Bytes Filled(uint32_t size, uint8_t value)
{
    Bytes b = { 0 };

    Reserve(&b, size + 1);
    memset(b.data, value, size);
    b.size = size;
    return b;
}

static inline void FreeMembers(Bytes *members, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; ++i) free(members[i].data);
}

#define PERSONAL_COUNT 508
#define ITEM_RECORDS 446
#define BANKS 413

static const uint16_t kWesternName[] = { 0x012B + 15, 0x0145 + 8, 0x0145 + 10, 0x0145, CP_CHAR_EOS }; /* "Pika" */
static const uint16_t kAsianName[] = { 0x0010, 0x0020, CP_CHAR_EOS };
static const uint16_t kOther[] = { 0x0130, CP_CHAR_EOS };
static const uint16_t kEmpty[] = { CP_CHAR_EOS };

static inline Bytes SyntheticPlatinum(const char code[4], uint8_t revision)
{
    Bytes personal[PERSONAL_COUNT], growth[8], evo[100], items[ITEM_RECORDS], banks[BANKS], fonts[2], moves[40];
    const uint16_t *western[30], *asian[30];
    RomFile files[9];
    Bytes rom, font = { 0 };
    uint32_t i, level;

    for (i = 0; i < PERSONAL_COUNT; ++i) {
        personal[i] = Filled(44, 0);
        memset(personal[i].data, (int)(i & 0xFF), 6); /* base stats */
        personal[i].data[16] = (uint8_t)(i % 256);    /* gender ratio */
        personal[i].data[18] = 70;                    /* base friendship */
        personal[i].data[19] = (uint8_t)(i % 6);      /* growth rate */
        personal[i].data[22] = (uint8_t)(i + 1);      /* abilities */
        personal[i].data[23] = (uint8_t)(i + 2);
    }
    for (i = 0; i < 8; ++i) {
        growth[i] = (Bytes){ 0 };
        for (level = 0; level <= 100; ++level) Put32(&growth[i], level * level * (i + 1));
    }
    for (i = 0; i < 100; ++i) evo[i] = Filled(44, 0);
    /* Species 64: a level evolution, then a plain trade evolution to 65. */
    evo[64].data[0] = 4; evo[64].data[2] = 30; evo[64].data[4] = 99;
    evo[64].data[6] = CP_EVO_TRADE; evo[64].data[10] = 65;
    /* Species 95: trade holding item 233 to 208, holding item 210 to 77. */
    evo[95].data[0] = CP_EVO_TRADE_WITH_HELD_ITEM; evo[95].data[2] = 233; evo[95].data[4] = 208;
    evo[95].data[6] = CP_EVO_TRADE_WITH_HELD_ITEM; evo[95].data[8] = 210; evo[95].data[10] = 77;
    for (i = 0; i < ITEM_RECORDS; ++i) {
        /* Record 113 (item 135) is a key item and record 398 (item 420) an HM;
         * both refuse tossing, as does record 2 in an ordinary pocket. */
        unsigned pocket = i == 113 ? 7 : i == 398 ? 3 : i % 7;
        unsigned preventToss = i == 113 || i == 398 || i == 2;

        items[i] = Filled(36, 0);
        items[i].data[0] = (uint8_t)(i * 10);
        items[i].data[1] = (uint8_t)(i * 10 >> 8);
        items[i].data[2] = (uint8_t)i;
        items[i].data[8] = (uint8_t)((pocket << 7 | preventToss << 5) & 0xFF);
        items[i].data[9] = (uint8_t)(pocket >> 1);
    }
    for (i = 0; i < 30; ++i) western[i] = asian[i] = i == 0 ? kEmpty : kOther;
    western[25] = kWesternName;
    asian[25] = kAsianName;
    for (i = 0; i < BANKS; ++i) banks[i] = Filled(4, 0);
    free(banks[412].data);
    banks[412] = MessageBank(0x1234, western, 30);
    free(banks[408].data);
    banks[408] = MessageBank(0x4321, asian, 30);

    /* A font of three 16x16 glyphs. Glyph 2 (code 3) has its top-left pixel
     * set to 1 and a 2 at (12, 9). */
    Put32(&font, 16);
    Put32(&font, 16 + 3 * 64);
    Put32(&font, 3);
    Put8(&font, 12); Put8(&font, 16); Put8(&font, 2); Put8(&font, 2);
    for (i = 0; i < 3 * 64; ++i) Put8(&font, 0);
    font.data[16 + 2 * 64 + 1] = 0x40;                   /* tile 0, row 0, left half */
    font.data[16 + 2 * 64 + 48 + 1 * 2 + 0] = 0x80;      /* tile 3, row 1, right half */
    Put8(&font, 5); Put8(&font, 6); Put8(&font, 7);
    fonts[0] = font;
    fonts[1] = Filled(4, 0);
    /* Move records of 16 bytes: move n has n PP. */
    for (i = 0; i < 40; ++i) {
        moves[i] = Filled(16, 0xEE);
        moves[i].data[6] = (uint8_t)i;
    }

    files[0] = (RomFile){ "poketool/personal/pl_personal.narc", Narc(personal, PERSONAL_COUNT) };
    files[1] = (RomFile){ "poketool/personal/pl_growtbl.narc", Narc(growth, 8) };
    files[2] = (RomFile){ "poketool/personal/evo.narc", Narc(evo, 100) };
    files[3] = (RomFile){ "itemtool/itemdata/pl_item_data.narc", Narc(items, ITEM_RECORDS) };
    files[4] = (RomFile){ "msgdata/pl_msg.narc", Narc(banks, BANKS) };
    files[5] = (RomFile){ "graphic/pl_font.narc", Narc(fonts, 2) };
    files[6] = (RomFile){ "poketool/personal/readme.txt", Filled(3, 'x') };
    files[7] = (RomFile){ "a.bin", Filled(5, 0xAB) };
    files[8] = (RomFile){ "poketool/waza/pl_waza_tbl.narc", Narc(moves, 40) };
    rom = Rom(code, revision, files, 9);

    FreeMembers(personal, PERSONAL_COUNT);
    FreeMembers(growth, 8);
    FreeMembers(evo, 100);
    FreeMembers(items, ITEM_RECORDS);
    FreeMembers(banks, BANKS);
    FreeMembers(fonts, 2);
    FreeMembers(moves, 40);
    for (i = 0; i < 9; ++i) free(files[i].data.data);
    return rom;
}

#endif
