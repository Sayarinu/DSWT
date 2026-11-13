#include "wonder_trade/dp/application.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "global.h"

#include "constants/rgb.h"
#include "constants/sndseq.h"

#include "msgdata/msg.naix"
#include "msgdata/msg/narc_0624.h"

#include "bag.h"
#include "bg_window.h"
#include "PAD_pad.h"
#include "field_system.h"
#include "filesystem.h"
#include "font.h"
#include "game_init.h"
#include "gf_gfx_loader.h"
#include "gf_gfx_planes.h"
#include "heap.h"
#include "mail.h"
#include "main.h"
#include "msgdata.h"
#include "options.h"
#include "party.h"
#include "play_timer.h"
#include "player_data.h"
#include "poke_overlay.h"
#include "pokemon.h"
#include "render_window.h"
#include "save.h"
#include "string16.h"
#include "string_util.h"
#include "text.h"
#include "unk_020051F4.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/dp/game_save.h"
#include "wonder_trade/menu_input.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"

/* asm/unk_0200E1D0_s.s; pokediamond declares these only where they are used.
 * sub_0200E3A0 sets a screen's master brightness to white (0x7FFF) or black. */
extern void sub_0200E3A0(PMLCDTarget screen, int color);
extern void BeginNormalPaletteFade(u32 pattern, u32 typeTop, u32 typeBottom, u16 colour, u32 duration, u32 framesPer, enum HeapID heapID);
extern BOOL IsPaletteFadeFinished(void);
/* src/unk_02006D98.c: undo a Pokémon picture's encryption, and paint
 * Spinda's spots on its first frame, as the battle and summary screens do. */
extern void sub_02008A74(u8 *pixels);
extern void sub_02008904(u8 *pixels, u32 personality, u32 bothFrames);
/* asm/unk_0206F3FC.s: the party screen, as the field's "choose a Pokémon"
 * script command (sub_020379F8) opens it. */
extern const struct OverlayManagerTemplate UNK_020F96DC;

enum WonderTradeAppState {
    WT_APP_BOOT,
    WT_APP_RECOVERY,
    WT_APP_SAVE_FIRST,
    WT_APP_CONNECTING,
    WT_APP_MENU,
    WT_APP_PICKER_FADE,
    WT_APP_PICKER,
    WT_APP_CONFIRM,
    WT_APP_COLLECT_CONFIRM,
    WT_APP_CANCEL_CONFIRM,
    WT_APP_PINGING,
    WT_APP_MESSAGE,
    WT_APP_ERROR,
    WT_APP_RESTART,
    WT_APP_CLOSING,
};

/* The GTS reserves the same for itself and its Wi-Fi heap (overlay 80). */
#define WT_HEAP_SIZE 0x70000
#define WT_BOOT_STACK_SIZE 0x8000
#define WT_BOOT_SILENT_FRAMES 30
#define WT_FONT_SYSTEM 0
#define WT_FONT_MESSAGE 1
/* A pressed button blinks this long before it acts, as touch menus do. */
#define WT_PRESS_FRAMES 8
/* graphic/worldtrade.narc, the GTS's own art (overlay 80 loads the same
 * members): its palettes, the striped backdrop and the listing card. */
#define WT_GTS_PALETTE 7
#define WT_GTS_BACKDROP_TILES 14
#define WT_GTS_CARD_TILES 17
#define WT_GTS_CARD_MAP 25
#define WT_GTS_BACKDROP_MAP 29
#define WT_GTS_BUTTON_MAP 30
/* Background tiles and palettes, as in Platinum's Wonder Trade screen. */
#define WT_FRAME_TILE 961
#define WT_MESSAGE_TILE 970
#define WT_PREVIEW_PALETTE 4
#define WT_TEXT_PALETTE 6
#define WT_FRAME_PALETTE 7
#define WT_MESSAGE_PALETTE 8
#define WT_BACKDROP_PALETTE 9
#define WT_SUB_TEXT_PALETTE 13
#define WT_BUTTON_PALETTE 1
#define WT_SELECTED_PALETTE 3
#define WT_COLOR_LIGHT MAKE_TEXT_COLOR(15, 2, 0)
#define WT_COLOR_DARK MAKE_TEXT_COLOR(1, 2, 15)
#define WT_COLOR_FAINT MAKE_TEXT_COLOR(2, 15, 15)
#define WT_COLOR_BUTTON MAKE_TEXT_COLOR(1, 2, 0)
/* Both screens fade to or from black over six frames, as the field does
 * around every start-menu application (ov05_021D7CA4). */
#define WT_FADE_BOTH_SCREENS 0
#define WT_FADE_OUT 0
#define WT_FADE_IN 1
#define WT_FADE_STEPS 6
/* The party screen's arguments (sub_0203797C), 0x40 bytes: "choose a
 * Pokémon" is type 0, mode 3, and leaves the chosen slot at 0x22. */
#define WT_PARTY_MODE_SELECT 3

typedef struct WonderTradePartyMenu {
    struct Party *party;
    Bag *bag;
    struct Mail *mailbox;
    struct Options *options;
    void *unused[3];
    FieldSystem *fieldSystem;
    u8 mode;
    u8 type;
    u8 selectedMonSlot;
    u8 rest[0x1D];
} WonderTradePartyMenu;
typedef char WonderTradePartyMenuSize[sizeof(WonderTradePartyMenu) == 0x40 ? 1 : -1];

typedef struct WonderTradeBootWorker {
    OSThread thread;
    WtFlashAccess flash;
    WtGame game;
    int result;
    unsigned char stack[WT_BOOT_STACK_SIZE + 8];
} WonderTradeBootWorker;

typedef struct WonderTradeApp {
    FieldSystem *fieldSystem;
    WonderTradeNetwork network;
    BgConfig *bgConfig;
    struct Window title, message, details, body, subTitle, preview;
    struct Window actions[WT_MENU_MAX_ACTIONS];
    struct SomeDrawPokemonStruct previewTemplate;
    u16 buttonTilemap[32 * 24];
    BOOL previewLoaded;
    int touchBlocked;
    int pressTimer;
    BOOL redraw;
    WaitingIcon *waitingIcon;
    struct MsgData *messages;
    struct OverlayManager *picker;
    struct Party partySnapshot;
    WonderTradePartyMenu partyMenu;
    int state;
    int statusMessage;
    int cursor;
    int selectedSlot;
    BOOL graphicsReady;
    BOOL fadeInPending;
    BOOL fading;
    BOOL leaving;
    WonderTradeBootWorker *boot;
    int bootFrames;
    WtGame game;
} WonderTradeApp;

