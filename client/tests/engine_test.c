#include "wonder_trade/engine.h"
#include "wonder_trade/seed.h"
#include "journal_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <psa/crypto.h>

typedef struct Fixture {
    unsigned char banks[2][4096], pokemon[136], delivered[136];
    unsigned random, writes, failWrite, failStep, removed, received, restored, full;
    unsigned char id;
    int rotateSeed;
    WtEngine engine;
} Fixture;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

static void Put(unsigned char *bytes, uint64_t value, unsigned size)
{
    while (size--) { *bytes++ = (unsigned char)value; value >>= 8; }
}

static int Read(void *arg, int bank, unsigned char record[4096])
{
    memcpy(record, ((Fixture *)arg)->banks[bank], 4096); return 0;
}

static int Write(void *arg, int bank, const unsigned char record[4096])
{
    Fixture *fixture = arg;
    if (++fixture->writes == fixture->failWrite) return -1;
    memcpy(fixture->banks[bank], record, 4096); return 0;
}

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    Fixture *fixture = arg;
    unsigned char input[8] = { 0 }, digest[32];
    if (fixture->random == UINT32_MAX) return -1;
    Put(input, ++fixture->random, 4); input[4] = fixture->id;
    if (fixture->rotateSeed) {
        unsigned char seed[32];
        memset(seed, fixture->random + 10, sizeof(seed));
        assert(WtSeed_Write(&fixture->engine.journal, seed, sizeof(seed)) == 32);
    }
    assert(size <= 32 && Hash(input, sizeof(input), digest) == 0);
    memcpy(bytes, digest, size); return 0;
}

static void Mirrored(Fixture *fixture, unsigned phase)
{
    WtJournalEntry entry;
    assert(memcmp(fixture->banks[0], fixture->banks[1], 4096) == 0);
    assert(WtJournal_Decode(fixture->banks[0], Hash, &entry) == 0 && entry.phase == phase);
}

static int Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    Fixture *fixture = arg;
    if ((unsigned)step + 1 == fixture->failStep) return -1;
    switch (step) {
    case WT_INVENTORY_PREPARE_REMOVE:
        assert(!fixture->removed && slot == 0);
        entry->source.kind = 1; entry->source.slot = 0; entry->source.length = 236;
        memset(entry->original, fixture->id, 236); memcpy(entry->outgoing, fixture->pokemon, 136);
        assert(Hash(entry->original, 236, entry->originalDigest) == 0);
        memset(entry->inventoryBefore, 1, 32); memset(entry->inventoryAfter, 2, 32);
        return 0;
    case WT_INVENTORY_REMOVE:
        Mirrored(fixture, WT_JOURNAL_ESCROW);
        fixture->removed = 1; entry->globalCounter = entry->generalCounter = entry->boxCounter = 2;
        return 0;
    case WT_INVENTORY_VERIFY_REMOVED: return fixture->removed ? 0 : -1;
    case WT_INVENTORY_PREPARE_RECEIVE:
        if (fixture->full) return 1;
        Mirrored(fixture, WT_JOURNAL_RECEIVED);
        entry->destination.kind = 1; entry->destination.slot = 1; entry->destination.length = 236;
        memset(entry->destinationBefore, 0, 236); memset(entry->destinationAfter, 3, 236);
        memcpy(entry->destinationAfter, entry->incoming, 136);
        assert(Hash(entry->incoming, 136, entry->appliedDigest) == 0);
        return 0;
    case WT_INVENTORY_RECEIVE:
        Mirrored(fixture, WT_JOURNAL_APPLYING);
        if (!fixture->received) { fixture->received = 1; memcpy(fixture->delivered, entry->incoming, 136); }
        return 0;
    case WT_INVENTORY_VERIFY_RECEIVED: return fixture->received && memcmp(fixture->delivered, entry->incoming, 136) == 0 ? 0 : -1;
    case WT_INVENTORY_RESTORE:
        Mirrored(fixture, WT_JOURNAL_RESTORING);
        fixture->removed = 0; fixture->restored = 1; return 0;
    }
    return -1;
}

