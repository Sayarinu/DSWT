#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "constants/arc.h"
#include "constants/pokemon.h"
#include "field/field.h"
#include "field/field_event.h"
#include "gfl/arc_util.h"
#include "gfl/bmp.h"
#include "gfl/gx_layers.h"
#include "gfl/heapsys.h"
#include "gfl/key.h"
#include "gfl/net.h"
#include "gfl/overlay.h"
#include "gfl/proc.h"
#include "gfl/sound.h"
#include "gfl/str.h"
#include "nnsys/g2d.h"
#include "pml/poke_party.h"
#include "system/game_comm.h"
#include "system/game_data.h"
#include "system/game_event.h"
#include "system/game_system.h"
#include "system/gf_font.h"
#include "system/printsys.h"
#include "wonder_trade/bw2/application.h"
#include "wonder_trade/bw2/game_save.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"
#include "text.h"

/* Wonder Trade's screen for Black 2 and White 2. It runs as a process with
 * the field closed, like the Global Trade Station, from its own child heap of
 * heap 1, and at boot when the save needs recovery. The screen is the error
 * window the game shows for a save error (src/system/save_error.c) with the
 * text drawn into it: a title, the status and a short list of actions, chosen
 * with the +Control Pad and A, B to go back. */

typedef char WtWideCharCheck[sizeof(L"a"[0]) == 2 ? 1 : -1];

#define WT_HEAPID 0xBA
#define WT_HEAP_SIZE 0x70000
/* The error window's text area (save_error.c): tiles from 0x20, 30x16 tiles at (1, 4). */
#define TILE_SIZE 0x20
#define FIRST_TEXT_TILE 0x20
#define TEXT_LEFT 1
#define TEXT_TOP 4
#define TEXT_WIDTH 30
#define TEXT_HEIGHT 16
#define TEXT_BACKGROUND 7
#define LINE_HEIGHT 16
#define MAX_ACTIONS 6

enum WtAppState {
    WT_APP_CONNECTING,
    WT_APP_PINGING,
    WT_APP_MENU,
    WT_APP_PICKER,
    WT_APP_CONFIRM,
    WT_APP_COLLECT_CONFIRM,
    WT_APP_CANCEL_CONFIRM,
    WT_APP_MESSAGE,
    WT_APP_RECEIVED,
    WT_APP_ERROR,
    WT_APP_RESTART,
    WT_APP_RECOVERY,
    WT_APP_CLOSING,
    WT_APP_DONE,
};

struct WtApp {
    WonderTradeNetwork network;
    WtGame game;
    GFLBitmap *bitmap;
    Font *font;
    StrBuf *line;
    int state, message, cursor, selectedSlot;
    BOOL boot, started;
};

