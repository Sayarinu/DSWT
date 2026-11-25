#include "wonder_trade/engine.h"
#include "wonder_trade/seed.h"
#include "journal_fixture.h"
#include "wonder_trade/receive_guard.h"
#include "wonder_trade/wire_layout.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)
#include <psa/crypto.h>

typedef struct Fixture {
    unsigned char banks[2][4096], pokemon[136], delivered[136], checked[136];
    unsigned random, writes, failWrite, failStep, removed, received, restored, full, cannotHold, holdChecks, changed;
    unsigned reads, failRead;
    unsigned char id;
    int rotateSeed;
    unsigned char inventory[4][236], empty[236];
    uint32_t saveCounters[3];
    unsigned saveWrites, insertions;
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
    Fixture *fixture = arg;
    if (++fixture->reads == fixture->failRead) return -1;
    memcpy(record, fixture->banks[bank], 4096); return 0;
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
        /* A delivery, or a returned offer the save no longer has room for in place. */
        assert(entry->phase == WT_JOURNAL_RECEIVED || entry->phase == WT_JOURNAL_RESTORING);
        Mirrored(fixture, entry->phase);
        entry->destination.kind = 1; entry->destination.slot = 1; entry->destination.length = 236;
        memset(entry->destinationBefore, 0, 236); memset(entry->destinationAfter, 3, 236);
        memcpy(entry->destinationAfter, entry->incoming, 136);
        assert(Hash(entry->incoming, 136, entry->appliedDigest) == 0);
        return 0;
    case WT_INVENTORY_RECEIVE:
        assert(entry->phase == WT_JOURNAL_APPLYING || entry->phase == WT_JOURNAL_RESTORING);
        Mirrored(fixture, entry->phase);
        if (!fixture->received) { fixture->received = 1; memcpy(fixture->delivered, entry->incoming, 136); }
        return 0;
    case WT_INVENTORY_VERIFY_RECEIVED: return fixture->received && memcmp(fixture->delivered, entry->incoming, 136) == 0 ? 0 : -1;
    case WT_INVENTORY_PREPARE_RESTORE: return fixture->changed ? 1 : 0;
    case WT_INVENTORY_RESTORE:
        assert(!fixture->changed);
        Mirrored(fixture, WT_JOURNAL_RESTORING);
        fixture->removed = 0; fixture->restored = 1; return 0;
    }
    return -1;
}

static int CanHold(void *arg, const unsigned char pk4[136])
{
    Fixture *fixture = arg;
    ++fixture->holdChecks; memcpy(fixture->checked, pk4, 136);
    return !fixture->cannotHold;
}

static void Open(Fixture *fixture)
{
    WtJournalIO journal = { fixture, Read, Write, Hash };
    WtEngineIO io = { fixture, Random, Inventory, CanHold };
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
    memset(packet, 0, 902); memcpy(packet, fixture->engine.frame, 5); /* A reply repeats its request's version. */
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

static void Collect(Fixture *fixture, const unsigned char incoming[136], unsigned kind, unsigned counter, unsigned ackEvent)
{
    unsigned char packet[902], pending[256];
    WtWireReply wire;
    size_t size;
    assert(WtEngine_Request(&fixture->engine, WT_OP_COLLECT) == 0 && fixture->engine.frame[5] == WT_OP_COLLECT);
    size = Header(fixture, packet, WT_RESULT_OK, counter, counter + 1, 274);
    Snapshot(fixture, packet + 44, kind == WT_DELIVERY_MATCH ? WT_TRADE_MATCHED : WT_TRADE_RETURN_READY, kind, 3, incoming);
    memcpy(packet + 182, incoming, 136);
    assert(WtWire_Decode(fixture->engine.frame, fixture->engine.frameSize, packet, size, Hash, &wire) == 0);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == (fixture->full ? 1 : 0));
    if (fixture->full) {
        assert(fixture->engine.entry.phase == WT_JOURNAL_RECEIVED && !fixture->received);
        assert(memcmp(fixture->engine.entry.incoming, incoming, 136) == 0);
        assert(memcmp(fixture->engine.entry.incomingDigest, wire.body.collection.snapshot.receivedDigest, 32) == 0);
        WtEngine_Close(&fixture->engine); Open(fixture);
        assert(fixture->engine.entry.phase == WT_JOURNAL_RECEIVED && !fixture->received);
        assert(memcmp(fixture->engine.entry.incoming, incoming, 136) == 0);
        fixture->full = 0;
    }
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && fixture->engine.frame[5] == WT_OP_ACKNOWLEDGE);
    Mirrored(fixture, WT_JOURNAL_APPLIED); assert(fixture->received == 1);
    /* A successful Collect can prepare the next mutation. Its wire result
     * remains OK while the engine starts fresh ACK recovery bookkeeping. */
    assert(wire.result == WT_RESULT_OK && fixture->engine.lastResult == 0);
    assert(fixture->engine.entry.request[5] == WT_OP_ACKNOWLEDGE);
    memcpy(pending, fixture->engine.entry.request, 256);
    WtEngine_Close(&fixture->engine); Open(fixture);
    assert(WtEngine_Request(&fixture->engine, 0) == 0 && memcmp(fixture->engine.frame, pending, 184) == 0);
    assert(fixture->received == 1);
    size = Header(fixture, packet, WT_RESULT_OK, counter + 1, ackEvent, 146); Put(packet + 44, ackEvent, 8);
    Snapshot(fixture, packet + 52, WT_TRADE_ACKNOWLEDGED, kind, ackEvent, incoming);
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
    b.full = 1; Collect(&a, b.pokemon, WT_DELIVERY_MATCH, 2, 4); Collect(&b, a.pokemon, WT_DELIVERY_MATCH, 2, 4);
    assert(a.received == 1 && b.received == 1 && memcmp(a.engine.entry.saveID, b.engine.entry.saveID, 16) != 0);
    Init(&a, 1, 25); Enroll(&a); Submit(&a);
    assert(WtEngine_Request(&a.engine, WT_OP_CANCEL) == 0);
    Mirrored(&a, WT_JOURNAL_QUEUED);
    size = Header(&a, packet, WT_RESULT_OK, 3, 3, 146); Put(packet + 44, 3, 8);
    Snapshot(&a, packet + 52, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3, a.pokemon);
    assert(WtEngine_Reply(&a.engine, packet, size) == 0);
    /* A return is this save's own offer, so it is not checked against the game. */
    a.cannotHold = 1; a.holdChecks = 0;
    Collect(&a, a.pokemon, WT_DELIVERY_RETURN, 3, 4);
    assert(!a.holdChecks);
}

