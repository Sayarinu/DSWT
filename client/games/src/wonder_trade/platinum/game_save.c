#include "wonder_trade/platinum/game_save.h"

#include <string.h>
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "constants/forms.h"
#include "constants/items.h"
#include "constants/species.h"
#include "heap.h"
#include "game_records.h"
#include "journal.h"
#include "mail.h"
#include "pokedex.h"
#include "poketch.h"
#include "save_catchrecords.h"
#include "generated/game_records.h"
#include "generated/trainer_score_events.h"
#include "generated/evolution_methods.h"
#include "generated/items.h"
#include "system_vars.h"
#include "vars_flags.h"
#include "res/text/bank/location_names.h"
#include "item.h"
#include "party.h"
#include "pc_boxes.h"
#include "pokemon.h"
#include "savedata/save_table.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/delivery.h"

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
                || Read32(footer + 8) != info->size || Read32(footer + 12) != gWtGameConfig.saveMagic
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

static int SaveBoth(WtGame *game, const unsigned char expected[32])
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

int WtGame_Checkpoint(WtGame *game)
{
    unsigned char digest[32];
    if (Hash(game->save, digest) != 0) return -1;
    return Verify(game->save, digest) == 0 ? 0 : SaveBoth(game, digest);
}

static void Counters(WtGame *game, WtJournalEntry *entry)
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

/* Every slot a journal locator names is one of the game's (Destination): the
 * shared decisions scan each one. */
typedef char WtLocatorsMatchGame[MAX_PC_BOXES == WT_GAME_BOX_COUNT
    && MAX_MONS_PER_BOX == WT_GAME_BOX_SLOTS ? 1 : -1];

static unsigned char *Destination(SaveData *save, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < 6 && where.length == sizeof(Pokemon))
        return (unsigned char *)&SaveData_GetParty(save)->pokemon[where.slot];
    if (where.kind == 2 && where.box < MAX_PC_BOXES && where.slot < MAX_MONS_PER_BOX && where.length == sizeof(BoxPokemon))
        return (unsigned char *)&SaveData_GetPCBoxes(save)->boxMons[where.box][where.slot];
    return NULL;
}

/* The collection confirmation explicitly keeps the existing move set when
 * trade evolution occurs. No evolution animation can mutate a live save. */
static int ReceiveEffects(WtGame *game, SaveData *save, const WtJournalEntry *entry, Pokemon *mon)
{
    BoxPokemon box;
    u8 friendship = BASE_FRIENDSHIP_VALUE;
    u16 target, held, none = ITEM_NONE;
    int evolution = 0;
    u16 ot[TRAINER_NAME_LEN + 1], name[MON_NAME_LEN + 1];
    void *event;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, 136) != 0) return -1;
        memcpy(mon, entry->original, sizeof(*mon)); return 0;
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || WtPokemon_Encode(entry->incoming, (unsigned char *)&box) != 0) return -1;
    memset(mon, 0, sizeof(*mon)); Pokemon_FromBoxPokemon(&box, mon);
    Pokemon_SetValue(mon, MON_DATA_FRIENDSHIP, &friendship);
    Pokemon_SetValue(mon, MON_DATA_GENDER, NULL);
    SaveData_UpdateCatchRecords(save, mon);
    if (Pokemon_GetValue(mon, MON_DATA_SPECIES, NULL) == SPECIES_ARCEUS
        && (Pokemon_GetValue(mon, MON_DATA_FATEFUL_ENCOUNTER, NULL)
            || Pokemon_GetValue(mon, MON_DATA_MET_LOCATION, NULL) == LocationNames_Text_HallOfOrigin)) {
        SystemVars_SetArceusEventState(SaveData_GetVarsFlags(save), TRUE);
    }
    held = Pokemon_GetValue(mon, MON_DATA_HELD_ITEM, NULL);
    target = Pokemon_GetEvolutionTargetSpecies(NULL, mon, EVO_CLASS_BY_TRADE, held, &evolution);
    if (target) {
        Pokemon_SetValue(mon, MON_DATA_SPECIES, &target);
        Pokemon_CalcAbility(mon); Pokemon_CalcLevelAndStats(mon);
        if (!Pokemon_GetValue(mon, MON_DATA_HAS_NICKNAME, NULL)) Pokemon_SetValue(mon, MON_DATA_SPECIES_NAME, NULL);
        if (evolution == EVO_TRADE_WITH_HELD_ITEM) Pokemon_SetValue(mon, MON_DATA_HELD_ITEM, &none);
        Pokedex_Capture(SaveData_GetPokedex(save), mon);
        GameRecords_IncrementRecordValue(SaveData_GetGameRecords(save), RECORD_POKEMON_EVOLVED);
        GameRecords_IncrementTrainerScore(SaveData_GetGameRecords(save), TRAINER_SCORE_EVENT_CAUGHT_SPECIES);
        Poketch_PokemonHistoryEnqueue(SaveData_GetPoketch(save), Pokemon_GetBoxPokemon(mon));
    }
    GameRecords_IncrementRecordValue(SaveData_GetGameRecords(save), RECORD_WIFI_TRADES);
    GameRecords_IncrementTrainerScore(SaveData_GetGameRecords(save), TRAINER_SCORE_EVENT_GTS_TRADE_COMPLETE);
    Pokemon_GetValue(mon, MON_DATA_OT_NAME, ot); Pokemon_GetValue(mon, MON_DATA_NICKNAME, name);
    event = JournalEntry_CreateEventGotPokemonFromTrade(ot, Pokemon_GetValue(mon, MON_DATA_OT_GENDER, NULL),
        name, Pokemon_GetGender(mon), game->heapID);
    if (!event) return -1;
    JournalEntry_SaveData(SaveData_GetJournal(save), event, JOURNAL_ONLINE_EVENT);
    return 0;
}

