#include "ui/app.h"

#include <string.h>

#include <nds.h>

#include "ui/render.h"
#include "ui_button.h"
#include "ui_frame.h"
#include "ui_icons.h"
#include "ui_spinner.h"

/* Both engines: background 0 the text layer (its own tile for every cell,
 * from character base 0), 1 the windows and buttons and 2 the backdrop, both
 * from the shared tileset at character base 2. */
#define TEXT_MAP_BASE 28
#define UI_MAP_BASE 29
#define BACKDROP_MAP_BASE 30
#define TILESET_BASE 2

typedef struct Engine {
    int text, ui, backdrop;
} Engine;

static Engine sMain, sSub;
static uint8_t sTopText[CP_CANVAS_BYTES] __attribute__((aligned(32)));
static uint8_t sBottomText[CP_CANVAS_BYTES] __attribute__((aligned(32)));
static CpScreen sTop = { { sTopText }, { 0 } };
static CpScreen sBottom = { { sBottomText }, { 0 } };

static volatile unsigned sTick;
static volatile int sSpinning;

#define C(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))
/* The spinner's dots from its head backwards: the selected-button orange
 * fading into the backdrop blues. */
static const uint16_t kSpinnerRamp[8] = {
    C(30, 11, 5), C(31, 24, 12), C(31, 28, 18), C(19, 27, 30),
    C(19, 27, 30), C(19, 27, 30), C(19, 27, 30), C(19, 27, 30)
};

/* Runs every frame even while a TLS handshake keeps the main thread busy:
 * the backdrop drifts down one pixel every eight frames and the spinner turns
 * by palette alone. */
static void VBlank(void)
{
    unsigned tick = ++sTick, head = (tick >> 2) & 7, dot;
    REG_BG2VOFS = (uint16_t)(-(int)(tick >> 3) & 0x1FF);
    REG_BG2VOFS_SUB = (uint16_t)(-(int)(tick >> 3) & 0x1FF);
    if (!sSpinning) return;
    for (dot = 0; dot < 8; dot++)
        BG_PALETTE_SUB[CP_PAL_SPINNER * 16 + 1 + dot] = kSpinnerRamp[(head - dot) & 7];
}

static void InitEngine(Engine *engine, int sub)
{
    uint16_t *map;
    unsigned i;
    if (sub) {
        engine->text = bgInitSub(0, BgType_Text4bpp, BgSize_T_256x256, TEXT_MAP_BASE, 0);
        engine->ui = bgInitSub(1, BgType_Text4bpp, BgSize_T_256x256, UI_MAP_BASE, TILESET_BASE);
        engine->backdrop = bgInitSub(2, BgType_Text4bpp, BgSize_T_256x256, BACKDROP_MAP_BASE, TILESET_BASE);
    } else {
        engine->text = bgInit(0, BgType_Text4bpp, BgSize_T_256x256, TEXT_MAP_BASE, 0);
        engine->ui = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, UI_MAP_BASE, TILESET_BASE);
        engine->backdrop = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, BACKDROP_MAP_BASE, TILESET_BASE);
    }
    bgSetPriority(engine->text, 0);
    bgSetPriority(engine->ui, 1);
    bgSetPriority(engine->backdrop, 2);
    /* Tiles of the project-original art (res/gfx), at their CP_TILE_* numbers. */
    dmaCopy(ui_frameTiles, (uint8_t *)bgGetGfxPtr(engine->ui) + CP_TILE_BLANK * 32, ui_frameTilesLen);
    dmaCopy(ui_buttonTiles, (uint8_t *)bgGetGfxPtr(engine->ui) + CP_TILE_BUTTON * 32, ui_buttonTilesLen);
    dmaCopy(ui_iconsTiles, (uint8_t *)bgGetGfxPtr(engine->ui) + CP_TILE_ICON * 32, ui_iconsTilesLen);
    dmaCopy(ui_spinnerTiles, (uint8_t *)bgGetGfxPtr(engine->ui) + CP_TILE_SPINNER * 32, ui_spinnerTilesLen);
    map = bgGetMapPtr(engine->text);
    for (i = 0; i < 32 * 32; i++) map[i] = i < CP_CANVAS_TILES ? (uint16_t)i : 0;
    map = bgGetMapPtr(engine->backdrop);
    for (i = 0; i < 32 * 32; i++) map[i] = (uint16_t)(CP_TILE_STRIPE | CP_PAL_BACKDROP << 12);
    dmaFillHalfWords(0, bgGetMapPtr(engine->ui), CP_MAP_SIZE * 2);
    dmaFillWords(0, bgGetGfxPtr(engine->text), CP_CANVAS_BYTES);
}

