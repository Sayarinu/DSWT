#include "wonder_trade/hgss/application.h"

#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "global.h"

#include "constants/sndseq.h"

#include "msgdata/msg.naix"
#include "msgdata/msg/msg_0829.h"

#include "bg_window.h"
#include "field_system.h"
#include "font.h"
#include "gf_gfx_planes.h"
#include "heap.h"
#include "msgdata.h"
#include "party.h"
#include "pm_string.h"
#include "pokemon.h"
#include "render_window.h"
#include "save_arrays.h"
#include "screen_fade.h"
#include "string_util.h"
#include "system.h"
#include "text.h"
#include "unk_02005D10.h"
#include "wonder_trade/boot.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/hgss/game_save.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"

enum WonderTradeAppState {
    WT_APP_BOOT,
    WT_APP_RECOVERY,
    WT_APP_CONNECTING,
    WT_APP_MENU,
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

#define WT_HEAP_SIZE 0x80000
#define WT_BOOT_STACK_SIZE 0x8000
#define WT_BOOT_SILENT_FRAMES 30
#define WT_MAX_ROWS (PARTY_SIZE + 1)
#define WT_ROW_HEIGHT 16
#define WT_FRAME_TILE 0x3E0
#define WT_FRAME_PALETTE 2
#define WT_TEXT_PALETTE 1
#define WT_COLOR_TEXT MAKE_TEXT_COLOR(1, 2, 15)
#define WT_COLOR_SELECTED MAKE_TEXT_COLOR(3, 4, 15)

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
    Window title, body, list;
    MsgData *messages;
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

static const GraphicsBanks sBanks = {
    .bg = GX_VRAM_BG_256_AB,
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

static const GraphicsModes sModes = {
    .dispMode = GX_DISPMODE_GRAPHICS,
    .bgMode = GX_BGMODE_0,
    .subMode = GX_BGMODE_0,
    ._2d3dMode = GX_BG0_AS_2D,
};

static const BgTemplate sMainTemplate = {
    .x = 0, .y = 0, .bufferSize = 0x800, .baseTile = 0,
    .size = GF_BG_SCR_SIZE_256x256, .colorMode = GX_BG_COLORMODE_16,
    .screenBase = GX_BG_SCRBASE_0x0000, .charBase = GX_BG_CHARBASE_0x18000,
    .bgExtPltt = GX_BG_EXTPLTT_01, .priority = 1, .areaOver = GX_BG_AREAOVER_XLU,
    .dummy = 0, .mosaic = FALSE,
};

static const BgTemplate sSubTemplate = {
    .x = 0, .y = 0, .bufferSize = 0x800, .baseTile = 0,
    .size = GF_BG_SCR_SIZE_256x256, .colorMode = GX_BG_COLORMODE_16,
    .screenBase = GX_BG_SCRBASE_0x0000, .charBase = GX_BG_CHARBASE_0x04000,
    .bgExtPltt = GX_BG_EXTPLTT_01, .priority = 1, .areaOver = GX_BG_AREAOVER_XLU,
    .dummy = 0, .mosaic = FALSE,
};

static Party *WonderTrade_Party(WonderTradeApp *app)
{
    return SaveArray_Party_Get(app->game.save);
}

/* Action labels for the current state; the picker lists the party instead. */
static int WonderTrade_Actions(const WonderTradeApp *app, int labels[WT_MAX_ROWS])
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

static int WonderTrade_RowCount(WonderTradeApp *app)
{
    int labels[WT_MAX_ROWS];
    if (app->state == WT_APP_PICKER) return Party_GetCount(WonderTrade_Party(app)) + 1;
    return WonderTrade_Actions(app, labels);
}

static void WonderTrade_VBlank(void *arg)
{
    WonderTradeApp *app = arg;
    DoScheduledBgGpuUpdates(app->bgConfig);
    OS_SetIrqCheckFlag(OS_IE_V_BLANK);
}

static void WonderTrade_InitGraphics(WonderTradeApp *app)
{
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
    InitBgFromTemplate(app->bgConfig, GF_BG_LYR_MAIN_0, &sMainTemplate, GF_BG_TYPE_TEXT);
    InitBgFromTemplate(app->bgConfig, GF_BG_LYR_SUB_0, &sSubTemplate, GF_BG_TYPE_TEXT);
    BgClearTilemapBufferAndCommit(app->bgConfig, GF_BG_LYR_MAIN_0);
    BgClearTilemapBufferAndCommit(app->bgConfig, GF_BG_LYR_SUB_0);
    BG_ClearCharDataRange(GF_BG_LYR_MAIN_0, 0x20, 0, app->game.heapID);
    BG_ClearCharDataRange(GF_BG_LYR_SUB_0, 0x20, 0, app->game.heapID);
    LoadUserFrameGfx1(app->bgConfig, GF_BG_LYR_MAIN_0, WT_FRAME_TILE, WT_FRAME_PALETTE, 0, app->game.heapID);
    LoadUserFrameGfx1(app->bgConfig, GF_BG_LYR_SUB_0, WT_FRAME_TILE, WT_FRAME_PALETTE, 0, app->game.heapID);
    LoadFontPal0(GF_PAL_LOCATION_MAIN_BG, GF_PAL_SLOT_1_OFFSET, app->game.heapID);
    LoadFontPal0(GF_PAL_LOCATION_SUB_BG, GF_PAL_SLOT_1_OFFSET, app->game.heapID);
    BG_SetMaskColor(GF_BG_LYR_MAIN_0, RGB(9, 18, 27));
    BG_SetMaskColor(GF_BG_LYR_SUB_0, RGB(9, 18, 27));
    AddWindowParameterized(app->bgConfig, &app->title, GF_BG_LYR_MAIN_0, 2, 1, 28, 2, WT_TEXT_PALETTE, 1);
    AddWindowParameterized(app->bgConfig, &app->body, GF_BG_LYR_MAIN_0, 2, 5, 28, 17, WT_TEXT_PALETTE, 61);
    AddWindowParameterized(app->bgConfig, &app->list, GF_BG_LYR_SUB_0, 2, 2, 28, WT_MAX_ROWS * 2 + 1, WT_TEXT_PALETTE, 1);
    app->graphicsReady = TRUE;
    WonderTrade_Draw(app);
    Main_SetVBlankIntrCB(WonderTrade_VBlank, app);
    GfGfx_EngineATogglePlanes(GX_PLANEMASK_BG0, GF_PLANE_TOGGLE_ON);
    GfGfx_EngineBTogglePlanes(GX_PLANEMASK_BG0, GF_PLANE_TOGGLE_ON);
    GfGfx_BothDispOn();
    SetMasterBrightnessNeutral(PM_LCD_TOP);
    SetMasterBrightnessNeutral(PM_LCD_BOTTOM);
}

static void WonderTrade_FreeGraphics(WonderTradeApp *app)
{
    if (!app->graphicsReady) return;
    Main_SetVBlankIntrCB(NULL, NULL);
    RemoveWindow(&app->title);
    RemoveWindow(&app->body);
    RemoveWindow(&app->list);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_MAIN_0);
    FreeBgTilemapBuffer(app->bgConfig, GF_BG_LYR_SUB_0);
    GfGfx_DisableEngineAPlanes();
    GfGfx_DisableEngineBPlanes();
    app->graphicsReady = FALSE;
}

static void WonderTrade_PrintString(Window *window, String *text, int x, int y, u32 color)
{
    AddTextPrinterParameterizedWithColor(window, FONT_SYSTEM, text, x, y, TEXT_SPEED_NOTRANSFER, color, NULL);
}

static void WonderTrade_Print(WonderTradeApp *app, Window *window, int message, int x, int y, u32 color)
{
    String *text = NewString_ReadMsgData(app->messages, message);
    WonderTrade_PrintString(window, text, x, y, color);
    String_Delete(text);
}

static void WonderTrade_PrintChars(WonderTradeApp *app, Window *window, const u16 *chars, int x, int y, u32 color)
{
    String *text = String_New(POKEMON_NAME_LENGTH + PLAYER_NAME_LENGTH + 2, app->game.heapID);
    CopyU16ArrayToString(text, chars);
    WonderTrade_PrintString(window, text, x, y, color);
    String_Delete(text);
}

static void WonderTrade_PrintNumber(WonderTradeApp *app, Window *window, u32 value, int x, int y, u32 color)
{
    String *number = String_New(12, app->game.heapID);
    String16_FormatInteger(number, (int)value, 10, PRINTING_MODE_LEFT_ALIGN, TRUE);
    WonderTrade_PrintString(window, number, x, y, color);
    String_Delete(number);
}

static void WonderTrade_DrawReceived(WonderTradeApp *app, int y)
{
    const WtReceivedPokemon *received = &app->network.received;
    String *species = GetSpeciesName(received->species, app->game.heapID);
    WonderTrade_Print(app, &app->body, WonderTrade_Text_Received, 0, y, WT_COLOR_TEXT);
    WonderTrade_PrintString(&app->body, species, 104, y, WT_COLOR_TEXT);
    String_Delete(species);
    if (received->hasNickname) WonderTrade_PrintChars(app, &app->body, received->nickname, 104, y + WT_ROW_HEIGHT, WT_COLOR_TEXT);
    WonderTrade_Print(app, &app->body, WonderTrade_Text_OriginalTrainer, 0, y + 2 * WT_ROW_HEIGHT, WT_COLOR_TEXT);
    WonderTrade_PrintChars(app, &app->body, received->originalTrainer, 104, y + 2 * WT_ROW_HEIGHT, WT_COLOR_TEXT);
}

static void WonderTrade_DrawPicker(WonderTradeApp *app)
{
    Party *party = WonderTrade_Party(app);
    u16 nickname[POKEMON_NAME_LENGTH + 1];
    int i, count = Party_GetCount(party);
    for (i = 0; i < count; i++) {
        Pokemon *mon = Party_GetMonByIndex(party, i);
        u32 color = i == app->cursor ? WT_COLOR_SELECTED : WT_COLOR_TEXT;
        GetMonData(mon, MON_DATA_NICKNAME, nickname);
        WonderTrade_PrintChars(app, &app->list, nickname, 8, i * WT_ROW_HEIGHT + 4, color);
        if (!GetMonData(mon, MON_DATA_IS_EGG, NULL)) {
            WonderTrade_Print(app, &app->list, WonderTrade_Text_Level, 136, i * WT_ROW_HEIGHT + 4, color);
            WonderTrade_PrintNumber(app, &app->list, GetMonData(mon, MON_DATA_LEVEL, NULL), 160, i * WT_ROW_HEIGHT + 4, color);
        }
    }
    WonderTrade_Print(app, &app->list, WonderTrade_Text_Back, 8, count * WT_ROW_HEIGHT + 4,
        app->cursor == count ? WT_COLOR_SELECTED : WT_COLOR_TEXT);
}

static void WonderTrade_Draw(WonderTradeApp *app)
{
    int labels[WT_MAX_ROWS];
    int i, count;
    if (!app->graphicsReady) return;
    count = WonderTrade_RowCount(app);
    if (app->cursor >= count) app->cursor = 0;
    FillWindowPixelBuffer(&app->title, 0);
    WonderTrade_Print(app, &app->title, WonderTrade_Text_Title, 0, 0, MAKE_TEXT_COLOR(15, 2, 0));
    ScheduleWindowCopyToVram(&app->title);
    PutWindowTilemap(&app->title);

    FillWindowPixelBuffer(&app->body, 15);
    DrawFrameAndWindow1(&app->body, TRUE, WT_FRAME_TILE, WT_FRAME_PALETTE);
    WonderTrade_Print(app, &app->body, app->statusMessage, 4, 0, WT_COLOR_TEXT);
    if (app->state == WT_APP_ERROR) {
        WonderTrade_Print(app, &app->body, WonderTrade_Text_ErrorCode, 4, 96, WT_COLOR_TEXT);
        WonderTrade_PrintNumber(app, &app->body, (u32)app->network.error, 160, 96, WT_COLOR_TEXT);
    } else if (app->statusMessage == WonderTrade_Text_Diagnostics) {
        static const int names[] = { WonderTrade_Text_ConnectMs, WonderTrade_Text_TlsBytes, WonderTrade_Text_StackBytes, WonderTrade_Text_FrameMs };
        const u32 values[] = { app->network.handshakeMs, app->network.tlsPeakBytes, app->network.stackBytes, app->network.maxFrameMs };
        for (i = 0; i < 4; i++) {
            WonderTrade_Print(app, &app->body, names[i], 4, 24 + i * WT_ROW_HEIGHT, WT_COLOR_TEXT);
            WonderTrade_PrintNumber(app, &app->body, values[i], 160, 24 + i * WT_ROW_HEIGHT, WT_COLOR_TEXT);
        }
    } else if (app->state == WT_APP_MESSAGE && app->statusMessage == WonderTrade_Text_Completed && app->network.hasReceived) {
        WonderTrade_DrawReceived(app, 64);
    } else if (app->state == WT_APP_CONFIRM && app->selectedSlot >= 0) {
        u16 nickname[POKEMON_NAME_LENGTH + 1];
        GetMonData(Party_GetMonByIndex(WonderTrade_Party(app), app->selectedSlot), MON_DATA_NICKNAME, nickname);
        WonderTrade_PrintChars(app, &app->body, nickname, 4, 64, WT_COLOR_TEXT);
    }
    ScheduleWindowCopyToVram(&app->body);

    FillWindowPixelBuffer(&app->list, 15);
    if (count) {
        DrawFrameAndWindow1(&app->list, TRUE, WT_FRAME_TILE, WT_FRAME_PALETTE);
        if (app->state == WT_APP_PICKER) {
            WonderTrade_DrawPicker(app);
        } else {
            WonderTrade_Actions(app, labels);
            for (i = 0; i < count; i++) {
                WonderTrade_Print(app, &app->list, labels[i], 8, i * WT_ROW_HEIGHT + 4,
                    i == app->cursor ? WT_COLOR_SELECTED : WT_COLOR_TEXT);
            }
        }
        ScheduleWindowCopyToVram(&app->list);
    } else {
        ClearFrameAndWindow2(&app->list, TRUE);
        ClearWindowTilemapAndScheduleTransfer(&app->list);
    }
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_MAIN_0);
    ScheduleBgTilemapBufferTransfer(app->bgConfig, GF_BG_LYR_SUB_0);
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

static void WonderTrade_Pick(WonderTradeApp *app, int slot)
{
    if (slot < 0 || slot >= Party_GetCount(WonderTrade_Party(app))) {
        WonderTrade_Show(app, WT_APP_MENU, WonderTrade_Text_Choose);
    } else if (!WtGameSave_Eligible(app->game.save, slot)) {
        WonderTrade_Show(app, WT_APP_MESSAGE, WonderTrade_Text_Ineligible);
    } else if (!WtGameSave_Unique(app->game.save, slot, app->game.heapID)) {
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
    app->selectedSlot = -1;
    app->messages = NewMsgDataFromNarc(MSGDATA_LOAD_DIRECT, NARC_msgdata_msg, msg_0829, HEAP_ID_WONDER_TRADE);
    app->bgConfig = BgConfig_Alloc(HEAP_ID_WONDER_TRADE);
    app->state = state;
    app->statusMessage = message;
    if (visible) WonderTrade_InitGraphics(app);
    return app;
}

static BOOL WonderTrade_Init(OverlayManager *man, int *state)
{
    WonderTradeApp *app = WonderTrade_Create(man, OverlayManager_GetArgs(man), WT_APP_CONNECTING, WonderTrade_Text_Connecting, TRUE);
    (void)state;
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

static BOOL WonderTrade_BootInit(OverlayManager *man, int *state)
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
    if (WtRecovery_CanWrite()) return TRUE;
    app->state = WT_APP_RECOVERY;
    app->statusMessage = WonderTrade_Text_RecoveryRequired;
    if (app->graphicsReady) WonderTrade_Draw(app);
    else WonderTrade_InitGraphics(app);
    return FALSE;
}

static void WonderTrade_Back(WonderTradeApp *app)
{
    if (app->state == WT_APP_CONFIRM || app->state == WT_APP_PICKER) {
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
    case WT_APP_PICKER:
        WonderTrade_Pick(app, choice);
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
        } else {
            WonderTrade_Close(app);
        }
        break;
    case WT_APP_MENU:
        if (choice == 0) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE && app->fieldSystem) {
                app->selectedSlot = -1;
                WonderTrade_Show(app, WT_APP_PICKER, WonderTrade_Text_Choose);
            } else {
                WonderTrade_Show(app, WT_APP_COLLECT_CONFIRM, WonderTrade_Text_ConfirmCollect);
            }
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
    int count = WonderTrade_RowCount(app);
    if (app->state != WT_APP_RECOVERY && (gSystem.newKeys & (PAD_BUTTON_B | PAD_BUTTON_X))) {
        PlaySE(SEQ_SE_DP_SELECT);
        if (app->state == WT_APP_MESSAGE && (gSystem.newKeys & PAD_BUTTON_X)) WonderTrade_Close(app);
        else WonderTrade_Back(app);
        return;
    }
    if (!count) return;
    if (gSystem.newKeys & (PAD_KEY_UP | PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + ((gSystem.newKeys & PAD_KEY_UP) ? count - 1 : 1)) % count;
        PlaySE(SEQ_SE_DP_SELECT);
        WonderTrade_Draw(app);
    } else if (gSystem.newKeys & PAD_BUTTON_A) {
        PlaySE(SEQ_SE_DP_DECIDE);
        WonderTrade_Activate(app, app->cursor);
    }
}

static BOOL WonderTrade_Main(OverlayManager *man, int *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    BOOL couldLeave;
    (void)state;
    if (app->state == WT_APP_BOOT) return WonderTrade_BootPoll(app);
    if (app->state == WT_APP_RECOVERY) {
        WonderTrade_HandleInput(app);
        return FALSE;
    }
    couldLeave = app->network.canLeave;
    WonderTradeNetwork_Update(&app->network);
    if (couldLeave != app->network.canLeave) WonderTrade_Draw(app);

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

static BOOL WonderTrade_Exit(OverlayManager *man, int *state)
{
    WonderTradeApp *app = OverlayManager_GetData(man);
    (void)state;
    WonderTrade_FreeGraphics(app);
    Heap_Free(app->bgConfig);
    DestroyMsgData(app->messages);
    OverlayManager_FreeData(man);
    Heap_Destroy(HEAP_ID_WONDER_TRADE);
    return TRUE;
}

const OverlayManagerTemplate gWonderTradeAppTemplate = {
    WonderTrade_Init, WonderTrade_Main, WonderTrade_Exit, FS_OVERLAY_ID_NONE
};

const OverlayManagerTemplate gWonderTradeBootAppTemplate = {
    WonderTrade_BootInit, WonderTrade_Main, WonderTrade_Exit, FS_OVERLAY_ID_NONE
};
