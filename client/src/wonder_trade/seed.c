#include "wonder_trade/seed.h"
#include "wonder_trade/wire.h"

#include <string.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

static int WtSeed_Hash(const unsigned char *data, size_t length, unsigned char digest[32])
{
    size_t size = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, data, length, digest, 32, &size) == PSA_SUCCESS && size == 32 ? 0 : -1;
}

int WtSeed_Open(WtSeed *store, void *arg, WtSeedReadBank read, WtSeedWriteBank write)
{
    WtJournalIO io = { arg, read, write, WtSeed_Hash };
    return WtJournal_OpenEmpty(store, io);
}

int WtSeed_OpenRecovery(WtSeed *store, void *arg, WtSeedReadBank read, WtSeedWriteBank write)
{
    WtJournalIO io = { arg, read, write, WtSeed_Hash };
    return WtJournal_Open(store, io);
}

int WtSeed_RequestMatches(WtSeed *store, const unsigned char *frame, size_t size)
{
    WtJournalEntry entry;
    int result = 0, phaseMatches;
    unsigned operation;
    if (WtWire_CheckRequest(frame, size, WtSeed_Hash) != 0) return 0;
    if (WtJournal_Read(store, &entry) != 0) goto done;
    operation = frame[5];
    if (memcmp(entry.credential, frame + 40, 32) != 0 || memcmp(entry.saveID, frame + 24, 16) != 0) goto done;
    if (operation == WT_OP_STATUS || operation == WT_OP_COLLECT || operation == WT_OP_RECONCILE) {
        result = entry.phase != WT_JOURNAL_EMPTY && entry.phase != WT_JOURNAL_ENROLLING;
        goto done;
    }
    phaseMatches = operation == WT_OP_ENROLL ? entry.phase == WT_JOURNAL_ENROLLING
        : operation == WT_OP_SUBMIT ? entry.phase == WT_JOURNAL_SUBMIT_READY || entry.phase == WT_JOURNAL_QUEUED
        : operation == WT_OP_CANCEL ? entry.phase == WT_JOURNAL_QUEUED
        : operation == WT_OP_ACKNOWLEDGE && entry.phase == WT_JOURNAL_APPLIED;
    result = phaseMatches && entry.requestLength == size && memcmp(entry.request, frame, size) == 0;
done:
    mbedtls_platform_zeroize(&entry, sizeof(entry));
    return result;
}

int WtSeed_Read(WtSeed *store, unsigned char *seed, size_t size)
{
    WtJournalEntry entry;
    int result = -1;
    if (!seed || size != WT_SEED_SIZE) return -1;
    if (WtJournal_Read(store, &entry) == 0) {
        memcpy(seed, entry.seed, size);
        result = (int)size;
    }
    mbedtls_platform_zeroize(&entry, sizeof(entry));
    return result;
}

int WtSeed_Write(WtSeed *store, const unsigned char *seed, size_t size)
{
    WtJournalEntry entry;
    unsigned char nonzero = 0;
    size_t i;
    int result = -1;
    if (!seed || size != WT_SEED_SIZE) return -1;
    for (i = 0; i < size; i++) nonzero |= seed[i];
    if (!nonzero) return -1;
    if (WtJournal_Read(store, &entry) != 0
        || entry.generation == UINT64_MAX || memcmp(seed, entry.seed, size) == 0) goto done;
    entry.generation++;
    memcpy(entry.seed, seed, size);
    if (WtJournal_Commit(store, &entry) == 0) result = (int)size;
done:
    mbedtls_platform_zeroize(&entry, sizeof(entry));
    return result;
}
