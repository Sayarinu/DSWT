#include "wonder_trade/seed.h"
#include "journal_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <psa/crypto.h>

static unsigned char banks[2][4096], baseline[4096];
static int writes, failAt, cutAt, lie, readFailure, reads;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

static int Read(void *arg, int bank, unsigned char record[4096])
{
    (void)arg;
    if (++reads == readFailure) return -1;
    memcpy(record, banks[bank], 4096);
    return 0;
}

static int Write(void *arg, int bank, const unsigned char record[4096])
{
    int step, count = ++writes == failAt ? cutAt : 4100;
    (void)arg;
    for (step = 0; step < count; step++) {
        if (step < 4) banks[bank][4092 + step] = 0;
        else banks[bank][step - 4] = record[step - 4];
    }
    return writes == failAt && !lie ? -1 : 0;
}

static void Reset(const WtJournalEntry *entry)
{
    assert(WtJournal_Encode(entry, Hash, baseline) == 0);
    memcpy(banks[0], baseline, 4096); memcpy(banks[1], baseline, 4096);
    writes = failAt = cutAt = lie = readFailure = reads = 0;
}

static void Preserved(const WtJournalEntry *original, const WtJournalEntry *actual)
{
    WtJournalEntry expected = *original;
    expected.generation = actual->generation; memcpy(expected.seed, actual->seed, 32);
    assert(memcmp(&expected, actual, sizeof(expected)) == 0);
}

static void PhasesAndRequests(void)
{
    WtJournalEntry entry, view;
    WtSeed store, legacy;
    WtWireRequest readRequest;
    unsigned char seed[32], frame[256];
    size_t size;
    unsigned phase;
    memset(seed, 9, 32);
    for (phase = 0; phase <= WT_JOURNAL_RECOVERY_REQUIRED; phase++) {
        assert(WtTest_JournalEntry(phase, Hash, &entry) == 0); Reset(&entry);
        assert((WtSeed_Open(&legacy, NULL, Read, Write) == 0) == (phase == WT_JOURNAL_EMPTY));
        assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
        if (entry.requestLength) {
            const int allowed = phase == WT_JOURNAL_ENROLLING || phase == WT_JOURNAL_SUBMIT_READY
                || phase == WT_JOURNAL_QUEUED || phase == WT_JOURNAL_APPLIED;
            assert(WtSeed_RequestMatches(&store, entry.request, entry.requestLength) == allowed);
            memcpy(frame, entry.request, entry.requestLength); frame[8] ^= 1;
            assert(!WtSeed_RequestMatches(&store, frame, entry.requestLength));
        }
        if (phase >= WT_JOURNAL_IDLE) {
            memset(&readRequest, 0, sizeof(readRequest)); readRequest.operation = WT_OP_STATUS;
            memset(readRequest.requestID, 19, 16);
            memcpy(readRequest.saveID, entry.saveID, 16); memcpy(readRequest.credential, entry.credential, 32);
            assert(WtWire_Encode(&readRequest, Hash, frame, sizeof(frame), &size) == 0);
            assert(WtSeed_RequestMatches(&store, frame, size));
            frame[40] ^= 1; assert(!WtSeed_RequestMatches(&store, frame, size));
        }
        assert(WtSeed_Write(&store, seed, 32) == 32);
        assert(WtJournal_Read(&store, &view) == 0 && view.generation == entry.generation + 1);
        Preserved(&entry, &view);
        assert(memcmp(banks[0], banks[1], 4096) == 0);
        assert(WtSeed_Write(&store, seed, 32) == -1);
        WtJournal_Close(&legacy); WtJournal_Close(&store);
    }
}

