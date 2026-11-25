#include "wonder_trade/bw2/game_save.h"

#include <nitro.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "constants/pokemon.h"
#include "gfl/backup_system.h"
#include "gfl/heapsys.h"
#include "gfl/savedata.h"
#include "pml/evolution.h"
#include "pml/item.h"
#include "pml/personal.h"
#include "pml/poke_party.h"
#include "save/box.h"
#include "save/musical_savedata.h"
#include "save/pokedex_savedata.h"
#include "save/trainer_data_savedata.h"
#include "wonder_trade/bw2/musical_area.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/delivery.h"

/* Accessors the integration patch adds to src/gfl/backup_system.c. */
SaveDataTable *SaveData_GetTablePtr(SaveData *sv);
void SaveData_RequestFullWrite(SaveData *sv);
u32 SaveData_GetSaveCount(SaveData *sv);

/* The main save (src/system/save_table.c): two copies of SAVE_SIZE bytes, the
 * backup at 0x26000; each holds the blocks, each followed by a {count, CRC}
 * footer, then a CRC of each block and the 16-byte footer
 * (protocol/save-layout-b2w2.json). */
#define MAIN_COPY 0x0
#define BACKUP_COPY 0x26000
#define SAVE_USED 0x25FA4
#define SAVE_FOOTER_SIZE 16
#define BLOCK_FOOTER_SIZE 4
#define SPECIES_LAST 649 /* Genesect */
#define BALL_LAST 25     /* the Dream Ball's ball index */
#define MUSICAL_DIST_MARKER 6

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

typedef struct Image {
    SaveData *sv;
    SaveDataTable *table;
    unsigned char *data;
    u32 size, blocks, crcsSize;
} Image;

static int Open(const WtGame *game, Image *image)
{
    image->sv = game->save->sv;
    image->table = SaveData_GetTablePtr(image->sv);
    image->data = SaveData_GetData(image->sv, &image->size);
    image->blocks = getSaveSize(image->table);
    image->crcsSize = image->blocks * 2 + (image->blocks & 1) * 2;
    return image->size == SAVE_USED && image->blocks == SAVE_BLOCK_MAX
        && sizeof(BoxPkm) == WT_POKEMON_SIZE && sizeof(PartyPkm) == WT_GAME_PARTY_SIZE ? 0 : -1;
}

static void *Block(const Image *image, unsigned char *data, u32 id)
{
    return data + getStartOffsetOfSaveBlockDataInSave(image->table, id);
}

