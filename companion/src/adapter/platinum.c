#include "adapter/platinum.h"

#include <stdlib.h>
#include <string.h>

#include "save/sha256.h"
#include "wonder_trade/pokemon_codec.h"

/* The party (Party in the game) begins with its capacity and count, both
 * 32-bit, then six 236-byte Pokémon. The PC (PCBoxes) begins with the current
 * box, then 18 boxes of 30 stored Pokémon. */
enum {
    PARTY_SLOTS = 8,
    MON_SIZE = CP_SAVE_PARTY_STRIDE,
    BOX_MON_SIZE = CP_SAVE_BOX_STRIDE,
    EXTENSION_SIZE = MON_SIZE - BOX_MON_SIZE,
    BOXES = 18,
    BOX_SLOTS = 30,
    BOX_SLOTS_OFFSET = CP_SAVE_BOX_SLOTS_OFFSET - CP_SAVE_BOX_OFFSET,
    /* Bits of the unencrypted word at 4: party and box decrypted, checksum failed. */
    FLAG_DECRYPTED = 3,
    FLAG_CHECKSUM_FAILED = 4,
    /* MON_DATA_SPECIES of a Pokémon whose checksum failed. */
    SPECIES_EGG = 494
};

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }

static void Write32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

static void Sha(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, bytes, size);
    CpSaveSha256_Finish(&hash, digest);
}

