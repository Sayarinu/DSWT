#include "wonder_trade/application.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "applications/party_menu/defs.h"
#include "applications/party_menu/main.h"
#include "field/field_system.h"
#include "wonder_trade/network.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/game_save.h"

#include "bag.h"
#include "bg_window.h"
#include "font.h"
#include "gx_layers.h"
#include "heap.h"
#include "mail.h"
#include "message.h"
#include "party.h"
#include "play_time_manager.h"
#include "pokemon.h"
#include "render_window.h"
#include "save_player.h"
#include "screen_fade.h"
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
    WtGameSave game;
    int result;
    unsigned char stack[WT_BOOT_STACK_SIZE + 8];
} WonderTradeBootWorker;

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
    WonderTradeBootWorker *boot;
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

    if (app->state == WT_APP_CONFIRM && app->selectedSlot >= 0
        && app->selectedSlot < Party_GetCurrentCount(&app->partySnapshot)) {
        Pokemon *mon = Party_GetPokemonBySlotIndex(&app->partySnapshot, app->selectedSlot);
        charcode_t species[MON_NAME_LEN + 1];
        String *name = String_Init(MON_NAME_LEN + 1, HEAP_ID_WONDER_TRADE);
        MessageLoader_GetSpeciesName(Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL), HEAP_ID_WONDER_TRADE, species);
        String_CopyChars(name, species);
        Text_AddPrinterWithParamsAndColor(&app->window, FONT_SYSTEM, name, 8, 112, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
        Pokemon_GetValue(mon, MON_DATA_NICKNAME_STRING, name);
        Text_AddPrinterWithParamsAndColor(&app->window, FONT_SYSTEM, name, 8, 128, TEXT_SPEED_NO_TRANSFER, TEXT_COLOR(1, 2, 15), NULL);
        String_Free(name);
    }

    if (app->state == WT_APP_MENU) {
        const int choices[] = {
            app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Trade : WonderTrade_Text_Collect,
            app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Info : WonderTrade_Text_CancelOffer,
            WonderTrade_Text_Exit
        };
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
    } else if (app->state != WT_APP_BOOT && app->state != WT_APP_RECOVERY) {
        if (app->state != WT_APP_CONFIRM && app->state != WT_APP_COLLECT_CONFIRM
            && app->state != WT_APP_CANCEL_CONFIRM && app->network.canLeave) WonderTrade_Print(app, WonderTrade_Text_Cancel, 8, 144);
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

    app->selectedSlot = slot;
    WonderTrade_Show(app, WT_APP_CONFIRM, WonderTrade_Text_Confirm);
}

