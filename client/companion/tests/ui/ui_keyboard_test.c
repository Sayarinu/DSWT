/* The Wi-Fi password keyboard: its layout, editing (ASCII in, whole UTF-8
 * characters out), the stylus and the D-pad, and the labels fitting their
 * keys in every language. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/canvas.h"
#include "ui/keyboard.h"
#include "ui/render.h"

static int KeyOf(const CpKeyboard *keyboard, char character)
{
    int index;
    for (index = 0; index < CP_KEY_CHARACTERS; ++index) {
        CpKey key;
        CpKeyboard_Key(keyboard, index, &key);
        if (key.character == character) return index;
    }
    return -1;
}

static void TestLayout(void)
{
    CpKeyboard keyboard;
    int index, other, character, layer, found;
    CpKeyboard_Reset(&keyboard, CP_KEYBOARD_TEXT_MAX);
    /* Keys stay on the bottom screen below the title, and never overlap. */
    for (index = 0; index < CP_KEY_COUNT; ++index) {
        CpKey key, next;
        CpKeyboard_Key(&keyboard, index, &key);
        assert(key.w >= 3 && key.h == 3 && key.y >= 3 && key.x + key.w <= 32 && key.y + key.h <= 24);
        for (other = index + 1; other < CP_KEY_COUNT; ++other) {
            CpKeyboard_Key(&keyboard, other, &next);
            assert(key.x + key.w <= next.x || next.x + next.w <= key.x || key.y + key.h <= next.y || next.y + next.h <= key.y);
        }
        /* The centre of a key touches that key. */
        assert(CpKeyboard_Touch(key.x * 8 + key.w * 4, key.y * 8 + key.h * 4) == index);
    }
    assert(CpKeyboard_Touch(0, 0) == -1 && CpKeyboard_Touch(4, 100) == -1 && CpKeyboard_Touch(255, 191) == -1);
    /* Every printable ASCII character but space has a key on some layer;
     * space has its own. */
    for (character = 0x21; character <= 0x7E; ++character) {
        for (found = 0, layer = CP_KEYS_LOWER; layer <= CP_KEYS_SYMBOLS && !found; ++layer) {
            keyboard.layer = (uint8_t)layer;
            found = KeyOf(&keyboard, (char)character) >= 0;
        }
        if (!found) fprintf(stderr, "no key for '%c'\n", character);
        assert(found);
    }
}

static void TestEditing(void)
{
    CpKeyboard keyboard;
    unsigned i;
    CpKeyboard_Reset(&keyboard, 63);
    assert(CpKeyboard_Backspace(&keyboard) != 0 && keyboard.length == 0);
    /* Only printable ASCII goes in. */
    assert(CpKeyboard_Insert(&keyboard, 'a') == 0 && CpKeyboard_Insert(&keyboard, ' ') == 0);
    assert(CpKeyboard_Insert(&keyboard, '\n') != 0 && CpKeyboard_Insert(&keyboard, 0x7F) != 0);
    assert(CpKeyboard_Insert(&keyboard, 0xC3) != 0 && CpKeyboard_Insert(&keyboard, -61) != 0);
    assert(!strcmp(keyboard.text, "a ") && keyboard.length == 2);
    /* Up to the limit and no further. */
    for (i = 2; i < 63; ++i) assert(CpKeyboard_Insert(&keyboard, 'x') == 0);
    assert(CpKeyboard_Insert(&keyboard, 'y') != 0 && keyboard.length == 63 && strlen(keyboard.text) == 63);
    CpKeyboard_Reset(&keyboard, 200);
    assert(keyboard.max == CP_KEYBOARD_TEXT_MAX);
    for (i = 0; i < CP_KEYBOARD_TEXT_MAX; ++i) assert(CpKeyboard_Insert(&keyboard, 'f') == 0);
    assert(CpKeyboard_Insert(&keyboard, 'f') != 0 && keyboard.text[CP_KEYBOARD_TEXT_MAX] == 0);

    /* Delete removes a whole UTF-8 character, whatever its length. */
    CpKeyboard_Reset(&keyboard, 63);
    strcpy(keyboard.text, "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80z");
    keyboard.length = (unsigned)strlen(keyboard.text);
    assert(CpKeyboard_Backspace(&keyboard) == 0 && !strcmp(keyboard.text, "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80"));
    assert(CpKeyboard_Backspace(&keyboard) == 0 && !strcmp(keyboard.text, "a\xc3\xa9\xe3\x81\x82"));
    assert(CpKeyboard_Backspace(&keyboard) == 0 && !strcmp(keyboard.text, "a\xc3\xa9"));
    assert(CpKeyboard_Backspace(&keyboard) == 0 && !strcmp(keyboard.text, "a"));
    assert(CpKeyboard_Backspace(&keyboard) == 0 && keyboard.length == 0 && keyboard.text[0] == 0);
    /* A run of stray continuation bytes goes at most four at a time, and
     * never past the start. */
    strcpy(keyboard.text, "ab\x80\x80\x80\x80\x80\x80");
    keyboard.length = (unsigned)strlen(keyboard.text);
    assert(CpKeyboard_Backspace(&keyboard) == 0 && keyboard.length == 4);
    strcpy(keyboard.text, "\x80\x80");
    keyboard.length = 2;
    assert(CpKeyboard_Backspace(&keyboard) == 0 && keyboard.length == 0);

    /* Clear wipes every byte. */
    strcpy(keyboard.text, "secret-pass");
    keyboard.length = 11;
    CpKeyboard_Clear(&keyboard);
    for (i = 0; i < sizeof(keyboard.text); ++i) assert(keyboard.text[i] == 0);
    assert(keyboard.length == 0);
}

