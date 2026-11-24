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
        C(26, 26, 27), C(31, 18, 2), 0, 0, 0, 0, 0, 0, 0, C(31, 31, 31) },
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

static void Icon(CpScreen *screen, int x, int y, unsigned icon)
{
    Put(screen, x, y, ENTRY(CP_TILE_ICON + icon, CP_PAL_ICON));
}

static void Header(CpScreen *screen)
{
    int x;
    for (x = 0; x < 32; x++) {
        Put(screen, x, 0, ENTRY(CP_TILE_HEADER_TOP, CP_PAL_BACKDROP));
        Put(screen, x, 1, ENTRY(CP_TILE_HEADER, CP_PAL_BACKDROP));
        Put(screen, x, 2, ENTRY(CP_TILE_HEADER_EDGE, CP_PAL_BACKDROP));
    }
    CpCanvas_Text(&screen->text, TITLE_X * 8, TITLE_Y * 8, T(WonderTrade_Text_Title), kLight);
}

static void Clear(CpScreen *screen)
{
    CpCanvas_Clear(&screen->text);
    memset(screen->map, 0, sizeof(screen->map));
    Header(screen);
}

static int GameName(unsigned version)
{
    switch (version) {
    case CP_VERSION_DIAMOND: return Companion_Text_GameDiamond;
    case CP_VERSION_PEARL: return Companion_Text_GamePearl;
    case CP_VERSION_PLATINUM: return Companion_Text_GamePlatinum;
    case CP_VERSION_HEARTGOLD: return Companion_Text_GameHeartGold;
    case CP_VERSION_SOULSILVER: return Companion_Text_GameSoulSilver;
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

/* The state's message with its {tokens} filled in. */
static void MessageText(const CpUi *ui, const CpUiModel *model, char *out, size_t size)
{
    char game[512], language[512], stem[CP_UI_SAVE_NAME_MAX];
    int gameName = GameName(model->gameVersion), languageName = LanguageName(model->gameLanguage);
    stem[0] = 0;
    if (ui->listCursor >= 0 && ui->listCursor < (int)model->saveCount)
        SaveStem(stem, sizeof(stem), model->saves[ui->listCursor].name);
    CpText_Replace(game, sizeof(game), T(ui->message), "{game}", gameName >= 0 ? T(gameName) : "?");
    CpText_Replace(language, sizeof(language), game, "{language}", languageName >= 0 ? T(languageName) : "?");
    CpText_Replace(out, size, language, "{name}", stem);
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

static void DrawMessage(CpScreen *top, const char *text, int y)
{
    Window(top, MESSAGE_X, y, MESSAGE_W, MESSAGE_H);
    CpCanvas_Text(&top->text, MESSAGE_X * 8, y * 8, text, kDark);
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

static void GenderIcon(CpScreen *screen, int x, int y, unsigned gender)
{
    if (gender == CP_UI_MALE) Icon(screen, x, y, CP_ICON_MALE);
    else if (gender == CP_UI_FEMALE) Icon(screen, x, y, CP_ICON_FEMALE);
}

/* The details card (application.c 381-412). */
static void DrawPokemonCard(const CpUi *ui, const CpUiModel *model, CpScreen *top, enum CpUiCard card)
{
    CpCanvas *text = &top->text;
    int ox = DETAILS_X * 8, oy = DETAILS_Y * 8;
    char number[12];
    Card(top);
    if (card == CP_UI_CARD_PARTY) {
        int slot = ui->state == CP_UI_CONFIRM ? ui->selectedSlot : ui->listCursor;
        const CpUiPokemon *mon = &model->party[slot];
        int width = CpCanvas_GameText(text, ox, oy + 16, mon->speciesName, CP_UI_NAME_MAX, model->font, kLight);
        GenderIcon(top, (ox + width) / 8 + 1, DETAILS_Y + 2, mon->isEgg ? CP_UI_GENDERLESS : mon->gender);
        if (mon->shiny) Icon(top, PREVIEW_X, PREVIEW_Y, CP_ICON_SHINY);
        CpCanvas_GameText(text, ox, oy + 32, mon->nickname, CP_UI_NAME_MAX, model->font, kLight);
        if (mon->heldItem) {
            Icon(top, DETAILS_X, DETAILS_Y + 6, CP_ICON_ITEM);
            CpCanvas_GameText(text, ox + 12, oy + 48, mon->itemName, CP_UI_ITEM_NAME_MAX, model->font, kLight);
        }
        CpCanvas_Text(text, ox, oy + 64, T(WonderTrade_Text_Level), kLight);
        Number(number, sizeof(number), mon->level);
        CpCanvas_Text(text, ox + 48 - CpText_Width(number), oy + 64, number, kLight);
    } else {
        const CpUiReceived *received = &model->received;
        int width;
        CpCanvas_Text(text, ox, oy, T(WonderTrade_Text_Received), kLight);
        width = CpCanvas_GameText(text, ox, oy + 16, received->speciesName, CP_UI_NAME_MAX, model->font, kLight);
        GenderIcon(top, (ox + width) / 8 + 1, DETAILS_Y + 2, received->gender);
        if (received->shiny) Icon(top, PREVIEW_X, PREVIEW_Y, CP_ICON_SHINY);
        if (received->hasNickname)
            CpCanvas_GameText(text, ox, oy + 32, received->nickname, CP_UI_NAME_MAX, model->font, kLight);
        CpCanvas_Text(text, ox, oy + 64, T(WonderTrade_Text_OriginalTrainer), kLight);
        CpCanvas_GameText(text, ox + 40, oy + 64, received->originalTrainer, CP_UI_TRAINER_MAX, model->font, kLight);
    }
}

static void DrawSaves(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    int row, ox = LIST_X * 8, oy = LIST_Y * 8 + 4, right = (LIST_X + LIST_W) * 8 - 4;
    Window(top, LIST_X, LIST_Y, LIST_W, LIST_H);
    for (row = 0; row < CP_UI_SAVE_ROWS; row++) {
        int index = ui->listTop + row, detail;
        const CpUiSave *save;
        char label[96], name[CP_UI_SAVE_NAME_MAX];
        CpInk ink;
        int width;
        if (index >= (int)model->saveCount) break;
        save = &model->saves[index];
        ink = save->status == CP_UI_SAVE_READY ? kDark : kGrey;
        if (index == ui->listCursor) Marker(top, ox, oy + row * 16 + 3);
        if (save->status == CP_UI_SAVE_NO_ROM) snprintf(label, sizeof(label), "%s", T(Companion_Text_SaveNoRom));
        else if (save->status != CP_UI_SAVE_READY || (detail = GameName(save->version)) < 0)
            snprintf(label, sizeof(label), "%s", T(Companion_Text_SaveUnsupported));
        else {
            int language = LanguageName(save->language);
            snprintf(label, sizeof(label), "%s %s", T(detail), language >= 0 ? T(language) : "");
        }
        width = CpText_Width(label);
        CpCanvas_Text(&top->text, right - width, oy + row * 16, label, ink);
        Fit(name, sizeof(name), save->name, right - width - (ox + 8) - 8);
        CpCanvas_Text(&top->text, ox + 8, oy + row * 16, name, ink);
    }
}

static void DrawTop(const CpUi *ui, const CpUiModel *model, CpScreen *top)
{
    enum CpUiCard card = CpUi_Card(ui, model);
    char message[512];
    Clear(top);
    MessageText(ui, model, message, sizeof(message));
    switch (card) {
    case CP_UI_CARD_FULL_TEXT:
        Window(top, BODY_X, BODY_Y, BODY_W, BODY_H);
        CpCanvas_Text(&top->text, BODY_X * 8 + 4, BODY_Y * 8, message, kDark);
        if (ui->state == CP_UI_ERROR) {
            char number[12];
            int right = (BODY_X + BODY_W) * 8 - 8;
            /* Under up to five lines of explanation, as in Diamond/Pearl. */
            CpCanvas_Text(&top->text, BODY_X * 8 + 4, BODY_Y * 8 + 112, T(WonderTrade_Text_ErrorCode), kDark);
            Number(number, sizeof(number), model->error);
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
        palette = slot == ui->listCursor ? CP_PAL_SELECTED : mon->ineligible ? CP_PAL_DISABLED : CP_PAL_BUTTON;
        ink = mon->ineligible ? kGrey : kDark;
        Box(bottom, CP_TILE_BUTTON, tx, ty, CP_UI_PANEL_WIDTH / 8, CP_UI_PANEL_HEIGHT / 8, palette);
        CpCanvas_GameText(&bottom->text, x + 8, y + 3, mon->nickname, CP_UI_NAME_MAX, model->font, ink);
        if (!mon->isEgg) GenderIcon(bottom, tx + 13, ty + 1, mon->gender);
        if (mon->heldItem) Icon(bottom, tx + 12, ty + 3, CP_ICON_ITEM);
        if (mon->shiny) Icon(bottom, tx + 13, ty + 3, CP_ICON_SHINY);
        if (mon->ineligible) {
            CpCanvas_Text(&bottom->text, x + 8, y + 20, T(CpUi_IneligibleText(mon->ineligible)), ink);
        } else {
            int width = CpCanvas_Text(&bottom->text, x + 8, y + 20, T(WonderTrade_Text_Level), ink);
            Number(number, sizeof(number), mon->level);
            CpCanvas_Text(&bottom->text, x + 8 + width + 4, y + 20, number, ink);
        }
    }
    DrawButton(bottom, CP_UI_PICKER_BACK_LEFT, CP_UI_PICKER_BACK_TOP, CP_UI_PICKER_BACK_WIDTH, CP_UI_PICKER_BACK_HEIGHT,
        T(WonderTrade_Text_Back), ui->listCursor == CP_UI_PICKER_BACK ? CP_PAL_SELECTED : CP_PAL_BUTTON);
}

static void DrawBottom(const CpUi *ui, const CpUiModel *model, CpScreen *bottom)
{
    int labels[CP_UI_MAX_ACTIONS], count, i;
    Clear(bottom);
    if (ui->state == CP_UI_PICKER) {
        DrawParty(ui, model, bottom);
    } else {
        count = CpUi_Actions(ui, model, labels);
        for (i = 0; i < count; i++)
            DrawButton(bottom, WT_MENU_LEFT, WT_MENU_TOP + i * WT_MENU_SPACING, WT_MENU_WIDTH, WT_MENU_HEIGHT,
                T(labels[i]), i == ui->cursor ? CP_PAL_SELECTED : CP_PAL_BUTTON);
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
}
