#ifndef WONDER_TRADE_GEN5_SCREEN_H
#define WONDER_TRADE_GEN5_SCREEN_H

/* The layout of Wonder Trade's screen in Black, White, Black 2 and White 2,
 * in 8-pixel tiles, as the Global Trade Station lays out its own
 * (worldtrade_title.c): the station's title bar across the top of the upper
 * screen, with the title in it, and a message window with the game's frame
 * under it; the actions on the lower screen, as a column of the game's task
 * menu buttons (app_taskmenu.c), which the station uses for its own choices. */

/* The title, in the title bar's first four rows. */
#define WT_GEN5_BAR_ROWS 4
#define WT_GEN5_TITLE_X 2
#define WT_GEN5_TITLE_Y 1
#define WT_GEN5_TITLE_WIDTH 28
#define WT_GEN5_TITLE_HEIGHT 2

/* The message window, its frame one tile around it: the status, up to six
 * lines of 16 pixels in the game's font, 240 pixels wide. */
#define WT_GEN5_MESSAGE_X 1
#define WT_GEN5_MESSAGE_Y 6
#define WT_GEN5_MESSAGE_WIDTH 30
#define WT_GEN5_MESSAGE_HEIGHT 12
#define WT_GEN5_LINE_HEIGHT 16
#define WT_GEN5_MESSAGE_LINES (WT_GEN5_MESSAGE_HEIGHT * 8 / WT_GEN5_LINE_HEIGHT)

/* The buttons, centered on the lower screen: up to six party Pokémon and Back. */
#define WT_GEN5_SCREEN_ROWS 24
#define WT_GEN5_SCREEN_COLUMNS 32
#define WT_GEN5_BUTTON_X 4
#define WT_GEN5_BUTTON_WIDTH 24
#define WT_GEN5_BUTTON_HEIGHT 3
#define WT_GEN5_MAX_BUTTONS 7

/* The row of the first of count buttons. */
static inline int WtGen5_ButtonTop(int count)
{
    return (WT_GEN5_SCREEN_ROWS - count * WT_GEN5_BUTTON_HEIGHT) / 2;
}

/* The lines of a text, which \n separates. */
static inline int WtGen5_TextLines(const unsigned short *text)
{
    int lines = *text ? 1 : 0;
    for (; *text; ++text) lines += *text == '\n';
    return lines;
}

#endif
