#include "ui/hello.h"

#include <stdio.h>

void CompanionHello_Show(CompanionScreens *screens)
{
    consoleSelect(&screens->top);
    consoleClear();
    printf("\n  DSWT Companion\n\n");
    printf("  Wonder Trade for Diamond, Pearl,\n");
    printf("  Platinum, HeartGold and\n");
    printf("  SoulSilver saves.\n\n");
    printf("  Running in %s mode.\n", isDSiMode() ? "DSi" : "DS");

    consoleSelect(&screens->bottom);
    consoleClear();
    printf("\n  Server: %s\n\n", WONDER_TRADE_AUTHORITY);
    printf("  Press START to exit.\n");
}
