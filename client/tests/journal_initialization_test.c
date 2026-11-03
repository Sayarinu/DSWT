#include "wonder_trade/journal.h"
#include "journal_fixture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>

static unsigned char banks[2][4096], before[2][4096];
static int writes, reads, failWrite, cutAt, readFailure, lie;

static int Hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t actual;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &actual) == PSA_SUCCESS && actual == 32 ? 0 : -1;
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
    int step, count;
    (void)arg;
    count = ++writes == failWrite ? cutAt : 4100;
    for (step = 0; step < count; step++) {
        if (step < 4) banks[bank][4092 + step] = 0;
        else banks[bank][step - 4] = record[step - 4];
    }
    return writes == failWrite && !lie ? -1 : 0;
}

static int BrokenHash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    (void)bytes; (void)size; memset(digest, 0, 32); return 0;
}

static void Reset(void)
{
    memset(banks, 255, sizeof(banks));
    reads = writes = failWrite = cutAt = readFailure = lie = 0;
}

int main(void)
{
    WtJournal store;
    WtJournalEntry entry;
    WtJournalIO io = { NULL, Read, Write, Hash };
    unsigned char seed[32], expected[4096];
    int bank, cut, failure, phase, first, second;
    memset(seed, 7, 32);
    /* Independent legacy host-provisioner layout, not the initializer's encoder. */
    memset(expected, 0, sizeof(expected));
    memcpy(expected, "PWTJ\1\0\0\0\0\20\0\0\0\0\0\0", 16); expected[16] = 1;
    memcpy(expected + 1440, "\1\0\40\0", 4); memcpy(expected + 1444, seed, 32);
    assert(Hash(expected, 4060, expected + 4060) == 0); memcpy(expected + 4092, "CMIT", 4);
    for (first = 0; first < 2; first++) {
        for (second = 0; second < 2; second++) {
            Reset(); memset(banks[0], first ? 255 : 0, 4096); memset(banks[1], second ? 255 : 0, 4096);
            memcpy(before, banks, sizeof(before));
            assert(WtJournal_Initialize(&store, io, NULL) == WT_JOURNAL_NEEDS_SEED && writes == 0);
            assert(memcmp(before, banks, sizeof(before)) == 0);
            assert(WtJournal_Initialize(&store, io, seed) == 0 && writes == 2);
        }
    }
    Reset(); memcpy(before, banks, sizeof(before));
    assert(WtJournal_Initialize(&store, io, NULL) == WT_JOURNAL_NEEDS_SEED);
    assert(!store.usable && writes == 0 && memcmp(before, banks, sizeof(before)) == 0);
    assert(WtJournal_Initialize(&store, io, seed) == 0 && store.usable && writes == 2);
    assert(memcmp(banks[0], expected, 4096) == 0 && memcmp(banks[1], expected, 4096) == 0);
    assert(WtJournal_Read(&store, &entry) == 0 && entry.generation == 1 && entry.phase == WT_JOURNAL_EMPTY);
    /* Existing, conflicting, active and unrecognized data are never replaced. */
    for (phase = WT_JOURNAL_EMPTY; phase <= WT_JOURNAL_RECOVERY_REQUIRED; phase++) {
        Reset(); assert(WtTest_JournalEntry(phase, Hash, &entry) == 0);
        assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0);
        memcpy(before, banks, sizeof(before));
        assert(WtJournal_Initialize(&store, io, seed) != 0 && writes == 0);
        assert(memcmp(before, banks, sizeof(before)) == 0);
    }
    for (bank = 0; bank < 2; bank++) {
        Reset(); banks[bank][100] = 3; memcpy(before, banks, sizeof(before));
        assert(WtJournal_Initialize(&store, io, seed) != 0 && writes == 0);
        assert(memcmp(before, banks, sizeof(before)) == 0);
    }
    for (failure = 1; failure <= 8; failure++) {
        Reset(); readFailure = failure;
        assert(WtJournal_Initialize(&store, io, seed) != 0 && !store.usable);
    }
    /* Cut every byte of either tag-last publication. Failed calls authorize no use. */
    for (failure = 1; failure <= 2; failure++) {
        for (cut = 0; cut <= 4100; cut++) {
            Reset(); failWrite = failure; cutAt = cut;
            assert(WtJournal_Initialize(&store, io, seed) != 0 && !store.usable);
            failWrite = 0;
            if (WtJournal_OpenEmpty(&store, io) == 0) {
                assert(WtJournal_Read(&store, &entry) == 0 && entry.generation == 1);
                assert(entry.phase == WT_JOURNAL_EMPTY && memcmp(entry.seed, seed, 32) == 0);
            }
            memcpy(before, banks, sizeof(before)); writes = 0;
            WtJournal_Initialize(&store, io, NULL);
            assert(writes == 0 && memcmp(before, banks, sizeof(before)) == 0);
        }
    }
    Reset(); failWrite = 1; cutAt = 100; lie = 1;
    assert(WtJournal_Initialize(&store, io, seed) != 0 && !store.usable && writes == 1);
    Reset(); memset(seed, 0, 32);
    assert(WtJournal_Initialize(&store, io, seed) != 0 && writes == 0);
    Reset(); io.hash = BrokenHash;
    assert(WtJournal_Initialize(&store, io, seed) == WT_JOURNAL_ERROR_SELF_TEST && writes == 0 && reads == 0);
    puts("Blank-only generation-1 seed initialization and publication failures passed");
    return 0;
}