static int BuildReceived(WtGame *game, SaveData *save, WtJournalEntry *entry, int prepare)
{
    Pokemon mon;
    Party *party = SaveData_GetParty(save);
    unsigned char *destination, canonical[136];
    int box, slot, found;
    if (prepare) {
        if (party->capacity != 6 || party->currentCount < 1 || party->currentCount > 6) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (party->currentCount < 6) {
            entry->destination.kind = 1; entry->destination.slot = party->currentCount;
            entry->destination.length = sizeof(Pokemon);
        } else {
            PCBoxes *boxes = SaveData_GetPCBoxes(save);
            found = 0;
            for (box = 0; box < MAX_PC_BOXES && !found; ++box) for (slot = 0; slot < MAX_MONS_PER_BOX; ++slot) {
                if (BoxPokemon_GetValue(&boxes->boxMons[box][slot], MON_DATA_SPECIES_EXISTS, NULL)) continue;
                if (WtPokemon_Decode((const unsigned char *)&boxes->boxMons[box][slot], canonical) != 0
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
        || (entry->destination.kind == 1 && party->currentCount != entry->destination.slot)) return -1;
    if (ReceiveEffects(game, save, entry, &mon) != 0) return -1;
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 1) {
        Mail mail;
        memset(&mail, 0, sizeof(mail));
        Mail_Reset(&mail);
        /* The upstream constructor leaves unused sentence IDs uninitialized. */
        if (!prepare) {
            Pokemon recorded;
            Mail retained;
            unsigned i;
            if (WtPokemon_Decode(entry->destinationAfter, canonical) != 0) return -1;
            memcpy(&recorded, entry->destinationAfter, sizeof(recorded));
            Pokemon_GetValue(&recorded, MON_DATA_MAIL, &retained);
            /* Preserve the exact image/hash of an already prepared delivery. */
            for (i = 0; i < MAIL_MAX_SENTENCES; ++i) mail.sentences[i].id = retained.sentences[i].id;
        }
        Pokemon_SetValue(&mon, MON_DATA_MAIL, &mail);
    }
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 2) {
        BoxPokemon_RestorePP(Pokemon_GetBoxPokemon(&mon));
        BoxPokemon_SetShayminForm(Pokemon_GetBoxPokemon(&mon), SHAYMIN_FORM_LAND);
    }
    if (WtPokemon_Decode((const unsigned char *)&mon, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, &mon, entry->destination.length);
        if (WtFlash_Hash(canonical, sizeof(canonical), entry->appliedDigest) != 0) return -1;
    } else if (memcmp(entry->destinationAfter, &mon, entry->destination.length) != 0) return -1;
    memcpy(destination, &mon, entry->destination.length);
    if (entry->destination.kind == 1) ++party->currentCount;
    return 0;
}

/* The save as the shared delivery decisions see it (wonder_trade/delivery.h). */
static int PartyCount(void *arg, void *save)
{
    const Party *party = SaveData_GetParty(save);
    (void)arg;
    return party->capacity == 6 ? party->currentCount : -1;
}

static unsigned char *Slot(void *arg, void *save, WtJournalLocator where)
{
    (void)arg;
    return Destination(save, where);
}

/* Only a PC slot whose species exists holds a Pokémon, as the game reads it. */
static int Occupied(void *arg, void *save, WtJournalLocator where)
{
    BoxPokemon *mon = (BoxPokemon *)Destination(save, where);
    (void)arg;
    if (!mon) return -1;
    return BoxPokemon_GetValue(mon, MON_DATA_SPECIES_EXISTS, NULL) ? 1 : 0;
}

static int SaveHash(void *arg, void *save, unsigned char digest[32])
{
    (void)arg;
    return Hash(save, digest);
}

static int SaveVerify(void *arg, const unsigned char digest[32])
{
    return Verify(((WtGame *)arg)->save, digest);
}

static int SaveCheckpoint(void *arg)
{
    return WtGame_Checkpoint(arg);
}

static void *Copy(void *arg)
{
    WtGame *game = arg;
    SaveData *candidate = Heap_Alloc(game->heapID, sizeof(*candidate));
    if (candidate) memcpy(candidate, game->save, sizeof(*candidate));
    return candidate;
}

static void Discard(void *arg, void *candidate)
{
    (void)arg;
    if (!candidate) return;
    mbedtls_platform_zeroize(candidate, sizeof(SaveData)); Heap_Free(candidate);
}

static int Commit(void *arg, void *candidate, const unsigned char digest[32])
{
    WtGame *game = arg;
    memcpy(&game->save->body, &((SaveData *)candidate)->body, sizeof(game->save->body));
    return SaveBoth(game, digest);
}

static void SaveCounters(void *arg, uint32_t counters[3])
{
    const SaveData *save = ((WtGame *)arg)->save;
    counters[0] = save->globalCounter; counters[1] = save->blockCounters[0]; counters[2] = save->blockCounters[1];
}

static int Build(void *arg, void *save, WtJournalEntry *entry, int prepare)
{
    return BuildReceived(arg, save, entry, prepare);
}

/* Pokemon_Init's empty Pokémon; its box prefix is BoxPokemon_Init's. */
static void Empty(void *arg, unsigned char *image, unsigned length)
{
    Pokemon empty;
    (void)arg;
    Pokemon_Init(&empty);
    memcpy(image, &empty, length);
}

static void Ops(WtGame *game, WtDeliveryOps *ops)
{
    const WtDeliveryOps ops0 = {
        NULL, NULL, WtFlash_Hash, PartyCount, Slot, Occupied, SaveHash, SaveVerify, SaveCheckpoint,
        Copy, Discard, Commit, SaveCounters, Build, Empty
    };
    *ops = ops0; ops->arg = game; ops->live = game->save;
}

int WtGame_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGame *game = arg;
    WtDeliveryOps ops;
    SaveData *candidate = NULL;
    Party *party = SaveData_GetParty(game->save), *copy;
    unsigned char digest[32], canonical[136];
    int result = -1, i;
    if (!Layout(game->save)) return -1;
    Ops(game, &ops);
    if (step != WT_INVENTORY_PREPARE_REMOVE && !WtDelivery_Bound(entry)) return -1;
    if (step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE && step != WT_INVENTORY_RESTORE)
        return WtDelivery_Step(&ops, step, entry);
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
        if (WtDelivery_Unique(&ops, candidate, entry->outgoing) != 0 || Hash(candidate, entry->inventoryAfter) != 0) goto done;
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

BOOL WtGameSave_Unique(SaveData *save, unsigned slot, enum HeapID heapID)
{
    WtGame game;
    WtDeliveryOps ops;
    SaveData *candidate;
    Party *party = SaveData_GetParty(save);
    unsigned char canonical[136];
    BOOL unique = TRUE;
    if (slot >= (unsigned)party->currentCount
        || WtPokemon_Decode((const unsigned char *)&party->pokemon[slot], canonical) != 0) return TRUE;
    game.save = save; game.heapID = heapID; Ops(&game, &ops);
    candidate = Copy(&game);
    /* Anything uncertain falls through to the deposit's own fail-closed check. */
    if (!candidate) return TRUE;
    Party_RemovePokemonBySlotIndex(SaveData_GetParty(candidate), slot);
    unique = WtDelivery_Unique(&ops, candidate, canonical) == 0;
    Discard(&game, candidate);
    return unique;
}

int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    BoxPokemon box;
    if (entry->deliveryKind != WT_DELIVERY_MATCH || entry->destination.length < sizeof(box)) return -1;
    memcpy(&box, entry->destinationAfter, sizeof(box));
    received->species = BoxPokemon_GetValue(&box, MON_DATA_SPECIES, NULL);
    received->gender = BoxPokemon_GetGender(&box);
    received->form = BoxPokemon_GetForm(&box);
    received->personality = BoxPokemon_GetValue(&box, MON_DATA_PERSONALITY, NULL);
    received->shiny = Pokemon_IsPersonalityShiny(BoxPokemon_GetValue(&box, MON_DATA_OT_ID, NULL), received->personality);
    received->hasNickname = BoxPokemon_GetValue(&box, MON_DATA_HAS_NICKNAME, NULL);
    BoxPokemon_GetValue(&box, MON_DATA_NICKNAME, received->nickname);
    BoxPokemon_GetValue(&box, MON_DATA_OT_NAME, received->originalTrainer);
    received->originalTrainerGender = BoxPokemon_GetValue(&box, MON_DATA_OT_GENDER, NULL);
    return 0;
}

