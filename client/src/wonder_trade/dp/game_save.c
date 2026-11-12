#include "wonder_trade/dp/game_save.h"

#include <stddef.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/species.h"

#include "heap.h"
#include "itemtool.h"
#include "mail.h"
#include "party.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "unk_02022504.h"
#include "unk_0202C144.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"

/* Defined in src/pokemon.c, which declares it only for itself. */
extern u8 CalcShininessByOtIdAndPersonality(u32 otId, u32 personality);

#define FOOTER_SIZE 20
#define FOOTER_MAGIC 0x20060623
#define GENERAL_SIZE 0xC100
#define BOXES_OFFSET 0xC100
#define BOXES_SIZE 0x121E0
#define EXTRA_FOOTER_SIZE 16
/* The friendship the GTS gives every Pokémon it delivers (ov80_022355F0 in
 * overlays/80/asm/overlay_80.s). */
#define RECEIVED_FRIENDSHIP 70
/* GetMonEvolution's trade context (src/pokemon.c). */
#define EVOLUTION_CONTEXT_TRADE 1

static const struct { u32 offset, size; } sBlocks[2] = {
    { 0, GENERAL_SIZE }, { BOXES_OFFSET, BOXES_SIZE },
};

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static u32 Read32(const unsigned char *p) { return Read16(p) | ((u32)Read16(p + 2) << 16); }

int WtGameSave_LayoutKnown(const SaveData *save)
{
    int i;
    if (SAVE_BLOCK_NUM != 36 || SAVE_PAGE_MAX != 32 || SAVE_SECTOR_SIZE != 0x1000
        || sizeof(struct SaveChunkFooter) != FOOTER_SIZE || sizeof(struct SaveArrayFooter) != EXTRA_FOOTER_SIZE
        || sizeof(PCStorage) != 0x121C8 || offsetof(PCStorage, boxes) != 4
        || sizeof(struct Pokemon) != 236 || sizeof(struct BoxPokemon) != 136
        || sizeof(struct Party) != 8 + PARTY_SIZE * 236
        || save->arrayHeaders[35].offset != BOXES_OFFSET || save->arrayHeaders[2].offset != 0x90
        || UNK_020EE6D8 != 2) return 0;
    for (i = 0; i < 2; ++i) {
        if (save->saveSlotSpecs[i].offset != sBlocks[i].offset || save->saveSlotSpecs[i].size != sBlocks[i].size) return 0;
    }
    /* Every extra chunk and its mirror (+0x40000) ends before a journal bank. */
    for (i = 0; i < UNK_020EE6D8; ++i) {
        const struct SaveChunkHeader *chunk = &UNK_020EE6E0[i];
        if ((u32)chunk->linkedId * SAVE_SECTOR_SIZE + chunk->sizeFunc() + EXTRA_FOOTER_SIZE > gWtGameConfig.journalBank[0]) return 0;
    }
    return gWtGameConfig.journalBank[0] == 0x30000 && gWtGameConfig.journalBank[1] == 0x70000;
}