/* The game's EncodeData: each halfword XORed with the high half of its LCG. */
static void Crypt(unsigned char *bytes, size_t size, uint32_t seed)
{
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        seed = seed * UINT32_C(1103515245) + 24691;
        bytes[i] ^= (unsigned char)(seed >> 16);
        bytes[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

static unsigned char *Party(CpSaveImage *image)
{
    unsigned char *general = CpSaveImage_Block(image, 0);
    return general ? general + CP_SAVE_PARTY_CAPACITY_OFFSET : NULL;
}

static unsigned char *BoxMon(CpSaveImage *image, unsigned box, unsigned slot)
{
    unsigned char *boxes = CpSaveImage_Block(image, 1);
    return boxes ? boxes + BOX_SLOTS_OFFSET + (box * BOX_SLOTS + slot) * BOX_MON_SIZE : NULL;
}

static unsigned char *Slot(unsigned char *party, unsigned slot)
{
    return party + PARTY_SLOTS + slot * MON_SIZE;
}

/* The party's count when its header is one the game keeps, else -1. */
static int Count(const unsigned char *party, int minimum)
{
    uint32_t capacity = Read32(party), count = Read32(party + 4);
    return capacity == CP_SAVE_PARTY_CAPACITY && count >= (uint32_t)minimum && count <= CP_SAVE_PARTY_CAPACITY
        ? (int)count : -1;
}

/* One Pokémon as Pokemon_GetValue and BoxPokemon_GetValue read it. failed is
 * the game's checksumFailed after the read; mutates says the read would set
 * it in the save; decrypted marks a stored image that claims to be decrypted. */
typedef struct Mon {
    unsigned char box[BOX_MON_SIZE], extension[EXTENSION_SIZE];
    int failed, mutates, decrypted;
} Mon;

static void ReadMon(const unsigned char *image, int party, Mon *mon)
{
    unsigned flags = Read16(image + 4), sum = 0, i;
    (void)WtPokemon_Decode(image, mon->box);
    for (i = 8; i < BOX_MON_SIZE; i += 2) sum += Read16(mon->box + i);
    mon->decrypted = (flags & FLAG_DECRYPTED) != 0;
    mon->mutates = (sum & 65535) != Read16(image + 6) && !(flags & FLAG_CHECKSUM_FAILED);
    mon->failed = (sum & 65535) != Read16(image + 6) || (flags & FLAG_CHECKSUM_FAILED);
    memset(mon->extension, 0, sizeof(mon->extension));
    if (party) {
        memcpy(mon->extension, image + BOX_MON_SIZE, EXTENSION_SIZE);
        Crypt(mon->extension, EXTENSION_SIZE, Read32(image));
    }
}

static unsigned Species(const Mon *mon) { return mon->failed ? SPECIES_EGG : Read16(mon->box + 0x08); }
/* MON_DATA_IS_EGG and MON_DATA_SANITY_IS_EGG: bit 30 of the IV word. */
static int Egg(const Mon *mon) { return mon->failed || ((Read32(mon->box + 0x38) >> 30) & 1); }
static unsigned HeldItem(const Mon *mon) { return Read16(mon->box + 0x0A); }
static unsigned BallCapsule(const Mon *mon) { return mon->extension[5]; }
static unsigned Hp(const Mon *mon) { return Read16(mon->extension + 6); }

/* game_save.c Eligible. A stored image that claims to be decrypted, which the
 * game would read unencrypted, is refused. */
static int Eligible(const unsigned char *party, unsigned slot)
{
    Mon mon;
    int count = Count(party, 2), usable = 0, i;
    unsigned item;
    if (count < 0 || slot >= (unsigned)count) return 0;
    ReadMon(party + PARTY_SLOTS + slot * MON_SIZE, 1, &mon);
    item = HeldItem(&mon);
    if (mon.decrypted || !Species(&mon) || Species(&mon) > CP_ADAPTER_MAX_SPECIES || Egg(&mon)
        || BallCapsule(&mon) || (item >= CP_ADAPTER_FIRST_MAIL && item <= CP_ADAPTER_LAST_MAIL)) return 0;
    for (i = 0; i < count; ++i) {
        if ((unsigned)i == slot) continue;
        ReadMon(party + PARTY_SLOTS + i * MON_SIZE, 1, &mon);
        if (mon.decrypted) return 0;
        if (!Egg(&mon) && Hp(&mon) != 0) ++usable;
    }
    return usable != 0;
}

/* game_save.c Absent: no party or PC Pokémon is the outgoing one. An empty PC
 * slot whose checksum fails is refused: the game's read would mark it, so its
 * save would no longer match the escrow's recorded hash. */
static int Absent(CpSaveImage *image, const unsigned char outgoing[BOX_MON_SIZE])
{
    unsigned char canonical[BOX_MON_SIZE];
    unsigned char *party = Party(image);
    unsigned box, slot;
    int count, i;
    Mon mon;
    if (!party || (count = Count(party, 1)) < 0) return -1;
    for (i = 0; i < count; ++i) {
        if (WtPokemon_Decode(Slot(party, (unsigned)i), canonical) != 0
            || memcmp(canonical, outgoing, BOX_MON_SIZE) == 0) return -1;
    }
    for (box = 0; box < BOXES; ++box) for (slot = 0; slot < BOX_SLOTS; ++slot) {
        const unsigned char *stored = BoxMon(image, box, slot);
        ReadMon(stored, 0, &mon);
        if (!Read16(mon.box + 0x08)) {
            if (mon.decrypted || mon.mutates) return -1;
            continue;
        }
        if (WtPokemon_Decode(stored, canonical) != 0 || memcmp(canonical, outgoing, BOX_MON_SIZE) == 0) return -1;
    }
    return 0;
}

/* Pokemon_Init's empty Pokémon (MON_SIZE), or BoxPokemon_Init's
 * (BOX_MON_SIZE): zeros with their blocks encrypted with seed 0. */
static void Empty(unsigned char *image, unsigned size)
{
    memset(image, 0, size);
    Crypt(image + 8, BOX_MON_SIZE - 8, 0);
    if (size == MON_SIZE) Crypt(image + BOX_MON_SIZE, EXTENSION_SIZE, 0);
}

/* Party_RemovePokemonBySlotIndex: later Pokémon move down one slot and the
 * last becomes the empty Pokémon. */
static void Remove(unsigned char *party, unsigned slot)
{
    unsigned count = Read32(party + 4), i;
    for (i = slot; i + 1 < count; ++i) memcpy(Slot(party, i), Slot(party, i + 1), MON_SIZE);
    Empty(Slot(party, i), MON_SIZE);
    Write32(party + 4, count - 1);
}

/* Nonzero when the loaded save is older than the one recorded in the entry. */
static int Behind(const CpSaveImage *image, const WtJournalEntry *entry)
{
    return (uint32_t)(image->globalCounter - entry->globalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(image->blockCounters[0] - entry->generalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(image->blockCounters[1] - entry->boxCounter) >= UINT32_C(0x80000000);
}

static void Counters(const CpSaveImage *image, WtJournalEntry *entry)
{
    entry->globalCounter = image->globalCounter;
    entry->generalCounter = image->blockCounters[0];
    entry->boxCounter = image->blockCounters[1];
}

/* A game saved this save since the entry recorded its counters: none is
 * behind and one has moved on. The patched game settles before it can be
 * played; the stock game, played between companion sessions, does not. */
static int Moved(const CpSaveImage *image, const WtJournalEntry *entry)
{
    return !Behind(image, entry) && (image->globalCounter != entry->globalCounter
        || image->blockCounters[0] != entry->generalCounter || image->blockCounters[1] != entry->boxCounter);
}

/* Where a save holds a Pokémon: the slot whose canonical form digests to one
 * of `digests`, the images the step can have left. */
typedef struct Found {
    int party;
    unsigned box, slot;
} Found;

/* 1 found, 0 absent. -1 when a slot cannot be read as the game would, or when
 * a Pokémon shares the identity (personality and original trainer ID) without
 * being one of those images, or two do: another Pokémon, or this one changed
 * by play, is never proof either way. */
static int Locate(CpSaveImage *image, const unsigned char target[BOX_MON_SIZE], unsigned char (*digests)[32],
    unsigned count, Found *found)
{
    unsigned char canonical[BOX_MON_SIZE], sum[32];
    unsigned char *party = Party(image);
    unsigned box, slot, i, j, n;
    int members, matches = 0;
    Mon mon;
    if (!party || (members = Count(party, 1)) < 0) return -1;
    n = (unsigned)members + BOXES * BOX_SLOTS;
    for (i = 0; i < n; ++i) {
        const unsigned char *stored;
        int inParty = i < (unsigned)members;
        box = inParty ? 0 : (i - (unsigned)members) / BOX_SLOTS;
        slot = inParty ? i : (i - (unsigned)members) % BOX_SLOTS;
        stored = inParty ? Slot(party, slot) : BoxMon(image, box, slot);
        if (!inParty) {
            ReadMon(stored, 0, &mon);
            if (!Read16(mon.box + 0x08)) {
                if (mon.decrypted || mon.mutates) return -1;
                continue;
            }
        }
        if (WtPokemon_Decode(stored, canonical) != 0) return -1;
        if (memcmp(canonical, target, 4) != 0 || memcmp(canonical + 0x0C, target + 0x0C, 4) != 0) continue;
        Sha(canonical, sizeof(canonical), sum);
        for (j = 0; j < count && memcmp(sum, digests[j], 32) != 0; ++j) {}
        if (j == count || ++matches > 1) return -1;
        found->party = inParty; found->box = box; found->slot = slot;
    }
    return matches;
}

static int Hash(const CpSaveImage *image, unsigned char digest[32])
{
    return CpSaveImage_Hash(image, digest) == CP_SAVE_OK ? 0 : -1;
}

/* game_save.c SaveBoth: both retail copies hold exactly this image, written
 * twice the way the game saves when they do not already. */
static int SaveBoth(WtGame *game, const unsigned char expected[32])
{
    unsigned char actual[32];
    if (Hash(game->image, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (CpSaveImage_Verify(game->image, game->file, expected) == CP_SAVE_OK) return 0;
    return CpSaveImage_CommitBoth(game->image, game->file, expected) == CP_SAVE_OK ? 0 : -1;
}

int WtGame_Checkpoint(WtGame *game)
{
    unsigned char digest[32];
    if (!game || !game->image || Hash(game->image, digest) != 0) return -1;
    return CpSaveImage_Verify(game->image, game->file, digest) == CP_SAVE_OK ? 0 : SaveBoth(game, digest);
}

/* What a step may change, kept to put the image back when it fails or only
 * prepares: the general block (the party and everything a delivery's effects
 * reach) and the one PC slot a delivery may fill. */
typedef struct Backup {
    unsigned char general[CP_SAVE_GENERAL_SIZE - CP_SAVE_FOOTER_SIZE], box[BOX_MON_SIZE];
    unsigned char *generalAt, *boxAt;
} Backup;

static Backup *Keep(CpSaveImage *image)
{
    Backup *backup = malloc(sizeof(*backup));
    if (!backup) return NULL;
    backup->generalAt = CpSaveImage_Block(image, 0); backup->boxAt = NULL;
    if (!backup->generalAt) { free(backup); return NULL; }
    memcpy(backup->general, backup->generalAt, sizeof(backup->general));
    return backup;
}

static void KeepBox(Backup *backup, unsigned char *box)
{
    backup->boxAt = box;
    memcpy(backup->box, box, sizeof(backup->box));
}

static void Release(Backup *backup, int restore)
{
    if (restore) {
        memcpy(backup->generalAt, backup->general, sizeof(backup->general));
        if (backup->boxAt) memcpy(backup->boxAt, backup->box, sizeof(backup->box));
    }
    memset(backup, 0, sizeof(*backup));
    free(backup);
}

/* The escrow an entry records: the original party image and its canonical form. */
static int Bound(const WtJournalEntry *entry)
{
    unsigned char canonical[BOX_MON_SIZE];
    return entry->source.kind == 1 && entry->source.length == MON_SIZE
        && WtPokemon_Decode(entry->original, canonical) == 0
        && memcmp(canonical, entry->outgoing, BOX_MON_SIZE) == 0;
}

static int Removed(WtGame *game, const WtJournalEntry *entry)
{
    if (!Bound(entry)) return -1;
    /* A selected older retail block cannot be promoted to a new checkpoint. */
    if (Behind(game->image, entry) || Absent(game->image, entry->outgoing) != 0) return -1;
    return WtGame_Checkpoint(game);
}

static unsigned char *Destination(CpSaveImage *image, WtJournalLocator where)
{
    unsigned char *party = Party(image);
    if (party && where.kind == 1 && where.box == 0 && where.slot < CP_SAVE_PARTY_CAPACITY && where.length == MON_SIZE)
        return Slot(party, where.slot);
    if (where.kind == 2 && where.box < BOXES && where.slot < BOX_SLOTS && where.length == BOX_MON_SIZE)
        return BoxMon(image, where.box, where.slot);
    return NULL;
}

/* The mail sentence IDs of a prepared party image, which the game keeps when
 * it applies the delivery (its mail constructor leaves them unset). */
static void KeptSentences(const unsigned char image[MON_SIZE], uint16_t kept[3])
{
    unsigned char extension[EXTENSION_SIZE];
    unsigned i;
    memcpy(extension, image + BOX_MON_SIZE, EXTENSION_SIZE);
    Crypt(extension, EXTENSION_SIZE, Read32(image));
    for (i = 0; i < 3; ++i) kept[i] = (uint16_t)Read16(extension + 54 + 8 * i);
}

/* game_save.c BuildReceived: the Pokémon goes to the end of the party, or the
 * first empty PC slot when the party is full; 1 when there is no room. A
 * returned offer is its original image; a matched one is built with the
 * game's receive effects, which also change the rest of the general block. */
static int BuildReceived(WtGame *game, WtJournalEntry *entry, Backup *backup, int prepare)
{
    CpSaveImage *image = game->image;
    unsigned char *party = Party(image), *destination, canonical[BOX_MON_SIZE], built[MON_SIZE];
    uint16_t kept[3];
    unsigned box, slot;
    int count, found;
    Mon mon;
    if (!party) return -1;
    if (prepare) {
        if ((count = Count(party, 1)) < 0) return -1;
        /* A returned offer that is already back must never be added twice:
         * fail closed (Back and ReceiveMoved settle that case first). The game
         * has no such check; RECEIVE only follows a PREPARE on the same image. */
        if (entry->deliveryKind == WT_DELIVERY_RETURN && Absent(image, entry->outgoing) != 0) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (count < CP_SAVE_PARTY_CAPACITY) {
            entry->destination.kind = 1; entry->destination.slot = (unsigned char)count;
            entry->destination.length = MON_SIZE;
        } else {
            found = 0;
            for (box = 0; box < BOXES && !found; ++box) for (slot = 0; slot < BOX_SLOTS; ++slot) {
                const unsigned char *stored = BoxMon(image, box, slot);
                ReadMon(stored, 0, &mon);
                if (mon.decrypted || mon.mutates) return -1;
                if (Read16(mon.box + 0x08)) continue;
                if (WtPokemon_Decode(stored, canonical) != 0) return -1;
                found = 1; break;
            }
            if (!found) return 1;
            --box;
            entry->destination.kind = 2; entry->destination.box = (unsigned char)box;
            entry->destination.slot = (unsigned char)slot; entry->destination.length = BOX_MON_SIZE;
        }
        destination = Destination(image, entry->destination);
        if (!destination || Hash(image, entry->inventoryBefore) != 0) return -1;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        memcpy(entry->destinationBefore, destination, entry->destination.length);
    }
    destination = Destination(image, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && Read32(party + 4) != entry->destination.slot)) return -1;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, BOX_MON_SIZE) != 0) return -1;
        memcpy(built, entry->original, MON_SIZE);
    } else if (entry->deliveryKind == WT_DELIVERY_MATCH && game->data) {
        if (!prepare && entry->destination.kind == 1) {
            if (WtPokemon_Decode(entry->destinationAfter, canonical) != 0) return -1;
            KeptSentences(entry->destinationAfter, kept);
        }
        if (CpReceive_Apply(game->data, CpSaveImage_Block(image, 0), entry->incoming,
                entry->destination.kind == 1 ? CP_RECEIVE_PARTY : CP_RECEIVE_BOX,
                !prepare && entry->destination.kind == 1 ? kept : NULL, built) != 0) return -1;
    } else return -1;
    if (WtPokemon_Decode(built, canonical) != 0) return -1;
    if (prepare) {
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, built, entry->destination.length);
        Sha(canonical, sizeof(canonical), entry->appliedDigest);
    } else if (memcmp(entry->destinationAfter, built, entry->destination.length) != 0) return -1;
    if (entry->destination.kind == 2) KeepBox(backup, destination);
    memcpy(destination, built, entry->destination.length);
    if (entry->destination.kind == 1) Write32(party + 4, Read32(party + 4) + 1);
    return 0;
}

static int VerifyReceived(WtGame *game, const WtJournalEntry *entry)
{
    unsigned char digest[32], canonical[BOX_MON_SIZE];
    unsigned char *destination = Destination(game->image, entry->destination);
    if (!destination || memcmp(destination, entry->destinationAfter, entry->destination.length) != 0
        || WtPokemon_Decode(destination, canonical) != 0) return -1;
    Sha(canonical, sizeof(canonical), digest);
    if (memcmp(digest, entry->appliedDigest, 32) != 0
        || Hash(game->image, digest) != 0 || memcmp(digest, entry->inventoryAfter, 32) != 0) return -1;
    return CpSaveImage_Verify(game->image, game->file, entry->inventoryAfter) == CP_SAVE_OK ? 0 : -1;
}

/* A delivery applied earlier: either the save recorded with it, or one saved
 * after it while the ACK waited. An older copy is refused. */
static int Applied(WtGame *game, const WtJournalEntry *entry)
{
    unsigned char digest[32];
    if (Hash(game->image, digest) != 0) return -1;
    if (memcmp(digest, entry->inventoryAfter, 32) == 0) return VerifyReceived(game, entry);
    if (Behind(game->image, entry)) return -1;
    return WtGame_Checkpoint(game);
}

/* The Pokémon a delivery brings, where the save holds it. Proof is its exact
 * image: a returned offer as it left, a matched one as the receive effects
 * build it for the party or for the PC (they depend only on the Pokémon and
 * the ROM), or as recorded when it was prepared. */
static int Delivered(WtGame *game, const WtJournalEntry *entry, Found *found)
{
    unsigned char digests[3][32], image[MON_SIZE], canonical[BOX_MON_SIZE], *general, *scratch;
    const size_t size = CP_SAVE_GENERAL_SIZE - CP_SAVE_FOOTER_SIZE;
    unsigned count = 1, i;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        Sha(entry->outgoing, BOX_MON_SIZE, digests[0]);
        return Locate(game->image, entry->outgoing, digests, 1, found);
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || !game->data || !(general = CpSaveImage_Block(game->image, 0))
        || !(scratch = malloc(size))) return -1;
    memcpy(digests[0], entry->appliedDigest, 32);
    for (i = 0; i < 2; ++i) {
        memcpy(scratch, general, size);
        if (CpReceive_Apply(game->data, scratch, entry->incoming, i ? CP_RECEIVE_BOX : CP_RECEIVE_PARTY, NULL, image) != 0
            || WtPokemon_Decode(image, canonical) != 0) { free(scratch); return -1; }
        Sha(canonical, sizeof(canonical), digests[count++]);
    }
    free(scratch);
    return Locate(game->image, entry->incoming, digests, count, found);
}

