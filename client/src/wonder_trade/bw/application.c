#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "wonder_trade/bw/application.h"
#include "wonder_trade/bw/game.h"
#include "wonder_trade/bw/game_save.h"
#include "wonder_trade/gen5_screen.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"
#include "../bw2/text.h"

/* Wonder Trade's screen for Black and White. It runs as a process with the
 * field closed, as the Global Trade Station does, from its own child heap of
 * heap 1, and at boot when the save needs recovery. It is laid out as the
 * station's title menu (ov135/worldtrade_title.c; wonder_trade/gen5_screen.h):
 * on the upper screen the station's title bar with the title, over its grid,
 * and a message window in the game's frame holding the status; on the lower
 * screen the actions as the game's task menu buttons (arm9/app_taskmenu.c),
 * with its cursor and sounds, picked with the +Control Pad and A or by touch,
 * B to go back. The Pokémon to offer is picked on the game's own party screen,
 * and a received Pokémon is shown with the game's own trade animation first. */

typedef char WtWideCharCheck[sizeof(L"a"[0]) == 2 ? 1 : -1];

/* Heap IDs end at 0xAC (GFL_HEAP_Init in GFL_USE_InitSystem); the game uses none above 0x96. */
#define WT_HEAPID 0xAC
#define WT_HEAP_SIZE 0x70000
/* The screen's own heap, freed while the trade animation runs. */
#define WT_SCREEN_HEAPID 0xAB
#define WT_SCREEN_HEAP_SIZE 0x20000
/* The boot check's message needs only the screen. */
#define WT_CHECKING_HEAP_SIZE 0x10000
/* The party screen's heap, which it makes in heap 1 (POKELIST_Init). */
#define WT_PARTY_LIST_HEAP_SIZE 0x30000
/* The trade animation's work heap, which it makes in heap 1 (POKETRADE_PROC_AllocTradeWork). */
#define WT_TRADE_HEAP_SIZE 0x96000
#define WT_FADE_FRAMES 16
#define WT_TRADE_OVERLAY 119
/* The overlays the Global Trade Station keeps loaded around the animation (ov135/worldtrade.c). */
#define WT_TRADE_OVERLAY_A 67
#define WT_TRADE_OVERLAY_B 114
#define MAX_ACTIONS 3
/* The BGs: on the upper screen the windows, the station's title bar and its
 * grid; on the lower screen the buttons and the grid. */
#define BG_TEXT 0
#define BG_BAR 1
#define BG_GRID 2
#define BG_BUTTONS 4
#define BG_BUTTONS_GRID 5
/* The station's archive and its graphics, with its palettes 0 to 2, as its
 * title menu loads them (WORLDTRADE_TITLE_LoadBgGraphics): the title is in palette 1. */
#define ARC_WORLDTRADE 0x60
#define GTS_PALETTE_FILE 3
#define GTS_CHAR_FILE 13
#define GTS_GRID_SCREEN 31
#define GTS_BAR_SCREEN 32
#define GTS_PALETTES_SIZE 0x60
#define GTS_SCREEN_SIZE 0x600
#define TITLE_PALETTE 1
#define TITLE_COLOR WT_PRINT_COLOR(13, 14, 10)
#define TITLE_BACKGROUND 10
/* The message window, in the font's palette, and the game's window frame. */
#define ARC_FONT 0x17
#define FONT_PALETTE_FILE 5
#define FONT_PALETTE 13
#define FRAME_PALETTE 14
#define FRAME_CHAR 1
#define TEXT_COLOR WT_PRINT_COLOR(1, 2, 15)
#define TEXT_BACKGROUND 15
/* The buttons' two palettes and text color, as the station's own choices (WORLDTRADE_OpenTaskMenuList). */
#define BUTTON_PALETTE 10
#define BUTTON_COLOR WT_PRINT_COLOR(14, 15, 3)

/* The trade animation's request (ov119/pokemontrade_types.h struct TradeRequest),
 * as the Global Trade Station fills it (ov135/worldtrade_types.h WorldtradeReq). */
typedef struct WtTradeRequest {
    u8 unused[0x30];
    struct GameData *gameData;
    WtPartyPokemon *sent;
    WtPartyPokemon *received;
    struct MyStatus *player;
    struct MyStatus *partner;
} WtTradeRequest;
typedef char WtTradeRequestCheck[sizeof(WtTradeRequest) == 0x44 ? 1 : -1];

