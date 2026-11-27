#include "game/portrait.h"
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
    assert(top.portraits[0] == &demo->model.party[0].portrait);
    assert(bottom.portraits[0] == top.portraits[0]);
    assert((top.textMap[4 * 32 + 21] >> 12) == CpRender_PortraitPalette(0));
    assert((bottom.textMap[4 * 32 + 2] >> 12) == CpRender_PortraitPalette(0));
    assert(!memcmp(top.text.tiles + (4 * 32 + 21) * 32, top.portraits[0]->tiles, 10 * 32));
    ui.state = CP_UI_RECEIVED;
    ui.message = WonderTrade_Text_Completed;
    demo->model.received.portrait = demo->model.party[0].portrait;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
    assert(top.portraits[0] == &demo->model.received.portrait && !bottom.portraits[0]);
    ui.state = CP_UI_PICKER;
    ui.message = WonderTrade_Text_Choose;
    CpRender_Draw(&ui, &demo->model, &top, &bottom);
}

static void Dump(FILE *file, const CpScreen *screen)
{
    uint16_t palettes[16][16];
    memcpy(palettes, CpRender_Palettes, sizeof(palettes));
    for (unsigned i = 0; i < CP_UI_PARTY_MAX; ++i)
        if (screen->portraits[i])
            memcpy(palettes[CpRender_PortraitPalette(i)], screen->portraits[i]->palette, 32);
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
    int gen5 = game.table->gen5PersonalSize != 0;
    for (unsigned i = 1; i <= (gen5 ? 649u : 493u); ++i) {
        CpPersonal stats;
        assert(CpGame_Personal(&game, i, 0, &stats) == CP_OK);
        int female = stats.genderRatio == 254 || (stats.genderRatio > 0 && stats.genderRatio < 254 && i % 2);
        if (CpPortrait_Load(&game, i, 0, female, i % 2, 0, portrait)) {
            fprintf(stderr, "%.4s missing portrait for species %u\n", game.rom.gameCode, i);
            return 1;
        }
    }
    assert(CpPortrait_Load(&game, 490, 0, 0, 0, 1, portrait) == 0);
    assert(CpPortrait_Load(&game, 201, 27, 0, 1, 0, portrait) == 0);
    assert(CpPortrait_Load(&game, 25, 0, 0, 0, 0, portrait) == 0);
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