static void RefusedDelivery(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136], before[2][4096];
    unsigned attempt;
    size_t size;
    WtTest_Pokemon(incoming, 2);
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    fixture.cannotHold = 1;
    for (attempt = 0; attempt < 2; ++attempt) {
        /* Collect replays the delivery; a refusal records, imports and acknowledges nothing. */
        assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0 && fixture.engine.frame[5] == WT_OP_COLLECT);
        size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
        Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming);
        memcpy(packet + 182, incoming, 136);
        memcpy(before, fixture.banks, sizeof(before));
        assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.engine.refused && !fixture.engine.blocked);
        assert(fixture.holdChecks == attempt + 1 && memcmp(fixture.checked, incoming, 136) == 0);
        assert(memcmp(before, fixture.banks, sizeof(before)) == 0 && !fixture.received && fixture.removed);
        assert(fixture.engine.entry.phase == WT_JOURNAL_QUEUED && !fixture.engine.frameSize);
        assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.engine.frame[5] == WT_OP_STATUS);
        /* After a restart the save is still QUEUED with its offer escrowed. */
        WtEngine_Close(&fixture.engine); Open(&fixture);
        Mirrored(&fixture, WT_JOURNAL_QUEUED); assert(!fixture.received && fixture.removed);
        fixture.engine.synchronized = 1;
    }
    /* A build that can hold it collects the same pending delivery. */
    fixture.cannotHold = 0;
    Collect(&fixture, incoming, WT_DELIVERY_MATCH, 2, 4);
    assert(fixture.holdChecks == 3 && !fixture.engine.refused);
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
    assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0 && fixture.engine.frame[5] == WT_OP_COLLECT);
    memcpy(before, fixture.banks, sizeof(before));
    size = Header(&fixture, packet, WT_RESULT_CONFLICT, 2, 2, 0);  /* No partner yet: nothing is imported. */
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && !fixture.engine.blocked && fixture.removed && !fixture.restored);
    assert(memcmp(before, fixture.banks, sizeof(before)) == 0 && fixture.engine.entry.phase == WT_JOURNAL_QUEUED);
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_STATUS) == 0);
    memcpy(before, fixture.banks, sizeof(before));
    size = Header(&fixture, packet, WT_RESULT_NOT_FOUND, 2, 2, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.removed && !fixture.restored);
    assert(memcmp(before, fixture.banks, sizeof(before)) == 0);
    fixture.engine.synchronized = 1;
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

static void RejectedCancel(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136], digest[32];
    unsigned char *event;
    size_t size;
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    WtTest_Pokemon(incoming, 2); assert(Hash(incoming, 136, digest) == 0);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_CONFLICT, 3, 4, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.removed);
    assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0 && fixture.engine.frame[5] == WT_OP_RECONCILE);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 4, 222);
    packet[44] = 2; Put(packet + 46, 4, 8);
    event = packet + 54; Put(event, 3, 8); event[8] = WT_EVENT_MATCHED;
    memcpy(event + 9, fixture.engine.entry.trade, 16); memset(event + 41, 31, 16);
    memcpy(event + 57, digest, 32); memset(event + 89, 41, 16); event[105] = WT_TRADE_MATCHED;
    event += 106; Put(event, 4, 8); event[8] = WT_EVENT_REJECTED;
    memcpy(event + 9, fixture.engine.entry.trade, 16); memcpy(event + 25, fixture.engine.entry.operation, 16);
    event[105] = WT_TRADE_MATCHED;
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 0);
    assert(fixture.engine.entry.phase == WT_JOURNAL_QUEUED && !fixture.engine.entry.requestLength
        && fixture.engine.entry.nextCounter == 3 && !fixture.restored);
    assert(memcmp(fixture.engine.entry.incomingDigest, digest, 32) == 0);
    Mirrored(&fixture, WT_JOURNAL_QUEUED);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) != 0 && !fixture.engine.blocked && !fixture.engine.frameSize);
    /* Receipt and digest remain bound even across a process restart. */
    WtEngine_Close(&fixture.engine); Open(&fixture); fixture.engine.synchronized = 1;
    assert(WtEngine_Request(&fixture.engine, WT_OP_STATUS) == 0 && fixture.engine.frame[5] == WT_OP_STATUS);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 4, 138);
    Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming);
    packet[44 + 114] ^= 1;
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.received);
}

static void MatchedCancelRace(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136], digest[32];
    unsigned char *event;
    size_t size;
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    WtTest_Pokemon(incoming, 2); assert(Hash(incoming, 136, digest) == 0);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_CONFLICT, 3, 4, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && fixture.removed && !fixture.restored);
    assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 4, 222);
    packet[44] = 2; Put(packet + 46, 4, 8);
    event = packet + 54; Put(event, 3, 8); event[8] = WT_EVENT_MATCHED;
    memcpy(event + 9, fixture.engine.entry.trade, 16); memset(event + 41, 31, 16);
    memcpy(event + 57, digest, 32); memset(event + 89, 41, 16); event[105] = WT_TRADE_MATCHED;
    event += 106; Put(event, 4, 8); event[8] = WT_EVENT_REJECTED;
    memcpy(event + 9, fixture.engine.entry.trade, 16); memcpy(event + 25, fixture.engine.entry.operation, 16);
    event[105] = WT_TRADE_MATCHED;
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && fixture.engine.synchronized);
    assert(fixture.engine.entry.phase == WT_JOURNAL_QUEUED && fixture.engine.entry.deliveryKind == WT_DELIVERY_MATCH);
    assert(memcmp(fixture.engine.entry.receipt, packet + 54 + 89, 16) == 0);
    assert(memcmp(fixture.engine.entry.incomingDigest, digest, 32) == 0);
    assert(fixture.removed && !fixture.restored && !fixture.received);
    Collect(&fixture, incoming, WT_DELIVERY_MATCH, 3, 5);
    assert(fixture.engine.entry.phase == WT_JOURNAL_IDLE && fixture.removed && !fixture.restored);
    assert(fixture.received && memcmp(fixture.delivered, incoming, 136) == 0);
}

