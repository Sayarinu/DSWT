#include "wonder_trade/hgss/game_save.h"

#include <stddef.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "constants/balls.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/species.h"

#include "game_stats.h"
#include "heap.h"
#include "item.h"
#include "mail.h"
#include "party.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "save_arrays.h"
#include "update_dex_received.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"

/* Save_WriteManInit copies these RAM shadows into the save body before every
 * write (src/save.c); syncing first makes the body in RAM what is written. */
extern void sub_0202C714(SaveData *saveData);
extern void Save_Frontier_Commit(SaveData *saveData);

#define FOOTER_SIZE 16
#define GENERAL_SIZE 0xF628
#define BOXES_OFFSET 0xF700
#define BOXES_SIZE 0x12310
/* PCStorage.boxModifiedFlag: the save's own record of which boxes to write
 * next. Both halves legitimately differ there, so digests read it as zero. */
#define BOX_FLAG (BOXES_OFFSET + 0x12004)
/* Every Pokémon gets the same starting friendship as Platinum's receive path. */
#define RECEIVED_FRIENDSHIP 70

static const struct { u32 offset, size; } sBlocks[2] = {
    { 0, GENERAL_SIZE }, { BOXES_OFFSET, BOXES_SIZE },
};

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static u32 Read32(const unsigned char *p) { return Read16(p) | ((u32)Read16(p + 2) << 16); }

int WtGameSave_LayoutKnown(const SaveData *save)
{
    int i;
    if (SAVE_BLOCK_NUM != 42 || SAVE_PAGE_MAX != 35 || SAVE_SECTOR_SIZE != 0x1000
        || sizeof(struct SaveChunkFooter) != FOOTER_SIZE || sizeof(PCStorage) != 0x122FC
        || offsetof(PCStorage, boxModifiedFlag) != 0x12004
        || sizeof(Pokemon) != 236 || sizeof(BoxPokemon) != 136 || sizeof(PC_BOX) != 0x1000
        || save->arrayHeaders[SAVE_PCSTORAGE].offset != BOXES_OFFSET
        || save->arrayHeaders[SAVE_PARTY].offset != 0x90 || gNumExtraSaveChunkHeaders != 6) return 0;
    for (i = 0; i < 2; ++i) {
        if (save->saveSlotSpecs[i].offset != sBlocks[i].offset || save->saveSlotSpecs[i].size != sBlocks[i].size) return 0;
    }
    /* Every extra chunk and its mirror (+0x40000) ends before a journal bank. */
    for (i = 0; i < gNumExtraSaveChunkHeaders; ++i) {
        const struct ExtraSaveChunkHeader *chunk = &gExtraSaveChunkHeaders[i];
        if (chunk->sector * SAVE_SECTOR_SIZE + chunk->sizeFunc() + FOOTER_SIZE > gWtGameConfig.journalBank[0]) return 0;
    }
    return gWtGameConfig.journalBank[0] == 0x30000 && gWtGameConfig.journalBank[1] == 0x70000;
}

static void Sync(SaveData *save)
{
    sub_0202C714(save);
    Save_Frontier_Commit(save);
}

/* Hash body bytes [offset, offset + size) of a block as the digest sees them. */
static int Update(mbedtls_sha256_context *hash, const u8 *bytes, u32 offset, u32 size)
{
    u8 masked[4];
    u32 start = offset, end = offset + size;
    if (end <= BOX_FLAG || start >= BOX_FLAG + 4) return mbedtls_sha256_update(hash, bytes, size);
    if (start < BOX_FLAG && mbedtls_sha256_update(hash, bytes, BOX_FLAG - start) != 0) return -1;
    for (offset = start < BOX_FLAG ? BOX_FLAG : start; offset < end && offset < BOX_FLAG + 4; ++offset) {
        masked[0] = 0;
        if (mbedtls_sha256_update(hash, masked, 1) != 0) return -1;
    }
    if (end > BOX_FLAG + 4) return mbedtls_sha256_update(hash, bytes + (BOX_FLAG + 4 - start), end - BOX_FLAG - 4);
    return 0;
}