static void WonderTrade_Draw(WonderTradeApp *app);

static const struct GraphicsBanks sBanks = {
    .bg = GX_VRAM_BG_128_A,
    .bgextpltt = GX_VRAM_BGEXTPLTT_NONE,
    .subbg = GX_VRAM_SUB_BG_128_C,
    .subbgextpltt = GX_VRAM_SUB_BGEXTPLTT_NONE,
    .obj = GX_VRAM_OBJ_NONE,
    .objextpltt = GX_VRAM_OBJEXTPLTT_NONE,
    .subobj = GX_VRAM_SUB_OBJ_NONE,
    .subobjextpltt = GX_VRAM_SUB_OBJEXTPLTT_NONE,
    .tex = GX_VRAM_TEX_NONE,
    .texpltt = GX_VRAM_TEXPLTT_NONE,
};

static const struct GraphicsModes sModes = {
    .dispMode = GX_DISPMODE_GRAPHICS,
    .bgMode = GX_BGMODE_0,
    .subMode = GX_BGMODE_0,
    ._2d3dMode = GX_BG0_AS_2D,
};

static void WonderTrade_InitBg(BgConfig *bgConfig, u8 layer, u8 screenBase, u8 charBase, u8 priority)
{
    const struct BgTemplate template = {
        .x = 0, .y = 0, .bufferSize = 0x800, .baseTile = 0,
        .size = GF_BG_SCR_SIZE_256x256, .colorMode = GX_BG_COLORMODE_16,
        .screenBase = screenBase, .charBase = charBase,
        .bgExtPltt = GX_BG_EXTPLTT_01, .priority = priority, .areaOver = GX_BG_AREAOVER_XLU,
        .dummy = 0, .mosaic = FALSE,
    };
    InitBgFromTemplate(bgConfig, layer, &template, GF_BG_TYPE_TEXT);
    BgClearTilemapBufferAndCommit(bgConfig, layer);
}

/* The GTS menu tiles, with the trade screens' blue and cyan accents. */
static const u16 sBackdropPalette[16] = {
    0, RGB(31, 31, 31), RGB(9, 18, 27), RGB(13, 22, 31),
    RGB(14, 26, 29), RGB(19, 27, 30), 0, RGB(22, 30, 30),
    RGB(27, 31, 31), RGB(21, 26, 31), 0, 0, 0, 0, 0, 0
};
static const u16 sButtonPalette[16] = {
    0, RGB(25, 30, 31), 0, 0, 0, 0, 0, 0, 0,
    RGB(19, 27, 30), RGB(22, 30, 30), RGB(9, 18, 27), 0, 0, 0, 0
};
static const u16 sSelectedPalette[16] = {
    0, RGB(31, 31, 25), 0, 0, 0, 0, 0, 0, 0,
    RGB(31, 24, 12), RGB(31, 28, 18), RGB(30, 11, 5), 0, 0, 0, 0
};

static struct Party *WonderTrade_Party(WonderTradeApp *app)
{
    return SaveArray_Party_Get(app->game.save);
}

static int WonderTrade_Actions(const WonderTradeApp *app, int labels[WT_MENU_MAX_ACTIONS])
{
    switch (app->state) {
    case WT_APP_MENU:
        labels[0] = app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Trade : WonderTrade_Text_Collect;
        labels[1] = app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Info : WonderTrade_Text_CancelOffer;
        labels[2] = WonderTrade_Text_Exit;
        return 3;
    case WT_APP_CONFIRM:
    case WT_APP_COLLECT_CONFIRM:
    case WT_APP_CANCEL_CONFIRM:
        labels[0] = WonderTrade_Text_ConfirmAction;
        labels[1] = WonderTrade_Text_Back;
        return 2;
    case WT_APP_ERROR:
        labels[0] = WonderTrade_Text_Retry;
        if (!app->network.canLeave) return 1;
        labels[1] = WonderTrade_Text_Exit;
        return 2;
    case WT_APP_MESSAGE:
        labels[0] = WonderTrade_Text_Back;
        return 1;
    case WT_APP_SAVE_FIRST:
        labels[0] = WonderTrade_Text_Exit;
        return 1;
    case WT_APP_RECOVERY:
        labels[0] = WonderTrade_Text_RecoveryAction;
        return 1;
    case WT_APP_CONNECTING:
    case WT_APP_PINGING:
        if (!app->network.canLeave) return 0;
        labels[0] = WonderTrade_Text_Cancel;
        return 1;
    default:
        return 0;
    }
}

static void WonderTrade_VBlank(void *arg)
{
    WonderTradeApp *app = arg;
    DoScheduledBgGpuUpdates(app->bgConfig);
    OS_SetIrqCheckFlag(OS_IE_VBLANK);
}

/* The icon task restores the message box frame on its next run and then
 * frees itself (sub_0200DBFC); the screen is redrawn after that. */
static void WonderTrade_StopWaitingIcon(WonderTradeApp *app)
{
    if (app->waitingIcon == NULL) return;
    sub_0200DBFC(app->waitingIcon);
    app->waitingIcon = NULL;
    app->redraw = TRUE;
}

static void WonderTrade_Fade(WonderTradeApp *app, u32 type)
{
    if (type == WT_FADE_OUT) WonderTrade_StopWaitingIcon(app);
    BeginNormalPaletteFade(WT_FADE_BOTH_SCREENS, type, type, RGB_BLACK, WT_FADE_STEPS, 1, app->game.heapID);
    app->fading = TRUE;
}

static void WonderTrade_ClearTilemap(WonderTradeApp *app, u8 layer)
{
    memset(GetBgTilemapBuffer(app->bgConfig, layer), 0, 32 * 32 * sizeof(u16));
}

/* Every screen change happens behind black: the field has already faded out
 * before launching the application, and the boot check starts in black too. */
