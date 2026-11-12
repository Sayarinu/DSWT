#ifndef WONDER_TRADE_MENU_INPUT_H
#define WONDER_TRADE_MENU_INPUT_H

#define WT_MENU_LEFT 56
#define WT_MENU_TOP 40
#define WT_MENU_WIDTH 144
#define WT_MENU_HEIGHT 32
#define WT_MENU_SPACING 40
#define WT_MENU_MAX_ACTIONS 3

/* A new screen must see a release before accepting another touch. */
static int WtMenu_Touch(int *blocked, int pressed, int held, int x, int y, int count)
{
    int row;
    if (!held) {
        *blocked = 0;
        return -1;
    }
    if (*blocked || !pressed || x < WT_MENU_LEFT || x >= WT_MENU_LEFT + WT_MENU_WIDTH || y < WT_MENU_TOP) return -1;
    row = (y - WT_MENU_TOP) / WT_MENU_SPACING;
    if (row >= count || row >= WT_MENU_MAX_ACTIONS || (y - WT_MENU_TOP) % WT_MENU_SPACING >= WT_MENU_HEIGHT) return -1;
    return row;
}

#endif
