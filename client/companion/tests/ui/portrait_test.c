#include "game/portrait.h"
#include "game/gts_music.h"
#include "ui/demo.h"
#include "ui/render.h"
#include "../posix_reader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t topTiles[CP_CANVAS_BYTES], bottomTiles[CP_CANVAS_BYTES];
static CpScreen top = { .text = { .tiles = topTiles } }, bottom = { .text = { .tiles = bottomTiles } };

static void CheckScreen(CpUiDemo *demo)
{
    CpUi ui;
    CpUi_Start(&ui, &demo->model);
    ui.state = CP_UI_PICKER;
    ui.message = WonderTrade_Text_Choose;
    ui.partyCursor = 0;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    assert(top.spritePalettes[0] == demo->model.party[0].portrait.palette);
    assert(bottom.spritePalettes[0] == (demo->model.party[0].icon.ready ? demo->model.party[0].icon.palette : top.spritePalettes[0]));
    assert((top.textMap[4 * 32 + 21] >> 12) == CpRender_PortraitPalette(0));
    assert((bottom.textMap[5 * 32 + 2] >> 12) == CpRender_PortraitPalette(0));
    assert(!memcmp(top.text.tiles + (4 * 32 + 21) * 32, demo->model.party[0].portrait.tiles, 10 * 32));
    unsigned savedGender = demo->model.party[0].gender;
    demo->model.party[0].gender = CP_UI_GENDERLESS;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    uint16_t card[CP_MAP_SIZE], panel[CP_MAP_SIZE];
    memcpy(card, top.map, sizeof(card));
    memcpy(panel, bottom.map, sizeof(panel));
    for (unsigned gender = CP_UI_MALE; gender <= CP_UI_FEMALE; ++gender) {
        demo->model.party[0].gender = gender;
        CpRender_Draw(&ui, &demo->model, &top, &bottom);
        assert(!memcmp(card, top.map, sizeof(card)) && !memcmp(panel, bottom.map, sizeof(panel)));
        unsigned ink = gender == CP_UI_FEMALE ? CP_INK_FEMALE : CP_INK_MALE;
        unsigned upper = 0, lower = 0;
        for (int y = 0; y < 12; ++y) {
            for (int x = 16; x < 152; ++x) upper += CpCanvas_Get(&top.text, x, 50 + y) == ink;
            for (int x = 0; x < 12; ++x)
                lower += CpCanvas_Get(&bottom.text, CP_UI_PANEL_LEFT + 104 + x, CP_UI_PANEL_TOP + 4 + y) == ink;
        }
        assert(upper > 20 && lower == upper);
    }
    demo->model.party[0].gender = savedGender;
    unsigned held = demo->model.party[0].heldItem;
    demo->model.party[0].heldItem = 0;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    memcpy(card, top.map, sizeof(card));
    memcpy(panel, bottom.map, sizeof(panel));
    demo->model.party[0].heldItem = 1;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    assert(!memcmp(card, top.map, sizeof(card)) && !memcmp(panel, bottom.map, sizeof(panel)));
    assert(CpCanvas_Get(&top.text, 19, 83) == CP_INK_BAG_EDGE);
    assert(CpCanvas_Get(&bottom.text, CP_UI_PANEL_LEFT + 98, CP_UI_PANEL_TOP + 27) == CP_INK_BAG_FILL);
    assert(CpCanvas_Get(&bottom.text, CP_UI_PANEL_LEFT + 99, CP_UI_PANEL_TOP + 25) == 0);
    demo->model.party[0].heldItem = held;
    ui.state = CP_UI_SUMMARY;
    ui.summaryReturn = CP_UI_PICKER;
    for (int page = 0; page < 2; ++page) {
        ui.summaryMoves = page;
        CpRender_Draw(&ui, &demo->model, &top, &bottom);
        assert(top.spritePalettes[0] == demo->model.party[0].portrait.palette);
    }
    ui.state = CP_UI_RECEIVED;
    ui.message = WonderTrade_Text_Completed;
    demo->model.received.portrait = demo->model.party[0].portrait;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    assert(top.spritePalettes[0] == demo->model.received.portrait.palette && !bottom.spritePalettes[0]);
    ui.state = CP_UI_PICKER;
    ui.message = WonderTrade_Text_Choose;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
}

