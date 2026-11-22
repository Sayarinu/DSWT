#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "wonder_trade/bw/application.h"
#include "wonder_trade/bw/game.h"
#include "wonder_trade/bw/game_save.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"
#include "../bw2/text.h"

/* Wonder Trade's screen for Black and White. It runs as a process with the
 * field closed, as the Global Trade Station does, from its own child heap of
 * heap 1, and at boot when the save needs recovery. The screen is the window
 * the game shows when it must stop (APP_POWER_ShowShutdownScreen,
 * arm9/app_power.c) with the text drawn into it: a title, the status and a
 * short list of actions, chosen with the +Control Pad and A, B to go back. */

typedef char WtWideCharCheck[sizeof(L"a"[0]) == 2 ? 1 : -1];

/* Heap IDs end at 0xAC (GFL_HEAP_Init in GFL_USE_InitSystem); the game uses none above 0x96. */
#define WT_HEAPID 0xAC
#define WT_HEAP_SIZE 0x70000
/* The shutdown window's text area (APP_POWER_DrawShutdownScreen): tiles from 0x20, 30x16 tiles at (1, 4). */
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
    struct Bmp *bitmap;
    struct GfFont *font;
    struct Strbuf *line;
    int state, message, cursor, selectedSlot;
    BOOL boot, started;
};

/* The registers APP_POWER_ShowShutdownScreen sets up as the SDK's GX calls do. */
#define REG_DISPCNT (*(vu32 *)0x04000000)
#define REG_BG1CNT (*(vu16 *)0x0400000A)
#define REG_BG1HOFS (*(vu32 *)0x04000014)
#define REG_BLDCNT (*(vu16 *)0x04000050)
#define REG_MASTER_BRIGHT (*(vu16 *)0x0400006C)
#define REG_DB_DISPCNT (*(vu32 *)0x04001000)
#define REG_DB_MASTER_BRIGHT (*(vu16 *)0x0400106C)
#define HW_BG_PLTT 0x05000000
#define HW_DB_BG_PLTT 0x05000400
#define GX_VRAM_BG_128_C 4
/* The shutdown window's graphics (palette, characters, screen) and the font. */
#define ARC_SHUTDOWN_WINDOW 0x16
#define ARC_FONT 0x17
#define RGB(r, g, b) ((r) | ((g) << 5) | ((b) << 10))

static void Brightness(int level)
{
    GXx_SetMasterBrightness_(&REG_MASTER_BRIGHT, level);
    GXx_SetMasterBrightness_(&REG_DB_MASTER_BRIGHT, level);
}

static void Screen_Init(WtApp *app)
{
    void *file;
    WtG2dCharacter *chars;
    WtG2dScreen *screen;
    WtG2dPalette *palette;
    u16 *map;
    int x, y;
    u16 tile = FIRST_TEXT_TILE;
    Brightness(-16);
    GX_ResetBankForBG();
    GX_ResetBankForSubBG();
    GX_SetBankForBG(GX_VRAM_BG_128_C);
    GX_SetGraphicsMode(1, 0, 0);
    REG_DISPCNT = (REG_DISPCNT & 0xFFFFE0FF) | 0x200; /* BG1 only */
    REG_DISPCNT = REG_DISPCNT & 0xFFFF1FFF;           /* no windows */
    REG_BG1CNT = (REG_BG1CNT & 0x43) | 4;            /* 256x256, 16 colors, characters at 0x4000 */
    REG_BG1CNT = REG_BG1CNT & 0xFFBF;                 /* no mosaic */
    REG_BG1HOFS = 0;
    REG_BLDCNT = 0;
    BG_SYS_SetSubVisiblePlanes(0);
    REG_DB_DISPCNT |= 0x10000;
    *(vu16 *)HW_DB_BG_PLTT = RGB(10, 23, 31);
    MIi_CpuClear32(0, G2_GetBG1CharPtr(), 0x4000);
    file = ARC_UTIL_LoadMember(ARC_SHUTDOWN_WINDOW, 1, 0, WT_HEAPID);
    if (NNS_G2dGetUnpackedCharacterDataEx(file, (void **)&chars)) {
        DC_FlushRange(chars->raw, chars->size);
        MI_CpuCopy16(chars->raw, G2_GetBG1CharPtr(), chars->size);
    }
    GFL_HEAP_Free(file);
    file = ARC_UTIL_LoadMember(ARC_SHUTDOWN_WINDOW, 2, 0, WT_HEAPID);
    if (NNS_G2dGetUnpackedScreenData(file, (void **)&screen)) {
        DC_FlushRange(screen->raw, screen->size);
        MI_CpuCopy16(screen->raw, G2_GetBG1ScrPtr(), screen->size);
    }
    GFL_HEAP_Free(file);
    file = ARC_UTIL_LoadMember(ARC_SHUTDOWN_WINDOW, 0, 0, WT_HEAPID);
    if (NNS_G2dGetUnpackedPaletteData(file, (void **)&palette)) {
        DC_FlushRange(palette->raw, 0x20);
        MI_CpuCopy16(palette->raw, (void *)HW_BG_PLTT, 0x20);
    }
    GFL_HEAP_Free(file);
    map = G2_GetBG1ScrPtr();
    for (y = TEXT_TOP; y < TEXT_TOP + TEXT_HEIGHT; y++) {
        for (x = TEXT_LEFT; x < TEXT_LEFT + TEXT_WIDTH; x++) map[y * 32 + x] = tile++;
    }
    app->bitmap = BMP_CreateFromVram((u8 *)G2_GetBG1CharPtr() + FIRST_TEXT_TILE * TILE_SIZE, TEXT_WIDTH, TEXT_HEIGHT,
        TILE_SIZE, WT_HEAPID);
    app->font = GF_FONT_Create(ARC_FONT, 0, 0, 0, WT_HEAPID);
    app->line = STRBUF_Create(64, WT_HEAPID);
    Brightness(0);
}

