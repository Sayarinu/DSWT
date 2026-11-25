#include "adapter/gen5.h"

#include <stdlib.h>
#include <string.h>

#include "adapter/gen5_receive.h"
#include "save/gen5_save.h"
#include "save/journal_save.h"
#include "save/musical_area.h"
#include "save/sha256.h"
#include "wonder_trade/pokemon_codec.h"

enum {
    MON_SIZE = 220, BOX_MON_SIZE = 136, EXTENSION_SIZE = MON_SIZE - BOX_MON_SIZE,
    PARTY_SLOTS = 8, PARTY_CAPACITY = 6, PARTY_SIZE = 0x534,
    BOXES = 24, BOX_SLOTS = 30, BATTLE_BOX_SLOTS = 6,
    FLAG_DECRYPTED = 3, FLAG_CHECKSUM_FAILED = 4,
    SPECIES_LAST = 649, SPECIES_EGG = 650, BALL_LAST = 25,
    FIRST_MAIL = 137, LAST_MAIL = 148,
    POKEDEX_MAX = CP_RECEIVE5_B2W2_POKEDEX_SIZE
};

static unsigned Read16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t Read32(const unsigned char *p) { return Read16(p) | ((uint32_t)Read16(p + 2) << 16); }

static void Write32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}

typedef struct Rules {
    CpReceive5Game receive;
    unsigned itemLast;
} Rules;

static const Rules kBlackWhite = { CP_RECEIVE_BLACK_WHITE, 626 };
static const Rules kBlack2White2 = { CP_RECEIVE_BLACK2_WHITE2, 638 };

int CpGen5_Is(const CpSaveImage *image)
{
    return image && (image->layout == &CpSaveLayout_BlackWhite || image->layout == &CpSaveLayout_Black2White2);
}

static const Rules *RulesOf(const CpSaveImage *image)
{
    return image->layout == &CpSaveLayout_Black2White2 ? &kBlack2White2 : &kBlackWhite;
}

static const CpSaveGen5 *Layout(const CpSaveImage *image) { return image->layout->gen5; }

static void Sha(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    CpSaveSha256 hash;
    CpSaveSha256_Init(&hash);
    CpSaveSha256_Update(&hash, bytes, size);
    CpSaveSha256_Finish(&hash, digest);
}

static void Crypt(unsigned char *bytes, size_t size, uint32_t seed)
{
    size_t i;
    for (i = 0; i + 1 < size; i += 2) {
        seed = seed * UINT32_C(1103515245) + 24691;
        bytes[i] ^= (unsigned char)(seed >> 16);
        bytes[i + 1] ^= (unsigned char)(seed >> 24);
    }
}

/* Blocks of a loaded image; the image is const where only read. */
static unsigned char *BlockOf(const CpSaveImage *image, unsigned id)
{
    return (unsigned char *)image->bytes + image->blockOffsets[id];
}

static unsigned char *Party(const CpSaveImage *image) { return BlockOf(image, Layout(image)->partyBlock); }
static unsigned char *Slot(unsigned char *party, unsigned slot) { return party + PARTY_SLOTS + slot * MON_SIZE; }
static unsigned char *Pokedex(const CpSaveImage *image) { return BlockOf(image, Layout(image)->pokedexBlock); }

static unsigned char *BoxMon(const CpSaveImage *image, unsigned box, unsigned slot)
{
    return BlockOf(image, Layout(image)->boxTrayBlock + box) + slot * BOX_MON_SIZE;
}

static unsigned char *BattleBoxMon(const CpSaveImage *image, unsigned slot)
{
    return BlockOf(image, Layout(image)->battleBoxBlock) + slot * BOX_MON_SIZE;
}

static int Count(const unsigned char *party, int minimum)
{
    uint32_t capacity = Read32(party), count = Read32(party + 4);
    return capacity == PARTY_CAPACITY && count >= (uint32_t)minimum && count <= PARTY_CAPACITY ? (int)count : -1;
}

/* One Pokémon as POKE_TOOL_Get*Value reads it (adapter/platinum.c ReadMon):
 * failed is the game's checksum failure after the read, mutates that the read
 * would mark it in the save, decrypted a stored image claiming to be decrypted. */
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
static int Egg(const Mon *mon) { return mon->failed || ((Read32(mon->box + 0x38) >> 30) & 1); }
static unsigned Hp(const Mon *mon) { return Read16(mon->extension + 6); }

