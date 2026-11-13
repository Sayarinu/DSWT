#include "wonder_trade/boot.h"

#include <mbedtls/platform_util.h>

#include "wonder_trade/engine.h"
#include "wonder_trade/recovery.h"

/* Boot never sends anything, so it has no use for request IDs. */
static int NoRandom(void *arg, unsigned char *bytes, size_t size)
{
    (void)arg; (void)bytes; (void)size;
    return -1;
}

/* Phases with a local step left: an interrupted removal, a rejected offer
 * waiting to return, and a collected delivery not yet applied. */
static int Local(unsigned phase)
{
    return phase == WT_JOURNAL_ESCROW || phase == WT_JOURNAL_RESTORING
        || phase == WT_JOURNAL_RECEIVED || phase == WT_JOURNAL_APPLYING;
}

static int Settle(WtGame *game, WtJournalIO io)
{
    WtEngineIO local = { game, NoRandom, WtGame_Inventory, WtGame_CanHold };
    WtEngine *engine = WtGame_Alloc(game, sizeof(*engine));
    int result = -1;
    if (!engine) return -1;
    if (WtEngine_Open(engine, io, local) == 0) result = WtEngine_Settle(engine) < 0 ? -1 : 0;
    WtEngine_Close(engine); WtGame_Free(game, engine);
    return result;
}

int WtBoot_Check(WtGame *game, WtJournalIO io)
{
    WtJournal journal;
    WtJournalEntry entry;
    int result, settled = 0;
    if (WtRecovery_Check(io) == 0) return 0;
    for (;;) {
        result = -1;
        if (WtJournal_Open(&journal, io) != 0) return -1;
        if (WtJournal_Read(&journal, &entry) == 0 && entry.generation != UINT64_MAX) {
            if (Local(entry.phase) && !settled) result = 1;
            else if (WtGame_CanLeave(game, &entry) && WtJournal_Repair(&journal) == 0) {
                WtRecovery_Allow(entry.phase); result = 0;
            }
        }
        mbedtls_platform_zeroize(&entry, sizeof(entry)); WtJournal_Close(&journal);
        if (result != 1) return result;
        if (Settle(game, io) != 0) return -1;
        settled = 1;
    }
}