/* SHA-256 of both blocks' bodies in RAM, the bytes a save writes before each footer. */
static int Hash(const SaveData *save, unsigned char digest[32])
{
    mbedtls_sha256_context hash;
    unsigned block;
    int result = -1;
    if (!WtGameSave_LayoutKnown(save)) return -1;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
    for (block = 0; block < 2; ++block) {
        if (mbedtls_sha256_update(&hash, save->dynamic_region + sBlocks[block].offset,
                sBlocks[block].size - FOOTER_SIZE) != 0) goto done;
    }
    result = mbedtls_sha256_finish(&hash, digest);
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* Verify both complete halves on flash independently of the RAM selector:
 * footers, CRCs and the body digest. Each half must hold both blocks of one
 * full save; two consecutive full saves leave counters one apart, the newer
 * equal to RAM's (src/save.c: sub_02022DFC and sub_02022968). */
static int Verify(const SaveData *save, const unsigned char expected[32])
{
    unsigned half, block, offset, i, bit, size;
    unsigned char page[256], footer[FOOTER_SIZE], digest[32];
    mbedtls_sha256_context hash;
    int result = -1;
    if (!WtGameSave_LayoutKnown(save)) return -1;
    mbedtls_sha256_init(&hash);
    for (half = 0; half < 2; ++half) {
        u32 global = 0, age = 0;
        if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
        for (block = 0; block < 2; ++block) {
            unsigned crc = 65535;
            u32 body = sBlocks[block].size - FOOTER_SIZE, address = half * 0x40000 + sBlocks[block].offset, counter;
            for (offset = 0; offset < body; offset += size) {
                size = body - offset;
                if (size > sizeof(page)) size = sizeof(page);
                if (WtFlash_ReadRange(address + offset, page, size) != 0) goto done;
                for (i = 0; i < size; ++i) {
                    crc ^= (unsigned)page[i] << 8;
                    for (bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 65535;
                }
                if (mbedtls_sha256_update(&hash, page, size) != 0) goto done;
            }
            /* global counter, block counter, size, magic, block u8, pad, CRC u16 */
            if (WtFlash_ReadRange(address + body, footer, sizeof(footer)) != 0
                || Read32(footer + 8) != sBlocks[block].size || Read32(footer + 12) != FOOTER_MAGIC
                || footer[16] != block || Read16(footer + 18) != crc) goto done;
            counter = Read32(footer + 4);
            if (block == 0) {
                global = Read32(footer);
                age = save->unk_20214 - global;
                if (age > 1) goto done;
            } else if (Read32(footer) != global) goto done;
            if (save->unk_20218[block] - counter != age) goto done;
        }
        if (mbedtls_sha256_finish(&hash, digest) != 0 || memcmp(digest, expected, 32) != 0) goto done;
    }
    result = 0;
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* A save writes the box block only when the large-sector flag is set (src/save.c:
 * sub_02022DFC). Setting it before each of two consecutive saves writes a
 * full image to both halves. */
static int SaveBoth(WtGame *game, const unsigned char expected[32])
{
    unsigned char actual[32];
    int result = -1, i;
    if (Hash(game->save, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (Verify(game->save, expected) == 0) return 0;
    if (game->save->isNewGame || !game->save->saveFileExists || !game->save->flashOkay) return -1;
    WtRecovery_InternalWrite(1);
    for (i = 0; i < 2; ++i) {
        game->save->largeSectorChanged = 1;
        /* 2 is a completed write (src/save.c: sub_02022E78). */
        if (SaveGame(game->save) != 2) goto done;
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
    if (Hash(game->save, digest) != 0) return -1;
    return Verify(game->save, digest) == 0 ? 0 : SaveBoth(game, digest);
}

/* The global counter covers both blocks of a half; each block also counts its own writes. */
static void Counters(WtGame *game, WtJournalEntry *entry)
{
    entry->globalCounter = game->save->unk_20214;
    entry->generalCounter = game->save->unk_20218[0];
    entry->boxCounter = game->save->unk_20218[1];
}

static int Eligible(Party *party, unsigned slot)
{
    int i, usable = 0;
    Pokemon *mon;
    if (party->maxCount != PARTY_SIZE || party->curCount < 2 || party->curCount > PARTY_SIZE
        || slot >= (unsigned)party->curCount) return 0;
    mon = &party->mons[slot];
    if (!GetMonData(mon, MON_DATA_SPECIES, NULL)
        || GetMonData(mon, MON_DATA_SPECIES, NULL) > NATIONAL_DEX_COUNT
        || GetMonData(mon, MON_DATA_IS_EGG, NULL)
        || GetMonData(mon, MON_DATA_SANITY_IS_EGG, NULL)
        || GetMonData(mon, MON_DATA_BALL_CAPSULE_ID, NULL)
        || ItemIdIsMail((u16)GetMonData(mon, MON_DATA_HELD_ITEM, NULL))) return 0;
    for (i = 0; i < party->curCount; ++i) {
        if ((unsigned)i != slot && !GetMonData(&party->mons[i], MON_DATA_IS_EGG, NULL)
            && GetMonData(&party->mons[i], MON_DATA_HP, NULL) != 0) ++usable;
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
    unsigned i, box;
    if (party->maxCount != PARTY_SIZE || party->curCount < 1 || party->curCount > PARTY_SIZE) return -1;
    for (i = 0; i < (unsigned)party->curCount; ++i) {
        if (WtPokemon_Decode((const unsigned char *)&party->mons[i], canonical) != 0
            || memcmp(canonical, outgoing, 136) == 0) return -1;
    }
    for (box = 0; box < NUM_BOXES; ++box) for (i = 0; i < MONS_PER_BOX; ++i) {
        struct BoxPokemon *mon = &storage->boxes[box][i];
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
    if ((u32)(game->save->unk_20214 - entry->globalCounter) >= 0x80000000
        || Absent(game->save, entry->outgoing) != 0) return -1;
    return WtGame_Checkpoint(game);
}

static unsigned char *Destination(SaveData *save, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < PARTY_SIZE && where.length == sizeof(Pokemon))
        return (unsigned char *)&SaveArray_Party_Get(save)->mons[where.slot];
    if (where.kind == 2 && where.box < NUM_BOXES && where.slot < MONS_PER_BOX && where.length == sizeof(struct BoxPokemon))
        return (unsigned char *)&SaveArray_PCStorage_Get(save)->boxes[where.box][where.slot];
    return NULL;
}

/* The GTS's receive effects (ov80_022355F0): catch records, Pokédex and
 * Pokétch history (sub_0202C144) and friendship 70. The collection
 * confirmation explicitly keeps the existing move set when trade evolution
 * occurs; no evolution animation can mutate a live save. */
static int ReceiveEffects(SaveData *save, const WtJournalEntry *entry, Pokemon *mon)
{
    struct BoxPokemon box;
    Mail mail;
    u8 friendship = RECEIVED_FRIENDSHIP, gender;
    u16 target, held, none = ITEM_NONE;
    u32 evolution = 0;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, 136) != 0) return -1;
        memcpy(mon, entry->original, sizeof(*mon)); return 0;
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || WtPokemon_Encode(entry->incoming, (unsigned char *)&box) != 0) return -1;
    ZeroMonData(mon); CopyBoxPokemonToPokemon(&box, mon);
    /* Mail_New leaves each message number uninitialized; replace it with a
     * fully defined blank mail so the prepared and applied images match. */
    memset(&mail, 0, sizeof(mail)); Mail_Init(&mail);
    SetMonData(mon, MON_DATA_MAIL, &mail);
    SetMonData(mon, MON_DATA_FRIENDSHIP, &friendship);
    gender = GetGenderBySpeciesAndPersonality((u16)GetMonData(mon, MON_DATA_SPECIES, NULL), GetMonData(mon, MON_DATA_PERSONALITY, NULL));
    SetMonData(mon, MON_DATA_GENDER, &gender);
    sub_0202C144(save, mon);
    held = (u16)GetMonData(mon, MON_DATA_HELD_ITEM, NULL);
    target = GetMonEvolution(NULL, mon, EVOLUTION_CONTEXT_TRADE, held, &evolution);
    if (target != SPECIES_NONE) {
        SetMonData(mon, MON_DATA_SPECIES, &target);
        sub_0206A1C4(mon); /* the ability for the new species */
        CalcMonLevelAndStats(mon);
        if (!GetMonData(mon, MON_DATA_HAS_NICKNAME, NULL)) SetMonData(mon, MON_DATA_SPECIES_NAME, NULL);
        if (evolution == EVO_TRADE_ITEM) SetMonData(mon, MON_DATA_HELD_ITEM, &none);
        sub_0202C144(save, mon);
    }
    return 0;
}

static int BuildReceived(SaveData *save, WtJournalEntry *entry, int prepare)
{
    Pokemon mon;
    Party *party = SaveArray_Party_Get(save);
    unsigned char *destination, canonical[136];
    unsigned box, slot;
    int found;
    if (prepare) {
        if (party->maxCount != PARTY_SIZE || party->curCount < 1 || party->curCount > PARTY_SIZE) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (party->curCount < PARTY_SIZE) {
            entry->destination.kind = 1; entry->destination.slot = party->curCount;
            entry->destination.length = sizeof(Pokemon);
        } else {
            PCStorage *storage = SaveArray_PCStorage_Get(save);
            found = 0;
            for (box = 0; box < NUM_BOXES && !found; ++box) for (slot = 0; slot < MONS_PER_BOX; ++slot) {
                if (GetBoxMonData(&storage->boxes[box][slot], MON_DATA_SPECIES_EXISTS, NULL)) continue;
                if (WtPokemon_Decode((const unsigned char *)&storage->boxes[box][slot], canonical) != 0
                    || Read16(canonical + 8) != 0) return -1;
                found = 1; break;
            }
            if (!found) return 1;
            --box;
            entry->destination.kind = 2; entry->destination.box = box; entry->destination.slot = slot;
            entry->destination.length = sizeof(struct BoxPokemon);
        }
        destination = Destination(save, entry->destination);
        if (!destination || Hash(save, entry->inventoryBefore) != 0) return -1;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        memcpy(entry->destinationBefore, destination, entry->destination.length);
    }
    destination = Destination(save, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && party->curCount != entry->destination.slot)) return -1;
    if (ReceiveEffects(save, entry, &mon) != 0) return -1;
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 2) RestoreBoxMonPP(&mon.box);
    if (WtPokemon_Decode((const unsigned char *)&mon, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, &mon, entry->destination.length);
        if (WtFlash_Hash(canonical, sizeof(canonical), entry->appliedDigest) != 0) return -1;
    } else if (memcmp(entry->destinationAfter, &mon, entry->destination.length) != 0) return -1;
    memcpy(destination, &mon, entry->destination.length);
    if (entry->destination.kind == 1) ++party->curCount;
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
        result = BuildReceived(candidate, entry, 1);
        if (result == 0) result = Hash(candidate, entry->inventoryAfter);
        goto done;
    }
    if (Hash(candidate, digest) != 0) goto done;
    if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
        if (BuildReceived(candidate, entry, 0) != 0 || Hash(candidate, digest) != 0) goto done;
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
        if (!Eligible(copy, slot) || WtPokemon_Decode((const unsigned char *)&copy->mons[slot], canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = slot; entry->source.length = sizeof(Pokemon);
        memcpy(entry->original, &copy->mons[slot], sizeof(Pokemon));
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
            if (slot >= (unsigned)copy->curCount
                || memcmp(&copy->mons[slot], entry->original, sizeof(Pokemon)) != 0) goto done;
            Party_RemoveMon(copy, slot);
            if (Hash(candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            if (copy->curCount >= PARTY_SIZE || slot > (unsigned)copy->curCount) goto done;
            for (i = copy->curCount; i > (int)slot; --i) copy->mons[i] = copy->mons[i - 1];
            memcpy(&copy->mons[slot], entry->original, sizeof(Pokemon));
            ++copy->curCount;
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
    if (slot >= (unsigned)party->curCount
        || WtPokemon_Decode((const unsigned char *)&party->mons[slot], canonical) != 0) return TRUE;
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
    struct BoxPokemon box;
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

/* The forms Diamond and Pearl draw (sub_02068C00 in src/pokemon.c, which
 * shows form 0 beyond these). Pichu, Rotom, Giratina and Shaymin have only
 * form 0 here: Spiky-eared Pichu (HeartGold/SoulSilver), Rotom's appliance
 * forms, Giratina's Origin Forme and Shaymin's Sky Forme (Platinum) are refused. */
static unsigned Forms(unsigned species)
{
    switch (species) {
    case SPECIES_UNOWN: return 28;
    case SPECIES_CASTFORM: return 4;
    case SPECIES_DEOXYS: return 4;
    case SPECIES_BURMY: return 3;
    case SPECIES_WORMADAM: return 3;
    case SPECIES_CHERRIM: return 2;
    case SPECIES_SHELLOS: return 2;
    case SPECIES_GASTRODON: return 2;
    case SPECIES_ARCEUS: return 18;
    default: return 1;
    }
}

/* The bag's rule: an item with the prevent-toss attribute (key items, HMs)
 * cannot be held. Items 112 to 134 do not exist in Diamond and Pearl
 * (constants/items.h jumps from the Odd Keystone to the Adamant Orb); 112 is
 * Platinum's Griseous Orb. */
static int Holdable(void *arg, unsigned item)
{
    const WtGame *game = arg;
    if (item > ITEM_ODD_KEYSTONE && item < ITEM_ADAMANT_ORB) return 0;
    return !GetItemAttr((u16)item, ITEMATTR_PREVENT_TOSS, game->heapID);
}

int WtGame_CanHold(void *arg, const unsigned char pk4[136])
{
    /* NATIONAL_DEX_COUNT is 493 and the Secret Potion (464) is the last item
     * (constants/species.h, constants/items.h). Byte 0x83 holds the ball's item
     * ID; the Cherish Ball (16) is the last. */
    const WtPokemonLimits limits = {
        NATIONAL_DEX_COUNT, ITEM_SECRETPOTION, ITEM_CHERISH_BALL, Forms, Holdable, arg
    };
    return WtPokemon_Fits(pk4, &limits);
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    if (entry->phase == WT_JOURNAL_EMPTY || entry->phase == WT_JOURNAL_IDLE) return 1;
    if (entry->phase == WT_JOURNAL_QUEUED || entry->phase == WT_JOURNAL_RECEIVED) return Removed(game, entry) == 0;
    return 0;
}