static void Screen_Init(WtApp *app)
{
    void *file;
    NNSG2dCharacterData *chars;
    NNSG2dScreenData *screen;
    NNSG2dPaletteData *palette;
    u16 *map;
    int x, y;
    u16 tile = FIRST_TEXT_TILE;
    GX_SetMasterBrightness(-16);
    GXS_SetMasterBrightness(-16);
    GX_ResetBankForBG();
    GX_ResetBankForSubBG();
    GX_SetBankForBG(GX_VRAM_BG_128_C);
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_2D);
    GX_SetVisiblePlane(GX_PLANEMASK_BG1);
    GX_SetVisibleWnd(GX_WNDMASK_NONE);
    G2_SetBG1Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000,
                     GX_BG_EXTPLTT_01);
    G2_BG1Mosaic(FALSE);
    G2_SetBG1Offset(0, 0);
    G2_BlendNone();
    GFL_BGSysSetEnabledBGsB(0);
    GXS_DispOn();
    *(vu16 *)HW_DB_BG_PLTT = GX_RGB(10, 23, 31);
    MIi_CpuClear32(0, G2_GetBG1CharPtr(), 0x4000);
    file = GFL_ArcSysReadHeapNewLZ(ARCID_ERROR_WINDOW_GRA, ERROR_WINDOW_GRA_CHAR, FALSE, WT_HEAPID);
    if (NNS_G2dGetUnpackedBGCharacterData(file, &chars)) {
        DC_FlushRange(chars->pRawData, chars->szByte);
        MI_CpuCopy16(chars->pRawData, G2_GetBG1CharPtr(), chars->szByte);
    }
    GFL_HeapFree(file);
    file = GFL_ArcSysReadHeapNewLZ(ARCID_ERROR_WINDOW_GRA, ERROR_WINDOW_GRA_SCRN, FALSE, WT_HEAPID);
    if (NNS_G2dGetUnpackedScreenData(file, &screen)) {
        DC_FlushRange(screen->rawData, screen->szByte);
        MI_CpuCopy16(screen->rawData, G2_GetBG1ScrPtr(), screen->szByte);
    }
    GFL_HeapFree(file);
    file = GFL_ArcSysReadHeapNewLZ(ARCID_ERROR_WINDOW_GRA, ERROR_WINDOW_GRA_PLTT, FALSE, WT_HEAPID);
    if (NNS_G2dGetUnpackedPaletteData(file, &palette)) {
        DC_FlushRange(palette->pRawData, 0x20);
        MI_CpuCopy16(palette->pRawData, (void *)HW_BG_PLTT, 0x20);
    }
    GFL_HeapFree(file);
    map = G2_GetBG1ScrPtr();
    for (y = TEXT_TOP; y < TEXT_TOP + TEXT_HEIGHT; y++) {
        for (x = TEXT_LEFT; x < TEXT_LEFT + TEXT_WIDTH; x++) map[y * 32 + x] = tile++;
    }
    app->bitmap = GFL_BitmapWrapVRAM((u8 *)G2_GetBG1CharPtr() + FIRST_TEXT_TILE * TILE_SIZE, TEXT_WIDTH, TEXT_HEIGHT,
        TILE_SIZE, WT_HEAPID);
    app->font = GFL_FontCreate(ARCID_FONT, 0, 0, FALSE, WT_HEAPID);
    app->line = GFL_StrBufCreate(64, WT_HEAPID);
    GX_SetMasterBrightness(0);
    GXS_SetMasterBrightness(0);
}

static void Screen_Free(WtApp *app)
{
    if (app->line) GFL_StrBufFree(app->line);
    if (app->font) GFL_FontFree(app->font);
    if (app->bitmap) GFL_BitmapFree(app->bitmap);
    app->line = NULL; app->font = NULL; app->bitmap = NULL;
    GX_SetMasterBrightness(-16);
    GXS_SetMasterBrightness(-16);
}

/* Prints text from chars up to the end, a line break or count characters. */
static int Screen_Print(WtApp *app, const u16 *chars, int count, int x, int y)
{
    int length = 0;
    while (length < count && chars[length] != 0 && chars[length] != '\n' && chars[length] != 0xffff) length++;
    if (length) {
        u16 copy[65];
        memcpy(copy, chars, length * sizeof(u16));
        copy[length] = 0xffff;
        GFL_StrBufLoadString(app->line, copy);
        GFL_TextRendererDrawToBitmapEx(app->bitmap, (s16)x, (s16)y, app->line, app->font,
            PRINT_COLOR(4, 11, TEXT_BACKGROUND));
    }
    return length;
}

/* Prints every line of a text from y; returns the y after it. */
static int Screen_Text(WtApp *app, const u16 *text, int y)
{
    while (*text) {
        text += Screen_Print(app, text, 64, 0, y);
        y += LINE_HEIGHT;
        if (*text == '\n') text++;
    }
    return y;
}

static void Number(u16 *out, unsigned value)
{
    u16 digits[10];
    int count = 0, i = 0;
    do { digits[count++] = (u16)('0' + value % 10); value /= 10; } while (value && count < 10);
    while (count) out[i++] = digits[--count];
    out[i] = 0;
}

static int Actions(const WtApp *app, int labels[MAX_ACTIONS])
{
    switch (app->state) {
    case WT_APP_MENU:
        labels[0] = app->network.tradePhase == WT_JOURNAL_IDLE ? WT_TEXT_TRADE : WT_TEXT_COLLECT;
        labels[1] = app->network.tradePhase == WT_JOURNAL_IDLE ? WT_TEXT_INFO : WT_TEXT_CANCEL_OFFER;
        labels[2] = WT_TEXT_EXIT;
        return 3;
    case WT_APP_CONFIRM: case WT_APP_COLLECT_CONFIRM: case WT_APP_CANCEL_CONFIRM:
        labels[0] = WT_TEXT_CONFIRM_ACTION;
        labels[1] = WT_TEXT_BACK;
        return 2;
    case WT_APP_ERROR:
        labels[0] = WT_TEXT_RETRY;
        if (!app->network.canLeave) return 1;
        labels[1] = WT_TEXT_EXIT;
        return 2;
    case WT_APP_MESSAGE: case WT_APP_RECEIVED:
        labels[0] = WT_TEXT_BACK;
        return 1;
    case WT_APP_RECOVERY:
        labels[0] = WT_TEXT_RECOVERY_ACTION;
        return 1;
    case WT_APP_CONNECTING: case WT_APP_PINGING:
        if (!app->network.canLeave || app->boot) return 0;
        labels[0] = WT_TEXT_CANCEL;
        return 1;
    default:
        return 0;
    }
}

