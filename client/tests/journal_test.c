#include "wonder_trade/journal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>

static unsigned char banks[2][4096], baseline[4096];
static int writes, failAt, cutAt, lie, reads, readFailure;

static int Hash(const unsigned char *bytes, size_t length, unsigned char digest[32])
{
    size_t size = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, length, digest, 32, &size) == PSA_SUCCESS && size == 32 ? 0 : -1;
}

static int Read(void *arg, int bank, unsigned char *bytes)
{
    (void)arg;
    if (++reads == readFailure) return -1;
    memcpy(bytes, banks[bank], 4096);
    return 0;
}

static int Write(void *arg, int bank, const unsigned char *bytes)
{
    int step, count;
    (void)arg;
    count = ++writes == failAt ? cutAt : 4100;
    for (step = 0; step < count; step++) {
        if (step < 4) banks[bank][4092 + step] = 0;
        else banks[bank][step - 4] = bytes[step - 4];
    }
    return writes == failAt && !lie ? -1 : 0;
}

static WtJournalIO IO(void)
{
    WtJournalIO io = { NULL, Read, Write, Hash };
    return io;
}

static void Reset(void)
{
    memcpy(banks[0], baseline, 4096); memcpy(banks[1], baseline, 4096);
    writes = failAt = cutAt = lie = reads = readFailure = 0;
}

static void Identity(WtJournalEntry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->phase = WT_JOURNAL_IDLE; entry->generation = 1; entry->nextCounter = 1;
    memset(entry->saveID, 1, 16); memset(entry->credential, 2, 32);
    memset(entry->enrollmentID, 3, 16); memset(entry->seed, 4, 32);
}

static void Escrow(WtJournalEntry *entry)
{
    Identity(entry);
    entry->phase = WT_JOURNAL_ESCROW; entry->generation = 2;
    entry->source.kind = 1; entry->source.slot = 2; entry->source.length = 236;
    memset(entry->operation, 5, 16); entry->operationCounter = 1;
    memset(entry->original, 6, 236); memset(entry->outgoing, 7, 136);
    memset(entry->inventoryBefore, 8, 32); memset(entry->inventoryAfter, 9, 32);
    assert(Hash(entry->original, entry->source.length, entry->originalDigest) == 0);
    entry->requestLength = 256;
    memcpy(entry->request, "WTDS\1\2\260\0", 8);
    memcpy(entry->request + 8, entry->operation, 16); memcpy(entry->request + 24, entry->saveID, 16);
    memcpy(entry->request + 40, entry->credential, 32); entry->request[72] = 1;
    entry->request[80] = 2; memcpy(entry->request + 88, entry->outgoing, 136);
    assert(Hash(entry->outgoing, 136, entry->request + 224) == 0);
}

static void Rehash(unsigned char *record)
{
    assert(Hash(record, 4060, record + 4060) == 0);
}

static void CutTransition(const WtJournalEntry *before, const WtJournalEntry *next)
{
    WtJournal store, recovered;
    WtJournalEntry view;
    unsigned char oldRecord[4096], newRecord[4096];
    int failure, cut;
    assert(WtJournal_Encode(before, Hash, oldRecord) == 0);
    assert(WtJournal_Encode(next, Hash, newRecord) == 0);
    for (failure = 1; failure <= 2; failure++) {
        for (cut = 0; cut <= 4100; cut++) {
            Reset(); memcpy(banks[0], oldRecord, 4096); memcpy(banks[1], oldRecord, 4096);
            assert(WtJournal_Open(&store, IO()) == 0);
            failAt = failure; cutAt = cut;
            assert(WtJournal_Commit(&store, next) != 0);
            assert(WtJournal_Read(&store, &view) != 0);
            assert(WtJournal_Open(&recovered, IO()) == 0);
            assert(memcmp(recovered.record, failure == 2 || cut == 4100 ? newRecord : oldRecord, 4096) == 0);
            assert(WtJournal_Read(&recovered, &view) == 0);
            failAt = 0;
            assert(WtJournal_Repair(&recovered) == 0);
            assert(memcmp(banks[0], banks[1], 4096) == 0);
            view.generation++;
            assert(WtJournal_Commit(&recovered, &view) == 0);
            assert(memcmp(banks[0], banks[1], 4096) == 0);
            WtJournal_Close(&store); WtJournal_Close(&recovered);
        }
    }
}