static void Dump(FILE *file, const CpScreen *screen)
{
    uint16_t palettes[16][16];
    memcpy(palettes, CpRender_Palettes, sizeof(palettes));
    for (unsigned i = 0; i < CP_UI_PARTY_MAX; ++i)
        if (screen->spritePalettes[i])
            memcpy(palettes[CpRender_PortraitPalette(i)], screen->spritePalettes[i], 32);
    assert(fwrite(screen->text.tiles, CP_CANVAS_BYTES, 1, file) == 1);
    assert(fwrite(screen->map, sizeof(screen->map), 1, file) == 1);
    assert(fwrite(screen->textMap, sizeof(screen->textMap), 1, file) == 1);
    assert(fwrite(palettes, sizeof(palettes), 1, file) == 1);
}

int main(int argc, char **argv)
{
    static CpUiDemo demo;
    CpUiDemo_Init(&demo, Companion_Language_en_US);
    demo.model.partyCount = 1;
    CpPortrait *portrait = &demo.model.party[0].portrait;
    portrait->ready = 1;
    memset(portrait->tiles, 0x21, sizeof(portrait->tiles));
    CheckScreen(&demo);
    if (argc == 1) return 0;

    PosixReader file;
    CpGame game;
    assert(PosixReader_Open(&file, argv[1]) == 0 && CpGame_Open(&file.reader, &game) == CP_OK);
    CpGtsMusic music = {0};
    assert(CpGtsMusic_Load(&file.reader, game.rom.family, &music) == CP_OK);
    printf("%.4s: verified %u bytes of GTS sequence, instruments and samples\n", game.rom.gameCode, music.bytes);
    CpGtsMusic_Free(&music);
    CpPartyIcons icons;
    CpPartyIcons_Open(&icons, &game);
    assert(icons.count);
    int gen5 = game.table->gen5PersonalSize != 0;
    for (unsigned i = 1; i <= (gen5 ? 649u : 493u); ++i) {
        CpPersonal stats;
        assert(CpGame_Personal(&game, i, 0, &stats) == CP_OK);
        int female = stats.genderRatio == 254 || (stats.genderRatio > 0 && stats.genderRatio < 254 && i % 2);
        assert(!CpPartyIcon_Load(&icons, &game, i, 0, female, 0, &demo.model.party[0].icon));
        if (CpPortrait_Load(&game, i, 0, female, i % 2, 0, portrait)) {
            fprintf(stderr, "%.4s missing portrait for species %u\n", game.rom.gameCode, i);
            return 1;
        }
    }
    assert(CpPortrait_Load(&game, 490, 0, 0, 0, 1, portrait) == 0);
    assert(CpPortrait_Load(&game, 201, 27, 0, 1, 0, portrait) == 0);
    assert(CpPortrait_Load(&game, 25, 0, 0, 0, 0, portrait) == 0);
    assert(!CpPartyIcon_Load(&icons, &game, 25, 0, 0, 0, &demo.model.party[0].icon));
    demo.model.party[0].species = 25;
    demo.model.party[0].level = 25;
    CpGameText_FromAscii(demo.model.party[0].speciesName, CP_UI_NAME_MAX, "Pikachu");
    CpGameText_FromAscii(demo.model.party[0].nickname, CP_UI_NAME_MAX, "Pikachu");
    CheckScreen(&demo);
    if (argc == 3) {
        FILE *capture = fopen(argv[2], "wb");
        assert(capture);
        Dump(capture, &top);
        Dump(capture, &bottom);
        assert(!fclose(capture));
    }
    printf("%.4s: all base species, Unown form, egg, picker, thumbnail and received card passed\n", game.rom.gameCode);
    PosixReader_Close(&file);
}