/* PREPARE_RECEIVE of a returned offer that is already back: a restore that
 * finished before the companion could record it, then a game save over both
 * recorded inventories. 1 when it is back and RECEIVE need only checkpoint. */
static int Back(WtGame *game, const WtJournalEntry *entry)
{
    Found found;
    int where;
    if (entry->deliveryKind != WT_DELIVERY_RETURN) return 0;
    where = Delivered(game, entry, &found);
    /* Back in a save no game wrote since: something else put it there. */
    return where == 1 && !Moved(game->image, entry) ? -1 : where;
}

/* RECEIVE when a game saved over both recorded inventories (the stock game,
 * played before the companion could finish). The patched game never gets
 * here: it settles before it can be played. Decided by the Pokémon's exact
 * image (Delivered): present, the delivery was applied (or the offer
 * restored), the save is only checkpointed and the record describes where it
 * is; absent, it is prepared again on this save, where PREPARE_RECEIVE would
 * put it, and applied. Never twice, never dropped. */
static int ReceiveMoved(WtGame *game, WtJournalEntry *entry, Backup *backup, int *restore)
{
    Found found;
    int where = Delivered(game, entry, &found);
    if (where < 0) return -1;
    if (where == 0) {
        /* No room (1) fails too: the delivery stays in the journal. */
        if (BuildReceived(game, entry, backup, 1) != 0 || Hash(game->image, entry->inventoryAfter) != 0) return -1;
        *restore = 0;
        if (SaveBoth(game, entry->inventoryAfter) != 0 || VerifyReceived(game, entry) != 0) return -1;
    } else {
        /* Recorded from the save, not the journal: a recovery that put it
         * elsewhere than prepared (a full party sends it to the PC, with the
         * PC's effects) may have been cut before the engine committed. The
         * engine only commits after RECEIVE, so the record is rebuilt here
         * and the ACK and WtGame_Received report what the save holds. */
        WtJournalLocator where;
        unsigned char *stored, canonical[BOX_MON_SIZE];
        memset(&where, 0, sizeof(where));
        where.kind = found.party ? 1 : 2; where.box = (unsigned char)found.box; where.slot = (unsigned char)found.slot;
        where.length = found.party ? MON_SIZE : BOX_MON_SIZE;
        if (!(stored = Destination(game->image, where)) || WtPokemon_Decode(stored, canonical) != 0) return -1;
        *restore = 0;
        if (WtGame_Checkpoint(game) != 0 || Hash(game->image, entry->inventoryAfter) != 0) return -1;
        /* Every image field follows the destination's length, padded with
         * zeros as the journal requires; the slot held the empty Pokémon. */
        entry->destination = where;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        Empty(entry->destinationBefore, where.length);
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, stored, where.length);
        Sha(canonical, sizeof(canonical), entry->appliedDigest);
    }
    Counters(game->image, entry);
    return 0;
}

