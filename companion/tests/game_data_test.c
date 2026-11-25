/* The ROM reader against small synthetic ROMs built here. Every value in them
 * is made up; no game data is in this test. */
#include "game/gamedata.h"
#include "game/nitrofs.h"
#include "game/pairing.h"
#include "game/rom.h"
#include "game/text.h"
#include "posix_reader.h"
#include "synthetic_rom.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    /* LUNE-880: a Diamond header names Diamond's archives, which this
     * Platinum-shaped ROM lacks. */
    rom = SyntheticPlatinum("CPUE", 1);
    memcpy(rom.data + 0x0C, "ADAE", 4);
    MemoryReader_Init(&memory, &rom);
    assert(CpGame_Open(&memory.reader, &game) == CP_ERR_NOT_FOUND && game.rom.family == CP_FAMILY_DP);
    free(rom.data);
}

/* LUNE-880: Diamond/Pearl and HeartGold/SoulSilver read their own paths,
 * forms, item gaps and banks. */
static void TestFamilyData(void)
{
    static const struct {
        const char *code;
        CpFamily family;
        uint8_t language;
        int pearl;
    } cases[] = {
        { "ADAE", CP_FAMILY_DP, CP_LANGUAGE_ENGLISH, 0 }, { "APAE", CP_FAMILY_DP, CP_LANGUAGE_ENGLISH, 1 },
        { "ADAJ", CP_FAMILY_DP, CP_LANGUAGE_JAPANESE, 0 }, { "APAK", CP_FAMILY_DP, CP_LANGUAGE_KOREAN, 1 },
        { "IPKE", CP_FAMILY_HGSS, CP_LANGUAGE_ENGLISH, 0 }, { "IPGD", CP_FAMILY_HGSS, CP_LANGUAGE_GERMAN, 0 },
        { "IPKJ", CP_FAMILY_HGSS, CP_LANGUAGE_JAPANESE, 0 }, { "IPGK", CP_FAMILY_HGSS, CP_LANGUAGE_KOREAN, 0 },
    };
    unsigned i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        Bytes rom = SyntheticGame(cases[i].code, 0);
        MemoryReader memory;
        CpGame game;
        CpPersonal personal;
        CpItem item;
        uint16_t name[8];
        size_t length;
        int dp = cases[i].family == CP_FAMILY_DP, asian = cases[i].language == CP_LANGUAGE_JAPANESE
            || cases[i].language == CP_LANGUAGE_KOREAN;

        MemoryReader_Init(&memory, &rom);
        assert(CpGame_Open(&memory.reader, &game) == CP_OK);
        assert(game.rom.family == cases[i].family && game.rom.language == cases[i].language);
        /* Pearl has its own personal archive, which Diamond lacks. */
        assert(strcmp(dp ? (cases[i].pearl ? "poketool/personal_pearl/personal.narc" : "poketool/personal/personal.narc")
                          : "a/0/0/2",
                      cases[i].pearl ? game.table->personalPearl : game.table->personal) == 0);
        /* Deoxys and Wormadam forms everywhere; Rotom, Giratina and Shaymin
         * forms have records only in HeartGold/SoulSilver. */
        assert(CpGame_Personal(&game, 386, 3, &personal) == CP_OK && personal.baseStats[0] == (498 & 0xFF));
        assert(CpGame_Personal(&game, 413, 2, &personal) == CP_OK && personal.baseStats[0] == (500 & 0xFF));
        assert(CpGame_Personal(&game, 479, 2, &personal) == CP_OK
            && personal.baseStats[0] == (dp ? 479 & 0xFF : 504 & 0xFF));
        assert(CpGame_Personal(&game, 492, 1, &personal) == CP_OK
            && personal.baseStats[0] == (dp ? 492 & 0xFF : 502 & 0xFF));
        /* Item gaps: DP 112..134 (no Griseous Orb); HGSS 113..134 and 428. */
        assert((CpGame_Item(&game, 112, &item) == CP_OK) == !dp);
        assert(CpGame_Item(&game, 134, &item) == CP_ERR_NOT_FOUND);
        assert(CpGame_Item(&game, 135, &item) == CP_OK && item.holdEffect == (dp ? 112 : 113));
        assert(CpGame_Item(&game, 427, &item) == CP_OK && item.holdEffect == (uint8_t)(dp ? 404 : 405));
        assert((CpGame_Item(&game, 428, &item) == CP_OK) == dp);
        assert(CpGame_Item(&game, 429, &item) == CP_OK && item.holdEffect == (uint8_t)(dp ? 406 : 406));
        assert(CpGame_Item(&game, dp ? 464 : 536, &item) == CP_OK);
        assert(CpGame_Item(&game, dp ? 465 : 537, &item) == CP_ERR_NOT_FOUND);
        /* Species names from the language's own bank number. */
        assert(CpGame_Bank(&game, &game.table->speciesNames)
            == (!asian ? (dp ? 362 : 237) : cases[i].language == CP_LANGUAGE_JAPANESE ? (dp ? 356 : 232) : (dp ? 357 : 233)));
        assert(CpGame_Bank(&game, &game.table->itemNames)
            == (!asian ? (dp ? 344 : 222) : cases[i].language == CP_LANGUAGE_JAPANESE ? (dp ? 341 : 219) : (dp ? 342 : 220)));
        assert(CpGame_SpeciesName(&game, 25, name, 8, &length) == CP_OK && length == (asian ? 2u : 4u));
        free(rom.data);
    }
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
    TestFamilyData();
    TestMessageCipher();
    TestFont();
    TestPairing();
    TestPosixReader();
    puts("game_data_test: all checks passed");
    return 0;
}