static void ByteCuts(void)
{
    WtJournalEntry before, next;
    Identity(&before); Escrow(&next);
    CutTransition(&before, &next);
}

static void OwnershipAndStaleness(void)
{
    WtJournal first, stale;
    WtJournalEntry next, view;
    unsigned char saved[2][4096];
    Reset(); Escrow(&next);
    assert(WtJournal_Open(&first, IO()) == 0 && WtJournal_Open(&stale, IO()) == 0);
    assert(WtJournal_Commit(&first, &next) == 0);
    writes = 0;
    assert(WtJournal_Commit(&first, &next) != 0 && writes == 0);
    assert(WtJournal_Commit(&stale, &next) != 0 && writes == 0);
    assert(WtJournal_Read(&stale, &view) != 0);
    next.generation++;
    next.credential[0] ^= 1;
    next.request[40] ^= 1;
    assert(WtJournal_Commit(&first, &next) != 0 && writes == 0);
    Identity(&next); next.generation = 20; next.saveID[0] ^= 1;
    assert(WtJournal_Encode(&next, Hash, banks[0]) == 0);
    memcpy(saved, banks, sizeof(saved));
    assert(WtJournal_Open(&first, IO()) != 0 && writes == 0);
    assert(memcmp(saved, banks, sizeof(saved)) == 0);
    Reset(); banks[1][48] ^= 64; memset(banks[1] + 4092, 0, 4);
    memcpy(saved, banks, sizeof(saved));
    assert(WtJournal_Open(&first, IO()) != 0 && writes == 0);
    assert(memcmp(saved, banks, sizeof(saved)) == 0);
}

static void UnknownEvidence(void)
{
    WtJournal store;
    WtJournalEntry view;
    unsigned char saved[2][4096];
    const int offsets[] = { 4, 6, 12, 24, 152, 397, 1396, 1440, 1476, 1954, 2216 };
    size_t i;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        Reset(); banks[1][offsets[i]] = 255; memset(banks[1] + 4092, 0, 4);
        memcpy(saved, banks, sizeof(saved));
        assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
        assert(memcmp(saved, banks, sizeof(saved)) == 0);
    }
    Reset(); banks[1][48] = 0; memset(banks[1] + 49, 0, 31); Rehash(banks[1]);
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    Reset(); banks[1][1444] ^= 1; Rehash(banks[1]);
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    Reset(); readFailure = 2;
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    assert(WtJournal_Read(&store, &view) != 0);
    memset(saved, 0, sizeof(saved));
}

static void ReadbackAndPoisoning(void)
{
    WtJournal store;
    WtJournalEntry next, view;
    Reset(); Escrow(&next);
    assert(WtJournal_Open(&store, IO()) == 0);
    failAt = 1; cutAt = 100; lie = 1;
    assert(WtJournal_Commit(&store, &next) != 0 && writes == 1);
    assert(memcmp(banks[0], baseline, 4096) == 0);
    assert(WtJournal_Read(&store, &view) != 0);
    failAt = 0;
    assert(WtJournal_Commit(&store, &next) != 0 && writes == 1);
    assert(WtJournal_Open(&store, IO()) == 0);
    assert(WtJournal_Read(&store, &view) == 0 && view.generation == 1);
}

