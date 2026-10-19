#include "wonder_trade/recovery.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <psa/crypto.h>

static unsigned char banks[2][4096], saved[2][4096], empty[4096];
static int reads, writes, readFailure;

static int Hash(const unsigned char *bytes, size_t length, unsigned char digest[32])
{
    size_t size = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, length, digest, 32, &size) == PSA_SUCCESS && size == 32 ? 0 : -1;
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
    (void)arg; (void)bank; (void)record;
    writes++;
    return 0;
}

static void Check(int allowed)
{
    WtJournalIO io = { NULL, Read, Write, Hash };
    memcpy(saved, banks, sizeof(saved)); reads = writes = 0;
    WtRecovery_Begin();
    assert(WtRecovery_Started() && !WtRecovery_CanWrite() && !WtRecovery_CanClear());
    assert((WtRecovery_Check(io) == 0) == allowed);
    assert(WtRecovery_CanWrite() == allowed && WtRecovery_CanClear() == allowed);
    assert(writes == 0 && memcmp(saved, banks, sizeof(saved)) == 0);
}

static int BrokenHash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    (void)bytes; (void)size; memset(digest, 0, 32);
    return 0;
}

int main(void)
{
    WtJournalEntry entry;
    WtJournalIO io = { NULL, Read, Write, BrokenHash };
    int first, second, phase;
    size_t i;
    const int malformed[] = { 0, 4, 12, 24, 1476, 1954, 2216, 4060, 4092 };
    assert(!WtRecovery_Started() && !WtRecovery_CanWrite() && WtRecovery_CanClear());
    for (first = 0; first < 2; first++) {
        for (second = 0; second < 2; second++) {
            memset(banks[0], first ? 255 : 0, 4096); memset(banks[1], second ? 255 : 0, 4096);
            Check(1);
        }
    }
    memset(&entry, 0, sizeof(entry)); entry.generation = 1; memset(entry.seed, 4, 32);
    assert(WtJournal_Encode(&entry, Hash, empty) == 0);
    memcpy(banks[0], empty, 4096); memcpy(banks[1], empty, 4096); Check(1);
    memset(banks[1], 255, 4096); Check(1);
    memcpy(banks[1], empty, 4096); memset(banks[1] + 4092, 0, 4); Check(1);
    for (phase = WT_JOURNAL_ENROLLING; phase <= WT_JOURNAL_RECOVERY_REQUIRED; phase++) {
        memcpy(banks[1], empty, 4096); banks[1][6] = (unsigned char)phase;
        assert(Hash(banks[1], 4060, banks[1] + 4060) == 0); Check(0);
        memset(banks[1] + 4092, 0, 4); Check(0);
    }
    for (i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        memcpy(banks[1], empty, 4096); banks[1][malformed[i]] ^= 128;
        /* A recognized torn digest/tag is safe with the other EMPTY mirror. */
        Check(malformed[i] >= 4060);
    }
    memcpy(banks[1], empty, 4096); banks[1][1444] ^= 1;
    assert(Hash(banks[1], 4060, banks[1] + 4060) == 0); Check(0);
    entry.generation = UINT64_MAX;
    assert(WtJournal_Encode(&entry, Hash, banks[0]) == 0); memcpy(banks[1], banks[0], 4096); Check(0);
    memcpy(banks[0], empty, 4096); memcpy(banks[1], empty, 4096);
    readFailure = 1; Check(0); readFailure = 2; Check(0); readFailure = 0;
    reads = writes = 0;
    assert(WtRecovery_Check(io) != 0 && reads == 0 && writes == 0 && !WtRecovery_CanWrite());
    io.hash = Hash; io.read = NULL;
    assert(WtRecovery_Check(io) != 0 && !WtRecovery_CanClear());
    Check(1); WtRecovery_Block(); assert(!WtRecovery_CanWrite() && !WtRecovery_CanClear());
    WtRecovery_Allow(WT_JOURNAL_QUEUED);
    assert(WtRecovery_CanWrite() && !WtRecovery_CanClear());
    WtRecovery_Allow(WT_JOURNAL_RECEIVED);
    assert(WtRecovery_CanWrite() && !WtRecovery_CanClear());
    WtRecovery_Block(); WtRecovery_InternalWrite(1);
    assert(WtRecovery_CanWrite() && !WtRecovery_CanClear());
    WtRecovery_InternalWrite(0); assert(!WtRecovery_CanWrite());
    WtRecovery_Allow(WT_JOURNAL_APPLYING);
    assert(!WtRecovery_CanWrite() && !WtRecovery_CanClear());
    WtRecovery_Allow(WT_JOURNAL_IDLE);
    assert(WtRecovery_CanWrite() && WtRecovery_CanClear());
    puts("Read-only boot classification, mutation gates and evidence preservation passed");
    return 0;
}