static PokeParty *Party(const WtApp *app)
{
    return SaveControl_GetPokePartySave(app->game.save);
}

static void Draw(WtApp *app)
{
    static const u16 cursor[] = { '>', 0 }, level[] = { 'L', 'v', '.', 0 };
    int labels[MAX_ACTIONS], count, i, y;
    u16 name[16], digits[12];
    if (!app->bitmap) return;
    MIi_CpuClear16(TEXT_BACKGROUND * 0x1111, (u8 *)G2_GetBG1CharPtr() + FIRST_TEXT_TILE * TILE_SIZE,
                   TEXT_WIDTH * TEXT_HEIGHT * TILE_SIZE);
    Screen_Text(app, gWtText[WT_TEXT_TITLE], 0);
    if (app->state == WT_APP_PICKER) {
        PokeParty *party = Party(app);
        y = Screen_Text(app, gWtText[WT_TEXT_PICK], LINE_HEIGHT);
        for (i = 0; i < party->count && i < PARTY_SIZE; ++i) {
            if (i == app->cursor) Screen_Print(app, cursor, 2, 0, y);
            memset(name, 0xff, sizeof(name));
            PokeParty_GetParam(&party->pkms[i], PKM_PARAM_NICKNAME_RAW, name);
            name[11] = 0xffff;
            Screen_Print(app, name, 11, 12, y);
            Screen_Print(app, level, 3, 140, y);
            Number(digits, PokeParty_GetParam(&party->pkms[i], PKM_PARAM_LEVEL, NULL));
            Screen_Print(app, digits, 10, 164, y);
            y += LINE_HEIGHT;
        }
        return;
    }
    y = Screen_Text(app, gWtText[app->message], LINE_HEIGHT);
    if (app->state == WT_APP_RECEIVED && app->network.hasReceived) {
        Screen_Print(app, gWtText[WT_TEXT_RECEIVED], 32, 0, y);
        Screen_Print(app, app->network.received.nickname, WT_GAME_NAME_LENGTH, 100, y);
        y += LINE_HEIGHT;
        Screen_Print(app, gWtText[WT_TEXT_ORIGINAL_TRAINER], 32, 0, y);
        Screen_Print(app, app->network.received.originalTrainer, WT_GAME_TRAINER_NAME_LENGTH, 100, y);
        y += LINE_HEIGHT;
    }
    if (app->state == WT_APP_ERROR) {
        Screen_Print(app, gWtText[WT_TEXT_ERROR_CODE], 32, 0, y);
        Number(digits, (unsigned)(app->network.error < 0 ? -app->network.error : app->network.error));
        if (app->network.error < 0) { memmove(digits + 1, digits, sizeof(digits) - sizeof(u16)); digits[0] = '-'; }
        Screen_Print(app, digits, 11, 140, y);
        y += LINE_HEIGHT;
    }
    count = Actions(app, labels);
    y = (TEXT_HEIGHT * 8) - count * LINE_HEIGHT;
    for (i = 0; i < count; ++i, y += LINE_HEIGHT) {
        if (i == app->cursor) Screen_Print(app, cursor, 2, 0, y);
        Screen_Text(app, gWtText[labels[i]], y);
    }
}

static void Show(WtApp *app, int state, int message)
{
    app->state = state;
    app->message = message;
    app->cursor = 0;
    Draw(app);
}

static int RefusalText(unsigned refusal, int fallback)
{
    switch (refusal) {
    case WT_REFUSAL_EGG: return WT_TEXT_REFUSED_EGG;
    case WT_REFUSAL_MAIL: return WT_TEXT_REFUSED_MAIL;
    case WT_REFUSAL_INCOMPATIBLE: return WT_TEXT_REFUSED_GAME;
    case WT_REFUSAL_ILLEGAL: return WT_TEXT_REFUSED_ILLEGAL;
    case WT_REFUSAL_NAME: return WT_TEXT_REFUSED_NAME;
    default: return fallback;
    }
}

