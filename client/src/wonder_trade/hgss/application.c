#include "wonder_trade/hgss/application.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "global.h"

#include "constants/pokemon.h"
#include "constants/sndseq.h"
#include "constants/species.h"

#include "data/sbox_gra.naix"
#include "graphic/plist_gra.naix"
#include "msgdata/msg.naix"
#include "msgdata/msg/msg_0829.h"

#include "bag.h"
#include "bg_window.h"
#include "field_system.h"
#include "font.h"
#include "gf_gfx_loader.h"
#include "gf_gfx_planes.h"
#include "heap.h"
#include "mail.h"
#include "menu_input_state.h"
#include "msgdata.h"
#include "options.h"
#include "party.h"
#include "party_menu.h"
#include "player_data.h"
#include "pm_string.h"
#include "pokemon.h"
#include "pokepic.h"
#include "render_window.h"
#include "save_arrays.h"
#include "screen_fade.h"
#include "string_util.h"
#include "system.h"
#include "text.h"
#include "unk_02005D10.h"
#include "unk_020210A0.h"
#include "unk_02018380.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/hgss/game_save.h"
#include "wonder_trade/menu_input.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"

/* The same screens, actions and flow as Platinum's Wonder Trade, drawn with
 * HeartGold and SoulSilver's own parts: the party screen's background, the
 * player's message box frame, the standard window frame, the touch-screen
 * buttons, the wait icon and the party menu itself. */

enum WonderTradeAppState {
    WT_APP_BOOT,
    WT_APP_RECOVERY,
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
    WT_APP_SAVE_FIRST,
};

#define WT_HEAP_SIZE 0x80000
#define WT_BOOT_STACK_SIZE 0x8000
#define WT_FADE_FRAMES 6
#define WT_PRESS_FRAMES 8

/* Main BG0 and BG1 share one character block. Tiles: */
#define WT_TITLE_TILE 1        /* 28x2 */
#define WT_MESSAGE_TILE 57     /* 23x4 */
#define WT_DETAILS_TILE 219    /* 16x10 */
#define WT_BODY_TILE 399       /* 26x16; the card shares it, never shown together */
#define WT_PREVIEW_TILE 847    /* 10x10 */
#define WT_FRAME_TILE 985      /* standard window frame, 9 tiles */
#define WT_MESSAGE_FRAME_TILE 994 /* message box frame and cursor, 30 tiles */
/* Main palettes. */
#define WT_PAL_BACKGROUND 0
#define WT_PAL_FONT 1
#define WT_PAL_FRAME 2
#define WT_PAL_MESSAGE_FRAME 3
#define WT_PAL_PREVIEW 4
/* Sub BG0 tiles and palettes. */
#define WT_SUB_TITLE_TILE 1    /* 28x2 */
#define WT_BUTTON_TILE 57      /* 16x2 each */
#define WT_SBOX_TILE 0x300
#define WT_PAL_SBOX 2

#define WT_COLOR_TEXT MAKE_TEXT_COLOR(1, 2, 15)
#define WT_COLOR_TITLE MAKE_TEXT_COLOR(1, 2, 0)
#define WT_COLOR_PANEL MAKE_TEXT_COLOR(1, 2, 0)
#define WT_COLOR_CODE MAKE_TEXT_COLOR(2, 15, 15)

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
    Window title, message, details, body, card, preview, subTitle;
    Window buttons[WT_MENU_MAX_ACTIONS];
    MsgData *messages;
    WaitingIcon *waitingIcon;
    PokepicTemplate previewTemplate;
    BOOL previewLoaded;
    OverlayManager *picker;
    PartyMenuArgs *partyMenu;
    Party *partySnapshot;
    int state;
    int statusMessage;
    int cursor;
    int selectedSlot;
    int pressTimer;
    BOOL touchBlocked;
    BOOL touchInput;
    BOOL graphicsReady;
    BOOL redraw;
    BOOL leaving;
    WonderTradeBootWorker *boot;
    int bootFrames;
    WtGame game;
} WonderTradeApp;

static void WonderTrade_Draw(WonderTradeApp *app);

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
    case WT_APP_RECOVERY:
        labels[0] = WonderTrade_Text_RecoveryAction;
        return 1;
    case WT_APP_SAVE_FIRST:
        labels[0] = WonderTrade_Text_Exit;
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

static SaveData *WonderTrade_Save(WonderTradeApp *app)
{
    return app->game.save;
}

static Party *WonderTrade_Party(WonderTradeApp *app)
{
    return SaveArray_Party_Get(WonderTrade_Save(app));
}

static void WonderTrade_VBlank(void *arg)
{
    WonderTradeApp *app = arg;
    DoScheduledBgGpuUpdates(app->bgConfig);
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
}

