/* Included only by the separate local test-ROM builder, never the game build. */
#include <string.h>
#include "savedata.h"
#include "system.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/recovery.h"

static int WtRecovery_ProbeEntrypoints(void)
{
    SaveData *save = SaveData_Ptr();
    SaveDataState originalState = save->state, blockedState;
    unsigned char before[32], after[32];
    u8 sleepFlags = gSystem.inhibitSleep;
    u32 scratch = 0;
    int passed = 1;
    WtRecovery_Block();
    if (WtFlash_Hash((const unsigned char *)save, sizeof(*save), before) != 0) return 0;
    passed &= SaveData_Save(save) == SAVE_RESULT_CORRUPT;
    passed &= SaveData_SaveBlock(save, SAVE_BLOCK_ID_NORMAL) == SAVE_RESULT_CORRUPT;
    passed &= SaveData_Erase(save) == FALSE;
    SaveData_Clear(save);
    passed &= SaveData_CardSave(0, &scratch, sizeof(scratch)) == FALSE;
    SaveData_SaveStateInit(save, SAVE_BLOCK_ID_MAX);
    passed &= SaveData_SaveStateMain(save) == SAVE_RESULT_CORRUPT;
    SaveData_SaveStateCancel(save);
    blockedState = originalState; blockedState.wonderTradeBlocked = TRUE;
    passed &= memcmp(&save->state, &blockedState, sizeof(blockedState)) == 0;
    save->state = originalState;
    passed &= gSystem.inhibitSleep == sleepFlags;
    passed &= WtFlash_Hash((const unsigned char *)save, sizeof(*save), after) == 0;
    passed &= memcmp(before, after, 32) == 0;
    return passed;
}