static int Receive(WtGame *game, enum WtInventoryStep step, WtJournalEntry *entry)
{
    Backup *backup;
    unsigned char digest[32];
    int result = -1, restore = 1;
    if (step == WT_INVENTORY_VERIFY_RECEIVED) return Applied(game, entry);
    if (!(backup = Keep(game->image))) return -1;
    if (step == WT_INVENTORY_PREPARE_RECEIVE) {
        result = Back(game, entry);
        if (result == 1) { result = 0; goto done; }
        if (result == 0) result = BuildReceived(game, entry, backup, 1);
        if (result == 0) result = Hash(game->image, entry->inventoryAfter);
        goto done;
    }
    if (Hash(game->image, digest) != 0) goto done;
    if (memcmp(digest, entry->inventoryBefore, 32) != 0 && memcmp(digest, entry->inventoryAfter, 32) != 0
        && Moved(game->image, entry)) {
        result = ReceiveMoved(game, entry, backup, &restore);
        goto done;
    }
    if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
        if (BuildReceived(game, entry, backup, 0) != 0 || Hash(game->image, digest) != 0) goto done;
    }
    if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    restore = 0;
    if (SaveBoth(game, digest) != 0 || VerifyReceived(game, entry) != 0) goto done;
    Counters(game->image, entry); result = 0;