static void WonderTrade_InitBg(BgConfig *bgConfig, u8 layer, u8 screenBase, u8 charBase, u8 priority)
{
    BgTemplate template = {
        .x = 0, .y = 0, .bufferSize = 0x800, .baseTile = 0,
        .size = GF_BG_SCR_SIZE_256x256, .colorMode = GX_BG_COLORMODE_16,
        .screenBase = screenBase, .charBase = charBase,
        .bgExtPltt = GX_BG_EXTPLTT_01, .priority = priority, .areaOver = GX_BG_AREAOVER_XLU,
        .dummy = 0, .mosaic = FALSE,
    };
    InitBgFromTemplate(bgConfig, layer, &template, GF_BG_TYPE_TEXT);
    BgClearTilemapBufferAndCommit(bgConfig, layer);
}

/* The party screen's backdrop: a Poké Ball on soft teal. */
static void WonderTrade_LoadBackdrop(WonderTradeApp *app, GFBgLayer layer, enum GFPalLoadLocation palettes)
{
    GfGfxLoader_LoadCharData(NARC_graphic_plist_gra, NARC_plist_gra_plist_gra_00000012_NCGR, app->bgConfig, layer, 0, 0, FALSE, HEAP_ID_WONDER_TRADE);
    GfGfxLoader_LoadScrnData(NARC_graphic_plist_gra, NARC_plist_gra_plist_gra_00000014_NSCR, app->bgConfig, layer, 0, 0, FALSE, HEAP_ID_WONDER_TRADE);
    GfGfxLoader_GXLoadPal(NARC_graphic_plist_gra, NARC_plist_gra_plist_gra_00000013_NCLR, palettes, GF_PAL_SLOT_0_OFFSET, 0x20, HEAP_ID_WONDER_TRADE);
}