static void Close(WtApp *app)
{
    if (!app->network.canLeave) return;
    WonderTradeNetwork_Close(&app->network);
    Show(app, WT_APP_CLOSING, WT_TEXT_DISCONNECTING);
}

static void Connect(WtApp *app)
{
    app->started = TRUE;
    WonderTradeNetwork_Start(&app->network, &app->game);
    Show(app, WT_APP_CONNECTING, WT_TEXT_CONNECTING);
}

static void Back(WtApp *app)
{
    if (app->state == WT_APP_CONFIRM || app->state == WT_APP_PICKER) {
        app->selectedSlot = -1;
        Show(app, WT_APP_MENU, WT_TEXT_CHOOSE);
    } else if (app->state == WT_APP_COLLECT_CONFIRM || app->state == WT_APP_CANCEL_CONFIRM) {
        Show(app, WT_APP_MENU, WT_TEXT_QUEUED);
    } else if ((app->state == WT_APP_MESSAGE || app->state == WT_APP_RECEIVED) && !app->network.needsEntropy
               && !app->boot) {
        Show(app, WT_APP_MENU, app->network.tradePhase == WT_JOURNAL_IDLE ? WT_TEXT_CHOOSE : WT_TEXT_QUEUED);
    } else {
        Close(app);
    }
}

static void Pick(WtApp *app, int slot)
{
    if (!WtGameSave_Eligible(&app->game, (unsigned)slot)) {
        Show(app, WT_APP_MESSAGE, WT_TEXT_INELIGIBLE);
    } else if (!WtGameSave_Unique(&app->game, (unsigned)slot)) {
        Show(app, WT_APP_MESSAGE, WT_TEXT_DUPLICATE);
    } else {
        app->selectedSlot = slot;
        Show(app, WT_APP_CONFIRM, WT_TEXT_CONFIRM);
    }
}

static void Activate(WtApp *app, int choice)
{
    switch (app->state) {
    case WT_APP_RECOVERY:
        Connect(app);
        break;
    case WT_APP_CONNECTING: case WT_APP_PINGING:
        Close(app);
        break;
    case WT_APP_PICKER:
        Pick(app, choice);
        break;
    case WT_APP_CONFIRM: case WT_APP_COLLECT_CONFIRM: case WT_APP_CANCEL_CONFIRM:
        if (choice == 1) {
            Back(app);
        } else {
            if (app->state == WT_APP_CONFIRM) WonderTradeNetwork_Trade(&app->network, app->selectedSlot);
            else WonderTradeNetwork_Collect(&app->network, app->state == WT_APP_CANCEL_CONFIRM);
            Show(app, WT_APP_PINGING, WT_TEXT_PINGING);
        }
        break;
    case WT_APP_MESSAGE: case WT_APP_RECEIVED:
        Back(app);
        break;
    case WT_APP_ERROR:
        if (choice == 0) {
            WonderTradeNetwork_Close(&app->network);
            Show(app, WT_APP_RESTART, WT_TEXT_DISCONNECTING);
        } else Close(app);
        break;
    case WT_APP_MENU:
        if (choice == 0) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE) Show(app, WT_APP_PICKER, WT_TEXT_PICK);
            else Show(app, WT_APP_COLLECT_CONFIRM, WT_TEXT_CONFIRM_COLLECT);
        } else if (choice == 1) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE) Show(app, WT_APP_MESSAGE, WT_TEXT_EXPLANATION);
            else Show(app, WT_APP_CANCEL_CONFIRM, WT_TEXT_CONFIRM_CANCEL);
        } else Close(app);
        break;
    }
}

static void Input(WtApp *app)
{
    int labels[MAX_ACTIONS];
    int count = app->state == WT_APP_PICKER ? Party(app)->count : Actions(app, labels);
    u32 keys = GCTX_HIDGetPressedKeys();
    if (app->state != WT_APP_RECOVERY && (keys & (PAD_BUTTON_B | PAD_BUTTON_X))) {
        Back(app);
        return;
    }
    if (count <= 0) return;
    if (keys & (PAD_KEY_UP | PAD_KEY_DOWN)) {
        app->cursor = (app->cursor + ((keys & PAD_KEY_UP) ? count - 1 : 1)) % count;
        Draw(app);
    } else if (keys & PAD_BUTTON_A) {
        Activate(app, app->cursor);
    }
}