/* The server hands back an offer its name screening refused: a CANCELLED event
 * without an operation, which used none of this save's counters. */
static void ServerReturn(void)
{
    Fixture fixture;
    unsigned char packet[902], digest[32];
    unsigned char *event;
    size_t size;
    unsigned attempt;
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(Hash(fixture.pokemon, 136, digest) == 0);
    for (attempt = 0; attempt < 2; ++attempt) {
        assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
        size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 116);
        packet[44] = 1; Put(packet + 46, 3, 8);
        event = packet + 54; Put(event, 3, 8); event[8] = WT_EVENT_CANCELLED;
        memcpy(event + 9, fixture.engine.entry.trade, 16); memset(event + 41, 31, 16);
        memcpy(event + 57, digest, 32); memset(event + 89, 41, 16); event[105] = WT_TRADE_RETURN_READY;
        /* Only this save's own Pokémon can come back. */
        if (attempt == 0) {
            event[57] ^= 1;
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0);
            assert(fixture.engine.entry.phase == WT_JOURNAL_QUEUED && !fixture.engine.entry.deliveryKind);
            WtEngine_Close(&fixture.engine); Open(&fixture);
            continue;
        }
        assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && fixture.engine.synchronized);
    }
    assert(fixture.engine.entry.phase == WT_JOURNAL_QUEUED && fixture.engine.entry.deliveryKind == WT_DELIVERY_RETURN);
    assert(fixture.engine.entry.nextCounter == 2 && memcmp(fixture.engine.entry.receipt, packet + 54 + 89, 16) == 0);
    fixture.cannotHold = 1; fixture.holdChecks = 0;
    Collect(&fixture, fixture.pokemon, WT_DELIVERY_RETURN, 2, 4);
    assert(!fixture.holdChecks && fixture.engine.entry.phase == WT_JOURNAL_IDLE);
}

static void LoadedStateAndMismatch(void)
{
    Fixture fixture;
    WtJournalEntry entry;
    WtJournalIO journal = { &fixture, Read, Write, Hash };
    WtEngineIO io = { &fixture, Random, Inventory, CanHold };
    unsigned phase, code;
    /* A deposit must keep its Submit. APPLIED may lack its ACK, which is
     * prepared on the next visit (LocalSettlement). */
    const unsigned phases[] = { WT_JOURNAL_ESCROW, WT_JOURNAL_SUBMIT_READY };
    unsigned char packet[902]; size_t size;
    for (phase = 0; phase < sizeof(phases) / sizeof(*phases); ++phase) {
        Init(&fixture, 1, 25);
        assert(WtTest_JournalEntry(phases[phase], Hash, &entry) == 0);
        memset(entry.request, 0, sizeof(entry.request)); entry.requestLength = 0;
        assert(WtJournal_Encode(&entry, Hash, fixture.banks[0]) == 0);
        memcpy(fixture.banks[1], fixture.banks[0], 4096);
        fixture.writes = 0;
        assert(WtEngine_Open(&fixture.engine, journal, io) != 0);
        assert(!fixture.writes && !fixture.removed && !fixture.received);
    }
    Init(&fixture, 1, 25);
    assert(WtTest_JournalEntry(WT_JOURNAL_ESCROW, Hash, &entry) == 0);
    entry.outgoing[4] = 1; memcpy(entry.request + 88, entry.outgoing, 136);
    assert(Hash(entry.outgoing, 136, entry.request + 224) == 0);
    assert(WtJournal_Encode(&entry, Hash, fixture.banks[0]) == 0);
    memcpy(fixture.banks[1], fixture.banks[0], 4096); fixture.writes = 0;
    assert(WtEngine_Open(&fixture.engine, journal, io) != 0 && !fixture.writes && !fixture.removed);
    for (code = WT_RESULT_COUNTER_MISMATCH; code <= WT_RESULT_CURSOR_AHEAD; ++code) {
        Init(&fixture, 1, 25); Enroll(&fixture);
        assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
        size = Header(&fixture, packet, code, 1, 1, 0);
        assert(WtEngine_Reply(&fixture.engine, packet, size) != 0);
        assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && !fixture.removed);
    }
    Init(&fixture, 1, 25); Enroll(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
    size = Header(&fixture, packet, WT_RESULT_UNAVAILABLE, 0, 0, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && !fixture.engine.synchronized);
    assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && !fixture.removed);
}

static void DeliveryKindBinding(void)
{
    Fixture fixture;
    WtJournalEntry known;
    unsigned char packet[902]; size_t size;
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
    Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, fixture.pokemon);
    memcpy(packet + 182, fixture.pokemon, 136);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 4, 146); Put(packet + 44, 4, 8);
    Snapshot(&fixture, packet + 52, WT_TRADE_ACKNOWLEDGED, WT_DELIVERY_RETURN, 4, fixture.pokemon);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && fixture.received == 1);
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 3, 146); Put(packet + 44, 3, 8);
    Snapshot(&fixture, packet + 52, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3, fixture.pokemon);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && fixture.engine.entry.deliveryKind == WT_DELIVERY_RETURN);
    assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 3, 274);
    Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, fixture.pokemon);
    memcpy(packet + 182, fixture.pokemon, 136);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.received);
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    known = fixture.engine.entry; known.generation++;
    known.deliveryKind = WT_DELIVERY_MATCH; memset(known.receipt, 41, 16);
    assert(Hash(fixture.pokemon, 136, known.incomingDigest) == 0);
    assert(WtJournal_Commit(&fixture.engine.journal, &known) == 0);
    WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.engine.frame[5] == WT_OP_CANCEL);
    size = Header(&fixture, packet, WT_RESULT_OK, 3, 3, 146); Put(packet + 44, 3, 8);
    Snapshot(&fixture, packet + 52, WT_TRADE_RETURN_READY, WT_DELIVERY_RETURN, 3, fixture.pokemon);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.restored);
}

