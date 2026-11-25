#ifndef COMPANION_UI_KEYBOARD_H
#define COMPANION_UI_KEYBOARD_H

#include <stdint.h>

/* The Wi-Fi password keyboard on the bottom screen: four rows of ten
 * character keys in three layers (lower case, upper case, symbols), a row of
 * Shift, Symbols, Space, Delete and Show/Hide, and the Connect and Back
 * buttons. Every key is a box of whole tiles, drawn with the screens' button
 * tiles; the stylus presses a key, the D-pad moves between keys and A presses
 * the highlighted one. The text holds printable ASCII only (passphrases are
 * ASCII); Delete removes a whole UTF-8 character all the same. */

#define CP_KEYBOARD_TEXT_MAX 64

enum CpKeyKind {
    CP_KEY_CHARACTER, CP_KEY_SHIFT, CP_KEY_SYMBOLS, CP_KEY_SPACE, CP_KEY_DELETE,
    CP_KEY_SHOW, CP_KEY_JOIN, CP_KEY_BACK
};
enum { CP_KEYS_LOWER, CP_KEYS_UPPER, CP_KEYS_SYMBOLS };

/* Keys by index: 40 character keys row by row, then the rest in this order. */
enum {
    CP_KEY_CHARACTERS = 40,
    CP_KEY_INDEX_SHIFT = 40, CP_KEY_INDEX_SYMBOLS, CP_KEY_INDEX_SPACE, CP_KEY_INDEX_DELETE,
    CP_KEY_INDEX_SHOW, CP_KEY_INDEX_JOIN, CP_KEY_INDEX_BACK, CP_KEY_COUNT
};

typedef struct CpKey {
    uint8_t x, y, w, h; /* in tiles on the bottom screen */
    uint8_t kind;       /* enum CpKeyKind */
    char character;     /* CHARACTER keys: what it types in the layer, 0 for a blank key */
} CpKey;

typedef struct CpKeyboard {
    char text[CP_KEYBOARD_TEXT_MAX + 1];
    unsigned length, max; /* bytes typed, and at most */
    uint8_t layer;        /* CP_KEYS_* */
    uint8_t shown;        /* show the text rather than dots */
    int cursor;           /* the highlighted key */
} CpKeyboard;

/* Empty, lower case, hidden, the cursor on the first key. */
void CpKeyboard_Reset(CpKeyboard *, unsigned max);
/* Wipe the text: it may hold a password. */
void CpKeyboard_Clear(CpKeyboard *);
/* Add a printable ASCII character; nonzero when it is refused (not
 * printable ASCII, or the text is full). */
int CpKeyboard_Insert(CpKeyboard *, int character);
/* Remove the last character, all its UTF-8 bytes; nonzero when empty. */
int CpKeyboard_Backspace(CpKeyboard *);

/* Key `index` as the current layer draws it. */
void CpKeyboard_Key(const CpKeyboard *, int index, CpKey *out);
/* The key under a touch, or -1. */
int CpKeyboard_Touch(int x, int y);
/* The key the D-pad reaches from `index`: dx -1/1 along its row (wrapping),
 * dy -1/1 to the nearest key in the row above or below (wrapping). */
int CpKeyboard_Move(int index, int dx, int dy);
/* Press a key: types, edits or switches the layer. Returns its kind, for
 * Join and Back to be handled by the caller. */
enum CpKeyKind CpKeyboard_Press(CpKeyboard *, int index);

#endif