static void FaultCuts(void)
{
    const unsigned phases[] = { WT_JOURNAL_ENROLLING, WT_JOURNAL_QUEUED, WT_JOURNAL_APPLIED };
    WtJournalEntry entry, view;
    WtSeed store, recovered;
    unsigned char seed[32], output[32];
    unsigned phase;
    int failure, cut;
    memset(seed, 9, 32);
    for (phase = 0; phase < 3; phase++) {
        assert(WtTest_JournalEntry(phases[phase], Hash, &entry) == 0);
        for (failure = 1; failure <= 2; failure++) {
            for (cut = 0; cut <= 4100; cut++) {
                Reset(&entry); failAt = failure; cutAt = cut;
                assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
                assert(WtSeed_Write(&store, seed, 32) == -1 && WtSeed_Read(&store, output, 32) == -1);
                assert(WtSeed_OpenRecovery(&recovered, NULL, Read, Write) == 0);
                assert(WtJournal_Read(&recovered, &view) == 0);
                Preserved(&entry, &view);
                assert(view.seed[0] == (failure == 2 || cut == 4100 ? 9 : 4));
                failAt = 0;
                assert(WtJournal_Repair(&recovered) == 0 && memcmp(banks[0], banks[1], 4096) == 0);
                WtJournal_Close(&store); WtJournal_Close(&recovered);
            }
        }
    }
}

static void StaleAndReadback(void)
{
    WtJournalEntry entry;
    WtSeed store, stale;
    unsigned char seed[32] = { 9 };
    assert(WtTest_JournalEntry(WT_JOURNAL_APPLIED, Hash, &entry) == 0); Reset(&entry);
    assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0 && WtSeed_OpenRecovery(&stale, NULL, Read, Write) == 0);
    assert(WtSeed_Write(&store, seed, 32) == 32);
    writes = 0; seed[0] = 10;
    assert(WtSeed_Write(&stale, seed, 32) == -1 && writes == 0);
    Reset(&entry); assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
    failAt = 1; cutAt = 100; lie = 1;
    assert(WtSeed_Write(&store, seed, 32) == -1);
    assert(WtSeed_Write(&store, seed, 32) == -1 && writes == 1);
    Reset(&entry); readFailure = 2;
    assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) != 0 && writes == 0);
    entry.generation = UINT64_MAX; Reset(&entry);
    assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0 && WtSeed_Write(&store, seed, 32) == -1 && writes == 0);
}

static void PacketPhaseBindings(void)
{
    WtJournalEntry entry;
    WtWireRequest request;
    WtSeed store;
    size_t size;
    assert(WtTest_JournalEntry(WT_JOURNAL_QUEUED, Hash, &entry) == 0);
    memset(entry.operation, 18, 16); entry.operationCounter = 2;
    memset(&request, 0, sizeof(request)); request.operation = WT_OP_CANCEL; request.counter = 2;
    memcpy(request.requestID, entry.operation, 16); memcpy(request.saveID, entry.saveID, 16);
    memcpy(request.credential, entry.credential, 32); memcpy(request.body.trade, entry.trade, 16);
    memset(entry.request, 0, 256);
    assert(WtWire_Encode(&request, Hash, entry.request, 256, &size) == 0); entry.requestLength = (uint16_t)size;
    Reset(&entry); assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
    assert(WtSeed_RequestMatches(&store, entry.request, entry.requestLength));
    assert(!WtSeed_RequestMatches(&store, NULL, 0));
    assert(!WtSeed_RequestMatches(NULL, entry.request, entry.requestLength));
    entry.phase = WT_JOURNAL_SUBMIT_READY; Reset(&entry);
    assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
    assert(!WtSeed_RequestMatches(&store, entry.request, entry.requestLength));
    assert(WtTest_JournalEntry(WT_JOURNAL_APPLIED, Hash, &entry) == 0);
    entry.phase = WT_JOURNAL_APPLYING; Reset(&entry);
    assert(WtSeed_OpenRecovery(&store, NULL, Read, Write) == 0);
    assert(!WtSeed_RequestMatches(&store, entry.request, entry.requestLength));
}

int main(void)
{
    PhasesAndRequests(); FaultCuts(); StaleAndReadback(); PacketPhaseBindings();
    puts("All active phases and 24,606 seed cuts preserve identity, escrow, receipts and exact pending packets");
    return 0;
}