static void ReceiptImagesAndPacketBinding(void)
{
    WtJournalEntry entry, decoded;
    unsigned char record[4096];
    Escrow(&entry);
    entry.phase = WT_JOURNAL_APPLIED; entry.generation = 10; entry.requestLength = 0;
    memset(entry.request, 0, 256);
    memset(entry.trade, 11, 16); memset(entry.receipt, 12, 16); entry.deliveryKind = 1;
    entry.destination.kind = 2; entry.destination.box = 17; entry.destination.slot = 29; entry.destination.length = 136;
    memset(entry.destinationBefore, 13, 136); memset(entry.destinationAfter, 14, 136);
    memset(entry.incoming, 15, 136);
    assert(Hash(entry.incoming, 136, entry.incomingDigest) == 0);
    assert(Hash(entry.destinationAfter, entry.destination.length, entry.appliedDigest) == 0);
    entry.globalCounter = 0x12345678; entry.generalCounter = 0xffffffff; entry.boxCounter = 0x87654321;
    assert(WtJournal_Encode(&entry, Hash, record) == 0);
    assert(record[225] == 0x78 && record[228] == 0x12 && record[233] == 0x21 && record[236] == 0x87);
    assert(WtJournal_Decode(record, Hash, &decoded) == 0);
    assert(memcmp(decoded.original, entry.original, 236) == 0 && memcmp(decoded.incoming, entry.incoming, 136) == 0);
    assert(memcmp(decoded.destinationBefore, entry.destinationBefore, 236) == 0);
    assert(memcmp(decoded.destinationAfter, entry.destinationAfter, 236) == 0);
    assert(memcmp(decoded.receipt, entry.receipt, 16) == 0 && decoded.generalCounter == 0xffffffff);
    {
        const int offsets[] = { 416, 1260, 888, 301, 333, 365 };
        size_t i;
        WtJournal store;
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            Reset(); memcpy(banks[1], record, 4096);
            banks[1][offsets[i]] ^= 1; Rehash(banks[1]);
            assert(WtJournal_Decode(banks[1], Hash, &decoded) != 0);
            assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
        }
    }
    entry.destinationAfter[200] = 1;
    assert(WtJournal_Encode(&entry, Hash, record) != 0);
    Escrow(&entry); entry.request[100] ^= 1;
    assert(WtJournal_Encode(&entry, Hash, record) != 0);
    Escrow(&entry); entry.request[24] ^= 1;
    assert(WtJournal_Encode(&entry, Hash, record) != 0);
    Escrow(&entry); entry.generation = 1;
    assert(WtJournal_Encode(&entry, Hash, record) != 0);
    Escrow(&entry); entry.operationCounter = UINT64_MAX; memset(entry.request + 72, 255, 8);
    assert(WtJournal_Encode(&entry, Hash, record) != 0);
}

static int BrokenHash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    (void)bytes; (void)size;
    memset(digest, 0, 32);
    return 0;
}

