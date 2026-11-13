#include "wonder_trade/platinum/application.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "applications/party_menu/defs.h"
#include "applications/party_menu/main.h"
#include "field/field_system.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/network.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/platinum/game_save.h"
#include "wonder_trade/menu_input.h"

#include "bag.h"
#include "bg_window.h"
#include "font.h"
#include "game_options.h"
#include "graphics.h"
#include "gx_layers.h"
#include "heap.h"
#include "mail.h"
#include "message.h"
#include "party.h"
#include "play_time_manager.h"
#include "pokemon.h"
#include "pokemon_sprite.h"
#include "render_window.h"
#include "save_player.h"
#include "screen_fade.h"
#include "sound_playback.h"
#include "system.h"
#include "text.h"
#include "touch_pad.h"

#include "res/text/bank/wonder_trade.h"

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
};

#define WT_BOOT_STACK_SIZE 0x8000
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
    Window title, message, details, body, subTitle, preview;
    Window actions[WT_MENU_MAX_ACTIONS];
    PokemonSpriteTemplate previewTemplate;
    u16 buttonTilemap[32 * 24];
    BOOL previewLoaded;
    BOOL touchBlocked;
    MessageLoader *messages;
    ApplicationManager *picker;
    Party partySnapshot;
    PartyMenu partyMenu;
    int state;
    int statusMessage;
    int cursor;
    int selectedSlot;
    BOOL graphicsReady;
    WonderTradeBootWorker *boot;
    int bootFrames;
    WtGame game;
} WonderTradeApp;

static void WonderTrade_Draw(WonderTradeApp *app);

#define WT_FRAME_TILE 961
#define WT_MESSAGE_TILE 970

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
    Bg_RunScheduledUpdates(app->bgConfig);
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
}

