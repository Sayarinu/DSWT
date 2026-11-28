#ifndef WONDER_TRADE_DELIVERY_H
#define WONDER_TRADE_DELIVERY_H

/* What every game adapter decides about the Pokémon a save holds: whether an
 * offer is gone and unique, where a delivery goes, and how a delivery cut short
 * is settled. Each game's game_save.c includes this file once and describes its
 * save through WtDeliveryOps; the host tests include it with a synthetic save,
 * so these decisions are tested as the ROMs run them. Every function is static:
 * a ROM build needs no new object.
 *
 * A Pokémon's identity is its personality value and original trainer ID
 * (canonical bytes 0-3 and 12-15). Play changes the rest of its image
 * (friendship, experience, PP, moves), never these.
 *
 * The patched game settles its journal before it can be played, so the only
 * save written between a delivery's record and its completion is another
 * game's: the unmodified game, played after a power cut. When a write was cut
 * short there, the save it loaded may or may not hold the delivery, and that
 * save may since have moved the Pokémon where no locator reaches (the Day
 * Care, the Pokéwalker, a trade). Nothing in the save tells these apart, so a
 * delivery is never added to such a save again: it is accepted as it is, and
 * recorded where it is when exactly one Pokémon has its identity. */

#include <stdint.h>
#include <string.h>

#include "wonder_trade/engine.h"
#include "wonder_trade/game_family.h"
#include "wonder_trade/journal.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/wire.h"

/* A save is the adapter's handle: the live save (live) or a copy from copy. */
typedef struct WtDeliveryOps {
    void *arg, *live;
    /* SHA-256, for canonical images. */
    WtJournalHash sha;
    /* Party Pokémon in the save; anything outside 1 to 6 is refused. */
    int (*partyCount)(void *arg, void *save);
    /* The stored image at a locator (party kind 1, PC kind 2, Battle Box kind
     * 3), or NULL when the game has no such slot. */
    unsigned char *(*slot)(void *arg, void *save, WtJournalLocator where);
    /* A PC or Battle Box slot: 1 holds a Pokémon, 0 empty, -1 unreadable,
     * decided as the game itself reads the slot. */
    int (*occupied)(void *arg, void *save, WtJournalLocator where);
    /* The save digest the journal records (inventoryBefore and After). */
    int (*hash)(void *arg, void *save, unsigned char digest[32]);
    /* Both retail copies on flash hold exactly this digest. */
    int (*verify)(void *arg, const unsigned char digest[32]);
    /* Make both retail copies the live save (WtGame_Checkpoint). */
    int (*checkpoint)(void *arg);
    void *(*copy)(void *arg);
    void (*discard)(void *arg, void *copy);
    /* The live save becomes the copy; both retail copies are written and verified as digest. */
    int (*commit)(void *arg, void *copy, const unsigned char digest[32]);
    /* The live save's global, general-block and box-block counters. */
    void (*counters)(void *arg, uint32_t counters[3]);
    /* Put the delivery in the save. Preparing chooses the destination (the
     * party's end, else the first empty PC slot; 1 when there is none) and
     * records destinationBefore, destinationAfter, appliedDigest and
     * inventoryBefore. Otherwise it builds exactly the recorded image. */
    int (*build)(void *arg, void *save, WtJournalEntry *entry, int prepare);
    /* The game's empty Pokémon of length bytes. */
    void (*empty)(void *arg, unsigned char *image, unsigned length);
} WtDeliveryOps;

typedef struct WtDeliveryScan {
    /* Pokémon with the target's identity, those with its exact canonical
     * image, and occupied slots the game cannot read. */
    unsigned identities, exact, unreadable;
    /* The first Pokémon with the identity, and its canonical image. */
    WtJournalLocator where;
    unsigned char canonical[WT_POKEMON_SIZE];
} WtDeliveryScan;

static unsigned WtDelivery_Length(unsigned kind)
{
    return kind == 1 ? WT_GAME_PARTY_SIZE : WT_POKEMON_SIZE;
}