static void EnrollmentAndRetirementFaults(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136];
    unsigned stage, failure;
    size_t size;
    WtTest_Pokemon(incoming, 2);
    for (stage = 0; stage < 4; ++stage) for (failure = 1; failure <= (stage == 2 ? 4U : 2U); ++failure) {
        Init(&fixture, 1, 25);
        if (stage == 0) {
            fixture.failWrite = fixture.writes + failure;
            assert(WtEngine_Enroll(&fixture.engine, 1) != 0);
        } else if (stage == 1) {
            assert(WtEngine_Enroll(&fixture.engine, 1) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
            size = Header(&fixture, packet, WT_RESULT_OK, 1, 1, 16); memset(packet + 44, 1, 16);
            fixture.failWrite = fixture.writes + failure;
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0);
        } else {
            Enroll(&fixture);
            if (stage == 2) {
                assert(WtEngine_Deposit(&fixture.engine, 0) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
                size = Header(&fixture, packet, WT_RESULT_ILLEGAL_POKEMON, 2, 2, 0);
                assert(WtEngine_Reply(&fixture.engine, packet, size) == 1);
                assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
                size = Header(&fixture, packet, WT_RESULT_OK, 2, 2, 116);
                packet[44] = 1; Put(packet + 46, 2, 8); Put(packet + 54, 2, 8); packet[62] = WT_EVENT_REJECTED;
                memcpy(packet + 79, fixture.engine.entry.operation, 16); assert(Hash(fixture.pokemon, 136, packet + 111) == 0);
            } else {
                Submit(&fixture); assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
                size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
                Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming); memcpy(packet + 182, incoming, 136);
                assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
                size = Header(&fixture, packet, WT_RESULT_OK, 3, 4, 146); Put(packet + 44, 4, 8);
                Snapshot(&fixture, packet + 52, WT_TRADE_ACKNOWLEDGED, WT_DELIVERY_MATCH, 4, incoming);
            }
            fixture.failWrite = fixture.writes + failure;
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0);
        }
        assert(fixture.engine.blocked && !fixture.engine.frameSize);
        fixture.failWrite = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
        if (stage == 0) assert(fixture.engine.entry.phase == WT_JOURNAL_EMPTY || fixture.engine.entry.phase == WT_JOURNAL_ENROLLING);
        if (stage == 1) assert(fixture.engine.entry.phase == WT_JOURNAL_ENROLLING || fixture.engine.entry.phase == WT_JOURNAL_IDLE);
        if (stage == 2) {
            assert(fixture.engine.entry.phase == WT_JOURNAL_SUBMIT_READY || fixture.engine.entry.phase == WT_JOURNAL_RESTORING || fixture.engine.entry.phase == WT_JOURNAL_IDLE);
            if (fixture.engine.entry.phase == WT_JOURNAL_RESTORING) assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.restored);
        }
        if (stage == 3) assert(fixture.received == 1 && (fixture.engine.entry.phase == WT_JOURNAL_APPLIED || fixture.engine.entry.phase == WT_JOURNAL_IDLE));
    }
}

static void ReadbackAndInventoryFaults(void)
{
    Fixture fixture;
    unsigned failure;
    unsigned char packet[902], incoming[136]; size_t size;
    WtTest_Pokemon(incoming, 2);
    for (failure = 1; failure <= 4; ++failure) {
        Init(&fixture, 1, 25); Enroll(&fixture);
        fixture.failRead = fixture.reads + failure;
        assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && fixture.engine.blocked && !fixture.removed);
        fixture.failRead = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
        assert(fixture.engine.entry.phase == WT_JOURNAL_IDLE || fixture.engine.entry.phase == WT_JOURNAL_ESCROW);
    }
    for (failure = WT_INVENTORY_REMOVE; failure <= WT_INVENTORY_VERIFY_RECEIVED; ++failure) {
        Init(&fixture, 1, 25); Enroll(&fixture);
        if (failure <= WT_INVENTORY_VERIFY_REMOVED) {
            fixture.failStep = failure + 1;
            assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && fixture.engine.blocked);
        } else {
            Submit(&fixture); assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
            size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
            Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming); memcpy(packet + 182, incoming, 136);
            fixture.failStep = failure + 1;
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked);
        }
        assert(!fixture.engine.frameSize);
        fixture.failStep = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
        assert(WtEngine_Request(&fixture.engine, 0) == 0);
    }
    Init(&fixture, 1, 25); Enroll(&fixture);
    assert(WtEngine_Deposit(&fixture.engine, 0) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
    size = Header(&fixture, packet, WT_RESULT_ILLEGAL_POKEMON, 2, 2, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 2, 2, 116);
    packet[44] = 1; Put(packet + 46, 2, 8); Put(packet + 54, 2, 8); packet[62] = WT_EVENT_REJECTED;
    memcpy(packet + 79, fixture.engine.entry.operation, 16); assert(Hash(fixture.pokemon, 136, packet + 111) == 0);
    fixture.failStep = WT_INVENTORY_RESTORE + 1;
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && !fixture.restored && fixture.removed);
    Mirrored(&fixture, WT_JOURNAL_RESTORING);
    fixture.failStep = 0; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.restored && !fixture.removed);
}