done:
    Release(backup, restore);
    return result;
}

/* REMOVE when a game saved over both recorded inventories (the stock game,
 * played before the companion could finish an offer). Nothing was sent: an
 * escrow's Submit goes out only once the removal is proven. The engine has no
 * way back from an escrow, and the patched game would finish the removal, so
 * this does too, decided by the Pokémon itself. Still in the party exactly as
 * offered: removed from this save, with the escrow recorded again against it
 * (its slot, image and both inventories), as PREPARE_REMOVE would record it
 * now. Absent (the game loaded the removal): only a checkpoint. Anything else,
 * a changed, moved or unreadable copy, fails closed and changes nothing. */
static int RemoveMoved(WtGame *game, WtJournalEntry *entry, unsigned char *party, int *restore)
{
    CpSaveImage *image = game->image;
    unsigned char digest[1][32];
    Found found;
    int where;
    Sha(entry->outgoing, BOX_MON_SIZE, digest[0]);
    where = Locate(image, entry->outgoing, digest, 1, &found);
    if (where < 0) return -1;
    if (where == 1) {
        if (!found.party || !Eligible(party, found.slot)) return -1;
        entry->source.slot = (unsigned char)found.slot;
        memcpy(entry->original, Slot(party, found.slot), MON_SIZE);
        Sha(entry->original, MON_SIZE, entry->originalDigest);
        if (Hash(image, entry->inventoryBefore) != 0) return -1;
        Remove(party, found.slot);
        if (Absent(image, entry->outgoing) != 0 || Hash(image, entry->inventoryAfter) != 0) return -1;
        *restore = 0;
        if (SaveBoth(game, entry->inventoryAfter) != 0) return -1;
    } else {
        *restore = 0;
        if (WtGame_Checkpoint(game) != 0) return -1;
    }
    Counters(image, entry);
    return 0;
}