static int Hash(const SaveData *save, unsigned char digest[32])
{
    mbedtls_sha256_context hash;
    unsigned block;
    int result = -1;
    if (!WtGameSave_LayoutKnown(save)) return -1;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
    for (block = 0; block < 2; ++block) {
        if (Update(&hash, save->dynamic_region + sBlocks[block].offset, sBlocks[block].offset,
                sBlocks[block].size - FOOTER_SIZE) != 0) goto done;
    }
    result = mbedtls_sha256_finish(&hash, digest);
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* Verify both complete halves on flash independently of the RAM selector:
 * footers, CRCs and the masked body digest. The two consecutive saves leave
 * counters of saveCounter - 1 and saveCounter. */
static int Verify(const SaveData *save, const unsigned char expected[32])
{
    unsigned half, block, offset, i, bit, size;
    unsigned char page[256], footer[FOOTER_SIZE], digest[32];
    mbedtls_sha256_context hash;
    int result = -1;
    if (!WtGameSave_LayoutKnown(save)) return -1;
    mbedtls_sha256_init(&hash);
    for (half = 0; half < 2; ++half) {
        u32 counter = 0;
        if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
        for (block = 0; block < 2; ++block) {
            unsigned crc = 65535;
            u32 body = sBlocks[block].size - FOOTER_SIZE, address = half * 0x40000 + sBlocks[block].offset;
            for (offset = 0; offset < body; offset += size) {
                size = body - offset;
                if (size > sizeof(page)) size = sizeof(page);
                if (WtFlash_ReadRange(address + offset, page, size) != 0) goto done;
                for (i = 0; i < size; ++i) {
                    crc ^= (unsigned)page[i] << 8;
                    for (bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 65535;
                }
                if (Update(&hash, page, sBlocks[block].offset + offset, size) != 0) goto done;
            }
            /* count, size, magic, block u16, CRC u16 */
            if (WtFlash_ReadRange(address + body, footer, sizeof(footer)) != 0
                || Read32(footer + 4) != sBlocks[block].size || Read32(footer + 8) != SAVE_CHUNK_MAGIC
                || Read16(footer + 12) != block || Read16(footer + 14) != crc
                || (Read32(footer) != save->saveCounter && Read32(footer) + 1 != save->saveCounter)
                || (block == 1 && Read32(footer) != counter)) goto done;
            counter = Read32(footer);
        }
        if (mbedtls_sha256_finish(&hash, digest) != 0 || memcmp(digest, expected, 32) != 0) goto done;
    }
    result = 0;
done:
    mbedtls_sha256_free(&hash);
    return result;
}

BOOL WtGameSave_Written(const SaveData *save)
{
    return !save->isNewGame && save->saveFileExists;
}

/* HeartGold and SoulSilver write only changed PC boxes. Marking every box
 * modified before each of two consecutive saves writes a full image to both
 * halves. */
static int SaveBoth(WtGame *game, const unsigned char expected[32])
{
    unsigned char actual[32];
    int result = -1, i;
    if (Hash(game->save, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (Verify(game->save, expected) == 0) return 0;
    if (game->save->isNewGame || !game->save->saveFileExists) return -1;
    WtRecovery_InternalWrite(1);
    for (i = 0; i < 2; ++i) {
        Save_SetAllPCBoxesModified(game->save);
        if (SaveGameNormal(game->save) != WRITE_STATUS_SUCCESS) goto done;
    }
    result = Verify(game->save, expected);
done:
    WtRecovery_InternalWrite(0);
    if (result != 0) WtRecovery_Block();
    return result;
}

int WtGame_Checkpoint(WtGame *game)
{
    unsigned char digest[32];
    Sync(game->save);
    if (Hash(game->save, digest) != 0) return -1;
    return Verify(game->save, digest) == 0 ? 0 : SaveBoth(game, digest);
}

/* One save counter covers both blocks of a half. */
static void Counters(WtGame *game, WtJournalEntry *entry)
{
    entry->globalCounter = game->save->saveCounter;
    entry->generalCounter = game->save->saveCounter;
    entry->boxCounter = game->save->saveCounter;
}

static int AprijuiceClear(const Party *party, unsigned slot)
{
    const u8 *modifiers = (const u8 *)&party->extra.aprijuiceModifiers[slot];
    unsigned i;
    for (i = 0; i < sizeof(party->extra.aprijuiceModifiers[slot]); ++i) if (modifiers[i]) return 0;
    return 1;
}

static int Eligible(Party *party, unsigned slot)
{
    int i, usable = 0;
    Pokemon *mon;
    if (party->core.maxCount != PARTY_SIZE || party->core.curCount < 2 || party->core.curCount > PARTY_SIZE
        || slot >= (unsigned)party->core.curCount) return 0;
    mon = &party->core.mons[slot];
    /* The journal keeps only the Pokémon image, so a restore cannot bring back
     * the slot's Aprijuice modifiers; such a Pokémon is not offered. */
    if (!GetMonData(mon, MON_DATA_SPECIES, NULL)
        || GetMonData(mon, MON_DATA_SPECIES, NULL) > NATIONAL_DEX_COUNT
        || GetMonData(mon, MON_DATA_IS_EGG, NULL)
        || GetMonData(mon, MON_DATA_SANITY_IS_EGG, NULL)
        || GetMonData(mon, MON_DATA_BALL_CAPSULE_ID, NULL)
        || ItemIdIsMail((u16)GetMonData(mon, MON_DATA_HELD_ITEM, NULL))
        || (GetMonData(mon, MON_DATA_SPECIES, NULL) == SPECIES_PICHU && GetMonData(mon, MON_DATA_FORM, NULL) != 0)
        || !AprijuiceClear(party, slot)) return 0;
    for (i = 0; i < party->core.curCount; ++i) {
        if ((unsigned)i != slot && !GetMonData(&party->core.mons[i], MON_DATA_IS_EGG, NULL)
            && GetMonData(&party->core.mons[i], MON_DATA_HP, NULL) != 0) ++usable;
    }
    return usable != 0;
}

int WtGameSave_Eligible(SaveData *save, unsigned slot)
{
    return Eligible(SaveArray_Party_Get(save), slot);
}

static int Absent(SaveData *save, const unsigned char outgoing[136])
{
    unsigned char canonical[136];
    Party *party = SaveArray_Party_Get(save);
    PCStorage *storage = SaveArray_PCStorage_Get(save);
    int i, box;
    if (party->core.maxCount != PARTY_SIZE || party->core.curCount < 1 || party->core.curCount > PARTY_SIZE) return -1;
    for (i = 0; i < party->core.curCount; ++i) {
        if (WtPokemon_Decode((const unsigned char *)&party->core.mons[i], canonical) != 0
            || memcmp(canonical, outgoing, 136) == 0) return -1;
    }
    for (box = 0; box < NUM_BOXES; ++box) for (i = 0; i < MONS_PER_BOX; ++i) {
        BoxPokemon *mon = &storage->boxes[box].mons[i];
        if (!GetBoxMonData(mon, MON_DATA_SPECIES_EXISTS, NULL)) continue;
        if (WtPokemon_Decode((const unsigned char *)mon, canonical) != 0
            || memcmp(canonical, outgoing, 136) == 0) return -1;
    }
    return 0;
}

static int Removed(WtGame *game, const WtJournalEntry *entry)
{
    unsigned char canonical[136];
    if (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon)
        || WtPokemon_Decode(entry->original, canonical) != 0
        || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0) return -1;
    /* A selected older retail half cannot be promoted to a new checkpoint. */
    if ((u32)(game->save->saveCounter - entry->globalCounter) >= 0x80000000
        || Absent(game->save, entry->outgoing) != 0) return -1;
    return WtGame_Checkpoint(game);
}

static unsigned char *Destination(SaveData *save, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < PARTY_SIZE && where.length == sizeof(Pokemon))
        return (unsigned char *)&SaveArray_Party_Get(save)->core.mons[where.slot];
    if (where.kind == 2 && where.box < NUM_BOXES && where.slot < MONS_PER_BOX && where.length == sizeof(BoxPokemon))
        return (unsigned char *)&SaveArray_PCStorage_Get(save)->boxes[where.box].mons[where.slot];
    return NULL;
}

/* The collection confirmation explicitly keeps the existing move set when
 * trade evolution occurs. No evolution animation can mutate a live save. */
static int ReceiveEffects(WtGame *game, SaveData *save, const WtJournalEntry *entry, Pokemon *mon)
{
    BoxPokemon box;
    u8 friendship = RECEIVED_FRIENDSHIP;
    u16 target, held, none = ITEM_NONE;
    int evolution = 0;
    GameStats *stats = Save_GameStats_Get(save);
    (void)game;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, 136) != 0) return -1;
        memcpy(mon, entry->original, sizeof(*mon)); return 0;
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || WtPokemon_Encode(entry->incoming, (unsigned char *)&box) != 0) return -1;
    /* Also gives the party image fresh mail and ball capsule data. */
    ZeroMonData(mon); CopyBoxPokemonToPokemon(&box, mon);
    SetMonData(mon, MON_DATA_FRIENDSHIP, &friendship);
    SetMonData(mon, MON_DATA_GENDER, NULL);
    UpdatePokedexWithReceivedSpecies(save, mon);
    held = (u16)GetMonData(mon, MON_DATA_HELD_ITEM, NULL);
    target = GetMonEvolution(NULL, mon, EVOCTX_TRADE, held, &evolution);
    if (target != SPECIES_NONE) {
        SetMonData(mon, MON_DATA_SPECIES, &target);
        UpdateMonAbility(mon); CalcMonLevelAndStats(mon);
        if (!GetMonData(mon, MON_DATA_HAS_NICKNAME, NULL)) SetMonData(mon, MON_DATA_SPECIES_NAME, NULL);
        if (evolution == EVO_TRADE_ITEM) SetMonData(mon, MON_DATA_HELD_ITEM, &none);
        UpdatePokedexWithReceivedSpecies(save, mon);
    }
    GameStats_Inc(stats, GAME_STAT_WIFI_TRADES);
    GameStats_AddScore(stats, SCORE_EVENT_GTS_TRADE);
    return 0;
}