/* The content of every block, without the footers and CRCs a save rewrites. */
static int Hash(const Image *image, const unsigned char *data, unsigned char digest[32])
{
    mbedtls_sha256_context hash;
    u32 id;
    int result = -1;
    mbedtls_sha256_init(&hash);
    if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
    for (id = 0; id < image->blocks; ++id) {
        if (mbedtls_sha256_update(&hash, data + getStartOffsetOfSaveBlockDataInSave(image->table, id),
                getSizeOfSaveBlockData(image->table, id)) != 0) goto done;
    }
    result = mbedtls_sha256_finish(&hash, digest);
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* Both copies on the card hold the expected content, each block with the
 * footer and CRC the RAM save has, and the save's CRC table and footer: the
 * game loads exactly this save from either copy. */
static int Verify(const Image *image, const unsigned char expected[32])
{
    static const u32 copies[2] = { MAIN_COPY, BACKUP_COPY };
    unsigned char page[256], digest[32];
    mbedtls_sha256_context hash;
    u32 copy, id, offset, size, start, length, i, bit;
    int result = -1;
    mbedtls_sha256_init(&hash);
    for (copy = 0; copy < 2; ++copy) {
        if (mbedtls_sha256_starts(&hash, 0) != 0) goto done;
        for (id = 0; id < image->blocks; ++id) {
            unsigned crc = 0xffff;
            start = getStartOffsetOfSaveBlockDataInSave(image->table, id);
            length = getSizeOfSaveBlockData(image->table, id);
            for (offset = 0; offset < length; offset += size) {
                size = length - offset < sizeof(page) ? length - offset : sizeof(page);
                if (WtFlash_ReadRange(copies[copy] + start + offset, page, size) != 0
                    || mbedtls_sha256_update(&hash, page, size) != 0) goto done;
                for (i = 0; i < size; ++i) {
                    crc ^= (unsigned)page[i] << 8;
                    for (bit = 0; bit < 8; ++bit) crc = ((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0)) & 0xffff;
                }
            }
            if (WtFlash_ReadRange(copies[copy] + start + length, page, BLOCK_FOOTER_SIZE) != 0
                || Read16(page + 2) != crc || memcmp(page, image->data + start + length, BLOCK_FOOTER_SIZE) != 0) goto done;
        }
        if (mbedtls_sha256_finish(&hash, digest) != 0 || memcmp(digest, expected, 32) != 0) goto done;
        start = image->size - SAVE_FOOTER_SIZE - image->crcsSize;
        for (offset = 0; offset < image->crcsSize + SAVE_FOOTER_SIZE; offset += size) {
            size = image->crcsSize + SAVE_FOOTER_SIZE - offset;
            if (size > sizeof(page)) size = sizeof(page);
            if (WtFlash_ReadRange(copies[copy] + start + offset, page, size) != 0
                || memcmp(page, image->data + start + offset, size) != 0) goto done;
        }
    }
    result = 0;
done:
    mbedtls_sha256_free(&hash);
    return result;
}

/* The game's own save writes the changed blocks to both copies; if both are
 * not then exactly the expected save, a second save rewrites every block of
 * both copies. Both copies are checked after each. */
static int SaveBoth(WtGame *game, const Image *image, const unsigned char expected[32])
{
    unsigned char actual[32];
    u32 status;
    int result = -1, attempt;
    if (Hash(image, image->data, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (Verify(image, expected) == 0) return 0;
    WtRecovery_InternalWrite(1);
    for (attempt = 0; attempt < 2 && result != 0; ++attempt) {
        if (attempt) SaveData_RequestFullWrite(image->sv);
        SaveControl_StartSave(game->save);
        do {
            status = SaveControl_ContinueSave(game->save);
            if (status == SAVE_RESULT_CONTINUE || status == SAVE_RESULT_LAST) OS_Sleep(1);
        } while (status == SAVE_RESULT_CONTINUE || status == SAVE_RESULT_LAST);
        if (status == SAVE_RESULT_OK) result = Verify(image, expected);
    }
    WtRecovery_InternalWrite(0);
    if (result != 0) WtRecovery_Block();
    return result;
}

int WtGame_Checkpoint(WtGame *game)
{
    Image image;
    unsigned char digest[32];
    if (Open(game, &image) != 0 || Hash(&image, image.data, digest) != 0) return -1;
    return SaveBoth(game, &image, digest);
}

static int MusicalRead(void *arg, int bank, unsigned char bytes[WT_MUSICAL_AREA_BANK_SIZE])
{
    (void)arg;
    return WtFlash_ReadRange(gWtGameConfig.journalBank[bank], bytes, WT_MUSICAL_AREA_BANK_SIZE);
}

/* Programs erased flash over one bank, a page at a time, each verified. */
static int MusicalErase(void *arg, int bank)
{
    static unsigned char erased[256];
    u32 address = gWtGameConfig.journalBank[bank], offset, type;
    int result = -1;
    s32 lockID = OS_GetLockID();
    (void)arg;
    if (lockID == OS_LOCK_ID_ERROR) return -1;
    memset(erased, 0xff, sizeof(erased));
    WtGame_HoldAwake(TRUE);
    CARD_LockBackup(lockID);
    type = (u32)CARD_GetCurrentBackupType();
    if (type == gWtGameConfig.flashType[0] || type == gWtGameConfig.flashType[1]) {
        for (offset = 0; offset < WT_MUSICAL_AREA_BANK_SIZE; offset += sizeof(erased))
            if (!CARD_WriteAndVerifyFlash(address + offset, erased, sizeof(erased))) break;
        if (offset == WT_MUSICAL_AREA_BANK_SIZE) result = 0;
    }
    CARD_UnlockBackup(lockID);
    OS_ReleaseLockID(lockID);
    WtGame_HoldAwake(FALSE);
    return result;
}

static const WtMusicalAreaIO sMusicalArea = { NULL, MusicalRead, MusicalErase };

BOOL WtGameSave_MusicalLeftover(void)
{
    return WtMusicalArea_Inspect(sMusicalArea) == WT_MUSICAL_AREA_LEFTOVER;
}

/* The musical distribution save holds the journal banks. Before the first
 * journal write the game must never read it again: its trainer data marker
 * is cleared, the downloaded program marked absent, and both save copies
 * written, since a failed load at Continue clears the marker in RAM only.
 * Only then is any data a musical left in the banks erased. Zero when the
 * area is free for the journal. */
int WtGameSave_ReleaseMusicalArea(WtGame *game)
{
    int state = WtMusicalArea_Inspect(sMusicalArea);
    BOOL marked = TrainerDataSave_IsMarkerSet(game->save, MUSICAL_DIST_MARKER);
    if (state < 0) return -1;
    if (state == WT_MUSICAL_AREA_JOURNAL && !marked) return 0;
    if (marked) {
        TrainerDataSave_ClearMarker(game->save, MUSICAL_DIST_MARKER);
        MusicalSave_SetHasDistProgram(getAddressOfMusicalDataInfo(game->save), FALSE);
    }
    /* Writes nothing when both copies already hold the save in RAM. */
    if (WtGame_Checkpoint(game) != 0) return -1;
    return WtMusicalArea_Erase(sMusicalArea);
}

static void Counters(WtGame *game, WtJournalEntry *entry)
{
    /* Gen V keeps one save count for the whole save; its block counts change
     * with every save of the block, so the journal records the save count in
     * each field the Gen IV layout has. */
    u32 count = SaveData_GetSaveCount(game->save->sv);
    entry->globalCounter = count;
    entry->generalCounter = count;
    entry->boxCounter = count;
}

static PokeParty *Party(const Image *image, unsigned char *data)
{
    return Block(image, data, SAVE_BLOCK_PARTY);
}

static BoxPkm *BoxSlot(const Image *image, unsigned char *data, unsigned box, unsigned slot)
{
    return &((BoxTraySave *)Block(image, data, SAVE_BLOCK_BOX_TRAY + box))->pkm[slot];
}

static int Eligible(PokeParty *party, unsigned slot)
{
    int i, usable = 0;
    PartyPkm *pkm;
    u32 species;
    if (party->capacity != PARTY_SIZE || party->count < 2 || party->count > PARTY_SIZE
        || slot >= (unsigned)party->count) return 0;
    pkm = &party->pkms[slot];
    species = PokeParty_GetParam(pkm, PKM_PARAM_SPECIES, NULL);
    if (species == 0 || species > SPECIES_LAST
        || PokeParty_GetParam(pkm, PKM_PARAM_EGG_OR_BAD_EGG, NULL)
        || PML_ItemIsMail((u16)PokeParty_GetParam(pkm, PKM_PARAM_ITEM, NULL))) return 0;
    for (i = 0; i < party->count; ++i) {
        if ((unsigned)i != slot && !PokeParty_GetParam(&party->pkms[i], PKM_PARAM_EGG_OR_BAD_EGG, NULL)
            && PokeParty_GetParam(&party->pkms[i], PKM_PARAM_HP, NULL) != 0) ++usable;
    }
    return usable != 0;
}

int WtGameSave_Eligible(WtGame *game, unsigned slot)
{
    Image image;
    return Open(game, &image) == 0 && Eligible(Party(&image, image.data), slot);
}

static void RemoveSlot(PokeParty *party, unsigned slot)
{
    int i;
    for (i = (int)slot; i < party->count - 1; ++i) party->pkms[i] = party->pkms[i + 1];
    PokeParty_ClearPkm(&party->pkms[party->count - 1]);
    --party->count;
}

/* Every slot a journal locator names is one of the game's (Destination): the
 * shared decisions scan each one. */
typedef char WtLocatorsMatchGame[BOX_MAX_TRAY == WT_GAME_BOX_COUNT && BOX_MAX_POS == WT_GAME_BOX_SLOTS
    && BATTLE_BOX_SLOTS == WT_GAME_BATTLE_BOX_SLOTS ? 1 : -1];

static unsigned char *Destination(const Image *image, unsigned char *data, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < PARTY_SIZE && where.length == sizeof(PartyPkm))
        return (unsigned char *)&Party(image, data)->pkms[where.slot];
    if (where.kind == 2 && where.box < BOX_MAX_TRAY && where.slot < BOX_MAX_POS && where.length == sizeof(BoxPkm))
        return (unsigned char *)BoxSlot(image, data, where.box, where.slot);
    /* Only a delivery found there after another game's save is recorded in the Battle Box. */
    if (where.kind == 3 && where.box == 0 && where.slot < BATTLE_BOX_SLOTS && where.length == sizeof(BoxPkm))
        return (unsigned char *)&((BattleBoxSave *)Block(image, data, SAVE_BLOCK_BATTLE_BOX))->pkm[0][where.slot];
    return NULL;
}

/* A traded Pokémon's effects in the receiving game: its friendship returns to
 * its species' base, it is registered as caught, and it evolves by trade as
 * the game's own trades check it (CheckEvolveSpecies with EVO_CHECK_TRADE),
 * the Pokémon sent being the partner for Karrablast and Shelmet. The move set
 * is kept; no evolution scene can change a live save. */
static int ReceiveEffects(WtGame *game, const Image *image, unsigned char *data, const WtJournalEntry *entry,
                          PartyPkm *pkm)
{
    BoxPkm box;
    PartyPkm partner;
    u32 method = 0;
    u16 target;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, WT_POKEMON_SIZE) != 0) return -1;
        memcpy(pkm, entry->original, sizeof(*pkm));
        return 0;
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || WtPokemon_Encode(entry->incoming, (unsigned char *)&box) != 0)
        return -1;
    memset(pkm, 0, sizeof(*pkm));
    pkm->base = box;
    PokeParty_RecalcStats(pkm);
    PokeParty_Recover(pkm);
    PokeParty_SetParam(pkm, PKM_PARAM_HAPPINESS,
        PML_PersonalGetParamSingle((u16)PokeParty_GetParam(pkm, PKM_PARAM_SPECIES, NULL),
            (u16)PokeParty_GetParam(pkm, PKM_PARAM_FORM, NULL), PERSONAL_FRIENDSHIP));
    memcpy(&partner, entry->original, sizeof(partner));
    target = CheckEvolveSpecies(Party(image, data), pkm, EVO_CHECK_TRADE, (u32)&partner, 0, &method, game->heapId);
    mbedtls_platform_zeroize(&partner, sizeof(partner));
    if (target) {
        setChangedPkmSpecies(pkm, target);
        if (method == EVO_METHOD_TRADE_WITH_ITEM) PokeParty_SetParam(pkm, PKM_PARAM_ITEM, 0);
    }
    PokeDex_RegistPkm(Block(image, data, SAVE_BLOCK_POKEDEX), pkm);
    return 0;
}