static void WonderTrade_InitGraphics(WonderTradeApp *app)
{
    struct Options *options = Save_PlayerData_GetOptionsAddr(app->game.save);
    u16 *card;
    int i;
    sub_0200E3A0(PM_LCD_TOP, RGB_BLACK);
    sub_0200E3A0(PM_LCD_BOTTOM, RGB_BLACK);
    Main_SetVBlankIntrCB(NULL, NULL);
    Main_SetHBlankIntrCB(NULL, NULL);
    GfGfx_DisableEngineAPlanes();
    GfGfx_DisableEngineBPlanes();
    GX_SetVisiblePlane(0);
    GXS_SetVisiblePlane(0);
    gSystem.screensFlipped = FALSE;
    GfGfx_SwapDisplay();
    G2_BlendNone();
    G2S_BlendNone();
    GX_SetVisibleWnd(GX_WNDMASK_NONE);
    GXS_SetVisibleWnd(0);
    GfGfx_SetBanks(&sBanks);
    SetBothScreensModesAndDisable(&sModes);
    /* BG 0 holds the windows and BG 1 the GTS backdrop; BG 2 is the listing
     * card on the top screen and, sharing BG 1's tiles, the touch buttons on
     * the bottom one. */
    for (i = 0; i < 2; i++) {
        u8 base = i ? GF_BG_LYR_SUB_0 : GF_BG_LYR_MAIN_0;
        WonderTrade_InitBg(app->bgConfig, base, GX_BG_SCRBASE_0xf800, GX_BG_CHARBASE_0x00000, 0);
        WonderTrade_InitBg(app->bgConfig, (u8)(base + 1), GX_BG_SCRBASE_0xf000, GX_BG_CHARBASE_0x08000, 2);
        WonderTrade_InitBg(app->bgConfig, (u8)(base + 2), GX_BG_SCRBASE_0xe800, i ? GX_BG_CHARBASE_0x08000 : GX_BG_CHARBASE_0x0c000, 1);
        BG_ClearCharDataRange(base, 0x20, 0, app->game.heapID);
        GfGfxLoader_LoadCharData(NARC_GRAPHIC_WORLDTRADE, WT_GTS_BACKDROP_TILES, app->bgConfig, base + 1, 0, 0, TRUE, app->game.heapID);
        GfGfxLoader_LoadScrnData(NARC_GRAPHIC_WORLDTRADE, WT_GTS_BACKDROP_MAP, app->bgConfig, base + 1, 0, 0, TRUE, app->game.heapID);
    }

    BgTilemapRectChangePalette(app->bgConfig, GF_BG_LYR_MAIN_1, 0, 0, 32, 24, WT_BACKDROP_PALETTE);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_1);
    GfGfxLoader_LoadCharData(NARC_GRAPHIC_WORLDTRADE, WT_GTS_CARD_TILES, app->bgConfig, GF_BG_LYR_MAIN_2, 0, 0, TRUE, app->game.heapID);
    GfGfxLoader_LoadScrnData(NARC_GRAPHIC_WORLDTRADE, WT_GTS_CARD_MAP, app->bgConfig, GF_BG_LYR_MAIN_2, 0, 0, TRUE, app->game.heapID);
    /* Keep only the listing card, a tile clear of the header bar and above the message box. */
    card = GetBgTilemapBuffer(app->bgConfig, GF_BG_LYR_MAIN_2);
    memmove(card + 4 * 32, card + 32, 12 * 32 * sizeof(*card));
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_MAIN_2, 0, 0, 0, 32, 4, 0);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_MAIN_2, 0, 0, 16, 32, 8, 0);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_2);
    GfGfxLoader_LoadScrnData(NARC_GRAPHIC_WORLDTRADE, WT_GTS_BUTTON_MAP, app->bgConfig, GF_BG_LYR_SUB_2, 0, 0, TRUE, app->game.heapID);
    memcpy(app->buttonTilemap, GetBgTilemapBuffer(app->bgConfig, GF_BG_LYR_SUB_2), sizeof(app->buttonTilemap));

    GfGfxLoader_GXLoadPal(NARC_GRAPHIC_WORLDTRADE, WT_GTS_PALETTE, GF_PAL_LOCATION_MAIN_BG, GF_PAL_SLOT_0_OFFSET, 3 * 32, app->game.heapID);
    BG_LoadPlttData(GF_PAL_LOCATION_MAIN_BG, (void *)sBackdropPalette, sizeof(sBackdropPalette), (enum GFPalSlotOffset)(WT_BACKDROP_PALETTE * 32));
    BG_LoadPlttData(GF_PAL_LOCATION_SUB_BG, (void *)sBackdropPalette, sizeof(sBackdropPalette), GF_PAL_SLOT_0_OFFSET);
    BG_LoadPlttData(GF_PAL_LOCATION_SUB_BG, (void *)sButtonPalette, sizeof(sButtonPalette), (enum GFPalSlotOffset)(WT_BUTTON_PALETTE * 32));
    BG_LoadPlttData(GF_PAL_LOCATION_SUB_BG, (void *)sSelectedPalette, sizeof(sSelectedPalette), (enum GFPalSlotOffset)(WT_SELECTED_PALETTE * 32));
    LoadFontPal1(GF_PAL_LOCATION_MAIN_BG, (enum GFPalSlotOffset)(WT_TEXT_PALETTE * 32), app->game.heapID);
    LoadFontPal1(GF_PAL_LOCATION_SUB_BG, (enum GFPalSlotOffset)(WT_SUB_TEXT_PALETTE * 32), app->game.heapID);
    /* The game's own window and, in the player's chosen frame, message box. */
    LoadUserFrameGfx1(app->bgConfig, GF_BG_LYR_MAIN_0, WT_FRAME_TILE, WT_FRAME_PALETTE, 0, app->game.heapID);
    sub_0200CD68(app->bgConfig, GF_BG_LYR_MAIN_0, WT_MESSAGE_TILE, WT_MESSAGE_PALETTE, (u8)Options_GetFrame(options), app->game.heapID);

    AddWindowParameterized(app->bgConfig, &app->title, GF_BG_LYR_MAIN_0, 2, 1, 28, 2, WT_TEXT_PALETTE, 1);
    /* The field's message box (text_02054590.c), 4 tiles high, narrowed to
     * line up with the listing card and raised clear of the bottom bar. */
    AddWindowParameterized(app->bgConfig, &app->message, GF_BG_LYR_MAIN_0, 3, 18, 25, 4, WT_TEXT_PALETTE, 61);
    AddWindowParameterized(app->bgConfig, &app->details, GF_BG_LYR_MAIN_0, 2, 5, 18, 10, WT_TEXT_PALETTE, 223);
    AddWindowParameterized(app->bgConfig, &app->body, GF_BG_LYR_MAIN_0, 2, 5, 28, 16, WT_TEXT_PALETTE, 413);
    AddWindowParameterized(app->bgConfig, &app->subTitle, GF_BG_LYR_SUB_0, 2, 1, 28, 2, WT_SUB_TEXT_PALETTE, 1);
    for (i = 0; i < WT_MENU_MAX_ACTIONS; i++) {
        AddWindowParameterized(app->bgConfig, &app->actions[i], GF_BG_LYR_SUB_0,
            WT_MENU_LEFT / 8, (u8)((WT_MENU_TOP + i * WT_MENU_SPACING) / 8),
            WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, WT_SUB_TEXT_PALETTE, (u16)(61 + i * 72));
    }
    AddWindowParameterized(app->bgConfig, &app->preview, GF_BG_LYR_MAIN_0, 21, 5, 10, 10, WT_PREVIEW_PALETTE, 861);
    app->previewLoaded = FALSE;
    app->graphicsReady = TRUE;
    app->touchBlocked = TRUE;
    app->pressTimer = 0;
    WonderTrade_Draw(app);
    Main_SetVBlankIntrCB(WonderTrade_VBlank, app);
    GfGfx_EngineATogglePlanes(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1, GX_PLANE_TOGGLE_ON);
    GfGfx_EngineBTogglePlanes(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1 | GX_PLANEMASK_BG2, GX_PLANE_TOGGLE_ON);
    GfGfx_BothDispOn();
    /* The fade in starts once the next VBlank has uploaded the tilemaps. */
    app->fadeInPending = TRUE;
}

