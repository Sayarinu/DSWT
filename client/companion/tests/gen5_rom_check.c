/* LUNE-881: Black/White and Black 2/White 2 game data read from owned
 * cartridge dumps, given as arguments (none ship with the project; CTest runs
 * it without any). Checks what the receive effects and the screens use:
 * personal records and their forms, experience tables, trade evolutions,
 * items, move PP, species and item names, and the font's glyphs. */
#include "game/gamedata.h"
#include "game/text.h"
#include "posix_reader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int Equal(const uint16_t *text, const char *ascii)
{
    size_t i;
    for (i = 0; ascii[i]; ++i) if (text[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
    return text[i] == CP_CHAR_EOS;
}

static void Check(const char *path)
{
    PosixReader posix;
    CpGame game;
    CpPersonal personal;
    CpEvolution rows[CP_EVOLUTIONS];
    CpItem item;
    CpFont font;
    CpGlyph glyph;
    uint32_t table[CP_MAX_LEVEL + 1];
    uint16_t name[16];
    uint8_t pp;
    int b2w2;
    unsigned x, y, ink = 0;

    assert(PosixReader_Open(&posix, path) == 0);
    assert(CpGame_Open(&posix.reader, &game) == CP_OK);
    b2w2 = game.rom.family == CP_FAMILY_B2W2;
    assert(b2w2 || game.rom.family == CP_FAMILY_BW);
    /* Bulbasaur, Karrablast and the forms' own records. */
    assert(CpGame_Personal(&game, 1, 0, &personal) == CP_OK);
    assert(personal.baseStats[0] == 45 && personal.baseStats[3] == 45 && personal.genderRatio == 31);
    assert(personal.baseFriendship == 70 && personal.growthRate == 3 && personal.abilities[0] == 65
        && personal.hiddenAbility == 34);
    assert(CpGame_Personal(&game, 588, 0, &personal) == CP_OK && personal.hiddenAbility == 99);
    /* Deoxys Attack Forme; Rotom's Heat form; form 0 and an absent form read the species. */
    assert(CpGame_Personal(&game, 386, 1, &personal) == CP_OK && personal.baseStats[1] == 180);
    assert(CpGame_Personal(&game, 386, 7, &personal) == CP_OK && personal.baseStats[1] == 150);
    assert(CpGame_Personal(&game, 479, 1, &personal) == CP_OK && personal.baseStats[1] == 65);
    if (b2w2) {
        /* White Kyurem: Turboblaze. */
        assert(CpGame_Personal(&game, 646, 1, &personal) == CP_OK && personal.abilities[0] == 163);
    } else {
        assert(CpGame_Personal(&game, 646, 1, &personal) == CP_OK && personal.abilities[0] == 46);
    }
    assert(CpGame_ExpTable(&game, 0, table) == CP_OK && table[100] == 1000000);
    assert(CpGame_ExpTable(&game, 5, table) == CP_OK && table[100] == 1250000);
    /* Trade evolutions. */
    assert(CpGame_Evolutions(&game, 588, rows) == CP_OK && rows[0].method == CP_EVO_TRADE_FOR_SPECIES
        && rows[0].target == 589);
    assert(CpGame_Evolutions(&game, 616, rows) == CP_OK && rows[0].method == 7 && rows[0].target == 617);
    assert(CpGame_Evolutions(&game, 525, rows) == CP_OK && rows[0].method == CP_EVO_TRADE && rows[0].target == 526);
    assert(CpGame_Evolutions(&game, 95, rows) == CP_OK && rows[0].method == CP_EVO_TRADE_WITH_HELD_ITEM
        && rows[0].parameter == 233 && rows[0].target == 208);
    assert(CpGame_Evolutions(&game, 349, rows) == CP_OK && rows[1].method == 6 && rows[1].parameter == 537);
    /* Items: the Everstone's hold effect, TMs and key items unholdable in
     * Black/White, the dummy and unholdable lists in Black 2/White 2. */
    assert(CpGame_Item(&game, 229, &item) == CP_OK && item.holdEffect == 64 && item.holdable);
    assert(CpGame_Item(&game, 233, &item) == CP_OK && item.holdEffect == 68);
    assert(CpGame_Item(&game, 328, &item) == CP_OK && item.fieldPocket == 2 && !item.holdable);
    assert(CpGame_Item(&game, 428, &item) == CP_OK && item.fieldPocket == 4 && !item.holdable);
    assert(CpGame_Item(&game, 626, &item) == CP_OK && !item.holdable);
    assert(CpGame_Item(&game, b2w2 ? 639 : 627, &item) == CP_ERR_NOT_FOUND);
    if (b2w2) assert(CpGame_Item(&game, 638, &item) == CP_OK && !item.holdable);
    assert(CpGame_MovePp(&game, 1, &pp) == CP_OK && pp == 35);
    assert(CpGame_MovePp(&game, 105, &pp) == CP_OK && pp == 10);
    assert(CpGame_MovePp(&game, 559, &pp) == CP_OK && pp == 5);
    /* Names, with the gender signs. */
    assert(CpGame_SpeciesName(&game, 1, name, 16, NULL) == CP_OK && Equal(name, "Bulbasaur"));
    assert(CpGame_SpeciesName(&game, 588, name, 16, NULL) == CP_OK && Equal(name, "Karrablast"));
    assert(CpGame_SpeciesName(&game, 29, name, 16, NULL) == CP_OK && name[7] == 0x246E && name[8] == CP_CHAR_EOS);
    assert(CpGame_SpeciesName(&game, 649, name, 16, NULL) == CP_OK && Equal(name, "Genesect"));
    assert(CpGame_ItemName(&game, 229, name, 16, NULL) == CP_OK && Equal(name, "Everstone"));
    assert(CpGame_ItemName(&game, 1, name, 16, NULL) == CP_OK && Equal(name, "Master Ball"));
    /* The font: 'A' and the female sign draw, unknown codes draw the alternate. */
    assert(CpGame_Font(&game, CP_FONT_SYSTEM, &font) == CP_OK && font.nftr && font.cellWidth == 12
        && font.cellHeight == 15);
    assert(CpFont_Glyph(&font, 'A', &glyph) == CP_OK && glyph.width == 6 && glyph.height == 15);
    for (y = 0; y < 16; ++y) for (x = 0; x < 16; ++x) ink += CpGlyph_Pixel(&glyph, x, y) == 1;
    assert(ink >= 20 && CpGlyph_Pixel(&glyph, 0, 4) == 1 && CpGlyph_Pixel(&glyph, 1, 1) == 0);
    assert(CpFont_Glyph(&font, 0x246E, &glyph) == CP_OK && glyph.width > 0);
    assert(CpFont_Glyph(&font, 0xE123, &glyph) == CP_OK);
    PosixReader_Close(&posix);
    printf("PASS %s (%s, build code 0x%08lX): personal, forms, experience, trade evolutions, items, PP, names, font\n",
           path, game.rom.gameCode, (unsigned long)game.rom.buildCode);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; ++i) Check(argv[i]);
    if (argc == 1) puts("gen5_rom_check: no dumps given, nothing to check");
    return 0;
}
