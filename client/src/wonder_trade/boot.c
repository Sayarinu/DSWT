#include "wonder_trade/boot.h"

#include <mbedtls/platform_util.h>

#include "wonder_trade/recovery.h"

int WtBoot_Check(WtGame *game, WtJournalIO io)
{
    WtJournal journal;
    WtJournalEntry entry;
    int result;
    if (WtRecovery_Check(io) == 0) return 0;
    result = -1;
    if (WtJournal_Open(&journal, io) != 0) return -1;
    if (WtJournal_Read(&journal, &entry) == 0 && entry.generation != UINT64_MAX
        && WtGame_CanLeave(game, &entry) && WtJournal_Repair(&journal) == 0) {
        WtRecovery_Allow(entry.phase); result = 0;
    }
    mbedtls_platform_zeroize(&entry, sizeof(entry)); WtJournal_Close(&journal);
    return result;
}
