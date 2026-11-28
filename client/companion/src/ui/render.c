#include "ui/render.h"

#include <stdio.h>
#include <string.h>

#include "game/rom.h"
/* Only its geometry: the touch handling lives in ui.c. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "wonder_trade/menu_input.h"
#pragma GCC diagnostic pop

/* GX_RGB: five bits each, red lowest. */
#define C(r, g, b) ((uint16_t)((r) | (g) << 5 | (b) << 10))

/* application.c 205-217, copied as values, and the companion's own text,
 * grey, icon and spinner colours. */
const uint16_t CpRender_Palettes[16][16] = {
    [CP_PAL_TEXT] = { 0, C(7, 7, 8), C(22, 22, 23), C(31, 31, 31), C(9, 18, 27), C(17, 17, 18),
        C(26, 26, 27), C(31, 18, 2), C(27, 4, 7), C(3, 11, 26), C(28, 30, 31), C(14, 8, 3), C(30, 21, 9), C(3, 7, 13), C(31, 28, 6), C(31, 31, 31) },
    [CP_PAL_BUTTON] = { 0, C(25, 30, 31), 0, 0, 0, 0, 0, 0, 0,
        C(19, 27, 30), C(22, 30, 30), C(9, 18, 27), 0, 0, 0, 0 },
    [CP_PAL_SELECTED] = { 0, C(31, 31, 25), 0, 0, 0, 0, 0, 0, 0,
        C(31, 24, 12), C(31, 28, 18), C(30, 11, 5), 0, 0, 0, 0 },
    [CP_PAL_DISABLED] = { 0, C(24, 25, 26), 0, 0, 0, 0, 0, 0, 0,
        C(20, 21, 22), C(27, 27, 28), C(13, 14, 16), 0, 0, 0, 0 },
    [CP_PAL_BACKDROP] = { 0, C(31, 31, 31), C(9, 18, 27), C(13, 22, 31), C(14, 26, 29),
        C(19, 27, 30), 0, C(22, 30, 30), C(27, 31, 31), C(21, 26, 31), 0, 0, 0, 0, 0, 0 },
    [CP_PAL_ICON] = { 0, C(31, 31, 31), C(4, 4, 6), C(6, 14, 31), C(16, 22, 31), C(31, 6, 8),
        C(31, 18, 20), C(31, 28, 6), C(31, 18, 2), C(20, 12, 6), C(30, 24, 14), C(16, 16, 18), 0, 0, 0, 0 },
    [CP_PAL_SPINNER] = { 0, C(9, 18, 27), C(11, 19, 27), C(13, 20, 28), C(15, 21, 28), C(17, 22, 29),
        C(19, 23, 29), C(21, 24, 30), C(23, 25, 30), 0, 0, 0, 0, 0, 0, 0 },
};

#define ENTRY(tile, palette) ((uint16_t)((tile) | (palette) << 12))

/* Windows of the top screen in tiles (application.c 278-288). */
#define TITLE_X 2
#define TITLE_Y 1
#define MESSAGE_X 2
#define MESSAGE_Y 17
#define MESSAGE_Y_ALONE 9
#define MESSAGE_W 27
#define MESSAGE_H 6
#define DETAILS_X 2
#define DETAILS_Y 4
#define BODY_X 2
#define BODY_Y 5
#define BODY_W 28
#define BODY_H 16
#define PREVIEW_X 21
#define PREVIEW_Y 4
#define PREVIEW_W 10
#define PREVIEW_H 10
/* The save list replaces the details card and preview above the message. */
#define LIST_X 2
#define LIST_Y 4
#define LIST_W 28
#define LIST_H 11

static const CpInk kDark = { CP_INK_DARK, CP_INK_SHADOW };
static const CpInk kGrey = { CP_INK_GREY, CP_INK_GREY_SHADOW };
static const CpInk kLight = { CP_INK_WHITE, CP_INK_BLUE };

static CompanionLanguage sLanguage;

static const char *T(int id)
{
    if (id < 0 || id >= Companion_Text_Count) return "";
    return Companion_Text[id][sLanguage];
}

static void Put(CpScreen *screen, int x, int y, uint16_t entry)
{
    if (x >= 0 && y >= 0 && x < 32 && y < 32) screen->map[y * 32 + x] = entry;
}

/* A box of nine-slice tiles covering w x h cells from (x, y). */
static void Box(CpScreen *screen, unsigned base, int x, int y, int w, int h, unsigned palette)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int column = i == 0 ? 0 : i == w - 1 ? 2 : 1;
            int row = j == 0 ? 0 : j == h - 1 ? 2 : 1;
            Put(screen, x + i, y + j, ENTRY(base + row * 3 + column, palette));
        }
}

/* A white text window with the frame around it, as Window_DrawStandardFrame. */
static void Window(CpScreen *screen, int x, int y, int w, int h)
{
    Box(screen, CP_TILE_FRAME, x - 1, y - 1, w + 2, h + 2, CP_PAL_BACKDROP);
    CpCanvas_Fill(&screen->text, x * 8, y * 8, w * 8, h * 8, CP_INK_FILL);
}

/* Each painted pixel sits on the text layer, leaving the panel visible
 * around the icon and through the handle of the bag. */
static void StatusMark(CpCanvas *canvas, int x, int y, int shiny)
{
    static const char bag[8][9] = {
        "..111...", ".1...1..", "1111111.", "1222221.",
        "1221221.", "1222221.", "1222221.", ".11111.."
    };
    static const char star[8][9] = {
        "...1....", "...2....", "..121...", "1222221.",
        ".12221..", "..212...", ".212.21.", ".1....1."
    };
    const char (*rows)[9] = shiny ? star : bag;
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            if (rows[row][col] == '1')
                CpCanvas_Pixel(canvas, x + col, y + row, shiny ? CP_INK_CURSOR : CP_INK_BAG_EDGE);
            else if (rows[row][col] == '2')
                CpCanvas_Pixel(canvas, x + col, y + row, shiny ? CP_INK_GOLD : CP_INK_BAG_FILL);
        }
    }
}

