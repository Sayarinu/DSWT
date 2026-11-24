/* The ROM reader against small synthetic ROMs built here. Every value in them
 * is made up; no game data is in this test. */
#include "game/gamedata.h"
#include "game/nitrofs.h"
#include "game/pairing.h"
#include "game/rom.h"
#include "game/text.h"
#include "posix_reader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- A growable byte buffer --------------------------------------------- */

typedef struct Bytes {
    uint8_t *data;
    uint32_t size, capacity;
} Bytes;

static void Reserve(Bytes *b, uint32_t size)
{
    if (size <= b->capacity) return;
    b->capacity = size * 2 + 64;
    b->data = realloc(b->data, b->capacity);
    assert(b->data);
}

static void Put(Bytes *b, const void *data, uint32_t size)
{
    if (size == 0) return;
    Reserve(b, b->size + size);
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void Put8(Bytes *b, unsigned v) { uint8_t x = (uint8_t)v; Put(b, &x, 1); }
static void Put16(Bytes *b, unsigned v) { Put8(b, v & 0xFF); Put8(b, v >> 8 & 0xFF); }
static void Put32(Bytes *b, uint32_t v) { Put16(b, v & 0xFFFF); Put16(b, v >> 16); }
static void Align(Bytes *b, uint32_t to) { while (b->size % to) Put8(b, 0xFF); }

static void Set32(Bytes *b, uint32_t at, uint32_t v)
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

static int MemoryReadAt(void *context, uint32_t offset, void *destination, uint32_t size)
{
    MemoryReader *self = context;

    if (offset > self->bytes->size || size > self->bytes->size - offset) return -1;
    memcpy(destination, self->bytes->data + offset, size);
    if (size > self->largestRead) self->largestRead = size;
    ++self->reads;
    return 0;
}

static uint32_t MemorySize(void *context)
{
    return ((MemoryReader *)context)->bytes->size;
}

static void MemoryReader_Init(MemoryReader *self, const Bytes *bytes)
{
    memset(self, 0, sizeof *self);
    self->reader.context = self;
    self->reader.readAt = MemoryReadAt;
    self->reader.size = MemorySize;
    self->bytes = bytes;
}

/* ---- NARC builder -------------------------------------------------------- */

static Bytes Narc(const Bytes *members, uint32_t count)
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

static int DirIndex(Dir *dirs, int *count, int parent, const char *name, size_t length)
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
static int PathDir(Dir *dirs, int *count, const char *path, const char **leaf)
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
static Bytes Rom(const char code[4], uint8_t revision, const RomFile *files, int fileCount)
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

static Bytes MessageBank(uint16_t seed, const uint16_t *const *texts, uint16_t count)
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

static Bytes Filled(uint32_t size, uint8_t value)
{
    Bytes b = { 0 };

    Reserve(&b, size + 1);
    memset(b.data, value, size);
    b.size = size;
    return b;
}

static void FreeMembers(Bytes *members, uint32_t count)
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

static Bytes SyntheticPlatinum(const char code[4], uint8_t revision)
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

/* ---- Tests --------------------------------------------------------------- */

static void TestBuildCodes(void)
{
    CpRomInfo info;
    static const struct { const char *code; uint8_t revision; uint32_t build; } cases[] = {
        { "CPUE", 1, 0x020C0201 }, { "CPUE", 10, 0x020C020A }, { "CPUJ", 0, 0x020C0100 },
        { "CPUF", 0, 0x020C0300 }, { "CPUI", 0, 0x020C0400 }, { "CPUD", 0, 0x020C0500 },
        { "CPUS", 0, 0x020C0700 }, { "CPUK", 0, 0x020C0800 }, { "ADAE", 5, 0x010A0205 },
        { "APAJ", 6, 0x010B0106 }, { "IPKE", 0, 0x03070200 }, { "IPGK", 0, 0x03080800 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        assert(CpRom_FromCode(cases[i].code, cases[i].revision, &info) == CP_OK);
        assert(info.buildCode == cases[i].build);
        assert(strcmp(info.gameCode, cases[i].code) == 0 && info.revision == cases[i].revision);
        assert(CpBuildCode_Family(info.buildCode) == info.family);
    }
    assert(CpRom_FromCode("CPUP", 0, &info) == CP_ERR_UNSUPPORTED);
    assert(CpRom_FromCode("IRBO", 0, &info) == CP_ERR_UNSUPPORTED);
    assert(CpRom_FromCode("XXXE", 0, &info) == CP_ERR_UNSUPPORTED);

    /* As the server reads codes: the legacy alias is Platinum; language 6,
     * unknown versions and Generation V are not trade families here. */
    assert(CpBuildCode_Family(CP_BUILD_CODE_LEGACY_PLATINUM) == CP_FAMILY_PLATINUM);
    assert(CpBuildCode_Family(0x020C0600) == CP_FAMILY_NONE);
    assert(CpBuildCode_Family(0x020D0200) == CP_FAMILY_NONE);
    assert(CpBuildCode_Family(0x02070200) == CP_FAMILY_NONE);
    assert(CpBuildCode_Family(0x04140200) == CP_FAMILY_NONE);
    assert(CpBuildCode(CP_FAMILY_HGSS, 8, 5, 0x10) == 0x03080510);
}

static void TestSaveMatch(void)
{
    CpRomInfo pt, ko, dp, hgss;
    CpSaveFingerprint save;

    assert(CpRom_FromCode("CPUE", 0, &pt) == CP_OK && CpRom_FromCode("CPUK", 0, &ko) == CP_OK);
    assert(CpRom_FromCode("ADAE", 5, &dp) == CP_OK && CpRom_FromCode("IPKE", 0, &hgss) == CP_OK);
    save = (CpSaveFingerprint){ 0xCF2C, CP_SAVE_MAGIC_INTERNATIONAL };
    assert(CpRom_MatchesSave(&pt, &save) && !CpRom_MatchesSave(&ko, &save));
    assert(!CpRom_MatchesSave(&dp, &save) && !CpRom_MatchesSave(&hgss, &save));
    save.footerMagic = CP_SAVE_MAGIC_KOREAN;
    assert(!CpRom_MatchesSave(&pt, &save) && CpRom_MatchesSave(&ko, &save));
    save = (CpSaveFingerprint){ 0xC100, CP_SAVE_MAGIC_INTERNATIONAL };
    assert(CpRom_MatchesSave(&dp, &save) && !CpRom_MatchesSave(&pt, &save));
    save = (CpSaveFingerprint){ 0xF628, CP_SAVE_MAGIC_INTERNATIONAL };
    assert(CpRom_MatchesSave(&hgss, &save) && !CpRom_MatchesSave(&dp, &save));
}

static void TestNitroFsAndNarc(void)
{
    Bytes members[3] = { Filled(3, 1), Filled(0, 0), Filled(9, 3) };
    RomFile files[3];
    Bytes rom;
    MemoryReader memory;
    CpRomFile file, member;
    CpNarc narc;
    uint8_t buffer[16];

    files[0] = (RomFile){ "top.bin", Filled(7, 0x11) };
    files[1] = (RomFile){ "dir/sub/deep.narc", Narc(members, 3) };
    files[2] = (RomFile){ "dir/other.bin", Filled(2, 0x22) };
    rom = Rom("CPUE", 0, files, 3);
    MemoryReader_Init(&memory, &rom);

    assert(CpNitroFs_Open(&memory.reader, "top.bin", &file) == CP_OK && file.size == 7);
    assert(CpRomFile_Read(&file, 0, buffer, 7) == CP_OK && buffer[0] == 0x11 && buffer[6] == 0x11);
    assert(CpRomFile_Read(&file, 1, buffer, 7) == CP_ERR_IO); /* past the file's end */
    assert(CpNitroFs_Open(&memory.reader, "dir/other.bin", &file) == CP_OK && file.size == 2);
    assert(CpNitroFs_Open(&memory.reader, "dir/sub/deep.narc", &file) == CP_OK);
    assert(CpNitroFs_Open(&memory.reader, "dir/missing.bin", &file) == CP_ERR_NOT_FOUND);
    assert(CpNitroFs_Open(&memory.reader, "dir", &file) == CP_ERR_NOT_FOUND);         /* a directory */
    assert(CpNitroFs_Open(&memory.reader, "top.bin/x", &file) == CP_ERR_NOT_FOUND);   /* a file as a folder */
    assert(CpNitroFs_Open(&memory.reader, "dir//other.bin", &file) == CP_ERR_NOT_FOUND);
    assert(CpNitroFs_Open(&memory.reader, "DIR/other.bin", &file) == CP_ERR_NOT_FOUND); /* case-sensitive */

    assert(CpNarc_OpenPath(&memory.reader, "dir/sub/deep.narc", &narc) == CP_OK && narc.count == 3);
    assert(CpNarc_Member(&narc, 0, &member) == CP_OK && member.size == 3);
    assert(CpRomFile_Read(&member, 0, buffer, 3) == CP_OK && buffer[2] == 1);
    assert(CpNarc_Member(&narc, 1, &member) == CP_OK && member.size == 0);
    assert(CpNarc_Member(&narc, 2, &member) == CP_OK && member.size == 9);
    assert(CpRomFile_Read(&member, 8, buffer, 1) == CP_OK && buffer[0] == 3);
    assert(CpNarc_Member(&narc, 3, &member) == CP_ERR_NOT_FOUND);
    assert(CpNarc_OpenPath(&memory.reader, "top.bin", &narc) == CP_ERR_FORMAT);

    /* A corrupt member table entry is refused, not read. */
    assert(CpNitroFs_Open(&memory.reader, "dir/sub/deep.narc", &file) == CP_OK);
    Set32(&rom, file.offset + 0x10 + 12 + 4, 0x7FFFFFFF);
    assert(CpNarc_OpenPath(&memory.reader, "dir/sub/deep.narc", &narc) == CP_OK);
    assert(CpNarc_Member(&narc, 0, &member) == CP_ERR_FORMAT);
    /* So is a truncated ROM. */
    rom.size = file.offset + 20;
    assert(CpNarc_OpenPath(&memory.reader, "dir/sub/deep.narc", &narc) != CP_OK);

    FreeMembers(members, 3);
    for (int i = 0; i < 3; ++i) free(files[i].data.data);
    free(rom.data);
}

static void TestPlatinumData(void)
{
    Bytes rom = SyntheticPlatinum("CPUE", 1);
    MemoryReader memory;
    CpGame game;
    CpPersonal personal;
    CpTradeEvolution evolution;
    CpItem item;
    uint32_t table[CP_MAX_LEVEL + 1];
    uint16_t name[8];
    uint8_t pp;
    size_t length;

    MemoryReader_Init(&memory, &rom);
    assert(CpGame_Open(&memory.reader, &game) == CP_OK);
    assert(game.rom.buildCode == 0x020C0201 && game.personal.count == PERSONAL_COUNT);

    assert(CpGame_Personal(&game, 25, 0, &personal) == CP_OK);
    assert(personal.baseStats[0] == 25 && personal.baseStats[5] == 25 && personal.genderRatio == 25);
    assert(personal.baseFriendship == 70 && personal.growthRate == 25 % 6);
    assert(personal.abilities[0] == 26 && personal.abilities[1] == 27);
    /* Forms with their own records; others share the species' record. */
    assert(CpGame_Personal(&game, 479, 2, &personal) == CP_OK && personal.baseStats[0] == (504 & 0xFF));
    assert(CpGame_Personal(&game, 386, 3, &personal) == CP_OK && personal.baseStats[0] == (498 & 0xFF));
    assert(CpGame_Personal(&game, 386, 4, &personal) == CP_OK && personal.baseStats[0] == (386 & 0xFF));
    assert(CpGame_Personal(&game, 201, 5, &personal) == CP_OK && personal.baseStats[0] == 201);
    assert(CpGame_Personal(&game, 0, 0, &personal) == CP_ERR_NOT_FOUND);
    assert(CpGame_Personal(&game, 600, 0, &personal) == CP_ERR_NOT_FOUND);

    assert(CpGame_ExpTable(&game, 2, table) == CP_OK && table[10] == 300 && table[100] == 30000);
    assert(CpExp_Level(table, 0) == 1 && CpExp_Level(table, 299) == 9 && CpExp_Level(table, 300) == 10);
    assert(CpExp_Level(table, 0xFFFFFFFF) == 100);
    assert(CpGame_ExpTable(&game, 8, table) == CP_ERR_NOT_FOUND);

    assert(CpGame_TradeEvolution(&game, 64, 0, &evolution) == CP_OK && evolution.target == 65 && evolution.item == 0);
    assert(CpGame_TradeEvolution(&game, 64, 17, &evolution) == CP_OK && evolution.target == 65);
    assert(CpGame_TradeEvolution(&game, 95, 0, &evolution) == CP_ERR_NOT_FOUND);
    assert(CpGame_TradeEvolution(&game, 95, 233, &evolution) == CP_OK && evolution.target == 208 && evolution.item == 233);
    assert(CpGame_TradeEvolution(&game, 95, 210, &evolution) == CP_OK && evolution.target == 77 && evolution.item == 210);
    assert(CpGame_TradeEvolution(&game, 10, 0, &evolution) == CP_ERR_NOT_FOUND);

    /* Items 113..134 have no record; later IDs shift down by 22. */
    assert(CpGame_Item(&game, 1, &item) == CP_OK && item.price == 10 && !item.preventToss && item.holdable);
    /* Holdable follows prevent-toss alone, whatever the pocket. */
    assert(CpGame_Item(&game, 2, &item) == CP_OK && item.preventToss && !item.holdable && item.fieldPocket == 2);
    assert(CpGame_Item(&game, 420, &item) == CP_OK && item.fieldPocket == 3 && item.preventToss && !item.holdable);
    assert(CpGame_Item(&game, 112, &item) == CP_OK && item.holdEffect == 112 && item.fieldPocket == 112 % 7);
    assert(CpGame_Item(&game, 113, &item) == CP_ERR_NOT_FOUND && CpGame_Item(&game, 134, &item) == CP_ERR_NOT_FOUND);
    assert(CpGame_Item(&game, 135, &item) == CP_OK && item.holdEffect == 113 && item.fieldPocket == 7);
    assert(!item.holdable && item.preventToss);
    assert(CpGame_Item(&game, 467, &item) == CP_OK && item.price == 445 * 10);
    assert(CpGame_Item(&game, 0, &item) == CP_ERR_NOT_FOUND && CpGame_Item(&game, 468, &item) == CP_ERR_NOT_FOUND);

    assert(CpGame_MovePp(&game, 33, &pp) == CP_OK && pp == 33);
    assert(CpGame_MovePp(&game, 0, &pp) == CP_ERR_NOT_FOUND && CpGame_MovePp(&game, 40, &pp) != CP_OK);

    assert(CpGame_SpeciesName(&game, 25, name, 8, &length) == CP_OK && length == 4);
    assert(memcmp(name, kWesternName, sizeof kWesternName) == 0);
    assert(CpGame_SpeciesName(&game, 25, name, 4, &length) == CP_ERR_RANGE);
    assert(CpGame_SpeciesName(&game, 0, name, 8, &length) == CP_OK && length == 0 && name[0] == CP_CHAR_EOS);
    assert(CpGame_SpeciesName(&game, 30, name, 8, &length) == CP_ERR_NOT_FOUND);

    /* Reads stay small: tables and records, never whole archives. */
    assert(memory.largestRead <= (CP_MAX_LEVEL + 1) * 4);
    free(rom.data);

    /* Japanese and Korean read species names from their own bank number. */
    rom = SyntheticPlatinum("CPUK", 0);
    MemoryReader_Init(&memory, &rom);
    assert(CpGame_Open(&memory.reader, &game) == CP_OK && game.rom.language == CP_LANGUAGE_KOREAN);
    assert(CpGame_SpeciesName(&game, 25, name, 8, &length) == CP_OK && length == 2);
    assert(memcmp(name, kAsianName, sizeof kAsianName) == 0);
    free(rom.data);

    /* A Diamond header is identified, but has no data table yet. */
    rom = SyntheticPlatinum("ADAE", 5);
    MemoryReader_Init(&memory, &rom);
    assert(CpGame_Open(&memory.reader, &game) == CP_ERR_UNSUPPORTED && game.rom.family == CP_FAMILY_DP);
    free(rom.data);
}

static void TestMessageCipher(void)
{
    /* Entry 1 of a bank with seed 0x1234 holding the codes 0x0001 0x0002 EOS:
     * key words worked out by hand from the cipher. */
    static const uint16_t first[] = { 0x0042, CP_CHAR_EOS }, second[] = { 0x0001, 0x0002, CP_CHAR_EOS };
    const uint16_t *texts[2] = { first, second };
    Bytes bank = MessageBank(0x1234, texts, 2);
    MemoryReader memory;
    CpRomFile file;
    uint16_t out[4];
    size_t length;
    uint32_t key = 0x1234u * 765u * 2u & 0xFFFF;

    assert(bank.size == 4 + 16 + 4 + 6);
    assert(CpLe32(bank.data + 12) == (24u ^ (key | key << 16))); /* entry 1 starts after entry 0's 2 codes */
    assert(CpLe16(bank.data + 24) == (0x0001 ^ (uint16_t)(2u * 596947u)));
    assert(CpLe16(bank.data + 26) == (0x0002 ^ (uint16_t)(2u * 596947u + 18749u)));
    MemoryReader_Init(&memory, &bank);
    CpRomFile_Whole(&memory.reader, &file);
    assert(CpMessage_Read(&file, 1, out, 4, &length) == CP_OK && length == 2);
    assert(out[0] == 1 && out[1] == 2 && out[2] == CP_CHAR_EOS);
    assert(CpMessage_Read(&file, 0, out, 4, NULL) == CP_OK && out[0] == 0x42 && out[1] == CP_CHAR_EOS);
    assert(CpMessage_Read(&file, 2, out, 4, &length) == CP_ERR_NOT_FOUND);
    assert(CpMessage_Read(&file, 1, out, 0, &length) == CP_ERR_RANGE);
    free(bank.data);
}

static void TestFont(void)
{
    Bytes rom = SyntheticPlatinum("CPUE", 0);
    MemoryReader memory;
    CpGame game;
    CpFont font;
    CpGlyph glyph;
    unsigned x, y, set = 0;

    MemoryReader_Init(&memory, &rom);
    assert(CpGame_Open(&memory.reader, &game) == CP_OK);
    assert(CpGame_Font(&game, CP_FONT_SYSTEM, &font) == CP_OK && font.glyphCount == 3);
    assert(CpGame_Font(&game, CP_FONT_MESSAGE, &font) == CP_ERR_FORMAT);
    assert(CpGame_Font(&game, CP_FONT_SYSTEM, &font) == CP_OK);
    assert(CpFont_Glyph(&font, 3, &glyph) == CP_OK && glyph.width == 7 && glyph.height == 16);
    assert(glyph.tilesWide == 2 && glyph.tilesHigh == 2);
    assert(CpGlyph_Pixel(&glyph, 0, 0) == 1 && CpGlyph_Pixel(&glyph, 12, 9) == 2);
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 16; ++x) set += CpGlyph_Pixel(&glyph, x, y) != 0;
    assert(set == 2);
    assert(CpGlyph_Pixel(&glyph, 16, 0) == 0);
    assert(CpFont_Glyph(&font, 1, &glyph) == CP_OK && glyph.width == 5);
    /* Codes past the font draw as '?', which this font lacks too. */
    assert(CpFont_Glyph(&font, 0x0200, &glyph) == CP_ERR_NOT_FOUND);
    free(rom.data);
}

static void TestPairing(void)
{
    char out[CP_PAIR_CANDIDATES][CP_PATH_MAX];
    char longPath[300];

    assert(CpPair_RomCandidates("sd:/roms/nds/saves/Platinum.sav", out) == 2);
    assert(strcmp(out[0], "sd:/roms/nds/Platinum.nds") == 0);
    assert(strcmp(out[1], "sd:/roms/nds/saves/Platinum.nds") == 0);
    assert(CpPair_RomCandidates("sd:/roms/nds/Pt.SAV", out) == 1 && strcmp(out[0], "sd:/roms/nds/Pt.nds") == 0);
    assert(CpPair_RomCandidates("Saves/x.sav", out) == 2 && strcmp(out[0], "x.nds") == 0);
    assert(strcmp(out[1], "Saves/x.nds") == 0);
    assert(CpPair_RomCandidates("x.sav", out) == 1 && strcmp(out[0], "x.nds") == 0);
    assert(CpPair_RomCandidates("/mysaves/x.sav", out) == 1);
    assert(CpPair_RomCandidates("/saves/.sav", out) == 0);
    assert(CpPair_RomCandidates("/roms/x.nds", out) == 0);
    memset(longPath, 'a', sizeof longPath);
    memcpy(longPath + sizeof longPath - 5, ".sav", 5);
    assert(CpPair_RomCandidates(longPath, out) == 0);
}

static void TestPosixReader(void)
{
    Bytes rom = SyntheticPlatinum("CPUE", 1);
    char path[] = "game_data_test_rom.bin";
    FILE *file = fopen(path, "wb");
    PosixReader posix;
    CpGame game;
    CpPersonal personal;

    assert(file && fwrite(rom.data, 1, rom.size, file) == rom.size && fclose(file) == 0);
    assert(PosixReader_Open(&posix, path) == 0 && posix.size == rom.size);
    assert(CpGame_Open(&posix.reader, &game) == CP_OK);
    assert(CpGame_Personal(&game, 25, 0, &personal) == CP_OK && personal.baseStats[0] == 25);
    PosixReader_Close(&posix);
    remove(path);
    free(rom.data);
}

int main(void)
{
    TestBuildCodes();
    TestSaveMatch();
    TestNitroFsAndNarc();
    TestPlatinumData();
    TestMessageCipher();
    TestFont();
    TestPairing();
    TestPosixReader();
    puts("game_data_test: all checks passed");
    return 0;
}