static void SupersededSubmit(void)
{
    Fixture fixture;
    unsigned char packet[902], before[2][4096], *event = packet + 54, foreign[106];
    size_t size;
    unsigned variant;
    for (variant = 0; variant < 6; ++variant) {
        Init(&fixture, 1, 25); Enroll(&fixture);
        assert(WtEngine_Deposit(&fixture.engine, 0) == 0 && WtEngine_Request(&fixture.engine, 0) == 0);
        memcpy(before, fixture.banks, sizeof(before));
        /* Another copy used counter 1: the server is ahead of this retained Submit. */
        size = Header(&fixture, packet, WT_RESULT_COUNTER_MISMATCH, variant == 3 ? 1 : 2, variant == 5 ? 1 : 2, 0);
        if (variant == 3 || variant == 5) {  /* Server not ahead, or no newer durable event. */
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.restored);
            continue;
        }
        assert(WtEngine_Reply(&fixture.engine, packet, size) == 1 && !fixture.engine.blocked && fixture.removed);
        assert(memcmp(before, fixture.banks, sizeof(before)) == 0);
        assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0 && fixture.engine.frame[5] == WT_OP_RECONCILE);
        size = Header(&fixture, packet, WT_RESULT_OK, 2, 2, 116);
        packet[44] = 1; Put(packet + 46, 2, 8); Put(event, 2, 8);
        memset(event + 9, 0x11, 16); memset(event + 41, 0x22, 16); memset(event + 57, 0x33, 32);
        if (variant == 2) { event[8] = WT_EVENT_MATCHED; event[105] = WT_TRADE_MATCHED; memset(event + 89, 0x44, 16); }
        else if (variant == 4) {  /* Another copy's rejected Submit also used the counter. */
            memset(event + 9, 0, 16); memset(event + 41, 0, 16); memset(event + 25, 0x55, 16);
            event[8] = WT_EVENT_REJECTED; event[105] = WT_TRADE_NONE;
        } else {
            event[8] = WT_EVENT_SUBMITTED; event[105] = WT_TRADE_QUEUED;
            if (variant == 1) memcpy(event + 25, fixture.engine.entry.operation, 16);
            else memset(event + 25, 0x55, 16);
        }
        if (variant && variant != 4) {
            assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.restored);
            assert(memcmp(before, fixture.banks, sizeof(before)) == 0);
            continue;
        }
        memcpy(foreign, event, sizeof(foreign));
        assert(WtEngine_Reply(&fixture.engine, packet, size) == 0 && fixture.restored && !fixture.removed);
        Mirrored(&fixture, WT_JOURNAL_IDLE);
        /* The restored copy keeps its own counter and cursor, so it stays fenced. */
        assert(fixture.engine.entry.nextCounter == 2 && fixture.engine.entry.lastOperationCounter == 1
               && fixture.engine.entry.eventCursor == 1 && !fixture.engine.synchronized);
        assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && !fixture.removed);
        assert(WtEngine_Request(&fixture.engine, WT_OP_RECONCILE) == 0);
        size = Header(&fixture, packet, WT_RESULT_OK, 2, 2, 116);
        packet[44] = 1; Put(packet + 46, 2, 8); memcpy(event, foreign, sizeof(foreign));
        {   /* The same foreign event decodes but keeps this restored copy fenced. */
            WtWireReply wire;
            assert(WtWire_Decode(fixture.engine.frame, fixture.engine.frameSize, packet, size, Hash, &wire) == 0);
        }
        assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.removed);
        assert(fixture.engine.rolledBack && fixture.engine.entry.phase == WT_JOURNAL_IDLE);
    }
}

/* One reconcile page event for a trade this save's journal never made. */
static size_t ForeignPage(Fixture *fixture, unsigned char packet[902], uint64_t counter, uint64_t sequence)
{
    unsigned char *event = packet + 54;
    size_t size = Header(fixture, packet, WT_RESULT_OK, counter, sequence, 116);
    packet[44] = 1; Put(packet + 46, sequence, 8); Put(event, sequence, 8);
    event[8] = WT_EVENT_SUBMITTED; event[105] = WT_TRADE_QUEUED;
    memset(event + 9, 0x77, 16); memset(event + 25, 0x55, 16); memset(event + 41, 0x22, 16); memset(event + 57, 0x33, 32);
    return size;
}

/* An older copy of a save, restored after a newer copy traded: the server's
 * authenticated answer shows history this journal never made. It blocks, says
 * why, and writes nothing, so the copy may leave but never trade again. */
static void RolledBackCopy(void)
{
    Fixture fixture;
    unsigned char packet[902], old[2][4096];
    size_t size;
    /* An idle copy saved before the newer copy's offer used counter 1. */
    Init(&fixture, 1, 25); Enroll(&fixture);
    memcpy(old, fixture.banks, sizeof(old));
    Submit(&fixture);
    memcpy(fixture.banks, old, sizeof(old)); fixture.removed = 0;
    WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.engine.frame[5] == WT_OP_RECONCILE);
    size = ForeignPage(&fixture, packet, 2, 2);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && fixture.engine.rolledBack);
    assert(memcmp(old, fixture.banks, sizeof(old)) == 0 && !fixture.removed);
    assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && WtEngine_Request(&fixture.engine, 0) != 0);
    /* Every later visit finds the same history: the copy stays fenced. */
    WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(!fixture.engine.rolledBack && WtEngine_Request(&fixture.engine, 0) == 0);
    size = ForeignPage(&fixture, packet, 2, 2);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.rolledBack);

    /* A queued copy whose Cancel finds the server past every counter it could use. */
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    memcpy(old, fixture.banks, sizeof(old));
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_COUNTER_MISMATCH, 4, 4, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && fixture.engine.rolledBack);
    Mirrored(&fixture, WT_JOURNAL_QUEUED);
    /* At its own Cancel's counter the server is not provably ahead: an ordinary block. */
    memcpy(fixture.banks, old, sizeof(old)); WtEngine_Close(&fixture.engine); Open(&fixture);
    fixture.engine.synchronized = 1;
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_COUNTER_MISMATCH, 3, 3, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.engine.rolledBack);

    /* A failed local write is never read as a rolled-back copy. */
    Init(&fixture, 1, 25); Enroll(&fixture);
    WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 1, 1, 10); Put(packet + 46, 1, 8);
    fixture.failWrite = fixture.writes + 1;
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.engine.rolledBack);
    /* Nor is a server behind this save. */
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture);
    assert(WtEngine_Request(&fixture.engine, WT_OP_CANCEL) == 0);
    size = Header(&fixture, packet, WT_RESULT_COUNTER_MISMATCH, 1, 1, 0);
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && fixture.engine.blocked && !fixture.engine.rolledBack);
}

/* Rejected Submit for a deposited offer: an ILLEGAL reply, then the
 * reconcile page whose REJECTED event proves it may be restored. */
static int Reject(Fixture *fixture)
{
    unsigned char packet[902]; size_t size;
    assert(WtEngine_Deposit(&fixture->engine, 0) == 0 && WtEngine_Request(&fixture->engine, 0) == 0);
    size = Header(fixture, packet, WT_RESULT_ILLEGAL_POKEMON, 2, 2, 0);
    assert(WtEngine_Reply(&fixture->engine, packet, size) == 1);
    assert(WtEngine_Request(&fixture->engine, WT_OP_RECONCILE) == 0);
    size = Header(fixture, packet, WT_RESULT_OK, 2, 2, 116);
    packet[44] = 1; Put(packet + 46, 2, 8); Put(packet + 54, 2, 8); packet[62] = WT_EVENT_REJECTED;
    memcpy(packet + 54 + 25, fixture->engine.entry.operation, 16);
    assert(Hash(fixture->pokemon, 136, packet + 54 + 57) == 0);
    return WtEngine_Reply(&fixture->engine, packet, size);
}