static int BuildReceived(WtGame *game, SaveData *save, WtJournalEntry *entry, int prepare)
{
    Pokemon mon;
    Party *party = SaveArray_Party_Get(save);
    unsigned char *destination, canonical[136];
    int box, slot, found;
    if (prepare) {
        if (party->core.maxCount != PARTY_SIZE || party->core.curCount < 1 || party->core.curCount > PARTY_SIZE) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (party->core.curCount < PARTY_SIZE) {
            entry->destination.kind = 1; entry->destination.slot = party->core.curCount;
            entry->destination.length = sizeof(Pokemon);
        } else {
            PCStorage *storage = SaveArray_PCStorage_Get(save);
            found = 0;
            for (box = 0; box < NUM_BOXES && !found; ++box) for (slot = 0; slot < MONS_PER_BOX; ++slot) {
                if (GetBoxMonData(&storage->boxes[box].mons[slot], MON_DATA_SPECIES_EXISTS, NULL)) continue;
                if (WtPokemon_Decode((const unsigned char *)&storage->boxes[box].mons[slot], canonical) != 0
                    || Read16(canonical + 8) != 0) return -1;
                found = 1; break;
            }
            if (!found) return 1;
            --box;
            entry->destination.kind = 2; entry->destination.box = box; entry->destination.slot = slot;
            entry->destination.length = sizeof(BoxPokemon);
        }
        destination = Destination(save, entry->destination);
        if (!destination || Hash(save, entry->inventoryBefore) != 0) return -1;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        memcpy(entry->destinationBefore, destination, entry->destination.length);
    }
    destination = Destination(save, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && party->core.curCount != entry->destination.slot)) return -1;
    if (ReceiveEffects(game, save, entry, &mon) != 0) return -1;
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 1) {
        /* Mail_New leaves each message's msg_no as heap contents (MailMsg_Init),
         * so a replay after a restart would build a different image. Prepare a
         * fully defined blank mail; a replay keeps the recorded message numbers,
         * which also recovers deliveries prepared before this fix. */
        struct Mail mail;
        unsigned i;
        memset(&mail, 0, sizeof(mail)); Mail_Init(&mail);
        if (!prepare) {
            Pokemon recorded;
            struct Mail retained;
            if (WtPokemon_Decode(entry->destinationAfter, canonical) != 0) return -1;
            memcpy(&recorded, entry->destinationAfter, sizeof(recorded));
            GetMonData(&recorded, MON_DATA_MAIL, &retained);
            for (i = 0; i < NELEMS(mail.unk_20); ++i) mail.unk_20[i].msg_no = retained.unk_20[i].msg_no;
        }
        SetMonData(&mon, MON_DATA_MAIL, &mail);
    }
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 2) {
        RestoreBoxMonPP(Mon_GetBoxMon(&mon));
        BoxMon_UpdateShayminForm(Mon_GetBoxMon(&mon), SHAYMIN_LAND);
    }
    if (WtPokemon_Decode((const unsigned char *)&mon, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, &mon, entry->destination.length);
        if (WtFlash_Hash(canonical, sizeof(canonical), entry->appliedDigest) != 0) return -1;
    } else if (memcmp(entry->destinationAfter, &mon, entry->destination.length) != 0) return -1;
    memcpy(destination, &mon, entry->destination.length);
    if (entry->destination.kind == 1) {
        memset(&party->extra.aprijuiceModifiers[entry->destination.slot], 0, sizeof(party->extra.aprijuiceModifiers[0]));
        ++party->core.curCount;
    }
    return 0;
}