static void WonderTrade_InitGraphics(WonderTradeApp *app)
{
    const GXBanks banks = {
        GX_VRAM_BG_128_A, GX_VRAM_BGEXTPLTT_NONE, GX_VRAM_SUB_BG_128_C, GX_VRAM_SUB_BGEXTPLTT_NONE, GX_VRAM_OBJ_64_E, GX_VRAM_OBJEXTPLTT_NONE, GX_VRAM_SUB_OBJ_16_I, GX_VRAM_SUB_OBJEXTPLTT_NONE, GX_VRAM_TEX_0_B, GX_VRAM_TEXPLTT_01_FG
    };
    const GraphicsModes modes = {
        GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BGMODE_0, GX_BG0_AS_2D
    };
    BgTemplate bg = {
        .bufferSize = 0x800, .screenSize = BG_SCREEN_SIZE_256x256,
        .colorMode = GX_BG_COLORMODE_16, .screenBase = GX_BG_SCRBASE_0xf800,
        .charBase = GX_BG_CHARBASE_0x00000, .bgExtPltt = GX_BG_EXTPLTT_01, .priority = 0
    };
    /* The original GTS menu tiles, with the trade screens' blue/cyan accents. */
    static const u16 backgroundPalette[16] = {
        0, GX_RGB(31,31,31), GX_RGB(9,18,27), GX_RGB(13,22,31),
        GX_RGB(14,26,29), GX_RGB(19,27,30), 0, GX_RGB(22,30,30),
        GX_RGB(27,31,31), GX_RGB(21,26,31), 0, 0, 0, 0, 0, 0
    };
    static const u16 buttonPalette[16] = {
        0, GX_RGB(25,30,31), 0, 0, 0, 0, 0, 0, 0,
        GX_RGB(19,27,30), GX_RGB(22,30,30), GX_RGB(9,18,27), 0, 0, 0, 0
    };
    static const u16 selectedPalette[16] = {
        0, GX_RGB(31,31,25), 0, 0, 0, 0, 0, 0, 0,
        GX_RGB(31,24,12), GX_RGB(31,28,18), GX_RGB(30,11,5), 0, 0, 0, 0
    };
    SaveData *save = app->fieldSystem ? app->fieldSystem->saveData : SaveData_Ptr();

    SetVBlankCallback(NULL, NULL);
    DisableHBlank();
    // Keep input polling from waiting on ARM7 while its Wi-Fi driver starts.
    InitializeTouchPad(4);
    GXLayers_DisableEngineALayers();
    GXLayers_DisableEngineBLayers();
    GX_SetVisiblePlane(0);
    GXS_SetVisiblePlane(0);
    GXLayers_SetBanks(&banks);
    SetAllGraphicsModes(&modes);
    G2_BlendNone();
    G2S_BlendNone();
    ResetVisibleHardwareWindows(DS_SCREEN_MAIN);
    ResetVisibleHardwareWindows(DS_SCREEN_SUB);
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_MAIN_0, &bg, BG_TYPE_STATIC);
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_SUB_0, &bg, BG_TYPE_STATIC);
    Bg_ClearTilesRange(BG_LAYER_MAIN_0, 32, 0, HEAP_ID_WONDER_TRADE);
    Bg_ClearTilesRange(BG_LAYER_SUB_0, 32, 0, HEAP_ID_WONDER_TRADE);
    Bg_ClearTilemap(app->bgConfig, BG_LAYER_MAIN_0);
    Bg_ClearTilemap(app->bgConfig, BG_LAYER_SUB_0);
    bg.screenBase = GX_BG_SCRBASE_0xf000;
    bg.charBase = GX_BG_CHARBASE_0x08000;
    bg.priority = 2;
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_SUB_1, &bg, BG_TYPE_STATIC);
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_MAIN_1, &bg, BG_TYPE_STATIC);
    bg.screenBase = GX_BG_SCRBASE_0xe800;
    bg.priority = 1;
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_SUB_2, &bg, BG_TYPE_STATIC);
    bg.charBase = GX_BG_CHARBASE_0x0c000;
    Bg_InitFromTemplate(app->bgConfig, BG_LAYER_MAIN_2, &bg, BG_TYPE_STATIC);

    Graphics_LoadTilesToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 14, app->bgConfig, BG_LAYER_MAIN_1, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    Graphics_LoadTilemapToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 29, app->bgConfig, BG_LAYER_MAIN_1, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    Bg_ChangeTilemapRectPalette(app->bgConfig, BG_LAYER_MAIN_1, 0, 0, 32, 24, 9);
    Bg_ScheduleTilemapTransfer(app->bgConfig, BG_LAYER_MAIN_1);
    Graphics_LoadTilesToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 17, app->bgConfig, BG_LAYER_MAIN_2, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    Graphics_LoadTilemapToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 25, app->bgConfig, BG_LAYER_MAIN_2, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    /* Keep only the listing card, below the header and above the message frame. */
    u16 *listing = Bg_GetTilemapBuffer(app->bgConfig, BG_LAYER_MAIN_2);
    memmove(listing + 3 * 32, listing + 32, 12 * 32 * sizeof(*listing));
    Bg_FillTilemapRect(app->bgConfig, BG_LAYER_MAIN_2, 0, 0, 0, 32, 3, 0);
    Bg_FillTilemapRect(app->bgConfig, BG_LAYER_MAIN_2, 0, 0, 15, 32, 9, 0);
    Bg_ScheduleTilemapTransfer(app->bgConfig, BG_LAYER_MAIN_2);
    Graphics_LoadTilesToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 14, app->bgConfig, BG_LAYER_SUB_1, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    Graphics_LoadTilemapToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 29, app->bgConfig, BG_LAYER_SUB_1, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    Graphics_LoadTilemapToBgLayer(NARC_INDEX_GRAPHIC__WORLDTRADE, 30, app->bgConfig, BG_LAYER_SUB_2, 0, 0, TRUE, HEAP_ID_WONDER_TRADE);
    memcpy(app->buttonTilemap, Bg_GetTilemapBuffer(app->bgConfig, BG_LAYER_SUB_2), sizeof(app->buttonTilemap));
    Graphics_LoadPalette(NARC_INDEX_GRAPHIC__WORLDTRADE, 7, PAL_LOAD_MAIN_BG, 0, 3 * 32, HEAP_ID_WONDER_TRADE);
    Bg_LoadPalette(BG_LAYER_MAIN_1, backgroundPalette, sizeof(backgroundPalette), 9 * 32);
    Bg_LoadPalette(BG_LAYER_SUB_1, backgroundPalette, sizeof(backgroundPalette), 0);
    Bg_LoadPalette(BG_LAYER_SUB_2, buttonPalette, sizeof(buttonPalette), 32);
    Bg_LoadPalette(BG_LAYER_SUB_2, selectedPalette, sizeof(selectedPalette), 3 * 32);
    Font_LoadScreenIndicatorsPalette(PAL_LOAD_MAIN_BG, 6 * 32, HEAP_ID_WONDER_TRADE);
    Font_LoadScreenIndicatorsPalette(PAL_LOAD_SUB_BG, 13 * 32, HEAP_ID_WONDER_TRADE);
    LoadStandardWindowGraphics(app->bgConfig, BG_LAYER_MAIN_0, WT_FRAME_TILE, 7, STANDARD_WINDOW_SYSTEM, HEAP_ID_WONDER_TRADE);
    LoadMessageBoxGraphics(app->bgConfig, BG_LAYER_MAIN_0, WT_MESSAGE_TILE, 8, Options_Frame(SaveData_GetOptions(save)), HEAP_ID_WONDER_TRADE);
    Window_Add(app->bgConfig, &app->title, BG_LAYER_MAIN_0, 2, 1, 28, 2, 6, 1);
    Window_Add(app->bgConfig, &app->message, BG_LAYER_MAIN_0, 2, 17, 27, 6, 6, 61);
    Window_Add(app->bgConfig, &app->details, BG_LAYER_MAIN_0, 2, 4, 18, 10, 6, 223);
    Window_Add(app->bgConfig, &app->body, BG_LAYER_MAIN_0, 2, 5, 28, 16, 6, 413);
    Window_Add(app->bgConfig, &app->subTitle, BG_LAYER_SUB_0, 2, 1, 28, 2, 13, 1);
    for (int i = 0; i < WT_MENU_MAX_ACTIONS; i++) {
        Window_Add(app->bgConfig, &app->actions[i], BG_LAYER_SUB_0,
            WT_MENU_LEFT / 8, (WT_MENU_TOP + i * WT_MENU_SPACING) / 8,
            WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, 13, 61 + i * 72);
    }
    Window_Add(app->bgConfig, &app->preview, BG_LAYER_MAIN_0, 21, 4, 10, 10, 4, 861);
    app->previewLoaded = FALSE;
    app->graphicsReady = TRUE;
    app->touchBlocked = TRUE;
    WonderTrade_Draw(app);
    SetVBlankCallback(WonderTrade_VBlank, app);
    GXLayers_EngineAToggleLayers(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1, TRUE);
    GXLayers_EngineBToggleLayers(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1 | GX_PLANEMASK_BG2, TRUE);
    GXLayers_TurnBothDispOn();
    ResetScreenMasterBrightness(DS_SCREEN_MAIN);
    ResetScreenMasterBrightness(DS_SCREEN_SUB);
}