static void IdentityBootstrapAndExhaustion(void)
{
    WtJournal store;
    WtJournalEntry entry, bound, empty;
    WtJournalIO io = IO();
    Reset(); io.hash = BrokenHash;
    assert(WtJournal_Open(&store, io) != 0 && reads == 0 && writes == 0);
    memset(&entry, 0, sizeof(entry)); entry.generation = 1; memset(entry.seed, 4, 32);
    assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0);
    memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0);
    Identity(&bound); bound.generation = 2;
    assert(WtJournal_Commit(&store, &bound) != 0 && writes == 0); /* Cannot skip durable enrollment. */
    empty = entry;
    entry.phase = WT_JOURNAL_ENROLLING; entry.generation++;
    memset(entry.credential, 2, 32); memset(entry.enrollmentID, 3, 16); entry.nextCounter = 1;
    entry.requestLength = 85;
    memcpy(entry.request, "WTDS\1\1\5\0", 8); memcpy(entry.request + 8, entry.enrollmentID, 16);
    memcpy(entry.request + 40, entry.credential, 32); entry.request[80] = 1;
    CutTransition(&empty, &entry);
    Reset(); assert(WtJournal_Encode(&empty, Hash, banks[0]) == 0); memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0);
    assert(WtJournal_Commit(&store, &entry) == 0 && memcmp(banks[0], banks[1], 4096) == 0);
    assert(WtJournal_Read(&store, &bound) == 0 && memcmp(bound.credential, entry.credential, 32) == 0);
    {
        const int offsets[] = { 104, 160, 416, 2000 };
        size_t i;
        unsigned char saved[4096];
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            memcpy(banks[1], banks[0], 4096); banks[1][6] = WT_JOURNAL_IDLE;
            banks[1][offsets[i]] ^= 64; memset(banks[1] + 4092, 0, 4);
            memcpy(saved, banks[1], 4096); writes = 0;
            assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
            assert(memcmp(saved, banks[1], 4096) == 0);
        }
        memcpy(banks[1], banks[0], 4096);
    }
    writes = 0;
    assert(WtJournal_OpenEmpty(&store, IO()) == WT_JOURNAL_ERROR_UNKNOWN && writes == 0);
    memset(banks[1] + 4092, 0, 4);
    assert(WtJournal_OpenEmpty(&store, IO()) == WT_JOURNAL_ERROR_UNKNOWN && writes == 0);
    assert(WtJournal_Open(&store, IO()) == 0 && WtJournal_Repair(&store) == 0);
    bound.phase = WT_JOURNAL_IDLE; bound.generation++; memset(bound.saveID, 1, 16);
    bound.requestLength = 0; memset(bound.request, 0, 256);
    {
        const int offsets[] = { 104, 160, 416 };
        unsigned char saved[4096];
        size_t i;
        int phase;
        Reset(); assert(WtJournal_Encode(&bound, Hash, banks[0]) == 0);
        for (phase = WT_JOURNAL_ENROLLING; phase <= WT_JOURNAL_IDLE; phase++) {
            for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
                assert(WtJournal_Encode(&entry, Hash, banks[1]) == 0);
                memcpy(banks[1] + 32, bound.saveID, 16); banks[1][6] = (unsigned char)phase;
                banks[1][offsets[i]] ^= 64; memset(banks[1] + 4092, 0, 4);
                memcpy(saved, banks[1], 4096); writes = 0;
                assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
                assert(memcmp(saved, banks[1], 4096) == 0);
            }
        }
        assert(WtJournal_Encode(&empty, Hash, banks[0]) == 0);
        assert(WtJournal_Encode(&entry, Hash, banks[1]) == 0);
        banks[1][1952] = 0; banks[1][1953] = 1; banks[1][1965] = 2;
        memset(banks[1] + 4092, 0, 4); memcpy(saved, banks[1], 4096);
        assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
        assert(memcmp(saved, banks[1], 4096) == 0);
    }
    CutTransition(&entry, &bound);
    entry.requestLength = 149; entry.request[6] = 69; entry.request[84] = 64;
    memset(entry.request + 85, 19, 64);
    CutTransition(&empty, &entry);
    CutTransition(&entry, &bound);
    Reset(); assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0); memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0);
    assert(WtJournal_Commit(&store, &bound) == 0 && memcmp(banks[0], banks[1], 4096) == 0);
    bound.generation++; bound.enrollmentID[0] ^= 1; writes = 0;
    assert(WtJournal_Commit(&store, &bound) != 0 && writes == 0);
    Identity(&entry); entry.generation = UINT64_MAX;
    assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0); memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0); entry.generation = 0;
    assert(WtJournal_Commit(&store, &entry) != 0 && writes == 0);
}

static void DeliveryByteCuts(void)
{
    WtJournalEntry before, next;
    Escrow(&before);
    before.phase = WT_JOURNAL_QUEUED; before.generation = 3; before.nextCounter = 2; before.eventCursor = 1;
    before.lastOperationCounter = 1; memcpy(before.lastOperation, before.operation, 16);
    memset(before.trade, 11, 16); before.requestLength = 0; memset(before.request, 0, 256);
    next = before;
    next.phase = WT_JOURNAL_RECEIVED; next.generation++; next.eventCursor++;
    memset(next.receipt, 12, 16); next.deliveryKind = 1; memset(next.incoming, 13, 136);
    assert(Hash(next.incoming, 136, next.incomingDigest) == 0);
    CutTransition(&before, &next);
    before = next;
    before.phase = WT_JOURNAL_APPLYING; before.generation++;
    before.destination.kind = 1; before.destination.slot = 3; before.destination.length = 236;
    memset(before.destinationAfter, 14, 236);
    assert(Hash(before.destinationAfter, 236, before.appliedDigest) == 0);
    next = before;
    next.phase = WT_JOURNAL_APPLIED; next.generation++;
    memcpy(next.lastReceipt, next.receipt, 16); memset(next.operation, 15, 16); next.operationCounter = 2;
    next.requestLength = 184; memcpy(next.request, "WTDS\1\6\150\0", 8);
    memcpy(next.request + 8, next.operation, 16); memcpy(next.request + 24, next.saveID, 16);
    memcpy(next.request + 40, next.credential, 32); next.request[72] = 2;
    memcpy(next.request + 80, next.trade, 16); memcpy(next.request + 96, next.receipt, 16);
    memcpy(next.request + 112, next.incomingDigest, 32); memcpy(next.request + 144, next.appliedDigest, 32);
    next.request[176] = (unsigned char)next.generation;
    CutTransition(&before, &next);
}