static int BuildReceived(WtGame *game, const Image *image, unsigned char *data, WtJournalEntry *entry, int prepare)
{
    PartyPkm pkm;
    PokeParty *party = Party(image, data);
    unsigned char *destination, canonical[WT_POKEMON_SIZE];
    unsigned box = 0, slot = 0;
    int found = 0;
    if (prepare) {
        if (party->capacity != PARTY_SIZE || party->count < 1 || party->count > PARTY_SIZE) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (party->count < PARTY_SIZE) {
            entry->destination.kind = 1; entry->destination.slot = party->count;
            entry->destination.length = sizeof(PartyPkm);
        } else {
            for (box = 0; box < BOX_MAX_TRAY && !found; ++box) for (slot = 0; slot < BOX_MAX_POS; ++slot) {
                if (WtPokemon_Decode((const unsigned char *)BoxSlot(image, data, box, slot), canonical) != 0) return -1;
                if (Read16(canonical + 8) != 0) continue;
                found = 1; break;
            }
            if (!found) return 1;
            --box;
            entry->destination.kind = 2; entry->destination.box = box; entry->destination.slot = slot;
            entry->destination.length = sizeof(BoxPkm);
        }
        destination = Destination(image, data, entry->destination);
        if (!destination || Hash(image, data, entry->inventoryBefore) != 0) return -1;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        memcpy(entry->destinationBefore, destination, entry->destination.length);
    }
    destination = Destination(image, data, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && (unsigned)party->count != entry->destination.slot)) return -1;
    if (ReceiveEffects(game, image, data, entry, &pkm) != 0) return -1;
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 2) PML_PkmPPRecover(&pkm.base);
    if (WtPokemon_Decode((const unsigned char *)&pkm, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, &pkm, entry->destination.length);
        if (WtFlash_Hash(canonical, sizeof(canonical), entry->appliedDigest) != 0) return -1;
    } else if (memcmp(entry->destinationAfter, &pkm, entry->destination.length) != 0) return -1;
    memcpy(destination, &pkm, entry->destination.length);
    if (entry->destination.kind == 1) ++party->count;
    return 0;
}