static void WonderTrade_FreeGraphics(WonderTradeApp *app)
{
    if (!app->graphicsReady) return;
    SetVBlankCallback(NULL, NULL);
    DisableTouchPad();
    Window_Remove(&app->preview);
    Window_Remove(&app->title);
    Window_Remove(&app->message);
    Window_Remove(&app->details);
    Window_Remove(&app->body);
    Window_Remove(&app->subTitle);
    for (int i = 0; i < WT_MENU_MAX_ACTIONS; i++) Window_Remove(&app->actions[i]);
    for (int i = 0; i < 3; i++) {
        Bg_FreeTilemapBuffer(app->bgConfig, BG_LAYER_MAIN_0 + i);
        Bg_FreeTilemapBuffer(app->bgConfig, BG_LAYER_SUB_0 + i);
    }
    GXLayers_DisableEngineALayers();
    GXLayers_DisableEngineBLayers();
    app->graphicsReady = FALSE;
}

static void WonderTrade_Print(WonderTradeApp *app, Window *window, int message, int x, int y, u32 color)
{
    String *text = MessageLoader_GetNewString(app->messages, message);
    if (x < 0) x = (Window_GetWidth(window) * 8 - Font_CalcStringWidth(FONT_SYSTEM, text, 0)) / 2;
    Text_AddPrinterWithParamsAndColor(window, FONT_SYSTEM, text, x, y, TEXT_SPEED_NO_TRANSFER, color, NULL);
    String_Free(text);
}