static void Header(CpScreen *screen)
{
    int x;
    CpCanvas_Fill(&screen->text, 0, 0, CP_CANVAS_WIDTH, 24, 0);
    for (x = 0; x < 32; x++) {
        Put(screen, x, 0, ENTRY(CP_TILE_HEADER_TOP, CP_PAL_BACKDROP));
        Put(screen, x, 1, ENTRY(CP_TILE_HEADER, CP_PAL_BACKDROP));
        Put(screen, x, 2, ENTRY(CP_TILE_HEADER_EDGE, CP_PAL_BACKDROP));
    }
    CpCanvas_Text(&screen->text, TITLE_X * 8, TITLE_Y * 8, "DSWT", (CpInk){CP_INK_TITLE, CP_INK_STRIPE});
}

static void Clear(CpScreen *screen)
{
    CpCanvas_Clear(&screen->text);
    for (unsigned i = 0; i < CP_CANVAS_TILES; ++i) screen->textMap[i] = (uint16_t)i;
    memset(screen->spritePalettes, 0, sizeof(screen->spritePalettes));
    memset(screen->map, 0, sizeof(screen->map));
}


static int GameName(unsigned version)
{
    switch (version) {
    case CP_VERSION_DIAMOND: return Companion_Text_GameDiamond;
    case CP_VERSION_PEARL: return Companion_Text_GamePearl;
    case CP_VERSION_PLATINUM: return Companion_Text_GamePlatinum;
    case CP_VERSION_HEARTGOLD: return Companion_Text_GameHeartGold;
    case CP_VERSION_SOULSILVER: return Companion_Text_GameSoulSilver;
    case CP_VERSION_BLACK: return Companion_Text_GameBlack;
    case CP_VERSION_WHITE: return Companion_Text_GameWhite;
    case CP_VERSION_BLACK2: return Companion_Text_GameBlack2;
    case CP_VERSION_WHITE2: return Companion_Text_GameWhite2;
    default: return -1;
    }
}

static int LanguageName(unsigned language)
{
    switch (language) {
    case CP_LANGUAGE_JAPANESE: return Companion_Text_LanguageJapanese;
    case CP_LANGUAGE_ENGLISH: return Companion_Text_LanguageEnglish;
    case CP_LANGUAGE_FRENCH: return Companion_Text_LanguageFrench;
    case CP_LANGUAGE_ITALIAN: return Companion_Text_LanguageItalian;
    case CP_LANGUAGE_GERMAN: return Companion_Text_LanguageGerman;
    case CP_LANGUAGE_SPANISH: return Companion_Text_LanguageSpanish;
    case CP_LANGUAGE_KOREAN: return Companion_Text_LanguageKorean;
    default: return -1;
    }
}

/* A save's name without its folder or .sav, for {name}. */
static void SaveStem(char *out, size_t size, const char *name)
{
    const char *slash = strrchr(name, '/');
    size_t length;
    if (slash) name = slash + 1;
    length = strlen(name);
    if (length > 4 && !strcmp(name + length - 4, ".sav")) length -= 4;
    if (length >= size) length = size - 1;
    memcpy(out, name, length);
    out[length] = 0;
}

void CpRender_Message(const CpUi *ui, const CpUiModel *model, char *out, size_t size)
{
    char game[512], named[512], stem[CP_UI_SAVE_NAME_MAX];
    /* The stored network on the list and Forget, the chosen one elsewhere. */
    const char *network = ui->state == CP_UI_WIFI_LIST || ui->state == CP_UI_WIFI_FORGET
        ? model->wifi.savedSsid : ui->wifiSsid;
    stem[0] = 0;
    if (ui->listCursor >= 0 && ui->listCursor < (int)model->saveCount)
        SaveStem(stem, sizeof(stem), model->saves[ui->listCursor].name);
    int gameName = GameName(model->gameVersion);
    CpText_Replace(game, sizeof(game), T(ui->message), "{game}", gameName >= 0 ? T(gameName) : "?");
    CpText_Replace(named, sizeof(named), game, "{name}", stem);
    /* Last, so a network's name is never read as a placeholder. */
    CpText_Replace(out, size, named, "{network}", network);
}

/* UTF-8 text cut to fit `width` pixels, ending in ".." when cut. */
static void Fit(char *out, size_t size, const char *text, int width)
{
    size_t length = strlen(text);
    if (length >= size) length = size - 1;
    memcpy(out, text, length);
    out[length] = 0;
    if (CpText_Width(out) <= width) return;
    while (length > 0) {
        do length--; while (length > 0 && ((unsigned char)out[length] & 0xC0) == 0x80);
        if (length + 3 > size) continue;
        out[length] = '.';
        out[length + 1] = '.';
        out[length + 2] = 0;
        if (CpText_Width(out) <= width) return;
    }
}

static void Number(char *out, size_t size, int value)
{
    snprintf(out, size, "%d", value);
}

/* Text wrapped to a window's width and kept inside it. */
static void WindowText(CpScreen *screen, int x, int y, int w, int h, int inset, const char *text)
{
    char wrapped[CP_RENDER_MESSAGE_MAX];
    CpText_Wrap(wrapped, sizeof(wrapped), text, w * 8 - 2 * inset);
    CpCanvas_Clip(&screen->text, x * 8, y * 8, w * 8, h * 8);
    CpCanvas_Text(&screen->text, x * 8 + inset, y * 8, wrapped, kDark);
    CpCanvas_Unclip(&screen->text);
}

static void DrawMessage(CpScreen *top, const char *text, int y)
{
    Window(top, MESSAGE_X, y, MESSAGE_W, MESSAGE_H);
    WindowText(top, MESSAGE_X, y, MESSAGE_W, MESSAGE_H, 0, text);
}