static unsigned HeldItem(const Rules *rules, const Mon *mon)
{
    unsigned item = Read16(mon->box + 0x0A);
    return item > rules->itemLast ? 0 : item;
}

/* game_save.c Eligible, and why it refuses. A read that would mark a slot in
 * the save, or a slot claiming to be decrypted, refuses the offer. */
static int Reason(const Rules *rules, const unsigned char *party, unsigned slot)
{
    Mon mon;
    int count = Count(party, 2), usable = 0, i;
    unsigned item;
    if (count < 0 || slot >= (unsigned)count) return CP_GEN5_OTHER;
    ReadMon(party + PARTY_SLOTS + slot * MON_SIZE, 1, &mon);
    if (mon.decrypted || mon.mutates || !Species(&mon) || Species(&mon) > SPECIES_LAST) return CP_GEN5_OTHER;
    if (Egg(&mon)) return CP_GEN5_EGG;
    item = HeldItem(rules, &mon);
    if (item >= FIRST_MAIL && item <= LAST_MAIL) return CP_GEN5_MAIL;
    for (i = 0; i < count; ++i) {
        if ((unsigned)i == slot) continue;
        ReadMon(party + PARTY_SLOTS + i * MON_SIZE, 1, &mon);
        if (mon.decrypted || mon.mutates) return CP_GEN5_OTHER;
        if (!Egg(&mon) && Hp(&mon) != 0) ++usable;
    }
    return usable ? CP_GEN5_OK : CP_GEN5_LAST_USABLE;
}

static int Eligible(const Rules *rules, const unsigned char *party, unsigned slot)
{
    return Reason(rules, party, slot) == CP_GEN5_OK;
}

/* A stored slot: -1 unreadable as the game would read it, 0 empty, 1 a Pokémon
 * with `canonical` filled. */
static int Stored(const unsigned char *stored, unsigned char canonical[BOX_MON_SIZE])
{
    Mon mon;
    ReadMon(stored, 0, &mon);
    if (!Read16(mon.box + 0x08)) return mon.decrypted || mon.mutates ? -1 : 0;
    return WtPokemon_Decode(stored, canonical) == 0 ? 1 : -1;
}

/* game_save.c Absent: no party, PC or Battle Box Pokémon is the outgoing one. */
static int Absent(const CpSaveImage *image, const unsigned char outgoing[BOX_MON_SIZE])
{
    unsigned char canonical[BOX_MON_SIZE], *party = Party(image);
    unsigned box, slot;
    int count, i, kind;
    if ((count = Count(party, 1)) < 0) return -1;
    for (i = 0; i < count; ++i)
        if (WtPokemon_Decode(Slot(party, (unsigned)i), canonical) != 0 || memcmp(canonical, outgoing, BOX_MON_SIZE) == 0)
            return -1;
    for (box = 0; box <= BOXES; ++box) for (slot = 0; slot < BOX_SLOTS; ++slot) {
        if (box == BOXES && slot >= BATTLE_BOX_SLOTS) break;
        kind = Stored(box == BOXES ? BattleBoxMon(image, slot) : BoxMon(image, box, slot), canonical);
        if (kind < 0 || (kind == 1 && memcmp(canonical, outgoing, BOX_MON_SIZE) == 0)) return -1;
    }
    return 0;
}

/* POKE_TOOL_ClearPartyParam's empty Pokémon (or the first 136 bytes of it). */
static void Empty(unsigned char *image, unsigned size)
{
    unsigned char empty[MON_SIZE];
    CpReceive5_Empty(empty);
    memcpy(image, empty, size);
}

/* RemoveSlot: later Pokémon move down as stored, the last becomes empty. */
static void Remove(unsigned char *party, unsigned slot)
{
    unsigned count = Read32(party + 4), i;
    for (i = slot; i + 1 < count; ++i) memcpy(Slot(party, i), Slot(party, i + 1), MON_SIZE);
    Empty(Slot(party, i), MON_SIZE);
    Write32(party + 4, count - 1);
}

/* The one save count the journal records in every counter. */
static int Behind(const CpSaveImage *image, const WtJournalEntry *entry)
{
    return (uint32_t)(image->globalCounter - entry->globalCounter) >= UINT32_C(0x80000000);
}

static int Moved(const CpSaveImage *image, const WtJournalEntry *entry)
{
    return !Behind(image, entry) && image->globalCounter != entry->globalCounter;
}