static void WonderTrade_PrintChars(Window *window, const charcode_t *chars, int x, int y)
{
    String *text = String_Init(MON_NAME_LEN + TRAINER_NAME_LEN + 2, HEAP_ID_WONDER_TRADE);
    String_CopyChars(text, chars);
    Text_AddPrinterWithParamsAndColor(window, FONT_SYSTEM, text, x, y, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(15, 2, 0), NULL);
    String_Free(text);
}

static void WonderTrade_PrintNumber(Window *window, int value, int y)
{
    String *number = String_Init(12, HEAP_ID_WONDER_TRADE);
    String_FormatInt(number, value, 10, PADDING_MODE_NONE, CHARSET_MODE_EN);
    int x = Window_GetWidth(window) * 8 - Font_CalcStringWidth(FONT_SYSTEM, number, 0) - 8;
    Text_AddPrinterWithParamsAndColor(window, FONT_SYSTEM, number, x, y, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
    String_Free(number);
}

static void WonderTrade_Preview(WonderTradeApp *app, PokemonSpriteTemplate *sprite)
{
    if (!app->previewLoaded || memcmp(sprite, &app->previewTemplate, sizeof(*sprite)) != 0) {
        NNSG2dCharacterData *data;
        void *resource = Graphics_GetCharData(sprite->narcID, sprite->character, FALSE, &data, HEAP_ID_WONDER_TRADE);
        u8 *source = data->pRawData;
        u8 *tiles = app->preview.pixels;
        PokemonSprite_Decrypt(source, sprite->narcID);
        if (sprite->spindaSpots) PokemonSprite_DrawSpindaSpots(source, sprite->personality, FALSE);
        /* Copy the first 80x80 frame from the two-frame bitmap into BG tiles. */
        for (int y = 0; y < 80; y++) {
            for (int x = 0; x < 10; x++) {
                memcpy(tiles + ((y / 8) * 10 + x) * 32 + (y % 8) * 4,
                    source + y * data->W * 4 + x * 4, 4);
            }
        }
        Heap_Free(resource);
        Graphics_LoadPalette(sprite->narcID, sprite->palette, PAL_LOAD_MAIN_BG, 4 * 32, 32, HEAP_ID_WONDER_TRADE);
        app->previewTemplate = *sprite;
        app->previewLoaded = TRUE;
    }
    Window_ScheduleCopyToVRAM(&app->preview);
}

static void WonderTrade_DrawPokemon(WonderTradeApp *app)
{
    PokemonSpriteTemplate sprite;
    charcode_t species[MON_NAME_LEN + 1];
    memset(&sprite, 0, sizeof(sprite));
    Window_FillTilemap(&app->details, 0);
    if (app->state == WT_APP_CONFIRM) {
        Pokemon *mon = Party_GetPokemonBySlotIndex(&app->partySnapshot, app->selectedSlot);
        charcode_t nickname[MON_NAME_LEN + 1];
        int id = Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL);
        MessageLoader_GetSpeciesName(id, HEAP_ID_WONDER_TRADE, species);
        Pokemon_GetValue(mon, MON_DATA_NICKNAME, nickname);
        Pokemon_BuildSpriteTemplate(&sprite, mon, FACE_FRONT);
        WonderTrade_Preview(app, &sprite);
        WonderTrade_PrintChars(&app->details, species, 0, 16);
        WonderTrade_PrintChars(&app->details, nickname, 0, 32);
    } else {
        const WtReceivedPokemon *received = &app->network.received;
        MessageLoader_GetSpeciesName(received->species, HEAP_ID_WONDER_TRADE, species);
        BuildPokemonSpriteTemplate(&sprite, received->species, received->gender, FACE_FRONT,
            received->shiny, received->form, received->personality);
        WonderTrade_Preview(app, &sprite);
        WonderTrade_Print(app, &app->details, WonderTrade_Text_Received, 0, 0, TEXT_COLOR(15, 2, 0));
        WonderTrade_PrintChars(&app->details, species, 0, 16);
        if (received->hasNickname) WonderTrade_PrintChars(&app->details, received->nickname, 0, 32);
        WonderTrade_Print(app, &app->details, WonderTrade_Text_OriginalTrainer, 0, 64, TEXT_COLOR(15, 2, 0));
        WonderTrade_PrintChars(&app->details, received->originalTrainer, 40, 64);
    }
    Window_ScheduleCopyToVRAM(&app->details);
}