static void WonderTrade_FreeGraphics(WonderTradeApp *app)
{
    int i;
    if (!app->graphicsReady) return;
    Main_SetVBlankIntrCB(NULL, NULL);
    Main_SetHBlankIntrCB(NULL, NULL);
    RemoveWindow(&app->preview);
    RemoveWindow(&app->title);
    RemoveWindow(&app->message);
    RemoveWindow(&app->details);
    RemoveWindow(&app->body);
    RemoveWindow(&app->subTitle);
    for (i = 0; i < WT_MENU_MAX_ACTIONS; i++) RemoveWindow(&app->actions[i]);
    for (i = 0; i < 3; i++) {
        FreeBgTilemapBuffer(app->bgConfig, (u8)(GF_BG_LYR_MAIN_0 + i));
        FreeBgTilemapBuffer(app->bgConfig, (u8)(GF_BG_LYR_SUB_0 + i));
    }
    GfGfx_DisableEngineAPlanes();
    GfGfx_DisableEngineBPlanes();
    app->graphicsReady = FALSE;
    app->redraw = FALSE;
}

static void WonderTrade_PrintString(struct Window *window, u8 font, struct String *text, int x, int y, u32 color)
{
    /* A negative x centres the text in the window. */
    if (x < 0) x = (int)sub_02002F40(font, text, 0, GetWindowWidth(window) * 8);
    AddTextPrinterParameterized2(window, font, text, (u32)x, (u32)y, TEXT_SPEED_NOTRANSFER, color, NULL);
}

static void WonderTrade_Print(WonderTradeApp *app, struct Window *window, u8 font, int message, int x, int y, u32 color)
{
    struct String *text = NewString_ReadMsgData(app->messages, message);
    WonderTrade_PrintString(window, font, text, x, y, color);
    String_Delete(text);
}

static void WonderTrade_PrintChars(WonderTradeApp *app, struct Window *window, const u16 *chars, int x, int y)
{
    struct String *text = String_New(POKEMON_NAME_LENGTH + PLAYER_NAME_LENGTH + 2, app->game.heapID);
    CopyU16ArrayToString(text, (u16 *)chars);
    WonderTrade_PrintString(window, WT_FONT_SYSTEM, text, x, y, WT_COLOR_LIGHT);
    String_Delete(text);
}

/* Right-aligned, as the summary and Trainer Card screens set out figures. */
static void WonderTrade_PrintNumber(WonderTradeApp *app, struct Window *window, int value, int right, int y, u32 color)
{
    struct String *number = String_New(12, app->game.heapID);
    int x;
    String16_FormatInteger(number, value, 10, PRINTING_MODE_LEFT_ALIGN, TRUE);
    /* Right-align at `right`, never past the window's own edge. */
    if (right > GetWindowWidth(window) * 8 - 8) right = GetWindowWidth(window) * 8 - 8;
    x = right - (int)sub_02002E14(WT_FONT_SYSTEM, number, 0);
    WonderTrade_PrintString(window, WT_FONT_SYSTEM, number, x, y, color);
    String_Delete(number);
}

/* The Pokémon's front picture as a 10x10-tile window over the card's frame. */
static void WonderTrade_Preview(WonderTradeApp *app, struct SomeDrawPokemonStruct *sprite)
{
    if (!app->previewLoaded || memcmp(sprite, &app->previewTemplate, sizeof(*sprite)) != 0) {
        NNSG2dCharacterData *data;
        void *resource = GfGfxLoader_GetCharData((NarcId)sprite->unk0, sprite->unk2, FALSE, &data, app->game.heapID);
        u8 *source = data->pRawData;
        u8 *tiles = app->preview.pixelBuffer;
        int x, y;
        sub_02008A74(source);
        if (sprite->unk6) sub_02008904(source, sprite->unkC, FALSE);
        /* Copy the first 80x80 frame from the two-frame bitmap into BG tiles. */
        for (y = 0; y < 80; y++) {
            for (x = 0; x < 10; x++) {
                memcpy(tiles + ((y / 8) * 10 + x) * 32 + (y % 8) * 4,
                    source + y * data->W * 4 + x * 4, 4);
            }
        }
        Heap_Free(resource);
        GfGfxLoader_GXLoadPal((NarcId)sprite->unk0, sprite->unk4, GF_PAL_LOCATION_MAIN_BG,
            (enum GFPalSlotOffset)(WT_PREVIEW_PALETTE * 32), 32, app->game.heapID);
        app->previewTemplate = *sprite;
        app->previewLoaded = TRUE;
    }
    ScheduleWindowCopyToVram(&app->preview);
}