/* Retail applications build their screens hidden, then fade in (options_app.c). */
static void WonderTrade_InitGraphics(WonderTradeApp *app)
{
    static const GraphicsBanks banks = {
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
    static const GraphicsModes modes = {
        .dispMode = GX_DISPMODE_GRAPHICS,
        .bgMode = GX_BGMODE_0,
        .subMode = GX_BGMODE_0,
        ._2d3dMode = GX_BG0_AS_2D,
    };
    void *sbox;
    NNSG2dPaletteData *sboxPalette;
    int i;

    sub_0200FBF4(PM_LCD_TOP, RGB_BLACK);
    sub_0200FBF4(PM_LCD_BOTTOM, RGB_BLACK);
    Main_SetVBlankIntrCB(NULL, NULL);
    HBlankInterruptDisable();
    GfGfx_DisableEngineAPlanes();
    GfGfx_DisableEngineBPlanes();
    GX_SetVisiblePlane(0);
    GXS_SetVisiblePlane(0);
    GfGfx_SetBanks(&banks);
    GX_SetDispSelect(GX_DISP_SELECT_MAIN_SUB);
    G2_BlendNone();
    G2S_BlendNone();
    ResetVisibleHardwareWindows(PM_LCD_TOP);
    ResetVisibleHardwareWindows(PM_LCD_BOTTOM);
    EnableTouchPad();
    InitializeTouchPad(4);
    FontID_Alloc(FONT_UNK_4, HEAP_ID_WONDER_TRADE);
    SetBothScreensModesAndDisable(&modes);

    WonderTrade_InitBg(app->bgConfig, GF_BG_LYR_MAIN_0, GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000, 0);
    WonderTrade_InitBg(app->bgConfig, GF_BG_LYR_MAIN_1, GX_BG_SCRBASE_0x0800, GX_BG_CHARBASE_0x04000, 1);
    WonderTrade_InitBg(app->bgConfig, GF_BG_LYR_MAIN_3, GX_BG_SCRBASE_0x1000, GX_BG_CHARBASE_0x0c000, 3);
    WonderTrade_InitBg(app->bgConfig, GF_BG_LYR_SUB_0, GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000, 0);
    WonderTrade_InitBg(app->bgConfig, GF_BG_LYR_SUB_3, GX_BG_SCRBASE_0x1000, GX_BG_CHARBASE_0x0c000, 3);
    BG_ClearCharDataRange(GF_BG_LYR_MAIN_0, 0x20, 0, HEAP_ID_WONDER_TRADE);
    BG_ClearCharDataRange(GF_BG_LYR_SUB_0, 0x20, 0, HEAP_ID_WONDER_TRADE);

    WonderTrade_LoadBackdrop(app, GF_BG_LYR_MAIN_3, GF_PAL_LOCATION_MAIN_BG);
    WonderTrade_LoadBackdrop(app, GF_BG_LYR_SUB_3, GF_PAL_LOCATION_SUB_BG);
    LoadFontPal0(GF_PAL_LOCATION_MAIN_BG, GF_PAL_SLOT_1_OFFSET, HEAP_ID_WONDER_TRADE);
    LoadFontPal0(GF_PAL_LOCATION_SUB_BG, GF_PAL_SLOT_1_OFFSET, HEAP_ID_WONDER_TRADE);
    LoadUserFrameGfx1(app->bgConfig, GF_BG_LYR_MAIN_0, WT_FRAME_TILE, WT_PAL_FRAME, 0, HEAP_ID_WONDER_TRADE);
    LoadUserFrameGfx2(app->bgConfig, GF_BG_LYR_MAIN_0, WT_MESSAGE_FRAME_TILE, WT_PAL_MESSAGE_FRAME,
        Options_GetFrame(Save_PlayerData_GetOptionsAddr(WonderTrade_Save(app))), HEAP_ID_WONDER_TRADE);
    /* The touch-screen buttons of HeartGold and SoulSilver's field menus. */
    sbox = GfGfxLoader_LoadFromNarc(NARC_data_sbox_gra, sbox_gra_NCGR, FALSE, HEAP_ID_WONDER_TRADE, FALSE);
    {
        NNSG2dCharacterData *chars;
        NNS_G2dGetUnpackedBGCharacterData(sbox, &chars);
        BG_LoadCharTilesData(app->bgConfig, GF_BG_LYR_SUB_0, chars->pRawData, chars->szByte, WT_SBOX_TILE);
    }
    Heap_Free(sbox);
    sbox = GfGfxLoader_LoadFromNarc(NARC_data_sbox_gra, sbox_gra_NCLR, FALSE, HEAP_ID_WONDER_TRADE, FALSE);
    NNS_G2dGetUnpackedPaletteData(sbox, &sboxPalette);
    BG_LoadPlttData(GF_PAL_LOCATION_SUB_BG, sboxPalette->pRawData, 0x20, WT_PAL_SBOX * 0x20);
    Heap_Free(sbox);

    /* Every frame sits inside the backdrop's light panel, tiles 2 to 29: the
     * title one tile in from its edge, the field's message box (dialog_box.c)
     * at its own height but narrowed to the panel. */
    AddWindowParameterized(app->bgConfig, &app->title, GF_BG_LYR_MAIN_0, 3, 2, 28, 2, WT_PAL_FONT, WT_TITLE_TILE);
    AddWindowParameterized(app->bgConfig, &app->message, GF_BG_LYR_MAIN_0, 4, 19, 23, 4, WT_PAL_FONT, WT_MESSAGE_TILE);
    AddWindowParameterized(app->bgConfig, &app->details, GF_BG_LYR_MAIN_0, 3, 5, 16, 10, WT_PAL_FONT, WT_DETAILS_TILE);
    AddWindowParameterized(app->bgConfig, &app->body, GF_BG_LYR_MAIN_0, 3, 5, 26, 16, WT_PAL_FONT, WT_BODY_TILE);
    AddWindowParameterized(app->bgConfig, &app->card, GF_BG_LYR_MAIN_1, 3, 5, 26, 10, WT_PAL_FONT, WT_BODY_TILE);
    AddWindowParameterized(app->bgConfig, &app->preview, GF_BG_LYR_MAIN_0, 19, 5, 10, 10, WT_PAL_PREVIEW, WT_PREVIEW_TILE);
    AddWindowParameterized(app->bgConfig, &app->subTitle, GF_BG_LYR_SUB_0, 3, 2, 28, 2, WT_PAL_FONT, WT_SUB_TITLE_TILE);
    for (i = 0; i < WT_MENU_MAX_ACTIONS; i++) {
        AddWindowParameterized(app->bgConfig, &app->buttons[i], GF_BG_LYR_SUB_0,
            WT_MENU_LEFT / 8 + 1, (WT_MENU_TOP + i * WT_MENU_SPACING) / 8 + 1,
            WT_MENU_WIDTH / 8 - 2, WT_MENU_HEIGHT / 8 - 2, WT_PAL_SBOX, WT_BUTTON_TILE + i * 32);
    }
    app->previewLoaded = FALSE;
    app->graphicsReady = TRUE;
    app->touchBlocked = TRUE;
    app->pressTimer = 0;
    WonderTrade_Draw(app);
    Main_SetVBlankIntrCB(WonderTrade_VBlank, app);
    GfGfx_EngineATogglePlanes(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1 | GX_PLANEMASK_BG3, GF_PLANE_TOGGLE_ON);
    GfGfx_EngineBTogglePlanes(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG3, GF_PLANE_TOGGLE_ON);
    GfGfx_BothDispOn();
    BeginNormalPaletteFade(FADE_BOTH_SCREENS, FADE_TYPE_BRIGHTNESS_IN, FADE_TYPE_BRIGHTNESS_IN, RGB_BLACK, WT_FADE_FRAMES, 1, HEAP_ID_WONDER_TRADE);
}

static void WonderTrade_StopWaitingIcon(WonderTradeApp *app)
{
    if (app->waitingIcon == NULL) return;
    /* It restores the frame at the next VBlank, so redraw after that. */
    sub_0200F450(app->waitingIcon);
    app->waitingIcon = NULL;
    app->redraw = TRUE;
}

/* Called with both screens faded to black; leaves them black for the next
 * application, as retail applications do. */
static void WonderTrade_FreeGraphics(WonderTradeApp *app)
{
    int i;
    if (!app->graphicsReady) return;
    Main_SetVBlankIntrCB(NULL, NULL);
    HBlankInterruptDisable();
    GfGfx_DisableEngineAPlanes();
    GfGfx_DisableEngineBPlanes();
    GX_SetVisiblePlane(0);
    GXS_SetVisiblePlane(0);
    RemoveWindow(&app->title);
    RemoveWindow(&app->message);
    RemoveWindow(&app->details);
    RemoveWindow(&app->body);
    RemoveWindow(&app->card);
    RemoveWindow(&app->preview);
    RemoveWindow(&app->subTitle);
    for (i = 0; i < WT_MENU_MAX_ACTIONS; i++) RemoveWindow(&app->buttons[i]);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_MAIN_0);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_MAIN_1);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_MAIN_3);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_SUB_0);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_SUB_3);
    FontID_Release(FONT_UNK_4);
    DisableTouchPad();
    app->graphicsReady = FALSE;
}