static void WonderTrade_Draw(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int count = WonderTrade_Actions(app, labels);
    BOOL preview = (app->state == WT_APP_CONFIRM && app->selectedSlot >= 0
        && app->selectedSlot < Party_GetCurrentCount(&app->partySnapshot))
        || (app->state == WT_APP_MESSAGE && app->statusMessage == WonderTrade_Text_Completed && app->network.hasReceived);
    BOOL fullText = app->state == WT_APP_ERROR || app->state == WT_APP_RECOVERY
        || app->statusMessage == WonderTrade_Text_Explanation || app->statusMessage == WonderTrade_Text_Diagnostics;
    Bg_ClearTilemap(app->bgConfig, BG_LAYER_MAIN_0);
    Bg_ClearTilemap(app->bgConfig, BG_LAYER_SUB_0);
    GXLayers_EngineAToggleLayers(GX_PLANEMASK_BG2, preview);
    Window_FillTilemap(&app->title, 0);
    WonderTrade_Print(app, &app->title, WonderTrade_Text_Title, 0, 2, TEXT_COLOR(15, 2, 0));
    Window_ScheduleCopyToVRAM(&app->title);
    if (preview) WonderTrade_DrawPokemon(app);
    if (fullText) {
        Window_FillTilemap(&app->body, 15);
        Window_DrawStandardFrame(&app->body, TRUE, WT_FRAME_TILE, 7);
        WonderTrade_Print(app, &app->body, app->statusMessage, 4, 0, TEXT_COLOR(1, 2, 15));
        if (app->state == WT_APP_ERROR) {
            WonderTrade_Print(app, &app->body, WonderTrade_Text_ErrorCode, 4, 80, TEXT_COLOR(1, 2, 15));
            WonderTrade_PrintNumber(&app->body, app->network.error, 96);
        } else if (app->statusMessage == WonderTrade_Text_Diagnostics) {
            const int names[] = { WonderTrade_Text_ConnectMs, WonderTrade_Text_TlsBytes, WonderTrade_Text_StackBytes, WonderTrade_Text_FrameMs };
            const u32 values[] = { app->network.handshakeMs, app->network.tlsPeakBytes, app->network.stackBytes, app->network.maxFrameMs };
            for (int i = 0; i < 4; i++) {
                WonderTrade_Print(app, &app->body, names[i], 4, 32 + i * 20, TEXT_COLOR(1, 2, 15));
                WonderTrade_PrintNumber(&app->body, values[i], 32 + i * 20);
            }
        }
        Window_ScheduleCopyToVRAM(&app->body);
    } else {
        Window_SetYPos(&app->message, preview ? 17 : 9);
        Window_FillTilemap(&app->message, 15);
        Window_DrawMessageBoxWithScrollCursor(&app->message, TRUE, WT_MESSAGE_TILE, 8);
        WonderTrade_Print(app, &app->message, app->statusMessage, 0, 0, TEXT_COLOR(1, 2, 15));
        Window_ScheduleCopyToVRAM(&app->message);
    }
    Window_FillTilemap(&app->subTitle, 0);
    WonderTrade_Print(app, &app->subTitle, WonderTrade_Text_Title, 0, 2, TEXT_COLOR(15, 2, 0));
    Window_ScheduleCopyToVRAM(&app->subTitle);
    if (app->cursor >= count) app->cursor = 0;
    Bg_LoadTilemapBuffer(app->bgConfig, BG_LAYER_SUB_2, app->buttonTilemap, sizeof(app->buttonTilemap));
    for (int i = 0; i < WT_MENU_MAX_ACTIONS; i++) {
        int y = (WT_MENU_TOP + i * WT_MENU_SPACING) / 8;
        if (i >= count) {
            Bg_FillTilemapRect(app->bgConfig, BG_LAYER_SUB_2, 0, WT_MENU_LEFT / 8, y, WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, 0);
            continue;
        }
        Bg_ChangeTilemapRectPalette(app->bgConfig, BG_LAYER_SUB_2, WT_MENU_LEFT / 8, y,
            WT_MENU_WIDTH / 8, WT_MENU_HEIGHT / 8, i == app->cursor ? 3 : 1);
        Window_FillTilemap(&app->actions[i], 0);
        WonderTrade_Print(app, &app->actions[i], labels[i], -1, 8, TEXT_COLOR(1, 2, 0));
        Window_ScheduleCopyToVRAM(&app->actions[i]);
    }
    Bg_ScheduleTilemapTransfer(app->bgConfig, BG_LAYER_MAIN_0);
    Bg_ScheduleTilemapTransfer(app->bgConfig, BG_LAYER_SUB_0);
    Bg_ScheduleTilemapTransfer(app->bgConfig, BG_LAYER_SUB_2);
}