static void DrawBody(CpScreen *top, const char *text)
{
    Window(top, BODY_X, BODY_Y, BODY_W, BODY_H);
    WindowText(top, BODY_X, BODY_Y, BODY_W, BODY_H, 4, text);
}

int CpRender_MessageLines(const char *text, int *width)
{
    char wrapped[CP_RENDER_MESSAGE_MAX];
    int lines = CpText_Wrap(wrapped, sizeof(wrapped), text, MESSAGE_W * 8);
    if (width) *width = CpText_Width(wrapped);
    return lines;
}

/* A small right-pointing marker for the highlighted row of a list. */
static void Marker(CpScreen *screen, int x, int y)
{
    int i;
    for (i = 0; i < 4; i++) CpCanvas_Fill(&screen->text, x + i, y + i, 1, 9 - 2 * i, CP_INK_CURSOR);
}

static void Card(CpScreen *top)
{
    int x, y;
    Box(top, CP_TILE_CARD, 0, 3, 20, 12, CP_PAL_BACKDROP);
    for (x = 2; x < 18; x++) Put(top, x, 11, ENTRY(CP_TILE_CARD_LINE, CP_PAL_BACKDROP));
    Box(top, CP_TILE_CARD, PREVIEW_X - 1, PREVIEW_Y - 1, PREVIEW_W + 2, PREVIEW_H + 2, CP_PAL_BACKDROP);
    for (y = PREVIEW_Y; y < PREVIEW_Y + PREVIEW_H; y++)
        for (x = PREVIEW_X; x < PREVIEW_X + PREVIEW_W; x++) Put(top, x, y, ENTRY(CP_TILE_PREVIEW, CP_PAL_BACKDROP));
}

/* Draw only the foreground pixels so the card and button borders stay intact. */
static void GenderIcon(CpCanvas *canvas, int x, int y, unsigned gender)
{
    static const uint16_t shapes[][11] = {
        {0x01f,0x00f,0x00b,0x075,0x089,0x104,0x104,0x104,0x088,0x070,0},
        {0x078,0x0cc,0x084,0x084,0x0cc,0x078,0x030,0x0fc,0x0fc,0x030,0x030}
    };
    if (gender > CP_UI_FEMALE) return;
    for (int row = 0; row < 11; ++row)
        for (int col = 0; col < 9; ++col)
            if (shapes[gender][row] & (0x100 >> col))
                CpCanvas_Pixel(canvas, x + col, y + row, gender == CP_UI_FEMALE ? CP_INK_FEMALE : CP_INK_MALE);
}

unsigned CpRender_PortraitPalette(unsigned slot)
{
    static const unsigned banks[CP_UI_PARTY_MAX] = { 2, 4, 6, 7, 8, 11 };
    return banks[slot];
}

static unsigned PortraitPixel(const CpPortrait *picture, unsigned x, unsigned y)
{
    unsigned at = ((y / 8) * 10 + x / 8) * 32 + y % 8 * 4 + x % 8 / 2;
    return (picture->tiles[at] >> ((x & 1) * 4)) & 15;
}

/* Put the portrait in the text background's own tiles. Transparent pixels
 * reveal the existing party button or preview behind them. */
static void Portrait(CpScreen *screen, const CpPortrait *picture, unsigned slot, int tx, int ty, unsigned side)
{
    if (!picture->ready) return;
    screen->spritePalettes[slot] = picture->palette;
    unsigned bank = CpRender_PortraitPalette(slot);
    int left = 0, top = 0, span = CP_PORTRAIT_SIDE;
    if (side == 32) {
        int x0 = span, y0 = span, x1 = -1, y1 = -1;
        for (int y = 0; y < CP_PORTRAIT_SIDE; ++y)
            for (int x = 0; x < CP_PORTRAIT_SIDE; ++x)
                if (PortraitPixel(picture, x, y)) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        if (x1 >= x0) {
            span = 1 + (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0);
            left = (x1 + x0 + 1 - span) / 2;
            top = (y1 + y0 + 1 - span) / 2;
        }
    }
    for (unsigned y = 0; y < side; ++y) {
        for (unsigned x = 0; x < side; ++x) {
            unsigned cell = (ty + y / 8) * 32 + tx + x / 8;
            screen->textMap[cell] = ENTRY(cell, bank);
            int sx = left + (int)((2*x + 1) * span / (2*side));
            int sy = top + (int)((2*y + 1) * span / (2*side));
            unsigned ink = (unsigned)sx < CP_PORTRAIT_SIDE && (unsigned)sy < CP_PORTRAIT_SIDE
                ? PortraitPixel(picture, sx, sy) : 0;
            CpCanvas_Pixel(&screen->text, tx * 8 + x, ty * 8 + y, ink);
        }
    }
}

static int GameText(CpCanvas *canvas, int x, int y, const uint16_t *text, size_t capacity,
                    const CpUiModel *model, CpInk ink)
{
    if (!model->font && model->gameVersion >= CP_VERSION_WHITE && model->gameVersion <= CP_VERSION_BLACK2)
        return CpCanvas_UnicodeText(canvas, x, y, text, capacity, ink);
    return CpCanvas_GameText(canvas, x, y, text, capacity, model->font, ink);
}

static void SpeciesLine(CpScreen *screen, const CpUiModel *model, const uint16_t *name, unsigned gender)
{
    int x = DETAILS_X * 8, y = DETAILS_Y * 8 + 16;
    CpCanvas_Clip(&screen->text, x, y, 120, 16);
    int width = GameText(&screen->text, x, y, name, CP_UI_NAME_MAX, model, kLight);
    CpCanvas_Unclip(&screen->text);
    GenderIcon(&screen->text, x + (width < 120 ? width : 120) + 3, y + 2, gender);
}

static int SameName(const uint16_t *a, const uint16_t *b)
{
    for (unsigned i = 0; i < CP_UI_NAME_MAX; ++i) {
        if (a[i] != b[i]) return 0;
        if (a[i] == CP_CHAR_EOS) break;
    }
    return 1;
}