static WonderTradeApp *WonderTrade_Create(ApplicationManager *appMan, FieldSystem *fieldSystem, int state, int message)
{
    Heap_Create(HEAP_ID_APPLICATION, HEAP_ID_WONDER_TRADE, 0x80000);
    WonderTradeApp *app = ApplicationManager_NewData(appMan, sizeof(WonderTradeApp), HEAP_ID_WONDER_TRADE);
    memset(app, 0, sizeof(*app));
    app->fieldSystem = fieldSystem;
    PlayTime_FlagNotStarted();
    app->selectedSlot = -1;
    app->messages = MessageLoader_Init(MSG_LOADER_PRELOAD_ENTIRE_BANK, NARC_INDEX_MSGDATA__PL_MSG, TEXT_BANK_WONDER_TRADE, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_New(HEAP_ID_WONDER_TRADE);
    app->state = state;
    app->statusMessage = message;
    WonderTrade_InitGraphics(app);
    return app;
}

static BOOL WonderTrade_Init(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = WonderTrade_Create(appMan, ApplicationManager_Args(appMan), WT_APP_CONNECTING, WonderTrade_Text_Connecting);
    WonderTradeNetwork_Start(&app->network, HEAP_ID_WONDER_TRADE);
    return TRUE;
}

static void WonderTrade_BootWorker(void *arg)
{
    WonderTradeBootWorker *worker = arg;
    WtJournalIO io = { &worker->flash, WtFlash_ReadBank, WtFlash_WriteBank, WtFlash_Hash };
    /* Ordinary play remains available on stock emulators for checked blank
     * or EMPTY journals. Only active recovery requires durable capability. */
    worker->flash.requireDurability = FALSE;
    worker->game.save = SaveData_Ptr(); worker->game.heapID = HEAP_ID_WONDER_TRADE;
    worker->result = WtRecovery_Check(io);
    if (worker->result != 0) {
        worker->flash.requireDurability = TRUE;
        worker->result = WtGameSave_Boot(&worker->game, io);
    }
}

static BOOL WonderTrade_BootInit(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app;
    WonderTradeBootWorker *worker;
    void *stackTop;
    WtRecovery_Begin();
    app = WonderTrade_Create(appMan, NULL, WT_APP_BOOT, WonderTrade_Text_CheckingSave);
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
    if (!OS_IsThreadTerminated(&worker->thread)) return FALSE;
    OS_JoinThread(&worker->thread);
    stack = (const unsigned char *)(worker->thread.stackTop + sizeof(u32));
    size = worker->thread.stackBottom - worker->thread.stackTop - 2 * sizeof(u32);
    for (unused = 0; unused < size && stack[unused] == 0xa5; unused++) { (void)0; }
    if (unused < 256 || OS_GetStackStatus(&worker->thread) != OS_STACK_NO_ERROR || worker->result != 0) WtRecovery_Block();
    mbedtls_platform_zeroize(worker, sizeof(*worker));
    Heap_Free(worker); app->boot = NULL;
    if (WtRecovery_CanWrite()) return TRUE;
    WonderTrade_Show(app, WT_APP_RECOVERY, WonderTrade_Text_RecoveryRequired);
    return FALSE;
}

static BOOL WonderTrade_Main(ApplicationManager *appMan, int *state)
{
    WonderTradeApp *app = ApplicationManager_Data(appMan);
    if (app->state == WT_APP_BOOT) return WonderTrade_BootPoll(app);
    if (app->state == WT_APP_RECOVERY) {
        if (JOY_NEW(PAD_BUTTON_A)) {
            WonderTradeNetwork_Start(&app->network, HEAP_ID_WONDER_TRADE);
            WonderTrade_Show(app, WT_APP_CONNECTING, WonderTrade_Text_Connecting);
        }
        return FALSE;
    }
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
        if (JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X) && app->network.canLeave) {
            WonderTrade_Close(app);
        } else if (app->network.state == WT_NET_ERROR) {
            WonderTrade_Show(app, WT_APP_ERROR, app->state == WT_APP_CONNECTING ? WonderTrade_Text_ConnectionError : WonderTrade_Text_PingError);
        } else if (app->state == WT_APP_CONNECTING && app->network.state == WT_NET_READY) {
            WonderTradeNetwork_Trade(&app->network, SaveData_Ptr(), -1);
            WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
        } else if (app->network.state == WT_NET_RESPONSE) {
            app->selectedSlot = -1;
            if (!app->fieldSystem && app->network.canLeave) WonderTrade_Close(app);
            else if (app->network.completed) WonderTrade_Show(app, WT_APP_MESSAGE,
                app->network.returned ? WonderTrade_Text_Returned : WonderTrade_Text_Completed);
            else if (app->network.rejected) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Rejected);
            else if (app->network.superseded && app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Superseded);
            else if (app->network.waiting) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Waiting);
            else if (app->network.tradePhase == WT_JOURNAL_RECEIVED) WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_NoRoom);
            else WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
        }
        return FALSE;
    }

    if (app->state == WT_APP_CONFIRM) {
        if (JOY_NEW(PAD_BUTTON_A)) {
            WonderTradeNetwork_Trade(&app->network, SaveData_Ptr(), app->selectedSlot);
            WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
        } else if (JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X)) {
            app->selectedSlot = -1;
            WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
        }
        return FALSE;
    }

    if (app->state == WT_APP_COLLECT_CONFIRM || app->state == WT_APP_CANCEL_CONFIRM) {
        if (JOY_NEW(PAD_BUTTON_A)) {
            WonderTradeNetwork_Collect(&app->network, SaveData_Ptr(), app->state == WT_APP_CANCEL_CONFIRM);
            WonderTrade_Show(app, WT_APP_PINGING, WonderTrade_Text_Pinging);
        } else if (JOY_NEW(PAD_BUTTON_B | PAD_BUTTON_X)) {
            WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Queued);
        }
        return FALSE;
    }

    if (app->state == WT_APP_MESSAGE) {
        if (JOY_NEW(PAD_BUTTON_A | PAD_BUTTON_B)) {
            WonderTrade_Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WonderTrade_Text_Choose : WonderTrade_Text_Queued);
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
            if (app->network.tradePhase == WT_JOURNAL_IDLE && app->fieldSystem) WonderTrade_StartPicker(app);
            else WonderTrade_Show(app, WT_APP_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
        } else if (app->cursor == 1) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE) WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Explanation);
            else WonderTrade_Show(app, WT_APP_CANCEL_CONFIRM, WonderTrade_Text_ConfirmCancel);
        } else {
            WonderTrade_Close(app);
        }
    }
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