static void WonderTrade_Show(WonderTradeApp *app, int state, int message)
{
    app->state = state;
    app->statusMessage = message;
    app->cursor = 0;
    app->touchBlocked = TRUE;
    WonderTrade_Draw(app);
}

static void WonderTrade_Close(WonderTradeApp *app)
{
    if (!app->network.canLeave) return;
    WonderTradeNetwork_Close(&app->network);
    WonderTrade_Show(app, WT_APP_CLOSING, WonderTrade_Text_Disconnecting);
}

static void WonderTrade_StartPicker(WonderTradeApp *app)
{
    // A selection-only snapshot also isolates the party app's form bookkeeping.
    Party_Copy(SaveData_GetParty(app->fieldSystem->saveData), &app->partySnapshot);
    memset(&app->partyMenu, 0, sizeof(app->partyMenu));
    app->partyMenu.party = &app->partySnapshot;
    app->partyMenu.bag = SaveData_GetBag(app->fieldSystem->saveData);
    app->partyMenu.mailbox = SaveData_GetMailbox(app->fieldSystem->saveData);
    app->partyMenu.options = SaveData_GetOptions(app->fieldSystem->saveData);
    app->partyMenu.type = PARTY_MENU_TYPE_BASIC;
    app->partyMenu.mode = PARTY_MENU_MODE_SELECT_NO_PROMPT;
    app->partyMenu.fieldSystem = app->fieldSystem;
    app->selectedSlot = -1;
    app->state = WT_APP_PICKER_FADE;
    StartScreenFade(FADE_BOTH_SCREENS, FADE_TYPE_BRIGHTNESS_OUT, FADE_TYPE_BRIGHTNESS_OUT, COLOR_BLACK, 6, 1, HEAP_ID_WONDER_TRADE);
}