/* Every Pokémon the save holds: the party, the PC and the Battle Box. */
static int WtDelivery_Scan(const WtDeliveryOps *ops, void *save, const unsigned char target[WT_POKEMON_SIZE],
                           WtDeliveryScan *scan)
{
    WtJournalLocator where;
    const unsigned char *stored;
    unsigned char canonical[WT_POKEMON_SIZE];
    unsigned kind, box, boxes, slot, slots;
    int count = ops->partyCount(ops->arg, save), occupied;
    memset(scan, 0, sizeof(*scan));
    if (count < 1 || count > 6) return -1;
    for (kind = 1; kind <= 3; ++kind) {
        boxes = kind == 2 ? WT_GAME_BOX_COUNT : 1;
        slots = kind == 1 ? (unsigned)count : kind == 2 ? WT_GAME_BOX_SLOTS : WT_GAME_BATTLE_BOX_SLOTS;
        for (box = 0; box < boxes; ++box) for (slot = 0; slot < slots; ++slot) {
            where.kind = (unsigned char)kind; where.box = (unsigned char)box;
            where.slot = (unsigned char)slot; where.length = (unsigned char)WtDelivery_Length(kind);
            if (!(stored = ops->slot(ops->arg, save, where))) return -1;
            occupied = kind == 1 ? 1 : ops->occupied(ops->arg, save, where);
            if (occupied == 0) continue;
            if (occupied < 0 || WtPokemon_Decode(stored, canonical) != 0) { ++scan->unreadable; continue; }
            if (memcmp(canonical, target, 4) != 0 || memcmp(canonical + 12, target + 12, 4) != 0) continue;
            if (memcmp(canonical, target, WT_POKEMON_SIZE) == 0) ++scan->exact;
            if (scan->identities++ == 0) {
                scan->where = where;
                memcpy(scan->canonical, canonical, sizeof(canonical));
            }
        }
    }
    return 0;
}

/* Zero when no Pokémon in the save is exactly this image and every one can be
 * read: an offer has left it. */
static int WtDelivery_Absent(const WtDeliveryOps *ops, void *save, const unsigned char canonical[WT_POKEMON_SIZE])
{
    WtDeliveryScan scan;
    return WtDelivery_Scan(ops, save, canonical, &scan) == 0 && !scan.exact && !scan.unreadable ? 0 : -1;
}

/* Zero when no Pokémon in the save has this identity and every one can be
 * read. A deposit requires it of the save without its offer, so a delivery
 * returned or cut short later finds at most one Pokémon with the identity. */
static int WtDelivery_Unique(const WtDeliveryOps *ops, void *save, const unsigned char canonical[WT_POKEMON_SIZE])
{
    WtDeliveryScan scan;
    return WtDelivery_Scan(ops, save, canonical, &scan) == 0 && !scan.identities && !scan.unreadable ? 0 : -1;
}

static void WtDelivery_Counters(const WtDeliveryOps *ops, uint32_t counters[3])
{
    ops->counters(ops->arg, counters);
}

static void WtDelivery_Store(const WtDeliveryOps *ops, WtJournalEntry *entry)
{
    uint32_t counters[3];
    WtDelivery_Counters(ops, counters);
    entry->globalCounter = counters[0]; entry->generalCounter = counters[1]; entry->boxCounter = counters[2];
}