static BOOL WonderTrade_FadeOut(WonderTradeApp *app)
{
    if (!app->graphicsReady) return FALSE;
    if (app->waitingIcon != NULL) {
        sub_0200F450(app->waitingIcon);
        app->waitingIcon = NULL;
    }
    BeginNormalPaletteFade(FADE_BOTH_SCREENS, FADE_TYPE_BRIGHTNESS_OUT, FADE_TYPE_BRIGHTNESS_OUT, RGB_BLACK, WT_FADE_FRAMES, 1, HEAP_ID_WONDER_TRADE);
    return TRUE;
}

/* Leave through a fade to black; the field fades back in from black. */
static BOOL WonderTrade_Leave(WonderTradeApp *app)
{
    if (!app->leaving) {
        app->leaving = TRUE;
        if (!WonderTrade_FadeOut(app)) return TRUE;
    }
    return IsPaletteFadeFinished();
}

static void WonderTrade_Print(WonderTradeApp *app, Window *window, enum Font font, int message, int x, int y, u32 color)
{
    String *text = NewString_ReadMsgData(app->messages, message);
    if (x < 0) x = (GetWindowWidth(window) * 8 - FontID_String_GetWidth(font, text, 0)) / 2;
    AddTextPrinterParameterizedWithColor(window, font, text, x, y, TEXT_SPEED_NOTRANSFER, color, NULL);
    String_Delete(text);
}

static void WonderTrade_PrintChars(WonderTradeApp *app, Window *window, const u16 *chars, int x, int y)
{
    String *text = String_New(POKEMON_NAME_LENGTH + PLAYER_NAME_LENGTH + 2, HEAP_ID_WONDER_TRADE);
    CopyU16ArrayToString(text, chars);
    AddTextPrinterParameterizedWithColor(window, FONT_SYSTEM, text, x, y, TEXT_SPEED_NOTRANSFER, WT_COLOR_PANEL, NULL);
    String_Delete(text);
}

/* Right-aligned, like the game's own stat columns. */
static void WonderTrade_PrintNumber(Window *window, int value, int x, int y, u32 color)
{
    String *number = String_New(12, HEAP_ID_WONDER_TRADE);
    String16_FormatInteger(number, value, 10, PRINTING_MODE_LEFT_ALIGN, TRUE);
    AddTextPrinterParameterizedWithColor(window, FONT_SYSTEM, number,
        x - FontID_String_GetWidth(FONT_SYSTEM, number, 0), y, TEXT_SPEED_NOTRANSFER, color, NULL);
    String_Delete(number);
}