static void Screen_Free(WtApp *app)
{
    if (app->line) STRBUF_Delete(app->line);
    if (app->font) GF_FONT_Delete(app->font);
    if (app->bitmap) BMP_Delete(app->bitmap);
    app->line = NULL; app->font = NULL; app->bitmap = NULL;
    Brightness(-16);
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
        STRBUF_SetString(app->line, copy);
        PRINTSYS_PrintColor(app->bitmap, (u32)x, (s16)y, app->line, app->font, WT_PRINT_COLOR(4, 11, TEXT_BACKGROUND));
    }
    return length;
}

/* Prints every line of a text from y; returns the y after it. */
static int Screen_Text(WtApp *app, const u16 *text, int x, int y)
{
    while (*text) {
        text += Screen_Print(app, text, 64, x, y);
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

static WtParty *Party(const WtApp *app)
{
    return POKEPARTY_GetSaveData(app->game.save);
}

static void Draw(WtApp *app)
{
    static const u16 cursor[] = { '>', 0 }, level[] = { 'L', 'v', '.', 0 };
    int labels[MAX_ACTIONS], count, i, y;
    u16 name[16], digits[12];
    if (!app->bitmap) return;
    MIi_CpuClear16(TEXT_BACKGROUND * 0x1111, (u8 *)G2_GetBG1CharPtr() + FIRST_TEXT_TILE * TILE_SIZE,
                   TEXT_WIDTH * TEXT_HEIGHT * TILE_SIZE);
    Screen_Text(app, gWtText[WT_TEXT_TITLE], 0, 0);
    if (app->state == WT_APP_PICKER) {
        WtParty *party = Party(app);
        y = Screen_Text(app, gWtText[WT_TEXT_PICK], 0, LINE_HEIGHT);
        for (i = 0; i < party->count && i < WT_PARTY_SIZE; ++i) {
            if (i == app->cursor) Screen_Print(app, cursor, 2, 0, y);
            memset(name, 0xff, sizeof(name));
            POKE_TOOL_GetPartyValue(&party->members[i], WT_PARAM_NICKNAME_RAW, (u32)name);
            name[11] = 0xffff;
            Screen_Print(app, name, 11, 12, y);
            Screen_Print(app, level, 3, 140, y);
            Number(digits, POKE_TOOL_GetPartyValue(&party->members[i], WT_PARAM_LEVEL, 0));
            Screen_Print(app, digits, 10, 164, y);
            y += LINE_HEIGHT;
        }
        return;
    }
    y = Screen_Text(app, gWtText[app->message], 0, LINE_HEIGHT);
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
        Screen_Text(app, gWtText[labels[i]], 12, y);
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
    u32 keys = KEY_GetTrg();
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
    if (GFL_HEAP_GetAllocatableSize(WT_HEAP_APP) < WT_HEAP_SIZE + 0x1000
        || !GFL_HEAP_CreateChildHeapImpl(WT_HEAP_APP, WT_HEAPID, WT_HEAP_SIZE)) return NULL;
    app = GFL_HEAP_AllocMemory(WT_HEAPID, sizeof(*app));
    if (!app) {
        GFL_HEAP_DeleteChecked(WT_HEAPID);
        return NULL;
    }
    memset(app, 0, sizeof(*app));
    app->boot = boot;
    app->game.save = SAVE_CONTROL_Get();
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
        if (app->state == WT_APP_CLOSING || (app->state == WT_APP_MESSAGE && KEY_GetTrg() & PAD_BUTTON_A)) {
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
    GFL_HEAP_FreeMemory(app);
    GFL_HEAP_DeleteChecked(WT_HEAPID);
}

/* The process the field event runs with the field closed. */
static WtApp *sApp;

static u32 WtProc_Init(void *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    sApp = WtApp_Create(FALSE);
    return TRUE;
}

static u32 WtProc_Main(void *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    return sApp == NULL || WtApp_Update(sApp);
}

static u32 WtProc_End(void *proc, u32 *state, void *param, void *work)
{
    (void)proc; (void)state; (void)param; (void)work;
    if (sApp) WtApp_Free(sApp);
    sApp = NULL;
    return TRUE;
}

static const WtProcData sWtProcData = { WtProc_Init, WtProc_Main, WtProc_End };

/* The field event, as the X menu's own app calls run (ov010/event_shortcut_menu.c)
 * and as the Global Trade Station's (ov004/event_worldtrade.c) fades the music:
 * wait for the C-Gear's communication to end, fade, close the field, run the
 * screen, then open the field again. It lives in the module in ARM9 main
 * because Wi-Fi unloads overlay 10, where the field menu's own events are. */
enum { STEP_WAIT_COMM, STEP_FADE_OUT, STEP_CLOSE_FIELD, STEP_START_PROC, STEP_WAIT_PROC, STEP_OPEN, STEP_FADE_IN,
       STEP_END };

typedef struct WtEventWork {
    struct GameSystem *gsys;
    struct FieldMap *field;
    u32 bgm;
} WtEventWork;

static int WtEvent_Callback(struct GameEvent *event, int *state, void *data)
{
    WtEventWork *wk = data;
    switch (*state) {
    case STEP_WAIT_COMM:
        if (!GAME_COMM_GetKind(GAMESYSTEM_GetGameComm(wk->gsys))) *state = STEP_FADE_OUT;
        break;
    case STEP_FADE_OUT:
        GAME_EVENT_CallEvent(event, EVENT_FIELDMAP_CONTROL_StartFadeOut(wk->gsys, wk->field, 0, 0));
        wk->bgm = GFL_USE_GetBgmNo();
        GFL_USE_FadeOutBgm(6);
        *state = STEP_CLOSE_FIELD;
        break;
    case STEP_CLOSE_FIELD:
        GAME_EVENT_CallEvent(event, FIELD_STATUS_CreateFieldCloseEvent(wk->gsys, wk->field));
        *state = STEP_START_PROC;
        break;
    case STEP_START_PROC:
        GAMESYSTEM_CallProc(wk->gsys, WT_OVERLAY_NONE, &sWtProcData, NULL);
        *state = STEP_WAIT_PROC;
        break;
    case STEP_WAIT_PROC:
        if (!GAMESYSTEM_GetProcStatus(wk->gsys)) {
            GFL_USE_CancelChangeAndPlayBgm(wk->bgm, 0xFFFF);
            GFL_USE_FadeInBgmFull(60);
            GAME_EVENT_CallEvent(event, FIELD_STATUS_CreateFieldOpenEvent(wk->gsys));
            *state = STEP_OPEN;
        }
        break;
    case STEP_OPEN:
        wk->field = GAMESYSTEM_GetFieldMap(wk->gsys);
        FIELDMAP_SetBgMode();
        FIELDMAP_ResetBgAffineShowMsg(wk->field);
        *state = STEP_FADE_IN;
        break;
    case STEP_FADE_IN:
        GAME_EVENT_CallEvent(event, EVENT_FIELDMAP_CONTROL_StartFadeIn(wk->gsys, wk->field, 0, 0, 0, 0, 0));
        *state = STEP_END;
        break;
    case STEP_END:
        return WT_EVENT_DONE;
    }
    return WT_EVENT_CONTINUE;
}

struct GameEvent *WtEvent_Create(struct GameSystem *gsys, struct FieldMap *field)
{
    struct GameEvent *event = GAME_EVENT_Create(gsys, NULL, WtEvent_Callback, sizeof(WtEventWork));
    WtEventWork *wk = GAME_EVENT_GetWork(event);
    if (GAME_COMM_GetKind(GAMESYSTEM_GetGameComm(gsys))) GAME_COMM_RequestExitNoCallback(GAMESYSTEM_GetGameComm(gsys));
    MI_CpuFill8(wk, 0, sizeof(*wk));
    wk->gsys = gsys;
    wk->field = field;
    return event;
}