/* The details card (application.c 381-412). */
static void DrawPokemonCard(const CpUi *ui, const CpUiModel *model, CpScreen *top, enum CpUiCard card)
{
    CpCanvas *text = &top->text;
    int ox = DETAILS_X * 8, oy = DETAILS_Y * 8;
    char number[12];
    Card(top);
    if (card == CP_UI_CARD_PARTY) {
        int slot = ui->state == CP_UI_CONFIRM ? ui->selectedSlot : ui->partyCursor;
        const CpUiPokemon *mon = &model->party[slot];
        SpeciesLine(top, model, mon->speciesName, mon->isEgg ? CP_UI_GENDERLESS : mon->gender);
        Portrait(top, &mon->portrait, 0, PREVIEW_X, PREVIEW_Y, CP_PORTRAIT_SIDE);
        if (mon->shiny) StatusMark(text, 140, oy + 4, 1);
        if (ui->state == CP_UI_SUMMARY) CpCanvas_Text(text, ox, oy, T(Companion_Text_Summary), kLight);
        if (!SameName(mon->speciesName, mon->nickname))
            GameText(text, ox, oy + 32, mon->nickname, CP_UI_NAME_MAX, model, kLight);
        if (mon->heldItem && ui->state != CP_UI_SUMMARY) {
            StatusMark(text, ox, oy + 51, 0);
            GameText(text, ox + 12, oy + 48, mon->itemName, CP_UI_ITEM_NAME_MAX, model, kLight);
        }
        int labelWidth = CpCanvas_Text(text, ox, oy + 64, T(WonderTrade_Text_Level), kLight);
        Number(number, sizeof(number), mon->level);
        CpCanvas_Text(text, ox + labelWidth + 4, oy + 64, number, kLight);
    } else {
        const CpUiReceived *received = &model->received;
        if (ui->state == CP_UI_SUMMARY) {
            snprintf(number, sizeof(number), "%u", received->summary.level);
            int width = CpCanvas_Text(text, ox, oy, T(WonderTrade_Text_Level), kLight);
            CpCanvas_Text(text, ox + width + 4, oy, number, kLight);
        } else CpCanvas_Text(text, ox, oy, T(WonderTrade_Text_Received), kLight);
        Portrait(top, &received->portrait, 0, PREVIEW_X, PREVIEW_Y, CP_PORTRAIT_SIDE);
        SpeciesLine(top, model, received->speciesName, received->gender);
        if (received->shiny) StatusMark(text, 140, oy + 4, 1);
        if (received->hasNickname)
            GameText(text, ox, oy + 32, received->nickname, CP_UI_NAME_MAX, model, kLight);
        CpCanvas_Text(text, ox, oy + 64, T(WonderTrade_Text_OriginalTrainer), kLight);
        GameText(text, ox + 40, oy + 64, received->originalTrainer, CP_UI_TRAINER_MAX, model, kLight);
    }
}

static void DrawSaves(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    Window(top, LIST_X, LIST_Y, LIST_W, LIST_H);
    for (int row = 0; row < CP_SOURCE_ROWS; ++row) {
        int index = ui->listTop + row;
        if (index >= (int)model->saveCount) break;
        const CpUiSave *save = &model->saves[index];
        int game = GameName(save->version), lang = LanguageName(save->language);
        char detail[96], name[CP_UI_SAVE_NAME_MAX];
        if (save->status == CP_UI_SAVE_READY)
            snprintf(detail, sizeof(detail), "%s%s%s", game >= 0 ? T(game) : "",
                game >= 0 && lang >= 0 ? "  " : "", lang >= 0 ? T(lang) : "");
        else snprintf(detail, sizeof(detail), "%s",
            T(save->status == CP_UI_SAVE_NO_ROM ? Companion_Text_SaveNoRom : Companion_Text_SaveUnsupported));
        int right = (LIST_X + LIST_W) * 8 - 4;
        int labelX = right - CpText_Width(detail);
        int left = LIST_X * 8 + 8, y = LIST_Y * 8 + 4 + row * 20;
        Fit(name, sizeof(name), save->gameCard ? T(Companion_Text_GameCard) : save->name, labelX - left - 6);
        if (ui->state == CP_UI_GAME_LIST && index == ui->listCursor) {
            CpCanvas_Fill(&top->text, left - 8, y, LIST_W * 8, 17, CP_INK_STRIPE);
            Marker(top, left - 8, y + 3);
        }
        CpCanvas_Text(&top->text, left, y, name, kDark);
        CpCanvas_Text(&top->text, labelX, y, detail, kDark);
    }
}

/* ---- Wi-Fi settings ---------------------------------------------------------- */

static const char *SecurityName(unsigned security)
{
    switch (security) {
    case CP_WIFI_SECURITY_OPEN: return T(Companion_Text_WifiOpen);
    case CP_WIFI_SECURITY_WEP: return "WEP";
    case CP_WIFI_SECURITY_WPA2: return "WPA2";
    case CP_WIFI_SECURITY_WPA: return "WPA";
    default: return "802.1X";
    }
}

/* Three signal bars ending at `right`, standing on `bottom`: the lit ones in
 * the row's ink, the rest pale. Returns their width. */
#define BARS_WIDTH 11
static int Bars(CpScreen *screen, int right, int bottom, unsigned bars, CpInk ink)
{
    int i;
    for (i = 0; i < 3; i++) {
        int height = 4 + 3 * i;
        CpCanvas_Fill(&screen->text, right - BARS_WIDTH + i * 4, bottom - height, 3, height,
            (unsigned)i < bars ? ink.fg : CP_INK_GREY_SHADOW);
    }
    return BARS_WIDTH;
}

/* The networks in range, in the save list's window: name, security and
 * signal; ones the companion cannot join in grey. */