/* A copy of the save image to change; the live save takes it only once its
 * hash is the one the journal recorded. */
static unsigned char *Candidate(WtGame *game, const Image *image)
{
    unsigned char *candidate = WtGame_Alloc(game, image->size);
    if (candidate) memcpy(candidate, image->data, image->size);
    return candidate;
}

static void Discard(WtGame *game, const Image *image, unsigned char *candidate)
{
    mbedtls_platform_zeroize(candidate, image->size);
    WtGame_Free(game, candidate);
}

static int Commit(WtGame *game, const Image *image, const unsigned char *candidate, const unsigned char digest[32])
{
    memcpy(image->data, candidate, image->size);
    return SaveBoth(game, image, digest);
}

/* The save as the shared delivery decisions see it (wonder_trade/delivery.h):
 * a save is the image's data, the live one or a candidate. */
typedef struct Context {
    WtGame *game;
    Image image;
} Context;

static int PartyCount(void *arg, void *data)
{
    const PokeParty *party = Party(&((Context *)arg)->image, data);
    return party->capacity == PARTY_SIZE ? party->count : -1;
}

static unsigned char *Slot(void *arg, void *data, WtJournalLocator where)
{
    return Destination(&((Context *)arg)->image, data, where);
}

/* Every PC and Battle Box slot must decode, as the game reads them; an empty
 * one has no species. */