static void WonderTrade_DrawPokemon(WonderTradeApp *app)
{
    struct SomeDrawPokemonStruct sprite;
    struct String *species;
    memset(&sprite, 0, sizeof(sprite));
    FillWindowPixelBuffer(&app->details, 0);
    if (app->state == WT_APP_CONFIRM) {
        struct Pokemon *mon = Party_GetMonByIndex(&app->partySnapshot, app->selectedSlot);
        u16 nickname[POKEMON_NAME_LENGTH + 1];
        species = GetSpeciesName((u16)GetMonData(mon, MON_DATA_SPECIES, NULL), app->game.heapID);
        GetMonData(mon, MON_DATA_NICKNAME, nickname);
        sub_02068B68(&sprite, mon, 2);
        WonderTrade_Preview(app, &sprite);
        WonderTrade_PrintString(&app->details, WT_FONT_SYSTEM, species, 0, 16, WT_COLOR_LIGHT);
        WonderTrade_PrintChars(app, &app->details, nickname, 0, 32);
        WonderTrade_Print(app, &app->details, WT_FONT_SYSTEM, WonderTrade_Text_Level, 0, 64, WT_COLOR_LIGHT);
        WonderTrade_PrintNumber(app, &app->details, (int)GetMonData(mon, MON_DATA_LEVEL, NULL), 48, 64, WT_COLOR_LIGHT);
    } else {
        const WtReceivedPokemon *received = &app->network.received;
        species = GetSpeciesName(received->species, app->game.heapID);
        sub_02068C00(&sprite, received->species, received->gender, 2, received->shiny, received->form, received->personality);
        WonderTrade_Preview(app, &sprite);
        WonderTrade_Print(app, &app->details, WT_FONT_SYSTEM, WonderTrade_Text_Received, 0, 0, WT_COLOR_LIGHT);
        WonderTrade_PrintString(&app->details, WT_FONT_SYSTEM, species, 0, 16, WT_COLOR_LIGHT);
        if (received->hasNickname) WonderTrade_PrintChars(app, &app->details, received->nickname, 0, 32);
        WonderTrade_Print(app, &app->details, WT_FONT_SYSTEM, WonderTrade_Text_OriginalTrainer, 0, 64, WT_COLOR_LIGHT);
        WonderTrade_PrintChars(app, &app->details, received->originalTrainer, 40, 64);
    }
    String_Delete(species);
    ScheduleWindowCopyToVram(&app->details);
}

/* The GTS's touch buttons; the chosen one is lit, and blinks when pressed. */
static void WonderTrade_DrawButtons(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int i, count = WonderTrade_Actions(app, labels);
    if (app->cursor >= count) app->cursor = 0;
    memcpy(GetBgTilemapBuffer(app->bgConfig, GF_BG_LYR_SUB_2), app->buttonTilemap, sizeof(app->buttonTilemap));
    for (i = 0; i < WT_MENU_MAX_ACTIONS; i++) {
        u8 y = (u8)((WT_MENU_TOP + i * WT_MENU_SPACING) / 8);
        BOOL lit = i == app->cursor && (app->pressTimer == 0 || (app->pressTimer / 2) % 2 == 0);
        if (i >= count) {
            FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_2, 0, WT_MENU_LEFT / 8, y, WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, 0);
        } else {
            BgTilemapRectChangePalette(app->bgConfig, GF_BG_LYR_SUB_2, WT_MENU_LEFT / 8, y,
                WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, lit ? WT_SELECTED_PALETTE : WT_BUTTON_PALETTE);
        }
    }
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_SUB_2);
}

static BOOL WonderTrade_Waiting(const WonderTradeApp *app)
{
    return app->state == WT_APP_BOOT || app->state == WT_APP_CONNECTING || app->state == WT_APP_PINGING
        || app->state == WT_APP_RESTART || app->state == WT_APP_CLOSING;
}

