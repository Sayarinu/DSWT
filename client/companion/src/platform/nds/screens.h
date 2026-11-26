#ifndef COMPANION_PLATFORM_NDS_SCREENS_H
#define COMPANION_PLATFORM_NDS_SCREENS_H

#include <nds.h>

/* A text console on each screen. */
typedef struct CompanionScreens {
    PrintConsole top, bottom;
} CompanionScreens;

void CompanionScreens_Init(CompanionScreens *);

#endif
