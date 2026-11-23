#include <nitro.h>
#include <string.h>
#include <mbedtls/platform_util.h>

#include "communication/pokemon_trade.h"
#include "constants/arc.h"
#include "constants/pokemon.h"
#include "constants/sound.h"
#include "field/field.h"
#include "field/field_event.h"
#include "gfl/arc_util.h"
#include "gfl/bg_sys.h"
#include "gfl/bmp.h"
#include "gfl/bmpwin.h"
#include "gfl/gx_layers.h"
#include "gfl/heapsys.h"
#include "gfl/key.h"
#include "gfl/net.h"
#include "gfl/overlay.h"
#include "gfl/proc.h"
#include "gfl/sound.h"
#include "gfl/str.h"
#include "gfl/tcb.h"
#include "nitro/gx.h"
#include "pml/poke_party.h"
#include "save/player_info.h"
#include "system/app_taskmenu.h"
#include "system/bmp_winframe.h"
#include "system/game_comm.h"
#include "system/game_data.h"
#include "system/game_event.h"
#include "system/game_system.h"
#include "system/gf_font.h"
#include "system/printsys.h"
#include "wonder_trade/bw2/application.h"
#include "wonder_trade/bw2/game_save.h"
#include "wonder_trade/gen5_screen.h"
#include "wonder_trade/network.h"
#include "wonder_trade/recovery.h"
#include "text.h"

/* Wonder Trade's screen for Black 2 and White 2. It runs as a process with
 * the field closed, like the Global Trade Station, from its own child heap of
 * heap 1, and at boot when the save needs recovery. It is laid out as the
 * station's title menu (worldtrade_title.c; wonder_trade/gen5_screen.h): on
 * the upper screen the station's title bar with the title, over its grid, and
 * a message window in the game's frame holding the status; on the lower
 * screen the actions, or the party while picking, as the game's task menu
 * buttons (app_taskmenu.c), with its cursor and sounds, picked with the
 * +Control Pad and A or by touch, B to go back. A received Pokémon is shown
 * with the game's own trade animation first. */

typedef char WtWideCharCheck[sizeof(L"a"[0]) == 2 ? 1 : -1];

#define WT_HEAPID 0xBA
#define WT_HEAP_SIZE 0x70000
/* The screen's own heap, freed while the trade animation runs. Heap IDs end
 * at 0xC0 (GFL_MemInit in gfl_use.c). */
#define WT_SCREEN_HEAPID 0xBB
#define WT_SCREEN_HEAP_SIZE 0x20000
/* The boot check's message needs only the screen. */
#define WT_CHECKING_HEAP_SIZE 0x10000
/* The trade animation's work heap, which it makes in heap 1 (pokemontrade_proc.c). */
#define WT_TRADE_HEAP_SIZE 0x96000
#define WT_FADE_FRAMES 16
#define MAX_ACTIONS 3
/* The BGs: on the upper screen the windows, the station's title bar and its
 * grid; on the lower screen the buttons and the grid. */
#define BG_TEXT 0
#define BG_BAR 1
#define BG_GRID 2
#define BG_BUTTONS 4
#define BG_BUTTONS_GRID 5
/* The station's graphics in its archive, with its palettes 0 to 2, as its
 * title menu loads them (Title_BgGraphicSet): the title is in palette 1. */
#define GTS_PALETTE_FILE 3
#define GTS_CHAR_FILE 13
#define GTS_GRID_SCREEN 31
#define GTS_BAR_SCREEN 32
#define GTS_PALETTES_SIZE 0x60
#define GTS_SCREEN_SIZE 0x600
#define TITLE_PALETTE 1
#define TITLE_COLOR PRINT_COLOR(13, 14, 10)
#define TITLE_BACKGROUND 10
/* The message window, in the font's palette, and the game's window frame. */
#define FONT_PALETTE 13
#define FRAME_PALETTE 14
#define FRAME_CHAR 1
#define TEXT_COLOR PRINT_COLOR(1, 2, 15)
#define TEXT_BACKGROUND 15
/* The buttons' two palettes and text color, as the station's own choices (WorldTrade_SelBoxInit). */
#define BUTTON_PALETTE 10
#define BUTTON_COLOR PRINT_COLOR(14, 15, 3)

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
    Font *font;
    StrBuf *line;
    PrintQueue *queue;
    TCB *vblank;
    BmpWin *titleWindow, *messageWindow;
    AppTaskMenuRes *buttonRes;
    AppTaskMenu *buttons;
    int state, message, selectedSlot, fade;
    BOOL boot, started;
    /* For the trade animation; gameData is NULL at boot, which shows none. */
    GameData *gameData;
    GameProcManager *procs;
    PlayerInfo *partner;
    PartyPkm pkm[2];
    PokemonTradeDemoParam demo;
};