static void LocalSettlement(void)
{
    Fixture fixture;
    unsigned char packet[902], incoming[136], pending[256]; size_t size;
    uint64_t counter;
    /* Boot finishes a removal interrupted before SUBMIT_READY; the retained
     * Submit is sent unchanged on the next visit. */
    Init(&fixture, 1, 25); Enroll(&fixture); counter = fixture.engine.entry.nextCounter;
    fixture.failStep = WT_INVENTORY_VERIFY_REMOVED + 1;
    assert(WtEngine_Deposit(&fixture.engine, 0) != 0 && fixture.removed);
    Mirrored(&fixture, WT_JOURNAL_ESCROW); memcpy(pending, fixture.engine.entry.request, 256);
    fixture.failStep = 0; fixture.random = UINT32_MAX; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Settle(&fixture.engine) == 0 && fixture.removed && !fixture.restored);
    Mirrored(&fixture, WT_JOURNAL_SUBMIT_READY);
    assert(fixture.engine.entry.nextCounter == counter && memcmp(fixture.engine.entry.request, pending, 256) == 0);
    fixture.random = 100; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && memcmp(fixture.engine.frame, pending, 256) == 0);
    /* Nothing local is left once the offer is queued. */
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture); fixture.random = UINT32_MAX;
    WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Settle(&fixture.engine) == 0 && fixture.removed && !fixture.restored);
    Mirrored(&fixture, WT_JOURNAL_QUEUED);

    /* A rejected offer returns to the first free slot once the save has changed since. */
    Init(&fixture, 1, 25); Enroll(&fixture); fixture.changed = 1;
    assert(Reject(&fixture) == 0 && fixture.received && !fixture.restored);
    assert(memcmp(fixture.delivered, fixture.pokemon, 136) == 0);
    Mirrored(&fixture, WT_JOURNAL_IDLE);
    /* With no room it stays in the journal, and the next boot returns it. */
    Init(&fixture, 1, 25); Enroll(&fixture); fixture.changed = 1; fixture.full = 1;
    assert(Reject(&fixture) == 1 && !fixture.received);
    Mirrored(&fixture, WT_JOURNAL_RESTORING);
    fixture.full = 0; fixture.random = UINT32_MAX; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Settle(&fixture.engine) == 0 && fixture.received);
    assert(memcmp(fixture.delivered, fixture.pokemon, 136) == 0);
    Mirrored(&fixture, WT_JOURNAL_IDLE);

    /* Boot finishes a delivery interrupted while applying; its ACK waits for the next visit. */
    Init(&fixture, 1, 25); Enroll(&fixture); Submit(&fixture); WtTest_Pokemon(incoming, 2);
    assert(WtEngine_Request(&fixture.engine, WT_OP_COLLECT) == 0);
    size = Header(&fixture, packet, WT_RESULT_OK, 2, 3, 274);
    Snapshot(&fixture, packet + 44, WT_TRADE_MATCHED, WT_DELIVERY_MATCH, 3, incoming);
    memcpy(packet + 182, incoming, 136);
    fixture.failStep = WT_INVENTORY_RECEIVE + 1;
    assert(WtEngine_Reply(&fixture.engine, packet, size) != 0 && !fixture.received);
    Mirrored(&fixture, WT_JOURNAL_APPLYING);
    fixture.failStep = 0; fixture.random = UINT32_MAX; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Settle(&fixture.engine) == 0 && fixture.received);
    Mirrored(&fixture, WT_JOURNAL_APPLIED); assert(fixture.engine.entry.requestLength == 0);
    fixture.random = 100; WtEngine_Close(&fixture.engine); Open(&fixture);
    assert(WtEngine_Request(&fixture.engine, 0) == 0 && fixture.engine.frame[5] == WT_OP_ACKNOWLEDGE);
    Mirrored(&fixture, WT_JOURNAL_APPLIED); assert(fixture.engine.entry.request[5] == WT_OP_ACKNOWLEDGE);
}

/* A retail-inventory fixture for engine recovery: real encrypted images and
 * the production scan/decision/record helpers, with checkpoint writes counted.
 * Slots 0/1 are party, 2/3 PC; unoccupied slots are valid encrypted empties. */
static int RecoveryPrepare(Fixture *fixture, WtJournalEntry *entry)
{
    WtReceiveScan scan;
    unsigned i;
    int result;
    memset(&scan, 0, sizeof(scan));
    for (i = 0; i < 4; ++i) {
        if (WtReceive_Inspect(&scan, entry->outgoing, fixture->inventory[i],
                i < 2 ? 1 : 2, i < 2 ? 0 : 17, i % 2, i < 2 ? 236 : 136) != 0) return -1;
    }
    result = WtReceive_Check(entry, &scan, fixture->saveCounters[0], fixture->saveCounters[1], fixture->saveCounters[2]);
    if (result != 1) return result;
    if (WtReceive_Record(entry, &scan, fixture->empty, Hash) != 0
        || Hash((unsigned char *)fixture->inventory, sizeof(fixture->inventory), entry->inventoryAfter) != 0) return -1;
    memcpy(entry->inventoryBefore, entry->inventoryAfter, 32);
    return 1;
}

