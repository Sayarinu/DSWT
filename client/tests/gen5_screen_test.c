#include "wonder_trade/gen5_screen.h"
#include "../src/wonder_trade/bw2/text.h"

#include <assert.h>

/* Every Generation V screen fits: the windows inside the upper screen, the
 * party and Back on the lower one, and each text in the message window with
 * the lines the screen adds to it. */
int main(void)
{
    static const int labels[] = {
        WT_TEXT_TRADE, WT_TEXT_INFO, WT_TEXT_EXIT, WT_TEXT_RETRY, WT_TEXT_BACK, WT_TEXT_CANCEL, WT_TEXT_COLLECT,
        WT_TEXT_CANCEL_OFFER, WT_TEXT_CONFIRM_ACTION, WT_TEXT_RECOVERY_ACTION,
    };
    assert(WT_GEN5_TITLE_Y + WT_GEN5_TITLE_HEIGHT <= WT_GEN5_BAR_ROWS);
    assert(WT_GEN5_TITLE_X + WT_GEN5_TITLE_WIDTH <= WT_GEN5_SCREEN_COLUMNS);
    assert(WT_GEN5_MESSAGE_Y - 1 >= WT_GEN5_BAR_ROWS);
    assert(WT_GEN5_MESSAGE_Y + WT_GEN5_MESSAGE_HEIGHT + 1 <= WT_GEN5_SCREEN_ROWS);
    assert(WT_GEN5_MESSAGE_X >= 1 && WT_GEN5_MESSAGE_X + WT_GEN5_MESSAGE_WIDTH + 1 <= WT_GEN5_SCREEN_COLUMNS);
    assert(WT_GEN5_MESSAGE_LINES == 6);
    assert(WT_GEN5_BUTTON_X + WT_GEN5_BUTTON_WIDTH <= WT_GEN5_SCREEN_COLUMNS);
    assert(WT_GEN5_MAX_BUTTONS >= 6 + 1);
    for (int count = 1; count <= WT_GEN5_MAX_BUTTONS; ++count) {
        assert(WtGen5_ButtonTop(count) >= 0);
        assert(WtGen5_ButtonTop(count) + count * WT_GEN5_BUTTON_HEIGHT <= WT_GEN5_SCREEN_ROWS);
    }
    assert(WtGen5_ButtonTop(7) == 1 && WtGen5_ButtonTop(1) == 10);

    for (int text = 0; text < WT_TEXT_COUNT; ++text) {
        assert(WtGen5_TextLines(gWtText[text]) >= 1);
        assert(WtGen5_TextLines(gWtText[text]) <= WT_GEN5_MESSAGE_LINES);
    }
    /* The received Pokémon's name and trainer, and the support code, take a line each. */
    assert(WtGen5_TextLines(gWtText[WT_TEXT_COMPLETED]) + 2 <= WT_GEN5_MESSAGE_LINES);
    assert(WtGen5_TextLines(gWtText[WT_TEXT_CONNECTION_ERROR]) + 1 <= WT_GEN5_MESSAGE_LINES);
    assert(WtGen5_TextLines(gWtText[WT_TEXT_PING_ERROR]) + 1 <= WT_GEN5_MESSAGE_LINES);
    /* A button holds one line of up to 23 characters. */
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        int length = 0;
        assert(WtGen5_TextLines(gWtText[labels[i]]) == 1);
        while (gWtText[labels[i]][length]) length++;
        assert(length <= 23);
    }
    return 0;
}
