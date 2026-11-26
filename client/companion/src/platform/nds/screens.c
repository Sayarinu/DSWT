#include "platform/nds/screens.h"

void CompanionScreens_Init(CompanionScreens *screens)
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&screens->top, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&screens->bottom, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
}
