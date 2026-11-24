#include "wonder_trade/session.h"
#include "journal_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <psa/crypto.h>

typedef struct Fixture {
    unsigned char banks[2][WT_JOURNAL_RECORD_SIZE];
    WtEngine engine;
    unsigned random, failStep;
    int full;
} Fixture;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

static int Read(void *arg, int bank, unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    Fixture *fixture = arg;
    memcpy(record, fixture->banks[bank], WT_JOURNAL_RECORD_SIZE); return 0;
}

static int Write(void *arg, int bank, const unsigned char record[WT_JOURNAL_RECORD_SIZE])
{
    Fixture *fixture = arg;
    memcpy(fixture->banks[bank], record, WT_JOURNAL_RECORD_SIZE); return 0;
}

static int Random(void *arg, unsigned char *bytes, size_t size)
{
    Fixture *fixture = arg;
    memset(bytes, ++fixture->random, size); return 0;
}

static int Inventory(void *arg, enum WtInventoryStep step, WtJournalEntry *entry, unsigned slot)
{
    Fixture *fixture = arg;
    (void)slot;
    if ((unsigned)step + 1 == fixture->failStep) return -1;
    if (step == WT_INVENTORY_PREPARE_RECEIVE) {
        if (fixture->full) return 1;
        entry->destination.kind = 1; entry->destination.slot = 1; entry->destination.length = 236;
        memset(entry->destinationBefore, 13, 236); memset(entry->destinationAfter, 14, 236);
        return Hash(entry->incoming, 136, entry->appliedDigest);
    }
    return 0;
}

static int CanHold(void *arg, const unsigned char pokemon[136])
{
    (void)arg; (void)pokemon;
    return 1;
}

static void Init(Fixture *fixture, unsigned phase, int synchronized)
{
    WtJournalEntry entry;
    WtJournalIO journal = { fixture, Read, Write, Hash };
    WtEngineIO io = { fixture, Random, Inventory, CanHold };
    int result;
    memset(fixture, 0, sizeof(*fixture));
    assert(WtTest_JournalEntry(phase, Hash, &entry) == 0);
    if (phase == WT_JOURNAL_QUEUED) {
        memset(entry.request, 0, sizeof(entry.request)); entry.requestLength = 0;
    }
    assert(WtJournal_Encode(&entry, Hash, fixture->banks[0]) == 0);
    memcpy(fixture->banks[1], fixture->banks[0], WT_JOURNAL_RECORD_SIZE);
    result = WtEngine_Open(&fixture->engine, journal, io);
    assert(phase == WT_JOURNAL_RECOVERY_REQUIRED ? result < 0 : result == 0);
    fixture->engine.synchronized = synchronized;
}

static void Expect(Fixture *fixture, unsigned requested, int expected)
{
    int operation = WtSession_Next(&fixture->engine, requested);
    assert(operation == expected);
    if (operation > 0) {
        assert(fixture->engine.frameSize > 5);
        assert(fixture->engine.frame[5] == operation);
        assert(WtWire_CheckRequest(fixture->engine.frame, fixture->engine.frameSize, Hash) == 0);
    } else assert(fixture->engine.frameSize == 0);
}

static void EveryPhaseAndAction(void)
{
    const unsigned actions[] = { 0, WT_OP_COLLECT, WT_OP_CANCEL };
    /* Rows follow WtJournalPhase; columns follow actions, before/after sync. */
    const int expected[][2][3] = {
        {{-1, -1, -1}, {-1, -1, -1}},
        {{WT_OP_ENROLL, WT_OP_ENROLL, WT_OP_ENROLL}, {WT_OP_ENROLL, WT_OP_ENROLL, WT_OP_ENROLL}},
        {{WT_OP_RECONCILE, WT_OP_RECONCILE, WT_OP_RECONCILE}, {0, 0, 0}},
        {{WT_OP_SUBMIT, WT_OP_SUBMIT, WT_OP_SUBMIT}, {WT_OP_SUBMIT, WT_OP_SUBMIT, WT_OP_SUBMIT}},
        {{WT_OP_SUBMIT, WT_OP_SUBMIT, WT_OP_SUBMIT}, {WT_OP_SUBMIT, WT_OP_SUBMIT, WT_OP_SUBMIT}},
        {{WT_OP_RECONCILE, WT_OP_RECONCILE, WT_OP_RECONCILE}, {WT_OP_STATUS, WT_OP_COLLECT, WT_OP_CANCEL}},
        {{WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}, {WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}},
        {{WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}, {WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}},
        {{WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}, {WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE, WT_OP_ACKNOWLEDGE}},
        {{WT_OP_RECONCILE, WT_OP_RECONCILE, WT_OP_RECONCILE}, {WT_OP_RECONCILE, -1, -1}},
        {{-1, -1, -1}, {-1, -1, -1}}
    };
    Fixture fixture;
    unsigned phase, synchronized, action;
    assert(sizeof(expected) / sizeof(expected[0]) == WT_JOURNAL_RECOVERY_REQUIRED + 1);
    for (phase = WT_JOURNAL_EMPTY; phase <= WT_JOURNAL_RECOVERY_REQUIRED; ++phase) {
        for (synchronized = 0; synchronized < 2; ++synchronized) {
            for (action = 0; action < sizeof(actions) / sizeof(actions[0]); ++action) {
                Init(&fixture, phase, (int)synchronized);
                Expect(&fixture, actions[action], expected[phase][synchronized][action]);
                WtEngine_Close(&fixture.engine);
            }
        }
    }
}