static int RecoveryInventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    Fixture *fixture = arg;
    unsigned char digest[32], candidate[4][236];
    unsigned index;
    int prepared;
    (void)slot;
    if (step == WT_INVENTORY_PREPARE_RESTORE) return 1; /* Stock game changed the save. */
    if (step == WT_INVENTORY_PREPARE_RECEIVE) {
        prepared = RecoveryPrepare(fixture, entry);
        if (prepared != 0) return prepared == 1 ? 0 : -1;
        /* Normal insertion into the first free party slot. */
        entry->destination.kind = 1; entry->destination.box = 0;
        entry->destination.slot = 0; entry->destination.length = 236;
        memcpy(entry->destinationBefore, fixture->inventory[0], 236);
        memcpy(entry->destinationAfter, fixture->empty, 236);
        CHECK(WtPokemon_Encode(entry->incoming, entry->destinationAfter) == 0);
        CHECK(Hash(entry->incoming, 136, entry->appliedDigest) == 0);
        CHECK(Hash((unsigned char *)fixture->inventory, sizeof(fixture->inventory), entry->inventoryBefore) == 0);
        memcpy(candidate, fixture->inventory, sizeof(candidate));
        memcpy(candidate[0], entry->destinationAfter, 236);
        CHECK(Hash((unsigned char *)candidate, sizeof(candidate), entry->inventoryAfter) == 0);
        return 0;
    }
    index = entry->destination.kind == 1 ? entry->destination.slot : 2 + entry->destination.slot;
    if (step == WT_INVENTORY_RECEIVE) {
        CHECK(Hash((unsigned char *)fixture->inventory, sizeof(fixture->inventory), digest) == 0);
        if (memcmp(digest, entry->inventoryBefore, 32) != 0 && memcmp(digest, entry->inventoryAfter, 32) != 0) {
            if (RecoveryPrepare(fixture, entry) != 1) return -1;
            index = entry->destination.kind == 1 ? entry->destination.slot : 2 + entry->destination.slot;
        }
        if (memcmp(fixture->inventory[index], entry->destinationAfter, entry->destination.length) != 0) {
            memcpy(fixture->inventory[index], entry->destinationAfter, entry->destination.length);
            ++fixture->insertions;
        }
        ++fixture->saveWrites;
        entry->globalCounter = ++fixture->saveCounters[0];
        entry->generalCounter = ++fixture->saveCounters[1];
        entry->boxCounter = ++fixture->saveCounters[2];
        return 0;
    }
    if (step == WT_INVENTORY_VERIFY_RECEIVED) {
        return memcmp(fixture->inventory[index], entry->destinationAfter, entry->destination.length) == 0 ? 0 : -1;
    }
    return -1;
}

static int RecoveryRandom(void *arg, unsigned char *bytes, size_t size)
{
    Fixture *fixture = arg;
    unsigned char input[8] = { 0 }, digest[32];
    Put(input, ++fixture->random, 4);
    CHECK(size <= sizeof(digest) && Hash(input, sizeof(input), digest) == 0);
    memcpy(bytes, digest, size);
    return 0;
}

static void RecoveryOpen(Fixture *fixture)
{
    WtJournalIO journal = { fixture, Read, Write, Hash };
    WtEngineIO io = { fixture, RecoveryRandom, RecoveryInventory, CanHold };
    CHECK(WtEngine_Open(&fixture->engine, journal, io) == 0);
}

static void RecoveryInit(Fixture *fixture, unsigned phase, unsigned kind, unsigned where)
{
    WtJournalEntry entry;
    unsigned char canonical[136] = { 0 };
    unsigned i;
    memset(fixture, 0, sizeof(*fixture)); fixture->id = 1;
    CHECK(WtTest_JournalEntry(phase, Hash, &entry) == 0);
    CHECK(WtPokemon_Encode(canonical, fixture->empty) == 0);
    for (i = 0; i < 4; ++i) memcpy(fixture->inventory[i], fixture->empty, 236);
    memcpy(fixture->pokemon, entry.outgoing, 136);
    CHECK(WtPokemon_Encode(entry.outgoing, entry.original) == 0);
    CHECK(Hash(entry.original, entry.source.length, entry.originalDigest) == 0);
    memcpy(fixture->inventory[where], entry.original, where < 2 ? 236 : 136);
    entry.globalCounter = entry.generalCounter = entry.boxCounter = 10;
    for (i = 0; i < 3; ++i) fixture->saveCounters[i] = 11;
    entry.deliveryKind = (unsigned char)kind;
    if (kind == WT_DELIVERY_RETURN) memcpy(entry.incoming, entry.outgoing, 136);
    if (kind != WT_DELIVERY_NONE) CHECK(Hash(entry.incoming, 136, entry.incomingDigest) == 0);
    entry.requestLength = 0; memset(entry.request, 0, sizeof(entry.request));
    CHECK(WtJournal_Encode(&entry, Hash, fixture->banks[0]) == 0);
    memcpy(fixture->banks[1], fixture->banks[0], 4096);
    RecoveryOpen(fixture);
    fixture->writes = 0; /* Opening repairs the mirrored journal before the action under test. */
}

static unsigned RecoveryCopies(Fixture *fixture)
{
    unsigned char canonical[136];
    unsigned i, copies = 0;
    for (i = 0; i < 4; ++i) {
        CHECK(WtPokemon_Decode(fixture->inventory[i], canonical) == 0);
        if (memcmp(canonical, fixture->pokemon, 136) == 0) ++copies;
    }
    return copies;
}

static void RecoveryAck(Fixture *fixture)
{
    unsigned char packet[902], *snapshot, digest[32];
    WtJournalEntry applied = fixture->engine.entry;
    size_t size;
    CHECK(WtEngine_Request(&fixture->engine, 0) == 0);
    CHECK(fixture->engine.frame[5] == WT_OP_ACKNOWLEDGE);
    CHECK(memcmp(fixture->engine.frame + WT_WIRE_REQUEST_HEADER_SIZE + WT_WIRE_ACK_APPLIED_DIGEST,
        applied.appliedDigest, 32) == 0);
    size = Header(fixture, packet, WT_RESULT_OK, 3, 4, 146);
    Put(packet + 44, 4, 8); snapshot = packet + 52;
    memcpy(snapshot, applied.trade, 16); snapshot[16] = WT_TRADE_ACKNOWLEDGED; snapshot[17] = WT_DELIVERY_RETURN;
    memset(snapshot + 18, 21, 16); CHECK(Hash(applied.outgoing, 136, digest) == 0);
    memcpy(snapshot + 34, digest, 32); memcpy(snapshot + 66, snapshot + 18, 48);
    memcpy(snapshot + 114, applied.receipt, 16); Put(snapshot + 130, 4, 8);
    CHECK(WtEngine_Reply(&fixture->engine, packet, size) == 0);
    CHECK(fixture->engine.entry.phase == WT_JOURNAL_IDLE);
}