static void ProgressCannotMoveBackwards(void)
{
    WtJournal store;
    WtJournalEntry before, after, ignored;
    Identity(&before); before.generation = 2; before.nextCounter = 3; before.eventCursor = 4;
    before.lastOperationCounter = 2; memset(before.lastOperation, 9, 16); memset(before.lastReceipt, 10, 16);
    Reset(); assert(WtJournal_Encode(&before, Hash, banks[0]) == 0); memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0);
    after = before; after.generation++;
    memset(after.lastReceipt, 0, 16);
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    after = before; after.generation++; after.lastReceipt[0] ^= 1;
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    after = before; after.generation++; after.lastOperation[0] ^= 1;
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    after = before; after.generation++; after.nextCounter = 2; after.eventCursor = 3;
    after.lastOperationCounter = 1; memset(after.lastOperation, 8, 16);
    assert(WtJournal_Encode(&after, Hash, banks[1]) == 0);
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    assert(WtJournal_Encode(&before, Hash, banks[1]) == 0);
    assert(WtJournal_Open(&store, IO()) == 0);
    /* Accidental changes to the cached record cannot replace a valid owner. */
    store.record[48] ^= 1;
    after = before; after.generation++; after.credential[0] ^= 1;
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    assert(WtJournal_Read(&store, &ignored) != 0);
}

static void EmptyCannotDiscardActiveEvidence(void)
{
    WtJournal store;
    WtJournalEntry empty, active;
    unsigned char saved[2][4096];
    memset(&empty, 0, sizeof(empty)); empty.generation = 1; memset(empty.seed, 4, 32);
    Escrow(&active);
    Reset(); assert(WtJournal_Encode(&empty, Hash, banks[0]) == 0);
    assert(WtJournal_Encode(&active, Hash, banks[1]) == 0);
    memset(banks[1] + 4092, 0, 4);
    memcpy(saved, banks, sizeof(saved));
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    assert(WtJournal_Repair(&store) != 0 && memcmp(saved, banks, sizeof(saved)) == 0);
    assert(WtJournal_Encode(&active, Hash, banks[1]) == 0);
    memset(banks[0] + 4092, 0, 4);
    memcpy(saved, banks, sizeof(saved));
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    assert(memcmp(saved, banks, sizeof(saved)) == 0);
}

static void PendingPacketsAreImmutable(void)
{
    WtJournal store;
    WtJournalEntry before, after;
    Escrow(&before); after = before; after.generation++; after.request[80]++;
    Reset(); assert(WtJournal_Encode(&before, Hash, banks[0]) == 0);
    memcpy(banks[1], banks[0], 4096);
    assert(WtJournal_Open(&store, IO()) == 0);
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    assert(WtJournal_Encode(&after, Hash, banks[1]) == 0);
    assert(WtJournal_Open(&store, IO()) != 0 && writes == 0);
    memcpy(banks[1], banks[0], 4096); assert(WtJournal_Open(&store, IO()) == 0);
    after = before; after.generation++; after.operation[0] ^= 1; after.request[8] ^= 1;
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    after = before; after.generation++; after.requestLength = 0; memset(after.request, 0, 256);
    assert(WtJournal_Commit(&store, &after) != 0 && writes == 0);
    after.phase = WT_JOURNAL_QUEUED; memset(after.trade, 11, 16);
    after.nextCounter = 2; after.lastOperationCounter = before.operationCounter;
    memcpy(after.lastOperation, before.operation, 16);
    assert(WtJournal_Commit(&store, &after) == 0);
}

int main(void)
{
    WtJournalEntry entry;
    /* Hashing is independent of the transport RNG/entropy initialization. */
    Identity(&entry); assert(WtJournal_Encode(&entry, Hash, baseline) == 0);
    ByteCuts(); OwnershipAndStaleness(); UnknownEvidence(); ReadbackAndPoisoning(); ReceiptImagesAndPacketBinding();
    IdentityBootstrapAndExhaustion();
    DeliveryByteCuts();
    ProgressCannotMoveBackwards();
    EmptyCannotDiscardActiveEvidence();
    PendingPacketsAreImmutable();
    puts("Journal byte-cut, identity, replay, evidence, readback and image-binding checks passed");
    return 0;
}