static void WonderTrade_FinishPicker(WonderTradeApp *app)
{
    int slot = app->partyMenu.selectedMonSlot;
    app->state = WT_APP_MENU;
    app->statusMessage = WonderTrade_Text_Choose;
    app->cursor = 0;
    WonderTrade_InitGraphics(app);

    if (slot < 0 || slot >= Party_GetCurrentCount(&app->partySnapshot)) {
        return;
    }

    if (!WtGameSave_Eligible(app->fieldSystem->saveData, slot)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Ineligible);
        return;
    }

    if (!WtGameSave_Unique(app->fieldSystem->saveData, slot, HEAP_ID_WONDER_TRADE)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Duplicate);
        return;
    }

    app->selectedSlot = slot;
    WonderTrade_Show(app, WT_APP_CONFIRM, WonderTrade_Text_Confirm);
}

static WonderTradeApp *WonderTrade_Create(ApplicationManager *appMan, FieldSystem *fieldSystem, int state, int message, BOOL visible)
{
    Heap_Create(HEAP_ID_APPLICATION, HEAP_ID_WONDER_TRADE, 0x80000);
    WonderTradeApp *app = ApplicationManager_NewData(appMan, sizeof(WonderTradeApp), HEAP_ID_WONDER_TRADE);
    memset(app, 0, sizeof(*app));
    app->fieldSystem = fieldSystem;
    app->game.save = SaveData_Ptr(); app->game.heapID = HEAP_ID_WONDER_TRADE;
    PlayTime_FlagNotStarted();
    app->selectedSlot = -1;
    app->messages = MessageLoader_Init(MSG_LOADER_PRELOAD_ENTIRE_BANK, NARC_INDEX_MSGDATA__PL_MSG, TEXT_BANK_WONDER_TRADE, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_New(HEAP_ID_WONDER_TRADE);
    app->state = state;
    app->statusMessage = message;
    if (visible) WonderTrade_InitGraphics(app);
    return app;
}

static BOOL WonderTrade_Init(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = WonderTrade_Create(appMan, ApplicationManager_Args(appMan), WT_APP_CONNECTING, WonderTrade_Text_Connecting, TRUE);
    WonderTradeNetwork_Start(&app->network, &app->game);
    WonderTrade_Draw(app);
    return TRUE;
}

static void WonderTrade_BootWorker(void *arg)
{
    WonderTradeBootWorker *worker = arg;
    WtJournalIO io = { &worker->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    worker->game.save = SaveData_Ptr(); worker->game.heapID = HEAP_ID_WONDER_TRADE;
    worker->result = WtBoot_Check(&worker->game, io);
}

static BOOL WonderTrade_BootInit(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app;
    WonderTradeBootWorker *worker;
    void *stackTop;
    WtRecovery_Begin();
    /* A normal boot shows nothing: a screen opens only for recovery or a long check. */
    app = WonderTrade_Create(appMan, NULL, WT_APP_BOOT, WonderTrade_Text_CheckingSave, FALSE);
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
        /* Usually this finishes unseen; only a slow check opens the screen early. */
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
    if (WtRecovery_CanWrite()) return TRUE;
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
        WonderTrade_Back(app);
        break;
    case WT_APP_ERROR:
        if (choice == 0) {
            WonderTradeNetwork_Close(&app->network);
            WonderTrade_Show(app, WT_APP_RESTART, WonderTrade_Text_Disconnecting);
        } else WonderTrade_Close(app);
        break;
    case WT_APP_MENU:
        if (choice == 0) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE && app->fieldSystem) WonderTrade_StartPicker(app);
            else WonderTrade_Show(app, WT_APP_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
        } else if (choice == 1) {
            /* Holding Select keeps the hardware-validation numbers available. */
            if (app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE,
                JOY_HELD(PAD_BUTTON_SELECT) ? WonderTrade_Text_Diagnostics : WonderTrade_Text_Explanation);
            else WonderTrade_Show(app, WT_APP_CANCEL_CONFIRM, WonderTrade_Text_ConfirmCancel);
        } else WonderTrade_Close(app);
        break;
    }
}