static int PartyStep(WtGame *game, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    CpSaveImage *image = game->image;
    unsigned char *party = Party(image), digest[32], canonical[BOX_MON_SIZE];
    Backup *backup;
    int result = -1, restore = 1, count;
    unsigned i;
    if (!party || !(backup = Keep(image))) return -1;
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (!Eligible(party, slot) || WtPokemon_Decode(Slot(party, slot), canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0;
        entry->source.slot = (unsigned char)slot; entry->source.length = MON_SIZE;
        memcpy(entry->original, Slot(party, slot), MON_SIZE);
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        Sha(entry->original, MON_SIZE, entry->originalDigest);
        if (Hash(image, entry->inventoryBefore) != 0) goto done;
        Remove(party, slot);
        if (Absent(image, entry->outgoing) != 0 || Hash(image, entry->inventoryAfter) != 0) goto done;
        Counters(image, entry); result = 0; goto done;
    }
    if (entry->source.kind != 1 || entry->source.length != MON_SIZE || slot >= CP_SAVE_PARTY_CAPACITY
        || Hash(image, digest) != 0) goto done;
    if (step == WT_INVENTORY_REMOVE && memcmp(digest, entry->inventoryBefore, 32) != 0
        && memcmp(digest, entry->inventoryAfter, 32) != 0 && Moved(image, entry)) {
        result = RemoveMoved(game, entry, party, &restore);
        goto done;
    }
    if (step == WT_INVENTORY_REMOVE) {
        if (memcmp(digest, entry->inventoryBefore, 32) == 0) {
            count = Count(party, 1);
            if (count < 0 || slot >= (unsigned)count || memcmp(Slot(party, slot), entry->original, MON_SIZE) != 0) goto done;
            Remove(party, slot);
            if (Hash(image, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryAfter, 32) != 0) goto done;
    } else {
        if (memcmp(digest, entry->inventoryAfter, 32) == 0) {
            count = Count(party, 0);
            if (count < 0 || count >= CP_SAVE_PARTY_CAPACITY || slot > (unsigned)count) goto done;
            for (i = (unsigned)count; i > slot; --i) memcpy(Slot(party, i), Slot(party, i - 1), MON_SIZE);
            memcpy(Slot(party, slot), entry->original, MON_SIZE);
            Write32(party + 4, (uint32_t)count + 1);
            if (Hash(image, digest) != 0) goto done;
        }
        if (memcmp(digest, entry->inventoryBefore, 32) != 0) goto done;
    }
    restore = 0;
    if (SaveBoth(game, digest) != 0) goto done;
    Counters(image, entry); result = 0;
done:
    Release(backup, restore);
    return result;
}

int WtGame_Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    WtGame *game = arg;
    unsigned char digest[32];
    if (!game || !game->image || !game->image->loaded || !entry) return -1;
    if (step != WT_INVENTORY_PREPARE_REMOVE && !Bound(entry)) return -1;
    switch (step) {
    case WT_INVENTORY_VERIFY_REMOVED:
        return Removed(game, entry);
    case WT_INVENTORY_PREPARE_RESTORE:
        if (Hash(game->image, digest) != 0) return -1;
        return memcmp(digest, entry->inventoryBefore, 32) == 0 || memcmp(digest, entry->inventoryAfter, 32) == 0 ? 0 : 1;
    case WT_INVENTORY_PREPARE_RECEIVE: case WT_INVENTORY_RECEIVE: case WT_INVENTORY_VERIFY_RECEIVED:
        return Receive(game, step, entry);
    case WT_INVENTORY_PREPARE_REMOVE: case WT_INVENTORY_REMOVE: case WT_INVENTORY_RESTORE:
        return PartyStep(game, step, entry, slot);
    default:
        return -1;
    }
}

/* game_save.c WtGame_Received: the matched Pokémon as saved, for display. The
 * gender is the one saved with it, which the game computes from the same
 * species and personality (every trade evolution keeps the gender ratio). */
int WtGame_Received(const WtJournalEntry *entry, WtReceivedPokemon *received)
{
    unsigned char canonical[BOX_MON_SIZE];
    uint32_t otId;
    unsigned i;
    if (!entry || !received || entry->deliveryKind != WT_DELIVERY_MATCH || entry->destination.length < BOX_MON_SIZE
        || WtPokemon_Decode(entry->destinationAfter, canonical) != 0) return -1;
    received->species = (uint16_t)Read16(canonical + 0x08);
    received->gender = (canonical[0x40] >> 1) & 3;
    received->form = canonical[0x40] >> 3;
    received->personality = Read32(canonical);
    otId = Read32(canonical + 0x0C);
    received->shiny = ((otId >> 16) ^ (otId & 65535) ^ (received->personality >> 16) ^ (received->personality & 65535)) < 8;
    received->hasNickname = (int)(Read32(canonical + 0x38) >> 31);
    for (i = 0; i < WT_GAME_NAME_LENGTH; ++i) received->nickname[i] = (uint16_t)Read16(canonical + 0x48 + 2 * i);
    received->nickname[i] = 0xFFFF;
    for (i = 0; i < WT_GAME_TRAINER_NAME_LENGTH; ++i) received->originalTrainer[i] = (uint16_t)Read16(canonical + 0x68 + 2 * i);
    received->originalTrainer[i] = 0xFFFF;
    received->originalTrainerGender = canonical[0x84] >> 7;
    return 0;
}

int WtGame_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    if (!game || !game->image || !game->image->loaded || !entry) return 0;
    switch (entry->phase) {
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_IDLE: case WT_JOURNAL_ENROLLING: return 1;
    case WT_JOURNAL_SUBMIT_READY: case WT_JOURNAL_QUEUED: case WT_JOURNAL_RECEIVED: case WT_JOURNAL_RESTORING:
        return Removed(game, entry) == 0;
    case WT_JOURNAL_APPLIED: return Applied(game, entry) == 0;
    default: return 0;
    }
}