/* Nonzero when the save is older than the one the entry recorded. */
static int WtDelivery_Behind(const uint32_t counters[3], const WtJournalEntry *entry)
{
    return (uint32_t)(counters[0] - entry->globalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(counters[1] - entry->generalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(counters[2] - entry->boxCounter) >= UINT32_C(0x80000000);
}

/* Nonzero when a game has saved since the entry recorded its counters. */
static int WtDelivery_Moved(const uint32_t counters[3], const WtJournalEntry *entry)
{
    return !WtDelivery_Behind(counters, entry) && (counters[0] != entry->globalCounter
        || counters[1] != entry->generalCounter || counters[2] != entry->boxCounter);
}

/* The escrow the entry records: a party image and its canonical form. */
static int WtDelivery_Bound(const WtJournalEntry *entry)
{
    unsigned char canonical[WT_POKEMON_SIZE];
    return entry->source.kind == 1 && entry->source.length == WT_GAME_PARTY_SIZE
        && WtPokemon_Decode(entry->original, canonical) == 0
        && memcmp(canonical, entry->outgoing, sizeof(canonical)) == 0;
}

/* The offer is gone from a save at least as new as the escrow's, which both
 * retail copies then hold. */
static int WtDelivery_Removed(const WtDeliveryOps *ops, const WtJournalEntry *entry)
{
    uint32_t counters[3];
    WtDelivery_Counters(ops, counters);
    /* A selected older retail copy cannot be promoted to a new checkpoint. */
    if (!WtDelivery_Bound(entry) || WtDelivery_Behind(counters, entry)
        || WtDelivery_Absent(ops, ops->live, entry->outgoing) != 0) return -1;
    return ops->checkpoint(ops->arg);
}

/* The delivered image where the entry says, in the live save both retail
 * copies hold. */
static int WtDelivery_Verify(const WtDeliveryOps *ops, const WtJournalEntry *entry)
{
    unsigned char digest[32], canonical[WT_POKEMON_SIZE];
    const unsigned char *stored = entry->destination.kind ? ops->slot(ops->arg, ops->live, entry->destination) : NULL;
    if (!stored || memcmp(stored, entry->destinationAfter, entry->destination.length) != 0
        || WtPokemon_Decode(stored, canonical) != 0 || ops->sha(canonical, sizeof(canonical), digest) != 0
        || memcmp(digest, entry->appliedDigest, 32) != 0
        || ops->hash(ops->arg, ops->live, digest) != 0 || memcmp(digest, entry->inventoryAfter, 32) != 0) return -1;
    return ops->verify(ops->arg, entry->inventoryAfter);
}

/* A delivery applied earlier: either the save recorded with it, or one saved
 * after it while the ACK waited. An older copy is refused. */
static int WtDelivery_Applied(const WtDeliveryOps *ops, const WtJournalEntry *entry)
{
    unsigned char digest[32];
    uint32_t counters[3];
    if (ops->hash(ops->arg, ops->live, digest) != 0) return -1;
    if (memcmp(digest, entry->inventoryAfter, 32) == 0) return WtDelivery_Verify(ops, entry);
    WtDelivery_Counters(ops, counters);
    if (WtDelivery_Behind(counters, entry)) return -1;
    return ops->checkpoint(ops->arg);
}

/* Record the Pokémon a scan found as the delivery, as it is now: where it is,
 * its stored image (padded with zeros, as the journal requires) and the digest
 * of its canonical form, which the ACK reports. */
static int WtDelivery_Found(const WtDeliveryOps *ops, void *save, WtJournalEntry *entry, const WtDeliveryScan *scan)
{
    const unsigned char *stored = ops->slot(ops->arg, save, scan->where);
    if (!stored) return -1;
    entry->destination = scan->where;
    memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
    ops->empty(ops->arg, entry->destinationBefore, scan->where.length);
    memset(entry->destinationAfter, 0, sizeof(entry->destinationAfter));
    memcpy(entry->destinationAfter, stored, scan->where.length);
    return ops->sha(scan->canonical, sizeof(scan->canonical), entry->appliedDigest);
}

/* PREPARE_RESTORE: 0 when the save is still the escrow's, before or after its
 * removal, so RESTORE can put the offer back in its own slot. */
static int WtDelivery_PrepareRestore(const WtDeliveryOps *ops, WtJournalEntry *entry)
{
    unsigned char digest[32];
    if (ops->hash(ops->arg, ops->live, digest) != 0) return -1;
    if (memcmp(digest, entry->inventoryBefore, 32) != 0 && memcmp(digest, entry->inventoryAfter, 32) != 0) return 1;
    WtDelivery_Store(ops, entry);
    return 0;
}

/* PREPARE_RECEIVE. A matched delivery needs its offer gone. A returned offer
 * already back (an in-place restore an older build wrote, then a save over
 * it) is recorded where it is, in any state play left it, and never added. */
static int WtDelivery_Prepare(const WtDeliveryOps *ops, WtJournalEntry *entry)
{
    WtDeliveryScan scan;
    uint32_t counters[3];
    void *copy;
    int result;
    if (entry->deliveryKind == WT_DELIVERY_MATCH) {
        if (WtDelivery_Absent(ops, ops->live, entry->outgoing) != 0 ||
            WtDelivery_Scan(ops, ops->live, entry->incoming, &scan) != 0 || scan.unreadable) return -1;
        /* A resident with this identity would be indistinguishable after an
         * interrupted write and stock-game play. Keep the delivery pending. */
        if (scan.identities) return 1;
    } else if (entry->deliveryKind == WT_DELIVERY_RETURN) {
        if (memcmp(entry->incoming, entry->outgoing, WT_POKEMON_SIZE) != 0
            || WtDelivery_Scan(ops, ops->live, entry->outgoing, &scan) != 0) return -1;
        if (scan.unreadable || scan.identities > 1) return -1;
        if (scan.identities) {
            WtDelivery_Counters(ops, counters);
            if (WtDelivery_Behind(counters, entry) || WtDelivery_Found(ops, ops->live, entry, &scan) != 0
                || ops->hash(ops->arg, ops->live, entry->inventoryAfter) != 0) return -1;
            memcpy(entry->inventoryBefore, entry->inventoryAfter, 32);
            WtDelivery_Store(ops, entry);
            return 0;
        }
    } else return -1;
    if (!(copy = ops->copy(ops->arg))) return -1;
    result = ops->build(ops->arg, copy, entry, 1);
    if (result == 0 && ops->hash(ops->arg, copy, entry->inventoryAfter) != 0) result = -1;
    ops->discard(ops->arg, copy);
    /* RECEIVE tells another game's later save by these counters. */
    if (result == 0) WtDelivery_Store(ops, entry);
    return result;
}

/* RECEIVE on a save that is neither recorded inventory: another game saved
 * after a write here was cut short (see the top of this file). The save is
 * accepted as it is and nothing is added. A matched delivery still needs its
 * offer gone; an older save, or one with the recorded counters, is refused. */
static int WtDelivery_Recover(const WtDeliveryOps *ops, WtJournalEntry *entry)
{
    WtDeliveryScan scan;
    uint32_t counters[3];
    WtDelivery_Counters(ops, counters);
    if (!WtDelivery_Moved(counters, entry)
        || (entry->deliveryKind != WT_DELIVERY_MATCH && entry->deliveryKind != WT_DELIVERY_RETURN)
        || (entry->deliveryKind == WT_DELIVERY_RETURN
            && memcmp(entry->incoming, entry->outgoing, WT_POKEMON_SIZE) != 0)
        || WtDelivery_Scan(ops, ops->live, entry->outgoing, &scan) != 0) return -1;
    if (entry->deliveryKind == WT_DELIVERY_MATCH
        && (scan.exact || WtDelivery_Scan(ops, ops->live, entry->incoming, &scan) != 0)) return -1;
    if (scan.unreadable || scan.identities > 1) return -1;
    if (scan.identities == 1 && WtDelivery_Found(ops, ops->live, entry, &scan) != 0) return -1;
    if (ops->checkpoint(ops->arg) != 0) return -1;
    /* Without one such Pokémon the record stays as prepared; its inventoryAfter
     * is not this save, which VERIFY_RECEIVED then only checkpoints. */
    if (scan.identities == 1 && ops->hash(ops->arg, ops->live, entry->inventoryAfter) != 0) return -1;
    WtDelivery_Store(ops, entry);
    return 0;
}

/* RECEIVE: the recorded delivery, from exactly the recorded inventory, into
 * both retail copies; a save already holding it is only written again. */
static int WtDelivery_Receive(const WtDeliveryOps *ops, WtJournalEntry *entry)
{
    unsigned char digest[32];
    void *copy;
    int result = -1, before, after;
    if (ops->hash(ops->arg, ops->live, digest) != 0) return -1;
    before = memcmp(digest, entry->inventoryBefore, 32) == 0;
    after = memcmp(digest, entry->inventoryAfter, 32) == 0;
    /* A return to the offer's own slot is RESTORE's while the save is the escrow's. */
    if (!entry->destination.kind) return before || after ? -1 : WtDelivery_Recover(ops, entry);
    if (!before && !after) return WtDelivery_Recover(ops, entry);
    if (!(copy = ops->copy(ops->arg))) return -1;
    if (!after && (ops->build(ops->arg, copy, entry, 0) != 0 || ops->hash(ops->arg, copy, digest) != 0
            || memcmp(digest, entry->inventoryAfter, 32) != 0)) goto done;
    if (ops->commit(ops->arg, copy, entry->inventoryAfter) != 0 || WtDelivery_Verify(ops, entry) != 0) goto done;
    WtDelivery_Store(ops, entry);
    result = 0;
done:
    ops->discard(ops->arg, copy);
    return result;
}

/* The engine's delivery steps, PREPARE_RESTORE and VERIFY_REMOVED, for an
 * entry bound to its escrow. */
static int WtDelivery_Step(const WtDeliveryOps *ops, enum WtInventoryStep step, WtJournalEntry *entry)
{
    if (!WtDelivery_Bound(entry)) return -1;
    switch (step) {
    case WT_INVENTORY_VERIFY_REMOVED: return WtDelivery_Removed(ops, entry);
    case WT_INVENTORY_PREPARE_RESTORE: return WtDelivery_PrepareRestore(ops, entry);
    case WT_INVENTORY_PREPARE_RECEIVE: return WtDelivery_Prepare(ops, entry);
    case WT_INVENTORY_RECEIVE: return WtDelivery_Receive(ops, entry);
    case WT_INVENTORY_VERIFY_RECEIVED: return WtDelivery_Applied(ops, entry);
    default: return -1;
    }
}

/* WtGame_CanLeave. */
static int WtDelivery_CanLeave(const WtDeliveryOps *ops, const WtJournalEntry *entry)
{
    switch (entry->phase) {
    case WT_JOURNAL_EMPTY: case WT_JOURNAL_IDLE: case WT_JOURNAL_ENROLLING: return 1;
    case WT_JOURNAL_SUBMIT_READY: case WT_JOURNAL_QUEUED: case WT_JOURNAL_RECEIVED: case WT_JOURNAL_RESTORING:
        return WtDelivery_Removed(ops, entry) == 0;
    case WT_JOURNAL_APPLIED: return WtDelivery_Applied(ops, entry) == 0;
    default: return 0;
    }
}

#endif