static void WonderTrade_Draw(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int i, count;
    BOOL preview, fullText;
    if (!app->graphicsReady) return;
    if (app->waitingIcon != NULL) {
        /* Draw once the icon has put the message box frame back. */
        WonderTrade_StopWaitingIcon(app);
        return;
    }
    app->redraw = FALSE;
    count = WonderTrade_Actions(app, labels);
    preview = (app->state == WT_APP_CONFIRM && app->selectedSlot >= 0
        && app->selectedSlot < Party_GetCount(&app->partySnapshot))
        || (app->state == WT_APP_MESSAGE && app->statusMessage == WonderTrade_Text_Completed && app->network.hasReceived);
    fullText = app->state == WT_APP_ERROR || app->state == WT_APP_RECOVERY
        || app->statusMessage == WonderTrade_Text_Explanation || app->statusMessage == WonderTrade_Text_Diagnostics
        || app->statusMessage == WonderTrade_Text_Ineligible;
    WonderTrade_ClearTilemap(app, GF_BG_LYR_MAIN_0);
    WonderTrade_ClearTilemap(app, GF_BG_LYR_SUB_0);
    GfGfx_EngineATogglePlanes(GX_PLANEMASK_BG2, preview ? GX_PLANE_TOGGLE_ON : GX_PLANE_TOGGLE_OFF);
    FillWindowPixelBuffer(&app->title, 0);
    WonderTrade_Print(app, &app->title, WT_FONT_SYSTEM, WonderTrade_Text_Title, 0, 2, WT_COLOR_LIGHT);
    ScheduleWindowCopyToVram(&app->title);
    if (preview) WonderTrade_DrawPokemon(app);
    if (fullText) {
        FillWindowPixelBuffer(&app->body, 15);
        DrawFrameAndWindow1(&app->body, TRUE, WT_FRAME_TILE, WT_FRAME_PALETTE);
        WonderTrade_Print(app, &app->body, WT_FONT_SYSTEM, app->statusMessage, 4, 0, WT_COLOR_DARK);
        if (app->state == WT_APP_ERROR) {
            /* For support only: small and quiet, under the explanation. */
            WonderTrade_Print(app, &app->body, WT_FONT_SYSTEM, WonderTrade_Text_ErrorCode, 4, 112, WT_COLOR_FAINT);
            WonderTrade_PrintNumber(app, &app->body, app->network.error, 28 * 8 - 8, 112, WT_COLOR_FAINT);
        } else if (app->statusMessage == WonderTrade_Text_Diagnostics) {
            static const int names[] = { WonderTrade_Text_ConnectMs, WonderTrade_Text_TlsBytes, WonderTrade_Text_StackBytes, WonderTrade_Text_FrameMs };
            const u32 values[] = { app->network.handshakeMs, app->network.tlsPeakBytes, app->network.stackBytes, app->network.maxFrameMs };
            for (i = 0; i < 4; i++) {
                WonderTrade_Print(app, &app->body, WT_FONT_SYSTEM, names[i], 4, 32 + i * 20, WT_COLOR_DARK);
                WonderTrade_PrintNumber(app, &app->body, (int)values[i], 28 * 8 - 8, 32 + i * 20, WT_COLOR_DARK);
            }
        }
        ScheduleWindowCopyToVram(&app->body);
    } else {
        /* The player's own message box and message font, where conversations show them. */
        FillWindowPixelBuffer(&app->message, 15);
        DrawFrameAndWindow2(&app->message, TRUE, WT_MESSAGE_TILE, WT_MESSAGE_PALETTE);
        WonderTrade_Print(app, &app->message, WT_FONT_MESSAGE, app->statusMessage, 0, 0, WT_COLOR_DARK);
        ScheduleWindowCopyToVram(&app->message);
        /* The game's spinning wait icon, as while it saves (scrcmd.c). */
        if (WonderTrade_Waiting(app)) app->waitingIcon = WaitingIcon_New(&app->message, WT_MESSAGE_TILE);
    }
    FillWindowPixelBuffer(&app->subTitle, 0);
    WonderTrade_Print(app, &app->subTitle, WT_FONT_SYSTEM, WonderTrade_Text_Title, 0, 2, WT_COLOR_LIGHT);
    ScheduleWindowCopyToVram(&app->subTitle);
    for (i = 0; i < count; i++) {
        FillWindowPixelBuffer(&app->actions[i], 0);
        WonderTrade_Print(app, &app->actions[i], WT_FONT_SYSTEM, labels[i], -1, 8, WT_COLOR_BUTTON);
        ScheduleWindowCopyToVram(&app->actions[i]);
    }
    WonderTrade_DrawButtons(app);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_0);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_SUB_0);
}

/* Like every retail application, fade both screens to black before ending,
 * so the next one (the field, or the title screen at boot) loads unseen. */
static BOOL WonderTrade_Leave(WonderTradeApp *app)
{
    if (!app->graphicsReady) return TRUE;
    app->leaving = TRUE;
    WonderTrade_Fade(app, WT_FADE_OUT);
    return FALSE;
}

static void WonderTrade_Show(WonderTradeApp *app, int state, int message)
{
    app->state = state;
    app->statusMessage = message;
    app->cursor = 0;
    app->pressTimer = 0;
    app->touchBlocked = TRUE;
    WonderTrade_Draw(app);
}

static void WonderTrade_Close(WonderTradeApp *app)
{
    if (!app->network.canLeave) return;
    WonderTradeNetwork_Close(&app->network);
    WonderTrade_Show(app, WT_APP_CLOSING, WonderTrade_Text_Disconnecting);
}

/* The game's own party screen picks the Pokémon, faded in and out like any
 * other; it runs on a copy, so nothing it does reaches the save. */
static void WonderTrade_StartPicker(WonderTradeApp *app)
{
    SaveData *save = app->game.save;
    CopyParty(WonderTrade_Party(app), &app->partySnapshot);
    memset(&app->partyMenu, 0, sizeof(app->partyMenu));
    app->partyMenu.party = &app->partySnapshot;
    app->partyMenu.bag = Save_Bag_Get(save);
    app->partyMenu.mailbox = Save_Mailbox_Get(save);
    app->partyMenu.options = Save_PlayerData_GetOptionsAddr(save);
    app->partyMenu.fieldSystem = app->fieldSystem;
    app->partyMenu.mode = WT_PARTY_MODE_SELECT;
    app->selectedSlot = -1;
    app->state = WT_APP_PICKER_FADE;
    WonderTrade_Fade(app, WT_FADE_OUT);
}

static void WonderTrade_FinishPicker(WonderTradeApp *app)
{
    int slot = app->partyMenu.selectedMonSlot;
    app->state = WT_APP_MENU;
    app->statusMessage = WonderTrade_Text_Choose;
    app->cursor = 0;
    WonderTrade_InitGraphics(app);

    if (slot >= Party_GetCount(&app->partySnapshot)) return;
    if (!WtGameSave_Eligible(app->game.save, (unsigned)slot)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Ineligible);
    } else if (!WtGameSave_Unique(app->game.save, (unsigned)slot, app->game.heapID)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Duplicate);
    } else {
        app->selectedSlot = slot;
        WonderTrade_Show(app, WT_APP_CONFIRM, WonderTrade_Text_Confirm);
    }
}