int CpAdapter_Eligible(const CpSaveImage *image, unsigned slot)
{
    const unsigned char *general = image && image->loaded
        ? image->bytes + image->currentHalf[0] * CP_SAVE_HALF_SIZE + CP_SAVE_GENERAL_OFFSET : NULL;
    return general && Eligible(general + CP_SAVE_PARTY_CAPACITY_OFFSET, slot);
}

int CpAdapter_Unique(CpSaveImage *image, unsigned slot)
{
    unsigned char *party = Party(image), canonical[BOX_MON_SIZE];
    Backup *backup;
    int count, unique;
    if (!party || (count = Count(party, 1)) < 0 || slot >= (unsigned)count
        || WtPokemon_Decode(Slot(party, slot), canonical) != 0) return 1;
    /* Anything uncertain falls through to the deposit's own fail-closed check. */
    if (!(backup = Keep(image))) return 1;
    Remove(party, slot);
    unique = Absent(image, canonical) == 0;
    Release(backup, 1);
    return unique;
}

/* The species with forms, and their counts, are those Pokemon_SanitizeFormId
 * keeps; it resets any other form to 0. */
unsigned CpAdapter_Forms(unsigned species)
{
    switch (species) {
    case 201: return 28; /* Unown */
    case 351: return 4;  /* Castform */
    case 386: return 4;  /* Deoxys */
    case 412: return 3;  /* Burmy */
    case 413: return 3;  /* Wormadam */
    case 421: return 2;  /* Cherrim */
    case 422: return 2;  /* Shellos */
    case 423: return 2;  /* Gastrodon */
    case 479: return 6;  /* Rotom */
    case 487: return 2;  /* Giratina */
    case 492: return 2;  /* Shaymin */
    case 493: return 18; /* Arceus */
    default: return 1;
    }
}

static int Holdable(void *arg, unsigned item)
{
    const WtGame *game = arg;
    return game->holdable && game->holdable(game->holdableArg, item);
}

int WtGame_CanHold(void *arg, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    WtPokemonLimits limits;
    if (!arg || !pokemon) return 0;
    limits.maxSpecies = CP_ADAPTER_MAX_SPECIES; limits.maxItem = CP_ADAPTER_MAX_ITEM;
    limits.maxBall = CP_ADAPTER_MAX_BALL; limits.forms = CpAdapter_Forms;
    limits.holdable = Holdable; limits.arg = arg;
    return WtPokemon_Fits(pokemon, &limits);
}