static void DrawNetworks(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    int row, ox = LIST_X * 8, oy = LIST_Y * 8 + 4, right = (LIST_X + LIST_W) * 8 - 4;
    Window(top, LIST_X, LIST_Y, LIST_W, LIST_H);
    CpCanvas_Clip(&top->text, LIST_X * 8, LIST_Y * 8, LIST_W * 8, LIST_H * 8);
    for (row = 0; row < CP_UI_NETWORK_ROWS; row++) {
        int index = ui->wifiTop + row, labelWidth;
        const CpWifiNetwork *network;
        const char *label;
        char name[CP_WIFI_SSID_MAX * 2 + 4];
        CpInk ink;
        if (index >= (int)model->wifi.count) break;
        network = &model->wifi.networks[index];
        ink = CpWifi_Unsupported(network->security, model->wifi.wpa2) == CP_WIFI_SUPPORTED ? kDark : kGrey;
        if (index == ui->wifiCursor) Marker(top, ox, oy + row * 16 + 3);
        Bars(top, right, oy + row * 16 + 13, network->bars, ink);
        label = SecurityName(network->security);
        labelWidth = CpText_Width(label);
        CpCanvas_Text(&top->text, right - BARS_WIDTH - 6 - labelWidth, oy + row * 16, label, ink);
        Fit(name, sizeof(name), network->ssid, right - BARS_WIDTH - 6 - labelWidth - (ox + 8) - 8);
        CpCanvas_Text(&top->text, ox + 8, oy + row * 16, name, ink);
    }
    CpCanvas_Unclip(&top->text);
}

/* The chosen network, the password as typed (or as dots), how long it is,
 * and where it will be kept. */
static void DrawPassword(const CpUi *ui, CpScreen *top)
{
    const CpKeyboard *keyboard = &ui->keyboard;
    int ox = LIST_X * 8 + 4, oy = LIST_Y * 8 + 4, right = (LIST_X + LIST_W) * 8 - 4;
    int fieldTop = oy + 20, counterWidth, available, start = 0, width;
    char shown[CP_KEYBOARD_TEXT_MAX + 1], counter[16], name[CP_WIFI_SSID_MAX * 2 + 4];
    unsigned i, max = ui->wifiSecurity == CP_WIFI_SECURITY_WEP ? 26 : CP_WIFI_PASSPHRASE_MAX;
    Window(top, LIST_X, LIST_Y, LIST_W, LIST_H);
    CpCanvas_Clip(&top->text, LIST_X * 8, LIST_Y * 8, LIST_W * 8, LIST_H * 8);
    Fit(name, sizeof(name), ui->wifiSsid, right - ox);
    CpCanvas_Text(&top->text, ox, oy, name, kDark);
    /* The field: the text's tail and the caret over an underline. */
    snprintf(counter, sizeof(counter), "%u/%u", keyboard->length, max);
    counterWidth = CpText_Width(counter);
    CpCanvas_Text(&top->text, right - counterWidth, fieldTop + 2, counter, kGrey);
    available = right - counterWidth - 8 - ox - 4;
    CpCanvas_Fill(&top->text, ox, fieldTop + 17, right - counterWidth - 8 - ox, 1, CP_INK_GREY);
    for (i = 0; i < keyboard->length && i < CP_KEYBOARD_TEXT_MAX; i++) shown[i] = keyboard->shown ? keyboard->text[i] : '*';
    shown[i] = 0;
    while (shown[start] && CpText_Width(shown + start) > available) start++;
    width = CpCanvas_Text(&top->text, ox + 2, fieldTop + 2, shown + start, kDark);
    CpCanvas_Fill(&top->text, ox + 3 + width, fieldTop + 3, 2, 12, CP_INK_CURSOR);
    for (i = 0; i < sizeof(shown); i++) ((volatile char *)shown)[i] = 0;
    /* Where the password goes once it works. */
    {
        char wrapped[CP_RENDER_MESSAGE_MAX];
        CpText_Wrap(wrapped, sizeof(wrapped), T(Companion_Text_WifiPasswordWarning), right - ox);
        CpCanvas_Text(&top->text, ox, fieldTop + 28, wrapped, kGrey);
    }
    CpCanvas_Unclip(&top->text);
}

static const CpUiSummary *Summary(const CpUi *ui, const CpUiModel *model)
{
    return ui->summaryReturn == CP_UI_RECEIVED ? &model->received.summary : &model->party[ui->partyCursor].summary;
}

static void SummaryCard(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    const CpUiSummary *mon = Summary(ui, model);
    int received = ui->summaryReturn == CP_UI_RECEIVED;
    DrawPokemonCard(ui, model, top, received ? CP_UI_CARD_RECEIVED : CP_UI_CARD_PARTY);
    Window(top, 2, 16, 28, 7);
    if (mon->statsOnly) {
        char hp[32];
        snprintf(hp, sizeof(hp), "%s  %u/%u", T(Companion_Text_HP), mon->hp, mon->stats[0]);
        CpCanvas_Text(&top->text, 24, 143, hp, kDark);
        return;
    }
    const int labels[] = {Companion_Text_Nature, Companion_Text_Ability, Companion_Text_HeldItem};
    const uint16_t *values[] = {mon->nature, mon->ability, mon->item};
    for (int i = 0; i < 3; ++i) {
        int y = 129 + i * 18;
        CpCanvas_Clip(&top->text, 21, y, 88, 17);
        CpCanvas_Text(&top->text, 21, y, T(labels[i]), kGrey);
        CpCanvas_Clip(&top->text, 113, y, 125, 17);
        if (values[i][0] == CP_CHAR_EOS) CpCanvas_Text(&top->text, 113, y, "-", kDark);
        else GameText(&top->text, 113, y, values[i], i == 2 ? CP_UI_ITEM_NAME_MAX : CP_SUMMARY_NAME, model, kDark);
        CpCanvas_Unclip(&top->text);
    }
}