static void Responded(WtApp *app)
{
    WonderTradeNetwork *net = &app->network;
    app->selectedSlot = -1;
    if (net->needsEntropy) Show(app, WT_APP_MESSAGE, WT_TEXT_KEEP_PLAYING);
    else if (app->boot && net->canLeave && WtRecovery_CanWrite()) Close(app);
    else if (net->completed && net->returned) Show(app, WT_APP_MESSAGE, RefusalText(net->refusal, WT_TEXT_RETURNED));
    else if (net->completed) Show(app, WT_APP_RECEIVED, WT_TEXT_COMPLETED);
    else if (net->rejected) Show(app, WT_APP_MESSAGE, RefusalText(net->refusal, WT_TEXT_REJECTED));
    else if (net->superseded && net->tradePhase == WT_JOURNAL_IDLE) Show(app, WT_APP_MESSAGE, WT_TEXT_SUPERSEDED);
    else if (net->refused) Show(app, WT_APP_MESSAGE, WT_TEXT_CANNOT_HOLD);
    else if (net->waiting) Show(app, WT_APP_MESSAGE, WT_TEXT_WAITING);
    else if (net->tradePhase == WT_JOURNAL_RECEIVED || net->tradePhase == WT_JOURNAL_RESTORING)
        Show(app, WT_APP_MENU, WT_TEXT_NO_ROOM);
    else if (app->boot && !net->canLeave) Show(app, WT_APP_RECOVERY, WT_TEXT_RECOVERY_REQUIRED);
    else Show(app, WT_APP_MENU, net->tradePhase == WT_JOURNAL_IDLE ? WT_TEXT_CHOOSE : WT_TEXT_QUEUED);
}

WtApp *WtApp_Create(BOOL boot)
{
    WtApp *app;
    if (GFL_HeapGetHighestAllocatableSize(HEAPID_USER) < WT_HEAP_SIZE + 0x1000) return NULL;
    GFL_HeapCreateChild(HEAPID_USER, WT_HEAPID, WT_HEAP_SIZE);
    app = GFL_HeapAllocateCore(WT_HEAPID, sizeof(*app));
    if (!app) {
        GFL_HeapDelete(WT_HEAPID);
        return NULL;
    }
    memset(app, 0, sizeof(*app));
    app->boot = boot;
    app->game.save = SaveControl_GetInstance();
    app->game.heapId = WT_HEAPID;
    app->selectedSlot = -1;
    Screen_Init(app);
    if (boot) {
        Show(app, WT_APP_RECOVERY, WT_TEXT_RECOVERY_REQUIRED);
    } else if (WtGameSave_ReleaseMusicalArea(&app->game) != 0) {
        /* The journal may never be written while the game could read its area. */
        Show(app, WT_APP_MESSAGE, WT_TEXT_PING_ERROR);
    } else {
        Connect(app);
    }
    return app;
}

/* TRUE once the screen is over and its memory can be freed. */
BOOL WtApp_Update(WtApp *app)
{
    BOOL couldLeave = app->network.canLeave;
    if (app->state == WT_APP_DONE) return TRUE;
    if (!app->started) {
        Input(app);
        if (app->state == WT_APP_CLOSING || (app->state == WT_APP_MESSAGE && GCTX_HIDGetPressedKeys() & PAD_BUTTON_A)) {
            app->state = WT_APP_DONE;
            return TRUE;
        }
        return FALSE;
    }
    WonderTradeNetwork_Update(&app->network);
    if (couldLeave != app->network.canLeave) Draw(app);
    if (app->state == WT_APP_CLOSING) {
        if (app->network.state != WT_NET_CLOSED) return FALSE;
        app->state = WT_APP_DONE;
        return TRUE;
    }
    if (app->state == WT_APP_RESTART) {
        if (app->network.state == WT_NET_CLOSED) Connect(app);
        return FALSE;
    }
    if (app->state == WT_APP_CONNECTING || app->state == WT_APP_PINGING) {
        Input(app);
        if (app->state == WT_APP_CLOSING) return FALSE;
        if (app->network.state == WT_NET_ERROR) {
            Show(app, WT_APP_ERROR, app->state == WT_APP_CONNECTING ? WT_TEXT_CONNECTION_ERROR : WT_TEXT_PING_ERROR);
        } else if (app->state == WT_APP_CONNECTING && app->network.state == WT_NET_READY) {
            WonderTradeNetwork_Trade(&app->network, -1);
            Show(app, WT_APP_PINGING, WT_TEXT_PINGING);
        } else if (app->network.state == WT_NET_RESPONSE) {
            Responded(app);
        }
        return FALSE;
    }
    Input(app);
    return FALSE;
}