static WonderTradeApp *WonderTrade_Create(struct OverlayManager *man, FieldSystem *fieldSystem, int state, int message, BOOL visible)
{
    WonderTradeApp *app;
    Heap_Create(HEAP_ID_MAIN, HEAP_ID_WONDER_TRADE, WT_HEAP_SIZE);
    app = OverlayManager_CreateAndGetData(man, sizeof(WonderTradeApp), HEAP_ID_WONDER_TRADE);
    memset(app, 0, sizeof(*app));
    app->fieldSystem = fieldSystem;
    app->game.save = sub_020225F8(); app->game.heapID = HEAP_ID_WONDER_TRADE;
    /* The main loop adds play time to the save every second (PlayTimerUpdate);
     * pause it, as Platinum does, so the save stays still while Wonder Trade
     * hashes, verifies and saves it. */
    PlayTimerInit();
    app->selectedSlot = -1;
    app->messages = NewMsgDataFromNarc(MSGDATA_LOAD_DIRECT, NARC_MSGDATA_MSG, NARC_msg_narc_0624_bin, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_Alloc(HEAP_ID_WONDER_TRADE);
    app->state = state;
    app->statusMessage = message;
    if (visible) WonderTrade_InitGraphics(app);
    return app;
}

static BOOL WonderTrade_Init(struct OverlayManager *man, u32 *state)
{
    /* sub_020373D4 passes the launcher's argument, here the field system. */
    FieldSystem *fieldSystem = (FieldSystem *)OverlayManager_GetField18(man);
    /* Nothing can be traded or recovered until the adventure is on flash. */
    BOOL saved = WtGameSave_Written(sub_020225F8());
    WonderTradeApp *app = WonderTrade_Create(man, fieldSystem, saved ? WT_APP_CONNECTING : WT_APP_SAVE_FIRST,
        saved ? WonderTrade_Text_Connecting : WonderTrade_Text_SaveFirst, TRUE);
    (void)state;
    if (saved) WonderTradeNetwork_Start(&app->network, &app->game);
    WonderTrade_Draw(app);
    return TRUE;
}

static void WonderTrade_BootWorker(void *arg)
{
    WonderTradeBootWorker *worker = arg;
    WtJournalIO io = { &worker->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    worker->game.save = sub_020225F8(); worker->game.heapID = HEAP_ID_WONDER_TRADE;
    /* An unrecorded save layout never reaches the journal or a retail write. */
    worker->result = WtGameSave_LayoutKnown(worker->game.save) ? WtBoot_Check(&worker->game, io) : -1;
}

static BOOL WonderTrade_BootInit(struct OverlayManager *man, u32 *state)
{
    WonderTradeApp *app;
    WonderTradeBootWorker *worker;
    void *stackTop;
    (void)state;
    WtRecovery_Begin();
    /* A normal boot shows nothing: a screen opens only for recovery or a long check. */
    app = WonderTrade_Create(man, NULL, WT_APP_BOOT, WonderTrade_Text_CheckingSave, FALSE);
    worker = Heap_Alloc(HEAP_ID_WONDER_TRADE, sizeof(*worker));
    memset(worker, 0, sizeof(*worker)); app->boot = worker;
    stackTop = (void *)(((u32)worker->stack + WT_BOOT_STACK_SIZE) & ~7);
    OS_CreateThread(&worker->thread, WonderTrade_BootWorker, worker, stackTop,
        WT_BOOT_STACK_SIZE - 8, OS_GetThreadPriority(OS_GetCurrentThread()) + 1);
    memset((void *)(worker->thread.stackTop + sizeof(u32)), 0xa5,
        worker->thread.stackBottom - worker->thread.stackTop - 2 * sizeof(u32));
    OS_WakeupThreadDirect(&worker->thread);
    return TRUE;
}

static BOOL WonderTrade_BootPoll(WonderTradeApp *app)
{
    WonderTradeBootWorker *worker = app->boot;
    const unsigned char *stack;
    u32 size, unused;
    if (!OS_IsThreadTerminated(&worker->thread)) {
        /* Usually this finishes unseen. A longer check may be writing, so ask for patience. */
        if (!app->graphicsReady && ++app->bootFrames == WT_BOOT_SILENT_FRAMES) WonderTrade_InitGraphics(app);
        return FALSE;
    }
    OS_JoinThread(&worker->thread);
    stack = (const unsigned char *)(worker->thread.stackTop + sizeof(u32));
    size = worker->thread.stackBottom - worker->thread.stackTop - 2 * sizeof(u32);
    for (unused = 0; unused < size && stack[unused] == 0xa5; unused++) { (void)0; }
    if (unused < 256 || OS_GetStackStatus(&worker->thread) != OS_STACK_NO_ERROR || worker->result != 0) WtRecovery_Block();
    mbedtls_platform_zeroize(worker, sizeof(*worker));
    Heap_Free(worker); app->boot = NULL;
    if (WtRecovery_CanWrite()) return WonderTrade_Leave(app);
    app->state = WT_APP_RECOVERY;
    app->statusMessage = WonderTrade_Text_RecoveryRequired;
    if (app->graphicsReady) WonderTrade_Draw(app);
    else WonderTrade_InitGraphics(app);
    return FALSE;
}

static void WonderTrade_Back(WonderTradeApp *app)
{
    if (app->state == WT_APP_SAVE_FIRST) {
        WonderTrade_Leave(app);
    } else if (app->state == WT_APP_CONFIRM) {
        app->selectedSlot = -1;
        WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
    } else if (app->state == WT_APP_COLLECT_CONFIRM || app->state == WT_APP_CANCEL_CONFIRM) {
        WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Queued);
    } else if (app->state == WT_APP_MESSAGE && !app->network.needsEntropy) {
        WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
    } else {
        WonderTrade_Close(app);
    }
}

static void WonderTrade_Activate(WonderTradeApp *app, int choice)
{
    switch (app->state) {
    case WT_APP_RECOVERY:
        WonderTradeNetwork_Start(&app->network, &app->game);
        WonderTrade_Show(app, WT_APP_CONNECTING, WonderTrade_Text_Connecting);
        break;
    case WT_APP_CONNECTING:
    case WT_APP_PINGING:
        WonderTrade_Close(app);
        break;
    case WT_APP_CONFIRM:
    case WT_APP_COLLECT_CONFIRM:
    case WT_APP_CANCEL_CONFIRM:
        if (choice == 1) {
            WonderTrade_Back(app);
        } else {
            if (app->state == WT_APP_CONFIRM) WonderTradeNetwork_Trade(&app->network, app->selectedSlot);
            else WonderTradeNetwork_Collect(&app->network, app->state == WT_APP_CANCEL_CONFIRM);
            WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
        }
        break;
    case WT_APP_MESSAGE:
    case WT_APP_SAVE_FIRST:
        WonderTrade_Back(app);
        break;
    case WT_APP_ERROR:
        if (choice == 0) {
            WonderTradeNetwork_Close(&app->network);
            WonderTrade_Show(app, WT_APP_RESTART, WonderTrade_Text_Disconnecting);
        } else {
            WonderTrade_Close(app);
        }
        break;
    case WT_APP_MENU:
        if (choice == 0) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE && app->fieldSystem) WonderTrade_StartPicker(app);
            else WonderTrade_Show(app, WT_APP_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
        } else if (choice == 1) {
            /* Holding Select keeps the hardware-validation numbers available. */
            if (app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE,
                (gSystem.heldKeys & PAD_BUTTON_SELECT) ? WonderTrade_Text_Diagnostics : WonderTrade_Text_Explanation);
            else WonderTrade_Show(app, WT_APP_CANCEL_CONFIRM, WonderTrade_Text_ConfirmCancel);
        } else {
            WonderTrade_Close(app);
        }
        break;
    }
}