static void RightText(CpCanvas *text, int right, int y, const char *label, CpInk ink)
{
    CpCanvas_Text(text, right - CpText_Width(label), y, label, ink);
}

static void SummaryTable(const CpUi *ui, const CpUiModel *model, CpScreen *bottom)
{
    const CpUiSummary *mon = Summary(ui, model);
    CpCanvas *text = &bottom->text;
    char line[32];
    Window(bottom, 2, 4, 28, 14);
    CpCanvas_Text(text, 22, 32, T(ui->summaryMoves ? Companion_Text_Moves : Companion_Text_Stats), kDark);
    if (ui->summaryMoves) {
        RightText(text, 233, 32, "PP", kGrey);
        for (unsigned i = 0; i < 4; ++i) {
            int y = 51 + i * 22;
            if (!(i & 1)) CpCanvas_Fill(text, 16, y, 224, 18, CP_INK_STRIPE);
            if (mon->moves[i][0] == CP_CHAR_EOS) {
                CpCanvas_Text(text, 22, y, "-", kGrey);
                continue;
            }
            CpCanvas_Clip(text, 22, y, 162, 17);
            GameText(text, 22, y, mon->moves[i], CP_SUMMARY_NAME, model, kDark);
            CpCanvas_Unclip(text);
            snprintf(line, sizeof(line), "%u/%u", mon->pp[i], mon->maxPp[i]);
            RightText(text, 233, y, line, kDark);
        }
    } else {
        static const int labels[] = { Companion_Text_HP, Companion_Text_Attack, Companion_Text_Defense,
            Companion_Text_Speed, Companion_Text_SpAtk, Companion_Text_SpDef };
        static const unsigned order[] = {0,1,2,4,5,3};
        RightText(text, 191, 32, "IV", kGrey);
        RightText(text, 233, 32, "EV", kGrey);
        for (unsigned row = 0; row < 6; ++row) {
            unsigned i = order[row];
            int y = 48 + row * 16;
            if (!(row & 1)) CpCanvas_Fill(text, 16, y, 224, 16, CP_INK_STRIPE);
            CpCanvas_Clip(text, 22, y, 90, 16);
            CpCanvas_Text(text, 22, y, T(labels[i]), kDark);
            CpCanvas_Unclip(text);
            if (i == 0) snprintf(line, sizeof(line), "%u/%u", mon->hp, mon->stats[i]);
            else snprintf(line, sizeof(line), "%u", mon->stats[i]);
            RightText(text, 153, y, line, kDark);
            snprintf(line, sizeof(line), "%u", mon->iv[i]);
            RightText(text, 191, y, line, kDark);
            snprintf(line, sizeof(line), "%u", mon->ev[i]);
            RightText(text, 233, y, line, kDark);
        }
    }
}

static void DrawTop(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    enum CpUiCard card = CpUi_Card(ui, model);
    char message[CP_RENDER_MESSAGE_MAX];
    Clear(top);
    if (ui->state == CP_UI_SUMMARY) { SummaryCard(ui, model, top); return; }
    CpRender_Message(ui, model, message, sizeof(message));
    /* A message alone that needs more than its box's three lines, such as one
     * naming a long file, takes the full text window instead. */
    if (card == CP_UI_CARD_NONE && CpRender_MessageLines(message, NULL) > MESSAGE_H * 8 / 16) card = CP_UI_CARD_FULL_TEXT;
    switch (card) {
    case CP_UI_CARD_FULL_TEXT:
        DrawBody(top, message);
        if (ui->state == CP_UI_ERROR || ui->state == CP_UI_WIFI_FAILED || ui->state == CP_UI_WIFI_FORGET_FAILED) {
            char number[12];
            int right = (BODY_X + BODY_W) * 8 - 8;
            /* Under up to five lines of explanation, as in Diamond/Pearl. */
            CpCanvas_Text(&top->text, BODY_X * 8 + 4, BODY_Y * 8 + 112, T(WonderTrade_Text_ErrorCode), kDark);
            Number(number, sizeof(number), ui->state == CP_UI_ERROR ? model->error : model->wifi.code);
            CpCanvas_Text(&top->text, right - CpText_Width(number), BODY_Y * 8 + 112, number, kDark);
        }
        break;
    case CP_UI_CARD_PARTY:
    case CP_UI_CARD_RECEIVED:
        DrawPokemonCard(ui, model, top, card);
        DrawMessage(top, message, MESSAGE_Y);
        break;
    case CP_UI_CARD_SAVES:
        DrawSaves(ui, model, top);
        DrawMessage(top, message, MESSAGE_Y);
        break;
    case CP_UI_CARD_NETWORKS:
        DrawNetworks(ui, model, top);
        DrawMessage(top, message, MESSAGE_Y);
        break;
    case CP_UI_CARD_PASSWORD:
        DrawPassword(ui, top);
        DrawMessage(top, message, MESSAGE_Y);
        break;
    default:
        DrawMessage(top, message, MESSAGE_Y_ALONE);
        break;
    }
}

static void DrawButton(CpScreen *bottom, int x, int y, int w, int h, const char *label, unsigned palette)
{
    Box(bottom, CP_TILE_BUTTON, x / 8, y / 8, w / 8, h / 8, palette);
    CpCanvas_Text(&bottom->text, x + (w - CpText_Width(label)) / 2, y + (h - 16) / 2, label, kDark);
}

static void ActionButton(CpScreen *screen, int x, int y, int w, int h, const char *key,
                         const char *text, unsigned palette)
{
    char label[128], fitted[128];
    snprintf(label, sizeof(label), "%s%s%s", key, *key ? "  " : "", text);
    Fit(fitted, sizeof(fitted), label, w - 8);
    DrawButton(screen, x, y, w, h, fitted, palette);
}