static void KnownDeliveriesAreCollected(void)
{
    const unsigned actions[] = { 0, WT_OP_COLLECT, WT_OP_CANCEL };
    Fixture fixture;
    unsigned action;
    for (action = 0; action < sizeof(actions) / sizeof(actions[0]); ++action) {
        Init(&fixture, WT_JOURNAL_QUEUED, 1);
        fixture.engine.entry.deliveryKind = WT_DELIVERY_RETURN;
        ++fixture.engine.entry.generation;
        assert(WtJournal_Commit(&fixture.engine.journal, &fixture.engine.entry) == 0);
        Expect(&fixture, actions[action], WT_OP_COLLECT);
        WtEngine_Close(&fixture.engine);
    }
    Init(&fixture, WT_JOURNAL_QUEUED, 1);
    fixture.engine.entry.deliveryKind = WT_DELIVERY_MATCH;
    ++fixture.engine.entry.generation;
    assert(WtJournal_Commit(&fixture.engine.journal, &fixture.engine.entry) == 0);
    Expect(&fixture, WT_OP_CANCEL, WT_OP_COLLECT);
    WtEngine_Close(&fixture.engine);
}

static void PendingMutationsAreReplayed(void)
{
    Fixture fixture;
    unsigned char pending[WT_JOURNAL_FRAME_MAX];
    unsigned size;
    Init(&fixture, WT_JOURNAL_QUEUED, 1);
    Expect(&fixture, WT_OP_CANCEL, WT_OP_CANCEL);
    size = fixture.engine.entry.requestLength;
    memcpy(pending, fixture.engine.entry.request, size);
    fixture.engine.frameSize = 0;
    fixture.engine.synchronized = 0;
    Expect(&fixture, WT_OP_COLLECT, WT_OP_CANCEL);
    assert(fixture.engine.frameSize == size && memcmp(pending, fixture.engine.frame, size) == 0);
    WtEngine_Close(&fixture.engine);
}

static void RejectionsReconcileBeforeRetry(void)
{
    const unsigned actions[] = { 0, WT_OP_COLLECT, WT_OP_CANCEL };
    const int submitResults[] = { WT_RESULT_ILLEGAL_POKEMON, WT_RESULT_COUNTER_MISMATCH };
    const int results[] = { WT_RESULT_CONFLICT, WT_RESULT_NOT_FOUND };
    Fixture fixture;
    unsigned action, result, synchronized;
    for (action = 0; action < sizeof(actions) / sizeof(actions[0]); ++action) {
        for (synchronized = 0; synchronized < 2; ++synchronized) {
            for (result = 0; result < sizeof(submitResults) / sizeof(submitResults[0]); ++result) {
                Init(&fixture, WT_JOURNAL_SUBMIT_READY, (int)synchronized);
                fixture.engine.lastResult = submitResults[result];
                Expect(&fixture, actions[action], WT_OP_RECONCILE);
                WtEngine_Close(&fixture.engine);
            }
        }
        for (result = 0; result < sizeof(results) / sizeof(results[0]); ++result) {
            Init(&fixture, WT_JOURNAL_QUEUED, 1);
            Expect(&fixture, WT_OP_CANCEL, WT_OP_CANCEL);
            fixture.engine.frameSize = 0;
            fixture.engine.synchronized = 0;
            fixture.engine.lastResult = results[result];
            Expect(&fixture, actions[action], WT_OP_RECONCILE);
            WtEngine_Close(&fixture.engine);
        }
        Init(&fixture, WT_JOURNAL_QUEUED, 1);
        fixture.engine.lastResult = WT_RESULT_COUNTER_MISMATCH;
        Expect(&fixture, actions[action], actions[action] ? (int)actions[action] : WT_OP_STATUS);
        WtEngine_Close(&fixture.engine);
    }
}

static void FullStorageEndsVisitAndFailuresBlock(void)
{
    Fixture fixture;
    Init(&fixture, WT_JOURNAL_RECEIVED, 1);
    fixture.full = 1;
    Expect(&fixture, 0, WT_SESSION_DONE);
    assert(fixture.engine.entry.phase == WT_JOURNAL_RECEIVED && !fixture.engine.blocked);
    WtEngine_Close(&fixture.engine);
    Init(&fixture, WT_JOURNAL_RECEIVED, 1);
    fixture.failStep = WT_INVENTORY_PREPARE_RECEIVE + 1;
    Expect(&fixture, 0, WT_SESSION_BLOCKED);
    assert(fixture.engine.blocked);
    WtEngine_Close(&fixture.engine);
    Init(&fixture, WT_JOURNAL_IDLE, 1);
    fixture.engine.blocked = 1;
    Expect(&fixture, 0, WT_SESSION_BLOCKED);
    WtEngine_Close(&fixture.engine);
    assert(WtSession_Next(NULL, 0) == WT_SESSION_BLOCKED);
}

int main(void)
{
    EveryPhaseAndAction();
    KnownDeliveriesAreCollected();
    PendingMutationsAreReplayed();
    RejectionsReconcileBeforeRetry();
    FullStorageEndsVisitAndFailuresBlock();
    puts("session tests passed (66 phase/action/synchronization combinations and recovery cases)");
    return 0;
}