typedef char WtTradeImageCheck[sizeof(PartyPkm) <= WT_GAME_IMAGE_SIZE ? 1 : -1];
typedef char WtButtonsCheck[WT_GEN5_MAX_BUTTONS <= APP_TASKMENU_ITEM_MAX && PARTY_SIZE + 1 <= WT_GEN5_MAX_BUTTONS
                            && MAX_ACTIONS <= WT_GEN5_MAX_BUTTONS ? 1 : -1];

static void Screen_VBlank(TCB *task, void *work)
{
    (void)task; (void)work;
    GFL_BGSysUpdate();
}

static void Screen_CreateBG(u8 bg, u8 screenBase, u8 charBase, u8 priority)
{
    BGSetup setup = { 0, 0, 0x800, 0, BGRES_256x256, GX_BG_COLORMODE_16, 0, 0, 0x8000, GX_BG_EXTPLTT_01, 0,
                      GX_BG_AREAOVER_XLU, FALSE };
    setup.screenBase = screenBase;
    setup.charBase = charBase;
    setup.priority = priority;
    GFL_BGSysCreateBG(bg, &setup, BGMODE_TEXT);
    GFL_BGSysClearScr(bg);
}

/* Opens the screen, dark, in a heap of its own; Create has checked heap 1 has room. */
static void Screen_Init(WtApp *app)
{
    static const BGSysVRAMConfig vram = {
        GX_VRAM_BG_128_A,  GX_VRAM_BGEXTPLTT_NONE,  GX_VRAM_SUB_BG_128_C,        GX_VRAM_SUB_BGEXTPLTT_NONE,
        GX_VRAM_OBJ_128_B, GX_VRAM_OBJEXTPLTT_NONE, GX_VRAM_SUB_OBJ_128_D,       GX_VRAM_SUB_OBJEXTPLTT_NONE,
        GX_VRAM_TEX_NONE,  GX_VRAM_TEXPLTT_NONE,    GX_OBJVRAMMODE_CHAR_1D_128K, GX_OBJVRAMMODE_CHAR_1D_128K,
    };
    static const BGSysLCDConfig lcd = { GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BGMODE_0, GX_BG0_AS_2D };
    ArcTool *arc;
    GX_SetMasterBrightness(-16);
    GXS_SetMasterBrightness(-16);
    GFL_HeapCreateChild(HEAPID_USER, WT_SCREEN_HEAPID, WT_SCREEN_HEAP_SIZE);
    GFL_BGSysSetEnabledBGsA(0);
    GFL_BGSysSetEnabledBGsB(0);
    G2_BlendNone();
    G2S_BlendNone();
    GX_SetDispSelect(GX_DISP_SELECT_MAIN_SUB);
    GFL_BGSysInitVRAM(GX_VRAM_NONE);
    GFL_BGSysCreate(WT_SCREEN_HEAPID);
    GFL_BGSysSetVRAMBanks(&vram);
    GFL_BGSysSetLCDConfig(&lcd);
    GX_DispOn();
    GXS_DispOn();
    Screen_CreateBG(BG_TEXT, GX_BG_SCRBASE(0xf800), GX_BG_CHARBASE(0x00000), 0);
    Screen_CreateBG(BG_BAR, GX_BG_SCRBASE(0xf000), GX_BG_CHARBASE(0x08000), 1);
    Screen_CreateBG(BG_GRID, GX_BG_SCRBASE(0xe800), GX_BG_CHARBASE(0x08000), 2);
    Screen_CreateBG(BG_BUTTONS, GX_BG_SCRBASE(0xf800), GX_BG_CHARBASE(0x00000), 0);
    Screen_CreateBG(BG_BUTTONS_GRID, GX_BG_SCRBASE(0xf000), GX_BG_CHARBASE(0x08000), 1);
    GFL_BGSysClearCharCore(BG_TEXT, 32, 0, WT_SCREEN_HEAPID);
    GFL_BGSysClearCharCore(BG_BUTTONS, 32, 0, WT_SCREEN_HEAPID);
    /* The station's grid on both screens, and its title menu's first rows, the title bar, on the upper one. */
    arc = GFL_ArcSysCreateFileHandle(ARCID_WORLDTRADE, WT_SCREEN_HEAPID);
    GFL_G2DIOLoadArcNCLRDefault(arc, GTS_PALETTE_FILE, PALTYPE_MAIN_BG, 0, GTS_PALETTES_SIZE, WT_SCREEN_HEAPID);
    GFL_G2DIOLoadArcNCLRDefault(arc, GTS_PALETTE_FILE, PALTYPE_SUB_BG, 0, GTS_PALETTES_SIZE, WT_SCREEN_HEAPID);
    GFL_BGSysLoadArcNCGRStatic(arc, GTS_CHAR_FILE, BG_BAR, 0, 0, TRUE, WT_SCREEN_HEAPID);
    GFL_BGSysLoadArcNCGRStatic(arc, GTS_CHAR_FILE, BG_BUTTONS_GRID, 0, 0, TRUE, WT_SCREEN_HEAPID);
    loadBGScrToVramByFileNoReserveNegAlign(arc, GTS_BAR_SCREEN, BG_BAR, 0, WT_GEN5_BAR_ROWS * 32 * sizeof(u16), TRUE,
                                           WT_SCREEN_HEAPID);
    loadBGScrToVramByFileNoReserveNegAlign(arc, GTS_GRID_SCREEN, BG_GRID, 0, GTS_SCREEN_SIZE, TRUE, WT_SCREEN_HEAPID);
    loadBGScrToVramByFileNoReserveNegAlign(arc, GTS_GRID_SCREEN, BG_BUTTONS_GRID, 0, GTS_SCREEN_SIZE, TRUE,
                                           WT_SCREEN_HEAPID);
    GFL_ArcToolFree(arc);
    GFL_BGSysLoadNCLRDefault(ARCID_FONT, 5, PALTYPE_MAIN_BG, FONT_PALETTE * 0x20, 0x20, WT_SCREEN_HEAPID);
    LoadSysMsgBox(BG_TEXT, FRAME_CHAR, FRAME_PALETTE, 0, WT_SCREEN_HEAPID);
    app->font = GFL_FontCreate(ARCID_FONT, 0, 0, FALSE, WT_SCREEN_HEAPID);
    app->line = GFL_StrBufCreate(64, WT_SCREEN_HEAPID);
    app->queue = PrintQueue_CreateDefault(WT_SCREEN_HEAPID);
    app->buttonRes = AppTaskMenuRes_Create(BG_BUTTONS, BUTTON_PALETTE, app->font, app->queue, WT_SCREEN_HEAPID);
    BmpWin_InitAllocator(WT_SCREEN_HEAPID);
    app->titleWindow = BmpWin_CreateDynamic(BG_TEXT, WT_GEN5_TITLE_X, WT_GEN5_TITLE_Y, WT_GEN5_TITLE_WIDTH,
                                            WT_GEN5_TITLE_HEIGHT, TITLE_PALETTE, TRUE);
    app->messageWindow = BmpWin_CreateDynamic(BG_TEXT, WT_GEN5_MESSAGE_X, WT_GEN5_MESSAGE_Y, WT_GEN5_MESSAGE_WIDTH,
                                              WT_GEN5_MESSAGE_HEIGHT, FONT_PALETTE, TRUE);
    GFL_BitmapFill(BmpWin_GetBitmap(app->messageWindow), TEXT_BACKGROUND);
    BmpWin_DrawFrame(app->messageWindow, WINFRAME_TRANSFER_NONE, FRAME_CHAR, FRAME_PALETTE);
    BmpWin_TransferNow(app->messageWindow);
    app->vblank = GFL_VBlankTCBAdd(Screen_VBlank, app, 0);
    GFL_BGSysSetBGEnabledA(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1 | GX_PLANEMASK_BG2, TRUE);
    GFL_BGSysSetBGEnabledB(GX_PLANEMASK_BG0 | GX_PLANEMASK_BG1, TRUE);
}

