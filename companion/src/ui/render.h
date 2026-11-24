#ifndef COMPANION_UI_RENDER_H
#define COMPANION_UI_RENDER_H

#include <stdint.h>

#include "ui/canvas.h"
#include "ui/ui.h"

/* How a state looks, without touching the hardware: each screen is a text
 * layer (a CpCanvas whose every cell is its own tile) over a layer of
 * project-original tiles (res/gfx) over the striped backdrop. The DS front end
 * copies these to VRAM; host tests composite them to check the layout.
 *
 * Layers, front to back: 0 text, 1 windows, cards, buttons and icons,
 * 2 the backdrop (CP_TILE_STRIPE everywhere, scrolled by the front end). */

/* Tiles of the layer-1/2 tileset, as the front end loads them: ui_frame.png
 * at 0, ui_button.png at CP_TILE_BUTTON, ui_icons.png at CP_TILE_ICON and
 * ui_spinner.png at CP_TILE_SPINNER. */
enum {
    CP_TILE_BLANK = 0, CP_TILE_STRIPE = 1,
    CP_TILE_HEADER_TOP = 2, CP_TILE_HEADER = 3, CP_TILE_HEADER_EDGE = 4,
    CP_TILE_FRAME = 5,  /* nine tiles: corners, edges and centre, row by row */
    CP_TILE_CARD = 14,  /* nine tiles */
    CP_TILE_PREVIEW = 23, CP_TILE_CARD_LINE = 24,
    CP_TILE_BUTTON = 32, /* nine tiles */
    CP_TILE_ICON = 48,   /* item, male, female, shiny, cursor */
    CP_TILE_SPINNER = 56 /* 2x2 */
};
enum { CP_ICON_ITEM, CP_ICON_MALE, CP_ICON_FEMALE, CP_ICON_SHINY, CP_ICON_CURSOR };

/* Background palette slots, as application.c numbers the game's. */
enum {
    CP_PAL_TEXT = 0,
    CP_PAL_BUTTON = 1,    /* application.c buttonPalette */
    CP_PAL_SELECTED = 3,  /* application.c selectedPalette */
    CP_PAL_DISABLED = 5,  /* a grey button, for party Pokémon that cannot be offered */
    CP_PAL_BACKDROP = 9,  /* application.c backgroundPalette: backdrop, windows, cards */
    CP_PAL_ICON = 10,
    CP_PAL_SPINNER = 12   /* entries 1-8 are the spinner's dots, turned by the front end */
};
/* Every palette slot's 16 colours (BGR555), the same on both screens. */
extern const uint16_t CpRender_Palettes[16][16];

/* Colours of the text layer (CP_PAL_TEXT). */
enum {
    CP_INK_DARK = 1, CP_INK_SHADOW = 2, CP_INK_WHITE = 3, CP_INK_BLUE = 4,
    CP_INK_GREY = 5, CP_INK_GREY_SHADOW = 6, CP_INK_CURSOR = 7, CP_INK_FILL = 15
};

#define CP_MAP_SIZE (32 * 32)

typedef struct CpScreen {
    CpCanvas text;
    uint16_t map[CP_MAP_SIZE]; /* layer 1: tile | flip bits | palette << 12 */
} CpScreen;

/* Draw both screens for the UI's current state. */
void CpRender_Draw(const CpUi *, const CpUiModel *, CpScreen *top, CpScreen *bottom);

/* Where the spinner sits on the bottom screen (tiles), shown while CpUi_Busy. */
#define CP_SPINNER_X 15
#define CP_SPINNER_Y 18

#endif
