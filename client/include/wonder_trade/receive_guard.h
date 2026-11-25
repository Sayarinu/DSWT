#ifndef WONDER_TRADE_RECEIVE_GUARD_H
#define WONDER_TRADE_RECEIVE_GUARD_H

#include <string.h>
#include "wonder_trade/journal.h"
#include "wonder_trade/pokemon_codec.h"
#include "wonder_trade/wire.h"

typedef struct WtReceiveScan {
    unsigned matches;
    WtJournalLocator where;
    unsigned char stored[236];
} WtReceiveScan;

/* Read every slot, including empty ones. PID plus OT ID identifies a possible
 * copy; only one exact canonical image is proof that a RETURN is back. */
static int WtReceive_Inspect(WtReceiveScan *scan, const unsigned char outgoing[136],
    const unsigned char *stored, unsigned kind, unsigned box, unsigned slot, unsigned length)
{
    unsigned char canonical[136];
    if (!stored || length < 136 || length > sizeof(scan->stored)
        || WtPokemon_Decode(stored, canonical) != 0) return -1;
    if ((canonical[8] | canonical[9]) == 0) return 0;
    if (memcmp(canonical, outgoing, 4) != 0 || memcmp(canonical + 12, outgoing + 12, 4) != 0) return 0;
    if (memcmp(canonical, outgoing, 136) != 0 || ++scan->matches > 1) return -1;
    memset(&scan->where, 0, sizeof(scan->where));
    scan->where.kind = (unsigned char)kind; scan->where.box = (unsigned char)box;
    scan->where.slot = (unsigned char)slot; scan->where.length = (unsigned char)length;
    memset(scan->stored, 0, sizeof(scan->stored));
    memcpy(scan->stored, stored, length);
    return 0;
}

/* 1: an exact RETURN already exists in a later save; 0: deliver normally.
 * MATCH cannot legitimately retain the offer: engine.c verifies removal
 * before Submit, and only rejected/cancelled offers go through Restore. */
static int WtReceive_Check(const WtJournalEntry *entry, const WtReceiveScan *scan,
    uint32_t global, uint32_t general, uint32_t boxes)
{
    if ((uint32_t)(global - entry->globalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(general - entry->generalCounter) >= UINT32_C(0x80000000)
        || (uint32_t)(boxes - entry->boxCounter) >= UINT32_C(0x80000000)) return -1;
    if (scan->matches == 0) return 0;
    if (entry->deliveryKind != WT_DELIVERY_RETURN || scan->matches != 1
        || memcmp(entry->incoming, entry->outgoing, 136) != 0) return -1;
    return global != entry->globalCounter || general != entry->generalCounter || boxes != entry->boxCounter ? 1 : -1;
}

/* The SDK supplies the empty party image; its box prefix is the empty box
 * image too. Pad both located fields as EntryValid requires (PK5 is shorter). */
static int WtReceive_Record(WtJournalEntry *entry, const WtReceiveScan *scan,
    const unsigned char *empty, WtJournalHash hash)
{
    entry->destination = scan->where;
    memset(entry->destinationBefore, 0, sizeof(entry->destinationBefore));
    memcpy(entry->destinationBefore, empty, scan->where.length);
    memcpy(entry->destinationAfter, scan->stored, sizeof(entry->destinationAfter));
    return hash(entry->outgoing, 136, entry->appliedDigest);
}

#endif