static void TestPress(void)
{
    CpKeyboard keyboard;
    CpKeyboard_Reset(&keyboard, 63);
    assert(CpKeyboard_Press(&keyboard, KeyOf(&keyboard, 'p')) == CP_KEY_CHARACTER);
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SHIFT) == CP_KEY_SHIFT && keyboard.layer == CP_KEYS_UPPER);
    CpKeyboard_Press(&keyboard, KeyOf(&keyboard, 'W'));
    CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SHIFT);
    assert(keyboard.layer == CP_KEYS_LOWER);
    CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SYMBOLS);
    assert(keyboard.layer == CP_KEYS_SYMBOLS);
    CpKeyboard_Press(&keyboard, KeyOf(&keyboard, '\\'));
    CpKeyboard_Press(&keyboard, KeyOf(&keyboard, '~'));
    /* A blank key of the symbols layer types nothing. */
    CpKeyboard_Press(&keyboard, CP_KEY_CHARACTERS - 1);
    CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SYMBOLS);
    assert(keyboard.layer == CP_KEYS_LOWER);
    CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SPACE);
    assert(!strcmp(keyboard.text, "pW\\~ "));
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_DELETE) == CP_KEY_DELETE && !strcmp(keyboard.text, "pW\\~"));
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SHOW) == CP_KEY_SHOW && keyboard.shown);
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_SHOW) == CP_KEY_SHOW && !keyboard.shown);
    /* Join and Back are the caller's. */
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_JOIN) == CP_KEY_JOIN);
    assert(CpKeyboard_Press(&keyboard, CP_KEY_INDEX_BACK) == CP_KEY_BACK && !strcmp(keyboard.text, "pW\\~"));
}

static void TestMove(void)
{
    int index, steps;
    /* Along a row, wrapping. */
    assert(CpKeyboard_Move(0, 1, 0) == 1 && CpKeyboard_Move(0, -1, 0) == 9 && CpKeyboard_Move(9, 1, 0) == 0);
    assert(CpKeyboard_Move(CP_KEY_INDEX_SHOW, 1, 0) == CP_KEY_INDEX_SHIFT);
    assert(CpKeyboard_Move(CP_KEY_INDEX_JOIN, 1, 0) == CP_KEY_INDEX_BACK);
    /* Down a column to the nearest key, through the editing row to Connect
     * and Back, then round to the top. */
    assert(CpKeyboard_Move(0, 0, 1) == 10 && CpKeyboard_Move(30, 0, 1) == CP_KEY_INDEX_SHIFT);
    assert(CpKeyboard_Move(34, 0, 1) == CP_KEY_INDEX_SPACE && CpKeyboard_Move(39, 0, 1) == CP_KEY_INDEX_SHOW);
    assert(CpKeyboard_Move(CP_KEY_INDEX_SHIFT, 0, 1) == CP_KEY_INDEX_JOIN);
    assert(CpKeyboard_Move(CP_KEY_INDEX_SHOW, 0, 1) == CP_KEY_INDEX_BACK);
    assert(CpKeyboard_Move(CP_KEY_INDEX_JOIN, 0, 1) == 2 || CpKeyboard_Move(CP_KEY_INDEX_JOIN, 0, 1) < 10);
    assert(CpKeyboard_Move(0, 0, -1) < CP_KEY_COUNT && CpKeyboard_Move(0, 0, -1) >= CP_KEY_INDEX_JOIN);
    /* Every key is reached from the first by the D-pad alone. */
    for (index = 0; index < CP_KEY_COUNT; ++index) {
        int at = 0, row;
        for (steps = 0, row = 0; row < 8 && at != index; ++row) {
            int column;
            for (column = 0; column < 12 && at != index; ++column, ++steps) at = CpKeyboard_Move(at, 1, 0);
            if (at != index) at = CpKeyboard_Move(at, 0, 1);
        }
        assert(at == index);
    }
}