static void Screen_Free(WtApp *app)
{
    if (!app->font) return;
    GX_SetMasterBrightness(-16);
    GXS_SetMasterBrightness(-16);
    if (app->buttons) AppTaskMenu_Free(app->buttons);
    AppTaskMenuRes_Free(app->buttonRes);
    GFL_TCBRemove(app->vblank);
    BmpWin_Free(app->messageWindow);
    BmpWin_Free(app->titleWindow);
    BmpWin_FreeAllocator();
    PrintQueue_Clear(app->queue);
    PrintQueue_Free(app->queue);
    GFL_StrBufFree(app->line);
    GFL_FontFree(app->font);
    GFL_BGSysSetEnabledBGsA(0);
    GFL_BGSysSetEnabledBGsB(0);
    GFL_BGSysReleaseBG(BG_BUTTONS_GRID);
    GFL_BGSysReleaseBG(BG_BUTTONS);
    GFL_BGSysReleaseBG(BG_GRID);
    GFL_BGSysReleaseBG(BG_BAR);
    GFL_BGSysReleaseBG(BG_TEXT);
    GFL_BGSysFree();
    GFL_HeapDelete(WT_SCREEN_HEAPID);
    app->font = NULL; app->line = NULL; app->queue = NULL; app->vblank = NULL;
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
    GFL_StrBufLoadString(app->line, copy);
    return length;
}

