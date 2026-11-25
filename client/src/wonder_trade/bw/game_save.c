#include "wonder_trade/bw/game_save.h"

#include <nitro.h>
#include <string.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#include <mbedtls/platform_util.h>

#include "wonder_trade/bw2/musical_area.h"
#include "wonder_trade/flash.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/recovery.h"
#include "wonder_trade/receive_guard.h"

/* The main save (SAVEDATA_BuildLayout, arm9/backup_system.c): two copies of
 * 0x24000 bytes, the backup at 0x24000; each holds the blocks, each followed
 * by a {count, CRC} footer, then a CRC of each block and the 16-byte footer
 * (protocol/save-layout-bw.json). */
#define MAIN_COPY 0x0
#define BACKUP_COPY 0x24000
#define SAVE_USED 0x23F9C
#define SAVE_FOOTER_SIZE 16
#define BLOCK_FOOTER_SIZE 4
#define SAVE_BLOCK_MAX 69
#define SAVE_BLOCK_BOX_TRAY 1
#define SAVE_BLOCK_PARTY 0x1A
#define SAVE_BLOCK_BATTLE_BOX 0x31
#define SAVE_BLOCK_POKEDEX 0x37
#define BOX_MAX_TRAY 24
#define BOX_MAX_POS 30
#define BATTLE_BOX_SLOTS 6
#define PARTY_SIZE WT_PARTY_SIZE
#define SPECIES_LAST 649 /* Genesect */
#define ITEM_LAST 626    /* the last item of Black and White */
#define BALL_LAST 25     /* the Dream Ball's ball index */
#define MUSICAL_DIST_MARKER 6

typedef WtBoxPokemon BoxPkm;
typedef WtPartyPokemon PartyPkm;
typedef WtParty PokeParty;
typedef struct BoxTraySave {
    BoxPkm pkm[BOX_MAX_POS];
} BoxTraySave;
typedef struct BattleBoxSave {
    BoxPkm pkm[1][BATTLE_BOX_SLOTS];
} BattleBoxSave;

static struct BackupSaveData *MainSave(struct SaveControl *save)
{
    return *(struct BackupSaveData **)((u8 *)save + WT_SAVE_CONTROL_MAIN_SAVE);
}

static u32 SaveCount(struct BackupSaveData *sv)
{
    return *(u32 *)((u8 *)sv + WT_BACKUP_SAVE_COUNT);
}

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

typedef struct Image {
    struct BackupSaveData *sv;
    struct SaveDataTable *table;
    unsigned char *data;
    u32 size, blocks, crcsSize;
} Image;

static int Open(const WtGame *game, Image *image)
{
    image->sv = MainSave(game->save);
    image->table = *(struct SaveDataTable **)((u8 *)image->sv + WT_BACKUP_TABLE);
    image->data = BACKUP_SYSTEM_GetWork(image->sv, &image->size);
    image->blocks = SAVEDATA_GetTableMax(image->table);
    image->crcsSize = image->blocks * 2 + (image->blocks & 1) * 2;
    return image->size == SAVE_USED && image->blocks == SAVE_BLOCK_MAX
        && sizeof(BoxPkm) == WT_POKEMON_SIZE && sizeof(PartyPkm) == WT_GAME_PARTY_SIZE ? 0 : -1;
}

