#include "wonder_trade/application.h"

#include <nitro.h>
#include <string.h>

#include "applications/party_menu/defs.h"
#include "applications/party_menu/main.h"
#include "field/field_system.h"
#include "wonder_trade/network.h"

#include "bag.h"
#include "bg_window.h"
#include "font.h"
#include "gx_layers.h"
#include "heap.h"
#include "mail.h"
#include "message.h"
#include "party.h"
#include "pokemon.h"
#include "render_window.h"
#include "save_player.h"
#include "screen_fade.h"
#include "system.h"
#include "text.h"
#include "touch_pad.h"

#include "res/text/bank/wonder_trade.h"

enum WonderTradeAppState {
    WT_APP_CONNECTING,
    WT_APP_MENU,
    WT_APP_PICKER_FADE,
    WT_APP_PICKER,
    WT_APP_PINGING,
    WT_APP_MESSAGE,
    WT_APP_ERROR,
    WT_APP_RESTART,
    WT_APP_CLOSING,
};

typedef struct WonderTradeApp {
    FieldSystem *fieldSystem;
    WonderTradeNetwork network;
    BgConfig *bgConfig;
    Window window;
    MessageLoader *messages;
    ApplicationManager *picker;
    Party partySnapshot;
    PartyMenu partyMenu;
    int state;
    int statusMessage;
    int cursor;
    int selectedSlot;
    BOOL graphicsReady;
} WonderTradeApp;

static void WonderTrade_Draw(WonderTradeApp *app);

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
    const BgTemplate bg = {
        .bufferSize = 0x800,
        .screenSize = BG_SCREEN_SIZE_256x256,
        .colorMode = GX_BG_COLORMODE_16,
        .screenBase = GX_BG_SCRBASE_0xf800,
        .charBase = GX_BG_CHARBASE_0x00000,
        .bgExtPltt = GX_BG_EXTPLTT_01,
    };

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
    Bg_ClearTilesRange(BG_LAYER_MAIN_0, 32, 0, HEAP_ID_WONDER_TRADE);
    Bg_ClearTilemap(app->bgConfig, BG_LAYER_MAIN_0);
    Font_LoadTextPalette(PAL_LOAD_MAIN_BG, 13 * 32, HEAP_ID_WONDER_TRADE);
    LoadStandardWindowGraphics(app->bgConfig, BG_LAYER_MAIN_0, 640, 14, STANDARD_WINDOW_SYSTEM, HEAP_ID_WONDER_TRADE);
    Window_Add(app->bgConfig, &app->window, BG_LAYER_MAIN_0, 2, 1, 28, 22, 13, 1);
    Window_DrawStandardFrame(&app->window, TRUE, 640, 14);
    app->graphicsReady = TRUE;
    WonderTrade_Draw(app);
    SetVBlankCallback(WonderTrade_VBlank, app);
    GXLayers_EngineAToggleLayers(GX_PLANEMASK_BG0, TRUE);
    u16 background = GX_RGB(26, 28, 30);
    GXS_LoadBGPltt(&background, 0, sizeof(background));
    GXLayers_TurnBothDispOn();
    ResetScreenMasterBrightness(DS_SCREEN_MAIN);
    ResetScreenMasterBrightness(DS_SCREEN_SUB);
}

static void WonderTrade_FreeGraphics(WonderTradeApp *app)
{
    if (!app->graphicsReady) {
        return;
    }
    SetVBlankCallback(NULL, NULL);
    DisableTouchPad();
    Window_Remove(&app->window);
    Bg_FreeTilemapBuffer(app->bgConfig, BG_LAYER_MAIN_0);
    GXLayers_DisableEngineALayers();
    GXLayers_DisableEngineBLayers();
    app->graphicsReady = FALSE;
}