/* Prints a line of text from chars into the message window. */
static int Screen_Print(WtApp *app, const u16 *chars, int count, int x, int y)
{
    int length = Screen_Line(app, chars, count);
    if (length) {
        GFL_TextRendererDrawToBitmapEx(BmpWin_GetBitmap(app->messageWindow), (s16)x, (s16)y, app->line, app->font,
                                       TEXT_COLOR);
    }
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

static PokeParty *Party(const WtApp *app)
{
    return SaveControl_GetPokePartySave(app->game.save);
}

static int PartyCount(const WtApp *app)
{
    int count = Party(app)->count;
    return count < PARTY_SIZE ? count : PARTY_SIZE;
}

/* A button's text: the name and level of the party Pokémon in slot, or, for a negative slot, a text. */
static StrBuf *Label(WtApp *app, int slot, int text)
{
    u16 chars[24], digits[12];
    const u16 *from;
    StrBuf *str = GFL_StrBufCreate(24, WT_SCREEN_HEAPID);
    int length = 0;
    if (slot < 0) {
        Screen_Line(app, gWtText[text], 23);
        GFL_StrBufCopy(str, app->line);
        return str;
    }
    memset(chars, 0xff, sizeof(chars));
    PokeParty_GetParam(&Party(app)->pkms[slot], PKM_PARAM_NICKNAME_RAW, chars);
    while (length < 11 && chars[length] != 0xffff) length++;
    chars[length++] = ' ';
    chars[length++] = ' ';
    for (from = gWtText[WT_TEXT_LEVEL]; *from && length < 18; ) chars[length++] = *from++;
    Number(digits, PokeParty_GetParam(&Party(app)->pkms[slot], PKM_PARAM_LEVEL, NULL));
    for (from = digits; *from && length < 23; ) chars[length++] = *from++;
    chars[length] = 0xffff;
    GFL_StrBufLoadString(str, chars);
    return str;
}

/* The lower screen's buttons: the party and Back while picking, otherwise the actions. */
static void Buttons(WtApp *app)
{
    AppTaskMenuItem items[WT_GEN5_MAX_BUTTONS];
    AppTaskMenuInit init;
    int labels[MAX_ACTIONS], count, i, party = 0;
    if (app->buttons) AppTaskMenu_Free(app->buttons);
    app->buttons = NULL;
    if (app->state == WT_APP_PICKER) {
        party = PartyCount(app);
        count = party + 1;
    } else {
        count = Actions(app, labels);
    }
    if (count <= 0) return;
    for (i = 0; i < count; ++i) {
        int text = app->state == WT_APP_PICKER ? WT_TEXT_BACK : labels[i];
        items[i].str = Label(app, i < party ? i : -1, text);
        items[i].color = BUTTON_COLOR;
        /* A button that leaves has the return arrow and the cancel sound, as B does. */
        items[i].type = i >= party && (text == WT_TEXT_BACK || text == WT_TEXT_EXIT || text == WT_TEXT_CANCEL)
                            ? APP_TASKMENU_ITEM_RETURN : 0;
    }
    MI_CpuFill8(&init, 0, sizeof(init));
    init.heapId = WT_SCREEN_HEAPID;
    init.itemCount = (u8)count;
    init.items = items;
    init.posType = APP_TASKMENU_POS_TOP_LEFT;
    init.x = WT_GEN5_BUTTON_X;
    init.y = (u8)WtGen5_ButtonTop(count);
    init.width = WT_GEN5_BUTTON_WIDTH;
    init.height = WT_GEN5_BUTTON_HEIGHT;
    app->buttons = AppTaskMenu_Create(&init, app->buttonRes);
    for (i = 0; i < count; ++i) GFL_StrBufFree(items[i].str);
}

static void Draw(WtApp *app)
{
    GFLBitmap *title;
    u16 digits[12];
    int y;
    if (!app->font) return;
    title = BmpWin_GetBitmap(app->titleWindow);
    GFL_BitmapFill(title, TITLE_BACKGROUND);
    Screen_Line(app, gWtText[WT_TEXT_TITLE], 64);
    GFL_TextRendererDrawToBitmapEx(title,
        (s16)((WT_GEN5_TITLE_WIDTH * 8 - (int)GFL_FontGetBlockWidth(app->line, app->font, 0)) / 2), 0, app->line,
        app->font, TITLE_COLOR);
    BmpWin_Transfer(app->titleWindow);
    GFL_BitmapFill(BmpWin_GetBitmap(app->messageWindow), TEXT_BACKGROUND);
    y = Screen_Text(app, gWtText[app->state == WT_APP_PICKER ? WT_TEXT_PICK : app->message], 0, 0);
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
    BmpWin_Transfer(app->messageWindow);
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
    if (app->state == WT_APP_CONFIRM || app->state == WT_APP_PICKER) {
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
        if (choice < PartyCount(app)) Pick(app, choice);
        else Back(app);
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

/* The buttons read the +Control Pad, A and the touch screen themselves, and a
 * picked button flashes before it acts. B and X go back from every screen but
 * the recovery screen, as before, whether or not a button leaves. */
static void Input(WtApp *app)
{
    u32 keys = GCTX_HIDGetPressedKeys();
    if (app->buttons && AppTaskMenu_IsDecided(app->buttons)) {
        AppTaskMenu_Update(app->buttons);
        if (AppTaskMenu_IsFlashFinished(app->buttons)) Activate(app, AppTaskMenu_GetCursorPos(app->buttons));
        return;
    }
    if (app->state != WT_APP_RECOVERY && (keys & (PAD_BUTTON_B | PAD_BUTTON_X))) {
        if (app->buttons) GFL_SndSEPlay(SEQ_SE_CANCEL1);
        Back(app);
        return;
    }
    if (app->buttons) AppTaskMenu_Update(app->buttons);
}

/* The Global Trade Station's trade animation (POKEMONTRADE_GTS_TRADE_PROC_FUNCTIONS),
 * which the GTS plays for an online trade with Wi-Fi up, from a process
 * manager of its own (worldtrade_demo.c). It only shows the two Pokémon, the
 * one sent and the one received as saved: its GTS trade type ends without
 * touching the party, the boxes or the save (PoketradeSave_StepAfterTrade).
 * Its overlays follow Wi-Fi's 11 and 29; 139 and 189 are the ones the GTS
 * keeps loaded for it (worldtrade.c). FALSE leaves the result to show alone. */
static BOOL Trade_Begin(WtApp *app)
{
    WtReceivedPokemon *received = &app->network.received;
    PokemonTradeDemoParam *demo = &app->demo;
    if (!app->gameData || !app->network.hasReceived
        || GFL_HeapGetHighestAllocatableSize(HEAPID_USER) < WT_TRADE_HEAP_SIZE + 0x1000) return FALSE;
    memcpy(&app->pkm[0], received->sent, sizeof(PartyPkm));
    /* A Pokémon received into the PC was saved as a box Pokémon: give it party
     * data as the receive does (game_save.c ReceiveEffects). */
    memset(&app->pkm[1], 0, sizeof(PartyPkm));
    memcpy(&app->pkm[1].base, received->image, sizeof(BoxPkm));
    PokeParty_RecalcStats(&app->pkm[1]);
    PokeParty_Recover(&app->pkm[1]);
    if (PokeParty_GetParam(&app->pkm[0], PKM_PARAM_SPECIES, NULL) == 0
        || PokeParty_GetParam(&app->pkm[1], PKM_PARAM_SPECIES, NULL) == 0) return FALSE;
    app->partner = PlayerInfo_Create(WT_HEAPID);
    PlayerInfo_Init(app->partner);
    copyTrainerName(app->partner, received->originalTrainer);
    setTrainerGender(app->partner, received->originalTrainerGender);
    memset(demo, 0, sizeof(*demo));
    demo->gameData = app->gameData;
    demo->pkm[0] = &app->pkm[0];
    demo->pkm[1] = &app->pkm[1];
    demo->myInfo = GetGameDataPlayerInfo(app->gameData);
    demo->partnerInfo = app->partner;
    GFL_OvlLoad(OVERLAY_ID(139));
    GFL_OvlLoad(OVERLAY_ID(189));
    app->procs = CreateGameProcManager(WT_HEAPID);
    QueueGameProc(app->procs, OVERLAY_POKEMONTRADE, &POKEMONTRADE_GTS_TRADE_PROC_FUNCTIONS, demo);
    app->state = WT_APP_TRADE;
    return TRUE;
}

static void Trade_End(WtApp *app)
{
    FreeGameProcManager(app->procs);
    app->procs = NULL;
    GFL_OvlUnload(OVERLAY_ID(189));
    GFL_OvlUnload(OVERLAY_ID(139));
    GFL_HeapFree(app->partner);
    app->partner = NULL;
    /* The animation leaves its banks and engine layout; the screen takes the banks it needs. */
    GX_ResetBankForOBJ(); GX_ResetBankForBGExtPltt(); GX_ResetBankForOBJExtPltt(); GX_ResetBankForTex();
    GX_ResetBankForTexPltt(); GX_ResetBankForClearImage(); GX_ResetBankForSubOBJ(); GX_ResetBankForSubBGExtPltt();
    GX_ResetBankForSubOBJExtPltt();
}

/* The received Pokémon: fade out, play the animation, then fade the result in. */
static void Trade_Update(WtApp *app)
{
    if (app->state == WT_APP_TRADE_FADE) {
        if (++app->fade < WT_FADE_FRAMES) {
            GX_SetMasterBrightness(-app->fade);
            GXS_SetMasterBrightness(-app->fade);
            return;
        }
        Screen_Free(app);
        if (Trade_Begin(app)) return;
    } else if (GFL_ProcMgrUpdate(app->procs) != PROCMGR_DONE) {
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
    if (GFL_HeapGetHighestAllocatableSize(HEAPID_USER) < heapSize + 0x1000) return NULL;
    GFL_HeapCreateChild(HEAPID_USER, WT_HEAPID, heapSize);
    app = GFL_HeapGetHighestAllocatableSize(HEAPID_USER) < WT_SCREEN_HEAP_SIZE + 0x1000
              ? NULL : GFL_HeapAllocateCore(WT_HEAPID, sizeof(*app));
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
        GX_SetMasterBrightness(0);
        GXS_SetMasterBrightness(0);
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
    if (app->font) PrintQueue_Main(app->queue);
    if (app->state != WT_APP_TRADE_FADE && app->state != WT_APP_TRADE && app->fade > 0) {
        --app->fade;
        GX_SetMasterBrightness(-app->fade);
        GXS_SetMasterBrightness(-app->fade);
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
    (void)proc; (void)state; (void)work;
    sApp = WtApp_Create(FALSE);
    if (sApp) sApp->gameData = param;
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
        GSYS_QueueProc(wk->gsys, OVERLAY_NONE, &sWtProcFunctions, GSYS_GetGameData(wk->gsys));
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
