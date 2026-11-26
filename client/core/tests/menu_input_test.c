#include "wonder_trade/menu_input.h"

#include <assert.h>

int main(void)
{
    int blocked = 1;
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 50, 3) == -1);
    assert(WtMenu_Touch(&blocked, 0, 0, 100, 50, 3) == -1);
    assert(blocked == 0);
    for (int row = 0; row < 3; row++) {
        int top = WT_MENU_TOP + row * WT_MENU_SPACING;
        assert(WtMenu_Touch(&blocked, 1, 1, WT_MENU_LEFT, top, 3) == row);
        assert(WtMenu_Touch(&blocked, 1, 1, WT_MENU_LEFT + WT_MENU_WIDTH - 1, top + WT_MENU_HEIGHT - 1, 3) == row);
        assert(WtMenu_Touch(&blocked, 1, 1, 100, top + WT_MENU_HEIGHT, 3) == -1);
        assert(WtMenu_Touch(&blocked, 1, 1, 100, top, row) == -1);
    }
    assert(WtMenu_Touch(&blocked, 0, 1, 100, 50, 3) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 55, 50, 3) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 200, 50, 3) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 39, 3) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 191, 3) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 50, 0) == -1);
    blocked = 1; /* Entering confirmation while the stylus remains down. */
    assert(WtMenu_Touch(&blocked, 0, 1, 100, 50, 2) == -1);
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 50, 2) == -1);
    WtMenu_Touch(&blocked, 0, 0, 100, 50, 2);
    assert(WtMenu_Touch(&blocked, 1, 1, 100, 50, 2) == 0);
    return 0;
}