void WtApp_Free(WtApp *app)
{
    mbedtls_platform_zeroize(&app->network.received, sizeof(app->network.received));
    Screen_Free(app);
    mbedtls_platform_zeroize(app, sizeof(*app));
    GFL_HeapFreeCore(app);
    GFL_HeapDelete(WT_HEAPID);
}

/* The process the field event runs with the field closed. */
static WtApp *sApp;

static BOOL WtProc_Init(GameProc *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    sApp = WtApp_Create(FALSE);
    return TRUE;
}

static BOOL WtProc_Main(GameProc *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    return sApp == NULL || WtApp_Update(sApp);
}

static BOOL WtProc_End(GameProc *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    if (sApp) WtApp_Free(sApp);
    sApp = NULL;
    return TRUE;
}

static const GameProcFunctions sWtProcFunctions = { WtProc_Init, WtProc_Main, WtProc_End };

/* The field event, as the Global Trade Station's (event_worldtrade.c): wait
 * for the C-Gear's communication to end, fade, close the field, run the
 * screen, then open the field again. It lives in ARM9 main because Wi-Fi
 * unloads overlay 12, where the field menu's own events are. */
enum { STEP_WAIT_COMM, STEP_FADE_OUT, STEP_CLOSE_FIELD, STEP_START_PROC, STEP_WAIT_PROC, STEP_FADE_IN, STEP_END };

typedef struct WtEventWork {
    GameSystem *gsys;
    Field *field;
    u32 bgm;
} WtEventWork;

static GameEventReturnCode WtEvent_Callback(GameEvent *event, u32 *state, void *data)
{
    WtEventWork *wk = data;
    switch (*state) {
    case STEP_WAIT_COMM:
        if (!GameCommSys_BootCheck(GSYS_GetGameCommSystem(wk->gsys))) *state = STEP_FADE_OUT;
        break;
    case STEP_FADE_OUT:
        GameEvent_ChainNext(event,
            CallFieldMapEntranceOutTransitionDefault(wk->gsys, wk->field, FIELD_TRANSITION_FADE_BLACK, 0));
        wk->bgm = GFL_SndBGMGetID();
        GFL_SndBGMFadeOut(6);
        *state = STEP_CLOSE_FIELD;
        break;
    case STEP_CLOSE_FIELD:
        GameEvent_ChainNext(event, CreateFieldCloseEvent(wk->gsys, wk->field));
        *state = STEP_START_PROC;
        break;
    case STEP_START_PROC:
        GSYS_QueueProc(wk->gsys, OVERLAY_NONE, &sWtProcFunctions, NULL);
        *state = STEP_WAIT_PROC;
        break;
    case STEP_WAIT_PROC:
        if (!GSYS_GetProcMgrState(wk->gsys)) {
            GFL_SndBGMPlay(wk->bgm, SND_CHANNEL_MASK_ALL);
            GFL_SndBGMFadeIn(60);
            GameEvent_ChainNext(event, EventFieldOpen_CreateHeadless(wk->gsys));
            *state = STEP_FADE_IN;
        }
        break;
    case STEP_FADE_IN:
        GameEvent_ChainNext(event,
            CallFieldMapEntranceInTransition(wk->gsys, wk->field, FIELD_TRANSITION_FADE_BLACK, 0, 1, 0, 0));
        *state = STEP_END;
        break;
    case STEP_END:
        return GAMEEVENT_DONE;
    }
    return GAMEEVENT_CONTINUE;
}

GameEvent *WtEvent_Create(GameSystem *gsys, Field *field)
{
    GameEvent *event = GameEvent_Create(gsys, NULL, WtEvent_Callback, sizeof(WtEventWork));
    WtEventWork *wk = GameEvent_GetData(event);
    if (GameCommSys_BootCheck(GSYS_GetGameCommSystem(gsys))) GameCommSys_ExitReq(GSYS_GetGameCommSystem(gsys));
    MI_CpuFill8(wk, 0, sizeof(*wk));
    wk->gsys = gsys;
    wk->field = field;
    return event;
}