static void WonderTrade_HandleInput(WonderTradeApp *app)
{
    int labels[WT_MENU_MAX_ACTIONS];
    int count = WonderTrade_Actions(app, labels);
    int touched = WtMenu_Touch(&app->touchBlocked, gSystem.touchPressed, gSystem.touchHeld,
        gSystem.touchX, gSystem.touchY, count);
    if (app->state != WT_APP_RECOVERY && JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X)) {
        if (app->state == WT_APP_MESSAGE && JOY_NEW(PAD_BUTTON_X)) WonderTrade_Close(app);
        else WonderTrade_Back(app);
        return;
    }
    if (!count) return;
    if (touched >= 0) {
        Sound_PlayEffect(SE_CONFIRM_sseq_3);
        app->cursor = touched;
        WonderTrade_Activate(app, touched);
    } else if (JOY_NEW(PAD_KEY_UP | PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + (JOY_NEW(PAD_KEY_UP) ? count - 1 : 1)) % count;
        Sound_PlayEffect(SE_CONFIRM_sseq_3);
        WonderTrade_Draw(app);
    } else if (JOY_NEW(PAD_BUTTON_A)) {
        Sound_PlayEffect(SE_CONFIRM_sseq_3);
        WonderTrade_Activate(app, app->cursor);
    }
}

static BOOL WonderTrade_Main(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = ApplicationManager_Data(appMan);
    if (app->state == WT_APP_BOOT) return WonderTrade_BootPoll(app);
    if (app->state == WT_APP_RECOVERY) {
        WonderTrade_HandleInput(app);
        return FALSE;
    }
    BOOL couldLeave = app->network.canLeave;
    WonderTradeNetwork_Update(&app->network);
    if (app->graphicsReady && couldLeave != app->network.canLeave) WonderTrade_Draw(app);

    if (app->state == WT_APP_PICKER_FADE) {
        if (IsScreenFadeDone()) {
            WonderTrade_FreeGraphics(app);
            app->picker = ApplicationManager_New(&gPokemonPartyAppTemplate,
                &app->partyMenu,
                HEAP_ID_WONDER_TRADE);
            app->state = WT_APP_PICKER;
        }
        return FALSE;
    }
    if (app->state == WT_APP_PICKER) {
        if (ApplicationManager_Exec(app->picker)) {
            ApplicationManager_Free(app->picker);
            app->picker = NULL;
            WonderTrade_FinishPicker(app);
        }
        return FALSE;
    }
    if (app->state == WT_APP_CLOSING) {
        return app->network.state == WT_NET_CLOSED;
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
        if (app->state == WT_APP_CLOSING) return FALSE;
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
            else if (app->network.superseded && app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Superseded);
            else if (app->network.refused) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_CannotHold);
            else if (app->network.waiting) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Waiting);
            else if (app->network.tradePhase == WT_JOURNAL_RECEIVED) WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_NoRoom);
            else WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
        }
        return FALSE;
    }

    WonderTrade_HandleInput(app);
    return FALSE;
}

static BOOL WonderTrade_Exit(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = ApplicationManager_Data(appMan);
    if (app->fieldSystem) PlayTime_Start(SaveData_GetPlayTime(app->fieldSystem->saveData));
    WonderTrade_FreeGraphics(app);
    Heap_Free(app->bgConfig);
    MessageLoader_Free(app->messages);
    ApplicationManager_FreeData(appMan);
    Heap_Destroy(HEAP_ID_WONDER_TRADE);
    return TRUE;
}

const ApplicationManagerTemplate gWonderTradeAppTemplate = {
    WonderTrade_Init, WonderTrade_Main, WonderTrade_Exit, FS_OVERLAY_ID_NONE
};

const ApplicationManagerTemplate gWonderTradeBootAppTemplate = {
    WonderTrade_BootInit, WonderTrade_Main, WonderTrade_Exit, FS_OVERLAY_ID_NONE
};