/* The first 80x80 frame of the front picture, copied into a BG window. */
static void WonderTrade_Preview(WonderTradeApp *app, const PokepicTemplate *sprite, BOOL spindaSpots)
{
    if (!app->previewLoaded || memcmp(sprite, &app->previewTemplate, sizeof(*sprite)) != 0) {
        NNSG2dCharacterData *data;
        void *resource = GfGfxLoader_GetCharData((NarcId)sprite->narcID, sprite->charDataID, FALSE, &data, HEAP_ID_WONDER_TRADE);
        u8 *source = data->pRawData;
        u8 *tiles = app->preview.pixelBuffer;
        int x, y;
        UnscanPokepic(source, (NarcId)sprite->narcID);
        if (spindaSpots) RawChardata_PlaceSpindaSpots(source, sprite->personality, FALSE);
        for (y = 0; y < 80; y++) {
            for (x = 0; x < 10; x++) {
                memcpy(tiles + ((y / 8) * 10 + x) * 32 + (y % 8) * 4, source + y * data->W * 4 + x * 4, 4);
            }
        }
        Heap_Free(resource);
        GfGfxLoader_GXLoadPal((NarcId)sprite->narcID, sprite->palDataID, GF_PAL_LOCATION_MAIN_BG,
            (enum GFPalSlotOffset)(WT_PAL_PREVIEW * 0x20), 0x20, HEAP_ID_WONDER_TRADE);
        app->previewTemplate = *sprite;
        app->previewLoaded = TRUE;
    }
    PutWindowTilemap(&app->preview);
    ScheduleWindowCopyToVram(&app->preview);
}

static void WonderTrade_DrawPokemon(WonderTradeApp *app)
{
    PokepicTemplate sprite;
    String *species;
    memset(&sprite, 0, sizeof(sprite));
    FillWindowPixelBuffer(&app->card, 15);
    DrawFrameAndWindow1(&app->card, TRUE, WT_FRAME_TILE, WT_PAL_FRAME);
    ScheduleWindowCopyToVram(&app->card);
    FillWindowPixelBuffer(&app->details, 0);
    if (app->state == WT_APP_CONFIRM) {
        Pokemon *mon = Party_GetMonByIndex(app->partySnapshot, app->selectedSlot);
        u16 nickname[POKEMON_NAME_LENGTH + 1];
        BOOL egg = GetMonData(mon, MON_DATA_IS_EGG, NULL);
        u16 id = GetMonData(mon, MON_DATA_SPECIES, NULL);
        GetPokemonSpriteCharAndPlttNarcIds(&sprite, mon, MON_PIC_FACING_FRONT);
        WonderTrade_Preview(app, &sprite, !egg && id == SPECIES_SPINDA);
        species = GetSpeciesName(id, HEAP_ID_WONDER_TRADE);
        AddTextPrinterParameterizedWithColor(&app->details, FONT_SYSTEM, species, 0, 16, TEXT_SPEED_NOTRANSFER, WT_COLOR_PANEL, NULL);
        String_Delete(species);
        GetMonData(mon, MON_DATA_NICKNAME, nickname);
        WonderTrade_PrintChars(app, &app->details, nickname, 0, 32);
        if (!egg) {
            WonderTrade_Print(app, &app->details, FONT_SYSTEM, WonderTrade_Text_Level, 0, 64, WT_COLOR_PANEL);
            WonderTrade_PrintNumber(&app->details, GetMonData(mon, MON_DATA_LEVEL, NULL), 48, 64, WT_COLOR_PANEL);
        }
    } else {
        const WtReceivedPokemon *received = &app->network.received;
        GetMonSpriteCharAndPlttNarcIdsEx(&sprite, received->species, received->gender, MON_PIC_FACING_FRONT,
            received->shiny, received->form, received->personality);
        WonderTrade_Preview(app, &sprite, received->species == SPECIES_SPINDA);
        WonderTrade_Print(app, &app->details, FONT_SYSTEM, WonderTrade_Text_Received, 0, 0, WT_COLOR_PANEL);
        species = GetSpeciesName(received->species, HEAP_ID_WONDER_TRADE);
        AddTextPrinterParameterizedWithColor(&app->details, FONT_SYSTEM, species, 0, 16, TEXT_SPEED_NOTRANSFER, WT_COLOR_PANEL, NULL);
        String_Delete(species);
        if (received->hasNickname) WonderTrade_PrintChars(app, &app->details, received->nickname, 0, 32);
        WonderTrade_Print(app, &app->details, FONT_SYSTEM, WonderTrade_Text_OriginalTrainer, 0, 64, WT_COLOR_PANEL);
        WonderTrade_PrintChars(app, &app->details, received->originalTrainer, 40, 64);
    }
    PutWindowTilemap(&app->details);
    ScheduleWindowCopyToVram(&app->details);
}

static void WonderTrade_DrawButton(WonderTradeApp *app, int i, int label, BOOL selected)
{
    /* sbox_gra: top edge 0-2, sides 3-5, bottom edge 9-11; +12 is highlighted. */
    int tile = WT_SBOX_TILE + (selected ? 12 : 0);
    int x = WT_MENU_LEFT / 8, y = (WT_MENU_TOP + i * WT_MENU_SPACING) / 8;
    int w = WT_MENU_WIDTH / 8 - 2, h = WT_MENU_HEIGHT / 8 - 2;
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile, x, y, 1, 1, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 1, x + 1, y, w, 1, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 2, x + w + 1, y, 1, 1, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 3, x, y + 1, 1, h, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 5, x + w + 1, y + 1, 1, h, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 9, x, y + h + 1, 1, 1, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 10, x + 1, y + h + 1, w, 1, WT_PAL_SBOX);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, tile + 11, x + w + 1, y + h + 1, 1, 1, WT_PAL_SBOX);
    FillWindowPixelBuffer(&app->buttons[i], selected ? 6 : 3);
    WonderTrade_Print(app, &app->buttons[i], FONT_UNK_4, label, -1, 0,
        selected ? MAKE_TEXT_COLOR(4, 5, 6) : MAKE_TEXT_COLOR(1, 2, 3));
    PutWindowTilemap(&app->buttons[i]);
    ScheduleWindowCopyToVram(&app->buttons[i]);
}