static void ReturnAlreadyApplied(void)
{
    Fixture fixture;
    unsigned char before[sizeof(fixture.inventory)], digest[32], encoded[4096];
    unsigned where, phase, saves;
    for (where = 0; where < 4; ++where) for (phase = 0; phase < 3; ++phase) {
        RecoveryInit(&fixture, phase == 0 ? WT_JOURNAL_RESTORING : phase == 1 ? WT_JOURNAL_RECEIVED : WT_JOURNAL_APPLYING,
            phase == 0 ? WT_DELIVERY_NONE : WT_DELIVERY_RETURN, where);
        memcpy(before, fixture.inventory, sizeof(before));
        CHECK(WtEngine_Settle(&fixture.engine) == 0);
        CHECK(!fixture.engine.blocked && fixture.saveWrites == 1 && fixture.insertions == 0);
        CHECK(memcmp(before, fixture.inventory, sizeof(before)) == 0 && RecoveryCopies(&fixture) == 1);
        CHECK(fixture.engine.entry.phase == (phase == 0 ? WT_JOURNAL_IDLE : WT_JOURNAL_APPLIED));
        if (phase != 0) {
            CHECK(fixture.engine.entry.destination.kind == (where < 2 ? 1 : 2));
            CHECK(fixture.engine.entry.destination.length == (where < 2 ? 236 : 136));
            CHECK(fixture.engine.entry.destination.slot == where % 2);
            CHECK(memcmp(fixture.engine.entry.destinationBefore, fixture.empty, where < 2 ? 236 : 136) == 0);
            CHECK(memcmp(fixture.engine.entry.destinationAfter, before + where * 236, where < 2 ? 236 : 136) == 0);
            CHECK(Hash(fixture.pokemon, 136, digest) == 0 && memcmp(digest, fixture.engine.entry.appliedDigest, 32) == 0);
            CHECK(Hash(before, sizeof(before), digest) == 0 && memcmp(digest, fixture.engine.entry.inventoryAfter, 32) == 0);
            CHECK(WtJournal_Encode(&fixture.engine.entry, Hash, encoded) == 0);
            saves = fixture.saveWrites;
            WtEngine_Close(&fixture.engine); RecoveryOpen(&fixture);
            CHECK(WtEngine_Settle(&fixture.engine) == 0 && fixture.saveWrites == saves);
            RecoveryAck(&fixture);
            CHECK(RecoveryCopies(&fixture) == 1 && fixture.insertions == 0 && fixture.saveWrites == saves);
        }
        WtEngine_Close(&fixture.engine);
    }
    puts("RETURN present once after stock save: Restore/Resume checkpoint in place, ACK to IDLE, exactly one copy");
}

static void AbsentOfferDelivers(void)
{
    Fixture fixture;
    unsigned kind;
    for (kind = WT_DELIVERY_MATCH; kind <= WT_DELIVERY_RETURN; ++kind) {
        RecoveryInit(&fixture, WT_JOURNAL_RECEIVED, kind, 1);
        memcpy(fixture.inventory[1], fixture.empty, 236);
        CHECK(WtEngine_Settle(&fixture.engine) == 0 && !fixture.engine.blocked);
        CHECK(fixture.engine.entry.phase == WT_JOURNAL_APPLIED && fixture.saveWrites == 1 && fixture.insertions == 1);
        CHECK(RecoveryCopies(&fixture) == (kind == WT_DELIVERY_RETURN ? 1u : 0u));
        WtEngine_Close(&fixture.engine);
    }
}

static void UnsafeDeliveryBlocks(void)
{
    Fixture fixture;
    unsigned char before[sizeof(fixture.inventory)], banks[2][4096], canonical[136];
    unsigned scenario, phase;
    for (phase = 0; phase < 2; ++phase) for (scenario = 0; scenario < 8; ++scenario) {
        RecoveryInit(&fixture, phase == 0 && scenario != 7 ? WT_JOURNAL_RESTORING : WT_JOURNAL_RECEIVED,
            scenario == 7 ? WT_DELIVERY_MATCH : phase == 0 ? WT_DELIVERY_NONE : WT_DELIVERY_RETURN, 1);
        switch (scenario) {
        case 0: fixture.saveCounters[0] = 9; break; /* Behind even if the other blocks advanced. */
        case 1: fixture.saveCounters[1] = 9; break;
        case 2: fixture.saveCounters[2] = 9; break;
        case 3: fixture.saveCounters[0] = fixture.saveCounters[1] = fixture.saveCounters[2] = 10; break;
        case 4: memcpy(fixture.inventory[2], fixture.inventory[1], 136); break; /* Two exact copies. */
        case 5: /* Same PID and OT, changed canonical image. */
            memcpy(canonical, fixture.pokemon, 136); canonical[10] = 1; ++canonical[6];
            CHECK(WtPokemon_Encode(canonical, fixture.inventory[2]) == 0); break;
        case 6: fixture.inventory[3][7] ^= 1; break; /* An unreadable slot after the match. */
        case 7: break; /* MATCH with the outgoing offer still present. */
        }
        memcpy(before, fixture.inventory, sizeof(before)); memcpy(banks, fixture.banks, sizeof(banks));
        CHECK(WtEngine_Settle(&fixture.engine) == -1 && fixture.engine.blocked);
        if (fixture.saveWrites || fixture.insertions || fixture.writes) fprintf(stderr, "phase=%u scenario=%u saves=%u inserts=%u journals=%u\n", phase, scenario, fixture.saveWrites, fixture.insertions, fixture.writes);
        CHECK(fixture.saveWrites == 0 && fixture.insertions == 0 && fixture.writes == 0);
        CHECK(memcmp(before, fixture.inventory, sizeof(before)) == 0 && memcmp(banks, fixture.banks, sizeof(banks)) == 0);
        WtEngine_Close(&fixture.engine);
    }
    puts("Unsafe RETURN/MATCH: behind/equal counters, duplicate/changed identity and unreadable slots block without writes");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--unsafe-delivery") == 0) { UnsafeDeliveryBlocks(); return 0; }
    if (argc == 2 && strcmp(argv[1], "--receive-recovery") == 0) {
        ReturnAlreadyApplied(); UnsafeDeliveryBlocks(); AbsentOfferDelivers(); return 0;
    }
    ReturnAlreadyApplied(); UnsafeDeliveryBlocks(); AbsentOfferDelivers();
    PairAndReturn(); RefusedDelivery(); RejectionAndErrors(); SupersededSubmit(); FailedPersistence(); DeliveryPersistence();
    RejectedCancel(); MatchedCancelRace(); ServerReturn(); LoadedStateAndMismatch(); EnrollmentAndRetirementFaults(); ReadbackAndInventoryFaults();
    DeliveryKindBinding(); LocalSettlement(); RolledBackCopy();
    puts("Recoverable engine: paired delivery, match-cancel race, return, server return, refused delivery, rejection, retries, full-storage restart, stale context, failed persistence, boot settlement and rolled-back copies passed");
    return 0;
}