static void Counters(const CpSaveImage *image, WtJournalEntry *entry)
{
    entry->globalCounter = entry->generalCounter = entry->boxCounter = image->globalCounter;
}

typedef struct Found {
    int party;
    /* The journal locator kind: 1 party, 2 PC, 3 Battle Box. */
    unsigned kind, box, slot;
} Found;

/* As adapter/platinum.c Locate, over the party, the PC and the Battle Box:
 * 1 found, 0 absent, -1 unreadable or ambiguous. */
static int Locate(const CpSaveImage *image, const unsigned char target[BOX_MON_SIZE], unsigned char (*digests)[32],
                  unsigned count, Found *found)
{
    unsigned char canonical[BOX_MON_SIZE], sum[32], *party = Party(image);
    unsigned i, j, n;
    int members, matches = 0, kind;
    if ((members = Count(party, 1)) < 0) return -1;
    n = (unsigned)members + BOXES * BOX_SLOTS + BATTLE_BOX_SLOTS;
    for (i = 0; i < n; ++i) {
        int inParty = i < (unsigned)members, inBattleBox = i >= (unsigned)members + BOXES * BOX_SLOTS;
        unsigned box = inParty ? 0 : (i - (unsigned)members) / BOX_SLOTS;
        unsigned slot = inParty ? i : (i - (unsigned)members) % BOX_SLOTS;
        const unsigned char *stored = inParty ? Slot(party, slot)
            : inBattleBox ? BattleBoxMon(image, i - (unsigned)members - BOXES * BOX_SLOTS) : BoxMon(image, box, slot);
        if (inParty) kind = WtPokemon_Decode(stored, canonical) == 0 ? 1 : -1;
        else kind = Stored(stored, canonical);
        if (kind < 0) return -1;
        if (kind == 0) continue;
        if (memcmp(canonical, target, 4) != 0 || memcmp(canonical + 0x0C, target + 0x0C, 4) != 0) continue;
        Sha(canonical, sizeof(canonical), sum);
        for (j = 0; j < count && memcmp(sum, digests[j], 32) != 0; ++j) {}
        if (j == count || ++matches > 1) return -1;
        found->party = inParty; found->kind = inParty ? 1 : inBattleBox ? 3 : 2;
        /* The Battle Box follows the last tray: its slot is the same remainder. */
        found->box = inBattleBox ? 0 : box; found->slot = slot;
    }
    return matches;
}

static int Hash(const CpSaveImage *image, unsigned char digest[32])
{
    return CpSaveImage_Hash(image, digest) == CP_SAVE_OK ? 0 : -1;
}

static int SaveBoth(WtGame *game, const unsigned char expected[32])
{
    unsigned char actual[32];
    if (Hash(game->image, actual) != 0 || memcmp(actual, expected, 32) != 0) return -1;
    if (CpSaveImage_Verify(game->image, game->file, expected) == CP_SAVE_OK) return 0;
    return CpSaveImage_CommitBoth(game->image, game->file, expected) == CP_SAVE_OK ? 0 : -1;
}

static int Checkpoint(WtGame *game)
{
    unsigned char digest[32];
    if (Hash(game->image, digest) != 0) return -1;
    return CpSaveImage_Verify(game->image, game->file, digest) == CP_SAVE_OK ? 0 : SaveBoth(game, digest);
}

/* What a step may change, kept to put the image back when it fails or only
 * prepares: the party, the Pokédex and the one PC slot a delivery may fill. */
typedef struct Backup {
    unsigned char party[PARTY_SIZE], pokedex[POKEDEX_MAX], box[BOX_MON_SIZE];
    unsigned char *partyAt, *pokedexAt, *boxAt;
    uint32_t pokedexSize;
} Backup;

static Backup *Keep(CpSaveImage *image)
{
    Backup *backup = malloc(sizeof(*backup));
    if (!backup) return NULL;
    backup->partyAt = Party(image);
    backup->pokedexAt = Pokedex(image);
    backup->pokedexSize = CpSaveImage_BlockSize(image, Layout(image)->pokedexBlock);
    backup->boxAt = NULL;
    if (backup->pokedexSize > sizeof(backup->pokedex)) { free(backup); return NULL; }
    memcpy(backup->party, backup->partyAt, PARTY_SIZE);
    memcpy(backup->pokedex, backup->pokedexAt, backup->pokedexSize);
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
        memcpy(backup->partyAt, backup->party, PARTY_SIZE);
        memcpy(backup->pokedexAt, backup->pokedex, backup->pokedexSize);
        if (backup->boxAt) memcpy(backup->boxAt, backup->box, sizeof(backup->box));
    }
    memset(backup, 0, sizeof(*backup));
    free(backup);
}

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
    if (Behind(game->image, entry) || Absent(game->image, entry->outgoing) != 0) return -1;
    return Checkpoint(game);
}