static void WonderTrade_Print(WonderTradeApp *app, int message, int x, int y)
{
    String *text = MessageLoader_GetNewString(app->messages, message);
    Text_AddPrinterWithParamsAndColor(&app->window, FONT_SYSTEM, text, x, y, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
    String_Free(text);
}

static void WonderTrade_Draw(WonderTradeApp *app)
{
    Window_FillTilemap(&app->window, 15);
    WonderTrade_Print(app, WonderTrade_Text_Title, 8, 0);
    WonderTrade_Print(app, app->statusMessage, 8, 32);

    if (app->state == WT_APP_MENU) {
        const int choices[] = { WonderTrade_Text_Trade, WonderTrade_Text_Info, WonderTrade_Text_Exit };
        for (int i = 0; i < 3; i++) {
            WonderTrade_Print(app, choices[i], 24, 96 + i * 24);
        }
        Window_DrawMenuCursor(&app->window, 8, 96 + app->cursor * 24);
    } else if (app->state == WT_APP_ERROR) {
        WonderTrade_Print(app, WonderTrade_Text_Retry, 24, 96);
        WonderTrade_Print(app, WonderTrade_Text_Exit, 24, 120);
        Window_DrawMenuCursor(&app->window, 8, 96 + app->cursor * 24);
        WonderTrade_Print(app, WonderTrade_Text_ErrorCode, 8, 152);
        String *number = String_Init(12, HEAP_ID_WONDER_TRADE);
        String_FormatInt(number, app->network.error, 10, PADDING_MODE_NONE, CHARSET_MODE_EN);
        Text_AddPrinterWithParamsAndColor(&app->window, FONT_SYSTEM, number, 152, 152, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
        String_Free(number);
    } else if (app->state == WT_APP_MESSAGE) {
        if (app->statusMessage == WonderTrade_Text_Explanation) {
            const int labels[] = { WonderTrade_Text_ConnectMs, WonderTrade_Text_TlsBytes, WonderTrade_Text_StackBytes, WonderTrade_Text_FrameMs };
            const u32 values[] = { app->network.handshakeMs, app->network.tlsPeakBytes, app->network.stackBytes, app->network.maxFrameMs };
            String *number = String_Init(12, HEAP_ID_WONDER_TRADE);
            for (int i = 0; i < 4; i++) {
                WonderTrade_Print(app, labels[i], 8, 88 + i * 16);
                String_FormatInt(number, values[i], 10, PADDING_MODE_NONE, CHARSET_MODE_EN);
                Text_AddPrinterWithParamsAndColor(&app->window, FONT_SYSTEM, number, 152, 88 + i * 16, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
            }
            String_Free(number);
        }
        WonderTrade_Print(app, WonderTrade_Text_Back, 24, 160);
        Window_DrawMenuCursor(&app->window, 8, 160);
    } else {
        WonderTrade_Print(app, WonderTrade_Text_Cancel, 8, 144);
    }
    Window_CopyToVRAM(&app->window);
}

static void WonderTrade_Show(WonderTradeApp *app, int state, int message)
{
    app->state = state;
    app->statusMessage = message;
    app->cursor = 0;
    WonderTrade_Draw(app);
}

static void WonderTrade_Close(WonderTradeApp *app)
{
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

    Pokemon *mon = Party_GetPokemonBySlotIndex(&app->partySnapshot, slot);
    u32 species = Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL);
    if (Pokemon_GetValue(mon, MON_DATA_IS_EGG, NULL)
        || Pokemon_GetValue(mon, MON_DATA_SANITY_IS_EGG, NULL)
        || species == SPECIES_NONE || species > SPECIES_ARCEUS) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Ineligible);
        return;
    }

    app->selectedSlot = slot;
    WonderTradeNetwork_Ping(&app->network);
    WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
}

static BOOL WonderTrade_Init(ApplicationManager *appMan, int *state)
{
    Heap_Create(HEAP_ID_APPLICATION, HEAP_ID_WONDER_TRADE, 0x50000);
    WonderTradeApp *app = ApplicationManager_NewData(appMan, sizeof(WonderTradeApp), HEAP_ID_WONDER_TRADE);
    memset(app, 0, sizeof(*app));
    app->fieldSystem = ApplicationManager_Args(appMan);
    app->selectedSlot = -1;
    app->messages = MessageLoader_Init(MSG_LOADER_PRELOAD_ENTIRE_BANK, NARC_INDEX_MSGDATA__PL_MSG, TEXT_BANK_WONDER_TRADE, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_New(HEAP_ID_WONDER_TRADE);
    app->state = WT_APP_CONNECTING;
    app->statusMessage = WonderTrade_Text_Connecting;
    WonderTrade_InitGraphics(app);
    WonderTradeNetwork_Start(&app->network, HEAP_ID_WONDER_TRADE);
    return TRUE;
}

static BOOL WonderTrade_Main(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = ApplicationManager_Data(appMan);
    WonderTradeNetwork_Update(&app->network);

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
            WonderTradeNetwork_Start(&app->network, HEAP_ID_WONDER_TRADE);
            WonderTrade_Show(app, WT_APP_CONNECTING, WonderTrade_Text_Connecting);
        }
        return FALSE;
    }

    if (app->state == WT_APP_CONNECTING || app->state == WT_APP_PINGING) {
        if (JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X)) {
            WonderTrade_Close(app);
        } else if (app->network.state == WT_NET_ERROR) {
            WonderTrade_Show(app, WT_APP_ERROR, app->state == WT_APP_CONNECTING ? WonderTrade_Text_ConnectionError : WonderTrade_Text_PingError);
        } else if (app->state == WT_APP_CONNECTING && app->network.state == WT_NET_READY) {
            if (app->selectedSlot >= 0) {
                WonderTradeNetwork_Ping(&app->network);
                WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
            } else {
                WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
            }
        } else if (app->network.state == WT_NET_ACKNOWLEDGED) {
            app->selectedSlot = -1;
            WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Acknowledged);
        }
        return FALSE;
    }

    if (app->state == WT_APP_MESSAGE) {
        if (JOY_NEW(PAD_BUTTON_A | PAD_BUTTON_B)) {
            WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
        } else if (JOY_NEW(PAD_BUTTON_X)) {
            WonderTrade_Close(app);
        }
        return FALSE;
    }

    if (JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X)) {
        WonderTrade_Close(app);
        return FALSE;
    }
    int count = app->state == WT_APP_MENU ? 3 : 2;
    if (JOY_NEW(PAD_KEY_UP)) {
        app->cursor = (app->cursor + count - 1) % count;
        WonderTrade_Draw(app);
    } else if (JOY_NEW(PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + 1) % count;
        WonderTrade_Draw(app);
    } else if (JOY_NEW(PAD_BUTTON_A)) {
        if (app->state == WT_APP_ERROR) {
            if (app->cursor == 0) {
                WonderTradeNetwork_Close(&app->network);
                WonderTrade_Show(app, WT_APP_RESTART, WonderTrade_Text_Disconnecting);
            } else {
                WonderTrade_Close(app);
            }
        } else if (app->cursor == 0) {
            WonderTrade_StartPicker(app);
        } else if (app->cursor == 1) {
            WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Explanation);
        } else {
            WonderTrade_Close(app);
        }
    }
    return FALSE;
}

static BOOL WonderTrade_Exit(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = ApplicationManager_Data(appMan);
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