static void Present(const Engine *engine, const CpScreen *screen)
{
    DC_FlushRange(screen->text.tiles, CP_CANVAS_BYTES);
    DC_FlushRange(screen->map, sizeof(screen->map));
    dmaCopy(screen->text.tiles, bgGetGfxPtr(engine->text), CP_CANVAS_BYTES);
    dmaCopy(screen->map, bgGetMapPtr(engine->ui), sizeof(screen->map));
}

static void ReadInput(CpUiInput *input)
{
    static const struct { unsigned key, ui; } keys[] = {
        { KEY_A, CP_UI_KEY_A }, { KEY_B, CP_UI_KEY_B }, { KEY_X, CP_UI_KEY_X },
        { KEY_UP, CP_UI_KEY_UP }, { KEY_DOWN, CP_UI_KEY_DOWN }, { KEY_LEFT, CP_UI_KEY_LEFT },
        { KEY_RIGHT, CP_UI_KEY_RIGHT }, { KEY_SELECT, CP_UI_KEY_SELECT }
    };
    unsigned down, held, i;
    touchPosition touch;
    scanKeys();
    down = keysDown();
    held = keysHeld();
    memset(input, 0, sizeof(*input));
    for (i = 0; i < sizeof(keys) / sizeof(*keys); i++)
        if (down & keys[i].key) input->pressed |= keys[i].ui;
    input->touchHeld = (held & KEY_TOUCH) != 0;
    input->touchPressed = (down & KEY_TOUCH) != 0;
    if (input->touchHeld) {
        touchRead(&touch);
        input->touchX = touch.px;
        input->touchY = touch.py;
    }
}

CompanionLanguage CpUiApp_SystemLanguage(void)
{
    switch (PersonalData->language) {
    case 0: return Companion_Language_ja_JP;
    case 2: return Companion_Language_fr_FR;
    case 3: return Companion_Language_de_DE;
    case 4: return Companion_Language_it_IT;
    case 5: return Companion_Language_es_ES;
    case 7: return Companion_Language_ko_KR;
    default: return Companion_Language_en_US;
    }
}

void CpUiApp_Run(CpUiModel *model, CpUiDemo *tour)
{
    CpUi ui;
    CpUiInput input;
    unsigned sounds = 0, soundFrames = 0;
    int channel = -1;

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    InitEngine(&sMain, 0);
    InitEngine(&sSub, 1);
    dmaCopy(CpRender_Palettes, BG_PALETTE, sizeof(CpRender_Palettes));
    dmaCopy(CpRender_Palettes, BG_PALETTE_SUB, sizeof(CpRender_Palettes));
    soundEnable();
    irqSet(IRQ_VBLANK, VBlank);
    irqEnable(IRQ_VBLANK);

    CpUi_Start(&ui, model);
    for (;;) {
        ReadInput(&input);
        if (tour && !CpUiDemo_Tour(tour, &input)) tour = NULL;
        CpUi_Frame(&ui, model, &input);
        if (ui.state == CP_UI_EXIT) break;
        if (ui.sounds != sounds) {
            /* The game plays a confirm effect on every action: a short blip. */
            sounds = ui.sounds;
            if (channel >= 0) soundKill(channel);
            channel = soundPlayPSG(DutyCycle_25, 1760, 40, 64);
            soundFrames = 4;
        } else if (soundFrames && !--soundFrames && channel >= 0) {
            soundKill(channel);
            channel = -1;
        }
        if (ui.dirty) {
            ui.dirty = 0;
            CpRender_Draw(&ui, model, &sTop, &sBottom);
            sSpinning = CpUi_Busy(&ui);
            Present(&sMain, &sTop);
            Present(&sSub, &sBottom);
        }
        cothread_yield_irq(IRQ_VBLANK);
    }
    if (channel >= 0) soundKill(channel);
    irqSet(IRQ_VBLANK, NULL);
}