static void Open(Fixture *fixture)
{
    WtJournalIO journal = { fixture, Read, Write, Hash };
    WtEngineIO io = { fixture, Random, Inventory };
    assert(WtEngine_Open(&fixture->engine, journal, io) == 0);
}

static void Init(Fixture *fixture, unsigned char id, unsigned char species)
{
    WtJournalEntry entry;
    memset(fixture, 0, sizeof(*fixture)); fixture->id = id;
    WtTest_Pokemon(fixture->pokemon, species);
    assert(WtTest_JournalEntry(WT_JOURNAL_EMPTY, Hash, &entry) == 0);
    assert(WtJournal_Encode(&entry, Hash, fixture->banks[0]) == 0);
    memcpy(fixture->banks[1], fixture->banks[0], 4096); Open(fixture);
}

static size_t Header(Fixture *fixture, unsigned char packet[902], unsigned result, uint64_t counter, uint64_t highwater, size_t body)
{
    memset(packet, 0, 902); memcpy(packet, "WTDS\1", 5);
    packet[5] = fixture->engine.frame[5]; Put(packet + 6, body, 2);
    memcpy(packet + 8, fixture->engine.frame + 8, 16); Put(packet + 24, result, 2);
    Put(packet + 28, counter, 8); Put(packet + 36, highwater, 8);
    return body + 44;
}

static void Enroll(Fixture *fixture)
{
    unsigned char packet[902], pending[256];
    size_t size;
    assert(WtEngine_Enroll(&fixture->engine, 1) == 0);
    Mirrored(fixture, WT_JOURNAL_ENROLLING);
    memcpy(pending, fixture->engine.entry.request, 256);
    /* A lost enrollment response cannot generate replacement credentials. */
    WtEngine_Close(&fixture->engine); Open(fixture);
    assert(WtEngine_Enroll(&fixture->engine, 99) == 0);
    assert(memcmp(pending, fixture->engine.entry.request, 256) == 0);
    assert(WtEngine_Request(&fixture->engine, 0) == 0);
    size = Header(fixture, packet, WT_RESULT_OK, 1, 1, 16); memset(packet + 44, fixture->id, 16);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == 0);
    assert(fixture->engine.entry.phase == WT_JOURNAL_IDLE && !fixture->engine.synchronized);
    assert(WtEngine_Deposit(&fixture->engine, 0) != 0);
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && fixture->engine.frame[5] == WT_OP_RECONCILE);
    size = Header(fixture, packet, WT_RESULT_OK, 1, 1, 116);
    packet[44] = 1; Put(packet + 46, 1, 8); Put(packet + 54, 1, 8); packet[62] = WT_EVENT_ENROLLED;
    assert(WtEngine_Reply(&fixture->engine, packet, size) == 0 && fixture->engine.synchronized);
    Mirrored(fixture, WT_JOURNAL_IDLE);
}

static void Snapshot(Fixture *fixture, unsigned char *snapshot, unsigned state, unsigned kind, unsigned event, const unsigned char *incoming)
{
    memset(snapshot, 0, 138); memset(snapshot, fixture->id + 10, 16);
    snapshot[16] = (unsigned char)state; snapshot[17] = (unsigned char)kind;
    memset(snapshot + 18, fixture->id + 20, 16); assert(Hash(fixture->pokemon, 136, snapshot + 34) == 0);
    if (kind) {
        memset(snapshot + 66, fixture->id + 30, 16); assert(Hash(incoming, 136, snapshot + 82) == 0);
        memset(snapshot + 114, fixture->id + 40, 16);
        if (kind == WT_DELIVERY_RETURN) memcpy(snapshot + 66, snapshot + 18, 48);
    }
    Put(snapshot + 130, event, 8);
}