static int Occupied(void *arg, void *data, WtJournalLocator where)
{
    unsigned char canonical[WT_POKEMON_SIZE];
    const unsigned char *stored = Slot(arg, data, where);
    if (!stored || WtPokemon_Decode(stored, canonical) != 0) return -1;
    return Read16(canonical + 8) != 0;
}

static int SaveHash(void *arg, void *data, unsigned char digest[32])
{
    return Hash(&((Context *)arg)->image, data, digest);
}

static int SaveVerify(void *arg, const unsigned char digest[32])
{
    return Verify(&((Context *)arg)->image, digest);
}

static int SaveCheckpoint(void *arg)
{
    return WtGame_Checkpoint(((Context *)arg)->game);
}

static void *Copy(void *arg)
{
    Context *context = arg;
    return Candidate(context->game, &context->image);
}

static void Drop(void *arg, void *candidate)
{
    Context *context = arg;
    if (candidate) Discard(context->game, &context->image, candidate);
}

static int Install(void *arg, void *candidate, const unsigned char digest[32])
{
    Context *context = arg;
    return Commit(context->game, &context->image, candidate, digest);
}

static void SaveCounters(void *arg, uint32_t counters[3])
{
    WtGame *game = ((Context *)arg)->game;
    counters[0] = counters[1] = counters[2] = SaveData_GetSaveCount(game->save->sv);
}

static int Build(void *arg, void *data, WtJournalEntry *entry, int prepare)
{
    Context *context = arg;
    return BuildReceived(context->game, &context->image, data, entry, prepare);
}

/* The game's empty party Pokémon; its box prefix is the empty box Pokémon. */
static void Empty(void *arg, unsigned char *image, unsigned length)
{
    PartyPkm empty;
    (void)arg;
    PokeParty_ClearPkm(&empty);
    memcpy(image, &empty, length);
}

/* Zero when the save's layout is known. */
static int Ops(WtGame *game, Context *context, WtDeliveryOps *ops)
{
    const WtDeliveryOps ops0 = {
        NULL, NULL, WtFlash_Hash, PartyCount, Slot, Occupied, SaveHash, SaveVerify, SaveCheckpoint,
        Copy, Drop, Install, SaveCounters, Build, Empty
    };
    context->game = game;
    if (Open(game, &context->image) != 0) return -1;
    *ops = ops0; ops->arg = context; ops->live = context->image.data;
    return 0;
}