static int VerifyReceived(WtGame *game, const WtJournalEntry *entry)
{
    unsigned char digest[32], canonical[136];
    unsigned char *destination = Destination(game->save, entry->destination);
    if (!destination || memcmp(destination, entry->destinationAfter, entry->destination.length) != 0
        || WtPokemon_Decode(destination, canonical) != 0
        || WtFlash_Hash(canonical, sizeof(canonical), digest) != 0 || memcmp(digest, entry->appliedDigest, 32) != 0
        || Hash(game->save, digest) != 0 || memcmp(digest, entry->inventoryAfter, 32) != 0) return -1;
    return Verify(game->save, entry->inventoryAfter);
}

static void Install(SaveData *save, const SaveData *candidate)
{
    memcpy(save->dynamic_region, candidate->dynamic_region, sizeof(save->dynamic_region));
}

static int Receive(WtGame *game, enum WtInventoryStep step, WtJournalEntry *entry)
{
    SaveData *candidate;
    unsigned char digest[32];
    int result = -1;
    if (step == WT_INVENTORY_VERIFY_RECEIVED) return VerifyReceived(game, entry);
    candidate = Heap_Alloc(game->heapID, sizeof(*candidate));
    if (!candidate) return -1;
    memcpy(candidate, game->save, sizeof(*candidate));
    if (step == WT_INVENTORY_PREPARE_RECEIVE) {
        result = BuildReceived(game, candidate, entry, 1);
        if (result == 0) result = Hash(candidate, entry->inventoryAfter);
        goto done;
    }
    if (Hash(candidate, digest) != 0) goto done;
    if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
        if (BuildReceived(game, candidate, entry, 0) != 0 || Hash(candidate, digest) != 0) goto done;
    }
    if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    Install(game->save, candidate);
    if (SaveBoth(game, digest) != 0 || VerifyReceived(game, entry) != 0) goto done;
    Counters(game, entry); result = 0;