static void Submit(Fixture *fixture)
{
    unsigned char packet[902], pending[256];
    size_t size;
    assert(WtEngine_Deposit(&fixture->engine, 0) == 0);
    Mirrored(fixture, WT_JOURNAL_SUBMIT_READY); assert(fixture->removed);
    memcpy(pending, fixture->engine.entry.request, 256);
    assert(WtEngine_Request(&fixture->engine, 0) == 0);
    /* A committed Submit with a lost response must replay the identical frame. */
    WtEngine_Close(&fixture->engine); Open(fixture);
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && memcmp(fixture->engine.frame, pending, 256) == 0);
    size = Header(fixture, packet, WT_RESULT_OK, 2, 2, 146); Put(packet + 44, 2, 8);
    Snapshot(fixture, packet + 52, WT_TRADE_QUEUED, WT_DELIVERY_NONE, 2, NULL);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == 0);
    Mirrored(fixture, WT_JOURNAL_QUEUED); assert(fixture->engine.entry.nextCounter == 2);
    fixture->engine.synchronized = 1;
}

static void Collect(Fixture *fixture, const unsigned char incoming[136], unsigned kind, unsigned counter)
{
    unsigned char packet[902], pending[256];
    size_t size;
    assert(WtEngine_Request(&fixture->engine, WT_OP_COLLECT) == 0 && fixture->engine.frame[5] == WT_OP_COLLECT);
    size = Header(fixture, packet, WT_RESULT_OK, counter, 3, 274);
    Snapshot(fixture, packet + 44, kind == WT_DELIVERY_MATCH ? WT_TRADE_MATCHED : WT_TRADE_RETURN_READY, kind, 3, incoming);
    memcpy(packet + 182, incoming, 136);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == (fixture->full ? 1 : 0));
    if (fixture->full) {
        assert(fixture->engine.entry.phase == WT_JOURNAL_RECEIVED && !fixture->received);
        fixture->full = 0;
    }
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && fixture->engine.frame[5] == WT_OP_ACKNOWLEDGE);
    Mirrored(fixture, WT_JOURNAL_APPLIED); assert(fixture->received == 1);
    memcpy(pending, fixture->engine.entry.request, 256);
    WtEngine_Close(&fixture->engine); Open(fixture);
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && memcmp(fixture->engine.frame, pending, 184) == 0);
    assert(fixture->received == 1);
    size = Header(fixture, packet, WT_RESULT_OK, counter + 1, 4, 146); Put(packet + 44, 4, 8);
    Snapshot(fixture, packet + 52, WT_TRADE_ACKNOWLEDGED, kind, 4, incoming);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == 0);
    Mirrored(fixture, WT_JOURNAL_IDLE); assert(fixture->engine.entry.nextCounter == counter + 1);
    assert(memcmp(fixture->delivered, incoming, 136) == 0);
}

static void PairAndReturn(void)
{
    Fixture a, b;
    unsigned char packet[902]; size_t size;
    Init(&a, 1, 25); Init(&b, 2, 2); a.rotateSeed = 1;
    Enroll(&a); Enroll(&b); Submit(&a); Submit(&b);
    b.full = 1; Collect(&a, b.pokemon, WT_DELIVERY_MATCH, 2); Collect(&b, a.pokemon, WT_DELIVERY_MATCH, 2);
    assert(a.received == 1 && b.received == 1 && memcmp(a.engine.entry.saveID, b.engine.entry.saveID, 16) != 0);
    Init(&a, 1, 25); Enroll(&a); Submit(&a);
    assert(WtEngine_Request(&a.engine, WT_OP_CANCEL) == 0);
    Mirrored(&a, WT_JOURNAL_QUEUED);
    size = Header(&a, packet, WT_RESULT_OK, 3, 3, 146); Put(packet + 44, 3, 8);
    Snapshot(&a, packet + 52, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3, a.pokemon);
    assert(WtEngine_Reply(&a.engine, packet, size) == 0);
    Collect(&a, a.pokemon, WT_DELIVERY_RETURN, 3);
}