enum WtAppState {
    WT_APP_CONNECTING,
    WT_APP_PINGING,
    WT_APP_MENU,
    WT_APP_PICKER_FADE,
    WT_APP_PICKER,
    WT_APP_CONFIRM,
    WT_APP_COLLECT_CONFIRM,
    WT_APP_CANCEL_CONFIRM,
    WT_APP_MESSAGE,
    WT_APP_RECEIVED,
    WT_APP_TRADE_FADE,
    WT_APP_TRADE,
    WT_APP_CHECKING,
    WT_APP_ERROR,
    WT_APP_RESTART,
    WT_APP_RECOVERY,
    WT_APP_CLOSING,
    WT_APP_DONE,
};

struct WtApp {
    WonderTradeNetwork network;
    WtGame game;
    /* The screen, from WT_SCREEN_HEAPID; font is NULL while it is closed. */
    struct GfFont *font;
    struct Strbuf *line;
    struct PrintQueue *queue;
    struct BmpWin *titleWindow, *messageWindow;
    struct TaskMenuRes *buttonRes;
    struct TaskMenuWork *buttons;
    int state, message, selectedSlot, fade;
    BOOL boot, started;
    /* For the trade animation; gameData is NULL at boot, which shows none. */
    struct GameData *gameData;
    /* The party screen's or the trade animation's process system. */
    struct ProcSys *procs;
    WtPartyListArg partyList;
    struct MyStatus *partner;
    WtPartyPokemon pkm[2];
    WtTradeRequest request;
};

typedef char WtTradeImageCheck[sizeof(WtPartyPokemon) <= WT_GAME_IMAGE_SIZE ? 1 : -1];
typedef char WtButtonsCheck[WT_GEN5_MAX_BUTTONS <= WT_TASKMENU_MAX_ITEMS && MAX_ACTIONS <= WT_GEN5_MAX_BUTTONS ? 1 : -1];

#define REG_POWCNT1 (*(vu16 *)0x04000304)
#define REG_BLDCNT (*(vu16 *)0x04000050)
#define REG_MASTER_BRIGHT (*(vu16 *)0x0400006C)
#define REG_DB_DISPCNT (*(vu32 *)0x04001000)
#define REG_DB_BLDCNT (*(vu16 *)0x04001050)
#define REG_DB_MASTER_BRIGHT (*(vu16 *)0x0400106C)

static void Brightness(int level)
{
    GXx_SetMasterBrightness_(&REG_MASTER_BRIGHT, level);
    GXx_SetMasterBrightness_(&REG_DB_MASTER_BRIGHT, level);
}

static void Screen_CreateBG(u8 layer, u8 screenBase, u8 charBase, u8 priority)
{
    WtBgTemplate bg = { 0, 0, 0x800, 0, WT_BG_SCREEN_256, 0, 0, 0, 0x8000, 0, 0, 0, 0, 0 };
    bg.screenBase = screenBase;
    bg.charBase = charBase;
    bg.priority = priority;
    BG_SYS_InitBgLayer(layer, &bg, 0);
    BG_SYS_ClearSlot(layer);
}

/* A window shown at once. */
static void Screen_Transfer(struct BmpWin *window, u8 layer)
{
    BMP_WIN_TransferChars(window);
    BMP_WIN_MakeScreen(window);
    BG_SYS_PushTile(layer);
}

/* Opens the screen, dark, in a heap of its own, as the station's
 * (WORLDTRADE_ClearAndShowPlanes, WORLDTRADE_TITLE_SetupBgPlanes): the main
 * engine on the upper screen; Create has checked heap 1 has room. */