static void WonderTrade_HandleInput(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int count = WonderTrade_Actions(app, labels);
    int touched;
    if (app->pressTimer > 0) {
        /* Let the pressed button blink, then act on it. */
        if (--app->pressTimer > 0) WonderTrade_DrawButtons(app);
        else WonderTrade_Activate(app, app->cursor);
        return;
    }
    touched = WtMenu_Touch(&app->touchBlocked, gSystem.touchNew, gSystem.touchHeld,
        gSystem.touchX, gSystem.touchY, count);
    if (app->state != WT_APP_RECOVERY && (gSystem.newKeys & (PAD_BUTTON_B | PAD_BUTTON_X))) {
        PlaySE(SEQ_SE_DP_SELECT);
        if (app->state == WT_APP_MESSAGE && (gSystem.newKeys & PAD_BUTTON_X)) WonderTrade_Close(app);
        else WonderTrade_Back(app);
        return;
    }
    if (!count) return;
    if (touched >= 0 || (gSystem.newKeys & PAD_BUTTON_A)) {
        PlaySE(SEQ_SE_DP_SELECT);
        if (touched >= 0) app->cursor = touched;
        app->pressTimer = WT_PRESS_FRAMES;
        WonderTrade_DrawButtons(app);
    } else if (gSystem.newKeys & (PAD_KEY_UP | PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + ((gSystem.newKeys & PAD_KEY_UP) ? count - 1 : 1)) % count;
        PlaySE(SEQ_SE_DP_SELECT);
        WonderTrade_DrawButtons(app);
    }
}

static BOOL WonderTrade_Main(struct OverlayManager *man, u32 *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    BOOL couldLeave;
    (void)state;
    if (app->fadeInPending) {
        app->fadeInPending = FALSE;
        WonderTrade_Fade(app, WT_FADE_IN);
        return FALSE;
    }
    if (app->fading) {
        if (!IsPaletteFadeFinished()) return FALSE;
        app->fading = FALSE;
        if (app->leaving) return TRUE;
    }
    if (app->redraw && app->state != WT_APP_PICKER_FADE) WonderTrade_Draw(app);
    if (app->state == WT_APP_BOOT) return WonderTrade_BootPoll(app);
    if (app->state == WT_APP_RECOVERY || app->state == WT_APP_SAVE_FIRST) {
        WonderTrade_HandleInput(app);
        return FALSE;
    }
    couldLeave = app->network.canLeave;
    WonderTradeNetwork_Update(&app->network);
    if (couldLeave != app->network.canLeave) WonderTrade_Draw(app);

    if (app->state == WT_APP_PICKER_FADE) {
        /* The fade out has finished; the party screen sets up its own. */
        WonderTrade_FreeGraphics(app);
        app->picker = OverlayManager_New(&UNK_020F96DC, (s32 *)&app->partyMenu, HEAP_ID_WONDER_TRADE);
        app->state = WT_APP_PICKER;
        return FALSE;
    }
    if (app->state == WT_APP_PICKER) {
        if (OverlayManager_Run(app->picker)) {
            OverlayManager_Delete(app->picker);
            app->picker = NULL;
            WonderTrade_FinishPicker(app);
        }
        return FALSE;
    }
    if (app->state == WT_APP_CLOSING) {
        return app->network.state == WT_NET_CLOSED && WonderTrade_Leave(app);
    }
    if (app->state == WT_APP_RESTART) {
        if (app->network.state == WT_NET_CLOSED) {
            WonderTradeNetwork_Start(&app->network, &app->game);
            WonderTrade_Show(app, WT_APP_CONNECTING, WonderTrade_Text_Connecting);
        }
        return FALSE;
    }
    if (app->state == WT_APP_CONNECTING || app->state == WT_APP_PINGING) {
        WonderTrade_HandleInput(app);
        if (app->state == WT_APP_CLOSING || app->pressTimer > 0) return FALSE;
        if (app->network.state == WT_NET_ERROR) {
            WonderTrade_Show(app, WT_APP_ERROR, app->state == WT_APP_CONNECTING ? WonderTrade_Text_ConnectionError : WonderTrade_Text_PingError);
        } else if (app->state == WT_APP_CONNECTING && app->network.state == WT_NET_READY) {
            WonderTradeNetwork_Trade(&app->network, -1);
            WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
        } else if (app->network.state == WT_NET_RESPONSE) {
            app->selectedSlot = -1;
            if (app->network.needsEntropy) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_KeepPlaying);
            else if (!app->fieldSystem && app->network.canLeave) WonderTrade_Close(app);
            else if (app->network.completed) WonderTrade_Show(app, WT_APP_MESSAGE,
                app->network.returned ? WonderTrade_Text_Returned : WonderTrade_Text_Completed);
            else if (app->network.rejected) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Rejected);
            else if (app->network.refused) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_CannotHold);
            else if (app->network.superseded && app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Superseded);
            else if (app->network.waiting) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Waiting);
            else if (app->network.tradePhase == WT_JOURNAL_RECEIVED) WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_NoRoom);
            else WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
        }
        return FALSE;
    }

    WonderTrade_HandleInput(app);
    return FALSE;
}

static BOOL WonderTrade_Exit(struct OverlayManager *man, u32 *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    if (app->fieldSystem) PlayTimerStart(Save_PlayerData_GetIGTAddr(app->game.save));
    (void)state;
    WonderTrade_FreeGraphics(app);
    Heap_Free(app->bgConfig);
    DestroyMsgData(app->messages);
    OverlayManager_FreeData(man);
    Heap_Destroy(HEAP_ID_WONDER_TRADE);
    return TRUE;
}

const struct OverlayManagerTemplate gWonderTradeAppTemplate = {
    WonderTrade_Init, WonderTrade_Main, WonderTrade_Exit, SDK_OVERLAY_INVALID_ID
};

const struct OverlayManagerTemplate gWonderTradeBootAppTemplate = {
    WonderTrade_BootInit, WonderTrade_Main, WonderTrade_Exit, SDK_OVERLAY_INVALID_ID
};
