#include "wonder_trade/game_save.h"

#include <string.h>
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "heap.h"
#include "item.h"
#include "party.h"
#include "pc_boxes.h"
#include "pokemon.h"
#include "savedata/save_table.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static u32 Read32(const unsigned char *p) { return Read16(p) | ((u32)Read16(p + 2) << 16); }

static int Layout(const SaveData *save)
{
    return SAVE_BLOCK_ID_MAX == 2 && sizeof(SaveBlockFooter) == 20
        && save->blockInfo[0].offset == 0 && save->blockInfo[0].size == 53036
        && save->blockInfo[1].offset == 53036 && save->blockInfo[1].size == 74212;
}

static int Hash(const SaveData *save, unsigned char digest[32])
{
    mbedtls_sha256_context hash;
    unsigned block;
    int result = -1;
    if (!Layout(save)) return -1;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
    for (block = 0; block < 2; ++block) {
        const SaveBlockInfo *info = &save->blockInfo[block];
        if (mbedtls_sha256_update(&hash, save->body.data + info->offset, info->size - 20) != 0) goto done;
    }
    result = mbedtls_sha256_finish(&hash, digest);
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* Verify complete retail blocks independently of the in-RAM save selector.
 * The two alternating full saves have adjacent counters and identical bodies. */
static int Verify(const SaveData *save, const unsigned char expected[32])
{
    unsigned bank, block, offset, i, bit, size;
    unsigned char page[256], footer[20], digest[32];
    mbedtls_sha256_context hash;
    int result = -1;
    if (!Layout(save)) return -1;
    mbedtls_sha256_init(&hash);
    for (bank = 0; bank < 2; ++bank) {
        if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
        for (block = 0; block < 2; ++block) {
            const SaveBlockInfo *info = &save->blockInfo[block];
            unsigned crc = 65535;
            u32 address = bank * 0x40000 + info->offset;
            for (offset = 0; offset < info->size - 20; offset += size) {
                size = info->size - 20 - offset;
                if (size > sizeof(page)) size = sizeof(page);
                if (WtFlash_ReadRange(address + offset, page, size) != 0
                    || mbedtls_sha256_update(&hash, page, size) != 0) goto done;
                for (i = 0; i < size; ++i) {
                    crc ^= (unsigned)page[i] << 8;
                    for (bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 65535;
                }
            }
            if (WtFlash_ReadRange(address + info->size - 20, footer, sizeof(footer)) != 0
                || Read32(footer + 8) != info->size || Read32(footer + 12) != 0x20060623
                || footer[16] != block || Read16(footer + 18) != crc
                || (Read32(footer) != save->globalCounter && Read32(footer) + 1 != save->globalCounter)
                || (Read32(footer + 4) != save->blockCounters[block]
                    && Read32(footer + 4) + 1 != save->blockCounters[block])) goto done;
        }
        if (mbedtls_sha256_finish(&hash, digest) != 0 || memcmp(digest, expected, 32) != 0) goto done;
    }
    result = 0;
done:
    mbedtls_sha256_free(&hash);
    return result;
}

static int SaveBoth(WtGameSave *game, const unsigned char expected[32])
{
    unsigned char actual[32];
    int result = -1, i;
    if (Hash(game->save, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (Verify(game->save, expected) == 0) return 0;
    WtRecovery_InternalWrite(1);
    for (i = 0; i < 2; ++i) {
        SaveData_SetFullSaveRequired();
        if (SaveData_Save(game->save) != SAVE_RESULT_OK) goto done;
    }
    result = Verify(game->save, expected);
done:
    WtRecovery_InternalWrite(0);
    if (result != 0) WtRecovery_Block();
    return result;
}

int WtGameSave_Checkpoint(WtGameSave *game)
{
    unsigned char digest[32];
    if (Hash(game->save, digest) != 0) return -1;
    return Verify(game->save, digest) == 0 ? 0 : SaveBoth(game, digest);
}

static void Counters(WtGameSave *game, WtJournalEntry *entry)
{
    entry->globalCounter = game->save->globalCounter;
    entry->generalCounter = game->save->blockCounters[0];
    entry->boxCounter = game->save->blockCounters[1];
}

static int Eligible(Party *party, unsigned slot)
{
    int i, usable = 0;
    Pokemon *mon;
    if (party->capacity != 6 || party->currentCount < 2 || party->currentCount > 6
        || slot >= (unsigned)party->currentCount) return 0;
    mon = &party->pokemon[slot];
    if (!Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL)
        || Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL) > SPECIES_ARCEUS
        || Pokemon_GetValue(mon, MON_DATA_IS_EGG, NULL)
        || Pokemon_GetValue(mon, MON_DATA_SANITY_IS_EGG, NULL)
        || Pokemon_GetValue(mon, MON_DATA_BALL_CAPSULE_ID, NULL)
        || Item_IsMail(Pokemon_GetValue(mon, MON_DATA_HELD_ITEM, NULL))) return 0;
    for (i = 0; i < party->currentCount; ++i) {
        if ((unsigned)i != slot && !Pokemon_GetValue(&party->pokemon[i], MON_DATA_IS_EGG, NULL)
            && Pokemon_GetValue(&party->pokemon[i], MON_DATA_HP, NULL) != 0) ++usable;
    }
    return usable != 0;
}

int WtGameSave_Eligible(SaveData *save, unsigned slot)
{
    return Eligible(SaveData_GetParty(save), slot);
}

static int Absent(SaveData *save, const unsigned char outgoing[136])
{
    unsigned char canonical[136];
    Party *party = SaveData_GetParty(save);
    PCBoxes *boxes = SaveData_GetPCBoxes(save);
    int i, box;
    if (party->capacity != 6 || party->currentCount < 1 || party->currentCount > 6) return -1;
    for (i = 0; i < party->currentCount; ++i) {
        if (WtPokemon_Decode((const unsigned char *)&party->pokemon[i], canonical) != 0
            || memcmp(canonical, outgoing, 136) == 0) return -1;
    }
    for (box = 0; box < MAX_PC_BOXES; ++box) for (i = 0; i < MAX_MONS_PER_BOX; ++i) {
        BoxPokemon *mon = &boxes->boxMons[box][i];
        if (!BoxPokemon_GetValue(mon, MON_DATA_SPECIES_EXISTS, NULL)) continue;
        if (WtPokemon_Decode((const unsigned char *)mon, canonical) != 0
            || memcmp(canonical, outgoing, 136) == 0) return -1;
    }
    return 0;
}

static int Removed(WtGameSave *game, const WtJournalEntry *entry)
{
    unsigned char canonical[136];
    if (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon)
        || WtPokemon_Decode(entry->original, canonical) != 0
        || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0) return -1;
    /* A selected older retail block cannot be promoted to a new checkpoint. */
    if ((u32)(game->save->globalCounter - entry->globalCounter) >= 0x80000000
        || (u32)(game->save->blockCounters[0] - entry->generalCounter) >= 0x80000000
        || (u32)(game->save->blockCounters[1] - entry->boxCounter) >= 0x80000000
        || Absent(game->save, entry->outgoing) != 0) return -1;
    return WtGameSave_Checkpoint(game);
}

int WtGameSave_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGameSave *game = arg;
    SaveData *candidate = NULL;
    Party *party = SaveData_GetParty(game->save), *copy;
    unsigned char digest[32], canonical[136];
    int result = -1, i;
    if (!Layout(game->save)) return -1;
    if (step != WT_INVENTORY_PREPARE_REMOVE
        && (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon)
            || WtPokemon_Decode(entry->original, canonical) != 0
            || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0)) return -1;
    if (step == WT_INVENTORY_VERIFY_REMOVED) return Removed(game, entry);
    /* Collection is enabled by the separate receive integration. Keep any
     * authenticated delivery pending without applying it or acknowledging. */
    if (step == WT_INVENTORY_PREPARE_RECEIVE) return 1;
    if (step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE && step != WT_INVENTORY_RESTORE) return -1;
    candidate = Heap_Alloc(game->heapID, sizeof(*candidate));
    if (!candidate) return -1;
    memcpy(candidate, game->save, sizeof(*candidate)); copy = SaveData_GetParty(candidate);
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (!Eligible(copy, slot) || WtPokemon_Decode((const unsigned char *)&copy->pokemon[slot], canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = slot; entry->source.length = sizeof(Pokemon);
        memcpy(entry->original, &copy->pokemon[slot], sizeof(Pokemon));
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        if (WtFlash_Hash(entry->original, sizeof(Pokemon), entry->originalDigest) != 0
            || Hash(candidate, entry->inventoryBefore) != 0) goto done;
        Party_RemovePokemonBySlotIndex(copy, slot);
        if (Absent(candidate, entry->outgoing) != 0 || Hash(candidate, entry->inventoryAfter) != 0) goto done;
        Counters(game, entry); result = 0; goto done;
    }
    if (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon) || slot > 5
        || Hash(candidate, digest) != 0) goto done;
    if (step == WT_INVENTORY_REMOVE) {
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
            if (slot >= (unsigned)copy->currentCount || memcmp(&copy->pokemon[slot], entry->original, sizeof(Pokemon)) != 0) goto done;
            Party_RemovePokemonBySlotIndex(copy, slot);
            if (Hash(candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            if (copy->currentCount >= 6 || slot > (unsigned)copy->currentCount) goto done;
            for (i = copy->currentCount; i > (int)slot; --i) copy->pokemon[i] = copy->pokemon[i - 1];
            memcpy(&copy->pokemon[slot], entry->original, sizeof(Pokemon)); ++copy->currentCount;
            if (Hash(candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryBefore, 32) != 0) goto done;
    }
    memcpy(party, copy, sizeof(*party));
    if (SaveBoth(game, digest) != 0) goto done;
    Counters(game, entry); result = 0;
done:
    mbedtls_platform_zeroize(candidate, sizeof(*candidate)); Heap_Free(candidate);
    return result;
}

int WtGameSave_CanLeave(WtGameSave *game, const WtJournalEntry *entry)
{
    if (entry->phase == WT_JOURNAL_EMPTY || entry->phase == WT_JOURNAL_IDLE) return 1;
    if (entry->phase == WT_JOURNAL_QUEUED || entry->phase == WT_JOURNAL_RECEIVED) return Removed(game, entry) == 0;
    return 0;
}

int WtGameSave_Boot(WtGameSave *game, WtJournalIO io)
{
    WtJournal journal;
    WtJournalEntry entry;
    int result = -1;
    if (WtJournal_Open(&journal, io) != 0) return -1;
    if (WtJournal_Read(&journal, &entry) == 0 && entry.generation != UINT64_MAX
        && WtGameSave_CanLeave(game, &entry) && WtJournal_Repair(&journal) == 0) {
        WtRecovery_Allow(entry.phase); result = 0;
    }
    mbedtls_platform_zeroize(&entry, sizeof(entry)); WtJournal_Close(&journal);
    return result;
}