static void Screen_Init(WtApp *app)
{
    static const WtDispVram vram = { WT_GX_VRAM_A, 0, WT_GX_VRAM_C, 0, 0, 0, 0, 0, 0, 0, WT_GX_OBJ_1D_32K,
                                     WT_GX_OBJ_1D_32K };
    WtDispConfig mode = { 1, 0, 0, 0 };
    void *arc;
    Brightness(-16);
    GFL_HEAP_CreateChildHeapImpl(WT_HEAP_APP, WT_SCREEN_HEAPID, WT_SCREEN_HEAP_SIZE);
    BG_SYS_SetMainVisiblePlanes(0);
    BG_SYS_SetSubVisiblePlanes(0);
    REG_BLDCNT = 0;
    REG_DB_BLDCNT = 0;
    REG_POWCNT1 |= 0x8000;
    BG_SYS_SetVramBanks(&vram);
    BG_SYS_Create(WT_SCREEN_HEAPID);
    BMP_WIN_Init(WT_SCREEN_HEAPID);
    BG_SYS_SetGraphicsMode(&mode);
    REG_DB_DISPCNT |= 0x10000;
    Screen_CreateBG(BG_TEXT, 0x1f, 0, 0);
    Screen_CreateBG(BG_BAR, 0x1e, 2, 1);
    Screen_CreateBG(BG_GRID, 0x1d, 2, 2);
    Screen_CreateBG(BG_BUTTONS, 0x1f, 0, 0);
    Screen_CreateBG(BG_BUTTONS_GRID, 0x1e, 2, 1);
    BG_SYS_InitRun(BG_TEXT, 0x20, 0, WT_SCREEN_HEAPID);
    BG_SYS_InitRun(BG_BUTTONS, 0x20, 0, WT_SCREEN_HEAPID);
    /* The station's grid on both screens, and its title menu's first rows, the title bar, on the upper one. */
    arc = ARC_TOOL_CreateHandle(ARC_WORLDTRADE, WT_SCREEN_HEAPID);
    ARC_UTIL_UploadPaletteFromHandleWhole(arc, GTS_PALETTE_FILE, WT_PALETTE_MAIN_BG, 0, GTS_PALETTES_SIZE,
                                          WT_SCREEN_HEAPID);
    ARC_UTIL_UploadPaletteFromHandleWhole(arc, GTS_PALETTE_FILE, WT_PALETTE_SUB_BG, 0, GTS_PALETTES_SIZE,
                                          WT_SCREEN_HEAPID);
    ARC_UTIL_UploadBgCharFromHandle(arc, GTS_CHAR_FILE, BG_BAR, 0, 0, TRUE, WT_SCREEN_HEAPID);
    ARC_UTIL_UploadBgCharFromHandle(arc, GTS_CHAR_FILE, BG_BUTTONS_GRID, 0, 0, TRUE, WT_SCREEN_HEAPID);
    ARC_UTIL_UploadBgPaletteFromHandle(arc, GTS_BAR_SCREEN, BG_BAR, 0, WT_GEN5_BAR_ROWS * 32 * sizeof(u16), TRUE,
                                       WT_SCREEN_HEAPID);
    ARC_UTIL_UploadBgPaletteFromHandle(arc, GTS_GRID_SCREEN, BG_GRID, 0, GTS_SCREEN_SIZE, TRUE, WT_SCREEN_HEAPID);
    ARC_UTIL_UploadBgPaletteFromHandle(arc, GTS_GRID_SCREEN, BG_BUTTONS_GRID, 0, GTS_SCREEN_SIZE, TRUE,
                                       WT_SCREEN_HEAPID);
    ARC_TOOL_DeleteHandle(arc);
    ARC_UTIL_UploadPaletteFromArchiveWhole(ARC_FONT, FONT_PALETTE_FILE, WT_PALETTE_MAIN_BG, FONT_PALETTE * 0x20, 0x20,
                                           WT_SCREEN_HEAPID);
    BMP_MENUWORK_LoadCharAndPltt(BG_TEXT, FRAME_CHAR, FRAME_PALETTE, 0, WT_SCREEN_HEAPID);
    app->font = GF_FONT_Create(ARC_FONT, 0, 0, 0, WT_SCREEN_HEAPID);
    app->line = STRBUF_Create(64, WT_SCREEN_HEAPID);
    app->queue = PRINTSYS_QUE_Create(WT_SCREEN_HEAPID);
    app->buttonRes = APP_TASKMENU_RES_Create(BG_BUTTONS, BUTTON_PALETTE, app->font, app->queue, WT_SCREEN_HEAPID);
    app->titleWindow = BMP_WIN_Create(BG_TEXT, WT_GEN5_TITLE_X, WT_GEN5_TITLE_Y, WT_GEN5_TITLE_WIDTH,
                                      WT_GEN5_TITLE_HEIGHT, TITLE_PALETTE, TRUE);
    app->messageWindow = BMP_WIN_Create(BG_TEXT, WT_GEN5_MESSAGE_X, WT_GEN5_MESSAGE_Y, WT_GEN5_MESSAGE_WIDTH,
                                        WT_GEN5_MESSAGE_HEIGHT, FONT_PALETTE, TRUE);
    BMP_Clear(BMP_WIN_GetBmp(app->messageWindow), TEXT_BACKGROUND);
    BMP_MENUWORK_DrawWindow(app->messageWindow, 0, FRAME_CHAR, FRAME_PALETTE);
    Screen_Transfer(app->messageWindow, BG_TEXT);
    BG_SYS_SetPlaneVisible(BG_TEXT, TRUE);
    BG_SYS_SetPlaneVisible(BG_BAR, TRUE);
    BG_SYS_SetPlaneVisible(BG_GRID, TRUE);
    BG_SYS_SetPlaneVisible(BG_BUTTONS, TRUE);
    BG_SYS_SetPlaneVisible(BG_BUTTONS_GRID, TRUE);
}