static void *Block(const Image *image, unsigned char *data, u32 id)
{
    return data + SAVEDATA_GetBlockOffset(image->table, id);
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
        if (mbedtls_sha256_update(&hash, data + SAVEDATA_GetBlockOffset(image->table, id),
                SAVEDATA_GetBlockSize(image->table, id)) != 0) goto done;
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
            start = SAVEDATA_GetBlockOffset(image->table, id);
            length = SAVEDATA_GetBlockSize(image->table, id);
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
        if (attempt) BACKUP_SYSTEM_FillBlockAreas(image->sv, WT_BLOCK_AREA_MAIN);
        SAVE_CONTROL_StartSave(game->save);
        do {
            status = SAVE_CONTROL_MainSave(game->save);
            if (status == WT_SAVE_CONTINUE || status == WT_SAVE_LAST) OS_Sleep(1);
        } while (status == WT_SAVE_CONTINUE || status == WT_SAVE_LAST);
        if (status == WT_SAVE_OK) result = Verify(image, expected);
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
    BOOL marked = SYSTEM_DATA_IsMarked(game->save, MUSICAL_DIST_MARKER);
    if (state < 0) return -1;
    if (state == WT_MUSICAL_AREA_JOURNAL && !marked) return 0;
    if (marked) {
        SYSTEM_DATA_ClearMark(game->save, MUSICAL_DIST_MARKER);
        MUSICAL_SAVE_SetHasDistData(MUSICAL_SAVE_GetFromSave(game->save), 0);
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
    u32 count = SaveCount(MainSave(game->save));
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
    pkm = &party->members[slot];
    species = POKE_TOOL_GetPartyValue(pkm, WT_PARAM_SPECIES, 0);
    if (species == 0 || species > SPECIES_LAST
        || POKE_TOOL_GetPartyValue(pkm, WT_PARAM_EGG_OR_BAD_EGG, 0)
        || FILE_UTIL_IsMailItem((u16)POKE_TOOL_GetPartyValue(pkm, WT_PARAM_ITEM, 0))) return 0;
    for (i = 0; i < party->count; ++i) {
        if ((unsigned)i != slot && !POKE_TOOL_GetPartyValue(&party->members[i], WT_PARAM_EGG_OR_BAD_EGG, 0)
            && POKE_TOOL_GetPartyValue(&party->members[i], WT_PARAM_HP, 0) != 0) ++usable;
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
    for (i = (int)slot; i < party->count - 1; ++i) party->members[i] = party->members[i + 1];
    POKE_TOOL_ClearPartyParam(&party->members[party->count - 1]);
    --party->count;
}

/* No copy of the outgoing Pokémon remains in the party, the PC or the
 * Battle Box, and every Pokémon there can be read. */
static int ScanOutgoing(const Image *image, unsigned char *data, const unsigned char outgoing[136], WtReceiveScan *scan)
{
    PokeParty *party = Party(image, data);
    BattleBoxSave *battleBox = Block(image, data, SAVE_BLOCK_BATTLE_BOX);
    unsigned i, box;
    memset(scan, 0, sizeof(*scan));
    if (party->capacity != PARTY_SIZE || party->count < 1 || party->count > PARTY_SIZE) return -1;
    for (i = 0; i < (unsigned)party->count; ++i) {
        if (WtReceive_Inspect(scan, outgoing, (const unsigned char *)&party->members[i],
                1, 0, i, sizeof(PartyPkm)) != 0) return -1;
    }
    for (box = 0; box < BOX_MAX_TRAY; ++box) for (i = 0; i < BOX_MAX_POS; ++i) {
        if (WtReceive_Inspect(scan, outgoing, (const unsigned char *)BoxSlot(image, data, box, i),
                2, box, i, sizeof(BoxPkm)) != 0) return -1;
    }
    /* Battle Box locators are not representable in the journal. A retained
     * identity there still makes delivery unsafe, even with a PC match. */
    for (i = 0; i < BATTLE_BOX_SLOTS; ++i) {
        if (WtReceive_Inspect(scan, outgoing, (const unsigned char *)&battleBox->pkm[0][i],
                2, BOX_MAX_TRAY, i, sizeof(BoxPkm)) != 0 || (scan->matches != 0 && scan->where.box == BOX_MAX_TRAY)) return -1;
    }
    return 0;
}

static int Absent(const Image *image, unsigned char *data, const unsigned char outgoing[136])
{
    WtReceiveScan scan;
    return ScanOutgoing(image, data, outgoing, &scan) == 0 && scan.matches == 0 ? 0 : -1;
}

/* Nonzero when the loaded save is older than the one recorded in the entry. */
static int Behind(const WtGame *game, const WtJournalEntry *entry)
{
    return (u32)(SaveCount(MainSave(game->save)) - entry->globalCounter) >= 0x80000000;
}

static int Removed(WtGame *game, const WtJournalEntry *entry)
{
    Image image;
    unsigned char canonical[WT_POKEMON_SIZE];
    if (Open(game, &image) != 0 || entry->source.kind != 1 || entry->source.length != sizeof(PartyPkm)
        || WtPokemon_Decode(entry->original, canonical) != 0
        || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0) return -1;
    /* A selected older retail copy cannot be promoted to a new checkpoint. */
    if (Behind(game, entry) || Absent(&image, image.data, entry->outgoing) != 0) return -1;
    return WtGame_Checkpoint(game);
}

static unsigned char *Destination(const Image *image, unsigned char *data, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < PARTY_SIZE && where.length == sizeof(PartyPkm))
        return (unsigned char *)&Party(image, data)->members[where.slot];
    if (where.kind == 2 && where.box < BOX_MAX_TRAY && where.slot < BOX_MAX_POS && where.length == sizeof(BoxPkm))
        return (unsigned char *)BoxSlot(image, data, where.box, where.slot);
    return NULL;
}

/* A traded Pokémon's effects in the receiving game: its friendship returns to
 * its species' base, it is registered as caught, and it evolves by trade as
 * the game's own trades check it (CheckEvolveSpecies with WT_EVO_CHECK_TRADE),
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
    pkm->box = box;
    POKE_TOOL_RecalcParam(pkm);
    POKE_TOOL_HealFull(pkm);
    POKE_TOOL_SetPartyValue(pkm, WT_PARAM_HAPPINESS,
        POKE_TOOL_GetPersonalParam((u16)POKE_TOOL_GetPartyValue(pkm, WT_PARAM_SPECIES, 0),
            (u16)POKE_TOOL_GetPartyValue(pkm, WT_PARAM_FORM, 0), WT_PERSONAL_FRIENDSHIP));
    memcpy(&partner, entry->original, sizeof(partner));
    target = SHINKA_CHECK_CheckEvolution(Party(image, data), pkm, WT_EVO_CHECK_TRADE, (u32)&partner, 0, &method, game->heapId);
    mbedtls_platform_zeroize(&partner, sizeof(partner));
    if (target) {
        POKE_TOOL_ChangeSpecies(pkm, target);
        if (method == WT_EVO_METHOD_TRADE_WITH_ITEM) POKE_TOOL_SetPartyValue(pkm, WT_PARAM_ITEM, 0);
    }
    ZUKAN_SAVE_RegisterCaught(Block(image, data, SAVE_BLOCK_POKEDEX), pkm);
    return 0;
}

/* An offer already returned in a later retail save needs no insertion. */
static int PrepareDelivery(WtGame *game, const Image *image, unsigned char *data, WtJournalEntry *entry)
{
    WtReceiveScan scan;
    PartyPkm empty;
    int result;
    if (ScanOutgoing(image, data, entry->outgoing, &scan) != 0) return -1;
    result = WtReceive_Check(entry, &scan, SaveCount(MainSave(game->save)), SaveCount(MainSave(game->save)), SaveCount(MainSave(game->save)));
    if (result != 1) return result;
    POKE_TOOL_ClearPartyParam(&empty);
    if (WtReceive_Record(entry, &scan, (const unsigned char *)&empty, WtFlash_Hash) != 0
        || Hash(image, data, entry->inventoryAfter) != 0) return -1;
    memcpy(entry->inventoryBefore, entry->inventoryAfter, 32);
    return 1;
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
    if (entry->deliveryKind == WT_DELIVERY_MATCH && entry->destination.kind == 2) POKE_TOOL_RecoverBoxAllPP(&pkm.box);
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

static int VerifyReceived(WtGame *game, const WtJournalEntry *entry)
{
    Image image;
    unsigned char digest[32], canonical[WT_POKEMON_SIZE];
    unsigned char *destination;
    if (Open(game, &image) != 0) return -1;
    destination = Destination(&image, image.data, entry->destination);
    if (!destination || memcmp(destination, entry->destinationAfter, entry->destination.length) != 0
        || WtPokemon_Decode(destination, canonical) != 0
        || WtFlash_Hash(canonical, sizeof(canonical), digest) != 0 || memcmp(digest, entry->appliedDigest, 32) != 0
        || Hash(&image, image.data, digest) != 0 || memcmp(digest, entry->inventoryAfter, 32) != 0) return -1;
    return Verify(&image, entry->inventoryAfter);
}

/* A delivery applied earlier: either the save recorded with it, or one the
 * game saved after it while the ACK waited. An older copy is refused. */
static int Applied(WtGame *game, const WtJournalEntry *entry)
{
    Image image;
    unsigned char digest[32];
    if (Open(game, &image) != 0 || Hash(&image, image.data, digest) != 0) return -1;
    if (memcmp(digest, entry->inventoryAfter, 32) == 0) return VerifyReceived(game, entry);
    if (Behind(game, entry)) return -1;
    return WtGame_Checkpoint(game);
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

static int Receive(WtGame *game, const Image *image, enum WtInventoryStep step, WtJournalEntry *entry)
{
    unsigned char *candidate, digest[32];
    int result = -1;
    if (step == WT_INVENTORY_VERIFY_RECEIVED) return Applied(game, entry);
    candidate = Candidate(game, image);
    if (!candidate) return -1;
    if (step == WT_INVENTORY_PREPARE_RECEIVE) {
        result = PrepareDelivery(game, image, candidate, entry);
        if (result != 0) { if (result == 1) result = 0; goto done; }
        result = BuildReceived(game, image, candidate, entry, 1);
        if (result == 0) result = Hash(image, candidate, entry->inventoryAfter);
        goto done;
    }
    if (Hash(image, candidate, digest) != 0) goto done;
    if (memcmp(digest, entry->inventoryBefore, 32) != 0 && memcmp(digest, entry->inventoryAfter, 32) != 0) {
        if (PrepareDelivery(game, image, candidate, entry) != 1) goto done;
    }
    if (memcmp(digest, entry->inventoryBefore, 32) == 0 && memcmp(digest, entry->inventoryAfter, 32) != 0) {
        if (BuildReceived(game, image, candidate, entry, 0) != 0 || Hash(image, candidate, digest) != 0) goto done;
    }
    if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    if (Commit(game, image, candidate, digest) != 0 || VerifyReceived(game, entry) != 0) goto done;
    Counters(game, entry); result = 0;
done:
    Discard(game, image, candidate);
    return result;
}

int WtGame_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGame *game = arg;
    Image image;
    unsigned char *candidate, digest[32], canonical[WT_POKEMON_SIZE];
    PokeParty *copy;
    int result = -1, i;
    if (Open(game, &image) != 0) return -1;
    if (step != WT_INVENTORY_PREPARE_REMOVE
        && (entry->source.kind != 1 || entry->source.length != sizeof(PartyPkm)
            || WtPokemon_Decode(entry->original, canonical) != 0
            || memcmp(canonical, entry->outgoing, sizeof(canonical)) != 0)) return -1;
    if (step == WT_INVENTORY_VERIFY_REMOVED) return Removed(game, entry);
    if (step == WT_INVENTORY_PREPARE_RESTORE) {
        if (Hash(&image, image.data, digest) != 0) return -1;
        return memcmp(digest, entry->inventoryBefore, 32) == 0 || memcmp(digest, entry->inventoryAfter, 32) == 0 ? 0 : 1;
    }
    if (step == WT_INVENTORY_PREPARE_RECEIVE || step == WT_INVENTORY_RECEIVE || step == WT_INVENTORY_VERIFY_RECEIVED)
        return Receive(game, &image, step, entry);
    if (step != WT_INVENTORY_PREPARE_REMOVE && step != WT_INVENTORY_REMOVE && step != WT_INVENTORY_RESTORE) return -1;
    candidate = Candidate(game, &image);
    if (!candidate) return -1;
    copy = Party(&image, candidate);
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (!Eligible(copy, slot) || WtPokemon_Decode((const unsigned char *)&copy->members[slot], canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0; entry->source.slot = slot; entry->source.length = sizeof(PartyPkm);
        memcpy(entry->original, &copy->members[slot], sizeof(PartyPkm));
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        if (WtFlash_Hash(entry->original, sizeof(PartyPkm), entry->originalDigest) != 0
            || Hash(&image, candidate, entry->inventoryBefore) != 0) goto done;
        RemoveSlot(copy, slot);
        if (Absent(&image, candidate, entry->outgoing) != 0 || Hash(&image, candidate, entry->inventoryAfter) != 0) goto done;
        Counters(game, entry); result = 0; goto done;
    }
    if (slot >= PARTY_SIZE || Hash(&image, candidate, digest) != 0) goto done;
    if (step == WT_INVENTORY_REMOVE) {
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
            if (slot >= (unsigned)copy->count || memcmp(&copy->members[slot], entry->original, sizeof(PartyPkm)) != 0) goto done;
            RemoveSlot(copy, slot);
            if (Hash(&image, candidate, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            if (copy->count >= PARTY_SIZE || slot > (unsigned)copy->count) goto done;
            for (i = copy->count; i > (int)slot; --i) copy->members[i] = copy->members[i - 1];
            memcpy(&copy->members[slot], entry->original, sizeof(PartyPkm)); ++copy->count;
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
    Image image;
    unsigned char *candidate, canonical[WT_POKEMON_SIZE];
    PokeParty *party;
    BOOL unique = TRUE;
    if (Open(game, &image) != 0) return TRUE;
    party = Party(&image, image.data);
    if (slot >= (unsigned)party->count
        || WtPokemon_Decode((const unsigned char *)&party->members[slot], canonical) != 0) return TRUE;
    /* Anything uncertain falls through to the deposit's own fail-closed check. */
    candidate = Candidate(game, &image);
    if (!candidate) return TRUE;
    RemoveSlot(Party(&image, candidate), slot);
    unique = Absent(&image, candidate, canonical) == 0;
    Discard(game, &image, candidate);
    return unique;
}

int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    BoxPkm box;
    if (entry->deliveryKind != WT_DELIVERY_MATCH || entry->destination.length < sizeof(box)) return -1;
    memcpy(&box, entry->destinationAfter, sizeof(box));
    received->species = (uint16_t)POKE_TOOL_GetBoxValue(&box, WT_PARAM_SPECIES, 0);
    received->gender = POKE_TOOL_GetBoxGender(&box);
    received->form = (uint8_t)POKE_TOOL_GetBoxValue(&box, WT_PARAM_FORM, 0);
    received->personality = POKE_TOOL_GetBoxValue(&box, WT_PARAM_PID, 0);
    received->shiny = POKE_TOOL_IsBoxShiny(&box) ? 1 : 0;
    received->hasNickname = POKE_TOOL_GetBoxValue(&box, WT_PARAM_IS_NICKNAMED, 0);
    POKE_TOOL_GetBoxValue(&box, WT_PARAM_NICKNAME_RAW, (u32)received->nickname);
    POKE_TOOL_GetBoxValue(&box, WT_PARAM_OT_NAME_RAW, (u32)received->originalTrainer);
    received->originalTrainerGender = (uint8_t)POKE_TOOL_GetBoxValue(&box, WT_PARAM_OT_GENDER, 0);
    return 0;
}

static unsigned Forms(unsigned species)
{
    unsigned forms = ZUKAN_SAVE_GetFormCount(species);
    return forms ? forms : 1;
}

/* Black and White let a Pokémon hold any item but a TM, an HM or a key item. */
static int Holdable(void *arg, unsigned item)
{
    WtGame *game = arg;
    u32 pocket;
    if (item > ITEM_LAST) return 0;
    pocket = FILE_UTIL_GetItemParam(item, WT_ITEM_PARAM_POCKET, game->heapId);
    return pocket != WT_POCKET_TMHM && pocket != WT_POCKET_KEY_ITEMS;
}

int WtGame_CanHold(void *arg, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    const WtPokemonLimits limits = { SPECIES_LAST, ITEM_LAST, BALL_LAST, Forms, Holdable, arg };
    return WtPokemon_Fits(pokemon, &limits);
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    switch (entry->phase) {
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_IDLE: case WT_JOURNAL_ENROLLING: return 1;
    case WT_JOURNAL_SUBMIT_READY: case WT_JOURNAL_QUEUED: case WT_JOURNAL_RECEIVED: case WT_JOURNAL_RESTORING:
        return Removed(game, entry) == 0;
    case WT_JOURNAL_APPLIED: return Applied(game, entry) == 0;
    default: return 0;
    }
}