static unsigned char *Destination(const CpSaveImage *image, WtJournalLocator where)
{
    if (where.kind == 1 && where.box == 0 && where.slot < PARTY_CAPACITY && where.length == MON_SIZE)
        return Slot(Party(image), where.slot);
    if (where.kind == 2 && where.box < BOXES && where.slot < BOX_SLOTS && where.length == BOX_MON_SIZE)
        return BoxMon(image, where.box, where.slot);
    /* Only a delivery found there is recorded in the Battle Box. */
    if (where.kind == 3 && where.box == 0 && where.slot < BATTLE_BOX_SLOTS && where.length == BOX_MON_SIZE)
        return BattleBoxMon(image, where.slot);
    return NULL;
}

/* game_save.c BuildReceived: to the end of the party, else the first empty
 * PC slot; 1 when there is no room. */
static int BuildReceived(WtGame *game, WtJournalEntry *entry, Backup *backup, int prepare)
{
    CpSaveImage *image = game->image;
    unsigned char *party = Party(image), *destination, canonical[BOX_MON_SIZE], built[MON_SIZE];
    unsigned box, slot;
    int count, found, kind;
    if (prepare) {
        if ((count = Count(party, 1)) < 0) return -1;
        /* A returned offer that is already back must never be added twice
         * (Back and ReceiveMoved settle that case first), and a matched
         * delivery needs its offer gone: a copy still here would make two. The
         * game refuses both too (client wonder_trade/delivery.h). */
        if (Absent(image, entry->outgoing) != 0) return -1;
        memset(&entry->destination, 0, sizeof(entry->destination));
        if (count < PARTY_CAPACITY) {
            entry->destination.kind = 1; entry->destination.slot = (unsigned char)count;
            entry->destination.length = MON_SIZE;
        } else {
            found = 0;
            for (box = 0; box < BOXES && !found; ++box) for (slot = 0; slot < BOX_SLOTS; ++slot) {
                if ((kind = Stored(BoxMon(image, box, slot), canonical)) < 0) return -1;
                if (kind == 0) { found = 1; break; }
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
    destination = entry->destination.kind == 3 ? NULL : Destination(image, entry->destination);
    if (!destination || memcmp(destination, entry->destinationBefore, entry->destination.length) != 0
        || (entry->destination.kind == 1 && Read32(party + 4) != entry->destination.slot)) return -1;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, BOX_MON_SIZE) != 0) return -1;
        memcpy(built, entry->original, MON_SIZE);
    } else if (entry->deliveryKind == WT_DELIVERY_MATCH && game->data) {
        if (CpReceive5_Apply(RulesOf(image)->receive, game->data, Pokedex(image), entry->incoming, entry->original,
                entry->destination.kind == 1 ? CP_RECEIVE_PARTY : CP_RECEIVE_BOX, built) != 0) return -1;
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

static int Applied(WtGame *game, const WtJournalEntry *entry)
{
    unsigned char digest[32];
    if (Hash(game->image, digest) != 0) return -1;
    if (memcmp(digest, entry->inventoryAfter, 32) == 0) return VerifyReceived(game, entry);
    if (Behind(game->image, entry)) return -1;
    return Checkpoint(game);
}

/* The delivered Pokémon's possible exact images (adapter/platinum.c Delivered). */
static int Delivered(WtGame *game, const WtJournalEntry *entry, Found *found)
{
    unsigned char digests[3][32], image[MON_SIZE], canonical[BOX_MON_SIZE], *scratch;
    const uint32_t size = CpSaveImage_BlockSize(game->image, Layout(game->image)->pokedexBlock);
    unsigned count = 1, i;
    if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        Sha(entry->outgoing, BOX_MON_SIZE, digests[0]);
        return Locate(game->image, entry->outgoing, digests, 1, found);
    }
    if (entry->deliveryKind != WT_DELIVERY_MATCH || !game->data || !(scratch = malloc(size))) return -1;
    memcpy(digests[0], entry->appliedDigest, 32);
    for (i = 0; i < 2; ++i) {
        memcpy(scratch, Pokedex(game->image), size);
        if (CpReceive5_Apply(RulesOf(game->image)->receive, game->data, scratch, entry->incoming, entry->original,
                i ? CP_RECEIVE_BOX : CP_RECEIVE_PARTY, image) != 0
            || WtPokemon_Decode(image, canonical) != 0) { free(scratch); return -1; }
        Sha(canonical, sizeof(canonical), digests[count++]);
    }
    free(scratch);
    return Locate(game->image, entry->incoming, digests, count, found);
}

static int Back(WtGame *game, const WtJournalEntry *entry)
{
    Found found;
    int where;
    if (entry->deliveryKind != WT_DELIVERY_RETURN) return 0;
    where = Delivered(game, entry, &found);
    return where == 1 && !Moved(game->image, entry) ? -1 : where;
}

static int ReceiveMoved(WtGame *game, WtJournalEntry *entry, Backup *backup, int *restore)
{
    Found found;
    int where = Delivered(game, entry, &found);
    if (where < 0) return -1;
    if (where == 0) {
        if (BuildReceived(game, entry, backup, 1) != 0 || Hash(game->image, entry->inventoryAfter) != 0) return -1;
        *restore = 0;
        if (SaveBoth(game, entry->inventoryAfter) != 0 || VerifyReceived(game, entry) != 0) return -1;
    } else {
        WtJournalLocator at;
        unsigned char *stored, canonical[BOX_MON_SIZE];
        memset(&at, 0, sizeof(at));
        at.kind = (unsigned char)found.kind; at.box = (unsigned char)found.box; at.slot = (unsigned char)found.slot;
        at.length = found.party ? MON_SIZE : BOX_MON_SIZE;
        if (!(stored = Destination(game->image, at)) || WtPokemon_Decode(stored, canonical) != 0) return -1;
        *restore = 0;
        if (Checkpoint(game) != 0 || Hash(game->image, entry->inventoryAfter) != 0) return -1;
        entry->destination = at;
        memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
        Empty(entry->destinationBefore, at.length);
        memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
        memcpy(entry->destinationAfter, stored, at.length);
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
        if (!found.party || !Eligible(RulesOf(image), party, found.slot)) return -1;
        entry->source.slot = (unsigned char)found.slot;
        memset(entry->original, 0, sizeof(entry->original));
        memcpy(entry->original, Slot(party, found.slot), MON_SIZE);
        Sha(entry->original, MON_SIZE, entry->originalDigest);
        if (Hash(image, entry->inventoryBefore) != 0) return -1;
        Remove(party, found.slot);
        if (Absent(image, entry->outgoing) != 0 || Hash(image, entry->inventoryAfter) != 0) return -1;
        *restore = 0;
        if (SaveBoth(game, entry->inventoryAfter) != 0) return -1;
    } else {
        *restore = 0;
        if (Checkpoint(game) != 0) return -1;
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
    if (!(backup = Keep(image))) return -1;
    if (step == WT_INVENTORY_PREPARE_REMOVE) {
        if (slot >= PARTY_CAPACITY || !Eligible(RulesOf(image), party, slot)
            || WtPokemon_Decode(Slot(party, slot), canonical) != 0) goto done;
        entry->source.kind = 1; entry->source.box = 0;
        entry->source.slot = (unsigned char)slot; entry->source.length = MON_SIZE;
        memset(entry->original, 0, sizeof(entry->original));
        memcpy(entry->original, Slot(party, slot), MON_SIZE);
        memcpy(entry->outgoing, canonical, sizeof(canonical));
        Sha(entry->original, MON_SIZE, entry->originalDigest);
        if (Hash(image, entry->inventoryBefore) != 0) goto done;
        Remove(party, slot);
        if (Absent(image, entry->outgoing) != 0 || Hash(image, entry->inventoryAfter) != 0) goto done;
        Counters(image, entry); result = 0; goto done;
    }
    if (entry->source.kind != 1 || entry->source.length != MON_SIZE || slot >= PARTY_CAPACITY
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
            if (count < 0 || count >= PARTY_CAPACITY || slot > (unsigned)count) goto done;
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

int CpGen5_Inventory(WtGame *game, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    unsigned char digest[32];
    if (!game || !CpGen5_Is(game->image) || !game->image->loaded || !entry) return -1;
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

int CpGen5_CanLeave(WtGame *game, const WtJournalEntry *entry)
{
    if (!game || !CpGen5_Is(game->image) || !game->image->loaded || !entry) return 0;
    switch (entry->phase) {
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_IDLE: case WT_JOURNAL_ENROLLING: return 1;
    case WT_JOURNAL_SUBMIT_READY: case WT_JOURNAL_QUEUED: case WT_JOURNAL_RECEIVED: case WT_JOURNAL_RESTORING:
        return Removed(game, entry) == 0;
    case WT_JOURNAL_APPLIED: return Applied(game, entry) == 0;
    default: return 0;
    }
}

int CpGen5_Reason(const CpSaveImage *image, unsigned slot)
{
    if (!CpGen5_Is(image) || !image->loaded) return CP_GEN5_OTHER;
    return Reason(RulesOf(image), Party(image), slot);
}

int CpGen5_Eligible(const CpSaveImage *image, unsigned slot)
{
    return CpGen5_Reason(image, slot) == CP_GEN5_OK;
}

int CpGen5_Unique(CpSaveImage *image, unsigned slot)
{
    unsigned char *party, canonical[BOX_MON_SIZE];
    Backup *backup;
    int count, unique;
    if (!CpGen5_Is(image) || !image->loaded) return 1;
    party = Party(image);
    if ((count = Count(party, 1)) < 0 || slot >= (unsigned)count || WtPokemon_Decode(Slot(party, slot), canonical) != 0)
        return 1;
    if (!(backup = Keep(image))) return 1;
    Remove(party, slot);
    unique = Absent(image, canonical) == 0;
    Release(backup, 1);
    return unique;
}

static unsigned FormsBlackWhite(unsigned species) { return CpReceive5_Forms(CP_RECEIVE_BLACK_WHITE, species); }
static unsigned FormsBlack2White2(unsigned species) { return CpReceive5_Forms(CP_RECEIVE_BLACK2_WHITE2, species); }

static int Holdable(void *arg, unsigned item)
{
    const WtGame *game = arg;
    return game->holdable && game->holdable(game->holdableArg, item);
}

int CpGen5_CanHold(const WtGame *game, const unsigned char pokemon[WT_POKEMON_SIZE])
{
    WtPokemonLimits limits;
    const Rules *rules;
    if (!game || !CpGen5_Is(game->image) || !pokemon) return 0;
    rules = RulesOf(game->image);
    limits.maxSpecies = SPECIES_LAST; limits.maxItem = rules->itemLast; limits.maxBall = BALL_LAST;
    limits.forms = rules->receive == CP_RECEIVE_BLACK2_WHITE2 ? FormsBlack2White2 : FormsBlackWhite;
    limits.holdable = Holdable; limits.arg = (void *)game;
    /* Black/White's Pokédex would write outside its block for language 0. */
    if (rules->receive == CP_RECEIVE_BLACK_WHITE && pokemon[0x17] == 0) return 0;
    return WtPokemon_Fits(pokemon, &limits);
}

/* ---- The musical distribution area ------------------------------------------ */

int CpGen5_MusicalSaved(const CpSaveImage *image)
{
    const CpSaveGen5 *g;
    if (!CpGen5_Is(image) || !image->loaded) return 0;
    g = Layout(image);
    /* The load cleared a marker whose musical does not load (save/gen5_save.h). */
    return Read16(BlockOf(image, g->trainerBlock) + g->markerOffset + 2 * CP_GEN5_MUSICAL_MARKER) == CP_GEN5_MARKER_SET;
}

int CpGen5_ReleaseMusical(WtGame *game)
{
    const CpJournalArea *area;
    int state;
    if (!game || !CpGen5_Is(game->image) || !game->image->loaded) return -1;
    if (CpGen5_MusicalSaved(game->image)) return CP_GEN5_MUSICAL_SAVED;
    area = CpSaveJournal_Area(game->image->layout);
    if ((state = CpMusicalArea_Inspect(game->file, area)) < 0) return -1;
    if (state == CP_MUSICAL_JOURNAL) return 0;
    /* Writes nothing when both copies hold the loaded save; otherwise the
     * cleared marker and program flag reach both before any bank is erased. */
    if (Checkpoint(game) != 0) return -1;
    return state == CP_MUSICAL_LEFTOVER ? CpMusicalArea_Erase(game->file, area) : 0;
}
