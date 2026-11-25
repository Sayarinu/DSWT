#include "ui/keyboard.h"

#include <string.h>

/* Every printable ASCII character is on some layer; the symbols layer ends
 * with blank keys. */
static const char kLayers[3][CP_KEY_CHARACTERS + 1] = {
    "1234567890" "qwertyuiop" "asdfghjkl-" "zxcvbnm_.@",
    "1234567890" "QWERTYUIOP" "ASDFGHJKL-" "ZXCVBNM_.@",
    "!\"#$%&'()*" "+,-./:;<=>" "?@[\\]^_`{|" "}~\0\0\0\0\0\0\0\0",
};

/* Tile rows: four of characters, the editing keys, then Connect and Back. */
#define KEYS_TOP 3
#define KEY_SIZE 3
#define KEYS_LEFT 1
#define EDIT_ROW 15
#define ACTION_ROW 19
#define ROWS 6

static const struct {
    uint8_t x, w, kind;
} kEditKeys[] = {
    { 1, 4, CP_KEY_SHIFT }, { 5, 4, CP_KEY_SYMBOLS }, { 9, 8, CP_KEY_SPACE },
    { 17, 7, CP_KEY_DELETE }, { 24, 7, CP_KEY_SHOW },
    { 1, 15, CP_KEY_JOIN }, { 16, 15, CP_KEY_BACK },
};

void CpKeyboard_Clear(CpKeyboard *keyboard)
{
    volatile char *text = keyboard->text;
    unsigned i;
    for (i = 0; i < sizeof(keyboard->text); ++i) text[i] = 0;
    keyboard->length = 0;
}

void CpKeyboard_Reset(CpKeyboard *keyboard, unsigned max)
{
    CpKeyboard_Clear(keyboard);
    keyboard->max = max > CP_KEYBOARD_TEXT_MAX ? CP_KEYBOARD_TEXT_MAX : max;
    keyboard->layer = CP_KEYS_LOWER;
    keyboard->shown = 0;
    keyboard->cursor = 0;
}

int CpKeyboard_Insert(CpKeyboard *keyboard, int character)
{
    if (character < 0x20 || character > 0x7E || keyboard->length >= keyboard->max) return -1;
    keyboard->text[keyboard->length++] = (char)character;
    keyboard->text[keyboard->length] = 0;
    return 0;
}

int CpKeyboard_Backspace(CpKeyboard *keyboard)
{
    unsigned removed = 0;
    unsigned char byte;
    if (!keyboard->length) return -1;
    /* Continuation bytes (10xxxxxx) back to the character's first byte; a
     * malformed run stops after the longest UTF-8 character. */
    do {
        byte = (unsigned char)keyboard->text[--keyboard->length];
        keyboard->text[keyboard->length] = 0;
    } while (keyboard->length && (byte & 0xC0) == 0x80 && ++removed < 4);
    return 0;
}

static int Row(int index)
{
    if (index < CP_KEY_CHARACTERS) return index / 10;
    return index < CP_KEY_INDEX_JOIN ? 4 : 5;
}

void CpKeyboard_Key(const CpKeyboard *keyboard, int index, CpKey *out)
{
    memset(out, 0, sizeof(*out));
    if (index < 0 || index >= CP_KEY_COUNT) return;
    if (index < CP_KEY_CHARACTERS) {
        out->x = (uint8_t)(KEYS_LEFT + (index % 10) * KEY_SIZE);
        out->y = (uint8_t)(KEYS_TOP + (index / 10) * KEY_SIZE);
        out->w = out->h = KEY_SIZE;
        out->kind = CP_KEY_CHARACTER;
        out->character = kLayers[keyboard ? keyboard->layer % 3 : 0][index];
        return;
    }
    out->x = kEditKeys[index - CP_KEY_CHARACTERS].x;
    out->w = kEditKeys[index - CP_KEY_CHARACTERS].w;
    out->kind = kEditKeys[index - CP_KEY_CHARACTERS].kind;
    out->y = Row(index) == 4 ? EDIT_ROW : ACTION_ROW;
    out->h = KEY_SIZE;
}

int CpKeyboard_Touch(int x, int y)
{
    int index;
    for (index = 0; index < CP_KEY_COUNT; ++index) {
        CpKey key;
        CpKeyboard_Key(NULL, index, &key);
        if (x >= key.x * 8 && x < (key.x + key.w) * 8 && y >= key.y * 8 && y < (key.y + key.h) * 8) return index;
    }
    return -1;
}

/* The first key of a row and how many it has. */
static void RowSpan(int row, int *first, int *count)
{
    if (row < 4) { *first = row * 10; *count = 10; }
    else if (row == 4) { *first = CP_KEY_CHARACTERS; *count = CP_KEY_INDEX_JOIN - CP_KEY_CHARACTERS; }
    else { *first = CP_KEY_INDEX_JOIN; *count = CP_KEY_COUNT - CP_KEY_INDEX_JOIN; }
}

int CpKeyboard_Move(int index, int dx, int dy)
{
    int row, first, count, i, best = -1, bestDistance = 1 << 30, centre;
    CpKey key;
    if (index < 0 || index >= CP_KEY_COUNT) return 0;
    row = Row(index);
    RowSpan(row, &first, &count);
    if (dx) return first + ((index - first + dx) % count + count) % count;
    if (!dy) return index;
    CpKeyboard_Key(NULL, index, &key);
    centre = key.x * 2 + key.w;
    row = ((row + dy) % ROWS + ROWS) % ROWS;
    RowSpan(row, &first, &count);
    for (i = first; i < first + count; ++i) {
        int distance;
        CpKeyboard_Key(NULL, i, &key);
        distance = key.x * 2 + key.w - centre;
        if (distance < 0) distance = -distance;
        if (distance < bestDistance) { bestDistance = distance; best = i; }
    }
    return best;
}

enum CpKeyKind CpKeyboard_Press(CpKeyboard *keyboard, int index)
{
    CpKey key;
    CpKeyboard_Key(keyboard, index, &key);
    switch (key.kind) {
    case CP_KEY_CHARACTER:
        if (key.character) CpKeyboard_Insert(keyboard, key.character);
        break;
    case CP_KEY_SHIFT:
        keyboard->layer = keyboard->layer == CP_KEYS_LOWER ? CP_KEYS_UPPER : CP_KEYS_LOWER;
        break;
    case CP_KEY_SYMBOLS:
        keyboard->layer = keyboard->layer == CP_KEYS_SYMBOLS ? CP_KEYS_LOWER : CP_KEYS_SYMBOLS;
        break;
    case CP_KEY_SPACE:
        CpKeyboard_Insert(keyboard, ' ');
        break;
    case CP_KEY_DELETE:
        CpKeyboard_Backspace(keyboard);
        break;
    case CP_KEY_SHOW:
        keyboard->shown = !keyboard->shown;
        break;
    default:
        break;
    }
    return (enum CpKeyKind)key.kind;
}
