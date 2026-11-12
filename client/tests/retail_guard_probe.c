/* Included only by the separate local test-ROM builder, never the game build. */
#include <string.h>
#include "savedata.h"
#include "system.h"
#include "heap.h"
#include "mail.h"
#include "party.h"
#include "pokemon.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/game_save.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"

static int WtRecovery_ProbeReceiveImage(void)
{
    SaveData *save = SaveData_Ptr();
    Party *party = SaveData_GetParty(save);
    WtGameSave game = { save, HEAP_ID_WONDER_TRADE };
    WtJournalEntry *entries;
    Pokemon recorded;
    WtReceivedPokemon received;
    Mail mail, *poison;
    unsigned char before[32], after[32];
    unsigned pass, i;
    int passed = 0;
    /* Preparation uses temporary save copies and never writes retail/journal data. */
    if (party->currentCount < 1 || party->currentCount >= 6) return 0;
    entries = Heap_Alloc(HEAP_ID_WONDER_TRADE, sizeof(*entries) * 2);
    if (!entries) return 0;
    if (WtFlash_Hash((const unsigned char *)save, sizeof(*save), before) != 0) goto done;
    for (pass = 0; pass < 2; ++pass) {
        WtJournalEntry *entry = &entries[pass];
        memset(entry, 0, sizeof(*entry));
        entry->phase = WT_JOURNAL_RECEIVED;
        entry->deliveryKind = WT_DELIVERY_MATCH;
        entry->source.kind = 1; entry->source.length = sizeof(Pokemon);
        memcpy(entry->original, &party->pokemon[0], sizeof(Pokemon));
        if (WtPokemon_Decode(entry->original, entry->outgoing) != 0) goto done;
        memcpy(entry->incoming, entry->outgoing, sizeof(entry->incoming));
        poison = Heap_AllocAtEnd(HEAP_ID_SYSTEM, sizeof(*poison));
        if (!poison) goto done;
        memset(poison, pass ? 0x55 : 0xaa, sizeof(*poison));
        Heap_Free(poison);
        if (WtGameSave_Inventory(&game, WT_INVENTORY_PREPARE_RECEIVE, entry, 0) != 0
            || entry->destination.kind != 1) goto done;
        memcpy(&recorded, entry->destinationAfter, sizeof(recorded));
        if (WtGameSave_Received(entry, &received) != 0
            || received.species != Pokemon_GetValue(&recorded, MON_DATA_SPECIES, NULL)
            || received.gender != Pokemon_GetGender(&recorded)
            || received.form != Pokemon_GetForm(&recorded)
            || received.shiny != Pokemon_IsShiny(&recorded)
            || received.personality != Pokemon_GetValue(&recorded, MON_DATA_PERSONALITY, NULL)) goto done;
        Pokemon_GetValue(&recorded, MON_DATA_MAIL, &mail);
        for (i = 0; i < MAIL_MAX_SENTENCES; ++i) if (mail.sentences[i].id != 0) goto done;
    }
    passed = memcmp(entries[0].destinationAfter, entries[1].destinationAfter, sizeof(Pokemon)) == 0
        && memcmp(entries[0].appliedDigest, entries[1].appliedDigest, 32) == 0
        && memcmp(entries[0].inventoryAfter, entries[1].inventoryAfter, 32) == 0
        && WtFlash_Hash((const unsigned char *)save, sizeof(*save), after) == 0
        && memcmp(before, after, 32) == 0;
done:
    Heap_Free(entries);
    return passed;
}

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
    passed &= WtRecovery_ProbeReceiveImage();
    return passed;
}