/* The party screen and the trade animation leave their banks and engine
 * layout; the screen takes the banks it needs. */
static void ResetBanks(void)
{
    GX_ResetBankForOBJ(); GX_ResetBankForBGExtPltt(); GX_ResetBankForOBJExtPltt(); GX_ResetBankForTex();
    GX_ResetBankForTexPltt(); GX_ResetBankForClearImage(); GX_ResetBankForSubOBJ(); GX_ResetBankForSubBGExtPltt();
    GX_ResetBankForSubOBJExtPltt();
}

static void Screen_Free(WtApp *app)
{
    if (!app->font) return;
    Brightness(-16);
    if (app->buttons) APP_TASKMENU_CloseMenu(app->buttons);
    APP_TASKMENU_RES_Delete(app->buttonRes);
    BMP_WIN_Delete(app->messageWindow);
    BMP_WIN_Delete(app->titleWindow);
    PRINTSYS_QUE_Clear(app->queue);
    PRINTSYS_QUE_Delete(app->queue);
    STRBUF_Delete(app->line);
    GF_FONT_Delete(app->font);
    BG_SYS_SetMainVisiblePlanes(0);
    BG_SYS_SetSubVisiblePlanes(0);
    BMP_WIN_Exit();
    BG_SYS_Delete();
    GFL_HEAP_DeleteChecked(WT_SCREEN_HEAPID);
    app->font = NULL; app->line = NULL; app->queue = NULL;
    app->titleWindow = NULL; app->messageWindow = NULL; app->buttonRes = NULL; app->buttons = NULL;
}

/* Loads a line of text from chars, up to the end, a line break or count
 * characters, into the screen's line; returns the characters taken. */
static int Screen_Line(WtApp *app, const u16 *chars, int count)
{
    u16 copy[65];
    int length = 0;
    while (length < count && length < 64 && chars[length] != 0 && chars[length] != '\n' && chars[length] != 0xffff)
        length++;
    memcpy(copy, chars, length * sizeof(u16));
    copy[length] = 0xffff;
    STRBUF_SetString(app->line, copy);
    return length;
}

/* Prints a line of text from chars into the message window. */
static int Screen_Print(WtApp *app, const u16 *chars, int count, int x, int y)
{
    int length = Screen_Line(app, chars, count);
    if (length) PRINTSYS_PrintColor(BMP_WIN_GetBmp(app->messageWindow), (u32)x, (s16)y, app->line, app->font, TEXT_COLOR);
    return length;
}