static void WonderTrade_DrawButtons(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int i, count = WonderTrade_Actions(app, labels);
    if (app->cursor >= count) app->cursor = 0;
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, 0, 0, WT_MENU_TOP / 8, 32, 24 - WT_MENU_TOP / 8, 0);
    for (i = 0; i < count; i++) {
        /* A pressed button blinks, as in the game's own touch menus. */
        BOOL lit = i == app->cursor && (app->pressTimer == 0 || (app->pressTimer / 2) % 2 == 0);
        WonderTrade_DrawButton(app, i, labels[i], lit);
    }
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_SUB_0);
}

static BOOL WonderTrade_Waiting(const WonderTradeApp *app)
{
    return app->state == WT_APP_BOOT || app->state == WT_APP_CONNECTING || app->state == WT_APP_PINGING
        || app->state == WT_APP_RESTART || app->state == WT_APP_CLOSING;
}

static void WonderTrade_Draw(WonderTradeApp *app)
{
    BOOL preview, fullText;
    if (!app->graphicsReady) return;
    if (app->waitingIcon != NULL) {
        WonderTrade_StopWaitingIcon(app);
        return;
    }
    app->redraw = FALSE;
    preview = (app->state == WT_APP_CONFIRM && app->selectedSlot >= 0
        && app->selectedSlot < Party_GetCount(app->partySnapshot))
        || (app->state == WT_APP_MESSAGE && app->statusMessage == WonderTrade_Text_Completed && app->network.hasReceived);
    fullText = app->state == WT_APP_ERROR || app->state == WT_APP_RECOVERY
        || app->statusMessage == WonderTrade_Text_Explanation || app->statusMessage == WonderTrade_Text_Diagnostics
        || app->statusMessage == WonderTrade_Text_Ineligible;
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_MAIN_0, 0, 0, 0, 32, 32, 0);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_MAIN_1, 0, 0, 0, 32, 32, 0);
    FillBgTilemapRect(app->bgConfig, GF_BG_LYR_SUB_0, 0, 0, 0, 32, 32, 0);

    FillWindowPixelBuffer(&app->title, 0);
    WonderTrade_Print(app, &app->title, FONT_SYSTEM, WonderTrade_Text_Title, 0, 0, WT_COLOR_TITLE);
    PutWindowTilemap(&app->title);
    ScheduleWindowCopyToVram(&app->title);
    if (preview) WonderTrade_DrawPokemon(app);
    if (fullText) {
        FillWindowPixelBuffer(&app->body, 15);
        DrawFrameAndWindow1(&app->body, TRUE, WT_FRAME_TILE, WT_PAL_FRAME);
        WonderTrade_Print(app, &app->body, FONT_SYSTEM, app->statusMessage, 4, 0, WT_COLOR_TEXT);
        if (app->state == WT_APP_ERROR) {
            /* For support only: small and quiet, under the explanation. */
            WonderTrade_Print(app, &app->body, FONT_SYSTEM, WonderTrade_Text_ErrorCode, 4, 112, WT_COLOR_CODE);
            WonderTrade_PrintNumber(&app->body, app->network.error, 26 * 8 - 8, 112, WT_COLOR_CODE);
        } else if (app->statusMessage == WonderTrade_Text_Diagnostics) {
            static const int names[] = { WonderTrade_Text_ConnectMs, WonderTrade_Text_TlsBytes, WonderTrade_Text_StackBytes, WonderTrade_Text_FrameMs };
            const u32 values[] = { app->network.handshakeMs, app->network.tlsPeakBytes, app->network.stackBytes, app->network.maxFrameMs };
            int i;
            for (i = 0; i < 4; i++) {
                WonderTrade_Print(app, &app->body, FONT_SYSTEM, names[i], 4, 32 + i * 20, WT_COLOR_TEXT);
                WonderTrade_PrintNumber(&app->body, values[i], 26 * 8 - 8, 32 + i * 20, WT_COLOR_TEXT);
            }
        }
        ScheduleWindowCopyToVram(&app->body);
    } else {
        /* The player's own message box, where conversations show it. */
        FillWindowPixelBuffer(&app->message, 15);
        DrawFrameAndWindow2(&app->message, TRUE, WT_MESSAGE_FRAME_TILE, WT_PAL_MESSAGE_FRAME);
        WonderTrade_Print(app, &app->message, FONT_MESSAGE, app->statusMessage, 0, 0, WT_COLOR_TEXT);
        ScheduleWindowCopyToVram(&app->message);
        if (WonderTrade_Waiting(app)) app->waitingIcon = WaitingIcon_New(&app->message, WT_MESSAGE_FRAME_TILE);
    }

    FillWindowPixelBuffer(&app->subTitle, 0);
    WonderTrade_Print(app, &app->subTitle, FONT_SYSTEM, WonderTrade_Text_Title, 0, 0, WT_COLOR_TITLE);
    PutWindowTilemap(&app->subTitle);
    ScheduleWindowCopyToVram(&app->subTitle);
    WonderTrade_DrawButtons(app);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_0);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_1);
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