/* The species with forms, and their counts, are those Pokemon_SanitizeFormId
 * (src/pokemon.c) keeps; it resets any other form to 0. Spiky-eared Pichu is
 * not among them: it is HG/SS-only. */
static unsigned Forms(unsigned species)
{
    switch (species) {
    case SPECIES_UNOWN: return UNOWN_FORM_COUNT;
    case SPECIES_CASTFORM: return CASTFORM_FORM_COUNT;
    case SPECIES_DEOXYS: return DEOXYS_FORM_COUNT;
    case SPECIES_BURMY: return BURMY_FORM_COUNT;
    case SPECIES_WORMADAM: return WORMADAM_FORM_COUNT;
    case SPECIES_CHERRIM: return CHERRIM_FORM_COUNT;
    case SPECIES_SHELLOS: return SHELLOS_FORM_COUNT;
    case SPECIES_GASTRODON: return GASTRODON_FORM_COUNT;
    case SPECIES_ROTOM: return ROTOM_FORM_COUNT;
    case SPECIES_GIRATINA: return GIRATINA_FORM_COUNT;
    case SPECIES_SHAYMIN: return SHAYMIN_FORM_COUNT;
    case SPECIES_ARCEUS: return ARCEUS_FORM_COUNT;
    default: return 1;
    }
}

/* The bag's own rule: an item with preventToss set (key items and HMs)
 * "can't be held" (Bag_Text_ItemCantBeHeld in src/applications/bag/main.c). */
static int Holdable(void *arg, unsigned item)
{
    const WtGame *game = arg;
    return !Item_LoadParam((u16)item, ITEM_PARAM_PREVENT_TOSS, game->heapID);
}

int WtGame_CanHold(void *arg, const unsigned char pk4[136])
{
    /* NATIONAL_DEX_COUNT is 493 (Arceus) and NUM_ITEMS 467 (Secret Key; the range
     * includes Platinum's Griseous Orb, 112). The ball byte holds the ball's
     * item ID (PokemonDataBlockD.pokeball); the Cherish Ball is the last. */
    const WtPokemonLimits limits = {
        NATIONAL_DEX_COUNT, NUM_ITEMS, ITEM_CHERISH_BALL, Forms, Holdable, arg
    };
    return WtPokemon_Fits(pk4, &limits);
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    WtDeliveryOps ops;
    if (!Layout(game->save)) return 0;
    Ops(game, &ops);
    return WtDelivery_CanLeave(&ops, entry);
}