int WtGame_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGame *game = arg;
    Context context;
    WtDeliveryOps ops;
    Image image;
    unsigned char *candidate, digest[32], canonical[WT_POKEMON_SIZE];
    PokeParty *copy;
    int result = -1, i;
    if (Ops(game, &context, &ops) != 0) return -1;
    image = context.image;
    if (step != WT_INVENTORY_PREPARE_REMOVE && !WtDelivery_Bound(entry)) return -1;
    if (step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE && step != WT_INVENTORY_RESTORE)
        return WtDelivery_Step(&ops, step, entry);
    candidate = Candidate(game, &image);
    if (!candidate) return -1;
    copy = Party(&image, candidate);
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (!Eligible(copy, slot) || WtPokemon_Decode((const unsigned char *)&copy->pkms[slot], canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = slot; entry->source.length = sizeof(PartyPkm);
        memcpy(entry->original, &copy->pkms[slot], sizeof(PartyPkm));
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        if (WtFlash_Hash(entry->original, sizeof(PartyPkm), entry->originalDigest) != 0
            || Hash(&image, candidate, entry->inventoryBefore) != 0) goto done;
        RemoveSlot(copy, slot);
        if (WtDelivery_Unique(&ops, candidate, entry->outgoing) != 0 || Hash(&image, candidate, entry->inventoryAfter) != 0) goto done;
        Counters(game, entry); result = 0; goto done;
    }
    if (slot >= PARTY_SIZE || Hash(&image, candidate, digest) != 0) goto done;
    if (step == WT_INVENTORY_REMOVE) {
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
            if (slot >= (unsigned)copy->count || memcmp(&copy->pkms[slot], entry->original, sizeof(PartyPkm)) != 0) goto done;
            RemoveSlot(copy, slot);
            if (Hash(&image, candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            if (copy->count >= PARTY_SIZE || slot > (unsigned)copy->count) goto done;
            for (i = copy->count; i > (int)slot; --i) copy->pkms[i] = copy->pkms[i - 1];
            memcpy(&copy->pkms[slot], entry->original, sizeof(PartyPkm)); ++copy->count;
            if (Hash(&image, candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryBefore, 32) != 0) goto done;
    }
    if (Commit(game, &image, candidate, digest) != 0) goto done;
    Counters(game, entry); result = 0;
done:
    Discard(game, &image, candidate);
    return result;
}

BOOL WtGameSave_Unique(WtGame *game, unsigned slot)
{
    Context context;
    WtDeliveryOps ops;
    unsigned char *candidate, canonical[WT_POKEMON_SIZE];
    PokeParty *party;
    BOOL unique = TRUE;
    if (Ops(game, &context, &ops) != 0) return TRUE;
    party = Party(&context.image, context.image.data);
    if (slot >= (unsigned)party->count
        || WtPokemon_Decode((const unsigned char *)&party->pkms[slot], canonical) != 0) return TRUE;
    /* Anything uncertain falls through to the deposit's own fail-closed check. */
    candidate = Candidate(game, &context.image);
    if (!candidate) return TRUE;
    RemoveSlot(Party(&context.image, candidate), slot);
    unique = WtDelivery_Unique(&ops, candidate, canonical) == 0;
    Discard(game, &context.image, candidate);
    return unique;
}

int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    BoxPkm box;
    if (entry->deliveryKind != WT_DELIVERY_MATCH || entry->destination.length < sizeof(box)) return -1;
    memcpy(&box, entry->destinationAfter, sizeof(box));
    received->species = (uint16_t)PML_PkmGetParam(&box, PKM_PARAM_SPECIES, NULL);
    received->gender = PML_PkmGetSex(&box);
    received->form = (uint8_t)PML_PkmGetParam(&box, PKM_PARAM_FORM, NULL);
    received->personality = PML_PkmGetParam(&box, PKM_PARAM_PID, NULL);
    received->shiny = PML_PkmIsRare(&box) ? 1 : 0;
    received->hasNickname = PML_PkmGetParam(&box, PKM_PARAM_IS_NICKNAMED, NULL);
    PML_PkmGetParam(&box, PKM_PARAM_NICKNAME_RAW, received->nickname);
    PML_PkmGetParam(&box, PKM_PARAM_OT_NAME_RAW, received->originalTrainer);
    received->originalTrainerGender = (uint8_t)PML_PkmGetParam(&box, PKM_PARAM_OT_GENDER, NULL);
    return 0;
}

static unsigned Forms(unsigned species)
{
    unsigned forms = getNumberOfForms((u16)species);
    return forms ? forms : 1;
}

static int Holdable(void *arg, unsigned item)
{
    (void)arg;
    return !PML_ItemIsUnholdable((u16)item) && !PML_ItemIsDummy((u16)item);
}

int WtGame_CanHold(void *arg, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    const WtPokemonLimits limits = { SPECIES_LAST, ITEM_LAST, BALL_LAST, Forms, Holdable, arg };
    return WtPokemon_Fits(pokemon, &limits);
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    Context context;
    WtDeliveryOps ops;
    return Ops(game, &context, &ops) == 0 && WtDelivery_CanLeave(&ops, entry);
}