/* The game's own party menu picks the Pokémon, as for an in-game trade. */
static void WonderTrade_StartPicker(WonderTradeApp *app)
{
    /* A selection-only snapshot also isolates the party menu's bookkeeping. */
    Party_Copy(WonderTrade_Party(app), app->partySnapshot);
    memset(app->partyMenu, 0, sizeof(*app->partyMenu));
    app->partyMenu->party = app->partySnapshot;
    app->partyMenu->bag = Save_Bag_Get(WonderTrade_Save(app));
    app->partyMenu->mailbox = Save_Mailbox_Get(WonderTrade_Save(app));
    app->partyMenu->options = Save_PlayerData_GetOptionsAddr(WonderTrade_Save(app));
    app->partyMenu->context = PARTY_MENU_CONTEXT_SELECT_MON_NO_PROMPT;
    app->partyMenu->fieldSystem = app->fieldSystem;
    app->partyMenu->menuInputStatePtr = &app->fieldSystem->menuInputState;
    app->selectedSlot = -1;
    app->state = WT_APP_PICKER_FADE;
    WonderTrade_FadeOut(app);
}

static void WonderTrade_FinishPicker(WonderTradeApp *app)
{
    int slot = app->partyMenu->partySlot;
    app->state = WT_APP_MENU;
    app->statusMessage = WonderTrade_Text_Choose;
    app->cursor = 0;
    WonderTrade_InitGraphics(app);
    if (slot < 0 || slot >= Party_GetCount(app->partySnapshot)) {
        return;
    }
    if (!WtGameSave_Eligible(WonderTrade_Save(app), slot)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Ineligible);
    } else if (!WtGameSave_Unique(WonderTrade_Save(app), slot, HEAP_ID_WONDER_TRADE)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Duplicate);
    } else {
        app->selectedSlot = slot;
        WonderTrade_Show(app, WT_APP_CONFIRM, WonderTrade_Text_Confirm);
    }
}