static void DrawParty(const CpUi *ui, const CpUiModel *model, CpScreen *bottom)
{
    int slot;
    for (slot = 0; slot < CP_UI_PARTY_MAX; slot++) {
        int x = CP_UI_PANEL_LEFT + (slot % 2) * CP_UI_PANEL_STEP_X;
        int y = CP_UI_PANEL_TOP + (slot / 2) * CP_UI_PANEL_STEP_Y;
        int tx = x / 8, ty = y / 8;
        const CpUiPokemon *mon = &model->party[slot];
        unsigned palette;
        CpInk ink;
        char number[12];
        if (slot >= (int)model->partyCount) {
            Box(bottom, CP_TILE_BUTTON, tx, ty, CP_UI_PANEL_WIDTH / 8, CP_UI_PANEL_HEIGHT / 8, CP_PAL_DISABLED);
            continue;
        }
        palette = slot == ui->partyCursor ? CP_PAL_SELECTED : mon->ineligible ? CP_PAL_DISABLED : CP_PAL_BUTTON;
        ink = mon->ineligible ? kGrey : kDark;
        Box(bottom, CP_TILE_BUTTON, tx, ty, CP_UI_PANEL_WIDTH / 8, CP_UI_PANEL_HEIGHT / 8, palette);
        int nameX = x + 8;
        if (mon->icon.ready) {
            bottom->spritePalettes[slot] = mon->icon.palette;
            for (unsigned row = 0; row < 4; ++row) {
                unsigned cell = (ty + 1 + row) * 32 + tx + 1;
                memcpy(bottom->text.tiles + cell * 32, mon->icon.tiles + row * 128, 128);
                for (unsigned col = 0; col < 4; ++col)
                    bottom->textMap[cell + col] = ENTRY(cell + col, CpRender_PortraitPalette(slot));
            }
            nameX += 32;
        } else if (mon->portrait.ready) {
            Portrait(bottom, &mon->portrait, (unsigned)slot, tx + 1, ty + 1, 32);
            nameX += 32;
        }
        CpCanvas_Clip(&bottom->text, nameX, y, x + 104 - nameX, 20);
        GameText(&bottom->text, nameX, y + 3, mon->nickname, CP_UI_NAME_MAX, model, ink);
        CpCanvas_Unclip(&bottom->text);
        if (!mon->isEgg) GenderIcon(&bottom->text, x + 104, y + 4, mon->gender);
        if (mon->heldItem) StatusMark(&bottom->text, x + 96, y + 24, 0);
        if (mon->shiny) StatusMark(&bottom->text, x + 104, y + 24, 1);
        CpCanvas_Clip(&bottom->text, nameX, y + 20, x + 96 - nameX, 18);
        if (mon->ineligible) {
            CpCanvas_Text(&bottom->text, nameX, y + 20, T(CpUi_IneligibleText(mon->ineligible)), ink);
        } else {
            int width = CpCanvas_Text(&bottom->text, nameX, y + 20, T(WonderTrade_Text_Level), ink);
            Number(number, sizeof(number), mon->level);
            CpCanvas_Text(&bottom->text, nameX + width + 4, y + 20, number, ink);
        }
        CpCanvas_Unclip(&bottom->text);
    }
    if (ui->partyCursor < (int)model->partyCount && model->party[ui->partyCursor].summary.ready) {
        ActionButton(bottom, 8, 160, 136, 24, "Y", T(Companion_Text_Summary), CP_PAL_BUTTON);
    }
    ActionButton(bottom, CP_UI_PICKER_BACK_LEFT, CP_UI_PICKER_BACK_TOP, CP_UI_PICKER_BACK_WIDTH, CP_UI_PICKER_BACK_HEIGHT,
        "B", T(WonderTrade_Text_Back), ui->partyCursor == CP_UI_PICKER_BACK ? CP_PAL_SELECTED : CP_PAL_BUTTON);
}

static void DrawSources(const CpUi *ui, const CpUiModel *model, CpScreen *bottom)
{
    static const int languages[] = { Companion_Text_LanguageEnglish, Companion_Text_LanguageFrench,
        Companion_Text_LanguageGerman, Companion_Text_LanguageItalian, Companion_Text_LanguageSpanish,
        Companion_Text_LanguageJapanese, Companion_Text_LanguageKorean };
    if (ui->state == CP_UI_LANGUAGE) {
        for (int i = 0; i < Companion_Language_Count; ++i)
            ActionButton(bottom, CP_GRID_LEFT + (i % 2) * CP_GRID_STEP, CP_SOURCE_TOP + (i / 2) * CP_SOURCE_STEP,
                CP_GRID_WIDTH, CP_SOURCE_HEIGHT, "", Companion_Text[languages[i]][i],
                i == ui->languageCursor ? CP_PAL_SELECTED : CP_PAL_BUTTON);
        return;
    }
    if (ui->state == CP_UI_GAME_LIST) {
        DrawButton(bottom, CP_GRID_LEFT, CP_GAME_NAV_Y, CP_GRID_WIDTH, CP_SOURCE_HEIGHT, T(Companion_Text_PreviousGame), CP_PAL_BUTTON);
        DrawButton(bottom, CP_GRID_LEFT + CP_GRID_STEP, CP_GAME_NAV_Y, CP_GRID_WIDTH, CP_SOURCE_HEIGHT, T(Companion_Text_NextGame), CP_PAL_BUTTON);
        ActionButton(bottom, CP_GRID_LEFT, CP_GAME_ACTION_Y, CP_GRID_WIDTH, CP_SOURCE_HEIGHT, "A", T(WonderTrade_Text_ConfirmAction), CP_PAL_BUTTON);
        ActionButton(bottom, CP_GRID_LEFT + CP_GRID_STEP, CP_GAME_ACTION_Y, CP_GRID_WIDTH, CP_SOURCE_HEIGHT, "B", T(WonderTrade_Text_Back), CP_PAL_BUTTON);
        return;
    }
    for (int row = 0; row < CpUi_SourceRows(model); ++row) {
        int selected = row == ui->sourceFocus;
        ActionButton(bottom, CP_SOURCE_LEFT, CP_SOURCE_TOP + row * CP_SOURCE_STEP, CP_SOURCE_WIDTH, CP_SOURCE_HEIGHT,
            "", T(CpUi_SourceLabel(model, row)), selected ? CP_PAL_SELECTED : CP_PAL_BUTTON);
    }
}