static void RejectionAndErrors(void)
{
    Fixture fixture;
    unsigned char packet[902], before[2][4096]; size_t size;
    Init(&fixture, 1, 25); Enroll(&fixture);
    assert(WtEngine_Deposit(&fixture.engine, 0) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
    size = Header(&fixture, packet, WT_RESULT_ILLEGAL_POKEMON, 2, 2, 0);
    memcpy(before, fixture.banks, sizeof(before));
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.removed && !fixture.restored);
    assert(memcmp(before, fixture.banks, sizeof(before)) == 0);
    assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0 && fixture.engine.frame[5] == WT_OP_RECONCILE);
    size = Header(&fixture, packet, WT_RESULT_OK, 2, 2, 116);
    packet[44] = 1; Put(packet + 46, 2, 8); Put(packet + 54, 2, 8); packet[62] = WT_EVENT_REJECTED;
    memcpy(packet + 54 + 25, fixture.engine.entry.operation, 16);
    assert(Hash(fixture.pokemon, 136, packet + 54 + 57) == 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && fixture.restored && !fixture.removed);
    Mirrored(&fixture, WT_JOURNAL_IDLE);
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_STATUS) == 0);
    memcpy(before, fixture.banks, sizeof(before));
    size = Header(&fixture, packet, WT_RESULT_NOT_FOUND, 2, 2, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.removed && !fixture.restored);
    assert(memcmp(before, fixture.banks, sizeof(before)) == 0);
    assert(WtEngine_Request(&fixture.engine, WT_OP_STATUS) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 99, 3, 138);
    Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, fixture.pokemon);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && fixture.removed);
}

static void FailedPersistence(void)
{
    Fixture fixture;
    unsigned failure;
    for (failure = 1; failure <= 4; ++failure) {
        Init(&fixture, 1, 25); Enroll(&fixture);
        fixture.failWrite = fixture.writes + failure;
        assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && fixture.engine.blocked);
        assert(WtEngine_Request(&fixture.engine, 0) != 0 && fixture.engine.frameSize == 0);
        fixture.failWrite = 0;
        WtEngine_Close(&fixture.engine); Open(&fixture);
        if (fixture.engine.entry.phase == WT_JOURNAL_IDLE) assert(!fixture.removed);
        else assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.removed);
    }
    Init(&fixture, 1, 25); fixture.random = UINT32_MAX;
    assert(WtEngine_Enroll(&fixture.engine, 1) != 0 && fixture.engine.blocked);
    Mirrored(&fixture, WT_JOURNAL_EMPTY);
    Init(&fixture, 1, 25); Enroll(&fixture); fixture.failStep = WT_INVENTORY_REMOVE + 1;
    assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && !fixture.removed);
    Mirrored(&fixture, WT_JOURNAL_ESCROW);
    fixture.failStep = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.removed);
}

static void DeliveryPersistence(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136];
    unsigned failure;
    size_t size;
    WtTest_Pokemon(incoming, 2);
    for (failure = 1; failure <= 6; ++failure) {
        Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
        assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
        size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
        Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming);
        memcpy(packet + 182, incoming, 136);
        fixture.failWrite = fixture.writes + failure;
        assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked);
        assert(!fixture.engine.frameSize && fixture.received <= 1);
        fixture.failWrite = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
        if (fixture.engine.entry.phase == WT_JOURNAL_QUEUED) {
            fixture.engine.synchronized = 1;
            assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
            size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
            Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming);
            memcpy(packet + 182, incoming, 136);
            assert(WtEngine_Reply(&fixture.engine, packet, size) == 0);
        }
        assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.engine.frame[5] == WT_OP_ACKNOWLEDGE);
        assert(fixture.received == 1 && memcmp(fixture.delivered, incoming, 136) == 0);
        Mirrored(&fixture, WT_JOURNAL_APPLIED);
    }
}

int main(void)
{
    PairAndReturn(); RejectionAndErrors(); FailedPersistence(); DeliveryPersistence();
    puts("Recoverable engine: paired delivery, return, rejection, retries, full storage, stale context and failed persistence passed");
    return 0;
}