static WonderTradeApp *WonderTrade_Create(OverlayManager *man, FieldSystem *fieldSystem, int state, int message, BOOL visible)
{
    WonderTradeApp *app;
    Heap_Create(HEAP_ID_3, HEAP_ID_WONDER_TRADE, WT_HEAP_SIZE);
    app = OverlayManager_CreateAndGetData(man, sizeof(WonderTradeApp), HEAP_ID_WONDER_TRADE);
    memset(app, 0, sizeof(*app));
    app->fieldSystem = fieldSystem;
    app->game.save = SaveData_Get(); app->game.heapID = HEAP_ID_WONDER_TRADE;
    /* The main loop adds play time to the save every second (sub_020183B0);
     * pause it, as Platinum does, so the save stays still while Wonder Trade
     * hashes, verifies and saves it. */
    sub_02018380();
    app->selectedSlot = -1;
    app->messages = NewMsgDataFromNarc(MSGDATA_LOAD_DIRECT, NARC_msgdata_msg, msg_0829, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_Alloc(HEAP_ID_WONDER_TRADE);
    app->partySnapshot = Heap_Alloc(HEAP_ID_WONDER_TRADE, SaveArray_Party_sizeof());
    Party_InitWithMaxSize(app->partySnapshot, PARTY_SIZE);
    app->partyMenu = Heap_Alloc(HEAP_ID_WONDER_TRADE, sizeof(*app->partyMenu));
    app->state = state;
    app->statusMessage = message;
    if (visible) WonderTrade_InitGraphics(app);
    return app;
}

BOOL WonderTradeApp_Init(OverlayManager *man, int *state)
{
    FieldSystem *fieldSystem = OverlayManager_GetArgs(man);
    SaveData *save = SaveData_Get();
    WonderTradeApp *app;
    (void)state;
    /* A deposit first writes both copies of an existing save; refuse a game
     * that was never saved before connecting, rather than fail later. */
    if (fieldSystem != NULL && !WtGameSave_Written(save)) {
        WonderTrade_Create(man, fieldSystem, WT_APP_SAVE_FIRST, WonderTrade_Text_SaveFirst, TRUE);
        return TRUE;
    }
    app = WonderTrade_Create(man, fieldSystem, WT_APP_CONNECTING, WonderTrade_Text_Connecting, TRUE);
    WonderTradeNetwork_Start(&app->network, &app->game);
    WonderTrade_Draw(app);
    return TRUE;
}

static void WonderTrade_BootWorker(void *arg)
{
    WonderTradeBootWorker *worker = arg;
    WtJournalIO io = { &worker->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    worker->game.save = SaveData_Get(); worker->game.heapID = HEAP_ID_WONDER_TRADE;
    /* An unrecorded save layout never reaches the journal or a retail write. */
    worker->result = WtGameSave_LayoutKnown(worker->game.save) ? WtBoot_Check(&worker->game, io) : -1;
}

BOOL WonderTradeApp_BootInit(OverlayManager *man, int *state)
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
    if (worker == NULL) return WonderTrade_Leave(app);
    if (!OS_IsThreadTerminated(&worker->thread)) {
        /* Usually this finishes unseen; only a slow check opens the screen early. */
        if (!app->graphicsReady && ++app->bootFrames == WT_BOOT_SILENT_FRAMES) WonderTrade_InitGraphics(app);
        else if (app->redraw) WonderTrade_Draw(app);
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
    if (app->state == WT_APP_CONFIRM) {
        app->selectedSlot = -1;
        WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
    } else if (app->state == WT_APP_COLLECT_CONFIRM || app->state == WT_APP_CANCEL_CONFIRM) {
        WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Queued);
    } else if (app->state == WT_APP_MESSAGE && !app->network.needsEntropy) {
        WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
    } else if (app->state == WT_APP_SAVE_FIRST) {
        WonderTrade_Leave(app);
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
        if (--app->pressTimer > 0) {
            WonderTrade_DrawButtons(app);
            return;
        }
        if (app->fieldSystem) MenuInputStateMgr_SetState(&app->fieldSystem->menuInputState,
            app->touchInput ? MENU_INPUT_STATE_TOUCH : MENU_INPUT_STATE_BUTTONS);
        WonderTrade_Activate(app, app->cursor);
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
        app->touchInput = touched >= 0;
        app->pressTimer = WT_PRESS_FRAMES;
        WonderTrade_DrawButtons(app);
    } else if (gSystem.newKeys & (PAD_KEY_UP | PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + ((gSystem.newKeys & PAD_KEY_UP) ? count - 1 : 1)) % count;
        PlaySE(SEQ_SE_DP_SELECT);
        WonderTrade_DrawButtons(app);
    }
}

BOOL WonderTradeApp_Main(OverlayManager *man, int *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    BOOL couldLeave;
    (void)state;
    if (app->state == WT_APP_BOOT) return WonderTrade_BootPoll(app);
    if (app->leaving) return WonderTrade_Leave(app);
    if (app->redraw) WonderTrade_Draw(app);
    if (app->state == WT_APP_SAVE_FIRST) {
        if (IsPaletteFadeFinished()) WonderTrade_HandleInput(app);
        return FALSE;
    }
    if (app->state == WT_APP_RECOVERY) {
        if (IsPaletteFadeFinished()) WonderTrade_HandleInput(app);
        return FALSE;
    }
    couldLeave = app->network.canLeave;
    WonderTradeNetwork_Update(&app->network);
    if (app->graphicsReady && couldLeave != app->network.canLeave) WonderTrade_Draw(app);

    if (app->state == WT_APP_PICKER_FADE) {
        if (IsPaletteFadeFinished()) {
            WonderTrade_FreeGraphics(app);
            app->picker = OverlayManager_New(&gOverlayTemplate_PartyMenu, app->partyMenu, HEAP_ID_WONDER_TRADE);
            app->state = WT_APP_PICKER;
        }
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
        if (app->network.state == WT_NET_CLOSED) return WonderTrade_Leave(app);
        return FALSE;
    }
    if (!IsPaletteFadeFinished()) return FALSE;
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

BOOL WonderTradeApp_Exit(OverlayManager *man, int *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    (void)state;
    if (app->fieldSystem) sub_0201838C(Save_PlayerData_GetIGTAddr(app->game.save));
    WonderTrade_FreeGraphics(app);
    Heap_Free(app->partyMenu);
    Heap_Free(app->partySnapshot);
    Heap_Free(app->bgConfig);
    DestroyMsgData(app->messages);
    OverlayManager_FreeData(man);
    Heap_Destroy(HEAP_ID_WONDER_TRADE);
    return TRUE;
}