done:
    mbedtls_platform_zeroize(candidate, sizeof(*candidate)); Heap_Free(candidate);
    return result;
}

int WtGame_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGame *game = arg;
    SaveData *candidate = NULL;
    Party *party = SaveArray_Party_Get(game->save), *copy;
    unsigned char digest[32], canonical[136];
    int result = -1, i;
    if (!WtGameSave_LayoutKnown(game->save)) return -1;
    Sync(game->save);
    if (step != WT_INVENTORY_PREPARE_REMOVE
        && (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon)
            || WtPokemon_Decode(entry->original, canonical) != 0
            || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0)) return -1;
    if (step == WT_INVENTORY_VERIFY_REMOVED) return Removed(game, entry);
    if (step == WT_INVENTORY_PREPARE_RECEIVE || step == WT_INVENTORY_RECEIVE || step == WT_INVENTORY_VERIFY_RECEIVED)
        return Receive(game, step, entry);
    if (step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE && step != WT_INVENTORY_RESTORE) return -1;
    candidate = Heap_Alloc(game->heapID, sizeof(*candidate));
    if (!candidate) return -1;
    memcpy(candidate, game->save, sizeof(*candidate)); copy = SaveArray_Party_Get(candidate);
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (!Eligible(copy, slot) || WtPokemon_Decode((const unsigned char *)&copy->core.mons[slot], canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = slot; entry->source.length = sizeof(Pokemon);
        memcpy(entry->original, &copy->core.mons[slot], sizeof(Pokemon));
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        if (WtFlash_Hash(entry->original, sizeof(Pokemon), entry->originalDigest) != 0
            || Hash(candidate, entry->inventoryBefore) != 0) goto done;
        Party_RemoveMon(copy, slot);
        if (Absent(candidate, entry->outgoing) != 0 || Hash(candidate, entry->inventoryAfter) != 0) goto done;
        Counters(game, entry); result = 0; goto done;
    }
    if (entry->source.kind != 1 || entry->source.length != sizeof(Pokemon) || slot >= PARTY_SIZE
        || Hash(candidate, digest) != 0) goto done;
    if (step == WT_INVENTORY_REMOVE) {
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
            if (slot >= (unsigned)copy->core.curCount
                || memcmp(&copy->core.mons[slot], entry->original, sizeof(Pokemon)) != 0) goto done;
            Party_RemoveMon(copy, slot);
            if (Hash(candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            if (copy->core.curCount >= PARTY_SIZE || slot > (unsigned)copy->core.curCount) goto done;
            for (i = copy->core.curCount; i > (int)slot; --i) {
                copy->core.mons[i] = copy->core.mons[i - 1];
                copy->extra.aprijuiceModifiers[i] = copy->extra.aprijuiceModifiers[i - 1];
            }
            memcpy(&copy->core.mons[slot], entry->original, sizeof(Pokemon));
            memset(&copy->extra.aprijuiceModifiers[slot], 0, sizeof(copy->extra.aprijuiceModifiers[slot]));
            ++copy->core.curCount;
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

BOOL WtGameSave_Unique(SaveData *save, unsigned slot, enum HeapID heapID)
{
    SaveData *candidate;
    Party *party = SaveArray_Party_Get(save);
    unsigned char canonical[136];
    BOOL unique;
    if (slot >= (unsigned)party->core.curCount
        || WtPokemon_Decode((const unsigned char *)&party->core.mons[slot], canonical) != 0) return TRUE;
    candidate = Heap_Alloc(heapID, sizeof(*candidate));
    /* Anything uncertain falls through to the deposit's own fail-closed check. */
    if (!candidate) return TRUE;
    memcpy(candidate, save, sizeof(*candidate));
    Party_RemoveMon(SaveArray_Party_Get(candidate), slot);
    unique = Absent(candidate, canonical) == 0;
    mbedtls_platform_zeroize(candidate, sizeof(*candidate)); Heap_Free(candidate);
    return unique;
}

int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    BoxPokemon box;
    if (entry->deliveryKind != WT_DELIVERY_MATCH || entry->destination.length < sizeof(box)) return -1;
    memcpy(&box, entry->destinationAfter, sizeof(box));
    received->species = (uint16_t)GetBoxMonData(&box, MON_DATA_SPECIES, NULL);
    received->gender = GetBoxMonGender(&box);
    received->form = (uint8_t)GetBoxMonData(&box, MON_DATA_FORM, NULL);
    received->personality = GetBoxMonData(&box, MON_DATA_PERSONALITY, NULL);
    received->shiny = CalcShininessByOtIdAndPersonality(GetBoxMonData(&box, MON_DATA_OT_ID, NULL), received->personality);
    received->hasNickname = (int)GetBoxMonData(&box, MON_DATA_HAS_NICKNAME, NULL);
    GetBoxMonData(&box, MON_DATA_NICKNAME, received->nickname);
    GetBoxMonData(&box, MON_DATA_OT_NAME, received->originalTrainer);
    return 0;
}

/* The species with forms, and their counts, are those
 * GetMonData/SanitizeFormId-equivalent checks in src/pokemon.c keep;
 * Spiky-eared Pichu is HeartGold/SoulSilver's own second Pichu form. */
static unsigned Forms(unsigned species)
{
    switch (species) {
    case SPECIES_PICHU: return PICHU_FORM_MAX;
    case SPECIES_UNOWN: return UNOWN_FORM_MAX;
    case SPECIES_CASTFORM: return CASTFORM_FORM_MAX;
    case SPECIES_DEOXYS: return DEOXYS_FORM_MAX;
    case SPECIES_BURMY: return BURMY_FORM_MAX;
    case SPECIES_WORMADAM: return WORMADAM_FORM_MAX;
    case SPECIES_CHERRIM: return CHERRIM_FORM_MAX;
    case SPECIES_SHELLOS: return SHELLOS_FORM_MAX;
    case SPECIES_GASTRODON: return GASTRODON_FORM_MAX;
    case SPECIES_ROTOM: return ROTOM_FORM_MAX;
    case SPECIES_GIRATINA: return GIRATINA_FORM_MAX;
    case SPECIES_SHAYMIN: return SHAYMIN_FORM_MAX;
    case SPECIES_ARCEUS: return ARCEUS_FORM_MAX;
    default: return 1;
    }
}

/* The bag's rule: an item with the prevent-toss attribute (key items, HMs)
 * cannot be held. */
static int Holdable(void *arg, unsigned item)
{
    const WtGame *game = arg;
    return !GetItemAttr((u16)item, ITEMATTR_PREVENT_TOSS, game->heapID);
}

int WtGame_CanHold(void *arg, const unsigned char pk4[136])
{
    /* NATIONAL_DEX_COUNT is 493 and the Enigma Stone (536) is the last item.
     * Byte 0x83 holds the D/P/Pt ball (Cherish Ball is its last); HeartGold
     * and SoulSilver also read their own ball, Fast to Park, from 0x86 when
     * it is nonzero (GetMonData MON_DATA_POKEBALL), so it must be one too. */
    const WtPokemonLimits limits = {
        NATIONAL_DEX_COUNT, ITEM_ENIGMA_STONE, ITEM_CHERISH_BALL, Forms, Holdable, arg
    };
    return pk4[0x86] <= BALL_PARK && WtPokemon_Fits(pk4, &limits);
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    if (entry->phase == WT_JOURNAL_EMPTY || entry->phase == WT_JOURNAL_IDLE) return 1;
    if (entry->phase == WT_JOURNAL_QUEUED || entry->phase == WT_JOURNAL_RECEIVED) return Removed(game, entry) == 0;
    return 0;
}
