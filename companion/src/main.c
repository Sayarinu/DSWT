#include <nds.h>

#include "platform/nds/screens.h"
#include "ui/hello.h"

int main(void)
{
    CompanionScreens screens;
    CompanionScreens_Init(&screens);
    CompanionHello_Show(&screens);
    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) return 0;
    }
}