/* The password keyboard (ui/keyboard.h) in the buttons' tiles and palettes. */
static void DrawKeyboard(const CpUi *ui, CpScreen *bottom)
{
    const CpKeyboard *keyboard = &ui->keyboard;
    int index;
    for (index = 0; index < CP_KEY_COUNT; index++) {
        CpKey key;
        char character[2] = { 0, 0 };
        const char *label = character;
        unsigned palette = CP_PAL_BUTTON;
        CpKeyboard_Key(keyboard, index, &key);
        switch (key.kind) {
        case CP_KEY_CHARACTER:
            character[0] = key.character;
            if (!key.character) palette = CP_PAL_DISABLED;
            break;
        case CP_KEY_SHIFT: label = keyboard->layer == CP_KEYS_UPPER ? "abc" : "ABC"; break;
        case CP_KEY_SYMBOLS: label = keyboard->layer == CP_KEYS_SYMBOLS ? "abc" : "#+="; break;
        case CP_KEY_SPACE: label = T(Companion_Text_KeySpace); break;
        case CP_KEY_DELETE: label = T(Companion_Text_KeyDelete); break;
        case CP_KEY_SHOW: label = T(keyboard->shown ? Companion_Text_KeyHide : Companion_Text_KeyShow); break;
        case CP_KEY_JOIN:
            label = T(Companion_Text_WifiJoin);
            /* Grey until the password could be right; pressing it says why. */
            if (!CpWifi_KeyValid(ui->wifiSecurity, keyboard->text)) palette = CP_PAL_DISABLED;
            break;
        default: label = T(WonderTrade_Text_Back); break;
        }
        if (index == keyboard->cursor) palette = CP_PAL_SELECTED;
        const char *shortcut = "";
        if (key.kind == (keyboard->length ? CP_KEY_DELETE : CP_KEY_BACK)) shortcut = "B";
        ActionButton(bottom, key.x * 8, key.y * 8, key.w * 8, key.h * 8, shortcut, label, palette);
    }
}

static void DrawBottom(const CpUi *ui, const CpUiModel *model, CpScreen *bottom)
{
    int labels[CP_UI_MAX_ACTIONS], count, i;
    Clear(bottom);
    if (ui->state == CP_UI_SAVE_PICKER || ui->state == CP_UI_LANGUAGE || ui->state == CP_UI_GAME_LIST) {
        DrawSources(ui, model, bottom);
    } else if (ui->state == CP_UI_PICKER) {
        DrawParty(ui, model, bottom);
    } else if (ui->state == CP_UI_SUMMARY) {
        SummaryTable(ui, model, bottom);
        if (CpUi_SummaryHasMoves(ui, model)) {
            for (int page = 0; page < 2; ++page) {
                char label[64];
                snprintf(label, sizeof(label), "%s  %s", page ? "R" : "L", T(page ? Companion_Text_Moves : Companion_Text_Stats));
                int x = 8 + 76 * page;
                CpCanvas_Clip(&bottom->text, x, CP_SUMMARY_NAV_Y, 72, 30);
                CpCanvas_Text(&bottom->text, x + 2, CP_SUMMARY_NAV_Y + 6, label, kDark);
                CpCanvas_Unclip(&bottom->text);
                if (ui->summaryMoves == page)
                    CpCanvas_Fill(&bottom->text, x, CP_SUMMARY_NAV_Y + 24, 72, 2, CP_INK_TITLE);
            }
        }
        ActionButton(bottom, CP_UI_PICKER_BACK_LEFT, CP_SUMMARY_NAV_Y, CP_UI_PICKER_BACK_WIDTH, 32,
            "B", T(WonderTrade_Text_Back), CP_PAL_BUTTON);
    } else if (ui->state == CP_UI_WIFI_KEYBOARD) {
        DrawKeyboard(ui, bottom);
    } else {
        count = CpUi_Actions(ui, model, labels);
        for (i = 0; i < count; i++) {
            const char *key = "";
            if (labels[i] == WonderTrade_Text_Back || labels[i] == WonderTrade_Text_Cancel ||
                labels[i] == Companion_Text_ChangeGame) key = "B";
            if (ui->state == CP_UI_RECEIVED && labels[i] == Companion_Text_Summary) key = "Y";
            ActionButton(bottom, WT_MENU_LEFT, WT_MENU_TOP + i * WT_MENU_SPACING, WT_MENU_WIDTH, WT_MENU_HEIGHT,
                key, T(labels[i]), i == ui->cursor ? CP_PAL_SELECTED : CP_PAL_BUTTON);
        }
    }
    if (ui->state == CP_UI_MESSAGE && ui->message == WonderTrade_Text_Explanation) {
        /* Info's top screen is full; keep the repair warning below Back. */
        Window(bottom, 2, 10, 28, 13);
        WindowText(bottom, 2, 10, 28, 13, 4, T(Companion_Text_BackupWarning));
    }
    if (CpUi_Busy(ui)) {
        for (i = 0; i < 4; i++)
            Put(bottom, CP_SPINNER_X + i % 2, CP_SPINNER_Y + i / 2, ENTRY(CP_TILE_SPINNER + i, CP_PAL_SPINNER));
    }
}

void CpRender_Draw(const CpUi *ui, const CpUiModel *model, CpScreen *top, CpScreen *bottom)
{
    sLanguage = CpUi_Language(ui, model);
    DrawTop(ui, model, top);
    DrawBottom(ui, model, bottom);
    Header(top);
    Header(bottom);
}