/* Every label fits its key in every language. */
static void TestLabels(void)
{
    static const int ids[] = { Companion_Text_KeySpace, Companion_Text_KeyDelete, Companion_Text_KeyShow,
        Companion_Text_KeyHide, Companion_Text_WifiJoin, WonderTrade_Text_Back };
    static const int keys[] = { CP_KEY_INDEX_SPACE, CP_KEY_INDEX_DELETE, CP_KEY_INDEX_SHOW,
        CP_KEY_INDEX_SHOW, CP_KEY_INDEX_JOIN, CP_KEY_INDEX_BACK };
    unsigned i, language;
    for (i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        CpKey key;
        CpKeyboard_Key(NULL, keys[i], &key);
        for (language = 0; language < Companion_Language_Count; ++language) {
            int width = CpText_Width(Companion_Text[ids[i]][language]);
            if (width > key.w * 8 - 6) {
                fprintf(stderr, "label %u (language %u) is %d pixels for a %d-pixel key\n", i, language, width, key.w * 8);
                assert(0);
            }
        }
    }
    {
        CpKey shift;
        CpKeyboard_Key(NULL, CP_KEY_INDEX_SHIFT, &shift);
        assert(CpText_Width("ABC") <= shift.w * 8 - 6 && CpText_Width("#+=") <= shift.w * 8 - 6);
    }
}

/* The password field draws dots until Show: hidden "abc" looks exactly like
 * shown "***", and nothing of the keyboard's text draws past its window. */
static uint8_t sTopText[CP_CANVAS_BYTES], sBottomText[CP_CANVAS_BYTES], sHidden[CP_CANVAS_BYTES];
static CpScreen sTop = { .text = { .tiles = sTopText } }, sBottom = { .text = { .tiles = sBottomText } };

static void Draw(CpUi *ui, CpUiModel *model, const char *text, int shown)
{
    CpKeyboard_Reset(&ui->keyboard, 63);
    strcpy(ui->keyboard.text, text);
    ui->keyboard.length = (unsigned)strlen(text);
    ui->keyboard.shown = (uint8_t)shown;
    CpRender_Draw(ui, model, &sTop, &sBottom);
}

static void TestMasking(void)
{
    static CpUiModel model;
    CpUi ui;
    char longest[CP_KEYBOARD_TEXT_MAX + 1];
    int x, y;
    memset(&model, 0, sizeof(model));
    memset(&ui, 0, sizeof(ui));
    model.uiLanguage = Companion_Language_en_US;
    ui.state = CP_UI_WIFI_KEYBOARD;
    ui.message = Companion_Text_WifiPassword;
    ui.wifiReturn = CP_UI_SAVE_PICKER;
    ui.wifiSecurity = CP_WIFI_SECURITY_WPA2;
    strcpy(ui.wifiSsid, "Home");
    Draw(&ui, &model, "abc", 0);
    memcpy(sHidden, sTopText, sizeof(sHidden));
    Draw(&ui, &model, "***", 1);
    assert(!memcmp(sHidden, sTopText, sizeof(sHidden)));
    Draw(&ui, &model, "abc", 1);
    assert(memcmp(sHidden, sTopText, sizeof(sHidden)));
    /* 64 wide characters, shown: the field keeps the end in view, inside the window. */
    memset(longest, 'W', CP_KEYBOARD_TEXT_MAX);
    longest[CP_KEYBOARD_TEXT_MAX] = 0;
    Draw(&ui, &model, longest, 1);
    for (y = 24; y < 128; y++)
        for (x = 0; x < CP_CANVAS_WIDTH; x++)
            if (CpCanvas_Get(&sTop.text, x, y)) assert(x >= 16 && x < 240 && y >= 32 && y < 120);
}

int main(void)
{
    TestMasking();
    TestLayout();
    TestEditing();
    TestPress();
    TestMove();
    TestLabels();
    puts("ui_keyboard_test: ok");
    return 0;
}