/* Prints every line of a text from y; returns the y after it. */
static int Screen_Text(WtApp *app, const u16 *text, int x, int y)
{
    while (*text) {
        text += Screen_Print(app, text, 64, x, y);
        y += WT_GEN5_LINE_HEIGHT;
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

static int PartyCount(const WtApp *app)
{
    int count = Party(app)->count;
    return count < WT_PARTY_SIZE ? count : WT_PARTY_SIZE;
}

/* A button's text. */
static struct Strbuf *Label(int text)
{
    u16 chars[24];
    const u16 *from;
    struct Strbuf *str = STRBUF_Create(24, WT_SCREEN_HEAPID);
    int length = 0;
    for (from = gWtText[text]; *from && length < 23; ) chars[length++] = *from++;
    chars[length] = 0xffff;
    STRBUF_SetString(str, chars);
    return str;
}

/* The lower screen's buttons: the actions. */
static void Buttons(WtApp *app)
{
    WtTaskMenuItem items[WT_GEN5_MAX_BUTTONS];
    WtTaskMenuInit init;
    int labels[MAX_ACTIONS], count, i;
    if (app->buttons) APP_TASKMENU_CloseMenu(app->buttons);
    app->buttons = NULL;
    count = Actions(app, labels);
    if (count <= 0) return;
    for (i = 0; i < count; ++i) {
        items[i].str = Label(labels[i]);
        items[i].color = BUTTON_COLOR;
        /* A button that leaves has the return arrow and the cancel sound, as B does. */
        items[i].type = labels[i] == WT_TEXT_BACK || labels[i] == WT_TEXT_EXIT || labels[i] == WT_TEXT_CANCEL
                            ? WT_TASKMENU_ITEM_RETURN : 0;
    }
    MI_CpuFill8(&init, 0, sizeof(init));
    init.heapId = WT_SCREEN_HEAPID;
    init.count = (u8)count;
    init.items = items;
    init.x = WT_GEN5_BUTTON_X;
    init.y = (u8)WtGen5_ButtonTop(count);
    init.width = WT_GEN5_BUTTON_WIDTH;
    init.height = WT_GEN5_BUTTON_HEIGHT;
    app->buttons = APP_TASKMENU_OpenMenu(&init, app->buttonRes);
    for (i = 0; i < count; ++i) STRBUF_Delete(items[i].str);
}

static void Draw(WtApp *app)
{
    struct Bmp *title;
    u16 digits[12];
    int y;
    if (!app->font) return;
    title = BMP_WIN_GetBmp(app->titleWindow);
    BMP_Clear(title, TITLE_BACKGROUND);
    Screen_Line(app, gWtText[WT_TEXT_TITLE], 64);
    PRINTSYS_PrintColor(title,
        (u32)((WT_GEN5_TITLE_WIDTH * 8 - (int)PRINTSYS_MeasureWidestLine(app->line, app->font, 0)) / 2), 0, app->line,
        app->font, TITLE_COLOR);
    Screen_Transfer(app->titleWindow, BG_TEXT);
    BMP_Clear(BMP_WIN_GetBmp(app->messageWindow), TEXT_BACKGROUND);
    y = Screen_Text(app, gWtText[app->message], 0, 0);
    if (app->state == WT_APP_RECEIVED && app->network.hasReceived) {
        Screen_Print(app, gWtText[WT_TEXT_RECEIVED], 32, 0, y);
        Screen_Print(app, app->network.received.nickname, WT_GAME_NAME_LENGTH, 100, y);
        y += WT_GEN5_LINE_HEIGHT;
        Screen_Print(app, gWtText[WT_TEXT_ORIGINAL_TRAINER], 32, 0, y);
        Screen_Print(app, app->network.received.originalTrainer, WT_GAME_TRAINER_NAME_LENGTH, 100, y);
    }
    if (app->state == WT_APP_ERROR) {
        Screen_Print(app, gWtText[WT_TEXT_ERROR_CODE], 32, 0, y);
        Number(digits, (unsigned)(app->network.error < 0 ? -app->network.error : app->network.error));
        if (app->network.error < 0) { memmove(digits + 1, digits, sizeof(digits) - sizeof(u16)); digits[0] = '-'; }
        Screen_Print(app, digits, 11, 140, y);
    }
    Screen_Transfer(app->messageWindow, BG_TEXT);
    Buttons(app);
}

static void Show(WtApp *app, int state, int message)
{
    app->state = state;
    app->message = message;
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

/* Leaves the screen: before connecting at once, after once the connection is closed. */
static void Close(WtApp *app)
{
    if (!app->started) {
        app->state = WT_APP_DONE;
        return;
    }
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
    if (app->state == WT_APP_CONFIRM) {
        app->selectedSlot = -1;
        Show(app, WT_APP_MENU, WT_TEXT_CHOOSE);
    } else if (app->state == WT_APP_COLLECT_CONFIRM || app->state == WT_APP_CANCEL_CONFIRM) {
        Show(app, WT_APP_MENU, WT_TEXT_QUEUED);
    } else if ((app->state == WT_APP_MESSAGE || app->state == WT_APP_RECEIVED) && !app->network.needsEntropy
               && !app->boot && app->started) {
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

/* The game's own party screen picks the Pokémon (overlay 91, ov091/pokelist.c),
 * set up as the field's "choose a Pokémon" sets it up
 * (ov010/event_poke_status.c, EVENT_POKE_STATUS_CreatePokeSelectEvent): the
 * party, in mode 3, which takes the Pokémon chosen at once, with no menu, and
 * leaves its slot, or 6 or more for Back. It runs from a process system of its
 * own, as the mail box pushes it (ov136/mb_seq.c, MB_SEQ_PushPartyList), which
 * loads overlay 91; that shares no memory with the Wi-Fi overlays 9 and 13. It
 * makes its own heap in heap 1, fades itself in and out, and puts the main
 * engine on the lower screen. The screen fades out and is freed first, as for
 * the trade animation. */
static const WtProcData sPartyListProc = { POKELIST_Init, POKELIST_Main, POKELIST_Exit };

static void Picker_Start(WtApp *app)
{
    app->selectedSlot = -1;
    app->state = WT_APP_PICKER_FADE;
    app->fade = 0;
    Buttons(app);
}

static void Picker_Update(WtApp *app)
{
    int slot;
    if (app->state == WT_APP_PICKER_FADE) {
        if (++app->fade < WT_FADE_FRAMES) {
            Brightness(-app->fade);
            return;
        }
        Screen_Free(app);
        if (GFL_HEAP_GetAllocatableSize(WT_HEAP_APP) >= WT_PARTY_LIST_HEAP_SIZE + 0x1000) {
            memset(&app->partyList, 0, sizeof(app->partyList));
            app->partyList.party = Party(app);
            app->partyList.mode = WT_PARTY_LIST_SELECT;
            app->procs = PROCSYS_Create(WT_HEAPID);
            PROCSYS_Push(app->procs, WT_PARTY_LIST_OVERLAY, &sPartyListProc, &app->partyList);
            app->state = WT_APP_PICKER;
            return;
        }
        slot = -1;
    } else if (PROCSYS_Main(app->procs) != 0) {
        return;
    } else {
        PROCSYS_Delete(app->procs);
        app->procs = NULL;
        ResetBanks();
        slot = app->partyList.selectedIndex;
    }
    /* The screen puts the main engine back on the upper screen. */
    Screen_Init(app);
    app->fade = WT_FADE_FRAMES;
    if (slot >= 0 && slot < PartyCount(app)) Pick(app, slot);
    else Show(app, WT_APP_MENU, WT_TEXT_CHOOSE);
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
            if (app->network.tradePhase == WT_JOURNAL_IDLE) Picker_Start(app);
            else Show(app, WT_APP_COLLECT_CONFIRM, WT_TEXT_CONFIRM_COLLECT);
        } else if (choice == 1) {
            if (app->network.tradePhase == WT_JOURNAL_IDLE) Show(app, WT_APP_MESSAGE, WT_TEXT_EXPLANATION);
            else Show(app, WT_APP_CANCEL_CONFIRM, WT_TEXT_CONFIRM_CANCEL);
        } else Close(app);
        break;
    }
}

/* The buttons read the +Control Pad, A and the touch screen themselves, and a
 * picked button flashes before it acts. B and X go back from every screen but
 * the recovery screen, as before, whether or not a button leaves. */
static void Input(WtApp *app)
{
    u32 keys = KEY_GetTrg();
    if (app->buttons && APP_TASKMENU_IsDecide(app->buttons)) {
        APP_TASKMENU_UpdateMenu(app->buttons);
        if (APP_TASKMENU_IsFinish(app->buttons)) Activate(app, APP_TASKMENU_GetCursorPos(app->buttons));
        return;
    }
    if (keys & (PAD_BUTTON_B | PAD_BUTTON_X)) {
        /* The buttons would take B as picking the last one, the recovery screen's only button. */
        if (app->state == WT_APP_RECOVERY) return;
        if (app->buttons) GFL_USE_PlaySeDefaultVolume(WT_SE_CANCEL);
        Back(app);
        return;
    }
    if (app->buttons) APP_TASKMENU_UpdateMenu(app->buttons);
}

/* The Global Trade Station's trade animation (ov119's POKETRADE_PROC_StartRequest8,
 * the template ov135/worldtrade_demo.c pushes for a trade), from a process
 * system of its own. It only shows the two Pokémon, the one sent and the one
 * received as saved; the Global Trade Station evolves and saves after it
 * itself. It loads overlay 119, which loads its own parts; overlays 67 and 114
 * are the ones the Global Trade Station keeps loaded for it. None of these
 * share memory with the Wi-Fi overlays 9 and 13. FALSE leaves the result to
 * show alone. */
static const WtProcData sTradeProc = { POKETRADE_PROC_StartRequest8, POKETRADE_PROC_Main, POKETRADE_PROC_Exit };

static BOOL Trade_Begin(WtApp *app)
{
    WtReceivedPokemon *received = &app->network.received;
    if (!app->gameData || !app->network.hasReceived
        || GFL_HEAP_GetAllocatableSize(WT_HEAP_APP) < WT_TRADE_HEAP_SIZE + 0x1000) return FALSE;
    memcpy(&app->pkm[0], received->sent, sizeof(WtPartyPokemon));
    /* A Pokémon received into the PC was saved as a box Pokémon: give it party
     * data as the receive does (game_save.c ReceiveEffects). */
    memset(&app->pkm[1], 0, sizeof(WtPartyPokemon));
    memcpy(&app->pkm[1].box, received->image, sizeof(WtBoxPokemon));
    POKE_TOOL_RecalcParam(&app->pkm[1]);
    POKE_TOOL_HealFull(&app->pkm[1]);
    if (POKE_TOOL_GetPartyValue(&app->pkm[0], WT_PARAM_SPECIES, 0) == 0
        || POKE_TOOL_GetPartyValue(&app->pkm[1], WT_PARAM_SPECIES, 0) == 0) return FALSE;
    app->partner = MYSTATUS_Create(WT_HEAPID);
    MYSTATUS_CopyName((u16 *)MYSTATUS_GetName(app->partner), received->originalTrainer);
    MYSTATUS_SetSex(app->partner, received->originalTrainerGender);
    memset(&app->request, 0, sizeof(app->request));
    app->request.gameData = app->gameData;
    app->request.sent = &app->pkm[0];
    app->request.received = &app->pkm[1];
    app->request.player = GAME_DATA_GetMyStatus(app->gameData);
    app->request.partner = app->partner;
    GF_OVERLAY_Load(WT_TRADE_OVERLAY_A);
    GF_OVERLAY_Load(WT_TRADE_OVERLAY_B);
    app->procs = PROCSYS_Create(WT_HEAPID);
    PROCSYS_Push(app->procs, WT_TRADE_OVERLAY, &sTradeProc, &app->request);
    app->state = WT_APP_TRADE;
    return TRUE;
}

static void Trade_End(WtApp *app)
{
    PROCSYS_Delete(app->procs);
    app->procs = NULL;
    GF_OVERLAY_Unload(WT_TRADE_OVERLAY_B);
    GF_OVERLAY_Unload(WT_TRADE_OVERLAY_A);
    GFL_HEAP_Free(app->partner);
    app->partner = NULL;
    ResetBanks();
}

/* The received Pokémon: fade out, play the animation, then fade the result
 * in. The screen puts the main engine back on the upper screen, which the
 * animation swaps (BG_SYS_SetLcdSwap). */
static void Trade_Update(WtApp *app)
{
    if (app->state == WT_APP_TRADE_FADE) {
        if (++app->fade < WT_FADE_FRAMES) {
            Brightness(-app->fade);
            return;
        }
        Screen_Free(app);
        if (Trade_Begin(app)) return;
    } else if (PROCSYS_Main(app->procs) != 0) {
        return;
    } else {
        Trade_End(app);
    }
    mbedtls_platform_zeroize(app->pkm, sizeof(app->pkm));
    mbedtls_platform_zeroize(app->network.received.sent, sizeof(app->network.received.sent));
    mbedtls_platform_zeroize(app->network.received.image, sizeof(app->network.received.image));
    Screen_Init(app);
    Show(app, WT_APP_RECEIVED, WT_TEXT_COMPLETED);
    app->fade = WT_FADE_FRAMES;
}

static void Responded(WtApp *app)
{
    WonderTradeNetwork *net = &app->network;
    app->selectedSlot = -1;
    if (net->needsEntropy) Show(app, WT_APP_MESSAGE, WT_TEXT_KEEP_PLAYING);
    else if (app->boot && net->canLeave && WtRecovery_CanWrite()) Close(app);
    else if (net->completed && net->returned) Show(app, WT_APP_MESSAGE, RefusalText(net->refusal, WT_TEXT_RETURNED));
    else if (net->completed) {
        app->state = WT_APP_TRADE_FADE;
        app->fade = 0;
        /* The response can let the screen leave, which drew a Cancel button; the fade has none. */
        Buttons(app);
    }
    else if (net->rejected) Show(app, WT_APP_MESSAGE, RefusalText(net->refusal, WT_TEXT_REJECTED));
    else if (net->superseded && net->tradePhase == WT_JOURNAL_IDLE) Show(app, WT_APP_MESSAGE, WT_TEXT_SUPERSEDED);
    else if (net->refused) Show(app, WT_APP_MESSAGE, WT_TEXT_CANNOT_HOLD);
    else if (net->waiting) Show(app, WT_APP_MESSAGE, WT_TEXT_WAITING);
    else if (net->tradePhase == WT_JOURNAL_RECEIVED || net->tradePhase == WT_JOURNAL_RESTORING)
        Show(app, WT_APP_MENU, WT_TEXT_NO_ROOM);
    else if (app->boot && !net->canLeave) Show(app, WT_APP_RECOVERY, WT_TEXT_RECOVERY_REQUIRED);
    else Show(app, WT_APP_MENU, net->tradePhase == WT_JOURNAL_IDLE ? WT_TEXT_CHOOSE : WT_TEXT_QUEUED);
}

static WtApp *Create(BOOL boot, u32 heapSize)
{
    WtApp *app;
    if (GFL_HEAP_GetAllocatableSize(WT_HEAP_APP) < heapSize + WT_SCREEN_HEAP_SIZE + 0x2000
        || !GFL_HEAP_CreateChildHeapImpl(WT_HEAP_APP, WT_HEAPID, heapSize)) return NULL;
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
    app->fade = WT_FADE_FRAMES;
    return app;
}

/* "Checking saved game...", while a slow boot check runs; free it before the check's result is shown. */
WtApp *WtApp_CreateChecking(void)
{
    WtApp *app = Create(TRUE, WT_CHECKING_HEAP_SIZE);
    if (app) {
        Show(app, WT_APP_CHECKING, WT_TEXT_CHECKING_SAVE);
        app->fade = 0;
        Brightness(0);
    }
    return app;
}

WtApp *WtApp_Create(BOOL boot)
{
    WtApp *app = Create(boot, WT_HEAP_SIZE);
    if (!app) return NULL;
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
    if (app->font) PRINTSYS_QUE_Main(app->queue);
    if (app->state != WT_APP_TRADE_FADE && app->state != WT_APP_TRADE && app->state != WT_APP_PICKER_FADE
        && app->state != WT_APP_PICKER && app->fade > 0) {
        --app->fade;
        Brightness(-app->fade);
    }
    if (!app->started) {
        Input(app);
        return app->state == WT_APP_DONE;
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
    if (app->state == WT_APP_TRADE_FADE || app->state == WT_APP_TRADE) {
        Trade_Update(app);
        return FALSE;
    }
    if (app->state == WT_APP_PICKER_FADE || app->state == WT_APP_PICKER) {
        Picker_Update(app);
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
    (void)proc; (void)state; (void)work;
    sApp = WtApp_Create(FALSE);
    if (sApp) sApp->gameData = param;
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
        GAMESYSTEM_CallProc(wk->gsys, WT_OVERLAY_NONE, &sWtProcData, GAMESYSTEM_GetGameData(wk->gsys));
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
